# Testing

The test suite is pytest, in [tests/](../tests). It needs no sound card and no installed plug-ins: the engine renders
offline, small VST3 plug-ins (and, with the ASIO SDK, a fake ASIO driver) are built with the engine, and the UI tests drive the real main
window offscreen. Benchmarks live apart, in [benchmarks/](../benchmarks), and are not run by pytest.

## Running the tests

```powershell
.\venv\Scripts\Activate.ps1
python -m pytest
```

`pyproject.toml` sets `testpaths = ["tests"]`. The usual pytest selections work:

```powershell
python -m pytest tests/test_browser_native.py          # one file
python -m pytest -k sidechain                          # by name
python -m pytest -x -q                                 # stop at the first failure
```

Before running, the compiled modules have to be current: rebuild after changing C++ (see
[building.md](building.md#rebuilding)). If the engine's `API_VERSION` doesn't match `ENGINE_API`, `conftest.py`
stops the whole run at once with the message saying to rebuild (`pytest.exit`), since the tests would otherwise
fail in confusing ways.

### What gets skipped

| Condition | Skipped |
|---|---|
| Engine built without the ASIO SDK (`"ASIO" not in driver_types()`) | ASIO, recording, resampling, MIDI input and live parallel tests, and the rack test `test_chain_meters` |
| ASIO SDK there but no fake driver (`SUBTestAsio.dll` missing: built with `SUBSTATION_TEST_PLUGINS=OFF`) | ASIO, recording, resampling, MIDI input and live parallel tests (they use `test_asio.py`'s marker, which checks both). `test_chain_meters` checks only `driver_types()`, so it **fails** rather than skips here. |
| Test plug-ins not built (`SUBSTATION_TEST_PLUGINS=OFF`) | VST3, scanner, sidechain, rack, send, group and parallel tests that use them |
| Not Windows | the browser's file system tests, plug-in editor tests |
| `_browser.UNICODE_VERSION` differs from the running Python's | the browser's every-character text tests |

## Layout

```
tests/
  conftest.py              environment, shared fixtures (app, window, WAV files)
  browser_reference.py     the browser's former Python index and search, kept as the reference
  asio_driver/             the fake ASIO driver (C++; built with the engine)
  vst3_plugins/            the test VST3 plug-ins (C++; built with the engine)
  test_*.py                the tests (table below)
```

Test files are named by area and layer: `*_engine.py` drive `substation._engine` directly (mostly offline renders),
`*_model.py` the Python model (often with the bridge to check that the engine takes it), `test_ui_*.py` the real
window.

## conftest.py

[tests/conftest.py](../tests/conftest.py) sets up the environment as it is imported, before any test:

| Setting | Why |
|---|---|
| `QT_QPA_PLATFORM=offscreen` (unless set) | Qt windows are made offscreen. |
| engine API check | `engine_mismatch()`; exits the run if the engine is stale. |
| `SUBSTATION_VST3_PATH` = the test plug-ins' folder | The scanner sees only the test plug-ins, never the installed ones. |
| `SUBSTATION_PLUGIN_CACHE` = a temporary file | The tests keep their own scan cache. |
| `SUBSTATION_LIBRARY`, `SUBSTATION_BROWSER_INDEX` = temporary files | Nor the user's use counts or browser index (each `window` gets its own). |
| `SUBSTATION_PRESETS` = a temporary folder | Nor the user's preset library (each `window` gets its own). |
| `SUBSTATION_ASIO_DRIVERS` = `SUB Test ASIO\|{5B2E8C1A-7F3D-4E6B-9C0A-1D2F3E4A5B6C}\|<SUBTestAsio.dll>` | The engine lists only the fake driver, loaded from its DLL, never the installed drivers. |

Constants: `SAMPLE_RATE = 48000` (the engine's rate when no device is open), `TEST_PLUGINS` (the
`SUBTestPlugins.vst3` bundle next to `_engine`), `TEST_ASIO` (`_testdrivers/SUBTestAsio.dll`) and
`TEST_ASIO_NAME = "SUB Test ASIO"`. `write_wav(path, samples, sample_rate)` writes float samples as 16-bit PCM.

| Fixture | Scope | What it gives |
|---|---|---|
| `make_wav` | function | A factory: `make_wav(samples, sample_rate)` writes `clipN.wav` in `tmp_path` and returns its path. |
| `dc_wav` | function | One second of constant 0.5 in both channels. |
| `ramp_wav` | function | A mono file whose sample i is i / 32768 (exact in 16-bit PCM), to check positions sample by sample. |
| `app` | module | The `QApplication`, with the theme applied and its own organization and application names ("SUBstation Tests"), so the tests stay out of the user's settings. |
| `window` | function | The real `MainWindow`, offscreen, 1400 x 820, with no audio device. Settings are cleared, the browser's only place is `tmp_path`, and its use counts and index are in `tmp_path`. |

The `window` fixture collects exceptions raised inside Qt slots (which Qt would only print) through `sys.excepthook`
and fails the test with them at the end. On teardown it marks the undo stack clean, closes the window and the
device, shuts the bridge down (which unloads the plug-ins) and deletes the window.

Test files add their own fixtures, for example `driver` and `engine` in
[test_asio.py](../tests/test_asio.py) (the `engine` fixture checks the driver was released), and `backends` in
[test_browser_native.py](../tests/test_browser_native.py).

## The fake ASIO driver

[tests/asio_driver/test_asio_driver.cpp](../tests/asio_driver/test_asio_driver.cpp) is *SUB Test ASIO*: a real
in-process COM object, like a real driver, loaded from its DLL rather than registered. `SUBSTATION_ASIO_DRIVERS`
lists drivers to use instead of the installed ones, as `Name|{CLSID}|C:\path\driver.dll`, several separated by `;`
(read by `listDrivers()` in [engine/src/backends/AsioBackend.cpp](../engine/src/backends/AsioBackend.cpp)). So the
ASIO tests never see the installed drivers and need no sound card. It is only built when the engine is built with the
ASIO SDK.

It has 4 inputs and 6 outputs. By default it uses `ASIOSTInt32LSB`, buffer sizes 32 to 2048 (preferred 256, powers of
two), input and output latencies of the buffer size plus 32 and 64 samples, and 44.1 kHz.

Once started, a thread calls the host's `bufferSwitchTimeInfo` at the pace a sound card would. In **manual mode** the
test drives it instead, a buffer at a time on the test's thread, so what the host plays can be checked sample by
sample. Its sample formats are encoded in the driver independently of the engine's conversions, and the tests decode
the outputs with numpy.

The hooks are exported functions ([test_asio_driver.def](../tests/asio_driver/test_asio_driver.def)) called through
ctypes (the `Driver` class in `test_asio.py` and the other driver tests load the same DLL the engine loaded):

| Hook | What it does |
|---|---|
| `SubTestAsio_Reset()` | Restores the default configuration and clears statistics and captured output. |
| `SubTestAsio_SetSampleType(type)` | The next instance's `ASIOSampleType`. |
| `SubTestAsio_SetBufferSizes(min, max, preferred, granularity)` | What `getBufferSize` reports (a driver that sets its size itself has min = max). |
| `SubTestAsio_SetLatencies(inputExtra, outputExtra)` | Latencies reported on top of the buffer size. |
| `SubTestAsio_SetManual(on)` | Manual mode. |
| `SubTestAsio_SetInputLevel(channel, level)` | What an input delivers. |
| `SubTestAsio_SetLoopback(output, input)` | A cable from an output back into an input, delayed by the input and output latencies the driver reports, as a real loopback would be (-1, -1: none). |
| `SubTestAsio_FailInit(message)` | `init()` fails with this message. |
| `SubTestAsio_SetControlPanelChange(bufferSize, rate)` | What the control panel changes when it is opened. |
| `SubTestAsio_Process(buffers)` | Manual mode: runs that many buffer switches now; returns how many ran. |
| `SubTestAsio_ReadOutput(channel, dest, maxBytes)` | The bytes the host wrote to an output so far, raw in the driver's format (up to 8 MB a channel); with no destination, how many there are. |
| `SubTestAsio_ClearOutput()` | Forgets the captured output. |
| `SubTestAsio_SendMessage(selector, value)` | Sends the host an `asioMessage` (reset request, latencies changed, ...); -1 if no host listens. |
| `SubTestAsio_ChangeSampleRate(rate)` | The "hardware" changes its rate (another clock) and tells the host. |
| `SubTestAsio_Get(key)` | Statistics: `instances`, `inits`, `control_panels`, `starts`, `running`, `switches`, `time_info`, `buffer_size`, `inputs`, `outputs`, `output_ready`, `rate`. |
| `SubTestAsio_InitHandle()` | The window handle the host passed to `init()`. |

With the loopback, a take of what the engine played comes back as the input it records, so a recording can be held
sample by sample to the arrangement it was recorded against. `benchmarks/parallel_render_bench.py --live` uses the
same driver in manual mode to time the audio thread.

## The test VST3 plug-ins

[tests/vst3_plugins](../tests/vst3_plugins) builds one module, `SUBTestPlugins.vst3`, with four tiny plug-ins whose
output the tests can predict exactly. They cover the host's code paths rather than sounding good:

| Plug-in | What it is |
|---|---|
| **SUB Test Synth** (instrument; [test_plugins.cpp](../tests/vst3_plugins/test_plugins.cpp)) | A processor with a separate edit controller. Each held note adds velocity/127 (*DC* mode) or a sine at the note's pitch, times *Gain*. It reports the transport it was given (tempo, playing, beat, loop) back as read-only parameters, and has ten *Macro* parameters that only its controller keeps (in its own state). |
| **SUB Test Effect** ([test_effect.cpp](../tests/vst3_plugins/test_effect.cpp)) | A single-component effect (processor and controller in one object, like most JUCE plug-ins): *Gain*, a *Latency* parameter that delays the audio and reports it (up to 4096 samples), a bypass parameter, state, and a Win32 editor that can resize itself and edit a parameter on request. |
| **SUB Test Mono** | Mono in, mono out, no edit controller. |
| **SUB Test Sidechain** ([test_sidechain.cpp](../tests/vst3_plugins/test_sidechain.cpp)) | A single-component effect with a stereo aux input, inactive until the host activates it: its output is its input plus its sidechain, and a read-only parameter, *Key Silent*, says whether the host flagged the sidechain silent. |

Environment variables make the module misbehave on purpose, for the scanner tests: `SUB_TEST_PLUGIN_CRASH=1` kills
the process as the module loads, `SUB_TEST_PLUGIN_HANG=1` makes loading hang.

The single-component plug-ins have their own translation units because the SDK's `SingleComponentEffect` renames
`IEditController::setState` with a macro, which only works if `vstsinglecomponenteffect.h` comes first
([test_plugins.h](../tests/vst3_plugins/test_plugins.h)).

## UI tests

The UI tests drive the real main window offscreen (`QT_QPA_PLATFORM=offscreen`, the `window` fixture): mouse drags,
drops, header controls, dialogs, the piano roll, devices' own editors, automation lanes. They send real Qt events
to the widgets (`QTest` mouse and key events, or events posted directly) and check both the model and what the engine plays. Plug-in editors are
real Win32 windows: editor tests briefly show them on screen.

## What the tests cover, by area

### ASIO

ASIO tests use a fake ASIO driver built with the engine (`tests/asio_driver`): a real in-process COM object, loaded
from its DLL rather than registered (`SUBSTATION_ASIO_DRIVERS`), so they never see the installed drivers and need no
sound card. Its hooks (called through ctypes) let a test drive it a buffer at a time, feed its inputs, read back the
bytes the engine wrote to its outputs, and send the engine what drivers send (reset requests, new latencies, a new
sample rate). They check every sample format against numpy's decoding, output channels and mono, inputs, rates and
buffer sizes, resets and the control panel, errors, playing on the driver's own thread, and the preferences and
start-up in the application. They are skipped when the engine was built without the ASIO SDK.
([test_asio.py](../tests/test_asio.py))

### Recording

Recording tests use the same driver; it can also loop an output back into an input, as a cable would, delayed by the
latencies it reports. A take of what the engine played then lines up with the timeline sample for sample, with and
without latent plug-ins (on a track or the master). Others check input monitoring (its modes, and that a monitored
track isn't delayed by another track's latency), stereo and mono takes and their live peaks, punching in, the
count-in, what ends a recording (a locate, a device change), and in the application: the header controls, the record
button, takes becoming clips in one undo step (and undo removing them), Space and device changes during recording.
([test_recording.py](../tests/test_recording.py), [test_ui_recording.py](../tests/test_ui_recording.py),
[test_edits.py](../tests/test_edits.py) for takes replacing what was under them)

### Resampling

Resampling tests record a track's, a group's or the master's output and hold the take, sample for sample, to an
offline render of that source, with latent plug-ins on the source, in the group and on the master, and a slower track
the source is delayed to line up with; device and resampled takes recorded together are each placed by their own
input. Others check that monitoring a track's output isn't delayed and the master's can't be heard, solo across a
monitored input, cycles refused (through groups, sends and other inputs), and a source going (also while recording).
In the model: undo, sources greyed out or refused, inputs dropped by regrouping and deleting (one undo step), saving,
and the engine taking it all; in the window: the input menu and a take recorded from a track and from the master.
([test_resampling_engine.py](../tests/test_resampling_engine.py),
[test_resampling_model.py](../tests/test_resampling_model.py), [test_ui_resampling.py](../tests/test_ui_resampling.py))

### MIDI input

MIDI input tests send messages (`send_midi_input()`) stamped against the audio clock the engine reports after a
buffer, so where each plays is known to the sample: live notes reach a test instrument (SUB Test Synth in DC mode,
which shows exactly when each note starts and stops) at their offsets in a block, tracks hear the inputs and channels
they choose, held keys are released when the transport stops or a track stops hearing them, and recorded notes land
exactly on the beats a player heard them on (with latent plug-ins on a track or the master). In the application: the
MIDI header controls, a MIDI take with its live notes and record quantization, and the MIDI preferences. The computer
MIDI keyboard has its own tests. ([test_midi_input.py](../tests/test_midi_input.py),
[test_ui_recording.py](../tests/test_ui_recording.py), [test_computer_keyboard.py](../tests/test_computer_keyboard.py))

### Engine rendering

Engine tests render offline, so no audio device is needed. They check sample-exact clip placement, gain/pan/mute/solo,
looping, tempo changes, the metronome, fades, the Utility device, the Compressor (its curve, attack on low notes,
sidechain keying and its displays), the Sampler (pitch from key, root and tuning at any file rate, start, end and
loop, velocity, its state's text and a missing file, and swapping samples while notes play, many times between
blocks) and export; and automation: volume and pan (tracks and master) sample by sample, curves, device parameters
(split exactly where they change, discrete ones in steps), and the normalized mapping of parameters.
([test_engine_render.py](../tests/test_engine_render.py), [test_compressor_engine.py](../tests/test_compressor_engine.py),
[test_sampler_engine.py](../tests/test_sampler_engine.py), [test_automation_engine.py](../tests/test_automation_engine.py))

### Groups

Tracks going into other tracks (group buses), delay compensation at every summing point, and solo and mute across
levels, rendered offline; in the model the tree kept in the flat track list, grouping, ungrouping, moving tracks into
and out of groups (all undoable), folding and saving; in the window Ctrl+G, folding, the group's header and summary
lane, dragging headers into and out of groups. ([test_groups_engine.py](../tests/test_groups_engine.py),
[test_groups_model.py](../tests/test_groups_model.py), [test_ui_groups.py](../tests/test_ui_groups.py))

### Sends and returns

Sends at their levels before and after the fader, a return's effect on the sum of what is sent to it, returns into
returns, mute and solo across sends, send automation in time, cycles refused across outputs and sends, and delay
compensation per edge (latent returns, latent tracks sending, one track into two returns of different latency, a
pre-fader tap after a latent device): clicks line up at the master. In the model: making and deleting returns (with
their sends, one undo step), saving, and the engine hearing it all; in the window: Ctrl+Alt+T, the send knobs,
pre-fader, greyed-out cycles and send automation. ([test_sends_engine.py](../tests/test_sends_engine.py),
[test_sends_model.py](../tests/test_sends_model.py), [test_ui_sends.py](../tests/test_ui_sends.py))

### Sidechains

Sidechain tests key SUB Test Sidechain (a test plug-in whose output is its input plus its sidechain) with a track's
click: it hears the source sample for sample, after its fader, before it, before its devices or after one of them,
with latent plug-ins before and after the tap and on the device's track before and after the device (the sidechain or
the track's own signal waits for the other), and taps before a device that waits for its own sidechain; into groups'
and the master's devices too, and live with workers. A device after one that waits for its sidechain keeps its
automation in time. Others check cycles refused (through outputs, sends, inputs and moving the device), the source
going, mute and solo, and that a missing sidechain reaches the plug-in flagged as silence. In the model: undo, sources
greyed out or refused, sidechains dropped by deleting, ungrouping, regrouping and moving devices (one undo step),
saving, and the engine taking it all; in the window: the sidechain button and its menu.
([test_sidechain_engine.py](../tests/test_sidechain_engine.py), [test_sidechain_model.py](../tests/test_sidechain_model.py),
[test_ui_sidechain.py](../tests/test_ui_sidechain.py))

### Racks

Rack tests: chains summing, their faders, mute and solo (also metered live), a rack switched off or empty passing its
input on; a latent device in one chain (and racks in racks) not smearing the others, with tracks beside lined up;
automation of a nested device and of a chain's fader in time behind latent devices; an instrument rack of two synths
rendering as two tracks of one each; sidechains into devices in racks lining up sample for sample; devices and racks
moving in and out of racks and between tracks (keeping their state), removals, cycles and nesting refused; and renders
with racks bit-identical with and without workers. In the model: grouping and ungrouping, chains and their mixers,
moves, nesting limits, instrument racks, macros (one undo step, mappings following their devices), sidechains in racks
as routing edges, saving and presets (fresh ids, loading twice); through the bridge: the engine keeping each device's
processor as racks are made, undone, moved and deleted, chain automation and overrides, macros moving plug-in
parameters, and a saved rack rendering as the original; in the window: Ctrl+G and Ctrl+Shift+G, chains shown and
dropped into, chain mixers and macros. ([test_racks_engine.py](../tests/test_racks_engine.py),
[test_racks_model.py](../tests/test_racks_model.py), [test_ui_racks.py](../tests/test_ui_racks.py))

### Presets

The preset library: presets saved by device and listed by group then name, names as file names, renaming. Loading
a preset into a device of its kind (a built-in device, a plug-in, a rack) as one undo step, and refusing other kinds.
Through the bridge: a plug-in preset renders as the device it was saved from, as a new device and loaded into another
(undone and redone); a built-in one reaches the engine; a preset of a missing plug-in, or of a built-in device this
version doesn't have, loads as a missing device, and the rest of a rack around it works. In the window: every
device's save button, the browser's Presets section (searched in *All*), drops between devices and onto devices of
the preset's kind or another, double-clicking (an instrument preset making a MIDI track), drops on a track, renaming
and deleting. Default presets: new devices (and new MIDI tracks' instruments) starting
as them, per plug-in, unreadable ones ignored, a plug-in's state reaching the engine; and racks named as their
presets (saved, loaded, loaded into, in projects, older files without names). The tests use their own library (`SUBSTATION_PRESETS`, set in `conftest.py`).
([test_presets.py](../tests/test_presets.py), [test_ui_presets.py](../tests/test_ui_presets.py))

### Parallel rendering

Parallel tests render with and without the audio thread's workers: random routing graphs (nested groups, returns and
sends before and after the fader, tracks taking their input from others, sidechains, latent plug-ins, solo, mute,
automation, looping), many tracks over many runs, and stateful synths and plug-ins moving between threads all render
the same bit for bit; the MIDI input, recording and resampling tests run again live with workers (held, preview and
recorded notes). Tracks are timed, and the tracks on the longest paths start first (ranks checked on a hand-made
graph). `python -m benchmarks.parallel_render_bench` compares render times on 1..N threads, and with
`--heavy N --compare-ordering` the gain from starting heavy tracks first.
([test_parallel_engine.py](../tests/test_parallel_engine.py), [test_parallel_live.py](../tests/test_parallel_live.py))

### Warping

Warp tests check that warped clips land on their beats at any tempo, keep their pitch, start sample-aligned (also
after a locate), transpose to the right frequency, and that Re-Pitch filters rather than aliases.
([test_warp.py](../tests/test_warp.py))

### MIDI in the engine

MIDI engine tests check that notes start on their sample and follow the tempo, that the Synth plays the right pitch
and level, and that loop wraps and offline renders leave no hanging notes. ([test_midi_engine.py](../tests/test_midi_engine.py))

### Model

Model tests cover overlap resolution, trims, splits (audio and MIDI), note editing, undo/redo and save/load; and
automation: envelope maths (curves, exact splits, range delete and duplicate), the parameter mappings against the
engine's, undoable edits, what happens to a deleted device's automation, the lanes shown, saving, and automation
moving (or copied) with clips unless locked. Others cover the timebase (time signatures, seconds and beats, position
formatting and parsing), keys and tempos from file names, duplicating tracks and copying automation ranges.
([test_edits.py](../tests/test_edits.py), [test_midi_model.py](../tests/test_midi_model.py),
[test_automation_model.py](../tests/test_automation_model.py), [test_serialization.py](../tests/test_serialization.py),
[test_timebase.py](../tests/test_timebase.py), [test_keys.py](../tests/test_keys.py),
[test_duplicate_tracks_copy_automation.py](../tests/test_duplicate_tracks_copy_automation.py))

### Browser

Browser tests hold the native backend to the Python code it replaced (`tests/browser_reference.py`, kept as it was):
the same files from a folder tree (hidden names, depth and file limits, junctions and symbolic links, overlapping and
missing places) in the same order, and the same results for random queries, sorts, filters and use counts, item for
item. Python's own text rules (`str.lower`, `casefold`, `split`, the regex word starts) are checked for every Unicode
character. Others check the saved index (checked by folder times, rescan, damaged files), changes seen while running,
that only the latest search's results are handed out, paging, and that waiting releases the GIL. Use counts, their
decay and the `library.json` file have their own tests, and the panel is driven in the window (searching, Ctrl+F,
Enter and Down, keeping its place when files change, drops). See [browser.md](browser.md#tests).
([test_browser_native.py](../tests/test_browser_native.py), [test_browser_search.py](../tests/test_browser_search.py),
[test_ui_smoke.py](../tests/test_ui_smoke.py))

### The UI

The UI tests drive the real main window offscreen: mouse drags, drops, header controls, dialogs, the piano roll, and
devices' own editors (the Compressor's graph; the Sampler's loading, undo, playhead, markers, drop and saving); and
automation lanes: A, parameters showing their lanes, clicking on the line, dragging and bending breakpoints, deleting,
time ranges, overriding and re-enabling, controls following automation, the master's lane, lanes below tracks, and
saving. Device folding and cut, copy and paste of devices are tested in the model and the device view.
([test_ui_smoke.py](../tests/test_ui_smoke.py), [test_ui_midi.py](../tests/test_ui_midi.py),
[test_ui_device_editors.py](../tests/test_ui_device_editors.py), [test_ui_automation.py](../tests/test_ui_automation.py),
[test_ui_device_view.py](../tests/test_ui_device_view.py))

### VST3

VST3 tests use the test plug-ins built with the engine: an instrument with a separate controller (it reports the
transport it gets back as parameters), a single-component effect with adjustable latency and a Win32 editor, a mono
effect without a controller, and the sidechain effect. They check scanning (including a plug-in that crashes or hangs
while loading), sample-exact notes, parameters, the transport and loop splitting, latency compensation, mono buses,
state and presets, editor windows (resizing, closing, edits reported for undo), and the device view, browser, undo
and projects in the application. Editor tests briefly show real windows. The tests see only these plug-ins, never the
installed ones. ([test_vst3_engine.py](../tests/test_vst3_engine.py), [test_plugin_scanner.py](../tests/test_plugin_scanner.py),
[test_ui_plugins.py](../tests/test_ui_plugins.py))

## Every test file

| File | Tests | What it covers |
|---|---|---|
| [test_asio.py](../tests/test_asio.py) | 19 | ASIO with the fake driver: finding and opening drivers, rates and buffer sizes, channels, every sample format, inputs, the driver's requests (resets, new latencies, another clock), its control panel, errors, and the preferences in the application. |
| [test_automation_engine.py](../tests/test_automation_engine.py) | 13 | Envelopes of mixer controls (tracks and master) and of device parameters, rendered offline. |
| [test_automation_model.py](../tests/test_automation_model.py) | 27 | Envelope maths, parameter mappings against the engine's, undoable edits, view state and saving. |
| [test_browser_native.py](../tests/test_browser_native.py) | 19 | The native browser backend against `browser_reference.py`: text rules, indexing, ordering, the saved index, watching, latest-search hand-over, paging, the GIL. |
| [test_browser_search.py](../tests/test_browser_search.py) | 6 | Item keys, use counts (decay, persistence, unknown fields, bad files), match quality, rank ordering. |
| [test_compressor_engine.py](../tests/test_compressor_engine.py) | 5 | The built-in Compressor: its gain curve, its displays, and keying from a sidechain. |
| [test_delay_engine.py](../tests/test_delay_engine.py) | 12 | The built-in Delay: synced and free times, offset, link, feedback, ping pong, freeze, the filter, the modes and its display. |
| [test_eq_engine.py](../tests/test_eq_engine.py) | 18 | The built-in EQ: each band type plays as `eq_response` draws it, the curves against the analog filters, placement (left, mid, side), output gain and gain scale, extremes, the displays. |
| [test_sidechain_device_engine.py](../tests/test_sidechain_device_engine.py) | 13 | The built-in Sidechain: hits to the sample, the curve sample by sample (straight and bent), depth, smoothing, threshold and re-arming, lookahead as latency, Lows Only, hits on the beat, the synced length, the displays, extremes. |
| [test_sidechain_fit.py](../tests/test_sidechain_fit.py) | 7 | Fitting the Sidechain to a kick: the clash band (with and without a bass), the reduction, curves as automation bends them, fitting points, the characters, capturing hits from the displays. |
| [test_computer_keyboard.py](../tests/test_computer_keyboard.py) | 4 | The computer MIDI keyboard: M, the letter keys, Z and X octaves, and those keys not reaching the window's shortcuts or text fields while it is on. |
| [test_duplicate_tracks_copy_automation.py](../tests/test_duplicate_tracks_copy_automation.py) | 5 | Duplicating tracks (clips, devices, automation and routing following the copies, one undo step) and cutting, copying and pasting automation ranges. |
| [test_edits.py](../tests/test_edits.py) | 29 | Clip edits: overlaps, trims, splits, moves, duplicates, deletes, undo; tempo and warping effects on clip lengths; track input and monitoring undo; recorded takes becoming clips; copy and paste of time ranges; automation copied with clips unless locked. |
| [test_engine_render.py](../tests/test_engine_render.py) | 25 | Offline renders: clip placement, tempo, offsets, gain, pan, mute and solo, looping, the metronome, fades, Utility and Over The Top, sources and peaks, resampling to the engine rate, export, chains and the master's devices. |
| [test_freeze_engine.py](../tests/test_freeze_engine.py) | 15 | Freezing in the engine: a track's signal before its fader (lined up, solo ignored, a group's bus, its tail into a 32-bit WAV), frozen tracks through their faders without their devices or notes, frozen groups and what goes only into them, sends tapping frozen audio, no latency from frozen tracks. |
| [test_freeze_model.py](../tests/test_freeze_model.py) | 14 | Freezing in the model: what can be frozen, undo, what frozen tracks and frozen groups refuse, flattening, saving; through the bridge, frozen audio playing with the devices unloaded and coming back (a plug-in's state too). |
| [test_groups_engine.py](../tests/test_groups_engine.py) | 8 | Group buses in the engine, delay compensation at every summing point, solo and mute across levels. |
| [test_groups_model.py](../tests/test_groups_model.py) | 9 | Group tracks in the model: grouping, ungrouping, moving, folding, saving, and the engine hearing a group as a bus. |
| [test_keys.py](../tests/test_keys.py) | 6 | Tempo and key from file names, transposing by the shortest way (relative keys alike), key names, the project key (undoable, saved). |
| [test_midi_engine.py](../tests/test_midi_engine.py) | 12 | Note scheduling and the built-in Synth, offline. |
| [test_midi_input.py](../tests/test_midi_input.py) | 10 | MIDI input with the fake driver: live notes at their offsets, routing by input and channel, monitoring, releases, recorded notes on their beats. |
| [test_midi_model.py](../tests/test_midi_model.py) | 22 | MIDI clips as windows onto notes, note editing, editor rules and files. |
| [test_parallel_engine.py](../tests/test_parallel_engine.py) | 14 | Rendering on several threads, bit-identical to one thread on random routing graphs; cost ordering. |
| [test_parallel_live.py](../tests/test_parallel_live.py) | 4 | The MIDI input, recording and resampling tests again, live with workers, through the fake driver. |
| [test_plugin_scanner.py](../tests/test_plugin_scanner.py) | 5 | Scanning in child processes (a crashing or hanging plug-in costs only itself), the cache, finding plug-in files, friendly messages. |
| [test_presets.py](../tests/test_presets.py) | 14 | The preset library, loading presets into devices (one undo step), and through the bridge: plug-in presets rendering as saved, missing plug-ins and devices. |
| [test_racks_engine.py](../tests/test_racks_engine.py) | 13 | Racks in the engine: chains, faders, mute and solo, delay compensation, automation, instrument racks, sidechains, moves, nesting, meters, workers. |
| [test_racks_model.py](../tests/test_racks_model.py) | 17 | Racks in the model: grouping, chains, moves, nesting, macros, automation, sidechains, undo, saving, presets, and the engine through the bridge. |
| [test_recording.py](../tests/test_recording.py) | 8 | Audio input and recording in the engine: inputs, monitoring, takes and their placement (loopback), count-in, what ends a recording. |
| [test_resampling_engine.py](../tests/test_resampling_engine.py) | 8 | Takes of a track's, group's, return's or the master's output equal to its render; monitoring; cycles. |
| [test_resampling_model.py](../tests/test_resampling_model.py) | 6 | Track inputs from other tracks in the model: cycles, inputs dropped by regrouping and deleting, saving, the bridge. |
| [test_sampler_engine.py](../tests/test_sampler_engine.py) | 14 | The Sampler: its sample state, playing it across the keyboard, swapping it while it plays. |
| [test_sends_engine.py](../tests/test_sends_engine.py) | 11 | Sends in the engine: pre and post fader, delay compensation per edge, cycles, solo and mute. |
| [test_sends_model.py](../tests/test_sends_model.py) | 9 | Returns and sends in the model: making and deleting returns, levels and taps, cycles, saving, the bridge. |
| [test_serialization.py](../tests/test_serialization.py) | 7 | `.gilproj` round trips, relative paths when the folder moves, foreign files rejected, old warp mode names and old projects, the master's devices, inputs and monitoring. |
| [test_sidechain_engine.py](../tests/test_sidechain_engine.py) | 13 | Sidechains in the engine, lined up sample for sample at every tap and latency; cycles, the source going, mute and solo. |
| [test_sidechain_model.py](../tests/test_sidechain_model.py) | 8 | Sidechains in the model: taps, cycles, sidechains dropped by moves and deletes (one undo step), saving, the bridge. |
| [test_timebase.py](../tests/test_timebase.py) | 6 | Time signatures, seconds and beats, formatting and parsing positions, bar labels, value formatting. |
| [test_ui_automation.py](../tests/test_ui_automation.py) | 23 | Automation in the window: showing lanes, editing envelopes with the mouse, what the engine plays. |
| [test_ui_device_editors.py](../tests/test_ui_device_editors.py) | 7 | Built-in devices' own editors in the device view (the Compressor's, the Sampler's, the Delay's, the EQ's: adding bands from the curve, dragging, the wheel, the panel, the analyzer, the window; the Sidechain's: drawing and bending the curve, the controls, hits from real renders, Fit and Auto). |
| [test_ui_device_view.py](../tests/test_ui_device_view.py) | 6 | Folding devices; cutting, copying and pasting them in the model and the device view. |
| [test_ui_freeze.py](../tests/test_ui_freeze.py) | 5 | Ctrl+Shift+F freezing and unfreezing, the device view of frozen tracks and groups, the track menu's actions, flattening a MIDI track. |
| [test_ui_groups.py](../tests/test_ui_groups.py) | 7 | Group tracks in the window: Ctrl+G, folding, headers and the summary lane, dragging into and out of groups. |
| [test_ui_midi.py](../tests/test_ui_midi.py) | 10 | MIDI tracks, clips and the piano roll in the window. |
| [test_ui_plugins.py](../tests/test_ui_plugins.py) | 26 | VST3 plug-ins in the application: the browser, the device view, editors, undo and projects. |
| [test_ui_presets.py](../tests/test_ui_presets.py) | 8 | Presets in the window: the save button, the browser's Presets section, dropping, double-clicking, renaming and deleting. |
| [test_ui_racks.py](../tests/test_ui_racks.py) | 4 | Racks in the device view: Ctrl+G and Ctrl+Shift+G, chains, dropping devices, chain mixers and macros. |
| [test_ui_recording.py](../tests/test_ui_recording.py) | 9 | Recording in the window: arming, inputs, monitoring, the record button, live waveforms, takes as clips; MIDI tracks and the MIDI preferences. |
| [test_ui_resampling.py](../tests/test_ui_resampling.py) | 3 | The input menu (Resampling and other tracks, cycles greyed out) and takes recorded from a track and the master. |
| [test_ui_sends.py](../tests/test_ui_sends.py) | 6 | Ctrl+Alt+T, return rows, send knobs, pre/post-fader taps, send automation, deleting a return. |
| [test_ui_sidechain.py](../tests/test_ui_sidechain.py) | 2 | The sidechain button and its menu (sources, taps, cycles greyed out). |
| [test_ui_smoke.py](../tests/test_ui_smoke.py) | 37 | The main window driven like a user: arrangement editing with the mouse, transport, zoom and scroll, devices and mixer, save and open, the browser, the clip view, track resizing and folding, dialogs, the audio threads preference, shortcuts from a plug-in editor, recent files. |
| [test_vst3_engine.py](../tests/test_vst3_engine.py) | 20 | VST3 hosting in the engine: instruments and effects, parameters, transport, latency, state, editors. |
| [test_warp.py](../tests/test_warp.py) | 16 | Warped clips on their beats, pitch shifting, Re-Pitch. |

[tests/browser_reference.py](../tests/browser_reference.py) is not a test file: it is the browser's former Python index
and search, kept unchanged as the reference the native backend must agree with and as the benchmarks' baseline.

## Writing tests

- Prefer offline renders (`Engine` with no device) for anything the engine computes: they are exact and need no
  hardware. Use `ramp_wav` or `dc_wav` to make positions and levels visible in the output.
- For anything that needs the audio callback (inputs, recording, MIDI input, live behaviour), open the fake driver in
  manual mode and step it with `process(n)`; decode its outputs with numpy.
- For plug-in behaviour, use the test plug-ins, and guard the file with
  `pytest.mark.skipif(not TEST_PLUGINS.exists(), ...)`; for ASIO, skip when `"ASIO" not in driver_types()`.
- UI tests take the `window` fixture; an exception in a Qt slot fails the test.
- A new test plug-in goes into `tests/vst3_plugins` and `sub_test_plugins` in [CMakeLists.txt](../CMakeLists.txt);
  a new driver hook into `test_asio_driver.cpp` and its `.def`.

## Benchmarks

[benchmarks/](../benchmarks) holds the browser's and the renderer's benchmarks and their results. They are not run by
pytest; run them as modules from the project folder, with the venv active. Full results and how they were measured
are in [benchmarks/README.md](../benchmarks/README.md); raw reports are in `benchmarks/results/`.

| Benchmark | What it measures |
|---|---|
| [browser_backend_bench.py](../benchmarks/browser_backend_bench.py) | The browser backend alone: the Python index and search from before (`tests/browser_reference.py`) against the native one, on a large synthetic library. Checks that every query returns the same items in the same order, and fails if not. |
| [browser_ui_bench.py](../benchmarks/browser_ui_bench.py) | The real `BrowserPanel`: time until results are laid out and painted, and the longest time the UI thread couldn't run (a 1 ms timer's gaps), at start, restart, per query, while typing and during a rescan. `--code DIR` runs it against another copy of the `substation` package, for example the one before the native backend. |
| [parallel_render_bench.py](../benchmarks/parallel_render_bench.py) | Render times on 1..N threads (offline, best of three, checked bit-identical), and live through the fake ASIO driver in manual mode (`--live 64,256`). `--heavy N --compare-ordering` measures the gain from starting heavy tracks first. |
| [library_gen.py](../benchmarks/library_gen.py) | Makes the synthetic sample libraries (packs, categories and sub-folders of 20-150 empty files with realistic names, mostly WAV with some FLAC and MP3, plus `.asd` files and artwork) once per size and seed, in `%TEMP%\sub-browser-bench`. |

```powershell
python -m benchmarks.browser_backend_bench --size 200000 [--audio] [--json out.json]
python -m benchmarks.browser_ui_bench --size 200000 [--audio] [--json out.json] [--code DIR]
python -m benchmarks.parallel_render_bench [--tracks 32] [--device synth|ott|plugin] [--live 64,256] [--json out.json]
```

`--audio` plays an arrangement through the default output the whole time, silently (master gain 0), and reports the
engine's CPU load and how far its playhead fell behind the wall clock at worst. The browser benchmarks never touch the
user's settings, use counts, index or plug-ins. The live parallel benchmark needs the engine built with the ASIO SDK.
