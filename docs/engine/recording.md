# Recording

Arrangement recording in the engine: a track's input (device channels, or another track's or the
master's output: resampling), monitoring that input, and recording it into takes that land where
they were heard. The recorder itself is [Recorder.h](../../engine/src/Recorder.h) /
[Recorder.cpp](../../engine/src/Recorder.cpp); the engine's input and recording API is
[EngineInput.cpp](../../engine/src/EngineInput.cpp); the audio-thread side lives in the renderer
([Renderer.cpp](../../engine/src/Renderer.cpp): `recordInput`, `recordRendered`, `isMonitored`,
`readInput`).

For what the user sees (arming, the input menu, monitoring modes, count-in, takes becoming clips) see
[guide/recording.md](../guide/recording.md). MIDI takes are covered in [midi.md](midi.md); the devices
whose inputs are recorded, in [audio-devices.md](audio-devices.md); input edges as part of the routing
graph, in [routing.md](routing.md).

## Overview

- A track's input (`InputEdge` in the snapshot) is a mono channel or a stereo pair of the device's
  inputs, another track's output (resampling), or the master's.
- A monitored track's strip takes its input instead of its clips; its delay compensation is left out,
  so a player hears themselves with only the latency of the track's own devices.
- A track being recorded plays none of its clips (a MIDI track none of its clips' notes): its take
  replaces them. Unmonitored, it is silent while it records.
- While recording, the renderer copies the recorded tracks' input into lock-free rings, with the
  timeline position where it was taken. A disk-writer thread empties them into WAV files (32-bit
  float); the audio thread never touches a file.
- Each take's start is moved back by its own *placement*, so it lands where what it recorded was heard.
- The live waveform's peaks come through their own queue, not from the file.
- Anything that changes the device, offline renders, and the playhead jumping end a recording; the
  takes so far are kept.

```
 audio thread, one chunk (Renderer::renderChunk)                 disk-writer thread
 ------------------------------------------------                -----------------
 prologue:  segments of the timeline (no loop wrap while recording)
            recordInput(): device input of each Device take ---> SampleRing ---+
                           (start stamped at its first segment;    gaps -------+--> drain() -> ma_encoder
                            a jump interrupts the session)         peaks --> UI |    (WAV, f32)
 graph:     tracks render (monitored ones from their input)                    |
 epilogue:  master sums, master strip                                          |
            recordRendered(): Track / Master takes from the    ---> SampleRing -+
                              same segments, post-fader
            metronome (after: never in a take)

 edit side:  startRecording() -> RecordingSession(takes) ; liveRecording_ = session
             finishRecordingLocked(): liveRecording_ = null, wait a callback,
                                      session.finish() -> RecordedTake[] (placed)
```

## Files

| File | What it holds |
|---|---|
| [Recorder.h](../../engine/src/Recorder.h) | `SampleRing`, `RecordingTake` (an audio take), `RecordedNote`, `MidiRecordingTake`, `RecordedTake` (what a finished take left), `RecordingSession` (the takes recorded together and their disk writer) |
| [Recorder.cpp](../../engine/src/Recorder.cpp) | The ring, `RecordingTake::push` (real-time), the writer loop, `finish()`, MIDI note pairing |
| [EngineInput.cpp](../../engine/src/EngineInput.cpp) | `setTrackInput`, `setTrackInputTrack`, `setTrackMonitor`, `setTrackArmed`, `inputEdgeLocked`, `startRecording` (placements), `finishRecordingLocked`, `stopRecording`, `isRecording`, `recordingProgress`; and MIDI input (see [midi.md](midi.md)) |
| [Snapshot.h](../../engine/src/Snapshot.h) | `InputEdge`, `MonitorMode`, `EdgeRender::Kind::Input`, `TrackBuffers::monitored` |
| [Renderer.cpp](../../engine/src/Renderer.cpp) | `isMonitored`, `isRecorded`, `readInput`, `inputChannel`, `recordInput` (prologue), `recordRendered` (epilogue), `compensationFor` |
| [EngineDevice.cpp](../../engine/src/EngineDevice.cpp), [EngineOffline.cpp](../../engine/src/EngineOffline.cpp) | Where a device change and an offline render end the recording (`closeDeviceLocked`, `suspendLiveLocked`) |

## Key types

**`InputEdge`** (Snapshot.h): where a strip's input comes from. `Source::None`, `Device` (`left`/`right`:
indices into the device's *open* inputs, in callback order; -1 when not open, which reads silence; a mono
input has both the same), `Track` (`edge`: the snapshot edge of `Kind::Input` it comes in on) or `Master`.
`monitorable()` is true for `Device` and `Track`: the master's output can't be heard on a track, since the
track goes into it.

