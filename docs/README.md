# SUBstation documentation

The project's [README](../README.md) says what SUBstation is and how to run it. These
pages go into detail: how to use it, and how each part of the code works.

## Start here

- [Architecture](architecture.md): the layers, the threads, the life of an edit, the
  real-time rules.
- [Building](building.md): requirements, the build, the ASIO SDK, native module versions.
- [Testing](testing.md): the test suite, the fake ASIO driver and test plug-ins, benchmarks.

## User guide

How SUBstation behaves, feature by feature: [guide/](guide/README.md).

| Page | Covers |
|---|---|
| [Arrangement](guide/arrangement.md) | timeline, clips, selection, tracks and their headers, groups, folding, the master |
| [Mixing](guide/mixing.md) | solo and mute, return tracks and sends, sidechains, delay compensation |
| [Devices](guide/devices.md) | the device view, built-in devices, racks, macros and presets |
| [Plug-ins](guide/plugins.md) | VST3: finding, using, editors, presets, latency |
| [Audio clips](guide/audio-clips.md) | the clip view, warping, transposing, tempo and key from file names |
| [MIDI](guide/midi.md) | MIDI clips, the piano roll, MIDI input, the computer MIDI keyboard |
| [Recording](guide/recording.md) | arming, inputs, monitoring, count-in, takes, resampling |
| [Automation](guide/automation.md) | lanes, editing envelopes, locking, overrides |
| [Browser](guide/browser.md) | places, search, preview, ranking |
| [Audio setup](guide/audio-setup.md) | ASIO and WASAPI, sample rate, buffers, audio threads, MIDI inputs |
| [Keyboard shortcuts](guide/shortcuts.md) | every shortcut |
| [Limitations](guide/limitations.md) | what isn't implemented yet |

## Code reference

### Python ([src/substation](../src/substation))

| Page | Covers |
|---|---|
| [Model](python/model.md) | `model/`: the project, pure edit maths, the editor, undo commands |
| [Analysis](python/analysis.md) | `analysis/`: signal maths for the editors (the Sidechain's fit) |
| [Serialization](python/serialization.md) | `.gilproj` projects and `.gilpreset` rack presets |
| [Engine bridge](python/engine-bridge.md) | `audio/`, `app.py`: start-up, mirroring the model into the engine |
| [Plug-in scanner](python/plugin-scanner.md) | `plugins/`: finding and reading VST3 plug-ins in child processes |
| [UI overview](ui/README.md) | main window, transport bar, dialogs, widgets, theme |
| [Arrangement view](ui/arrangement.md) | `ui/arrangement/`: ruler, lanes, headers, automation lanes |
| [Piano roll](ui/piano-roll.md) | `ui/piano_roll/` |
| [Device view](ui/device-view.md) | device panel, racks, built-in devices' editors, clip view |
| [Browser](browser.md) | the native backend (`browser/src`) and the panel (`ui/browser/`) |

### Audio engine ([engine/src](../engine/src))

| Page | Covers |
|---|---|
| [Engine overview](engine/README.md) | the `Engine` API, edit model, snapshots, bindings |
| [Rendering](engine/rendering.md) | the `Renderer`: chunks, strips, faders, metronome, loop |
| [Routing](engine/routing.md) | edges, groups, returns and sends, sidechains, racks, delay compensation |
| [Scheduler](engine/scheduler.md) | rendering the graph on several threads, deterministically |
| [Audio devices](engine/audio-devices.md) | WASAPI and ASIO backends, driver events |
| [Recording](engine/recording.md) | the recorder, take placement, input monitoring, resampling |
| [MIDI](engine/midi.md) | MIDI playback and MIDI input |
| [Automation](engine/automation.md) | envelopes, normalized parameters, sample-accurate playback |
| [Warping and sources](engine/warp.md) | time stretching, Re-Pitch, decoding, peaks |
| [Devices](engine/devices.md) | the `Processor` interface and the built-in devices; adding one |
| [Plug-in hosting](engine/plugins.md) | VST3 hosting, editors, threading |
