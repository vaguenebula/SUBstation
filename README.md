# GIL Studio

A basic DAW for Windows with an Ableton-style arrangement view. The UI and
editing logic are Python (PySide6/Qt 6); the real-time audio engine is C++
(miniaudio/WASAPI), bound with nanobind.

**What works today**
- Arrangement timeline with any number of audio and MIDI tracks, waveforms and note previews, adaptive grid with snapping, zoom and scroll.
- Clip editing: move (also across tracks of the same kind), Ctrl-drag to copy, trim either edge, split, duplicate, delete, rubber-band select. Overlaps follow Ableton's rule: the clip you place wins.
- Clip view (double-click a clip): for audio clips, warping, transpose/detune, clip volume and pan, for one clip or many at once. For a MIDI clip, the piano roll (below).
  - **Warp** locks a clip to the beat grid. Its audio is taken to be at the *Seg. BPM* and is stretched in real time to follow the project tempo. Turning Warp on sets Seg. BPM to the current tempo, so nothing moves until the tempo changes.
  - Warp modes: *Transients* (short stretch blocks, tight attacks), *Standard* (all-round), *Smooth* (long blocks, for pads and textures), *Formants* (Standard, keeping formants when transposing), and *Re-Pitch* (no stretching: speed and pitch change together, like a turntable). Projects saved with the earlier Ableton-style names load into the equivalent mode.
  - **Transpose/Detune** shift the pitch without changing the speed, warped or not (except in Re-Pitch).
- MIDI tracks (Ctrl+Shift+T) come with the built-in **Synth**. Double-click empty space on a MIDI track (or press Ctrl+Shift+M) to make a MIDI clip; it opens in the piano roll.
  - A MIDI clip is a window onto its notes, like an audio clip onto its file: trimming or splitting it hides notes but never deletes them. Its length is in beats, so it doesn't change with the tempo.
  - As in Ableton, a clip plays the notes that start inside it and cuts them at its end.
