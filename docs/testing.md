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
`intelligence_tests`, which runs and filters as `engine_tests` does, with the same harness), and one per
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
| `SUBSTATION_UI_SCREENSHOTS` not set | `test_ui_device_editors`'s screenshots of every editor |

## Layout

```
tests/
  CMakeLists.txt        the boundary check, engine_tests, intelligence_tests, and a program per tests/app/test_*.cpp
  TestPlugins.cmake     the test VST3 bundle and, with the ASIO SDK, the fake ASIO driver
  engine/
    harness/            the engine tests' harness (no Qt)
    test_*.cpp          the engine's tests: engine_tests
  intelligence/
    Sounds.h            drum hits, tones and loops made from formulas, written as WAV files
    test_*.cpp          the intelligence module's tests: intelligence_tests (the engine tests' harness)
  app/
    support/            what the application layer's and the UI's tests share (sub_app_test_support)
    test_*.cpp          a Qt Test program each; test_ui_* link the UI too
  vst3_plugins/         the test VST3 plug-ins (C++)
  asio_driver/          the fake ASIO driver (C++; Windows, with the ASIO SDK)
```

Sources are globbed: a new `tests/engine/test_*.cpp` joins `engine_tests`, a new `tests/intelligence/test_*.cpp`
joins `intelligence_tests`, and a new `tests/app/test_*.cpp` is a new program, on the next configure (`ninja` checks
the globs).

### tests/engine/harness

The engine's tests link `sub_engine` and nothing else, which also shows that the engine stands on its own.

