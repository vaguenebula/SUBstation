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

A track's input is chosen in its header's In/Out column, as in Ableton: **Audio From**
(a MIDI track's **MIDI From**) and, under it, the channel (or where the source is taken).
An audio track's Audio From lists:

- *Ext. In*: the audio device's inputs, and under it the channel or pair (*1*, *2*, ...,
  then *1/2*, *3/4*, ...; their names show as tooltips). Choosing Ext. In takes the
  first pair;
- *Configure…*: the audio preferences, where the driver and its inputs are chosen;
- *Resampling* (the master's output) and every other track, group and return (see
  [Resampling](#resampling));
- *No Input*.

Choosing an input the driver hasn't open opens it (and keeps it open next time). With
WASAPI there are no device inputs: the channel menu says to choose an ASIO driver (see
[audio-setup.md](audio-setup.md)). On Linux (the *System* driver) there are none either,
and the menu just says so.

A MIDI track's MIDI From is *All Ins*, the *Computer Keyboard* or one MIDI input, or *No
Input*, and under it the channel (*All Channels*, *Ch. 1* to *Ch. 16*); see
[midi.md](midi.md#midi-input).

## Monitoring

Each track's **In**, **Auto** and **Off** buttons (under its input, the one chosen lit) choose:

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

An audio track's Audio From also lists *Resampling* (the master's output) and every
other track, group and return: the track then takes that one's output as its input.
Under it, where it is taken, as in Ableton: *Post Mixer* (after its fader and pan, the
default), *Post FX* (after its devices, before its fader) or *Pre FX* (before its
devices; a MIDI track's after its instrument). The master's is taken as it is heard.

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