**`MonitorMode`**: `Off` (never), `In` (always), `Auto` (while armed, unless it plays back without
recording). Python: `MonitorMode.OFF/IN/AUTO`.

**`SampleRing`**: single-producer / single-consumer ring of floats, its capacity rounded up to a power of
two and allocated on the edit side. `write()` (real-time) writes all of `count` samples or none; `read()`
takes up to `count`. Head and tail sit on separate cache lines.

**`RecordingTake`**: one audio track's take while it records.

| Member | Meaning |
|---|---|
| `trackId`, `path` | the track recorded, its WAV file (created anew) |
| `channels` | 1 or 2. Device input: 2 for a pair of different channels, else 1. Track or master: always 2 |
| `inputs[2]` | Device: indices into the open inputs |
| `source`, `sourceTrackId` | `Source::Device`, `Track` (a track's post-fader output) or `Master` |
| `placement` | how much later than the timeline its input arrives; subtracted from its start |
| `ring` | interleaved samples, audio thread → writer (8 seconds' worth: `sampleRate * 8` frames) |
| `gaps` | overruns, audio thread → writer (`SpscQueue<Gap, 256>`) |
| `peaks` | (min, max) of every `kPeakFrames` (128) frames, all channels: audio thread → UI |
| `start` | timeline sample of its first frame, before placement; `kNotStarted` until input arrives |
| `frames`, `dropped` | frames taken (lost ones included), frames lost to overruns |

**`RecordingSession`**: the tracks recorded together. Its constructor creates the WAV files (creating the
folder if needed; on failure it removes those already made and throws "Could not create ..."), then
starts the writer thread. `interrupt()` / `interrupted()` (real-time) mark that the playhead jumped.
`finish()` (edit side, once the audio thread can no longer see the session) stops the writer, drains
what is left, closes the files and returns a `RecordedTake` for each take.

**`RecordedTake`**: `trackId`, `path` (empty for MIDI), `startSample` (timeline sample of its first
frame, latency-corrected; may be negative), `frames` (0: nothing was recorded, and the file was
removed), `channels`, `sampleRate`, `droppedFrames`, `error` (the file couldn't be written: "is the disk
full?"), and for MIDI takes `midi` and `notes`.

**`RecordingProgress`** (Engine.h): a take while it records, for the UI's live waveform: `started` (the
playhead moved, after any count-in, and input arrived), `startSample` (placed), `frames`, and the new
peaks since the last call (`(n, 2)` array in Python, each over `RECORD_PEAK_FRAMES` frames).

## How it works

### Track inputs (edit side)

- `setTrackInput(track, channels)`: no channel, one (mono) or two (a stereo pair) of the device's
  channels, 0-based as `DeviceStatus.input_channels` numbers them; anything else is
  `std::invalid_argument`. It goes back from a track source to the device. Channels the device hasn't
  open are silent; the bridge reopens an ASIO device with the inputs a track needs
  ([audio-devices.md](audio-devices.md)).
- `setTrackInputTrack(track, source)`: the track takes `source`'s output after its fader (and pan) as its
  input, or the master's (`kMaster`, resampling the mix). It is refused (`std::invalid_argument`) for an
  unknown track, the track itself, or one it feeds: `wouldCycle` over every routing edge (outputs, sends,
  inputs, sidechains). The track keeps one `EdgeState` for its input edge across snapshots (it never
  needs a signal of its own). When the source goes, the input goes too.
- `trackInputTrack(track)`: the source, or none for device channels.
- `setTrackMonitor`, `setTrackArmed`: armed tracks are what `Auto` monitoring listens to; what records is
  up to `startRecording()`.

Each of these rebuilds the snapshot. `inputEdgeLocked()` maps device channels to indices into the open
inputs (`openInputChannels_`).

### The input edge (resampling)

Another track's output comes in on an input edge (`EdgeRender::Kind::Input`), tapped after the source's
fader and before any delay compensation. It orders the graph (the source renders first) and closes cycles
like any edge, but isn't summed or aligned (`EdgeRender::sums()` is false). Solo goes across it only while
the track monitors it (`workOutSolo`). The master renders after every track, so it is no edge and can't
be monitored. See [routing.md](routing.md).

### Monitoring (audio thread)

In the prologue, each track's `TrackBuffers::monitored` is worked out by `isMonitored()`:

- never in offline renders (they play the arrangement), never for a master input;
- `In`: always; `Auto`: armed and (stopped, or recording); `Off`: never.

`TrackBuffers::recorded` is worked out there too, by `isRecorded()`: a take of the track records
(the session isn't interrupted; live renders only). Such a track renders none of its clips
(`renderTrack()`, `assignVoices()`) and gets none of its clips' notes (`buildNoteEvents()` with
`clipNotes` false, which releases those sounding): the take replaces them, so with monitoring Off
the track is silent while it records, and monitored it hears only its input.

In `renderTrack()`, a monitored track takes its input instead of its clips: a track source through
`sumEdge()` of its input edge (the source has rendered: it fed this one), device input through
`readInput()`, which adds the open channels (left and right; the same channel twice for mono). Its strip
then processes it like any audio. `compensationFor()` returns 0 for a monitored track's edges, so its
output isn't delayed to line up with latent plug-ins on other tracks; a monitored strip's sidechained
devices don't wait for their sidechains either.

### Starting (`Engine::startRecording`)

Takes `RecordTarget`s (`trackId`, `path`; no path: record the track's MIDI input) and a count-in in beats.
Throws `std::runtime_error` (for the user) if no device runs ("No audio device is running"), it is
already recording, a device input isn't open ("A track's input is not open on the audio device") or a file
can't be created; `std::invalid_argument` for no targets, a track listed twice, a track without input, or
a MIDI target without a MIDI input.

**Placement** (take placement / latency compensation). From the device's state and the published
snapshot at the start:

| Take | Placement (subtracted from its start) | Why |
|---|---|---|
| Device input | `lag + inputLatency + outputLatency` | a sample comes back through the input that long after the timeline was heard: the output lag (delay compensation and the master's devices: `RenderSnapshot::outputLatency()`), then the driver's output and input latency |
| A track's output | that track's `inputLatency + latency` | how late it leaves the track: its edges' arrival (what feeds it) and its devices |
| The master's output | `lag` | the master's output lags the timeline by the output lag |
| MIDI | `lag + outputLatency + midiInputDelay` | see [midi.md](midi.md) |

So takes land where they were played: what was played along to lines up. A resampled take lands exactly
where its source was heard, so latent plug-ins on the source (or the master) change nothing.

The session is then published (`liveRecording_`, an atomic pointer the callback loads each time), and if
the transport is stopped it starts playing, after the count-in (`TransportCommand::Play` with
`countInBeats`; the metronome counts in even if it is off: [rendering.md](rendering.md)).

### Recording (audio thread)

While a session is live, the loop doesn't wrap (`renderChunk`: punching in and out of the loop comes
later), so the chunk's segments are one straight stretch of the timeline (more than one only if the
playhead jumped).

- **Prologue, `recordInput()`**: for each segment, each take (audio and MIDI) whose `start` is still
  `kNotStarted` gets the segment's position; if a started take meets a segment that `jump`s (a locate,
  or stopped and started again), the session is interrupted and nothing more is recorded. Device takes
  then `push()` the segment's input. During a count-in the playhead stands, there are no segments, and
  nothing is taken. `recordSegments_` counts the segments taken.
- **Epilogue, `recordRendered()`**: after the master strip and before the metronome, track and master
  takes push the same segments: the source track's buffer (after its fader, before any edge's delay), or
  the master's. A source deleted meanwhile records silence, so the take stays in time.

