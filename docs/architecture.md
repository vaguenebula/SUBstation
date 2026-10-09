# Architecture

SUBstation is one C++ program in five layers: a Qt Quick UI that draws everything, an application layer that owns
the project and everything that works on it, a real-time audio engine that plays it, a small library that indexes
and searches the sample browser's files, and the intelligence module, which works things out about music and sound
(how similar sounds are, in the background; a song's chords and key, from its MIDI). This page is the map; each part has its own pages (see the
[index](README.md)).

## Layers

```
                ┌─────────────────────────────────────────────────────────────────────┐
  UI            │ ui/qml   QML: windows, panels, menus, dialogs, controls             │
  (Qt Quick)    │ ui/src   scene-graph items: arrangement, piano roll, devices,       │
                │          meters, knobs (SgCanvas, drawn by SgPainter)               │
                │     │ Q_INVOKABLE calls (QML),          ▲ signals, properties,      │
                │     │ C++ calls (gestures)              │ list models               │
                └─────┼───────────────────────────────────┼───────────────────────────┘
                ┌─────▼───────────────────────────────────┴───────────────────────────┐
  application   │ Session (session/): the open project and what works on it           │
  layer         │   └─ ProjectEditor (editor/) ─► QUndoCommand (model/Commands.h)     │
  (Qt Core and  │                                   │ redo() / undo()                 │
   Gui: the GUI │                                   ▼                                 │
   thread)      │     the UI ◄── Qt signals ─── Project (model/)                      │
                │                                   │ Qt signals                      │
                │                                   ▼                                 │
                │ EngineBridge (audio/): mirrors the project into the engine;         │
                │ polls the playhead, meters, plug-in reports, device events          │
                └─────────────┬────────────────────────────────────────▲──────────────┘
                     Engine.h │ calls                                  │ atomics, queues
                ┌─────────────▼────────────────────────────────────────┴──────────────┐
  audio engine  │ Engine: edit model under a recursive mutex                          │
  (sub_engine)  │   └─ builds an immutable RenderSnapshot ─► one atomic pointer swap  │
                │ Renderer + Scheduler (audio thread + workers) read it               │
                │ audio devices (WASAPI, ASIO; System elsewhere), MIDI input,         │
                │ Recorder, processors: built-in devices, VST3 plug-ins               │
                └─────────────────────────────────────────────────────────────────────┘
                ┌─────────────────────────────────────────────────────────────────────┐
  browser       │ sub_browser: indexer thread + search thread; their wake callback    │
  backend       │ ─► FileIndex (app/src/browser) takes the results on the GUI thread  │
                └─────────────────────────────────────────────────────────────────────┘
                ┌─────────────────────────────────────────────────────────────────────┐
  intelligence  │ sub_intelligence: sound similarity's keeper, analysers and search   │
                │ threads; their wake callback ─► SoundSimilarity (app/src/           │
                │ intelligence) on the GUI thread. Its library: the browser's files   │
                │ Harmony: pure functions; Harmony (app/src/intelligence) calls them  │
                │ on the GUI thread with the song's MIDI notes when asked             │
                └─────────────────────────────────────────────────────────────────────┘
```

