# MIDI in the engine

How MIDI tracks play: the notes of their clips turned into note events for the track's devices, the
notes the piano roll plays, and MIDI input from controllers (WinMM on Windows) played live and recorded. Playback
is in the renderer ([Renderer.cpp](../../engine/src/Renderer.cpp): `buildNoteEvents`,
`releaseNotes`, `syncTempo`); MIDI input devices and the audio clock are in
[MidiInput.h](../../engine/src/MidiInput.h) / [MidiInput.cpp](../../engine/src/MidiInput.cpp); the
engine's MIDI input API is in [EngineInput.cpp](../../engine/src/EngineInput.cpp); MIDI takes are in
[Recorder.h](../../engine/src/Recorder.h).

For what the user sees (MIDI clips, the piano roll, MIDI input, the computer MIDI keyboard) see
[guide/midi.md](../guide/midi.md). How instruments take the events is in [devices.md](devices.md) (built-in)
and [plugins.md](plugins.md) (VST3); chunks, segments and the prologue in [rendering.md](rendering.md);
audio takes and placement in [recording.md](recording.md).

On platforms other than Windows the engine has no MIDI input devices
([backends/MidiNone.cpp](../../engine/src/backends/MidiNone.cpp): none is ever listed or opened). Everything else on
this page works the same there, including input sent with `Engine::sendMidiInput()` (the computer MIDI keyboard,
the tests).

## Overview