`RecordingTake::push()` interleaves into scratch, folds the frames into the pending peak (pushing one per
128 frames; a full peak queue only loses the picture), then writes the ring. A pending gap is pushed to
`gaps` before the input after it, so the writer sees it in time. If the gap queue or the ring is full,
the input is dropped but its place is kept: the gap grows and `dropped` counts it. `frames` always
advances.

### The disk writer

`RecordingSession::writerLoop()` runs on its own thread: it drains every take, then waits up to 10 ms (or
until stopped). `drain()` first reads how much input there is (so any gap before it is visible), writes
silence for a gap it has reached, and otherwise writes up to the next gap. `writeFrames()` uses
miniaudio's WAV encoder (`ma_format_f32`, the take's channels, the session's rate); a write that fails
records the error once, and `written` still advances so the rest stays in time.

### Ending (`finishRecordingLocked`, `stopRecording`)

`finishRecordingLocked()` unpublishes the session, waits for a callback to pass (so the audio thread no
longer sees it), calls `finish()` and keeps the takes in `finishedTakes_`. `stopRecording()` does that
and returns those takes, along with any of a recording ended otherwise since the last call; the transport
plays on. `finish()` sets each take's `startSample` to `start - placement` and `frames` to what was
written; a take that never started has 0 frames and its file is removed.