| Layer | Where | Built as | Depends on | Docs |
|---|---|---|---|---|
| UI | [ui/qml](../ui/qml) (QML module `SUBstation`), [ui/src](../ui/src) (C++ Qt Quick items), [ui/style](../ui/style) (the controls' style), [ui/main.cpp](../ui/main.cpp) | `sub_ui` + the `substation` executable | the application layer, Qt Quick, QML, Quick Controls | [ui/](ui/README.md) |
| Application layer | [app/src](../app/src): `model/`, `editor/`, `io/`, `audio/`, `session/`, `browser/`, `intelligence/`, `plugins/`, `analysis/` | `sub_app` | Qt Core and Gui, `sub_engine`, `sub_browser`, `sub_intelligence` | [app/](README.md#application-layer) |
| Audio engine | [engine/src](../engine/src) | `sub_engine` (namespace `sub`) | the C++ standard library, miniaudio, the VST 3 SDK | [engine/](engine/README.md) |
| Browser backend | [browser/src](../browser/src) | `sub_browser` (namespace `sub::browser`) | the C++ standard library | [browser.md](browser.md) |
| Intelligence | [intelligence/src](../intelligence/src): `core/`, `similarity/`, `harmony/`, `humanize/` | `sub_intelligence` (namespace `sub::intelligence`) | the C++ standard library, miniaudio (its decoders), Essentia (its descriptors: vendored, AGPLv3, [licensing.md](licensing.md)), HUMANBRO's runtime (`humanbro`, vendored; its model in intelligence/models) | [intelligence.md](intelligence.md) |
| Plug-in scanner | [tools/scanner](../tools/scanner/main.cpp) | `substation-scan`, a program of its own | `sub_engine` | [app/plugin-scanner.md](app/plugin-scanner.md) |

[ui/main.cpp](../ui/main.cpp) puts the layers together: it makes the `QGuiApplication`, the engine, the
`Session` on the engine, registers the session as the QML singleton `Session` (`sub::ui::registerSession`,
[ui/src/app/AppTypes.cpp](../ui/src/app/AppTypes.cpp)) and loads `Main.qml`. Once the window shows it calls
`Session::start()` (the audio device) and opens the project given on the command line. After the event loop,
`Session::shutdown()` unloads everything while the application is still whole. See
[app/session.md](app/session.md).

### Who talks to whom

- **The UI never changes the project itself.** QML calls the application layer's `Q_INVOKABLE` methods
  (`Session.editor.setTempo(...)`, `Session.arrangement.paste()`), and the scene-graph items call it in C++ from
  their mouse, key and wheel handlers (that is where gestures are worked out). It repaints from the
  application layer's signals, properties and list models.
- **QML reaches the application through the `Session` singleton**: `Session.project`, `Session.editor`,
  `Session.selection`, `Session.bridge`, `Session.browser`, `Session.plugins`, `Session.undoStack` and the
  session's parts (`Session.arrangement`, `Session.deviceSelection`, `Session.render`...). Their types are
  registered as uncreatable QML types in `AppTypes.cpp`. The application layer declares `Q_PROPERTY`s,
  `Q_INVOKABLE`s and signals, but includes nothing of QML.
- **The application layer talks to the engine only through [Engine.h](../engine/src/Engine.h)** and the few engine
  headers it needs besides: `builtin/BuiltinRegistry.h` (the built-in devices), `Processor.h` (`ParamInfo`),
  `AudioSource.h`, `RenderJob.h`, `AudioDevice.h` (the default driver), `builtin/EqDesign.h` (the EQ's curves),
  `builtin/DisperserDesign.h` (the Disperser's group delay)
  and `plugins/Vst3Format.h` (the default VST3 folders). Only the engine bridge talks to the engine about the
  project. What the UI shows of the engine (waveform peaks, meters, the scope, device status, plug-in parameter
  texts, devices' displays) comes through application-layer types whose headers include no engine header
  (`Waveform`, `MeterLevel`, `ProcessorParam`...: see [app/engine-bridge.md](app/engine-bridge.md)).
- **The browser backend** knows nothing of the rest: `FileIndex` ([app/src/browser](../app/src/browser)) hands it
  places and queries and takes back results when its wake callback says there are some.
- **The intelligence module** knows nothing of the rest either, not even the browser: `SoundSimilarity`
  ([app/src/intelligence](../app/src/intelligence)) gives it a source for its library (the browser's latest snapshot,
  read on the module's thread) and its searches, and takes back results when its wake callback says there are some.
  The browser orders a list by a result through a score function (`Sort::Score`), knowing nothing of where it came
  from. `Harmony` hands the module's harmony functions the notes the song's MIDI tracks play (plain values) and takes
  back its chords and key; the piano roll draws them and writes parts from them.

### The boundaries, checked

`ctest -R boundaries` runs [cmake/CheckBoundaries.cmake](../cmake/CheckBoundaries.cmake), which reads every
source file's `#include` lines and fails if:

| Folder | Must not include |
|---|---|
| `engine/src`, `browser/src`, `intelligence/src` | anything of Qt (`Q...`, `qt...`), `app/` or `ui/` |
| `intelligence/src` | the engine's headers, the browser backend's headers (it decodes through the `miniaudio` library and analyses with `essentia`, third-party targets of their own) |
| `app/src` | Qt Quick or QML (`QtQuick`, `QtQml`, `QQuick*`, `QQml*`, `QJSValue`, `QJSEngine`) or `ui/` |
| `ui/src` | the engine's headers (any header under `engine/src` by its name, miniaudio, the VST 3 SDK) |

`ui/main.cpp` is outside `ui/src`: it alone makes the engine. The check looks at each file's own includes, not at
what the headers it includes include in turn. The build enforces the rest: `sub_engine`, `sub_browser` and
`sub_intelligence` don't link Qt, and `sub_app` links only Qt Core and Gui (`QUndoStack` is in Qt Gui). See
[building.md](building.md#the-layers-boundaries).

## The life of an edit

1. **The UI asks.** A QML control calls the editor, for example the tempo box in
   [TransportBar.qml](../ui/qml/transport/TransportBar.qml): `onMoved: (value, gestureKey) =>
   Session.editor.setTempo(value, gestureKey)`. Or a scene-graph item's gesture does, in C++: a time selection
   dragged in the arrangement ([ui/src/arrangement/ClipGestures.cpp](../ui/src/arrangement/ClipGestures.cpp))
   lets the engine play what the drag would make (`EngineBridge::previewClips`, from
   `ProjectEditor::movedRange`) while it goes on, and on release calls `ProjectEditor::moveRange()`, then
   `EngineBridge::endClipPreview()`. Commands that act on the selection go to the session's parts
   (`Session.arrangement`, `Session.deviceSelection`).
2. **The editor works out the new state** ([app/src/editor](../app/src/editor)) with the model's pure functions:
   clips in [Edits.h](../app/src/model/Edits.h), notes in [Notes.h](../app/src/model/Notes.h), envelopes in
   [Automation.h](../app/src/model/Automation.h); they take and return values, and are tested on their own. It
   pushes a `QUndoCommand` from [Commands.h](../app/src/model/Commands.h) onto the session's `QUndoStack`. Every
   change to the project goes through one. A continuous gesture passes a merge key (a `QString`, one per gesture:
   the UI makes it with `QUuid::createUuid()`), so a whole drag is one undo step. An edit the user can't make
   throws `EditError` in C++; the `Q_INVOKABLE` forms (`trySetTrackParam`, ...) report it on `refused` instead,
   and the session shows it in the status line.
3. **The command changes the `Project`** ([Project.h](../app/src/model/Project.h)) in its `redo()` (or `undo()`),
   which emits a signal: `clipsChanged`, `devicesChanged`, `automationChanged`...
4. **Everything that shows or plays the project follows the signal.** QML bindings read their properties again;
   scene-graph items work out what to draw on the GUI thread and call `update()` (their `paint()` runs at the next
   frame's sync). The session prunes the selection of what is gone. The engine bridge
   ([app/src/audio](../app/src/audio)) pushes the change into the engine: only what changed, compared with what
   it gave the engine last.
5. **The engine** updates its edit model under its mutex, builds a new `RenderSnapshot` (positions already
   converted to samples) and publishes it with one atomic pointer swap. The audio thread picks it up at the next
   chunk. Continuous controls (volume, pan, mute, solo, device parameters) are atomics instead: no new snapshot.

View state (track heights, folding, arming, solo, which devices are folded and which automation shows, Lock
Envelopes) changes the project directly: it is saved, but isn't an undo step. See [app/model.md](app/model.md).

## Threads

| Thread | Runs | Must not |
|---|---|---|
| Qt GUI thread (main) | QML and the items' event handlers; the whole application layer: session, model, undo, editor, bridge, browser controller, plug-in index; every engine call but those below; plug-in creation, editors and `Engine::idle()`; opening audio devices and MIDI inputs | block on the audio thread |
| Qt Quick render thread | the scene graph's sync and rendering: every `SgCanvas` item's `paint()`, during the sync step, while the GUI thread is blocked. (Where Qt uses its basic render loop, this is the GUI thread itself.) | write anything: `paint()` reads the item and the model, and changes nothing (no property writes, signals, JavaScript or new QObjects) |
| Audio thread (driver callback) | the `Renderer`, a chunk at a time; it hands tracks to the workers | lock, allocate, free, or wait for any other thread |
| Render workers ([Scheduler](engine/scheduler.md)) | tracks of the routing graph, each after what goes into it | the same as the audio thread |
| The bridge's decoding pool (`QThreadPool`, 2 threads) | decoding audio files into engine sources (`Engine::loadSource`); results come back to the GUI thread | touch the model |
| The bridge's state pool (`QThreadPool`, 1 thread) | restoring built-in devices' states (a sampler's sample), one at a time, in the order they were set | touch the model |
| Recorder's disk writer | empties the recording rings into WAV files | — |
| Render jobs ([RenderJob](engine/README.md#in-the-background)) | an export or a freeze's render, on the snapshot it began with (live output is silent meanwhile) | take the engine's mutex |
| Reverse jobs (`ReverseJob`) | a reversed copy of a decoded file written as a WAV file, then decoded | touch the model |
| MIDI driver threads | stamp incoming messages against the audio clock and queue them for the audio thread | take the engine's mutex |
| Browser indexer and search threads (`sub_browser`) | keep the file index, run searches; then call the wake callback, which posts a queued call to `FileIndex` on the GUI thread | call into the application (but for the wake callback) |
| Sound similarity's keeper and analysers (`sub_intelligence`, background priority; a quarter of the cores, one to four analysers) | read and save the fingerprints, take the library from the browser's snapshots, fingerprint new and changed files | call into the application (but for the wake callback); the library source reads only immutable snapshots |
| Sound similarity's search thread (`sub_intelligence`) | fingerprints the sound searched from if it must (a clip's part, a file outside the library), compares it with every file; then the wake callback | the same |
| Plug-in index's scan thread (`QThread`) | `PluginScanner::scan()`: the cache, and the child processes it starts and reads | touch the index's objects (it hands its result over when it finishes) |
| `substation-scan` child processes | load VST3 modules to read their classes | — (a crash or a hang costs only that file) |

The application's renders in the background (exporting, freezing, reversing) never wait on the GUI thread: a
`RenderTask` looks at its job every 30 ms on a timer and says how far it got, and the session's renders follow its
signals. There are no nested event loops (see [app/session.md](app/session.md#renders-in-the-background)).

## Real-time rules and the boundary with the audio thread

- **The audio callback never locks, allocates, frees or waits.** (That is the host's part; what a plug-in does in
  its `process()` is up to the plug-in.)
- **Edits build an immutable `RenderSnapshot`**, published with one atomic pointer swap. Retired snapshots are freed
  once the audio thread's epoch counter shows it no longer uses them, never by the audio thread: by whichever
  thread makes the next engine call (`collectGarbageLocked()`), and by `Engine::idle()`, which the bridge calls
  from its 33 ms timer. Plug-ins are destroyed only on the GUI thread, in `idle()`.
- **Continuous controls are atomics** (volume, pan, mute, solo, send levels, device parameters), smoothed on the
  audio thread. A fader drag never rebuilds a snapshot.
- **What goes to the audio thread** besides snapshots and atomics goes through single-producer, single-consumer
  lock-free queues: transport commands (play, stop, locate), notes played by hand, and MIDI input (stamped on the
  drivers' threads).
- **The application never waits on the audio thread.** What comes back is read without waiting: the playhead, the
  transport state, CPU load and meters are atomics (meters are written with "store max" and taken with
  `exchange(0)`); recorded peaks, plug-in reports and driver events go through lock-free queues or flags. The bridge
  drains them on the GUI thread on its timers: the playhead every 16 ms (`kPositionPollMs`), and every 33 ms
  (`kMeterPollMs`) recording progress, meters, `Engine::idle()` with the plug-ins' reports, and device events
  (see [app/engine-bridge.md](app/engine-bridge.md#polling)).
- **Renders are deterministic**: the result never depends on which thread finished a track first, so offline
  renders are bit-identical on any number of threads.
- **Plug-ins live on the GUI thread.** They are created, called and destroyed there, as plug-in formats require.
  A plug-in (or an ASIO driver's control panel) may run a message loop inside a call, which may call back into the
  UI and the engine on that thread: the engine's mutex is recursive, slow plug-in calls are made without it, and
  the bridge doesn't dispatch plug-in reports or device events while such a call runs.

Details: [engine/README.md](engine/README.md) (the edit model, snapshots, epochs, `Transport.h`'s shared state),
[engine/rendering.md](engine/rendering.md), [engine/scheduler.md](engine/scheduler.md),
[engine/plugins.md](engine/plugins.md).

## One routing model

Tracks, groups, returns, the master and a rack's chains are all *strips*: input → devices → delay compensation →
volume/pan/mute/solo → meter → output. They differ only in where their input comes from. The engine knows *edges*,
not hierarchy: a track's output into a group or the master, sends into returns, a track's input from another
track (resampling), a sidechain into a device. The hierarchy (which track is in which group) lives in the
application's model (`Track::parent`; the tracks stay a flat list), which also refuses any edge that would close a
cycle before it reaches the engine. Delay compensation is worked out per edge. See
[engine/routing.md](engine/routing.md) and [app/model.md](app/model.md#returns-sends-inputs-sidechains-the-routing-graph).

## Parameters and automation

Every automatable thing (device parameters of built-ins and plug-ins, volume, pan, sends, rack chains' volume and
pan) is described the same way: a normalized 0..1 value with a mapping (linear, logarithmic, or stepped). The
engine describes its processors' parameters as `ParamInfo` ([engine/src/Processor.h](../engine/src/Processor.h));
the application layer's `ParamSpec` ([app/src/model/ParamSpec.h](../app/src/model/ParamSpec.h)) is made from one
(`ParamSpec::fromInfo`) and maps the same way, and describes the mixer's controls itself (`mixerSpecs`). The
mixer's mappings and the shape of curved segments in [Automation.h](../app/src/model/Automation.h) are the
engine's ([engine/src/Automation.h](../engine/src/Automation.h): `kAutomationCurvature`, `kMaxVolumeGain`), and
[test_automation_model.cpp](../tests/app/test_automation_model.cpp) holds the two together. The bridge hands the
UI every owner's parameters as `ParamSpec`s (`EngineBridge::paramGroups`), and the editor learns plug-ins'
mappings (for macros) through a hook the session wires to the bridge. See
[engine/automation.md](engine/automation.md) and [app/model.md](app/model.md#automation).

## One program

The engine, the application layer and the UI are linked into one executable, built from one source tree, so they
can't disagree about the engine's API: there is no API version to keep in step. The only other program is
`substation-scan`, which the plug-in index starts to read plug-in files; it speaks a line-based JSON protocol
([app/plugin-scanner.md](app/plugin-scanner.md)).
