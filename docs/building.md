# Building

SUBstation is a C++20 program built with CMake: the real-time audio engine, the browser's backend and the
intelligence module are libraries with no Qt in them, the application layer is Qt Core and Qt Gui, and the UI is Qt Quick (QML).
[CMakeLists.txt](../CMakeLists.txt) and the `CMakeLists.txt` of each layer hold the whole build; what the layers
are is in [architecture.md](architecture.md).

## Requirements

- **Windows 10/11** is the target: WASAPI and ASIO audio, WinMM MIDI input, VST3 plug-ins with their editors. It
  also builds and runs on **Linux** (the tests run there in CI-like containers): audio through the system's default
  backend (PulseAudio, ALSA, JACK... via miniaudio, as the *System* driver), VST3 plug-ins without their editors, no
  MIDI devices (the computer MIDI keyboard still plays).
- A C++20 compiler: Visual Studio 2022 or newer (*Desktop development with C++*), MinGW-w64 GCC 13 or newer on
  Windows (the one Qt's installer ships), or GCC 13 / Clang 16 or newer.
- CMake 3.26 or newer, and Ninja (recommended; Visual Studio's generator works too).
- Qt 6.4 or newer (6.5+ recommended on Windows): Core, Gui, Qml, Quick, QuickControls2, and Test and QuickTest for
  the tests.
  - Windows: Qt's online installer, the *MSVC 2022 64-bit* build of a Qt 6 release (or MSVC 2019's, which works
    with VS 2022), or its *MinGW 64-bit* build with the matching MinGW toolchain.
  - Debian/Ubuntu: `qt6-base-dev qt6-declarative-dev qml6-module-qtquick qml6-module-qtquick-controls
    qml6-module-qtquick-layouts qml6-module-qtquick-window qml6-module-qtquick-templates
    qml6-module-qtquick-dialogs qml6-module-qtqml-workerscript qml6-module-qt-labs-settings
    qml6-module-qtquick-shapes libgl-dev libxkbcommon-dev` (and `xvfb` to run the UI's tests headless).
- On x86-64, a CPU with AVX2 (Intel Haswell, AMD Zen or later) to run it: the engine is compiled with AVX2
  (`/arch:AVX2` with MSVC, `-mavx2 -mfma` with GCC and Clang; with MinGW also `-Wa,-muse-unaligned-vector-move`, as
  GCC can't align the stack for AVX on 64-bit Windows). On arm64 the engine builds with the CPU's baseline (NEON),
  no flags. Not with fast math (`/fp:fast`, `-ffast-math`): the results mustn't
  depend on the compiler reordering arithmetic or assuming there is no NaN or infinity. Denormals are flushed to
  zero where audio renders (`ScopedNoDenormals`), not by a compiler flag.
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

Windows with Qt's MinGW build instead (the *MinGW 64-bit* Qt and the MinGW toolchain that Qt's installer ships
with it, under *Developer and Designer Tools*), in the *Qt 6.x (MinGW 64-bit)* command prompt from the Start menu
(it puts Qt's and MinGW's `bin` folders on `PATH`; CMake and Ninja can come from the same installer, in `C:\Qt\Tools\CMake_64\bin` and `C:\Qt\Tools\Ninja`):

```bat
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.8.0\mingw_64 -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++
cmake --build build
C:\Qt\6.8.0\mingw_64\bin\windeployqt.exe --qmldir ui\qml build\bin\substation.exe
build\bin\substation.exe
```

`CMAKE_PREFIX_PATH` points CMake at Qt (the folder of a Qt 6 MSVC or MinGW build). `windeployqt` copies Qt's
libraries and the QML modules the UI imports next to the executable (and, with MinGW, its runtime DLLs, which the
plug-in scanner `substation-scan` needs too); without it, run from a prompt with Qt's `bin` folder (and MinGW's) on
`PATH` instead. Use the compiler the Qt build was made with: MSVC for an `msvc*` Qt, Qt's own MinGW for a `mingw_64`
one (another GCC may not match its C++ runtime). CI builds and tests both. The build is
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
| `miniaudio` | static library (C) | engine | [miniaudio](../engine/third_party/miniaudio), compiled once as C (`MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_GENERATION`). The engine's and the intelligence module's decoders. |
| `signalsmith_linear` | interface library | engine | [Signalsmith Linear](../engine/third_party/signalsmith-linear)'s headers: the FFTs of Signalsmith Stretch. |
| `vst3_base`, `vst3_hosting` | static libraries | engine | The VST 3 SDK's interfaces and base library, and its host side (module loading: `module_win32.cpp` or `module_linux.cpp`). `vst3_hosting` is C++17 on purpose: as C++17 the module loader reads paths as UTF-8 (`u8path`); as C++20 it would use the ANSI code page on Windows, and plug-ins with non-ASCII paths would not load. |
| `mp3lame` | static library (C) | engine | [LAME](../engine/third_party/lame)'s encoding library (`libmp3lame`, its SSE quantizer on x86-64), with a `config.h` of ours; MP3 export (`Mp3Writer.cpp`). |
| `sub_platform` | static library | platform | What every layer needs from the operating system ([platform/](../platform/CMakeLists.txt), [platform.md](platform.md)): paths and their keys, files, binary fields, threads' priorities; no Qt, nothing of the other layers. `PlatformWin32.cpp` on Windows, `PlatformPosix.cpp` elsewhere. |
| `sub_engine` | static library | engine | The real-time engine ([engine/](../engine/CMakeLists.txt)): no Qt; on `sub_platform`. Each system's audio drivers, MIDI inputs, plug-in editor windows and VST3 folders are files of their own ([engine/README.md](engine/README.md#platforms)): on Windows WASAPI and, with the SDK, ASIO, WinMM MIDI, Win32 editor windows; elsewhere miniaudio's "System", a MIDI backend without devices and no editor windows. |
| `sub_browser` | static library | browser | The browser's file index and search ([browser/](../browser/CMakeLists.txt)): no Qt; `sub_platform`, and a Win32 or POSIX layer of its own for listing and watching folders (inotify's watcher on Linux). |
| `essentia` | static library | intelligence | [Essentia](../intelligence/third_party/essentia) 2.1-beta5's core and the 29 algorithms the sound similarity uses, with KISS FFT: no other dependency. Built as C++17, its warnings not shown, with `ESSENTIA_STATIC` and `DEBUGGING_ENABLED=0` (public: they shape its headers). AGPLv3: [licensing.md](licensing.md). |
| `humanbro` | static library | intelligence | [HUMANBRO](../intelligence/third_party/humanbro)'s C++ runtime: MIDI features and the tree ensemble that predicts velocities. Built with its own strict floating-point flags (`/fp:precise`; `-ffp-contract=off -fno-fast-math`). |
| `sub_intelligence` | static library | intelligence | Sound similarity, harmony, humanizing, and later more ([intelligence/](../intelligence/CMakeLists.txt)): no Qt, nothing of the engine or the browser; decodes through `miniaudio`, describes sounds with `essentia`, predicts velocities with `humanbro`; the operating system through `sub_platform`. Configuring copies its models ([intelligence/models](../intelligence/models)) into `bin/models`, beside the executables. |
| `substation-scan` | executable | tools | The VST3 scanner's child process ([tools/scanner](../tools/scanner/main.cpp)): links `sub_engine`, no Qt. |
| `sub_app` | static library | app | The application layer ([app/](../app/CMakeLists.txt)): Qt Core and Gui, `sub_engine`, `sub_browser`, `sub_intelligence`. Built with `QT_NO_KEYWORDS` (public): it and everything on it write `Q_SIGNALS`, `Q_SLOTS`, `Q_EMIT`. |
| `sub_ui`, `sub_uiplugin` | static library + its QML plugin | ui | The QML module `SUBstation` ([ui/](../ui/CMakeLists.txt)): the QML files, the C++ Qt Quick items, the icons; and `SUBstation.Style`, the Qt Quick Controls style (`ui/style`). |
| `substation` | executable | ui | [ui/main.cpp](../ui/main.cpp): makes the engine, the application's session on it, and the UI on that. |
| `sub_test_plugins`, `sub_test_asio` | modules | tests | The test VST3 bundle and the fake ASIO driver ([testing.md](testing.md)). |
| `engine_tests`, `intelligence_tests`, `test_*` | executables | tests | The tests. |

