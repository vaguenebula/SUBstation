# SUBstation

A basic DAW for Windows with an Ableton-style arrangement view. It is one C++20
program: a real-time audio engine (ASIO, or WASAPI through miniaudio; VST3
hosting) and a browser backend with no Qt in them, an application layer on Qt
Core and Qt Gui that holds the project, its editing and undo, and mirrors it into
the engine, and a Qt Quick (QML) UI whose arrangement, piano roll, envelopes and
meters are C++ scene-graph items. It also builds and runs on Linux.

## Features

- **Arrangement**: audio and MIDI tracks, group tracks (nesting, folding), return
  tracks and sends, a master with its own effects; track headers laid out as Ableton's,
  with an In/Out column routing each track as Ableton does (Audio From with Pre FX, Post
  FX or Post Mixer; Audio To its group, Main, another track's input or a device's
  sidechain, or Sends Only); clip editing with snapping
  (trimming, stretching, sliding a clip's content), clip fades with curves,
  deactivating clips (0, as in Ableton), time selections, undo/redo for every edit.
- **Audio clips**: real-time warping (Signalsmith Stretch) with several warp modes,
  transpose and detune; tempo and key read from sample file names.
- **MIDI**: a piano roll with legato, quantize and humanize tools (velocities from a
  machine-learning model trained on pianists' performances, timing); the song's chords
  and key worked out from its MIDI and shown over the notes (notes out of the key in
  red), and block chords or a bass line written from them; MIDI input from
  controllers and the computer keyboard.
- **Devices**: a built-in Synth, a Sampler after Ableton's Simpler (Classic, 1-Shot and
  Slice modes, slicing at transients, warping, a filter and an LFO), Compressor (with sidechain), Over The Top
  multiband compressor, Disperser (phase dispersion through up to 64 all-pass stages,
  its group delay drawn) and Utility; racks with parallel chains, macros and presets.
- **VST3 plug-ins**: instruments and effects, their own editors, presets, sidechains,
  latency compensation everywhere, crash-safe scanning.
- **Automation** of every parameter, played sample-accurately.
- **Recording** from ASIO inputs, with count-in and latency-corrected takes, and
  resampling of any track or the master.
- **Browser** with instant search over hundreds of thousands of samples, preview,
  and ranking by use.
- **Multi-threaded rendering**, bit-identical on any number of threads; WAV export.

See the [user guide](docs/guide/README.md) for how each of these behaves, and
[what isn't implemented yet](docs/guide/limitations.md).

## Setup

You need a C++20 compiler, CMake 3.26 or newer with Ninja, and Qt 6.4 or newer
(Core, Gui, Qml, Quick, QuickControls2, Test). A CPU with AVX2 runs it.

- **Windows 10/11**: Visual Studio 2022 or newer with the *Desktop development
  with C++* workload, and Qt's *MSVC 2022 64-bit* build of a Qt 6 release (6.5 or
  newer recommended), from Qt's online installer. Or Qt's *MinGW 64-bit* build
  with the MinGW toolchain the installer ships with it (see
  [docs/building.md](docs/building.md#building)).
- **Linux** (Debian/Ubuntu): GCC 13 or Clang 16, `cmake ninja-build`, and Qt's
  packages (`qt6-base-dev qt6-declarative-dev` and the QML modules the UI imports:
  the list is in [docs/building.md](docs/building.md#requirements)), plus `xvfb`
  to run the UI's tests headless. Audio goes through the system's default backend
  (the *System* driver); VST3 plug-ins load without their editors; there are no
  MIDI devices (the computer MIDI keyboard plays).

Linux:

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
ninja -C build
```

Windows, in a *Developer PowerShell for VS 2022*:

```powershell
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.8.0\msvc2022_64
ninja -C build
C:\Qt\6.8.0\msvc2022_64\bin\windeployqt.exe --qmldir ui\qml build\bin\substation.exe   # once
```

Re-run `ninja -C build` after changing anything (the build is incremental).

**ASIO** needs Steinberg's ASIO SDK, which isn't in the repository (its licence
doesn't allow passing it on). Download it from <https://www.steinberg.net/asiosdk>,
unzip it into the project folder as it comes, and configure again. Without it the
engine builds with WASAPI only.

More in [docs/building.md](docs/building.md): where the build looks for the ASIO
SDK, build options and targets, and the layers' boundaries.

## Run

```sh
build/bin/substation                  # or: build/bin/substation path/to/song.gilproj
```

(`build\bin\substation.exe` on Windows.) The first launch uses the system default
output device; change it in *Options → Preferences* ([audio setup](docs/guide/audio-setup.md)).
The browser starts with your Music folder as a Place. Add more with *Add Folder…*.

## Tests

```sh
ctest --test-dir build --output-on-failure
```

The tests need no sound card and see none of the installed plug-ins: the build
makes a few test VST3 plug-ins for them, and a fake ASIO driver when the ASIO SDK is
there (without it, the ASIO tests are skipped). Engine tests render offline; the
application layer's tests drive it without a window; the UI's tests drive the real
QML views (on Linux under `xvfb-run`). See [docs/testing.md](docs/testing.md).

## Project layout

```
engine/src/          the real-time audio engine and plug-in hosting (sub_engine; no Qt)
engine/third_party/  miniaudio (public domain / MIT-0), Signalsmith Stretch and Linear, the VST 3 SDK subset (MIT)
browser/src/         the browser's file index and search (sub_browser; no Qt)
app/src/             the application layer (sub_app; Qt Core and Gui): model/, editor/, io/, audio/,
                     session/, browser/, plugins/, analysis/
ui/                  the Qt Quick UI: qml/ (QML module SUBstation), src/ (C++ scene-graph items),
                     style/ (the controls' style), main.cpp (the substation executable)
tools/scanner/       substation-scan, the VST3 scanner's child process
tests/               engine/ (engine_tests), app/ (Qt Test), vst3_plugins/, asio_driver/
benchmarks/          the renderer's and the browser backend's benchmarks (not run by ctest)
cmake/               compiler warnings, the layers' boundary check
docs/                documentation
```

## Documentation

- [Architecture](docs/architecture.md): layers, threads, real-time rules
- [Building](docs/building.md) and [testing](docs/testing.md)
- [User guide](docs/guide/README.md) and [keyboard shortcuts](docs/guide/shortcuts.md)
- [Code reference](docs/README.md): the engine, the application layer, the UI, the browser

Note:
This project kinda started out as a meme, but I'm starting to think it's actually much better for my specific workflow than Ableton is. You may or may not find it a good replacement, but I know for certain a lot of issues that bothered me the most with Ableton (crashing on switching audio devices, slow project save and load times) are solved with this DAW. The end goal, however, is to re-invent the way I make music with a DAW. Right now, features are very limited to my specific workflow, but I will continue adding stuff that make it more versatile and suited for everyone's music production needs. 