- The engine bridge flattens a MIDI track's clips into the notes they play, in beats (`setTrackNotes`), keeping
  only what the clips play (notes cut at their clip's end). The snapshot converts them to samples at the
  current tempo.
- Each block, the renderer turns notes starting in it into note-on events for the track's devices, and
  remembers when each ends. Note-offs come from that record, not from the snapshot, so a note edited or
  deleted while it sounds still ends.
- Stopping, locating and wrapping around the loop release every sounding note. A tempo change moves the
  recorded note ends along with the playhead.
- Notes the piano roll plays go through a lock-free queue to the next audio block, straight to the
  track's instrument.
- Offline renders share devices with live playback, so the engine resets them before and after. A device
  that is switched off resets when it comes back on, so it can't keep notes whose note-offs it missed.
- MIDI input: each message is stamped with the host clock as it arrives and turned, right there, into a
  sample on the audio device's clock, plus one device buffer, so the delay is constant. The renderer
  routes the messages due in each chunk to the tracks whose `MidiInputRoute` accepts them, and remembers
  the live notes it started so it can release them.
- A MIDI track being recorded gets a `MidiRecordingTake`; its note-ons and note-offs go through a queue
  and the edit side pairs them into notes.

## Files

| File | What it holds |
|---|---|
| [Engine.h](../../engine/src/Engine.h) | `NoteDesc` (a note in timeline beats), `AudioClockStatus`, the MIDI API (`setTrackNotes`, `previewNote`, `midiInputDevices`, `openMidiInput`, `closeMidiInput`, `openMidiInputs`, `setTrackMidiInput`, `sendMidiInput`, `audioClock`) |
| [EngineTracks.cpp](../../engine/src/EngineTracks.cpp) | `setTrackNotes`, `previewNote` |
| [EngineSnapshot.cpp](../../engine/src/EngineSnapshot.cpp) | Notes into `NoteRender`s in samples; `MidiInputRoute`, monitoring and arming into `TrackRender` |
| [Snapshot.h](../../engine/src/Snapshot.h) | `NoteRender`, `MidiInputRoute`, `TrackBuffers::events` (a track's note events per chunk, at most 1024) |
| [Transport.h](../../engine/src/Transport.h) | `PreviewNote`; in `SharedState`: `previewNotes` queue, `midiInput` queue and `midiInputMutex`, `midiInputDelay`, `midiSampleRate`, `clock` |
| [Processor.h](../../engine/src/Processor.h) | `ProcessEvent` (`NoteOn`, `NoteOff`, `Midi`), `setEnabled`, `requestReset`, `takeResetRequest`, `reset`, `resetOffline` |
| [Renderer.h](../../engine/src/Renderer.h), [Renderer.cpp](../../engine/src/Renderer.cpp) | Note events, active notes, preview notes, MIDI input gathering and routing, live notes, MIDI recording |
| [MidiInput.h](../../engine/src/MidiInput.h), [MidiInput.cpp](../../engine/src/MidiInput.cpp) | `MidiInputEvent`, `AudioClock`, `hostTimeNs()`, `MidiInputDevices` |
| [backends/MidiWinMM.cpp](../../engine/src/backends/MidiWinMM.cpp), [backends/MidiNone.cpp](../../engine/src/backends/MidiNone.cpp) | `MidiInputDevices` on Windows (WinMM), and elsewhere (no devices) |
| [EngineInput.cpp](../../engine/src/EngineInput.cpp) | MIDI ports, `Engine::midiInput` (stamping), `sendMidiInput`, `discardMidiInputLocked`, MIDI targets in `startRecording` |
| [Recorder.h](../../engine/src/Recorder.h), [Recorder.cpp](../../engine/src/Recorder.cpp) | `RecordedNote`, `MidiRecordingTake`, `RecordingSession::midiNotes`, MIDI results in `finish()` |
| [EngineOffline.cpp](../../engine/src/EngineOffline.cpp) | `resetProcessorsLocked` around offline renders and exports |

## Key types

**`NoteDesc`** (Engine.h): `startBeat`, `lengthBeats`, `key` (60 = C3), `velocity`, in timeline beats.

**`NoteRender`** (Snapshot.h): a note already cut to its clip, in timeline samples: `start`, `end` (always
`> start`), `key` (clamped to 0..127), `velocity` (clamped to 1..127). A track's are sorted by start, then
key. `EngineSnapshot.cpp` rounds `beat * samplesPerBeat` for both ends.

**`ProcessEvent`** (Processor.h): what devices get. `NoteOn`/`NoteOff` with `data[0]` the key, `data[1]`
the velocity (0 for note-offs), `data[2]` the MIDI channel; `Midi` with the raw bytes (pressure,
controllers, program changes, pitch bend from MIDI input). `sampleOffset` is in the block (the renderer
rebases it to each stretch a device processes).

**`PreviewNote`** (Transport.h): `trackId`, `key`, `velocity` (0: a note-off), from the piano roll.

**`MidiInputEvent`** (MidiInput.h): `time` (the device sample at which it plays), `port` (the engine's id
of the input), `status`, `data1`, `data2`.

**`AudioClock`**: the device's sample clock against the host clock. The audio thread writes it at each
callback (`update(hostTimeNs, sampleTime)`) as a seqlock (wait-free for it; readers retry); `stop()` is
called by the edit side while no callback runs; `read()` returns `running`, `hostTimeNs`, `sampleTime`.
Callbacks that share a host time (WASAPI splitting a device buffer into engine callbacks) keep the first
one's anchor.

**`MidiInputRoute`** (Snapshot.h): which MIDI input a track hears and records: `enabled`, `port`
(`kAllPorts` or an engine port id), `channel` (`kAllChannels` or 0-15). `accepts(port, status)` checks
both.

**`MidiInputDevices`**: the system's MIDI inputs (WinMM; none off Windows). `available()` lists them by name (a second
device of the same name gets " #2"); `open(name, port)` (throws `std::runtime_error` for the user: not
connected, or "another application may be using it"), `close`, `closeAll`, `openNames`. Its handler is
called on the driver's thread for every short message (channel and system messages; System Exclusive is
ignored: no long buffers are given).

**`MidiRecordingTake`**: one MIDI track's take: `events` (`SpscQueue<Event, 1 << 14>`, audio thread →
edit side: `time` in timeline samples, channel, key, velocity, 0 for a note-off), `start`, `frames`,
`dropped` (events lost because nobody took them in time), and on the edit side the `notes` paired so far.

**`RecordedNote`**: `start`, `end` (-1: still held), `key`, `velocity`, `channel`, in timeline samples.
The API hands them over as a `std::vector<RecordedNote>` (`recordingProgress()`, `RecordedTake`).

## How it works: playback

### Notes into events (`Renderer::buildNoteEvents`)

In the chunk's prologue (serial: sounding notes are shared state), for each track that has notes, or
while anything is sounding, queued or arriving, the renderer builds the track's events into its
`TrackBuffers`:

1. **Preview notes** for this track, all at offset 0 and in the order they were played (dragging a note
   across keys releases one key and plays the next several times within a block; reordering those would
   leave notes playing whose note-off came first).
2. **MIDI input** (`routeMidiInput`, below).
3. If stopped, or the clips' notes don't play (monitoring `In` with MIDI input), every active note of
   the track is released at offset 0 (`releaseNotes`).
