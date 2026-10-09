# SUBstation

A DAW in C++20 with CMake and Qt 6 (Qt Quick for the UI). Windows is the target; it also builds and runs on
Linux, where its tests run too. The documentation in [docs/](docs/README.md) is detailed and kept current: read the
page for the part you are changing before changing it. [docs/architecture.md](docs/architecture.md) has the layers, the life of
an edit, the threads and the real-time rules.

## Building and testing (Linux)

```sh
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
ninja -C build                                     # a cold build takes several minutes: run it in the background
ctest --test-dir build -j4 --output-on-failure     # everything
ctest --test-dir build -R live_import              # the test programs whose names match
QT_QPA_PLATFORM=offscreen build/bin/test_live_import              # one Qt Test program by hand
QT_QPA_PLATFORM=offscreen build/bin/test_live_import readingSets  # one of its functions
build/bin/engine_tests warp                        # the engine's tests whose names contain "warp"
```

- Qt and the tools: the packages the Linux job of [.github/workflows/ci.yml](.github/workflows/ci.yml) installs
  (also in [docs/building.md](docs/building.md#requirements)). A fresh cloud container may not have them.
- `test_ui_*` programs need a display: CTest runs them under `xvfb-run` if it was found when the build was
  configured. `SUBSTATION_UI_SCREENSHOTS=<folder>` makes the UI's tests save screenshots, to look at a UI change.
  See [docs/testing.md](docs/testing.md).
- Sources are globbed: a new `.cpp`, `.h`, `.qml` or `tests/app/test_*.cpp` is picked up by the next `ninja`.
- Known traps (AUTOMOC on Qt 6.4, raw string literals and moc, `QT_NO_KEYWORDS`):
  [docs/building.md](docs/building.md#gotchas).

## Layers

| Layer | Where | May use |
|---|---|---|
| Engine | `engine/src` | no Qt: the audio thread, the plug-in hosting |
| Browser backend, intelligence | `browser/src`, `intelligence/src` | no Qt; intelligence nothing of the engine or the browser |
| Application | `app/src` | Qt Core and Gui, the engine; no Qt Quick or QML |
| UI | `ui/qml`, `ui/src` | Qt Quick and the application layer; never the engine's headers |

`ui/main.cpp` alone puts them together. `ctest -R boundaries` fails on an include across a boundary.

- The audio callback never locks, allocates, frees or waits
  ([real-time rules](docs/architecture.md#real-time-rules-and-the-boundary-with-the-audio-thread)).
- Every change to the project is a `QUndoCommand` the editor (`app/src/editor`) pushes; view state is not
  ([the life of an edit](docs/architecture.md#the-life-of-an-edit)).
- `QT_NO_KEYWORDS`: write `Q_SIGNALS`, `Q_SLOTS`, `Q_EMIT`.

## CI

Every pull request (and push to `master`) is built and tested three times
([.github/workflows/ci.yml](.github/workflows/ci.yml)): Linux with GCC and Qt 6.4 (Ubuntu 24.04's), Windows with
MSVC and with MinGW, both Qt 6.8.

- Qt 6.4 is the oldest, in C++ and in QML: QML in particular behaves differently before some later versions. Use
  nothing newer unless what it falls back to works on 6.4.
- The project's code builds with `/W4 /permissive-` (MSVC) and `-Wall -Wextra` (GCC, MinGW): keep it free of
  warnings on all three. MSVC's Debug builds define `_DEBUG`, which some third-party code reacts to.

## What a change comes with

- **Tests.** The engine's in `tests/engine` (its own harness, `engine_tests`); the application layer's and the UI's
  in `tests/app`, a Qt Test program per file. A fix comes with a test that fails without it.
- **Docs, in the same change.** What the user sees in the user guide ([docs/guide/](docs/guide/README.md)), how it
  works in the code reference (`docs/engine`, `docs/app`, `docs/ui`), a new page in [docs/README.md](docs/README.md)'s
  index.
- **A change to the project file format** raises `kProjectVersion` ([io/Serialization.h](app/src/io/Serialization.h)),
  listed in that header's comment and in [docs/app/serialization.md](docs/app/serialization.md#versions-and-migrations).
  An older file must load as it was: a default for what it lacks, no migration code.
- **Third-party code** is vendored under the layer's `third_party/`, with its licence and a `VERSION.txt` (what was
  taken, and any change to it), and listed in [docs/building.md](docs/building.md#third-party-code) and
  [docs/licensing.md](docs/licensing.md).

## Style

- Write like the code around it: its naming, its idioms, its density of comments.
- Comments and docs are plain, short sentences that say what something is for and why, not what the code says.
- Commit messages: a subject naming the area and what changed ("Live import: limit what reading a set may take"),
  then a body saying why.
