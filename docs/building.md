# Building

SUBstation is a C++20 program built with CMake: the real-time audio engine and the browser's backend are
libraries with no Qt in them, the application layer is Qt Core and Qt Gui, and the UI is Qt Quick (QML).
[CMakeLists.txt](../CMakeLists.txt) and the `CMakeLists.txt` of each layer hold the whole build; what the layers
are is in [architecture.md](architecture.md).

## Requirements

- **Windows 10/11** is the target: WASAPI and ASIO audio, WinMM MIDI input, VST3 plug-ins with their editors. It
  also builds and runs on **Linux** (the tests run there in CI-like containers): audio through the system's default
  backend (PulseAudio, ALSA, JACK... via miniaudio, as the *System* driver), VST3 plug-ins without their editors, no
  MIDI devices (the computer MIDI keyboard still plays).
- A C++20 compiler: Visual Studio 2022 or newer (*Desktop development with C++*), or GCC 13 / Clang 16 or newer.
- CMake 3.26 or newer, and Ninja (recommended; Visual Studio's generator works too).
- Qt 6.4 or newer (6.5+ recommended on Windows): Core, Gui, Qml, Quick, QuickControls2, and Test for the tests.
  - Windows: Qt's online installer, the *MSVC 2022 64-bit* build of a Qt 6 release (or MSVC 2019's, which works
    with VS 2022).
  - Debian/Ubuntu: `qt6-base-dev qt6-declarative-dev qml6-module-qtquick qml6-module-qtquick-controls
    qml6-module-qtquick-layouts qml6-module-qtquick-window qml6-module-qtquick-templates
    qml6-module-qtquick-dialogs qml6-module-qtqml-workerscript qml6-module-qt-labs-settings
    qml6-module-qtquick-shapes libgl-dev libxkbcommon-dev` (and `xvfb` to run the UI's tests headless).
- A CPU with AVX2 (Intel Haswell, AMD Zen or later) to run it: the engine is compiled with AVX2 and fast float
  math (`/arch:AVX2 /fp:fast` with MSVC, `-mavx2 -mfma -ffast-math` with GCC and Clang). The engine must not rely on
  NaN or infinity (fast math may drop checks for them); test for NaN on the bits instead.
- Optional: Steinberg's ASIO SDK, for ASIO (see [ASIO SDK](#asio-sdk)).

Nothing else needs installing: the engine's third-party code is vendored (see [Third-party code](#third-party-code)).

## Building

Linux:

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
ninja -C build
build/bin/substation                     # or: build/bin/substation path/to/song.gilproj
```

Windows, in a *Developer PowerShell for VS 2022* (so MSVC and Ninja are on the path):

```powershell
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.8.0\msvc2022_64
ninja -C build
C:\Qt\6.8.0\msvc2022_64\bin\windeployqt.exe --qmldir ui\qml build\bin\substation.exe   # once: Qt's DLLs and QML modules beside it
build\bin\substation.exe
```

`CMAKE_PREFIX_PATH` points CMake at Qt (any Qt 6 MSVC build folder). `windeployqt` copies Qt's libraries and the QML
modules the UI imports next to the executable; without it, run with Qt's `bin` folder on `PATH` instead. The build is
incremental: re-run `ninja -C build` after changing anything. Everything built lands in `build/bin`: `substation`,
`substation-scan` (the plug-in scanner, which the application starts from its own folder), the test executables,
and the test plug-ins under `build/testplugins`.

Sources are globbed per library (`file(GLOB_RECURSE ... CONFIGURE_DEPENDS)`): a new `.cpp`, `.h` or `.qml` under
`app/src`, `ui/src` or `ui/qml`, or a new built-in device in `engine/src/builtin/devices`, is picked up by the next
`ninja`, which re-checks the globs.

### Options

| Option | Default | What it does |
|---|---|---|
| `SUBSTATION_BUILD_APP` | `ON` | The application layer and the UI (needs Qt). `OFF` builds the engine, the browser backend, the scanner and the engine's tests only: no Qt needed. |
| `SUBSTATION_BUILD_TESTS` | `ON` | The tests ([testing.md](testing.md)). |
| `SUBSTATION_TEST_PLUGINS` | `ON` | The VST3 plug-ins (and, with the ASIO SDK, the fake ASIO driver) the tests use. |
| `SUBSTATION_ASIO` | `ON` | ASIO support, if the SDK is found (Windows). `OFF` doesn't look for it. |
| `SUBSTATION_ASIO_SDK` | `$ENV{SUBSTATION_ASIO_SDK}` | The ASIO SDK's folder (the one containing `common/iasiodrv.h`). |
| `SUBSTATION_BUILD_BENCHMARKS` | `OFF` | The benchmarks ([benchmarks/README.md](../benchmarks/README.md)). |

Pass them at configure time: `cmake -B build -DSUBSTATION_TEST_PLUGINS=OFF`.

## Targets

| Target | Kind | Layer | What it is |
|---|---|---|---|
| `miniaudio` | static library (C) | engine | [miniaudio](../engine/third_party/miniaudio), compiled once as C (`MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_GENERATION`). |
| `vst3_base`, `vst3_hosting` | static libraries | engine | The VST 3 SDK's interfaces and base library, and its host side (module loading: `module_win32.cpp` or `module_linux.cpp`). `vst3_hosting` is C++17 on purpose: as C++17 the module loader reads paths as UTF-8 (`u8path`); as C++20 it would use the ANSI code page on Windows, and plug-ins with non-ASCII paths would not load. |
| `sub_engine` | static library | engine | The real-time engine ([engine/](../engine/CMakeLists.txt)): no Qt. On Windows it adds the WinMM MIDI backend, the plug-in editor windows and, with the SDK, ASIO; elsewhere a MIDI backend without devices and no editor windows. |
| `sub_browser` | static library | browser | The browser's file index and search ([browser/](../browser/CMakeLists.txt)): no Qt; Win32 or POSIX platform layer. |
| `substation-scan` | executable | tools | The VST3 scanner's child process ([tools/scanner](../tools/scanner/main.cpp)): links `sub_engine`, no Qt. |
| `sub_app` | static library | app | The application layer ([app/](../app/CMakeLists.txt)): Qt Core and Gui, `sub_engine`, `sub_browser`. Built with `QT_NO_KEYWORDS` (public): it and everything on it write `Q_SIGNALS`, `Q_SLOTS`, `Q_EMIT`. |
| `sub_ui`, `sub_uiplugin` | static library + its QML plugin | ui | The QML module `SUBstation` ([ui/](../ui/CMakeLists.txt)): the QML files, the C++ Qt Quick items, the icons; and `SUBstation.Style`, the Qt Quick Controls style (`ui/style`). |
| `substation` | executable | ui | [ui/main.cpp](../ui/main.cpp): makes the engine, the application's session on it, and the UI on that. |
| `sub_test_plugins`, `sub_test_asio` | modules | tests | The test VST3 bundle and the fake ASIO driver ([testing.md](testing.md)). |
| `engine_tests`, `test_*` | executables | tests | The tests. |

With MSVC the project's own code builds with `/W4 /permissive- /utf-8 /Zc:__cplusplus`, with GCC and Clang with
`-Wall -Wextra` ([cmake/Warnings.cmake](../cmake/Warnings.cmake)); third-party code builds without warnings.

## The layers' boundaries

`ctest -R boundaries` runs [cmake/CheckBoundaries.cmake](../cmake/CheckBoundaries.cmake), which fails if:

- the engine (`engine/src`) or the browser backend (`browser/src`) includes anything of Qt, the application layer or
  the UI;
- the application layer (`app/src`) includes Qt Quick or QML (or the UI);
- the UI (`ui/src`) includes the engine's headers: it talks to the application layer only.

`ui/main.cpp` is where the layers are put together, so it alone makes the engine.

## ASIO SDK

ASIO needs Steinberg's ASIO SDK, which isn't in the repository: its licence doesn't allow passing it on. Download it
from <https://www.steinberg.net/asiosdk> and unzip it into the project folder as it comes (for example
`SUBstation\asiosdk_2.3.3_2019-06-14\common\...`). `.gitignore` keeps `asiosdk*`, `ASIOSDK*`, `ASIO-SDK*` and
`asio-sdk*` folders out of git. Only its headers are used.

Where the build looks, in this order:

1. `SUBSTATION_ASIO_SDK` (environment or CMake variable), if set. It must be the folder containing `common`; if it
   has no `common/iasiodrv.h` the configure stops with an error.
2. Otherwise any folder of the project folder, or one level deeper (some unzip tools add a level):
   `<project>/*/common/iasiodrv.h`, `<project>/*/*/common/iasiodrv.h`.
3. An `asio*` folder beside the project (or one level inside it).
4. An `asio*` folder in the root of the system drive (`%SystemDrive%\asio*`, or one level inside it).

If several are found, the one whose path sorts highest wins (the newest of several versions). The configure prints
`ASIO SDK: <folder>` when it found it; without it, it warns and builds the engine with WASAPI only (the preferences
then show ASIO greyed out, and the ASIO tests are skipped). Re-run CMake after adding the SDK.

What the engine does with ASIO is in [engine/audio-devices.md](engine/audio-devices.md).

## Third-party code

Vendored in [engine/third_party](../engine/third_party), so nothing else needs installing:

| Library | Version | Licence | What is used |
|---|---|---|---|
| [miniaudio](../engine/third_party/miniaudio) | 0.11.25 | public domain (Unlicense) or MIT No Attribution | WASAPI and the other systems' audio; decoding |
| [Signalsmith Stretch](../engine/third_party/signalsmith-stretch) | 1.3.2 | MIT | time stretching and pitch shifting (header-only) |
| [Signalsmith Linear](../engine/third_party/signalsmith-linear) | 0.6.4 | MIT | its FFT (`stft.h`, `fft.h` only) |
| [VST 3 SDK](../engine/third_party/vst3sdk) | 3.8.1 | MIT (since SDK 3.8) | `pluginterfaces`, `base`, `public.sdk/source/{common,main}` and `public.sdk/source/vst`, with the Windows and Linux module loaders; without VSTGUI, the SDK's tests and the wrappers |

Each folder has its licence and a `VERSION.txt` saying what was taken and that it is unmodified. Qt is not vendored:
it is LGPL-3.0 (or commercial), linked dynamically.

## Gotchas

- **A new header isn't moc'd.** With Qt 6.4, a header added to a build folder configured before it existed is
  sometimes skipped by AUTOMOC (link errors: undefined `vtable` or `staticMetaObject`). Delete the target's
  `build/<dir>/<target>_autogen/timestamp` (or configure a fresh build folder) and build again.
- **Raw string literals and moc.** moc (Qt 6.4) stops reading a file at a C++ raw string literal (`R"(...)"`): keep
  them out of files with `Q_OBJECT` classes (or after the class).
- **Qt's keywords.** `signals`, `slots`, `emit` and `foreach` are not defined (`QT_NO_KEYWORDS`): the engine uses
  `slots` as a name.
- **The engine as a static library.** The built-in devices register themselves from their own files, which nothing
  else refers to; each defines an anchor function that `BuiltinRegistry::instance()` calls (the build writes the
  list, `generated/BuiltinDevices.cpp`), so a program linking the engine keeps them all.
- **Linux without a sound server.** The *System* driver opens no device when there is none (miniaudio's null
  backend is left out on purpose: it "plays" faster than real time); the application then runs without audio and
  says so.