4. For each segment of the chunk (a continuous stretch of the timeline; see [rendering.md](rendering.md)):
   - a segment that `jump`s (a locate, a loop wrap) releases the track's sounding notes at its start;
   - a segment that `chase`s (playback starts here) sounds the notes already underway from there;
   - note-ons for the notes starting in it (binary search on `start`), each recorded as an `ActiveNote`
     (`trackId`, `key`, `end`);
   - note-offs for active notes ending in it, those that just started included.
5. The arrangement's notes and the input are sorted by offset; at the same offset a note-off comes
   first, so a note that ends where the next one on its key starts doesn't cut the new one short. The
   preview notes stay first, in their order.

Active notes are capped at 512 across all tracks (`kMaxActiveNotes`); a note that can't be recorded
there isn't started. A track's events are capped at `TrackBuffers::kMaxEvents` (1024); events that don't
fit (note-offs from `releaseNotes`) wait for the next block. `forgetNotesOfRemovedTracks()` drops the
active and live notes of tracks no longer in the snapshot: their instruments are gone with them.

Because note-offs come from `activeNotes_`, not the snapshot, a note edited or deleted while it sounds
still ends when it was going to. Every chain of a rack hears the track's notes, so an Instrument Rack
layers its instruments ([routing.md](routing.md)). A device then processes each stretch with the events
that fall in it (`processInserts` splits them by slice).

### Tempo changes (`Renderer::syncTempo`)

When the snapshot's samples per beat change (tempo, or sample rate), the playhead, the expected
position and every active note's `end` are scaled by the ratio, so they stay on the same beat. The new
snapshot already has the notes themselves at the new tempo.

### Preview notes

`Engine::previewNote(track, key, velocity)` pushes a `PreviewNote` onto
`SharedState::previewNotes` (256 slots, one producer: it is pushed under the engine's lock). At the
start of each live callback `drainPreviewNotes()` moves them into the renderer's list (256), played in
the callback's first chunk and then cleared. They are only heard while a device runs: without one,
`serviceTransportIfIdleLocked()` discards them (nobody would hear them, and a stale note-on could hang).
Offline renders never play them.

### Resets: offline renders and devices switched off

- Offline renders (and exports) share the processors with live playback. `resetProcessorsLocked()` runs
  before and after: `resetOffline()` on every processor (clearing what a real-time reset can't, such as a
  plug-in's reverb tail, while it isn't being processed), and `requestReset()`. Before keeps live notes
  out of the render; after keeps the render's last notes from hanging in live playback. Live output is
  suspended meanwhile ([rendering.md](rendering.md)).
- `requestReset()` sets a flag; whichever renderer processes the device next calls `reset()` first
  (`Renderer::takeResets`, from the rendering thread). A rack asked to reset passes it on to everything in
  it.
- `Processor::setEnabled(false)` requests a reset, so a device switched off resets when it comes back on
  and can't keep notes whose note-offs it missed.
- A device moved to another track resets too (`moveProcessor`): what it holds of the old strip's signal,
  and notes that strip will release elsewhere, stop.

## How it works: MIDI input

### Devices and ports

`Engine` owns a `MidiInputDevices` whose handler is `Engine::midiInput`. Inputs are opened, listed and
closed on the main thread, like audio devices.
Port ids are indices into `midiPorts_`, a list of names kept for the engine's life, so a track can name an
input that isn't open (yet). The engine bridge opens every connected input but those turned off in the
preferences (`EngineBridge::openMidiInputs()`), and the application adds the computer keyboard as one more
input, `Computer Keyboard` (`kComputerKeyboard`), which plays through `sendMidiInput`
([app/engine-bridge.md](../app/engine-bridge.md)). Without MIDI devices (off Windows) it is the only input.

`setTrackMidiInput(track, enabled, device, channel)`: none (`enabled` false), every input (`""`) or one
by name, on every channel (0) or one (1-16; anything else is `std::invalid_argument`).

### Stamping (`Engine::midiInput`)

On the driver's thread (WinMM calls back for each `MIM_DATA`; the callback takes `hostTimeNs()` first) or
the caller's (`sendMidiInput`, which checks the message is a status byte and up to two data bytes, and
uses "now" when given host time 0):

```
clock   = shared_.clock.read()                 // the last callback's (hostTimeNs, sampleTime)
if !clock.running: drop                        // no device runs
time    = clock.sampleTime
        + round((hostTime - clock.hostTimeNs) * midiSampleRate)
        + midiInputDelay                       // one device buffer
push MidiInputEvent{time, port, bytes} onto shared_.midiInput   (holding midiInputMutex; full: dropped)
```

