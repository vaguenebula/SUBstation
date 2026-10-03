# Architecture

SUBstation is three programs in one process: a Python/Qt application that owns the
project and draws everything, a C++ audio engine that plays it, and a small C++
library that indexes and searches the sample browser's files. This page is the map;
each part has its own pages (see the [index](README.md)).

## Layers

```
                 ┌──────────────────────────────────────────────────────────────┐
  Python         │ ui/            widgets, arrangement, piano roll, device view │
  (Qt main       │   │ calls                          ▲ Qt signals           │
  thread)        │   ▼                                │                       │
                 │ model/editor/    ──► model/commands.py (QUndoCommand)       │
                 │                              │ mutate                       │
                 │                              ▼                              │
                 │                      model/project.py  (single source of    │
                 │                              │ Qt signals   truth, saved)   │
                 │                              ▼                              │
                 │ audio/engine_bridge/    (mirrors the model into the engine; │
                 │                          polls playhead, meters, events)    │
                 └──────────────┬──────────────────────────────▲───────────────┘
                     nanobind   │ substation._engine           │ atomics, queues
                 ┌──────────────▼──────────────────────────────┴───────────────┐
  C++ engine     │ Engine (edit model, under a recursive mutex)                 │
                 │   └─ builds an immutable RenderSnapshot ─► atomic swap        │
                 │ Renderer + Scheduler (audio thread + workers) read it        │
                 │ AudioDevice backends (WASAPI / ASIO), MIDI input, Recorder   │
                 │ Processors: built-in devices, VST3 plug-ins                   │
                 └──────────────────────────────────────────────────────────────┘
                 ┌──────────────────────────────────────────────────────────────┐
  C++ browser    │ substation._browser: indexer thread + search thread;         │
                 │ wakes the UI through a Win32 event (independent of engine)   │
                 └──────────────────────────────────────────────────────────────┘
```

| Part | Where | Docs |
|---|---|---|
| UI | [src/substation/ui](../src/substation/ui) | [ui/](ui/README.md) |
| Project model, undo, saving | [src/substation/model](../src/substation/model) | [python/model.md](python/model.md), [python/serialization.md](python/serialization.md) |
| Engine bridge, start-up, settings | [src/substation/audio](../src/substation/audio), [app.py](../src/substation/app.py) | [python/engine-bridge.md](python/engine-bridge.md) |
| Plug-in scanner | [src/substation/plugins](../src/substation/plugins) | [python/plugin-scanner.md](python/plugin-scanner.md) |
| Audio engine | [engine/src](../engine/src) | [engine/](engine/README.md) |
| Browser backend | [browser/src](../browser/src) | [browser.md](browser.md) |
| Build, tests, benchmarks | [CMakeLists.txt](../CMakeLists.txt), [tests](../tests), [benchmarks](../benchmarks) | [building.md](building.md), [testing.md](testing.md) |

## The life of an edit

1. A widget calls a method on the `ProjectEditor` ([model/editor/](../src/substation/model/editor)),
   such as moving clips or setting a device parameter.
2. The editor works out the new state (clip maths in [edits.py](../src/substation/model/edits.py),
   note maths in [notes.py](../src/substation/model/notes.py), envelope maths in
   [automation.py](../src/substation/model/automation.py): pure functions, tested without Qt)
   and pushes a `QUndoCommand` from [commands.py](../src/substation/model/commands.py).
   Every mutation of the project goes through one; continuous gestures pass a `merge_key`
   so a whole drag is one undo step.
3. The command mutates the `Project` ([project.py](../src/substation/model/project.py)),
   which emits a Qt signal (`clips_changed`, `devices_changed`, `automation_changed`, ...).
4. The UI repaints from the signal, and the engine bridge
   ([engine_bridge/](../src/substation/audio/engine_bridge)) pushes the change into
   the engine through the `substation._engine` bindings.
5. The engine updates its edit model under its mutex, builds a new `RenderSnapshot`
   (positions already converted to samples) and publishes it with one atomic pointer
   swap. The audio thread picks it up at the next chunk.

View state (track heights, folding, which automation lanes show) changes the project
directly: it is saved, but isn't an undo step.

## Threads

| Thread | Runs | Must not |
|---|---|---|
| Qt main thread | UI, model, undo, bridge, every engine API call, plug-in creation/editors/`idle()`, opening devices | block on the audio thread |
| Audio thread (driver callback) | `Renderer` for each chunk; hands tracks to workers | lock, allocate, free, touch Python, wait for the main thread |
| Render workers | tracks of the routing graph ([Scheduler](engine/scheduler.md)) | same as the audio thread |
| Decoding pool (`QThreadPool`) | decoding audio files into engine sources (the engine releases the GIL) | touch the model |
| Recorder's disk writer | empties recording rings into WAV files | — |
| MIDI driver threads | stamp incoming messages against the audio clock and queue them | take the engine's mutex |
| Browser indexer / search threads | keep the file index, run searches | call into Python |
| Plug-in scan child processes | load VST3 modules to read their classes | — (a crash only marks the file failed) |

## Real-time rules

- The audio callback never locks, allocates, frees or touches Python, so the GIL cannot
  cause dropouts. (That is the host's part; what a plug-in does in its `process()` is up
  to the plug-in.)
- Edits build an immutable `RenderSnapshot`, published with one atomic pointer swap.
  Retired snapshots are freed once the audio thread's epoch counter shows they are no
  longer in use, never by the audio thread: by whichever thread makes the next API call
  (`collectGarbageLocked()`, also run by `idle()`). Plug-ins are destroyed only on the
  main thread, in `idle()`.
- Continuous controls (volume, pan, mute, solo, device parameters) are atomics, smoothed
  on the audio thread.
- The UI never waits on the audio thread: the playhead, meters and CPU load are read from
  atomics, and everything else that comes back (plug-in edits, recorded peaks, driver
  events) goes through lock-free queues or flags drained on the main thread.
- Renders are deterministic: the result never depends on which thread finished a track
  first, so offline renders are bit-identical on any number of threads.

Details: [engine/README.md](engine/README.md), [engine/rendering.md](engine/rendering.md),
[engine/scheduler.md](engine/scheduler.md).

## One routing model

Tracks, groups, returns, the master and a rack's chains are all *strips*: input →
devices → delay compensation → volume/pan/mute/solo → meter → output. They differ only
in where their input comes from. The engine knows *edges*, not hierarchy: a track's
output into a group or the master, sends into returns, a track's input from another
track (resampling), a sidechain into a device. The hierarchy (which track is in which
group) lives in the Python model. Delay compensation is worked out per edge. See
[engine/routing.md](engine/routing.md).

## Parameters and automation

Every automatable thing (device parameters of built-ins and plug-ins, volume, pan,
sends, rack chains' volume and pan) is described the same way: a normalized 0..1 value with a
mapping (linear, logarithmic, or stepped). The engine's `ParamInfo` and the UI's
`ParamSpec` implement the same mapping and the tests hold them to each other. See
[engine/automation.md](engine/automation.md) and [python/model.md](python/model.md).

## Native modules and versioning

The build makes two extension modules: `substation._engine` (from
[engine/src/bindings.cpp](../engine/src/bindings.cpp)) and `substation._browser`
(from [browser/src/bindings.cpp](../browser/src/bindings.cpp)). The Python side refuses
to start with an engine built from older (or newer) code: `ENGINE_API` in
[src/substation/\_\_init\_\_.py](../src/substation/__init__.py) must equal the engine's
`API_VERSION`. Bump both together when Python code needs a change in the bindings. See
[building.md](building.md).
