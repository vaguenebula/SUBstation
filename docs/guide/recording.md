# Recording

Audio recording works as Ableton's Arrangement recording, from an ASIO driver's inputs;
MIDI tracks record along with audio ones, and any track can record another track's (or
the master's) output by resampling. This page covers arming, inputs, monitoring, the
count-in, takes and resampling. On Linux there are no device inputs: audio records by
resampling, and MIDI from the computer MIDI keyboard.

## Arming

- An audio or MIDI track's header has an **arm** button (●). Arming one disarms the
  others unless Ctrl is held, and clicking a selected track's arms all the selected
  tracks.
- Arming a track with no input says so in the status line: choose one to record.
- Group tracks have no arm button: they record nothing.

## Inputs

An audio track's **input** menu lists:

- *No Input*;
- each of the driver's inputs on its own (mono), then each pair of them (stereo);
- *Resampling* (the master's output) and every other track, group and return (see
  [Resampling](#resampling)).

Choosing an input the driver hasn't open opens it (and keeps it open next time). With
WASAPI (and the *System* driver on Linux) there are no device inputs: the menu says to
choose an ASIO driver (see [audio-setup.md](audio-setup.md)).

A MIDI track's input is a MIDI input and a channel; see [midi.md](midi.md#midi-input).

## Monitoring

Each track's **monitoring** button chooses:

| Mode | Audio track | MIDI track |
|---|---|---|
| *In* | Always hears its input, not its clips | Plays its input, not its clips |
| *Auto* | Hears its input while armed, unless playing back without recording | Plays its input while armed, its clips as well |
| *Off* | Never hears its input | Never plays its input |

A monitored track's input goes through its devices and mixer like any audio; it isn't
delayed to line up with latent plug-ins on other tracks, so you hear yourself with only
the latency of the track's own devices.

While a track records it plays none of its clips (nor a MIDI track its clips' notes):
its take replaces them. With monitoring *Off* it is silent until recording stops.

## Recording

- **Record** (the button next to Stop, or **F9**) records every armed track that has an
  input:
  - from the insert marker, after the **count-in** chosen next to it (none, 1, 2 or 4
    bars: the metronome counts in even if it is off) if stopped;
  - or from the playhead while playing (punch in).
- Record again to stop recording and keep playing; Stop or Space stop both.
- While recording the loop doesn't wrap.

## Takes

- A take grows on its track as it records, right up to the playhead, with its waveform
  (which comes in a little behind, by the input's latency).
- When recording stops, the takes become clips in one undo step, replacing what was
  under them (overdub); undo takes them away again.
- They are WAV files (32-bit float) in the project's `Recordings` folder (for a project
  not saved yet, `Music\SUBstation\Recordings`).
- Takes land where they were played: the engine moves them back by the driver's input
  and output latency and the delay compensation's lag, so what you played along to
  lines up.
- Changing the audio device (or its sample rate, or a reset by the driver) ends a
  recording; what was recorded so far is kept. So does moving the playhead.

### MIDI takes

- Recording records armed MIDI tracks along with audio ones: the notes show on the take
  as they are played, and become a MIDI clip of what was recorded (replacing what was
  under it), in the same undo step as the audio takes.
- Notes land where they were heard against the timeline: moved back by the audio output
  latency, the delay compensation's lag and the buffer MIDI waits for.
- A key still held when recording stops ends with the take.
- *Edit › Record Quantization* (none, 1/4 to 1/32, triplets) puts recorded notes' starts
  on that grid (lengths as played).

## Resampling

An audio track's input menu also lists *Resampling* (the master's output) and every
other track, group and return: the track then takes that one's output, after its fader
(and pan), as its input.

- Recording records it (in stereo) and the take lands exactly where it was heard, so it
  lines up with what it was recorded from; latent plug-ins on the source (or the
  master) change nothing.
- Monitored, the track hears its source instead of its clips, as it would a microphone
  (not delayed to line up with other tracks); never the master's, which would feed back
  into it.
- A source the track feeds (its own group, a return it sends to) is greyed out.
- Moving a track into the group it takes its input from, or deleting its source, leaves
  it with no input (in the same undo step).
- Resampling needs no audio inputs, so it records with WASAPI devices (and on Linux)
  too.

---

For developers: [../engine/recording.md](../engine/recording.md),
[../engine/midi.md](../engine/midi.md),
[../ui/arrangement.md](../ui/arrangement.md#track-headers) (the track headers).