A callback renders a buffer ahead of what is heard, so a message that came in during one buffer is placed
at the same distance into the next, and the delay stays constant instead of jittering with the
callbacks. Converting on arrival also makes the timing testable: a message stamped against a known clock
reading lands on a known sample. `midiInputDelay` is the device's buffer size (set when the device opens;
512 if it reports none); `audioClock()` reports it with the clock.

The queue (`SpscQueue<MidiInputEvent, 4096>`) has one consumer, the audio thread; the producers (driver
threads, `sendMidiInput()`) share a mutex on their side of it, so the audio thread never waits. The engine
never takes its own `mutex_` on the driver's thread. While no device runs, the edit side empties the queue
(`discardMidiInputLocked`, in `serviceTransportIfIdleLocked`), and closing a device stops the clock, so
input is dropped from then on.

### Due in a chunk (`drainMidiInput`, `gatherMidiInput`)

At the start of each live callback, `drainMidiInput()` moves what arrived into `pendingInput_` (at most
2048 not yet due). Before each chunk, `gatherMidiInput()` takes the messages due in it, in the order they
came, with their offsets (`time - deviceTime_`, clamped into the chunk):

- A message more than a second off the chunk wasn't stamped against this run of the clock (it was reset,
  or live output was suspended meanwhile): it plays now, but a note-on that late is dropped rather than
  played out of time (its note-off is harmless).
- A note-off never plays before its note-on (they may be stamped out of order, or both late): just after
  it, or in the next chunk.
- Once one message is put off, those after it are too, keeping their order. At most 512 are due per
  chunk (`kMaxInputEvents`).

### Routing and live notes (`Renderer::routeMidiInput`)

A track hears its MIDI input (`hearsMidiInput`) only live, with a route enabled, and while monitored:
`In` (it plays its input, not its clips), `Auto` while armed (its clips play as well), never `Off`. For
each track, in the prologue:

1. Live notes it started that it no longer hears (its input or monitoring changed, or the transport
   stopped: `releaseLiveNotes_`) are released at the chunk's start (and recorded as note-offs).
2. Each due message its route `accepts`:
   - a note-on: recorded; if heard, a key it is already holding is released first (played again, it starts
     over), then the note-on goes to the track and a `LiveNote` (`trackId`, `port`, `channel`, `key`) is
     remembered (at most 512 across all tracks; one that couldn't be released isn't played);
   - a note-off (or note-on with velocity 0): recorded; it goes to the track only if the track started that
     note;
   - pressure, controllers, program changes and pitch bend go to the track as raw `Midi` events if heard
     (played, not recorded).

So each live note gets its note-off even if the track stops hearing its input, stops existing
(`forgetNotesOfRemovedTracks`), or the transport stops while the key is held.

## How it works: MIDI takes

`startRecording()` makes a `MidiRecordingTake` for each target without a file (the track must have a MIDI
input). The audio thread:

- stamps the take's `start` with the first segment's position and counts its `frames`, in `recordInput()`,
  alongside the audio takes; a jump interrupts the whole session ([recording.md](recording.md));
