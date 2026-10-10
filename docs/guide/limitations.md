# Limitations

What SUBstation doesn't do yet, by area.

## Recording and MIDI input

- MIDI overdub (recording into existing clips).
- Recording controllers (sustain, channel pitch bend): they are played, not recorded.
  (MIDI 2.0's per-note pitch bends are recorded, as the notes' bends.)
- Windows MIDI Services (MIDI input is WinMM), and with it MIDI 2.0 devices: the engine
  takes MIDI 2.0's messages (Universal MIDI Packets) and plays their per-note pitch
  bends, but no driver hands them over yet. MIDI 2.0's other per-note controllers, and
  its 16-bit velocities and 32-bit controllers' resolution, are played as MIDI 1.0's.
- Recording WASAPI devices' inputs: WASAPI opens outputs only (resampling works).
- Punching in and out at the loop, and stacking takes while looping.

## Routing

- *Ext. Out*: tracks can't play on the audio device's outputs directly; they go through
  the master (whose outputs are chosen as its *Main Out*). There is no cue output.
- What goes into a track's Track In (other tracks' Audio To) is heard while it monitors
  but not recorded with it: record another track's output through Audio From instead.
- A MIDI track's notes reach other tracks' devices (*MIDI From* on a device), but a
  track can't take another MIDI track's notes as its MIDI input (Ableton's MIDI From a
  track), and notes going into a device aren't delayed to line up with plug-in latency
  before it.

## MIDI editing

- Looping MIDI clips.
- MIDI effects.
- Note bends edit pitch only: MIDI 2.0's other per-note expressions (pressure,
  brightness, per-note controllers) can't be drawn.
- Chords and the key are worked out from MIDI only, not from audio clips, and the
  project has one time signature for them to follow. Generate writes block chords and
  bass lines; melodies and accompaniment with rhythms of their own are to come.

## Automation and tempo

- Recording automation (writing it while playing).
- Tempo changes over time.
- Automation lanes below a track have a fixed height.

## Devices and plug-ins

- CLAP plug-ins.
- Multi-output instruments: plug-ins get their main buses and a sidechain only.
- MIDI effect plug-ins.
- Notes' bends reach VST3 plug-ins as note expression only (VST3's per-note "tuning"):
  one that doesn't take it plays the notes unbent (there is no MPE fallback).
- The Sampler plays one sample (no zones or multisamples). Of Simpler, it doesn't have
  manual slicing (moving or adding slices by hand), slicing to a drum rack, previewing
  slices by clicking them, warp markers within the sample or detecting its tempo (Warp
  takes the whole sample as so many beats), filter and pitch envelopes, the filter's
  analog-modelled circuits, or Spread.

## Audio clips

- Warp markers (warping within a clip) and automatic tempo detection: a warped clip has
  one segment BPM, and you set it.
- Streaming long files from disk: sources are decoded into memory.

## Audio devices

- DSD ASIO drivers.
- More than one ASIO driver open at once.

## On Linux

- Audio inputs: the *System* driver opens outputs only (resampling works), and there is
  no ASIO.
- MIDI devices: only the computer MIDI keyboard plays.
- Plug-ins' own editor windows: plug-ins are edited in the device view.

See [README.md](README.md#on-linux).

---

For developers: [../../TODO.md](../../TODO.md), [../architecture.md](../architecture.md).
