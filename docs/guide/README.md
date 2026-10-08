# User guide

How SUBstation behaves as you use it. SUBstation is a basic DAW for Windows with an
Ableton-style arrangement view: tracks run down the screen with their headers on the
right, the browser is on the left, the device view (the selected track's devices) runs
along the bottom beside the info view (what the control under the mouse is), and the
clip view opens over the arrangement. It also runs on Linux, with fewer audio,
MIDI and plug-in features (see [On Linux](#on-linux)).

For building and running the program see [../building.md](../building.md); for how the
code is put together, [../README.md](../README.md).

## What works today

| Page | What it covers |
|---|---|
| [arrangement.md](arrangement.md) | The timeline, clips and selecting, tracks and their headers, group tracks, folding, the master, the title bar and the transport bar, projects, undo and export |
| [mixing.md](mixing.md) | Solo and mute, groups in the mix, return tracks and sends, sidechains, delay compensation, freezing and flattening |
| [devices.md](devices.md) | The device view, the built-in devices (Synth, Sampler, Utility, Over The Top, Compressor, Delay, EQ, Sidechain), racks, macros and presets, folding, cut/copy/paste |
| [plugins.md](plugins.md) | VST3 plug-ins: finding them, using them, their editors and presets, projects, latency |
| [audio-clips.md](audio-clips.md) | The clip view for audio clips: warping, warp modes, transpose and detune, clip gain and pan, reversing, tempo and key from file names |
| [midi.md](midi.md) | MIDI clips, the piano roll and its note tools, MIDI input, the computer MIDI keyboard |
| [recording.md](recording.md) | Arming, inputs, monitoring, the count-in, takes, resampling |
| [automation.md](automation.md) | Automation lanes and editing envelopes, Lock Envelopes, overriding and re-enabling |
| [browser.md](browser.md) | The browser: categories, places, search, preview, ranking |
| [audio-setup.md](audio-setup.md) | Preferences: ASIO and WASAPI (the *System* driver on Linux), sample rate, buffer size, audio threads, MIDI inputs, plug-in folders |
| [shortcuts.md](shortcuts.md) | Every keyboard shortcut, and which keys reach a plug-in's editor |
| [limitations.md](limitations.md) | What isn't implemented yet |

## In a nutshell

- Any number of audio and MIDI tracks, with waveforms and note previews, an adaptive
  grid with snapping, zoom and scroll.
- Clip editing (move, copy, trim, split, duplicate, delete) with an always-on grid
  selection, as in Ableton.
- Group tracks, return tracks and sends, sidechains, and delay compensation everywhere.
- Freezing tracks, groups and returns (Ctrl+Shift+F), and flattening frozen tracks.
- Built-in Synth and Sampler instruments; Utility, Over The Top, Compressor, Delay, EQ
  and Sidechain effects; VST3 instruments and effects; racks with chains and macros.
- Warping and transposing audio, set up from the file name.
- A piano roll with legato, timing, quantize and humanize tools (velocities by machine learning).
- Audio and MIDI recording, resampling, MIDI input and a computer MIDI keyboard.
- Automation of every device parameter and of volume, pan and sends.
- Undo and redo for all edits, `.gilproj` projects, WAV export.

## Starting it

Build it as [../building.md](../building.md) says, then run `build\bin\substation.exe`
on Windows or `build/bin/substation` on Linux. Give it a project to open it at once:

```
build\bin\substation.exe C:\Music\song.gilproj
build/bin/substation ~/Music/song.gilproj
```

The first start uses the system's default output device; change it in
*Options › Preferences* (see [audio-setup.md](audio-setup.md)). The browser starts with
your Music folder as a place (your home folder if there is none); add more with *Add
Folder…* (see [browser.md](browser.md)).

## Where it keeps things

| What | Windows | Linux |
|---|---|---|
| Settings: the audio device, MIDI inputs, plug-in folders, the browser's places and sort, recent projects, the window's layout | the registry, `HKEY_CURRENT_USER\Software\SUBstation\SUBstation` | `~/.config/SUBstation/SUBstation.conf` |
| The browser's index and use counts, the plug-in scan's cache | `%LOCALAPPDATA%\SUBstation` | `~/.local/share/SUBstation` |
| Presets | `Documents\SUBstation\Presets` | `~/Documents/SUBstation/Presets` |
| Recordings of a project not saved yet | `Music\SUBstation\Recordings` | `~/Music/SUBstation/Recordings` |

A saved project's recordings go into a `Recordings` folder beside it.

## On Linux

SUBstation is made for Windows; on Linux it runs with these differences:

- Audio plays through the system's default sound backend (PulseAudio, ALSA, JACK...) as
  the *System* driver: output only, so audio is recorded only by resampling. There is
  no ASIO and no exclusive mode (see [audio-setup.md](audio-setup.md)).
- No MIDI devices are listed; the computer MIDI keyboard plays and records (see
  [midi.md](midi.md#computer-midi-keyboard)).
- VST3 plug-ins load and play, but their own editor windows don't open: edit them in
  the device view (see [plugins.md](plugins.md#the-plug-ins-editor)). Plug-ins are
  looked for in `~/.vst3`, `/usr/lib/vst3` and `/usr/local/lib/vst3`.

---

For developers: [../README.md](../README.md) (the docs' index) and
[../architecture.md](../architecture.md).
