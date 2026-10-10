# Testing

The tests are run by CTest. There are four kinds: the engine's tests, one Qt-free executable
([tests/engine](../tests/engine)); the intelligence module's, another ([tests/intelligence](../tests/intelligence));
the application layer's and the UI's tests, Qt Test, one executable per file ([tests/app](../tests/app)); and the check
of the layers' boundaries. They need no sound card and no installed
plug-ins: the engine renders offline, small VST3 plug-ins (and, with the ASIO SDK, a fake ASIO driver) are built
with the tests, and the UI tests drive the real QML views. Benchmarks live apart, in [benchmarks/](../benchmarks),
and are not run by CTest.

## Running the tests

Build first (`ninja -C build`: see [building.md](building.md)); then, from the project folder:

```sh
ctest --test-dir build --output-on-failure     # everything
ctest --test-dir build -j8                     # several test programs at once
ctest --test-dir build -R editor               # the tests whose names match a regular expression (test_editor_*)
ctest --test-dir build -R boundaries           # the layers' boundaries only
ctest --test-dir build -N                      # list them without running
```

CTest knows each test program as one test: `boundaries`, `engine` (all of `engine_tests`), `intelligence` (all of
`intelligence_tests`) and `platform` (all of `platform_tests`, the platform layer's: [platform.md](platform.md)), the
last two running and filtering as `engine_tests` does, with the same harness, and one per
`tests/app/test_*.cpp`, by its name (`test_editor_racks`, `test_ui_arrangement`...). Each Qt Test program keeps its
settings in a folder of its own (see [tests/app/support](#testsappsupport)), so `-j` is safe.

### The engine's tests

`engine_tests` runs every engine test, in file order, and prints `ok`, `FAIL` or `SKIP` (with the reason) for
each, then the counts; it exits with 1 if any failed. Its arguments filter by name:

```sh
build/bin/engine_tests                   # all
build/bin/engine_tests sidechain warp    # those whose names contain "sidechain" or "warp"
build/bin/engine_tests --list            # "file: name" of each, without running them
```

### A Qt Test program

Each `test_*` program takes Qt Test's options:

```sh
build/bin/test_editor_racks                       # all its test functions
build/bin/test_editor_racks -functions            # list them
build/bin/test_editor_racks ungroupRack           # one (function:tag for one row of a data-driven test)
build/bin/test_editor_racks -v2                   # every check as it runs
```

CTest runs the application layer's programs with `QT_QPA_PLATFORM=offscreen`; set it when running one by hand too
(only `test_session_keyboard` and the UI's tests make a `QGuiApplication`, but none should show windows on your
desktop).

### The UI's tests and xvfb

Programs named `test_ui_*` link the UI too, and need a real display: Qt's `offscreen` platform renders Qt Quick with
its software renderer, which draws none of the scene-graph geometry the UI's items make (`SgPainter`'s
vertex-coloured nodes: lanes, clips, envelopes, the piano roll, meters, knobs). Their windows would be blank and
their pixel checks meaningless. So on Linux CTest runs them under `xvfb-run -a -s "-screen 0 1920x1080x24"` with
`QT_QPA_PLATFORM=xcb` (Mesa renders OpenGL in software), if `xvfb-run` was found when the build was configured. On
Windows they run on the desktop. By hand:

```sh
xvfb-run -a -s "-screen 0 1920x1080x24" env QT_QPA_PLATFORM=xcb build/bin/test_ui_arrangement
```

Without `xvfb-run` they run on whatever display the environment has; on the `offscreen` platform the tests that draw
skip ("needs a display": `haveDisplay()` in [UiTestSupport.h](../tests/app/support/UiTestSupport.h)).

Set `SUBSTATION_UI_SCREENSHOTS` to a folder to have the UI's tests save screenshots there as PNGs (the gallery, every
device editor, the windows they drive), to look at.

### What gets skipped

| Condition | Skipped |
|---|---|
| No fake ASIO driver: not Windows, the engine built without the ASIO SDK, or the test plug-ins off (`SUBSTATION_TEST_PLUGINS=OFF`; the driver is built with them) | the engine's tests that play through it ("built without the ASIO SDK"): `test_asio`, `test_recording`, `test_midi_input` (but for the device list), `test_resampling_engine` (but for the cycles), `test_parallel_live`, and `test_racks_engine`'s chain meters |
| Test plug-ins not built (`SUBSTATION_TEST_PLUGINS=OFF`) | the tests that use them ("test plug-ins not built"): VST3, sidechains, racks, sends, groups, freezing and parallel renders in the engine; the bridge's, the session's and the device view's plug-in tests; the scanner's tests with the real bundle |
| No audio device opens (common on Linux) | `test_midi_engine`'s live test, `test_bridge_recording`'s takes, `test_ui_pianoroll`'s playing; `test_session_files` has a test that skips when one *does* run (the device's own buffer sizes are listed then) |
| Not Windows | plug-in editors (`test_vst3_engine`'s editor test, `test_bridge_plugins`'s editor parts): plug-ins show no editor elsewhere. On Windows, where editors open, one `test_bridge_plugins` test skips its part about editors following the selected track |
| No display (`offscreen` or `minimal` platform) | the UI's tests that draw |
| Places can't be watched (not Windows or Linux) | the browser's tests of changes seen while running |
| The browser backend's Unicode tables are not Unicode 15.0.0's | `test_browser_native`'s every-character text tests (their reference values are Unicode 15.0.0's) |
| No trash for the temporary folder | `test_ui_browser`'s deleting a preset |
| `SUBSTATION_UI_SCREENSHOTS` not set | `test_ui_device_editors`'s and `test_ui_device_editors_gate`'s screenshots (the other editors' tests take theirs anyway, and save nothing) |

## Layout

```
tests/
  CMakeLists.txt        the boundary check, engine_tests, intelligence_tests, platform_tests, and a program per tests/app/test_*.cpp
  TestPlugins.cmake     the test VST3 bundle and, with the ASIO SDK, the fake ASIO driver
  engine/
    harness/            the engine tests' harness (no Qt)
    test_*.cpp          the engine's tests: engine_tests
  intelligence/
    Sounds.h            drum hits, tones and loops made from formulas, written as WAV files
    test_*.cpp          the intelligence module's tests: intelligence_tests (the engine tests' harness)
  platform/
    test_*.cpp          the platform layer's tests: platform_tests (the engine tests' harness)
  app/
    support/            what the application layer's and the UI's tests share (sub_app_test_support)
    test_*.cpp          a Qt Test program each; test_ui_* link the UI too
  vst3_plugins/         the test VST3 plug-ins (C++)
  asio_driver/          the fake ASIO driver (C++; Windows, with the ASIO SDK)
```

Sources are globbed: a new `tests/engine/test_*.cpp` joins `engine_tests`, a new `tests/intelligence/test_*.cpp`
joins `intelligence_tests`, a new `tests/platform/test_*.cpp` joins `platform_tests`, and a new `tests/app/test_*.cpp` is a new program, on the next configure (`ninja` checks
the globs).

### tests/engine/harness

The engine's tests link `sub_engine` and nothing else, which also shows that the engine stands on its own.

| File | What it holds |
|---|---|
| [Test.h](../tests/engine/harness/Test.h), [Main.cpp](../tests/engine/harness/Main.cpp) | `TEST_CASE("name") { ... }`; `CHECK` (records a failure, goes on), `REQUIRE` (ends the test), `CHECK_EQ`, `CHECK_NEAR` (absolute), `CHECK_APPROX` (relative 1e-6), `CHECK_APPROX_REL`, `CHECK_APPROX_TOL`, `CHECK_THROWS_AS`, `CHECK_THROWS_MATCHING` (the message searched with a regular expression), `CHECK_NOTHROW`; `INFO(text)` adds what a failure in its scope was about (a seed, a parameter); `SKIP(reason)`; `tempDir()`, a fresh folder for the running test, removed after it. `Main.cpp` runs them (filters, `--list`). |
| [Fixtures.h](../tests/engine/harness/Fixtures.h) | `kSampleRate` (48000: the engine's rate without a device), `kSpb`, `kBeat`; WAV files to play (`writeWav`, `makeWav`, `dcWav`: a second of 0.5 in both channels; `rampWav`: sample i is (i % 32768) / 32768, exact in 16-bit PCM; `clickWav`: one sample at a level in silence); the test plug-ins (`testPluginsBundle`, `requireTestPlugins`, `testPluginUids`, `addTestPlugin`; SUB Test Effect's parameters `FX_GAIN`, `FX_LATENCY`, `FX_BYPASS`, and `latentEffect`: it on a track, so many samples late; `keyed`: SUB Test Sidechain on a track); `clip`, `clipTrack`, `stereoClickTrack`, `setParam`, `paramInfo`, `utilityOn`, `utility` (on a track), `builtinInfo`. |
| [Signal.h](../tests/engine/harness/Signal.h) | What renders are checked with: channels and ranges of interleaved audio, peak and RMS levels, where a signal is non-zero, comparisons sample by sample, spectra of any length, envelopes, correlation, random numbers, reading WAV files back. |
| [AsioDriver.h](../tests/engine/harness/AsioDriver.h) | The fake ASIO driver: `AsioDriver` (its hooks, below; making one skips the test where there is no driver), the sample types and `decode()` (a driver buffer's bytes as samples, decoded independently of the engine), `asioConfig`, `openAsio`, `haveTestAsio`, `requireTestAsio`. |
| [Recording.h](../tests/engine/harness/Recording.h) | `RecordingTest`: the driver in manual mode with float samples (a loopback carries them exactly) and an engine without clip fades, which must have let go of the driver when it closes; with `onWorkers`, four render threads and seven silent tracks beside the test's, so every buffer's tracks are shared out. `output()`, `readTake()`. |
| [MidiInputTest.h](../tests/engine/harness/MidiInputTest.h) | SUB Test Synth in its DC mode (`dcSynth`), MIDI messages sent to play at a known offset into the next buffer (`send`), what the next buffers play (`heard`), what held notes should play (`held`). |
| [LiveTests.h](../tests/engine/harness/LiveTests.h) | The live tests (recording, MIDI input, resampling) that `test_parallel_live.cpp` runs again with workers; each is defined beside its own `TEST_CASE`. |
| [TaskGraphOrder.h](../tests/engine/harness/TaskGraphOrder.h) | `taskGraphOrder()`: the scheduler's queue order and ranks for a graph given by hand. |
| [Standalone.h](../tests/engine/harness/Standalone.h) | `Standalone`: a built-in device on its own, outside an engine, at any rate, run as the renderer runs it (`Standalone(kind, rate, values)`, `set`, `index`, `context()`): `run` (any channels) and `play` (a mono signal) in blocks, its parameters' changes (`ParamChange{frame, id, value, direct}`) handed over as automation, so its blocks split there to the sample, or set between blocks for one that isn't automatable. The built-in effects' own tests build on it. |

### tests/app/support

The application layer's tests link `sub_app` and `sub_app_test_support` (the `.cpp` files here, a static library);
the UI's tests (`test_ui_*`) link `sub_ui` and Qt Quick Test too, and use the header-only helpers, since the library
doesn't link Qt Quick.

| File | What it holds |
|---|---|
| [TestSupport.h](../tests/app/support/TestSupport.h) | `prepareApplication()`, called first (in `initTestCase`): the organization and application names are "SUBstation Tests", so tests never touch the user's settings; each program keeps its settings (INI) in a temporary folder of its own, cleared at start, so programs running side by side don't clear or change each other's; and the folders the application keeps things in are temporary ones for the run (`SUBSTATION_PRESETS`, `SUBSTATION_LIBRARY`, `SUBSTATION_BROWSER_INDEX`, `SUBSTATION_SOUND_INDEX`, `SUBSTATION_PLUGIN_CACHE`, `SUBSTATION_RECORDINGS`, `SUBSTATION_TEMPLATE`). `ScopedEnv` (an environment variable set, or unset, for as long as it lives, put back even when a check fails), `TempDir`, `writeWav` (16-bit PCM, rounded half to even), `makeTrack`, `makeDevice`, `kSampleRate`; `ids` and `kinds` (of devices or tracks, in order), `round6` and `spans` (where clips start and end, in beats). |
| [EditorFixture.h](../tests/app/support/EditorFixture.h) | `EditorFixture`: a project, its undo stack and a `ProjectEditor` on them, with what the editor refused collected (`messages`); `env()` (an envelope from points), `audioClip()`. |
| [BridgeTestSupport.h](../tests/app/support/BridgeTestSupport.h) | `Studio`: a project, its undo stack, an engine without a device and the bridge between them, shut down when it goes; `Edits`: the editor's edits made as the editor makes them (the model's commands, in the same order); the test plug-ins as `PluginRef`s and `PluginInfo`s; `BridgeTestAccess`, the bridge's friend: plug-in reports injected as if the plug-ins had sent them, the plug-in loading timer stopped and stepped by hand, the meters polled, the bridge made busy. |
| [SessionFixture.h](../tests/app/support/SessionFixture.h) | `SessionFixture`: an engine without a device (clips without fades, unless asked for) and a `Session` on it that scans no plug-ins, keeps no browser index and analyses no sounds in the background (its browser lists the user's Music folder); what it says (`messages`, `warnings`, `informations`) collected; `waitForRender`, `waitForSource`, `render` (offline), `level`, `clipTrack`. |
| [UiTestSupport.h](../tests/app/support/UiTestSupport.h) | `UiSession`: a session on a fresh engine, registered as the QML `Session` singleton, and a QML engine set up for the UI's module (analysing the browser's sounds only if asked: `UiSession(true)`, for a test with places of its own); `show()` loads a window of QML and waits until it is exposed and active. Mouse, wheel and key input as a user sends it (`press`, `moveTo`, `release`, `click`, `doubleClick`, `drag`, `wheel`), `haveDisplay()`, `screenshot()`. |
| [ArrangementTestSupport.h](../tests/app/support/ArrangementTestSupport.h) | The arrangement view in a window on a `UiSession`; its items found by name, points in its lanes and headers, the menus its items work out, audio files to put in it. |
| [DevicePanelTestSupport.h](../tests/app/support/DevicePanelTestSupport.h) | The device panel in a window as the main window places it; its parts found by object name or by device; its menus read and chosen from; drags from the browser (or along the chain) delivered as the platform delivers them. |
| [EditorHarness.h](../tests/app/support/EditorHarness.h) | `EditorHarness`, the host of the built-in devices' editors' tests (`test_ui_device_editors*.cpp`): a session over a real engine (with no audio device), and a window showing one editor at a time as the device view shows it (`show()`: the component for the device's kind, the body's size, the frame's colours). Its parts found by object name (`find`), drags with modifiers and the wheel (`dragTo`, `wheel`), the displays' clock ticked by hand (`refreshDisplays`), a track playing a signal (`audioTrackWith`, `tone`, `stereo`), `haveDisplay()`, `save()` for screenshots. |
| [BrowserReference.h](../tests/app/support/BrowserReference.h) | A plain, slower implementation of the browser's index and search, kept as the reference the native backend must agree with, item for item and in the same order (`test_browser_native`), and what `browser_backend_bench` checks every query against. |

## Environment variables

| Variable | What it does | In the tests |
|---|---|---|
| `SUBSTATION_PRESETS` | The preset library's folder instead of `Documents/SUBstation/Presets` (`libraryDir()`, [io/Presets.h](../app/src/io/Presets.h)) | a temporary folder (`prepareApplication`) |
| `SUBSTATION_LIBRARY` | The browser's use counts (`library.json`) instead of the one in the local data folder ([browser/Library.h](../app/src/browser/Library.h)) | a temporary file |
| `SUBSTATION_BROWSER_INDEX` | The browser's saved index (`browser-index.bin`) ([browser/FileIndex.h](../app/src/browser/FileIndex.h)) | a temporary file |
| `SUBSTATION_SOUND_INDEX` | The sounds' saved fingerprints (`sound-index.bin`) ([intelligence/SoundSimilarity.h](../app/src/intelligence/SoundSimilarity.h)) | a temporary file |
| `SUBSTATION_PLUGIN_CACHE` | The plug-in scan's cache (`vst3-cache.json`) (`pluginCachePath()`, [plugins/PluginPaths.h](../app/src/plugins/PluginPaths.h)) | a temporary file |
| `SUBSTATION_VST3_PATH` | The folders searched for plug-ins instead of the standard VST3 folders, separated by the system's list separator (`;` on Windows, `:` elsewhere); empty: none | the scanner's, the browser's and the preferences' tests point it at folders of their own, so no test sees the installed plug-ins |
| `SUBSTATION_RECORDINGS` | Where takes go for a project never saved, instead of `Music/SUBstation/Recordings` (`recordingsFolder()`, [audio/AudioFiles.h](../app/src/audio/AudioFiles.h)); frozen and reversed audio go into folders in it too | a temporary folder |
| `SUBSTATION_TEMPLATE` | The project template New Project opens (`Template.gilproj` in the local data folder otherwise: `Session::templatePath()`, [session/Session.h](../app/src/session/Session.h)) | a temporary file, not there until a test saves a template |
| `SUBSTATION_ASIO_DRIVERS` | ASIO drivers to use instead of the installed ones, as `Name\|{CLSID}\|C:\path\driver.dll`, several separated by `;` (read by `listDrivers()` in [engine/src/backends/AsioBackend.cpp](../engine/src/backends/AsioBackend.cpp)) | the fake driver (set by `AsioDriver`) |
| `SUBSTATION_SCANNER` | The scanner program instead of `substation-scan` beside the application (`PluginScanner::defaultProgram()`) | the tests hand the built scanner over directly (`Session::Options::scanner`, `PluginIndex`'s), from the `SUBSTATION_SCANNER` compile definition |
| `SUBSTATION_UI_SCREENSHOTS` | A folder the UI's tests save screenshots into (`SUBSTATION_SCREENS` is read too) | not set: set it to look |
| `SUB_TEST_PLUGIN_CRASH`, `SUB_TEST_PLUGIN_HANG` | `1`: the test plug-ins' module kills the process, or hangs, as it loads | the scanner's tests |
| `SUB_FAKE_SCANNER`, `SUB_FAKE_SCANNER_LOG` | `test_plugin_index` started as a fake scanner (below), and the file it logs the files it reads to | `test_plugin_index` |

## The fake ASIO driver

[tests/asio_driver/test_asio_driver.cpp](../tests/asio_driver/test_asio_driver.cpp) is *SUB Test ASIO*: a real
in-process COM object, like a real driver, loaded from its DLL rather than registered. `SUBSTATION_ASIO_DRIVERS`
lists the drivers the engine uses instead of the installed ones, so the ASIO tests never see the installed drivers
and need no sound card. It is built (as `SUBTestAsio.dll`, beside the test programs) only on Windows, with the ASIO
SDK, and with the test plug-ins on. The engine's tests get its path as the compile definition
`SUBSTATION_TEST_ASIO_DRIVER`.

It has 4 inputs and 6 outputs. By default it uses `ASIOSTInt32LSB`, buffer sizes 32 to 2048 (preferred 256, powers of
two), input and output latencies of the buffer size plus 32 and 64 samples, and 44.1 kHz.

Once started, a thread calls the host's `bufferSwitchTimeInfo` at the pace a sound card would. In **manual mode** the
test drives it instead, a buffer at a time on the test's thread, so what the host plays can be checked sample by
sample. Its sample formats are encoded in the driver independently of the engine's conversions, and the tests decode
the outputs with a decoder of their own (`decode()` in [AsioDriver.h](../tests/engine/harness/AsioDriver.h)).

The hooks are exported functions ([test_asio_driver.def](../tests/asio_driver/test_asio_driver.def)). The tests call
them through `AsioDriver` ([harness/AsioDriver.h](../tests/engine/harness/AsioDriver.h)), which loads the same DLL the
engine loads and binds them by name; making one resets the driver:

| Hook | `AsioDriver` | What it does |
|---|---|---|
| `SubTestAsio_Reset()` | (made) | Restores the default configuration and clears statistics and captured output. |
| `SubTestAsio_SetSampleType(type)` | `setSampleType` | The next instance's `ASIOSampleType`. |
| `SubTestAsio_SetBufferSizes(min, max, preferred, granularity)` | `setBufferSizes` | What `getBufferSize` reports (a driver that sets its size itself has min = max). |
| `SubTestAsio_SetLatencies(inputExtra, outputExtra)` | `setLatencies` | Latencies reported on top of the buffer size. |
| `SubTestAsio_SetManual(on)` | `setManual` | Manual mode. |
| `SubTestAsio_SetInputLevel(channel, level)` | `setInputLevel` | What an input delivers. |
| `SubTestAsio_SetLoopback(output, input)` | `setLoopback` | A cable from an output back into an input, delayed by the input and output latencies the driver reports, as a real loopback would be (-1, -1: none). |
| `SubTestAsio_FailInit(message)` | `failInit` | `init()` fails with this message. |
| `SubTestAsio_SetControlPanelChange(bufferSize, rate)` | `setControlPanelChange` | What the control panel changes when it is opened. |
| `SubTestAsio_Process(buffers)` | `process` | Manual mode: runs that many buffer switches now; returns how many ran. |
| `SubTestAsio_ReadOutput(channel, dest, maxBytes)` | `output` | The bytes the host wrote to an output so far, raw in the driver's format (up to 8 MB a channel); with no destination, how many there are. |
| `SubTestAsio_ClearOutput()` | `clearOutput` | Forgets the captured output. |
| `SubTestAsio_SendMessage(selector, value)` | `sendMessage` | Sends the host an `asioMessage` (reset request, latencies changed, ...); -1 if no host listens. |
| `SubTestAsio_ChangeSampleRate(rate)` | `changeSampleRate` | The "hardware" changes its rate (another clock) and tells the host. |
| `SubTestAsio_Get(key)` | `get` | Statistics: `instances`, `inits`, `control_panels`, `starts`, `running`, `switches`, `time_info`, `buffer_size`, `inputs`, `outputs`, `output_ready`, `rate`. |
| `SubTestAsio_InitHandle()` | `initHandle` | The window handle the host passed to `init()`. |

With the loopback, a take of what the engine played comes back as the input it records, so a recording can be held
sample by sample to the arrangement it was recorded against. `parallel_render_bench --live` uses the same driver in
manual mode to time the audio thread.

## The test VST3 plug-ins

[tests/vst3_plugins](../tests/vst3_plugins) builds one module, `SUBTestPlugins.vst3`, with four tiny plug-ins whose
output the tests can predict exactly. They cover the host's code paths rather than sounding good:

| Plug-in | What it is |
|---|---|
| **SUB Test Synth** (instrument; [test_plugins.cpp](../tests/vst3_plugins/test_plugins.cpp)) | A processor with a separate edit controller. Each held note adds velocity/127 (*DC* mode) or a sine at the note's pitch, times *Gain*. It reports the transport it was given (tempo, playing, beat, loop) back as read-only parameters, and has ten *Macro* parameters that only its controller keeps (in its own state). |
| **SUB Test Effect** ([test_effect.cpp](../tests/vst3_plugins/test_effect.cpp)) | A single-component effect (processor and controller in one object, like most JUCE plug-ins): *Gain*, a *Latency* parameter that delays the audio and reports it (up to 4096 samples), a bypass parameter, state, and on Windows a Win32 editor that can resize itself and edit a parameter on request (elsewhere it has no editor). |
| **SUB Test Mono** | Mono in, mono out, no edit controller. |
| **SUB Test Sidechain** ([test_sidechain.cpp](../tests/vst3_plugins/test_sidechain.cpp)) | A single-component effect with a stereo aux input, inactive until the host activates it: its output is its input plus its sidechain, and a read-only parameter, *Key Silent*, says whether the host flagged the sidechain silent. |

They are built on Windows and Linux ([TestPlugins.cmake](../tests/TestPlugins.cmake)), into a bundle laid out as VST3
bundles are on that platform: `build/testplugins/SUBTestPlugins.vst3/Contents/x86_64-win/SUBTestPlugins.vst3` on
Windows, `.../Contents/x86_64-linux/SUBTestPlugins.so` on Linux (`aarch64-linux` on ARM). The tests get the bundle's
path as the compile definition `SUBSTATION_TEST_PLUGINS_BUNDLE`, and load the plug-ins from it directly (or scan it),
never from the system's folders.

Environment variables make the module misbehave on purpose, for the scanner's tests: `SUB_TEST_PLUGIN_CRASH=1` kills
the process as the module loads, `SUB_TEST_PLUGIN_HANG=1` makes loading hang.

The single-component plug-ins have their own translation units because the SDK's `SingleComponentEffect` renames
`IEditController::setState` with a macro, which only works if `vstsinglecomponenteffect.h` comes first
([test_plugins.h](../tests/vst3_plugins/test_plugins.h)).

### The fake scanner

Most of the scanner's tests need no plug-ins: [test_plugin_index.cpp](../tests/app/test_plugin_index.cpp) is also a
fake `substation-scan`. Started with `SUB_FAKE_SCANNER` in its environment (as its scans start it), it speaks the
scanner's protocol before Qt starts and answers for each file by what the file says: `plugins:<JSON list>`,
`error:<reason>`, `crash` (it dies), `hang` (it never answers), `noise...` (stray lines first).
`SUB_FAKE_SCANNER_LOG` names a file it appends `<process id> <path>` to for each file it reads, so a test can see which
files were read, and by how many processes. The real scanner reads junk files, and the test plug-ins when they are
built. See [app/plugin-scanner.md](app/plugin-scanner.md).

## What the tests cover, by area

### The engine ([tests/engine](../tests/engine), `engine_tests`)

Rendered offline unless the file says otherwise; the live tests play through the fake ASIO driver in manual mode.

| File | What it covers |
|---|---|
| [test_engine_render.cpp](../tests/engine/test_engine_render.cpp) | Clip placement to the sample, tempo, offsets, gain, pan, mute and solo, looping, the metronome, clip fades (the short ones only where a clip cuts into its file, a clip's own with their curves, in a warped clip's time), Utility and Over The Top, sources and peaks, resampling to the engine's rate, export, transport state without a device, chains, moving processors, the master's devices. |
| [test_automation_engine.cpp](../tests/engine/test_automation_engine.cpp) | Envelopes of mixer controls (tracks and master) and of device parameters, sample by sample. |
| [test_switches_engine.cpp](../tests/engine/test_switches_engine.cpp) | A track's activator and a device's (or a rack's) on/off, automated: silence and the fade, standing in for mute, a latent device passed by in time. |
| [test_warp.cpp](../tests/engine/test_warp.cpp) | Warped clips on their beats at any tempo, pitch shifting, Re-Pitch filtering rather than aliasing. |
| [test_midi_engine.cpp](../tests/engine/test_midi_engine.cpp) | Note scheduling and the built-in Synth: notes on their sample, following the tempo, pitch and level, no hanging notes at loop wraps and after renders. |
| [test_dsp_blocks.cpp](../tests/engine/test_dsp_blocks.cpp) | The building blocks the built-in effects share ([DspBlocks.h](../engine/src/builtin/DspBlocks.h)), each measured against what it is meant to do: the one-pole smoothing and the DC blocker, the delay line's reads (between samples too), the envelope follower's times, noise and `fastTanh`, the LFO's shapes and cycles, biquads, the crossover's bands adding up flat, the sliding maximum, oversampling (transparent in band, delayed by its latency, saturation folding back far less), a low, narrow biquad ringing out to exact zeros. |
| [test_builtin_devices_engine.cpp](../tests/engine/test_builtin_devices_engine.cpp) | What every built-in device shares: NaN, infinity and absurd levels in its input taken as silence (every effect, one bad sample in each channel amid noise: the output stays finite and the device still plays); a device's latency changed back after another edit still compensated. |
| [test_compressor_engine.cpp](../tests/engine/test_compressor_engine.cpp) | The Compressor: its gain curve, its displays, keying from a sidechain. |
| [test_gate_engine.cpp](../tests/engine/test_gate_engine.cpp) | The Gate: opening at the threshold and closing below Return, after Hold, over Attack and Release, to the sample; Floor and Flip; lookahead as latency; keying from a sidechain (its gain and mix) and the key EQ playing as its editor draws it; listening to the key; every control changing without a jump or a click; automation to the sample whatever the block size; reset and a new rate; the extremes, stability and silence ringing out to exact zeros; one channel; its displays. |
| [test_limiter_engine.cpp](../tests/engine/test_limiter_engine.cpp) | The Limiter: below the ceiling the input, its lookahead late; no sample over the ceiling in any mode, routing or link, and in True Peak mode no peak between samples either (against a reference meter of the test's own); a lone peak caught exactly; the manual release and Auto's (sustained bass not distorted); the true-peak interpolator and its parabola; Soft Clip; Maximize; L/R, M/S and Link; every control changing without a click, the glides smooth in dB; automation through the engine; reset and new rates; the extremes and input that isn't finite; silence ringing out to exact zeros; one channel; the other tracks delayed by its lookahead; its displays. |
| [test_multiband_engine.cpp](../tests/engine/test_multiband_engine.cpp) | Multiband Dynamics: flat when it does nothing; each side's law (Above and Below, compressing and expanding, the knee, Amount); attack, release and Time as Ableton defines them; Peak and RMS; Input and the Outputs; each band switched off (into the mid band), bypassed by its activator and soloed; Listen; a sidechain keying each band by its own band; every control changing without a click; automation through the engine, to the sample; reset and new rates; silence ringing out to exact zeros; NaNs and infinities in the input and the key, and the loudest input let in; the extremes; one channel and linked stereo; the tail; its displays; its cost; the same output however blocks are cut. |
| [test_spectral_engine.cpp](../tests/engine/test_spectral_engine.cpp) | The Spectral Compressor (a compressor per frequency): its gentle defaults and the shared maths; its latency (the frame and a hop, to the sample) and transparency, fully dry the input delayed bit for bit; pink noise reading its own level; downward and upward compression, Range, Tilt, Smoothing, the Focus band and Delta, with numbers; attack and release in time; Stereo Link; keying by a sidechain; every control changing without a click; automation to the sample, alone and through the engine; reset, silence ringing out to exact zeros, the extremes, NaN, infinity and absurd levels in the input and the key, one channel; its displays; the engine lining other tracks up with it; its cost (the thread's CPU time), in all and per audio callback. |
| [test_saturator_engine.cpp](../tests/engine/test_saturator_engine.cpp) | The Saturator: quiet audio untouched at the defaults, bit for bit; each curve playing as its editor draws it, and the curves' numbers; the Waveshaper's controls; odd harmonics only; Post Clip holding the output to the Output level at any Dry/Wet; Output and Dry/Wet; Color leaving a clean sound alone and moving the saturation; DC; Hi-Quality's latency, aliasing and pre-roll; every control changing without a click; automation through the engine, to the sample; reset and a new rate; silence ringing out to exact zeros; the tail; the extremes; NaN and infinity in the input coming out as zeros would; one channel, and channels independent; its displays. |
| [test_amp_engine.cpp](../tests/engine/test_amp_engine.cpp) | The Amp: its design (the tone stacks, the curve and its anti-aliasing); each model's character, level-matched at the defaults; Gain, Volume (driving the power amp on Blues, Heavy and Bass) and the sag; NaN and infinity in its input playing as zeros; the tone curve and the transfer curve as played; the tone controls driving the stage after them; Mono and Dual; a model change morphing without a click and keeping its level (worked out a cell at a time, the same in any blocks; changes faster than a morph); every dial click-free; automation through the engine, to the sample; Dry/Wet with the dry delayed; latency reported and compensated; the tail and silence ringing out to exact zeros; reset and a new rate; the extremes at any rate; no DC; aliasing kept far down; its displays; its cost; any block size; sleeping through silence and waking as a fresh amp. |
| [test_erosion_engine.cpp](../tests/engine/test_erosion_engine.cpp) | Erosion: Amount 0 a clean delay of its latency (2 ms at any rate; through the engine too, aligned), a sine's sidebands as theory says, Stereo's quarter cycle, the noise as strong at any Frequency and Width and while the band moves, each device its own noise, highs eroded first, Width spreading the sidebands, the same output however blocks are split, every control changing without a click and landing where it was turned, automation through the engine (in its chunk), reset and a new rate, the extremes, silence to exact zeros, NaN and infinity in as silence, one channel the left of two, its displays, the band the editor draws. |
| [test_delay_engine.cpp](../tests/engine/test_delay_engine.cpp) | The Delay: synced and free times, offset, link, feedback, ping pong, freeze, the filter, the modes, its display. |
| [test_chorus_engine.cpp](../tests/engine/test_chorus_engine.cpp) | The Chorus-Ensemble: the design's voices; fully dry, the input bit for bit; at Amount 0 each layout a plain delay of its centre; the delays, sample by sample, in every mode and at any rate; Ensemble beating and the voices' detune; feedback and Invert (none in Vibrato), Warmth, the high-pass keeping the lows out of the delays, Width (one channel or three), Output and Dry/Wet; every control changing without a click, and switching it on in the middle of a sound; NaN and infinity in the input as silence; automation through the engine, to the sample; reset and a new rate; stability at the extremes; any block size; silence ringing out to exact zeros; its tail, no latency; its displays. |
| [test_phaser_engine.cpp](../tests/engine/test_phaser_engine.cpp) | The Phaser-Flanger: the notches where the design puts them (in closed form, against the stages' own phase and the measured response), Spread moving them apart, the LFO sweeping them, every LFO shape and Duty as its design draws it (and at Live's fastest, 40 Hz), LFO 2, stereo Phase and Spin; its parameters and their ranges, Live's; the flanger's comb and its feedback (and Ø) to the sample, the doubler's copy, the delays swept; the envelope follower, Safe Bass, Warmth, Output and Dry/Wet; a change of mode or of Notches landing exactly on the new setting; every control changing without a click, and a fast sweep into the stages' limits; automation through the engine, to the sample; synced LFOs following the song, random shapes repeating, an LFO's jumps crossfading; reset and a new rate; switched on in the middle of a sound (the renderer's switch emulated) coming in without a click in every mode, to the sample whatever the blocks; NaN and infinity in its input playing as silence; silence ringing out to exact zeros, nothing denormal on the way; the tail; stability at the extremes and at 22.05 to 192 kHz; one channel playing as either of two; its displays; its cost (in the thread's CPU time); the editor's curve (notches at their depth, a dense comb as a steady band). |
| [test_reverb_engine.cpp](../tests/engine/test_reverb_engine.cpp) | The Reverb: exact silence for silence, fully dry the input untouched; the decay per band as its design says (by Schroeder integration), the shelves damping their bands; the reflections placed as `earlyTaps` says, Shape moving the diffuse onset; the input filter as its design draws it; mono in, Stereo from mono to two independent sides, one channel; the levels; Freeze, Cut and Flat; the guard; each Density, and a change of it keeping a frozen tail; Spin swinging and drifting the reflections as its design says; Chorus, Diffusion and Scale; no metallic ringing; every control and switch changing without a click; automation through the engine, to the sample; reset and a new rate; the extremes; a NaN or an infinity in its input playing as silence; silence ringing out to exact zeros, and waking as a fresh device; its tail; its displays; the design's helpers for the editor; its cost (in the thread's CPU time). |
| [test_disperser_engine.cpp](../tests/engine/test_disperser_engine.cpp) | The Disperser: untouched with no stages, bypassed or fully dry, Dry/Wet's blend (to the sample) and its notches, a flat magnitude and the design's group delay (at the extremes, at 8 to 192 kHz, kept below Nyquist), never more energy out than in however Frequency and Pinch jump, each channel its own, Amount landing on the new number of stages, every control changing without a click, automation through the engine to the sample, reset and a new rate, silence ringing out to zeros, its tail, no latency. |
| [test_eq_engine.cpp](../tests/engine/test_eq_engine.cpp) | The EQ: each band plays as its curve shows, the curves against the analog filters, placement (mid, side), output gain and gain scale, extremes, the displays. |
| [test_sampler_engine.cpp](../tests/engine/test_sampler_engine.cpp) | The Sampler: its sample (its state), pitch from key, root and tuning at any file rate, start, end and loop, velocity, a missing file, swapping the sample while it plays; as Simpler: transients and slices, Slice (by region, beat, transient; Mono, Poly, Thru), 1-Shot (Trigger, Gate, fades), Classic's loop start and crossfade, reverse, snap, gain, pan, the filter (and its ringing out), the LFO, voices and glide, warping (resampled and stretched), repeatable renders. |
| [test_sidechain_device_engine.cpp](../tests/engine/test_sidechain_device_engine.cpp) | The Sidechain device: hits to the sample, the curve, depth, smoothing, threshold and re-arming, lookahead as latency, Lows Only, hits on the beat, its displays. |
| [test_groups_engine.cpp](../tests/engine/test_groups_engine.cpp) | Group buses, delay compensation at every summing point, solo and mute across levels. |
| [test_sends_engine.cpp](../tests/engine/test_sends_engine.cpp) | Sends before and after the fader, delay compensation per edge, cycles refused, solo and mute across sends. |
| [test_track_routing_engine.cpp](../tests/engine/test_track_routing_engine.cpp) | Ableton's routings: Sends Only, outputs into a device's sidechain (summed, lined up, cycles refused, the device going), Track In heard while monitored, inputs from a track tapped Pre FX, Post FX or Post Mixer. |
| [test_sidechain_engine.cpp](../tests/engine/test_sidechain_engine.cpp) | Sidechains lined up with the signal at the device sample for sample at every tap and latency (SUB Test Sidechain), cycles, the source going, mute and solo. |
| [test_racks_engine.cpp](../tests/engine/test_racks_engine.cpp) | Racks: chains summed, their faders, mute and solo, delay compensation inside and around them, nested automation, instrument racks, sidechains into racks, moves, nesting limits, chain meters, renders bit-identical with and without workers. |
| [test_freeze_engine.cpp](../tests/engine/test_freeze_engine.cpp) | A track's signal before its fader rendered (lined up, solo ignored, a group's bus, its tail, into a 32-bit WAV); frozen tracks playing through their faders without their devices or notes; frozen groups; sends tapping frozen audio; no latency from frozen tracks. |
| [test_render_jobs.cpp](../tests/engine/test_render_jobs.cpp) | Renders in the background (`RenderJob`): the same as in the foreground, cancelled (no file left), one at a time, the project as it was when it started, let go of unfinished. |
| [test_parallel_engine.cpp](../tests/engine/test_parallel_engine.cpp) | Rendering on several threads, bit-identical to one thread on random routing graphs; cost ordering on a hand-made graph. |
| [test_vst3_engine.cpp](../tests/engine/test_vst3_engine.cpp) | VST3 hosting with the test plug-ins: instruments and effects, parameters, transport and loop splitting, latency, mono buses, state, editors (Windows). |
| [test_asio.cpp](../tests/engine/test_asio.cpp) | ASIO with the fake driver: finding and opening it, rates and buffer sizes, channels, every sample format, inputs, the driver's requests (resets, new latencies, another clock), its control panel, errors, playing on the driver's thread. |
| [test_recording.cpp](../tests/engine/test_recording.cpp) | Audio input and recording: inputs, monitoring, takes lined up with the timeline through a loopback (with latent plug-ins on a track or the master), count-in, what ends a recording. |
| [test_midi_input.cpp](../tests/engine/test_midi_input.cpp) | MIDI input: live notes at their offsets in a block, which tracks hear which input and channel, monitoring, releases, recorded notes on the beats they were played. |
| [test_resampling_engine.cpp](../tests/engine/test_resampling_engine.cpp) | Takes of a track's, group's, return's or the master's output equal to its render; monitoring a source isn't delayed; cycles. |
| [test_parallel_live.cpp](../tests/engine/test_parallel_live.cpp) | The live recording, MIDI input and resampling tests again, with the tracks shared out among workers. |

### The intelligence module ([tests/intelligence](../tests/intelligence), `intelligence_tests`)

Qt-free, with the engine tests' harness (included as `harness/Test.h`: the harness folder itself is never an include
folder, since its `Signal.h` would be found for `<signal.h>` where file names ignore case). Decoding and resampling;
Essentia's descriptors: what each aspect of a fingerprint tells apart, on drum hits, synth one-shots, tones and loops
made from formulas, and what doesn't change it (level, rate, leading silence), with silence, clicks, odd rates and
broken samples and files; comparing and the library's statistics; the store (another extractor's is ignored); the
index's threads, saving and checking stamps, the library's statistics kept and measured again, searches, cancelling
(with an extractor of the tests' own, slow on purpose), an extractor that can't be made; harmony: the key, chords from notes (sevenths,
inversions, melodies, arpeggios, rests), the parts written from chords; humanizing with the velocity model shipped
(models/velocity.hbm): loading it, what it predicts depending only on the notes, the targets' level, the amount.
See [intelligence.md](intelligence.md#tests).

### The model and the editor ([tests/app](../tests/app))

| File | What it covers |
|---|---|
| [test_project.cpp](../tests/app/test_project.cpp) | The project's queries: the group tree, the routing graph, freezing, devices in racks. |
| [test_commands.cpp](../tests/app/test_commands.cpp) | Each undo command puts its snapshot in place and back, the project signals every change, gestures merge. |
| [test_edits.cpp](../tests/app/test_edits.cpp) | Pure clip maths: overlaps, cuts, trims, splits, tempo fitting, warping, reversing, stretching audio and MIDI clips, sliding their content, fades kept at the clips' ends. |
| [test_midi_model.cpp](../tests/app/test_midi_model.cpp) | MIDI clips as windows onto notes (consolidating what plays: deactivated clips' notes come deactivated), deactivated notes (not heard; saved), the piano roll's note maths, MIDI tracks in files. |
| [test_automation_model.cpp](../tests/app/test_automation_model.cpp) | Envelope maths, target keys, the parameter mappings held against the engine's, saving. |
| [test_timebase.cpp](../tests/app/test_timebase.cpp), [test_keys.cpp](../tests/app/test_keys.cpp) | Positions, bar labels, dB and pan text; tempo and key from file names, transposing to the project's key. |
| [test_serialization.cpp](../tests/app/test_serialization.cpp) | Saving and loading, paths that move with the project, older files, what an edited file can't have, what of frozen audio plays, clip fades (only where there are some; too long, held to their clip), deactivated clips; a file using every feature, as earlier versions wrote it, loads and saves the same. |
| [test_presets.cpp](../tests/app/test_presets.cpp) | The preset library, default presets, presets as new devices, rack names. |
| [test_editor_edits.cpp](../tests/app/test_editor_edits.cpp) | The editor's clip edits and undo: adding, moving, duplicating, splitting, tempo and warping trims, gesture merging, inputs and arming, recorded takes, copy and paste with the automation under clips, time selections, reversing, deactivating (split at a range's edges). |
| [test_track_names.cpp](../tests/app/test_track_names.cpp) | Track names: `#` numbering by place (tracks coming, going, moving, groups), audio tracks named by their clips' files (reversed clips, takes), MIDI tracks by their instrument, renaming, undo, duplicates, saving and loading older names. |
| [test_editor_midi.cpp](../tests/app/test_editor_midi.cpp) | MIDI tracks and their instrument, MIDI clips and note edits, clips moving only onto tracks of their kind, MIDI inputs, MIDI takes. |
| [test_editor_automation.cpp](../tests/app/test_editor_automation.cpp) | Undoable envelope edits, range edits across lanes, deleted devices' automation, touched parameters, the lanes shown, automation moving with clips unless locked. |
| [test_editor_duplicate.cpp](../tests/app/test_editor_duplicate.cpp) | Duplicating tracks (one undo step, routing following the copies); cutting, copying and pasting automation. |
| [test_editor_groups.cpp](../tests/app/test_editor_groups.cpp) | Grouping, ungrouping, moving tracks into and out of groups, deleting, folding, arming, saving, copying groups. |
| [test_editor_sends.cpp](../tests/app/test_editor_sends.cpp) | Returns and sends: making and deleting returns (one undo step), levels and taps, cycles, solo, saving. |
| [test_editor_resampling.cpp](../tests/app/test_editor_resampling.cpp) | Inputs from other tracks' outputs and the master's: cycles refused, inputs dropped by regrouping and deleting (one undo step), saving. |
| [test_editor_outputs.cpp](../tests/app/test_editor_outputs.cpp) | Tracks' outputs (Audio To): each one undo step, what they can go into, cycles refused, going into another group, what they went into going, copies, inputs' taps, saving and repairing. |
| [test_editor_sidechain.cpp](../tests/app/test_editor_sidechain.cpp) | Sidechains: taps, cycles refused, sidechains dropped by moves and deletes (one undo step), saving. |
| [test_editor_racks.cpp](../tests/app/test_editor_racks.cpp) | Racks: grouping and ungrouping, chains and their mixers, moves, nesting limits, instrument racks, macros (how many, their names, their ranges), sidechains in racks and taken after devices in them, saving, presets as new devices, showing chain lists and devices. |
| [test_editor_presets.cpp](../tests/app/test_editor_presets.cpp) | Loading a preset into a device of its kind (one undo step), default presets, racks named as their presets. |
| [test_editor_freeze.cpp](../tests/app/test_editor_freeze.cpp) | What can be frozen, freezing and unfreezing, what frozen tracks refuse, flattening, saving. |
| [test_editor_frozen_areas.cpp](../tests/app/test_editor_frozen_areas.cpp) | Time selections over frozen tracks and groups (delete, move, Ctrl-drag copy, duplicate, cut, paste) taking their frozen audio along in one undo step; what is refused (some of a frozen group, moves between a frozen track and another, pastes of what wasn't copied from it, other clip edits); unfreezing and flattening afterwards. |

### The engine bridge

| File | What it covers |
|---|---|
| [test_bridge_tracks.cpp](../tests/app/test_bridge_tracks.cpp) | Clips and notes as the engine has them (none of deactivated clips or notes), groups as buses, returns and sends (made silent when only automated), inputs from tracks, the master's mixer, the settings, a drag's preview, overrides. |
| [test_bridge_devices.cpp](../tests/app/test_bridge_devices.cpp) | Racks keeping each device's processor as they are made, undone, moved and deleted; chain and nested automation and overrides; macros moving plug-in parameters, and macro automation moving what is mapped to them; sidechains; presets reaching the engine. |
| [test_bridge_plugins.cpp](../tests/app/test_bridge_plugins.cpp) | Plug-ins in projects and their state, a project's plug-ins loading after it opens, missing and moved plug-ins, devices keeping their processors as they move, plug-ins' reports (edits for undo only from a shown editor), editors, automating plug-in parameters. |
| [test_bridge_freeze.cpp](../tests/app/test_bridge_freeze.cpp) | Frozen tracks playing their frozen audio without their devices (and getting them back), where a time selection moved it and where a drag would take it, renders in the background followed and cancelled, reversed copies written and found again. |
| [test_bridge_recording.cpp](../tests/app/test_bridge_recording.cpp) | What records and why not, inputs, monitoring and arming in the engine, a take recorded, drawn live and handed on. |
| [test_bridge_settings.cpp](../tests/app/test_bridge_settings.cpp) | The audio and MIDI preferences' settings, where the bridge puts its files and how it names them, the float WAV writer, the EQ's curve, the Disperser's group delay, device status, the scope, MIDI inputs. |

### The session

| File | What it covers |
|---|---|
| [test_selection.cpp](../tests/app/test_selection.cpp) | Selecting tracks (Ctrl and Shift), clip and lane ranges (and the rows they show over), breakpoints, the insert marker, what is left of a selection after the project changes. |
| [test_session_edit.cpp](../tests/app/test_session_edit.cpp) | The Edit and Create commands as the menus and keys drive them: split, select all, duplicate and delete, cut, copy and paste of clips, automation and tracks (one clipboard), groups, returns, reversing (at once and in the background), deactivating clips (0) and what the engine then plays. |
| [test_session_devices.cpp](../tests/app/test_session_devices.cpp) | The device view's selection and clipboard, folding, racks, drops; presets saved, loaded as new devices or into devices, default presets, renaming, the preset index watching the library. |
| [test_session_files.cpp](../tests/app/test_session_files.cpp) | Saving and opening (a Disperser and its automation, as far as the engine), the title, the recent projects, Export Audio's choices, count-in and record quantization, the preferences. |
| [test_session_renders.cpp](../tests/app/test_session_renders.cpp) | Exporting and freezing in the background, their progress and Cancel, closing while one runs, freezing and flattening the selected tracks, a project's plug-ins loading after it opens. |
| [test_session_engine.cpp](../tests/app/test_session_engine.cpp) | What the engine hears of the editor's edits: groups, returns and sends, inputs, sidechains, racks and macros, presets rendering as the devices they were saved from, frozen tracks, MIDI clips' notes. |
| [test_builtin_effects.cpp](../tests/app/test_builtin_effects.cpp) | The ten new audio effects with the rest of the application, a row per device: every parameter away from its default (one automated) saved and opened again, in the model and the engine, and sounding as it did; presets; in a rack's chain as on the track; latency lined up with a dry track, to the sample (after opening, and as it changes); undo and redo reaching the engine; sidechains keyed by another track; switched off and on again, in the sound and in silence, without a click or anything held before. |
| [test_session_keyboard.cpp](../tests/app/test_session_keyboard.cpp) | The computer MIDI keyboard: the letter keys, octaves, keys kept from shortcuts and text inputs, notes released. |
| [test_file_manager.cpp](../tests/app/test_file_manager.cpp) | The File Manager and hot swaps: the files a project plays (clips and samplers, in racks), a file replaced (whole files and stretches, trimmed at the next clip, frozen tracks refused, a hot swap's tries one undo step from where it began), missing files matched and moved along, the search in the background (the project's folder, a folder chosen, the browser's places), Locate, the list and its filter, hot swaps through the browser. |

### The browser, plug-ins and analysis

| File | What it covers |
|---|---|
| [test_browser_native.cpp](../tests/app/test_browser_native.cpp) | The browser backend against the reference (`BrowserReference`): the same files from a folder tree in the same order, the same results for random queries, sorts and use counts; the text rules (lower case, case folding, splitting into words) for every Unicode character; the saved index, changes while running, the latest search's results only, paging. See [browser.md](browser.md). |
| [test_browser_search.cpp](../tests/app/test_browser_search.cpp) | Item keys, use counts (decay, `library.json`, unknown fields, bad files), match quality, rank. |
| [test_browser_controller.cpp](../tests/app/test_browser_controller.cpp) | The browser's logic without its panel: places, searching, sorting, the sidebar, folder trees, activation and drops, previews, keeping the list's place, plug-ins and presets, drag payloads. |
| [test_sound_similarity.cpp](../tests/app/test_sound_similarity.cpp) | Find Similar: the browser's files analysed in the background; the list of the sounds most like one, its sort and status, filtering it, ending it; a clip's part of a loop; a sound outside the library; files found later; a sound that can't be analysed. See [intelligence.md](intelligence.md). |
| [test_harmony.cpp](../tests/app/test_harmony.cpp) | The song's harmony (`Session.harmony`): the notes it hears (MIDI tracks heard, not drums; what clips play, where; not deactivated clips), following the project once edits settle, the key (the project's or inferred), the time signature's bars, the setting that shows it. See [intelligence.md](intelligence.md#harmony-the-application-side). |
| [test_humanizer.cpp](../tests/app/test_humanizer.cpp) | Humanizing velocities (`Session.humanizer`): the model next to the application, each track judged with the notes it plays around the targets (where its clips play them; not deactivated notes or clips), each track keeping its level, the amount. See [intelligence.md](intelligence.md#humanizing-the-application-side). |
| [test_plugin_index.cpp](../tests/app/test_plugin_index.cpp) | Scanning in child processes (a crashing or hanging plug-in costs only itself), the cache, finding plug-in files, friendly messages, the index scanning in the background over the standard and the user's folders. |
| [test_sidechain_fit.cpp](../tests/app/test_sidechain_fit.cpp) | Fitting the Sidechain's curve to a kick: the clash, the envelope, the reduction, the points, capturing hits. |

### The UI (`test_ui_*`, on a display)

| File | What it covers |
|---|---|
| [test_ui_sg.cpp](../tests/app/test_ui_sg.cpp) | `SgCanvas` and `SgPainter` rendered for real and checked pixel by pixel; a benchmark of a big arrangement's worth of rectangles. |
| [test_ui_theme.cpp](../tests/app/test_ui_theme.cpp), [test_ui_controls.cpp](../tests/app/test_ui_controls.cpp), [test_ui_gallery.cpp](../tests/app/test_ui_gallery.cpp) | The theme's colours and icons; knobs, value boxes, meters, the oscilloscope and buttons driven as a user would; a gallery of every control. |
| [test_ui_mainwindow.cpp](../tests/app/test_ui_mainwindow.cpp) | The main window: its layout, every menu action, shortcuts, Open Recent, files and the unsaved-changes question, closing, the title bar (its status line, the window's buttons and what drags the window), Export Audio, the clip view over the arrangement, the window's place kept. |
| [test_ui_transport.cpp](../tests/app/test_ui_transport.cpp), [test_ui_dialogs.cpp](../tests/app/test_ui_dialogs.cpp) | The transport bar's controls; the preferences' pages, Export Audio, the render progress dialog, the message boxes. |
| [test_ui_arrangement.cpp](../tests/app/test_ui_arrangement.cpp), [test_ui_arrangement_automation.cpp](../tests/app/test_ui_arrangement_automation.cpp), [test_ui_arrangement_tracks.cpp](../tests/app/test_ui_arrangement_tracks.cpp), [test_ui_arrangement_selection.cpp](../tests/app/test_ui_arrangement_selection.cpp), [test_ui_arrangement_clips.cpp](../tests/app/test_ui_arrangement_clips.cpp) | The arrangement with the mouse, wheel and keys: clips (stretched with Alt, their content slid with Ctrl+Shift, their fades dragged with F held), time selections, the ruler, zoom and scroll, drops, menus, reversing, live takes; automation lanes and envelopes; tracks and headers, groups, returns and sends, inputs, sidechains, freezing; selections over every kind of track (shown over the rows they cover), Shift-click, frozen tracks' stretches dragged. |
| [test_ui_clipview.cpp](../tests/app/test_ui_clipview.cpp), [test_ui_pianoroll.cpp](../tests/app/test_ui_pianoroll.cpp) | The clip view with audio clips (settings, waveform, several clips in unison); the piano roll (notes drawn and heard, moved, resized, copied, keys (dragged over: their notes selected), a rubber band's notes heard, Ctrl+D by the stretch dragged over, copy and paste at the clicked beat, several clips edited together, velocity lane, note tools (Humanize › Velocity and › Timing), the song's chords along the top, notes out of the key in red, Generate). |
| [test_ui_browser.cpp](../tests/app/test_ui_browser.cpp) | The browser panel: the sidebar, searching, previews, activation, drags, keeping its place, the sort, folder trees, menus, places, presets renamed and deleted. |
| [test_ui_file_manager.cpp](../tests/app/test_ui_file_manager.cpp) | The File Manager panel beside the browser: its rows and the missing files' box, the filter, a row's menu, Show in File Manager, a row's hot swap button, the browser's hot-swap bar and Esc. |
| [test_ui_device_panel.cpp](../tests/app/test_ui_device_panel.cpp), [test_ui_device_panel_plugins.cpp](../tests/app/test_ui_device_panel_plugins.cpp), [test_ui_device_panel_presets.cpp](../tests/app/test_ui_device_panel_presets.cpp), [test_ui_device_panel_racks.cpp](../tests/app/test_ui_device_panel_racks.cpp), [test_ui_device_panel_sidechain.cpp](../tests/app/test_ui_device_panel_sidechain.cpp) | The device view: folding, the clipboard, drags and drops, menus; plug-in devices and their parameters; presets; racks, chains and macros; the sidechain button and its menu. |
| [test_ui_device_editors.cpp](../tests/app/test_ui_device_editors.cpp) | The built-in devices' own editors (Compressor, Delay, Disperser, EQ, Sampler: its pages, modes, slices, loop marker, warp and reverse; Sidechain; which kinds have one), what the editors share (`EditorPaint`'s meter ballistics and easing, `EditorKnob` and `DeviceParamMap`) and the shared parameter cell, against the project and offline renders. The other devices' editors have files of their own, below. |
| [test_ui_device_editors_gate.cpp](../tests/app/test_ui_device_editors_gate.cpp) | The Gate's editor: fitting the body, every control bound and undoable (the lookahead reaching the engine's latency); the threshold and return lines' drags (one undo step each) and their menus; the displays reaching the graph, scrolling and resting, silence and a steady tone drawing nothing; the key dot and the passing shade; the sidechain section (folding, what is dimmed but settable, the source button); the key EQ's curve being the engine's filter, its dot dragged, Ctrl and the wheel for the bell's Q. |
| [test_ui_device_editors_limiter.cpp](../tests/app/test_ui_device_editors_limiter.cpp) | The Limiter's editor: fitting the view, every control bound and undoable (Release dimmed but settable while Auto is on; boxes and lists wide enough; the lookahead reaching the engine's latency; Maximize swapping Gain for Output and the line for the Threshold), the line dragged (one undo step, Shift finely, held to the parameter's range, double-click for the default) and its hover following it, opening as the device is, the displays reaching the graph (levels, gain reduction, Soft Clip's share), its animation and its rest, the maths shared with the engine. |
| [test_ui_device_editors_multiband.cpp](../tests/app/test_ui_device_editors_multiband.cpp) | Multiband Dynamics' editor: fitting the body, every control bound and undoable (the engine having what they set; the activators and the split switches), the T/B/A pages (kept by device); the graph's threshold and ratio drags (with Ctrl, Alt and Shift), double-clicks and wheel, one undo step each; the displays reaching the graph as the engine renders (meters, eased gain, glows, lanes and highlights, the bars and figures agreeing as the meters let go) and stopping once still; a bypassed band's lane; the sidechain's controls (dimmed but settable, Listen); the `ratio` unit and the ratios and times typed (with no window, on any platform). |
| [test_ui_device_editors_spectral.cpp](../tests/app/test_ui_device_editors_spectral.cpp) | The Spectral Compressor's editor: fitting the body with every name and value whole, its knobs and Delta bound and undoable; the lines being the engine's, those leaving the plot drawn where they are with their handles on them, the level figures they cross fading; the Focus band dimmed by the engine's weights, the figures over the dim; Below dimmed (still settable) while Upward is 1:1; the Focus boxes wide enough for their widest value clear of the automation dot; the threshold, tilt, Below and Focus dragged (one undo step, Shift finely, double-click); the displays reaching the graph (cuts, lifts, the held cut, the glow only while cutting, Delta's tint), nothing drawn while still; the Sidechain badge, its menu under it. |
| [test_ui_device_editors_saturator.cpp](../tests/app/test_ui_device_editors_saturator.cpp) | The Saturator's editor: fitting the body (its columns in order, with either shaper section showing; the lists as wide as their longest names, DC and HQ on whole pixels at any width, no caption or readout cut short), every control bound, undoable and reaching the engine (Hi-Quality's latency), the Waveshaper and Bass Shaper sections swapping; the curve and Color's EQ being the engine's own `saturator::transfer` and `colorResponseDb`; an editor opening on the device as it is; the graphs' drags as single undo steps, showing the automation of what they move most; the displays reaching the curve (the dots, the afterglow, the over-full-scale flash) and the spectra, and both graphs settling without repaints in silence. |
| [test_ui_device_editors_amp.cpp](../tests/app/test_ui_device_editors_amp.cpp) | The Amp's editor: fitting the body, every control bound, undoable and reaching the engine; the models' names and the displays' rate and floor it takes through the application layer, and the Output buttons' names and the tone handles' ranges it takes from the parameters, being the device's; the model buttons and their sliding underline, an editor opened on a model showing it at once; the tone curve and the transfer being the engine's maths, the drive curve made again only for what it is made from; the tone handles as controls (drags as single undo steps, the wheel a fiftieth of the range a notch, both stopping at the range's ends, the automation dot, the parameter's menu); the displays reaching the tubes, the dots, the lamp and the meter (the sag, a backlog counting for nothing), and the face settling without repaints. |
| [test_ui_device_editors_erosion.cpp](../tests/app/test_ui_device_editors_erosion.cpp) | The Erosion's editor: fitting the body, an editor opening on the device as it is, its knobs bound and undoable (Width dimmed but settable, the glyphs by the engine's weights), the band as the engine's filter, the X-Y display's drags (Shift, Alt) and wheel (Ctrl finely, in a device chain too) as single undo steps, the displays reaching the graph and the scope (not a backlog's worth after the sound stopped; falling back by the time each refresh says), the engine having what it set. |
| [test_ui_device_editors_chorus.cpp](../tests/app/test_ui_device_editors_chorus.cpp) | The Chorus-Ensemble's editor: fitting the body (the high-pass box and Time wide enough for their texts), every control bound and undoable, the modes (their sets cross-fading; Feedback and Ø dimmed in Vibrato, still settable), Taps, Time, the high-pass and Ø; the display's drags as single undo steps, each setting only the parameter its direction picked; the displays reaching the graph (the voices where the engine's delays are, the glow from the sound now, freezing, resting without repaints), layouts fading without pops; the engine having what it set; no QML warnings. |
| [test_ui_device_editors_phaser.cpp](../tests/app/test_ui_device_editors_phaser.cpp) | The Phaser-Flanger's editor: fitting the body (its tabs on whole pixels, its knobs the house's 34 px, bipolar only about 0), every control bound and undoable, the swapped controls rebinding (Freq/Rate, Phase/Spin, the delay's Time, which stays put between the delay modes), a synced rate's wheel stepping a division a notch, the mode tabs, More as view state; the curve the engine's design with the notches marked where it puts them, a fine comb as a band; the graph's drags and double-click as single undo steps; the displays reaching the graph, going quiet and then not repainting; `DisplayPlayback` smooth at 1024-frame blocks, and at 2048-frame blocks the meters, a fast LFO's comet tail and a random shape's trace keeping their pace; what a curve costs (in the thread's CPU time, an optimized build only); no QML warnings. |
| [test_ui_device_editors_reverb.cpp](../tests/app/test_ui_device_editors_reverb.cpp) | The Reverb's editor: fitting the view and its own least height with nothing overlapping, its boxes, lists and switches as wide as their widest text, its knobs the house's 34 px, every control bound (with a tooltip) and undoable, Size as a bare number and Stereo in whole degrees; each row's dials in line; every switch lit under the mouse, Chorus's (over its Amount knob's caption) showing its own tooltip and the dial below the knob's; the spin pad's particles, lit and bobbing at Spin's most, clear of its captions at any Shape; the filter pad's, spin pad's and decay graph's drags (one undo step each, Shift finely, the engine having the values) and the decay graph's double-click; the curves being the engine's maths; the displays reaching them (input level and spectrum, the tail's meter and spectrum, Spin's phase); the transitions easing and everything resting in silence. |

## CI

[.github/workflows/ci.yml](../.github/workflows/ci.yml) builds everything (benchmarks too) and runs CTest on every push
to `master` (and the rewrite's branch), on pull requests, and by hand:

| Job | Runner | Compiler and Qt |
|---|---|---|
| Linux | `ubuntu-24.04` | GCC, Qt 6.4 from Ubuntu's packages, `xvfb` for the UI's tests |
| Windows | `windows-2022` | MSVC 2022, Qt 6.8 (`install-qt-action`) |

Each configures with `-G Ninja -DCMAKE_BUILD_TYPE=Release -DSUBSTATION_BUILD_BENCHMARKS=ON`, builds with
`ninja -C build -k 0` (every error at once) and runs `ctest --test-dir build --output-on-failure -j4`. Neither has
Steinberg's ASIO SDK, which isn't redistributable, so the ASIO tests skip there.

## Writing tests

- **Engine behaviour** goes into `tests/engine`: prefer offline renders (an `Engine` with no device): they are exact
  and need no hardware. Use `rampWav()` or `dcWav()` to make positions and levels visible in the output. For
  anything that needs the audio callback (inputs, recording, MIDI input, live behaviour), use `RecordingTest` (the
  fake driver in manual mode) and step it with `driver.process(n)`.
- **Plug-in behaviour**: use the test plug-ins (`addTestPlugin` skips the test without them; or
  `requireTestPlugins()`).
- **Application-layer behaviour** goes into `tests/app/test_<area>.cpp`, a Qt Test class: call `prepareApplication()`
  in `initTestCase`, end the file with `QTEST_GUILESS_MAIN(TestClass)` (`QTEST_MAIN` if it needs a
  `QGuiApplication`) and `#include "test_<area>.moc"`. Use the fixtures above (`EditorFixture` for the editor's rules,
  `Studio` for the bridge, `SessionFixture` for the session's actions) rather than making the pieces by hand.
- **UI behaviour** goes into `tests/app/test_ui_<area>.cpp`: a `UiSession`, the view in a window of QML, and input
  sent as a user would. Skip what needs drawn geometry without a display (`haveDisplay()`). moc stops reading a file
  at a C++ raw string literal: put inline QML in string constants after the test class (or in a support header).
  After a change that shows, hides or moves items, wait until the window is laid out again
  (`QQuickTest::qWaitForPolish(window)`) before taking an item's place to click it: layouts place their items when
  Qt Quick polishes, before the next frame, so until then items keep their old places and can lie over each other
  (a `QTRY_VERIFY` of something already true doesn't wait).
- Shared helpers go into `tests/app/support` (header-only if they need Qt Quick), not into a test file.
- A new test plug-in goes into `tests/vst3_plugins` and the `sub_test_plugins` target in
  [TestPlugins.cmake](../tests/TestPlugins.cmake); a new driver hook into `test_asio_driver.cpp`, its `.def` and
  `AsioDriver.h`.

## Benchmarks

[benchmarks/](../benchmarks) holds the benchmarks and their results: `parallel_render_bench` (render times on 1..N
threads, offline and live through the fake ASIO driver, checked bit-identical; `--heavy N --compare-ordering` measures
starting heavy tracks first), `browser_backend_bench` (the backend on a large synthetic library, every query checked
against `BrowserReference`), `builtin_devices_bench` (each built-in audio effect alone, as the audio thread runs it:
what it costs a core, at its defaults or at settings given) and `sound_similarity_bench` (sound similarity on a real
sample library: speed, and how often a one-shot's nearest sounds are of its kind). They are built with
`-DSUBSTATION_BUILD_BENCHMARKS=ON`, into `build/bin`, and run by hand; CTest doesn't run them. How to run them, what
they measure and the results are in [benchmarks/README.md](../benchmarks/README.md).