With MSVC the project's own code builds with `/W4 /permissive- /utf-8 /Zc:__cplusplus`, with GCC and Clang with
`-Wall -Wextra` ([cmake/Warnings.cmake](../cmake/Warnings.cmake)); third-party code builds without warnings.

## The layers' boundaries

`ctest -R boundaries` runs [cmake/CheckBoundaries.cmake](../cmake/CheckBoundaries.cmake), which fails if:

- the platform layer (`platform/src`), the engine (`engine/src`), the browser backend (`browser/src`) or the
  intelligence module (`intelligence/src`) includes anything of Qt, the application layer or the UI;
- the platform layer includes anything of the layers on it (the engine, the browser backend, the intelligence module);
- the intelligence module includes the engine's headers or the browser backend's;
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
| [miniaudio](../engine/third_party/miniaudio) | 0.11.25 | public domain (Unlicense) or MIT No Attribution | WASAPI and the other systems' audio; decoding (the engine's and the intelligence module's) |
| [Signalsmith Stretch](../engine/third_party/signalsmith-stretch) | 1.3.2 | MIT | time stretching and pitch shifting (header-only) |
| [Signalsmith Linear](../engine/third_party/signalsmith-linear) | 0.6.4 | MIT | its FFT (`stft.h`, `fft.h` only): Signalsmith Stretch's |
| [VST 3 SDK](../engine/third_party/vst3sdk) | 3.8.1 | MIT (since SDK 3.8) | `pluginterfaces`, `base`, `public.sdk/source/{common,main}` and `public.sdk/source/vst`, with the Windows and Linux module loaders; without VSTGUI, the SDK's tests and the wrappers |
| [LAME](../engine/third_party/lame) | 3.100 | LGPL-2.0-or-later | its encoding library (`include/lame.h`, `libmp3lame/` without the decoder's glue): MP3 export |

In [app/third_party](../app/third_party): [puff](../app/third_party/puff) 2.3 (zlib licence), from zlib 1.3.1's
`contrib/puff`: inflate, for reading Ableton Live Sets (target `puff`, linked into `sub_app`).

Each folder has its licence and a `VERSION.txt` saying what was taken and that it is unmodified. Qt is not vendored:
it is LGPL-3.0 (or commercial), linked dynamically. SUBstation itself is MIT ([LICENSE](../LICENSE)); what the
licences together mean for distributing it is in [licensing.md](licensing.md).

And in [intelligence/third_party](../intelligence/third_party):

| Library | Version | Licence | What is used |
|---|---|---|---|
| [Essentia](../intelligence/third_party/essentia) | 2.1-beta5 (tag v2.1_beta5) | **AGPL-3.0-or-later**; its KISS FFT BSD-3-Clause | its core library and 29 algorithms (spectrum, MFCC, spectral shape, contrast, peaks, envelope, YIN): the sound similarity's descriptors. Copied by [vendor.sh](../intelligence/third_party/essentia/vendor.sh) from the tag, with five small portability changes (C++17 standard libraries, Clang, MSVC) ([local-changes.patch](../intelligence/third_party/essentia/local-changes.patch), [VERSION.txt](../intelligence/third_party/essentia/VERSION.txt)). Every build includes it: a build you distribute is under the AGPLv3's terms as a whole ([licensing.md](licensing.md)) |
| [HUMANBRO](../intelligence/third_party/humanbro) | copied 2026-10-08 | the project's own | its C++ runtime (`include/`, `src/`; not its CLI), with one local change its `VERSION.txt` describes (loading a model from memory); the model it runs is [intelligence/models/velocity.hbm](../intelligence/models/README.md) |

## Gotchas

- **"Could not find a configuration file for package Qt6 ... version: 6.x.y (64bit)".** The compiler CMake
  found makes 32-bit programs (often an old MinGW, such as `C:\MinGW\bin`, first on `PATH`), and Qt is 64-bit.
  `gcc -dumpmachine` must say `x86_64-w64-mingw32`. Put Qt's MinGW first on `PATH` (or give its `gcc.exe` and
  `g++.exe` as `CMAKE_C_COMPILER` and `CMAKE_CXX_COMPILER`), and delete the build folder before configuring again:
  CMake keeps the compiler it found. (The configure now stops earlier, saying so.)

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