What ends a recording:

- `stopRecording()` (the Record button again, Stop, Space: the bridge calls it).
- The playhead jumping: `recordInput()` interrupts the session; `isRecording()` is false from then on,
  and the bridge, which polls it, stops the recording.
- Changing the audio device, its sample rate, or a reset by the driver: `closeDeviceLocked()` finishes it.
- Offline renders and exports: `suspendLiveLocked()` finishes it (its input would have a hole).
- Changing the number of audio threads does *not*: the recording goes on, and the playhead waits too.

`isRecording()` is "recording and still taking input". `recordingProgress()` reports each take's state and
drains its peaks, for the live waveform.

The bridge turns the takes into clips in one undo step, replacing what was under them, and names the
files in the project's `Recordings` folder (for a project not saved yet, `Music\SUBstation\Recordings`);
see [python/engine-bridge.md](../python/engine-bridge.md).

## Invariants and real-time rules

- The audio thread never opens, writes or closes files, allocates, frees or waits: rings, queues and
  scratch are allocated on the edit side (`Renderer::prepare` sizes `recordScratch_` for `kMaxBlock`
  stereo frames).
- One producer and one consumer per queue: the audio thread writes rings, gaps, peaks and MIDI events;
  the writer reads rings and gaps; the edit side (under the engine's lock) reads peaks and MIDI events.
- The session is freed only after the audio thread can no longer see it (`waitForCallbackLocked`).
- A take is always in time: overruns become silence of the same length, failed writes still count.
- Monitoring is never delay-compensated; recorded takes are placed by their own latency instead.
- The master's output is recorded after its fader and before the metronome, so clicks are never in a
  take.

## Extending it

- **Another input source**: add an `InputEdge::Source` and a `RecordingTake::Source`, decide whether it
  is monitorable, push it from the prologue (if it is known before the graph) or the epilogue (if it is
  rendered), and give it a placement in `startRecording()`.
- **Punching in and out at the loop, stacking takes while looping**: today the loop doesn't wrap while
  recording; a wrap would have to start a new take (or a new region) rather than interrupt the session.
- **WASAPI inputs**: only the backend is missing ([audio-devices.md](audio-devices.md)).

## Gotchas

- A device input that isn't open makes `startRecording()` throw, but a monitored track with one just
  hears silence.
- The bridge can only open missing ASIO inputs before recording: opening the device again ends a
  recording.
- Placement is worked out once, at the start: a latency that changes while recording (a plug-in's) isn't
  followed.
- Stopping the transport doesn't itself end the session in the engine: the next start is a jump, which
  does. The bridge stops recording when the transport stops.
- A resampled take of a track deleted while recording keeps going, in silence.
- The ring holds 8 seconds: a disk stalled longer than that loses input (reported in `droppedFrames`, and
  as a status message by the bridge).

## Tests

- [tests/test_recording.py](../../tests/test_recording.py) (fake ASIO driver in manual mode; the
  loopback patches an output back into an input, delayed by the latencies the driver reports):
  monitoring modes, monitoring through a latent plug-in not delayed by compensation, a loopback take
  lining up with the timeline sample for sample (with and without latent plug-ins on a track or the
  master), stereo takes and their live peaks, Auto monitoring while recording, a track being recorded
  playing none of its clips (silent with monitoring Off), the count-in, what ends a
  recording (a locate, a device change, stopping before anything came in), and needing an open input.
- [tests/test_resampling_engine.py](../../tests/test_resampling_engine.py): a resampled take equal to its
  source's offline render (latent plug-ins on the source, in the group and on the master, a slower track
  the source is delayed to line up with), monitoring a track not delayed, the master impossible to
  monitor, solo across a monitored input, cycles refused, the input going with its source, a source
  removed while recording leaving silence, device and resampled takes together.
- [tests/test_resampling_model.py](../../tests/test_resampling_model.py),
  [tests/test_ui_resampling.py](../../tests/test_ui_resampling.py): the model and window side.
- [tests/test_ui_recording.py](../../tests/test_ui_recording.py): header controls, a take recorded through
  the cable, Space stopping recording and the count-in, a device change ending the recording, take names.
- [tests/test_parallel_live.py](../../tests/test_parallel_live.py): the recording and resampling tests
  run again live with workers (with silent tracks beside, so every buffer's tracks are shared out).
  [tests/test_parallel_engine.py](../../tests/test_parallel_engine.py) puts tracks taking their input from
  others into its random routing graphs.
