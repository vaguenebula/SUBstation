# GIL Studio

A basic DAW for Windows with an Ableton-style arrangement view. The UI and
editing logic are Python (PySide6/Qt 6); the real-time audio engine is C++
(miniaudio/WASAPI), bound with nanobind.

**What works today**
- Arrangement timeline with any number of audio tracks, waveforms, adaptive grid with snapping, zoom and scroll.
- Clip editing: move (also across tracks), Ctrl-drag to copy, trim either edge, split, duplicate, delete, rubber-band select. Overlaps follow Ableton's rule: the clip you place wins.
- Clip view (double-click a clip): warping, transpose/detune, clip volume and pan, for one clip or many at once.
  - **Warp** locks a clip to the beat grid. Its audio is taken to be at the *Seg. BPM* and is stretched in real time to follow the project tempo. Turning Warp on sets Seg. BPM to the current tempo, so nothing moves until the tempo changes.
  - Warp modes: *Transients* (short stretch blocks, tight attacks), *Standard* (all-round), *Smooth* (long blocks, for pads and textures), *Formants* (Standard, keeping formants when transposing), and *Re-Pitch* (no stretching: speed and pitch change together, like a turntable). Projects saved with the earlier Ableton-style names load into the equivalent mode.
  - **Transpose/Detune** shift the pitch without changing the speed, warped or not (except in Re-Pitch).
- Track headers (on the right, like Ableton): activator (mute), solo, volume, pan, meters, rename, colour, resize.
- Master track, metronome, loop brace, follow mode, CPU meter.
- Browser: categories (Samples, Plug-ins), user "Places", instant search, click-to-preview, drag-and-drop or double-click to add clips.
- Device view with a built-in Utility device (gain/pan/width), wired through the same `Processor` interface future plugins will use.
- Undo/redo for all edits, `.gilproj` projects (JSON), WAV export (16/24/32-bit float).
- Audio device selection (WASAPI shared or exclusive, sample rate, buffer size).

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
- Model tests cover overlap resolution, trims, splits, undo/redo and save/load.
- The UI tests drive the real main window offscreen: mouse drags, drops, header controls and dialogs.

## Keyboard shortcuts

| Action | Keys |
|---|---|
| Play / stop (returns to the start marker) | Space |
| Stop; press again to return to the start | Stop button |
| Go to start | Home |
| Insert audio track | Ctrl+T |
| Duplicate / split at insert marker / delete | Ctrl+D / Ctrl+E / Delete |
| Select all clips | Ctrl+A |
| Undo / redo | Ctrl+Z / Ctrl+Y |
| Loop on/off | Ctrl+L |
| Zoom in / out / to arrangement | + / − / Z |
| Zoom around the mouse | Ctrl+wheel (or drag vertically in the ruler) |
| Scroll horizontally | Shift+wheel (or drag horizontally in the ruler) |
| Narrow / widen grid, toggle snap | Ctrl+1 / Ctrl+2 / Ctrl+4 |
| Bypass snapping while dragging | hold Alt |
| Copy clips while dragging | hold Ctrl |
| Toggle browser / device view | Ctrl+Alt+B / Ctrl+Alt+L |
| Search the browser | Ctrl+F |
| Export audio | Ctrl+Shift+R |

## Architecture

```
src/gilstudio/                 Python: UI, model, undo, file I/O
  model/        project.py (Project/Track/Clip + Qt signals), edits.py (pure clip maths),
                editor.py (undoable operations), commands.py (QUndoCommands), serialization.py
  audio/        engine_bridge.py: mirrors the model into the engine; async decoding;
                polls the playhead (60 Hz) and meters (30 Hz)
  ui/           main_window, transport_bar, device_panel, dialogs,
                arrangement/ (custom-painted ruler, lanes, headers; numpy waveform tiles),
                browser/ (background file index, search, preview)
  plugins/      scanner.py: lists installed VST3/CLAP plug-ins
engine/src/                    C++: everything on the audio thread
  Engine        public API; edit model; builds and publishes render snapshots
  Renderer      mixing: clips -> inserts -> fader/pan -> master; loop; metronome; preview
  Warp          stretch voices (time stretch / pitch shift) and the Re-Pitch resampler
  AudioSource   decoding (WAV/FLAC/MP3) at the engine rate + peak mipmaps
  AudioDevice   miniaudio WASAPI output (the only backend-specific code)
  Processor.h   insert-device interface (built-ins now, VST3/CLAP later)
  bindings.cpp  nanobind module gilstudio._engine
```

**Real-time safety**
- The audio callback never locks, allocates, frees or touches Python, so the GIL cannot cause dropouts.
- Edits build an immutable `RenderSnapshot`, in which positions are already converted to samples. It is published with one atomic pointer swap.
- Retired snapshots are freed on the UI thread once the audio thread's epoch counter shows they are no longer in use.
- Continuous controls (volume, pan, mute, solo, device parameters) are atomics, smoothed on the audio thread.
- The UI never waits on the audio thread: the playhead, meters and CPU load are read from atomics.

**Warping**
- The snapshot turns each clip's beats into samples at the current tempo. A warped clip's two ends sit on their beats, and it plays at `tempo / segment BPM` speed.
- Clips that need it are time-stretched on the audio thread by [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) (MIT, header-only; vendored with its FFT library in `engine/third_party/`). Nothing else is needed to build it.
- Stretchers keep state from block to block, so they live in *voices*. These are allocated on the edit side, pooled per warp-mode block size, and handed to the audio thread in the snapshot. The renderer binds a voice to a playing clip.
- A voice re-seeks when playback jumps (start, locate, loop, clip start). It uses the stretcher's `outputSeek` to pre-compute its latency, so the first frame is exactly aligned. A tempo change keeps the source position, so the clip carries on without a re-seek.
- Offline renders and exports use fresh voices with a fixed random seed, so they are repeatable and don't disturb live playback.
- Clips at their own tempo with no transposition skip the stretcher and play bit-exact. Re-Pitch uses windowed-sinc resampling, with the cutoff lowered when speeding up.

## Adding VST3 / CLAP hosting (planned)

The seams are already in place:
- **`engine/src/Processor.h`** is what the renderer hosts in each track's insert chain.
  - It already carries transport info and an event list for MIDI and parameter automation.
  - It has latency reporting for delay compensation, and `openEditor(void* hwnd)` for plugin GUIs.
- **`engine/src/plugins/PluginFormat.h`** is the scan/instantiate interface. Implement it with:
  - the VST3 SDK (MIT-licensed since 3.8), and
  - the CLAP headers (MIT).
- **Plugin editors** embed into a Qt window via `QWidget.winId()`, which is an HWND.
- **Main-thread callbacks** that plugins request (for example CLAP's `request_callback`) go in `Engine::idle()`, which the UI already calls about 30 times per second.
- **Scanning** should run in a child process so a crashing plugin can't take the DAW down. `plugins/scanner.py` already lists the files.
- **The device panel** builds its knobs from the engine's parameter list, so plugin parameters appear without UI changes.

## Not yet implemented

- Recording, MIDI clips and instruments.
- ASIO: the `AudioDevice` class is the only place a new backend has to go.
- Plugin hosting, automation, tempo changes over time.
- Warp markers (warping within a clip) and automatic tempo detection: a warped clip has one segment BPM, and you set it.
- Streaming long files from disk: sources are decoded into memory.