- in `routeMidiInput()`, sends what the track's route accepts, monitored or not, through `recordMidi()`:
  the timeline position where it played (the segment containing the event's offset). Nothing is recorded
  while the playhead stands (a count-in), or once the session is interrupted.

On the edit side, `MidiRecordingTake::collect()` pairs the events: a note-on starts a held note; a note-off
ends the earliest held note on its key and channel (one whose note-on came before the take began has none),
at least one sample long. `RecordingSession::midiNotes()` moves them back by the *MIDI placement*:

```
midiPlacement = lag (output lag: delay compensation and the master's devices)
              + the device's output latency
              + midiInputDelay (the buffer MIDI waits for)
```

A MIDI message was played in response to what was heard when it arrived; the renderer meets it one MIDI
delay later, a block ahead of the output. So notes land where they were heard against the timeline. No note
starts before the take. The take itself spans the timeline where it recorded (its start isn't moved).

`recordingProgress()` returns the notes so far (held ones end at -1) for the live take. `finish()` returns a
`RecordedTake` with `midi` set: notes starting after the take's end are dropped, and a key still held when
recording stops (or a note running past the end) ends with the take. Lost events become an error message
("... MIDI events were lost while recording"). Nothing is written to disk: the bridge turns the notes into
a MIDI clip of what was recorded (replacing what was under it), in the same undo step as the audio takes,
and the session applies *Record Quantization* (`recordQuantize()` in
[app/src/audio/AudioSettings.h](../../app/src/audio/AudioSettings.h)) to their starts when it adds the takes
(`ProjectEditor::addRecordings`).

## Invariants and real-time rules

- Every note-on the renderer sends has a note-off: clips' notes through `activeNotes_`, live notes
  through `liveNotes_`, both released on stop, jump, track removal (forgotten with the instrument) and
  device resets.
- The audio thread never waits on MIDI producers: one mutex serialises the producers only.
- Offline renders never hear MIDI input or preview notes, and never record (`flags.live` is false); they
  start clean (`prepare()` empties every note list) and reset the shared devices before and after.
- All note and input buffers are sized in `Renderer::prepare()`; their counts say how much is in use.
- Opening a device prepares the renderer again, which forgets sounding, pending and live notes; the
  processors are prepared (silenced) along with it.

## Extending it

- **Another MIDI backend** (Windows MIDI Services, or ALSA sequencer input on Linux): `MidiInputDevices` is
  the only part that knows about the system's MIDI; each platform has its own file in `backends/`, chosen in
  [engine/CMakeLists.txt](../../engine/CMakeLists.txt). Keep its interface: list by name, open with a port id,
  call the handler with the bytes and `hostTimeNs()` taken on arrival.
- **Recording controllers** (sustain, pitch bend): `routeMidiInput()` would send them to the take as well,
  and `MidiRecordingTake::Event` / `RecordedNote` would need a form for them.
- **MIDI effects** would sit between `buildNoteEvents()` and the instrument; today events go straight to
  the track's devices.
- **MIDI overdub** (recording into existing clips) is a model-side change: the engine already returns
  the notes.

## Gotchas

- A track with `In` monitoring and a MIDI input doesn't play its clips' notes; with `Auto` and armed it
  plays both.
- `previewNote` without a running device does nothing audible, and the note is discarded, not deferred.
- Messages sent with a host time far from the clock (over a second) are treated as stale; tests stamp
  against `audioClock()` to place them exactly.
- The live-note and active-note tables are shared by all tracks (512 each); a dense MIDI file on many
  tracks can fill them, and new notes are then not played rather than left hanging.
- `MidiRecordingTake` notes are paired by key and channel, not by port: two inputs on one track playing
  the same key and channel pair up with each other.

## Tests

- [tests/engine/test_midi_engine.cpp](../../tests/engine/test_midi_engine.cpp): notes start on their sample and
  follow the tempo, velocity sets the level, chords and repeated keys, offline renders leave no hanging notes,
  loop wraps release and retrigger notes, the instrument's output through the chain, notes without an
  instrument silent, the Synth's pitch, cutoff and parameters, preview notes needing a running device, and
  quick preview notes all ending.
- [tests/engine/test_midi_input.cpp](../../tests/engine/test_midi_input.cpp) (fake ASIO driver; skipped without
  the ASIO SDK): live notes reaching a test instrument at their offsets in a block, a note released as it is
  played still sounding, which tracks hear which input and channel, monitoring, held keys released when the
  transport stops or a track stops hearing them, input dropped without a running device, the device list,
  recorded notes landing on the beats a player heard them on (with latent plug-ins on a track or the master),
  MIDI and audio recorded together, and recording MIDI needing a MIDI input.
- [tests/engine/test_parallel_live.cpp](../../tests/engine/test_parallel_live.cpp): the MIDI input tests again live
  with workers (held, preview and recorded notes).
- In the application's tests: [test_ui_arrangement_tracks.cpp](../../tests/app/test_ui_arrangement_tracks.cpp)
  (the MIDI header controls), [test_editor_midi.cpp](../../tests/app/test_editor_midi.cpp) (a recorded MIDI take
  becoming a clip, record quantization), [test_ui_arrangement.cpp](../../tests/app/test_ui_arrangement.cpp) (a take
  drawn as it records), [test_ui_dialogs.cpp](../../tests/app/test_ui_dialogs.cpp) (the MIDI preferences),
  [test_ui_pianoroll.cpp](../../tests/app/test_ui_pianoroll.cpp) (the piano roll's notes being heard),
  [test_session_keyboard.cpp](../../tests/app/test_session_keyboard.cpp) (the computer keyboard playing) and
  [test_midi_model.cpp](../../tests/app/test_midi_model.cpp) (MIDI clips and notes in the model).