| File | What it holds |
|---|---|
| [Test.h](../tests/engine/harness/Test.h), [Main.cpp](../tests/engine/harness/Main.cpp) | `TEST_CASE("name") { ... }`; `CHECK` (records a failure, goes on), `REQUIRE` (ends the test), `CHECK_EQ`, `CHECK_NEAR` (absolute), `CHECK_APPROX` (relative 1e-6), `CHECK_APPROX_REL`, `CHECK_APPROX_TOL`, `CHECK_THROWS_AS`, `CHECK_THROWS_MATCHING` (the message searched with a regular expression), `CHECK_NOTHROW`; `INFO(text)` adds what a failure in its scope was about (a seed, a parameter); `SKIP(reason)`; `tempDir()`, a fresh folder for the running test, removed after it. `Main.cpp` runs them (filters, `--list`). |
| [Fixtures.h](../tests/engine/harness/Fixtures.h) | `kSampleRate` (48000: the engine's rate without a device), `kSpb`, `kBeat`; WAV files to play (`writeWav`, `makeWav`, `dcWav`: a second of 0.5 in both channels; `rampWav`: sample i is (i % 32768) / 32768, exact in 16-bit PCM); the test plug-ins (`testPluginsBundle`, `requireTestPlugins`, `testPluginUids`, `addTestPlugin`); `clip`, `clipTrack`, `setParam`, `paramInfo`, `utilityOn`, `builtinInfo`. |
| [Signal.h](../tests/engine/harness/Signal.h) | What renders are checked with: channels and ranges of interleaved audio, peak and RMS levels, where a signal is non-zero, comparisons sample by sample, spectra of any length, envelopes, correlation, random numbers, reading WAV files back. |
| [AsioDriver.h](../tests/engine/harness/AsioDriver.h) | The fake ASIO driver: `AsioDriver` (its hooks, below; making one skips the test where there is no driver), the sample types and `decode()` (a driver buffer's bytes as samples, decoded independently of the engine), `asioConfig`, `openAsio`, `haveTestAsio`, `requireTestAsio`. |
| [Recording.h](../tests/engine/harness/Recording.h) | `RecordingTest`: the driver in manual mode with float samples (a loopback carries them exactly) and an engine without clip fades, which must have let go of the driver when it closes; with `onWorkers`, four render threads and seven silent tracks beside the test's, so every buffer's tracks are shared out. `output()`, `readTake()`. |
| [MidiInputTest.h](../tests/engine/harness/MidiInputTest.h) | SUB Test Synth in its DC mode (`dcSynth`), MIDI messages sent to play at a known offset into the next buffer (`send`), what the next buffers play (`heard`), what held notes should play (`held`). |
| [LiveTests.h](../tests/engine/harness/LiveTests.h) | The live tests (recording, MIDI input, resampling) that `test_parallel_live.cpp` runs again with workers; each is defined beside its own `TEST_CASE`. |
| [TaskGraphOrder.h](../tests/engine/harness/TaskGraphOrder.h) | `taskGraphOrder()`: the scheduler's queue order and ranks for a graph given by hand. |

### tests/app/support

The application layer's tests link `sub_app` and `sub_app_test_support` (the `.cpp` files here, a static library);
the UI's tests (`test_ui_*`) link `sub_ui` too, and use the header-only helpers, since the library doesn't link Qt
Quick.

| File | What it holds |
|---|---|
| [TestSupport.h](../tests/app/support/TestSupport.h) | `prepareApplication()`, called first (in `initTestCase`): the organization and application names are "SUBstation Tests", so tests never touch the user's settings; each program keeps its settings (INI) in a temporary folder of its own, cleared at start, so programs running side by side don't clear or change each other's; and the folders the application keeps things in are temporary ones for the run (`SUBSTATION_PRESETS`, `SUBSTATION_LIBRARY`, `SUBSTATION_BROWSER_INDEX`, `SUBSTATION_SOUND_INDEX`, `SUBSTATION_PLUGIN_CACHE`, `SUBSTATION_RECORDINGS`). `ScopedEnv` (an environment variable for as long as it lives), `TempDir`, `writeWav` (16-bit PCM, rounded half to even), `makeTrack`, `makeDevice`, `kSampleRate`. |
| [EditorFixture.h](../tests/app/support/EditorFixture.h) | `EditorFixture`: a project, its undo stack and a `ProjectEditor` on them, with what the editor refused collected (`messages`); `env()` (an envelope from points), `audioClip()`. |
| [BridgeTestSupport.h](../tests/app/support/BridgeTestSupport.h) | `Studio`: a project, its undo stack, an engine without a device and the bridge between them, shut down when it goes; `Edits`: the editor's edits made as the editor makes them (the model's commands, in the same order); the test plug-ins as `PluginRef`s and `PluginInfo`s; `BridgeTestAccess`, the bridge's friend: plug-in reports injected as if the plug-ins had sent them, the plug-in loading timer stopped and stepped by hand, the meters polled, the bridge made busy. |
| [SessionFixture.h](../tests/app/support/SessionFixture.h) | `SessionFixture`: an engine without a device (clips without fades, unless asked for) and a `Session` on it that scans no plug-ins, keeps no browser index and analyses no sounds in the background (its browser lists the user's Music folder); what it says (`messages`, `warnings`, `informations`) collected; `waitForRender`, `waitForSource`, `render` (offline), `level`, `clipTrack`. |
| [UiTestSupport.h](../tests/app/support/UiTestSupport.h) | `UiSession`: a session on a fresh engine, registered as the QML `Session` singleton, and a QML engine set up for the UI's module (analysing the browser's sounds only if asked: `UiSession(true)`, for a test with places of its own); `show()` loads a window of QML and waits until it is exposed and active. Mouse, wheel and key input as a user sends it (`press`, `moveTo`, `release`, `click`, `doubleClick`, `drag`, `wheel`), `haveDisplay()`, `screenshot()`. |
| [ArrangementTestSupport.h](../tests/app/support/ArrangementTestSupport.h) | The arrangement view in a window on a `UiSession`; its items found by name, points in its lanes and headers, the menus its items work out, audio files to put in it. |
| [DevicePanelTestSupport.h](../tests/app/support/DevicePanelTestSupport.h) | The device panel in a window as the main window places it; its parts found by object name or by device; its menus read and chosen from; drags from the browser (or along the chain) delivered as the platform delivers them. |
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
| [test_compressor_engine.cpp](../tests/engine/test_compressor_engine.cpp) | The Compressor: its gain curve, its displays, keying from a sidechain. |
| [test_delay_engine.cpp](../tests/engine/test_delay_engine.cpp) | The Delay: synced and free times, offset, link, feedback, ping pong, freeze, the filter, the modes, its display. |
| [test_eq_engine.cpp](../tests/engine/test_eq_engine.cpp) | The EQ: each band plays as its curve shows, the curves against the analog filters, placement (mid, side), output gain and gain scale, extremes, the displays. |
| [test_sampler_engine.cpp](../tests/engine/test_sampler_engine.cpp) | The Sampler: its sample (its state), pitch from key, root and tuning at any file rate, start, end and loop, velocity, a missing file, swapping the sample while it plays; as Simpler: transients and slices, Slice (by region, beat, transient; Mono, Poly, Thru), 1-Shot (Trigger, Gate, fades), Classic's loop start and crossfade, reverse, snap, gain, pan, the filter (and its ringing out), the LFO, voices and glide, warping (resampled and stretched), repeatable renders. |
| [test_sidechain_device_engine.cpp](../tests/engine/test_sidechain_device_engine.cpp) | The Sidechain device: hits to the sample, the curve, depth, smoothing, threshold and re-arming, lookahead as latency, Lows Only, hits on the beat, its displays. |
| [test_groups_engine.cpp](../tests/engine/test_groups_engine.cpp) | Group buses, delay compensation at every summing point, solo and mute across levels. |
| [test_sends_engine.cpp](../tests/engine/test_sends_engine.cpp) | Sends before and after the fader, delay compensation per edge, cycles refused, solo and mute across sends. |
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
folder, since its `Signal.h` would be found for `<signal.h>` where file names ignore case). The spectrum as the fingerprint
reads Signalsmith Linear's FFT; decoding; what each aspect of a fingerprint tells apart, on drum hits and tones made from formulas; comparing; the
store; the index's threads, saving and checking stamps, searches; harmony: the key, chords from notes (sevenths,
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
| [test_bridge_settings.cpp](../tests/app/test_bridge_settings.cpp) | The audio and MIDI preferences' settings, where the bridge puts its files and how it names them, the float WAV writer, the EQ's curve, device status, the scope, MIDI inputs. |

