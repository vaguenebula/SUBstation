# User guide

How SUBstation behaves as you use it. SUBstation is a basic DAW for Windows with an
Ableton-style arrangement view: tracks run down the screen with their headers on the
right, the browser is on the left, and the device view (the selected track's devices)
and the clip view open below the arrangement.

For building and running the program see [../building.md](../building.md); for how the
code is put together, [../README.md](../README.md).

## What works today

| Page | What it covers |
|---|---|
| [arrangement.md](arrangement.md) | The timeline, clips and selecting, tracks and their headers, group tracks, folding, the master, the transport bar, projects, undo and export |
| [mixing.md](mixing.md) | Solo and mute, groups in the mix, return tracks and sends, sidechains, delay compensation |
| [devices.md](devices.md) | The device view, the built-in devices (Synth, Sampler, Utility, Over The Top, Compressor), racks, macros and presets, folding, cut/copy/paste |
| [plugins.md](plugins.md) | VST3 plug-ins: finding them, using them, their editors and presets, projects, latency |
| [audio-clips.md](audio-clips.md) | The clip view for audio clips: warping, warp modes, transpose and detune, clip volume and pan, tempo and key from file names |
| [midi.md](midi.md) | MIDI clips, the piano roll and its note tools, MIDI input, the computer MIDI keyboard |
| [recording.md](recording.md) | Arming, inputs, monitoring, the count-in, takes, resampling |
| [automation.md](automation.md) | Automation lanes and editing envelopes, Lock Envelopes, overriding and re-enabling |
| [browser.md](browser.md) | The browser: categories, places, search, preview, ranking |
| [audio-setup.md](audio-setup.md) | Preferences: ASIO and WASAPI, sample rate, buffer size, audio threads, MIDI inputs |
| [shortcuts.md](shortcuts.md) | Every keyboard shortcut, and which keys reach a plug-in's editor |
| [limitations.md](limitations.md) | What isn't implemented yet |

## In a nutshell

- Any number of audio and MIDI tracks, with waveforms and note previews, an adaptive
  grid with snapping, zoom and scroll.
- Clip editing (move, copy, trim, split, duplicate, delete) with an always-on grid
  selection, as in Ableton.
- Group tracks, return tracks and sends, sidechains, and delay compensation everywhere.
- Built-in Synth and Sampler instruments; Utility, Over The Top and Compressor effects;
  VST3 instruments and effects; racks with chains and macros.
- Warping and transposing audio, set up from the file name.
- A piano roll with legato, timing, quantize and humanize tools.
- Audio and MIDI recording, resampling, MIDI input and a computer MIDI keyboard.
- Automation of every device parameter and of volume, pan and sends.
- Undo and redo for all edits, `.gilproj` projects, WAV export.

## First start

The first launch uses the system's default output device; change it in
*Options › Preferences* (see [audio-setup.md](audio-setup.md)). The browser starts with
your Music folder as a place; add more with *Add Folder…* (see [browser.md](browser.md)).

---

For developers: [../README.md](../README.md) (the docs' index) and
[../architecture.md](../architecture.md).
