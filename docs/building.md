# Building

SUBstation is a Python package (`src/substation`) with two compiled modules, `substation._engine` (the audio engine,
[engine/src](../engine/src)) and `substation._browser` (the browser's backend, [browser/src](../browser/src)). They
are built by CMake through scikit-build-core when the package is installed; [CMakeLists.txt](../CMakeLists.txt) and
[pyproject.toml](../pyproject.toml) hold the whole build.

## Requirements

- Windows 10/11. CMake warns on other platforms: SUBstation targets Windows (WASAPI, ASIO) and nothing else is tested.
- Python 3.12 (`find_package(Python 3.12 ...)`; `requires-python = ">=3.12"`).
- Visual Studio 2022 or newer with the *Desktop development with C++* workload. The code is C++20.
- CMake (3.26 or newer) and Ninja are installed into the venv from PyPI; nothing else needs installing system-wide.
- Optional: Steinberg's ASIO SDK, for ASIO support (see [ASIO SDK](#asio-sdk)).

## Setting up

Always activate the venv before installing anything:

```powershell
py -3.12 -m venv venv                         # once
Set-ExecutionPolicy -Scope Process Bypass    # only if activation scripts are blocked
.\venv\Scripts\Activate.ps1
python -m pip install scikit-build-core nanobind cmake ninja pytest ruff PySide6 numpy
python -m pip install --no-build-isolation -e .    # compiles substation._engine and substation._browser
```

Then run it:

```powershell
python -m substation                  # or: python -m substation path\to\song.gilproj
```

`pyproject.toml` also declares a GUI script, `substation` (`substation.app:main`). Start-up is described in
[python/engine-bridge.md](python/engine-bridge.md).

### Why `--no-build-isolation`

Without it pip would build in a fresh, temporary environment, downloading scikit-build-core and nanobind each time
and configuring from scratch. With it the build uses the venv's own scikit-build-core, nanobind and CMake (which is
why they are installed first), and the build folder is reused, so rebuilds are incremental.

### Rebuilding

Re-run the last command after changing any C++ code (in `engine/src`, `browser/src`, or the test plug-ins and
driver). The build is incremental, in `build/` (`build-dir = "build/{wheel_tag}"`, so for example
`build\cp312-cp312-win_amd64`). Python changes need no reinstall: the editable install imports the Python files from
`src/substation` directly.