### The session

| File | What it covers |
|---|---|
| [test_selection.cpp](../tests/app/test_selection.cpp) | Selecting tracks (Ctrl and Shift), clip and lane ranges (and the rows they show over), breakpoints, the insert marker, what is left of a selection after the project changes. |
| [test_session_edit.cpp](../tests/app/test_session_edit.cpp) | The Edit and Create commands as the menus and keys drive them: split, select all, duplicate and delete, cut, copy and paste of clips, automation and tracks (one clipboard), groups, returns, reversing (at once and in the background), deactivating clips (0) and what the engine then plays. |
| [test_session_devices.cpp](../tests/app/test_session_devices.cpp) | The device view's selection and clipboard, folding, racks, drops; presets saved, loaded as new devices or into devices, default presets, renaming, the preset index watching the library. |
| [test_session_files.cpp](../tests/app/test_session_files.cpp) | Saving and opening, the title, the recent projects, Export Audio's choices, count-in and record quantization, the preferences. |
| [test_session_renders.cpp](../tests/app/test_session_renders.cpp) | Exporting and freezing in the background, their progress and Cancel, closing while one runs, freezing and flattening the selected tracks, a project's plug-ins loading after it opens. |
| [test_session_engine.cpp](../tests/app/test_session_engine.cpp) | What the engine hears of the editor's edits: groups, returns and sends, inputs, sidechains, racks and macros, presets rendering as the devices they were saved from, frozen tracks, MIDI clips' notes. |
| [test_session_keyboard.cpp](../tests/app/test_session_keyboard.cpp) | The computer MIDI keyboard: the letter keys, octaves, keys kept from shortcuts and text inputs, notes released. |

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
| [test_ui_device_panel.cpp](../tests/app/test_ui_device_panel.cpp), [test_ui_device_panel_plugins.cpp](../tests/app/test_ui_device_panel_plugins.cpp), [test_ui_device_panel_presets.cpp](../tests/app/test_ui_device_panel_presets.cpp), [test_ui_device_panel_racks.cpp](../tests/app/test_ui_device_panel_racks.cpp), [test_ui_device_panel_sidechain.cpp](../tests/app/test_ui_device_panel_sidechain.cpp) | The device view: folding, the clipboard, drags and drops, menus; plug-in devices and their parameters; presets; racks, chains and macros; the sidechain button and its menu. |
| [test_ui_device_editors.cpp](../tests/app/test_ui_device_editors.cpp) | The built-in devices' own editors (Compressor, Delay, EQ, Sampler: its pages, modes, slices, loop marker, warp and reverse; Sidechain) and the shared parameter cell, against the project and offline renders. |

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
- Shared helpers go into `tests/app/support` (header-only if they need Qt Quick), not into a test file.
- A new test plug-in goes into `tests/vst3_plugins` and the `sub_test_plugins` target in
  [TestPlugins.cmake](../tests/TestPlugins.cmake); a new driver hook into `test_asio_driver.cpp`, its `.def` and
  `AsioDriver.h`.

## Benchmarks

[benchmarks/](../benchmarks) holds the renderer's and the browser backend's benchmarks and their results:
`parallel_render_bench` (render times on 1..N threads, offline and live through the fake ASIO driver, checked
bit-identical; `--heavy N --compare-ordering` measures starting heavy tracks first), `browser_backend_bench` (the
backend on a large synthetic library, every query checked against `BrowserReference`) and `sound_similarity_bench`
(sound similarity on a real sample library: speed, and how often a one-shot's nearest sounds are of its kind). They are built with
`-DSUBSTATION_BUILD_BENCHMARKS=ON`, into `build/bin`, and run by hand; CTest doesn't run them. How to run them, what
they measure and the results are in [benchmarks/README.md](../benchmarks/README.md).