- Piano roll: keys (click to hear a key and select its notes), a ruler in the clip's own time (click to play from there), notes, and a velocity lane.
  - Double-click to add a note (one grid step long) or delete one. Drag notes to move them (Ctrl copies, Alt ignores the grid), drag their ends to resize, drag in empty space to select.
  - Delete, Ctrl+A, Ctrl+D (duplicate), Up/Down (Shift: an octave), Left/Right (a grid step; Shift: a bar).
  - Drag a stem in the velocity lane to change velocities; several selected notes change together.
  - Selecting a group of notes by dragging a rubber band (or with Ctrl+A) brings up a small tool bar that glides in next to them; clicking or drawing single notes doesn't. It acts on the selected notes and hides while you drag them:
    - **Legato** makes each note last until the next one starts (a chord's notes together); the last ones reach the clip's end. A note never runs into the next note on its own key.
    - **Quantize** (Ctrl+U) moves note starts onto a grid (1/4 to 1/32, or triplets), by an amount from 0 to 100 %. Lengths stay. With nothing selected, Ctrl+U quantizes every note.
    - **Humanize** nudges starts and velocities at random. At 100 % a note moves by up to a 32nd note and its velocity by up to 24; the default is 25 %.
  - Notes you click, add or move are played on the track's instrument (the headphones button turns this off). The part of the clip that plays is lit; the rest is dimmed.
- Synth: a polyphonic subtractive synth (16 voices) with sine, triangle, saw and square oscillators (band-limited saw and square), an ADSR envelope, a resonant low-pass filter and volume. Velocity sets the level.
- VST3 plug-ins, instruments and effects (see below).
- Track headers (on the right, like Ableton): activator (mute), solo, volume, pan, meters, rename, colour, resize.
- Master track, metronome, loop brace, follow mode, CPU meter.
- Browser: categories (Samples, Built-in, Plug-ins), user "Places", instant search, click-to-preview, drag-and-drop or double-click to add clips.
  - Instruments (built-in or plug-in) go on a MIDI track, replacing its instrument. With no MIDI track selected, double-clicking one or dropping it below the tracks makes one.
- Device view with the built-in Synth instrument and Utility device (gain/pan/width), and plug-ins, all through the same `Processor` interface. Parameters that choose between named values get a list; frequency and time knobs turn logarithmically. Right-click a device to move it along the chain.
- Undo/redo for all edits, `.gilproj` projects (JSON), WAV export (16/24/32-bit float).
- Audio device selection (WASAPI shared or exclusive, sample rate, buffer size).

## VST3 plug-ins

- **Finding them.** The browser lists the plug-ins in the standard VST3 folders (`C:\Program Files\Common Files\VST3` and `%LOCALAPPDATA%\Programs\Common\VST3`) under *Plug-ins › Instruments / Audio Effects*, with their vendor. A module with several plug-ins (an instrument and its FX version) shows each.
  - Plug-in files are read in a separate process, several per process, so a plug-in that crashes or hangs while loading is only marked as failed (hover over *Plug-ins* for the list and the reasons). The scan runs in the background at start-up and reads only new or changed files; the results are cached in `%LOCALAPPDATA%\GIL Studio\vst3-cache.json`. *Options › Rescan Plug-ins* reads everything again.
- **Using them.** Drag a plug-in onto a track, into the device view, or below the tracks (an instrument makes a MIDI track); or double-click it to add it to the selected track. Instruments play the MIDI clips (and the piano roll's notes) sample-accurately.
- **The device view** shows a plug-in's parameters eight at a time (‹ › pages), with the plug-in's own text for their values (`-3.0 dB`, `Bell`); lists get a menu. Read-only and hidden parameters are left out, and the device's on/off switch stands in for the plug-in's own bypass.
  - **Edit** opens the plug-in's editor in a window of its own, which floats above the main window. It follows the plug-in's resize requests, lets you resize it within the plug-in's limits (if it can resize), and tells the plug-in when it moves to a screen with another scale.
  - Turning knobs in the plug-in's editor is recorded for undo like any other edit: one step per knob drag. A plug-in that changes in a way no parameter shows (a preset picked in its editor) marks the project as changed.
  - Right-click: *Load Preset…* / *Save Preset…* read and write standard `.vstpreset` files (loading one is undoable), and *Show Editor*.
- **Projects** save each plug-in's complete state (as a `.vstpreset`, base64) and which plug-in it is. A plug-in that moved is found again by its class id; a missing one keeps its place and settings in the project, shows what's wrong in the device view, and loads when it's back (after a rescan).
- **Latency** that plug-ins report (look-ahead limiters, linear-phase EQs) is compensated: the other tracks, and the metronome, are delayed to line up, and exports come out aligned. The device's tooltip shows the latency.
- **Transport**: plug-ins get the tempo, time signature, position in samples and beats, bar position, playing state and the loop, and blocks are split where the loop wraps, so tempo-synced plug-ins stay in time.
- Mono-only plug-ins get the track mixed to mono, and their output goes to both sides. Plug-ins with more buses get silent inputs and their extra outputs are not used (no side-chains or multi-output instruments yet). MIDI controllers reach the parameters the plug-in maps them to.

## Setup

Requirements: Windows 10/11, Python 3.12, and Visual Studio 2022 or newer with
the *Desktop development with C++* workload. CMake and Ninja are installed into
the venv from PyPI.

Always activate the venv before installing anything:

```powershell
Set-ExecutionPolicy -Scope Process Bypass    # only if activation scripts are blocked
.\venv\Scripts\Activate.ps1
python -m pip install scikit-build-core nanobind cmake ninja pytest ruff PySide6 numpy
python -m pip install --no-build-isolation -e .    # compiles gilstudio._engine
```

Re-run the last command after changing any C++ code (the build is incremental,
in `build/`). Python changes need no reinstall. If the newest Visual Studio
causes trouble, pick another one with `$env:CMAKE_GENERATOR="Visual Studio 17 2022"`.

The parts of the VST 3 SDK the engine uses are included (`engine/third_party/vst3sdk`,
MIT-licensed since SDK 3.8), so nothing else needs installing. The build also makes
the small VST3 plug-ins the tests use (`tests/vst3_plugins`), installed next to the
engine; turn that off with `-C cmake.define.GILSTUDIO_TEST_PLUGINS=OFF`.

## Run

```powershell
.\venv\Scripts\Activate.ps1
python -m gilstudio                  # or: python -m gilstudio path\to\song.gilproj
```

The first launch uses the system default output device; change it in
*Options → Preferences*. The browser starts with your Music folder as a Place.
Add more with *Add Folder…*.

## Tests

```powershell
.\venv\Scripts\Activate.ps1
python -m pytest
```

- Engine tests render offline, so no audio device is needed. They check sample-exact clip placement, gain/pan/mute/solo, looping, tempo changes, the metronome, fades, the Utility device and export.
- Warp tests check that warped clips land on their beats at any tempo, keep their pitch, start sample-aligned (also after a locate), transpose to the right frequency, and that Re-Pitch filters rather than aliases.
- MIDI engine tests check that notes start on their sample and follow the tempo, that the Synth plays the right pitch and level, and that loop wraps and offline renders leave no hanging notes.
- Model tests cover overlap resolution, trims, splits (audio and MIDI), note editing, undo/redo and save/load.
- The UI tests drive the real main window offscreen: mouse drags, drops, header controls, dialogs, and the piano roll.
- VST3 tests use three test plug-ins built with the engine: an instrument with a separate controller (it reports the transport it gets back as parameters), a single-component effect with adjustable latency and a Win32 editor, and a mono effect without a controller. They check scanning (including a plug-in that crashes or hangs while loading), sample-exact notes, parameters, the transport and loop splitting, latency compensation, mono buses, state and presets, editor windows (resizing, closing, edits reported for undo), and the device view, browser, undo and projects in the application. Editor tests briefly show real windows. The tests see only these plug-ins, never the installed ones.

## Keyboard shortcuts

| Action | Keys |
|---|---|
| Play / stop (returns to the start marker) | Space |
| Stop; press again to return to the start | Stop button |
| Go to start | Home |
| Insert audio track / MIDI track | Ctrl+T / Ctrl+Shift+T |
| Insert MIDI clip (on the selected MIDI track, or over a time selection) | Ctrl+Shift+M |
| Duplicate / split at insert marker / delete | Ctrl+D / Ctrl+E / Delete |
| Select all clips | Ctrl+A |
| Undo / redo | Ctrl+Z / Ctrl+Y |
| Loop on/off | Ctrl+L |
| Zoom in / out / to arrangement | + / − / Z |
| Zoom around the mouse | Ctrl+wheel (or drag vertically in the ruler) |
| Scroll horizontally | Shift+wheel (or drag horizontally in the ruler) |
| Scroll in any direction | Ctrl+Alt drag |
| Resize the track under the mouse (piano roll: the keys' rows) | Alt+wheel |
| Narrow / widen grid, toggle snap | Ctrl+1 / Ctrl+2 / Ctrl+4 |
| Bypass snapping while dragging | hold Alt |
| Copy clips while dragging | hold Ctrl |
| Toggle browser / device view | Ctrl+Alt+B / Ctrl+Alt+L |
| Search the browser | Ctrl+F |
| Export audio | Ctrl+Shift+R |

In the piano roll, Delete, Ctrl+A and Ctrl+D act on notes, Ctrl+U quantizes them, and arrow keys move them.

## Architecture

```
src/gilstudio/                 Python: UI, model, undo, file I/O
  model/        project.py (Project/Track/Clip/MidiClip/Note + Qt signals), edits.py (pure clip maths),
                notes.py (pure note editing), editor.py (undoable operations),
                commands.py (QUndoCommands), serialization.py
  audio/        engine_bridge.py: mirrors the model into the engine; async decoding;
                polls the playhead (60 Hz) and meters (30 Hz)
  ui/           main_window, transport_bar, device_panel, dialogs, clip_view,
                arrangement/ (custom-painted ruler, lanes, headers; numpy waveform tiles),
                piano_roll/ (keys, ruler, note grid, velocity lane),
                browser/ (background file index, search, preview)
  plugins/      scanner.py: finds VST3 plug-ins and reads them in child processes (scan_worker.py); cache
engine/src/                    C++: everything on the audio thread, and plug-in hosting
  Engine        public API; edit model; builds and publishes render snapshots
  Renderer      mixing: clips and notes -> inserts -> fader/pan -> master; loop; metronome; preview
  Warp          stretch voices (time stretch / pitch shift) and the Re-Pitch resampler
  AudioSource   decoding (WAV/FLAC/MP3) at the engine rate + peak mipmaps
  AudioDevice   miniaudio WASAPI output (the only backend-specific code)
  Processor.h   insert-device interface (built-ins and plug-ins)
  processors/   built-in devices: Synth (instrument), Utility
  plugins/      PluginFormat.h (formats), Vst3Format (host context, modules, scanning),
                Vst3Processor (a VST3 plug-in as a Processor), EditorWindow (plug-in editors),
                Vst3Support.h (allocation-free event and parameter lists for the audio thread)
  bindings.cpp  nanobind module gilstudio._engine
engine/third_party/            miniaudio, Signalsmith Stretch, the VST 3 SDK (subset); all MIT
tests/vst3_plugins/            the VST3 plug-ins the tests use
```

**Real-time safety**
- The audio callback never locks, allocates, frees or touches Python, so the GIL cannot cause dropouts. (That is the host's part; what a plug-in does in its `process()` is up to the plug-in.)
- Edits build an immutable `RenderSnapshot`, in which positions are already converted to samples. It is published with one atomic pointer swap.
- Retired snapshots are freed on the UI thread once the audio thread's epoch counter shows they are no longer in use.
- Continuous controls (volume, pan, mute, solo, device parameters) are atomics, smoothed on the audio thread.
- The UI never waits on the audio thread: the playhead, meters and CPU load are read from atomics.

**MIDI**
- The UI flattens a MIDI track's clips into the notes they play, in beats (`set_track_notes`). The snapshot converts them to samples at the current tempo.
- Each block, the renderer turns notes starting in it into note-on events for the track's devices, and remembers when each ends. Note-offs come from that record, not from the snapshot, so a note edited or deleted while it sounds still ends.
- Stopping, locating and wrapping around the loop release every sounding note. A tempo change moves the recorded note ends along with the playhead.
- Notes the piano roll plays go through a lock-free queue to the next audio block, straight to the track's instrument.
- Offline renders share devices with live playback, so the engine resets them before and after (from the rendering thread, via a flag). A device that is switched off resets when it comes back on, so it can't keep notes whose note-offs it missed.

**Warping**
- The snapshot turns each clip's beats into samples at the current tempo. A warped clip's two ends sit on their beats, and it plays at `tempo / segment BPM` speed.
- Clips that need it are time-stretched on the audio thread by [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) (MIT, header-only; vendored with its FFT library in `engine/third_party/`). Nothing else is needed to build it.
- Stretchers keep state from block to block, so they live in *voices*. These are allocated on the edit side, pooled per warp-mode block size, and handed to the audio thread in the snapshot. The renderer binds a voice to a playing clip.
- A voice re-seeks when playback jumps (start, locate, loop, clip start). It uses the stretcher's `outputSeek` to pre-compute its latency, so the first frame is exactly aligned. A tempo change keeps the source position, so the clip carries on without a re-seek.
- Offline renders and exports use fresh voices with a fixed random seed, so they are repeatable and don't disturb live playback.
- Clips at their own tempo with no transposition skip the stretcher and play bit-exact. Re-Pitch uses windowed-sinc resampling, with the cutoff lowered when speeding up.

**Plug-in hosting**
- Plug-ins are created, configured, asked about and destroyed on the main thread, as VST3 requires; only `process()` runs on the audio thread. A removed plug-in waits until no snapshot uses it and is destroyed in `Engine::idle()`, on the main thread.
- The audio thread never waits for a plug-in's main-thread work. When the main thread must take a plug-in away for a moment (restarting it for a new latency or bus layout, loading its state) it takes it with a lock-free handshake, and the audio thread passes the track's audio by it meanwhile.
- Parameter values travel to the plug-in's processor through a lock-free queue; its output parameters (meters, its own changes) come back through another and reach its controller and the UI in `Engine::idle()`, with what its editor reported (edits, restarts, a closed window). Before its state is saved, queued changes are handed to it in a `process()` call without audio, so the state includes them even when no audio device runs.
- Plug-ins may run a message loop inside a call (a licence dialog) that calls back into the engine or the UI: the engine's lock is recursive and slow plug-in calls don't hold it, and the UI ignores plug-in reports until the call returns.
- A device's plug-in lives as long as the device is in its chain: reordering or changing the chain around it never reloads it. When a plug-in device goes away (deleted, or its track) its state is kept, so undo brings it back as it was.
- Delay compensation: each track is delayed (after its devices, before its fader) to line up with the track whose enabled devices add the most latency; the metronome is delayed as much. Offline renders render that much ahead and drop it.
- **CLAP** would be a second `PluginFormat` (engine/src/plugins/PluginFormat.h): its plug-ins become `Processor`s, its main-thread callbacks go in `Engine::idle()`, and the scanner, device view and projects work as they do for VST3.

## Not yet implemented

- Recording (audio or MIDI) and MIDI input from controllers.
- Looping MIDI clips, MIDI effects, and editing several MIDI clips in the piano roll at once.
- ASIO: the `AudioDevice` class is the only place a new backend has to go.
- Automation (of plug-in parameters too), tempo changes over time.
- CLAP plug-ins; side-chain inputs and multi-output instruments (plug-ins get the main buses only); MIDI effect plug-ins.
- Warp markers (warping within a clip) and automatic tempo detection: a warped clip has one segment BPM, and you set it.
- Streaming long files from disk: sources are decoded into memory.
