# SUBstation documentation

The project's [README](../README.md) says what SUBstation is and how to run it. These
pages go into detail: how to use it, and how each part of the code works.

## Start here

- [Architecture](architecture.md): the layers and their boundaries, the life of an edit,
  the threads, the real-time rules.
- [Building](building.md): requirements, the build, options and targets, the ASIO SDK,
  third-party code.
- [Testing](testing.md): the test suite, the fake ASIO driver and test plug-ins, CI,
  benchmarks.

## User guide

How SUBstation behaves, feature by feature: [guide/](guide/README.md).

| Page | Covers |
|---|---|
| [Arrangement](guide/arrangement.md) | timeline, clips, selection, tracks and their headers, groups, folding, the master |
| [Mixing](guide/mixing.md) | solo and mute, return tracks and sends, sidechains, delay compensation |
| [Devices](guide/devices.md) | the device view, built-in devices, racks, macros and presets, MIDI from another track |
| [Plug-ins](guide/plugins.md) | VST3: finding, using, editors, presets, latency |
| [Audio clips](guide/audio-clips.md) | the clip view, warping, transposing, tempo and key from file names |
| [MIDI](guide/midi.md) | MIDI clips, the piano roll, pitch bends and vibrato, MIDI input (MIDI 2.0 too), the computer MIDI keyboard |
| [Recording](guide/recording.md) | arming, inputs, monitoring, count-in, takes, resampling |
| [Automation](guide/automation.md) | lanes, editing envelopes, locking, overrides |
| [Browser](guide/browser.md) | places, search, preview, ranking, finding similar sounds |
| [Importing Ableton Live Sets](guide/ableton-import.md) | what of a Live Set comes across, and what doesn't |
| [File Manager and hot swap](guide/file-manager.md) | the project's files, finding missing ones, replacing a file everywhere, hot-swapping samples |
| [Audio setup](guide/audio-setup.md) | ASIO and WASAPI, sample rate, buffers, audio threads, MIDI inputs |
| [Keyboard shortcuts](guide/shortcuts.md) | every shortcut |
| [Limitations](guide/limitations.md) | what isn't implemented yet |

## Code reference

### Audio engine

[engine/src](../engine/src), no Qt: everything on the audio thread, and the plug-in hosting.

| Page | Covers |
|---|---|
| [Engine overview](engine/README.md) | the `Engine` API, edit model, snapshots |
| [Rendering](engine/rendering.md) | the `Renderer`: chunks, strips, faders, metronome, loop |
| [Routing](engine/routing.md) | edges, groups, returns and sends, sidechains, racks, delay compensation |
| [Scheduler](engine/scheduler.md) | rendering the graph on several threads, deterministically |
| [Audio devices](engine/audio-devices.md) | WASAPI and ASIO backends, driver events |
| [Recording](engine/recording.md) | the recorder, take placement, input monitoring, resampling |
| [MIDI](engine/midi.md) | MIDI playback, per-note pitch bends (MIDI 2.0), MIDI input, devices taking another track's notes |
| [Automation](engine/automation.md) | envelopes, normalized parameters, sample-accurate playback |
| [Warping and sources](engine/warp.md) | time stretching, Re-Pitch, decoding, peaks |
| [Devices](engine/devices.md) | the `Processor` interface and the built-in devices; adding one |
| [Plug-in hosting](engine/plugins.md) | VST3 hosting, editors, threading |

### Application layer

[app/src](../app/src), Qt Core and Gui, no Qt Quick: the project and everything that works on it.

| Page | Covers |
|---|---|
| [Model](app/model.md) | `model/`, `editor/`: the project, its types and signals, pure edit maths, the editor, undo commands |
| [Serialization](app/serialization.md) | `io/`: `.gilproj` projects, `.gilpreset` presets, the preset library |
| [Importing Ableton Live Sets](app/live-import.md) | `io/LiveSet`, `io/LiveImport`: reading a Live Set and translating it into a project |
| [Engine bridge](app/engine-bridge.md) | `audio/`: mirroring the project into the engine, transport, plug-ins, recording, renders, settings |
| [Session](app/session.md) | `session/`, `files/`: the main window's logic: files, transport, Edit and Create commands, renders, preferences; the File Manager (missing files found, files replaced) and hot swaps |
| [Plug-in scanner](app/plugin-scanner.md) | `plugins/`, `tools/scanner`: finding and reading VST3 plug-ins in child processes |
| [Analysis](app/analysis.md) | `analysis/`: signal maths for the devices' editors (the Sidechain's fit, spectra) |

### UI

[ui/](../ui), Qt Quick: QML for the layout, C++ scene-graph items for what draws time.

| Page | Covers |
|---|---|
| [UI overview](ui/README.md) | the main window, transport bar, dialogs, controls, the scene-graph painter, theme |
| [Arrangement view](ui/arrangement.md) | ruler, lanes, headers, automation lanes |
| [Piano roll](ui/piano-roll.md) | the piano roll: notes, keys, velocity lane, note tools, bend mode and vibrato |
| [Device view](ui/device-view.md) | the device panel, racks, built-in devices' editors, the clip view |

### Platform

[platform/src](../platform/src), no Qt: what the engine, the browser and the intelligence module need from the
operating system.

| Page | Covers |
|---|---|
| [Platform layer](platform.md) | paths in the system's form and their keys, files, binary fields and checksums, threads' priorities; one file per system |

### Browser

[browser/src](../browser/src), no Qt, and its application side in [app/src/browser](../app/src/browser).

| Page | Covers |
|---|---|
| [Browser](browser.md) | the backend (`browser/src`) and its application side (`app/src/browser`), the panel |

### Intelligence

[intelligence/src](../intelligence/src), no Qt, and its application side in [app/src/intelligence](../app/src/intelligence).

| Page | Covers |
|---|---|
| [Intelligence](intelligence.md) | sound similarity: the method and why, the fingerprint, comparing, the background index and its store, measurements; harmony: a song's chords and key from its MIDI, block chords and bass lines from them; humanizing: velocities from HUMANBRO's model (vendored, its model shipped); later MIDI generation by machine learning, a timing model, chords from audio, an MCP server |
