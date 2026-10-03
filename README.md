# SUBstation

A basic DAW for Windows with an Ableton-style arrangement view. The UI and
editing logic are Python (PySide6/Qt 6); the real-time audio engine is C++
(ASIO, or WASAPI through miniaudio), bound with nanobind.

## Features

- **Arrangement**: audio and MIDI tracks, group tracks (nesting, folding), return
  tracks and sends, a master with its own effects; clip editing with snapping,
  time selections, undo/redo for every edit.
- **Audio clips**: real-time warping (Signalsmith Stretch) with several warp modes,
  transpose and detune; tempo and key read from sample file names.
- **MIDI**: a piano roll with legato, quantize and humanize tools; MIDI input from
  controllers and the computer keyboard.
- **Devices**: a built-in Synth, Sampler, Compressor (with sidechain), Over The Top
  multiband compressor and Utility; racks with parallel chains, macros and presets.
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

Requirements: Windows 10/11, Python 3.12, and Visual Studio 2022 or newer with
the *Desktop development with C++* workload. CMake and Ninja are installed into
the venv from PyPI.

Always activate the venv before installing anything:

```powershell
Set-ExecutionPolicy -Scope Process Bypass    # only if activation scripts are blocked
.\venv\Scripts\Activate.ps1
python -m pip install scikit-build-core nanobind cmake ninja pytest ruff PySide6 numpy
python -m pip install --no-build-isolation -e .    # compiles substation._engine and substation._browser
```

Re-run the last command after changing any C++ code (the build is incremental, under
`build/`). Python changes need no reinstall.

**ASIO** needs Steinberg's ASIO SDK, which isn't in the repository (its licence
doesn't allow passing it on). Download it from <https://www.steinberg.net/asiosdk>
and unzip it into the project folder as it comes, then re-run the install command.
Without it the engine builds with WASAPI only.

More in [docs/building.md](docs/building.md): where the build looks for the ASIO
SDK, build options, choosing a Visual Studio version, and the engine API version.

## Run

```powershell
.\venv\Scripts\Activate.ps1
python -m substation                  # or: python -m substation path\to\song.gilproj
```

The first launch uses the system default output device; change it in
*Options → Preferences* ([audio setup](docs/guide/audio-setup.md)). The browser
starts with your Music folder as a Place. Add more with *Add Folder…*.

## Tests

```powershell
.\venv\Scripts\Activate.ps1
python -m pytest
```

The tests need no sound card and see none of the installed plug-ins: the build
makes a fake ASIO driver and a few test VST3 plug-ins for them. Engine tests render
offline; UI tests drive the real main window offscreen. See
[docs/testing.md](docs/testing.md).

## Project layout

```
src/substation/     Python: UI (ui/), project model and undo (model/), engine bridge (audio/), plug-in scanner (plugins/)
engine/src/         C++: the real-time audio engine and plug-in hosting (module substation._engine)
browser/src/        C++: the browser's file index and search (module substation._browser)
engine/third_party/ miniaudio (public domain / MIT-0), Signalsmith Stretch, the VST 3 SDK subset (MIT)
tests/              pytest suite, the fake ASIO driver and the test VST3 plug-ins
benchmarks/         browser and renderer benchmarks (not run by pytest)
docs/               documentation
```

## Documentation

- [Architecture](docs/architecture.md): layers, threads, real-time rules
- [User guide](docs/guide/README.md) and [keyboard shortcuts](docs/guide/shortcuts.md)
- [Code reference](docs/README.md): the engine, the model, the UI, the browser