The compiled files are installed into the venv's `site-packages\substation\` and found through the editable
install's import hook, not next to the sources: `_engine*.pyd`, `_browser*.pyd`, `_testplugins\` and
`_testdrivers\`. The tests find the test plug-ins and driver through `Path(_engine.__file__).parent`.

A new built-in device (a new `.cpp` in `engine/src/builtin/devices/`) is picked up on the next configure: the sources
are globbed with `CONFIGURE_DEPENDS`, so re-running the install command is enough.

## pyproject.toml

| Section | What it says |
|---|---|
| `[build-system]` | `scikit-build-core>=0.10` and `nanobind>=2.0`; backend `scikit_build_core.build`. |
| `[project]` | `substation` 0.1.0, Python >= 3.12; runtime dependencies `PySide6>=6.7`, `numpy>=2.0`. |
| `[project.optional-dependencies]` | `dev = ["pytest>=8", "ruff"]`. |
| `[project.gui-scripts]` | `substation = "substation.app:main"`. |
| `[tool.scikit-build]` | `minimum-version = "build-system.requires"`, `cmake.version = ">=3.26"`, `cmake.build-type = "Release"`, `build-dir = "build/{wheel_tag}"`, `wheel.packages = ["src/substation"]`. |
| `[tool.pytest.ini_options]` | `testpaths = ["tests"]`. |

The build is always Release. CMake options are passed through pip with `-C cmake.define.NAME=VALUE`, for example:

```powershell
python -m pip install --no-build-isolation -e . -C cmake.define.SUBSTATION_TEST_PLUGINS=OFF
```

## Choosing a Visual Studio generator

On Windows scikit-build-core configures with a Visual Studio generator, the newest one installed, by default. If the
newest Visual Studio causes trouble, pick another one before building:

```powershell
$env:CMAKE_GENERATOR="Visual Studio 17 2022"
python -m pip install --no-build-isolation -e .
```

CMake refuses to reconfigure a build folder with another generator than the one it was made with; delete the
`build\<wheel tag>` folder after switching.

## CMakeLists.txt

### Options and cache variables

| Name | Default | What it does |
|---|---|---|
| `SUBSTATION_TEST_PLUGINS` | `ON` | Build the VST3 plug-ins and the ASIO driver the tests use, installed next to the engine. |
| `SUBSTATION_ASIO` | `ON` | Build ASIO support (if the SDK is found). `OFF` skips looking for the SDK: WASAPI only. |
| `SUBSTATION_ASIO_SDK` | `$ENV{SUBSTATION_ASIO_SDK}` | The ASIO SDK's folder (the one containing `common/iasiodrv.h`). Set as an environment variable before building, or with `-C cmake.define.SUBSTATION_ASIO_SDK=...`. |

### Targets

| Target | Kind | What it is |
|---|---|---|
| `miniaudio` | static library (C) | [miniaudio](../engine/third_party/miniaudio), compiled once as C in its own translation unit ([engine/src/miniaudio_impl.c](../engine/src/miniaudio_impl.c)), with `MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_GENERATION`; warnings off. |
| `vst3_base` | static library | The VST 3 SDK's interfaces, base library and helpers both sides use (made by the `sub_vst3_sdk_library()` function: SDK include path, `UNICODE`, `NOMINMAX`, warnings off). |
| `vst3_hosting` | static library | The host side of the SDK: loading modules, host context, component/controller connection. Links `ole32 shell32`. Compiled as **C++17** on purpose: as C++17 the module loader reads paths as UTF-8 (`u8path`); as C++20 it would use the ANSI code page, and plug-ins with non-ASCII paths would not load. |
| `_engine` | nanobind module (`NB_STATIC`) | The engine: `engine/src/*.cpp` (the `Engine*.cpp` split files, `AudioDevice`, `WasapiBackend`, `AudioSource`, `Renderer`, `Scheduler`, `Recorder`, `Metronome`, `MidiInput`, `Warp`), the built-in devices (`builtin/` and every `builtin/devices/*.cpp`), the plug-in host (`plugins/EditorWindow`, `Vst3Format`, `Vst3Processor`) and `bindings.cpp`. Includes Signalsmith Stretch and Signalsmith Linear (header-only). Links `miniaudio vst3_hosting user32 ole32 uuid advapi32 winmm avrt`. With the ASIO SDK it adds `backends/AsioBackend.cpp`, the SDK's `common` folder and `SUBSTATION_HAS_ASIO=1`. |
| `sub_browser` | static library | The browser backend (`browser/src`: Browser, Indexer, Model, Platform, Search, Text), with `UNICODE`. |
| `_browser` | nanobind module (`NB_STATIC`) | `browser/src/bindings.cpp`, linking `sub_browser`. |
| `vst3_plugin_sdk` | static library | The plug-in side of the SDK, for the test plug-ins (with `PROJECT_INCLUDES_VSTEDITCONTROLLER=1`: `vsteditcontroller.cpp` is compiled on its own, not inside `vstsinglecomponenteffect.cpp`). Only with `SUBSTATION_TEST_PLUGINS`. |
| `sub_test_plugins` | module | [tests/vst3_plugins](../tests/vst3_plugins) and the SDK's `dllmain.cpp`, output as `SUBTestPlugins.vst3`, installed to `substation/_testplugins/SUBTestPlugins.vst3/Contents/x86_64-win`. Only with `SUBSTATION_TEST_PLUGINS`. |
| `sub_test_asio` | module | [tests/asio_driver](../tests/asio_driver) (`test_asio_driver.cpp` and its `.def`), output as `SUBTestAsio.dll`, installed to `substation/_testdrivers`. Only with `SUBSTATION_TEST_PLUGINS` **and** the ASIO SDK. |

`_engine` and `_browser` are installed to `substation`. With MSVC the project's own code builds with
`/W4 /permissive- /utf-8 /Zc:__cplusplus` (the test plug-ins and driver with `/W3`); the engine also defines
`NOMINMAX WIN32_LEAN_AND_MEAN _USE_MATH_DEFINES`.

nanobind is found by asking the venv's Python (`python -m nanobind --cmake_dir`), so it has to be installed there.

## The two native modules

| Module | Sources | Docs |
|---|---|---|
| `substation._engine` | [engine/src](../engine/src), bound in [engine/src/bindings.cpp](../engine/src/bindings.cpp) | [engine/README.md](engine/README.md) |
| `substation._browser` | [browser/src](../browser/src), bound in [browser/src/bindings.cpp](../browser/src/bindings.cpp) | [browser.md](browser.md) |

They are independent: the browser shares no code, locks or threads with the engine, and is built from its own
static library. One install command builds both.

`browser/src/UnicodeTables.inc` is generated, and checked in. It only needs re-generating for a newer Python
(another Unicode version): `python browser/tools/gen_unicode_tables.py`, then rebuild. See
[browser.md](browser.md#text-pythons-rules-from-pythons-tables).

## API_VERSION and ENGINE_API

The app (and the tests) won't start with an engine built from older code than the Python side.
`engine/src/bindings.cpp` sets `m.attr("API_VERSION")`, and `src/substation/__init__.py` has `ENGINE_API`, the
version the Python code needs. `engine_mismatch()` compares them:

- `app.main()` shows the message in a dialog (and on stderr) and exits.
- `tests/conftest.py` calls `pytest.exit()` with it, since the tests would otherwise fail in confusing ways.

The message says whether the engine is older or newer than the app, and to re-run
`python -m pip install --no-build-isolation -e .`.

**The rule:** when Python code comes to need a change in `engine/src/bindings.cpp`, bump `API_VERSION` there and
`ENGINE_API` in `src/substation/__init__.py` together, and note what the new version brought in the comment above
`ENGINE_API`. Both are 15 now.

`_browser` has no such check; after changing its bindings, rebuild before running.

## Third-party code

Vendored in [engine/third_party](../engine/third_party), so nothing else needs installing:

| Library | Version | Licence | What is used |
|---|---|---|---|
| [miniaudio](../engine/third_party/miniaudio) | 0.11.25 | public domain (Unlicense) or MIT No Attribution, as you choose | WASAPI devices; decoding |
| [Signalsmith Stretch](../engine/third_party/signalsmith-stretch) | 1.3.2 | MIT | time stretching and pitch shifting (header-only) |
| [Signalsmith Linear](../engine/third_party/signalsmith-linear) | 0.6.4 | MIT | its FFT (`stft.h`, `fft.h` only) |
| [VST 3 SDK](../engine/third_party/vst3sdk) | 3.8.1 | MIT (since SDK 3.8) | `pluginterfaces`, `base`, `public.sdk/source/{common,main}` and `public.sdk/source/vst`, without VSTGUI, the SDK's tests, the wrappers and the non-Windows module loaders |

Each folder has its licence and a `VERSION.txt` saying what was taken and that it is unmodified. The full VST 3 SDK
is at <https://github.com/steinbergmedia/vst3sdk>.

## ASIO SDK

ASIO needs Steinberg's ASIO SDK, which isn't in the repository: its licence doesn't allow passing it on. Download it
from <https://www.steinberg.net/asiosdk> and unzip it into the project folder as it comes (for example
`SUBstation\asiosdk_2.3.3_2019-06-14\common\...`, or `ASIO-SDK_2.3.4_...` for newer ones). `.gitignore` keeps
`asiosdk*`, `ASIOSDK*`, `ASIO-SDK*` and `asio-sdk*` folders out of git. Only its headers are used.

Where the build looks, in this order:

1. `SUBSTATION_ASIO_SDK` (environment or CMake variable), if set. It must be the folder containing `common`; if it
   has no `common/iasiodrv.h` the configure stops with an error.
2. Otherwise any folder of the project folder, or one level deeper (some unzip tools add a level):
   `<project>/*/common/iasiodrv.h`, `<project>/*/*/common/iasiodrv.h`.
3. An `asio*` folder beside the project (or one level inside it): `<project>/../asio*/common`,
   `<project>/../asio*/*/common`.
4. An `asio*` folder in the root of the system drive (`%SystemDrive%\asio*`, or one level inside it).

If several are found, the one whose path sorts highest wins (meant to pick the newest of several versions, as
`asiosdk_2.3.3_...` after `asiosdk2.3`). The
build prints `ASIO SDK: <folder>` when it found it; without it, it warns and builds the engine with WASAPI only (the
preferences then show ASIO greyed out, and the ASIO tests are skipped). Re-run the install command after adding the
SDK. With `-C cmake.define.SUBSTATION_ASIO=OFF` it doesn't look at all.

What the engine does with ASIO is in [engine/audio-devices.md](engine/audio-devices.md).

## Test plug-ins and the fake driver

The build also makes the small VST3 plug-ins and the fake ASIO driver the tests use (`tests/vst3_plugins`,
`tests/asio_driver`), installed next to the engine:

```
site-packages\substation\
  _engine.cp312-win_amd64.pyd
  _browser.cp312-win_amd64.pyd
  _testplugins\SUBTestPlugins.vst3\Contents\x86_64-win\SUBTestPlugins.vst3
  _testdrivers\SUBTestAsio.dll          (only with the ASIO SDK)
```

Turn that off with `-C cmake.define.SUBSTATION_TEST_PLUGINS=OFF`; the tests that need them are then skipped. What
they are and how the tests use them is in [testing.md](testing.md).

## Linting

`ruff` is installed with the dev tools. The project has no ruff configuration, so its defaults apply:

```powershell
ruff check src tests benchmarks
```

## Gotchas

- After pulling changes, re-run the install command if any C++ changed; the app and tests say so if the engine's
  API version moved, but not for other C++ changes, or for `_browser`.
- Switching Visual Studio versions needs a fresh build folder (see above).
- A plain `pip install -e .` (with build isolation) works but rebuilds from scratch each time.
- The modules are built for one Python (`cp312`); another Python version needs its own build (and gets its own
  `build\<wheel tag>` folder).
