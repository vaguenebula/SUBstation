# The engine

The real-time audio engine is C++ in [engine/src/](../../engine/src), built into the Python
module `substation._engine` with nanobind. It holds everything on the audio thread and the
plug-in hosting. This page is about the `Engine` class: the edit model behind its API, how
edits become an immutable `RenderSnapshot` the audio thread plays, how old snapshots are freed,
and the small shared headers. The other engine docs go into each part (see [the map](#the-other-engine-docs)).

For the layers above it (Python model, bridge, UI) and the threads of the whole program, see
[architecture.md](../architecture.md); for how the module is built, [building.md](../building.md).

## Files

| File | What it holds |
|---|---|
| [Engine.h](../../engine/src/Engine.h) | The public API (the class the bindings wrap), the API's plain structs (`ClipDesc`, `NoteDesc`, `RecordTarget`, `SendInfo`, `SidechainInfo`, `MeterReading`, `ProcessorInfo`, `DeviceStatus`, `TrackCost`, ...), and the private edit model (`TrackModel`, `ChainModel`, `ProcessorEntry`, `SendModel`, `SidechainModel`). Its header comment is the threading model. |
| [Engine.cpp](../../engine/src/Engine.cpp) | Lifetime, sources (`loadSource`, the source cache), transport calls, browser preview, audio threads (`setAudioThreads`, cost ordering), and housekeeping (`idle()`). |
| [EngineDevice.cpp](../../engine/src/EngineDevice.cpp) | Opening and closing the audio device, device events, input meters, the master scope, and `audioCallback()`. See [audio-devices.md](audio-devices.md). |
| [EngineTracks.cpp](../../engine/src/EngineTracks.cpp) | Tracks, their mixer, routing (outputs, sends, the list of edges, cycle checks), meters, `setTrackAutomation`. See [routing.md](routing.md). |
| [EngineInput.cpp](../../engine/src/EngineInput.cpp) | Track inputs (device channels or another track's output), monitoring, arming, recording, MIDI input. See [recording.md](recording.md) and [midi.md](midi.md). |
| [EngineChains.cpp](../../engine/src/EngineChains.cpp) | Device chains, racks, sidechains, and the processor calls (parameters, state, editors, events). See [routing.md](routing.md), [devices.md](devices.md), [plugins.md](plugins.md). |
| [EngineSnapshot.cpp](../../engine/src/EngineSnapshot.cpp) | Building the `RenderSnapshot` from the edit model, and publishing it. See [rendering.md](rendering.md) and [routing.md](routing.md). |
| [EngineOffline.cpp](../../engine/src/EngineOffline.cpp) | `renderOffline()` and `exportWav()`: suspending live output, a separate `Renderer`, fresh delay lines and stretch voices. |
| [Snapshot.h](../../engine/src/Snapshot.h) | The snapshot's types. See [rendering.md](rendering.md). |
| [Renderer.h](../../engine/src/Renderer.h) / [.cpp](../../engine/src/Renderer.cpp) | Turns a snapshot into audio. See [rendering.md](rendering.md). |
| [Routing.h](../../engine/src/Routing.h), [Rack.h](../../engine/src/Rack.h) | The routing graph, delay compensation, racks. See [routing.md](routing.md). |
| [Scheduler.h](../../engine/src/Scheduler.h) / [.cpp](../../engine/src/Scheduler.cpp) | The task graph and worker threads. See [scheduler.md](scheduler.md). |
| [Transport.h](../../engine/src/Transport.h) | `TransportCommand`, `PreviewNote` and `SharedState`: what the API threads and the audio thread share. |
| [Metronome.h](../../engine/src/Metronome.h) / [.cpp](../../engine/src/Metronome.cpp) | The click generator. See [rendering.md](rendering.md#metronome-and-count-in). |
| [rt/RtUtils.h](../../engine/src/rt/RtUtils.h) | Real-time helpers: `ScopedNoDenormals`, `SmoothedValue`, `SpscQueue`, `DeferredReleasePool`, `atomicStoreMax`, `dbToGain`, `balanceGains`, `DisplayStream`. |
| [PathUtils.h](../../engine/src/PathUtils.h) | `pathFromUtf8()` and `widen()`: Python hands the engine UTF-8, Windows file APIs want UTF-16. |
| [bindings.cpp](../../engine/src/bindings.cpp) | The nanobind module `substation._engine`, and `API_VERSION`. |

The `Engine` class is declared once in `Engine.h` and implemented by area across the
`Engine*.cpp` files (the comment at the top of `Engine.cpp` lists them). They all build into
the one `_engine` target ([CMakeLists.txt](../../CMakeLists.txt)); a new `Engine*.cpp` has to be
added to its source list there.

## Threads

The header comment of [Engine.h](../../engine/src/Engine.h) is the rule book. In short:

- **The audio thread** (the device callback) reads the published `RenderSnapshot`, which is
  never changed once published, and atomics. It also writes the render state the snapshot
  points at but doesn't own: `TrackBuffers`, the edges' `EdgeState`s and `DelayLine`s, and the
  meters and fader smoothing in `TrackParams` (allocated on the edit side; only the audio thread
  and its workers write them while a snapshot is live). It never locks, allocates, frees or
  touches Python, so the GIL cannot cause dropouts.
  (That is the host's part; what a plug-in does in its `process()` is up to the plug-in.)
- **API calls** may come from any Python thread. They serialise on `mutex_` (a
  `std::recursive_mutex`), change the edit model, then rebuild and publish a new snapshot.
- **Plug-ins** are created, called and destroyed on the main (UI) thread, as plug-in formats
  require: the plug-in calls and `idle()` must come from the thread that created the engine.
  A plug-in may run a message loop inside a call (a licence dialog) that calls back into the
  engine from the same thread, which is why `mutex_` is recursive and why slow plug-in calls
  (`addPluginProcessor`'s loading, `setProcessorParam`, `processorState`, editors, ...) are made
  without holding it. See [plugins.md](plugins.md).
- **Audio devices** are opened and closed on the main thread too (ASIO drivers are COM objects in
  its apartment). See [audio-devices.md](audio-devices.md).
- **Recording** has its own disk-writer thread; anything that changes the device, or renders
  offline, ends the recording first. See [recording.md](recording.md).
- **MIDI input** arrives on the drivers' threads, is stamped there and queued for the audio
  thread; it never takes `mutex_`. See [midi.md](midi.md).
- **Audio worker threads** (the `Scheduler`) share each chunk's tracks with the thread
  rendering. They touch only what the snapshot hands them. See [scheduler.md](scheduler.md).
- **The UI never waits on the audio thread**: the playhead, meters and CPU load are read from
  atomics (`SharedState`, `TrackParams`).

## The edit model

The engine keeps its own model of the arrangement, separate from the Python model
(`src/substation/model/`, see [model.md](../python/model.md)); the bridge
([engine-bridge.md](../python/engine-bridge.md)) mirrors one into the other. The engine's
model is in engine terms: ids, samples-to-be, edges. It never sees groups or returns as such,
only tracks and the edges between them.

- **`TrackModel`** (in `tracks_`, plus `master_`): an id, its `TrackParams` (gain, pan, mute,
  solo, meters, fader smoothing; shared with every snapshot that contains the track), its
  clips (`ClipDesc`, with `clipKeys` for the source cache) and notes (`NoteDesc`), its main
  chain id, its output (`kMaster` or a track), its sends (`SendModel`), its automation lanes,
  its input (device channels, or `inputTrack`), MIDI input route, monitor mode and arm state.
  It also owns state the renderer keeps across snapshots: `buffers` (`TrackBuffers`),
  `outputState` and the sends' `EdgeState`s, and the edges' `DelayLine`s.
- **Track id 0 is the master** (`Engine::kMaster`, `MASTER` in Python). It has devices, a
  mixer and automation like a track, but no clips or notes, and it never mutes or solos.
  `trackLocked()` finds the master too; `arrangementTrackLocked()` refuses it.
- **`ChainModel`** (in `chains_`, by id): a chain of devices. Every strip has a main chain;
  each chain of a rack is one more, with its own fader (`params`) and delay line. A chain knows
  the strip whose signal it processes (`stripId`) and the rack it belongs to (`parentRack`, 0
  for a main chain).
- **`ProcessorEntry`** (in `processors_`, by processor id): the chain a device is in, the
  `Processor`, its sidechain (`SidechainModel`, if any), and for a rack its chains in order.
  Processor ids are unique across chains, so a device can move from one chain to another (on
  any strip) and keep its state: a plug-in isn't loaded again.
- **`StripSlot`** (built on demand by `stripSlotsLocked()`): a strip's devices depth first,
  each device of its main chain and, after a rack, the devices of its chains. Delay
  compensation and sidechains refer to devices by their slot (see [routing.md](routing.md)).
- **Sources**: `sources_` caches decoded `AudioSource`s by `sourceKey()` (the path normalised and
  lower-cased, so one file is decoded once whatever its spelling). `loadSource()` decodes without
  holding the lock and retries if the sample rate changed meanwhile; it rebuilds the snapshot,
  so clips waiting for the file become audible. A new sample rate reloads every source
  (`reloadSourcesLocked()`). See [warp.md](warp.md) for decoding.

Every call that changes something the renderer reads ends with `rebuildSnapshotLocked()`.
Calls that only change an atomic don't: track and chain gain, pan, mute and solo
(`setTrackGain` etc. store into `TrackParams`), a send's level when only the level changes
(`EdgeState::gain`), the metronome switch, a processor's parameter.

Errors for the caller are exceptions: `std::invalid_argument` (unknown ids, a route that would
close a cycle, racks nested too deep) becomes `ValueError` in Python, `std::runtime_error` (a
message for the user: a plug-in that can't load, no device for recording) becomes
`RuntimeError`.

## Building and publishing the snapshot

`rebuildSnapshotLocked()` ([EngineSnapshot.cpp](../../engine/src/EngineSnapshot.cpp)) turns the
whole edit model into a new `RenderSnapshot` every time, in which positions are already
converted to samples at the current tempo and sample rate. In order:

1. Transport values: sample rate, tempo, time signature, the loop in samples (only enabled if
   at least 256 samples long), the clip fade length.
2. The routing graph: `routeEdgesLocked()` lists every edge, `topologicalOrder()` puts each
   track after everything that feeds it. That order is the snapshot's track order.
3. Delay compensation for the whole graph (`alignGraph()`), from each strip's devices as
   `ChainSlot`s; each rack learns its latency for the UI (`RackProcessor::setLatency`).
4. The edges in snapshot order (`EdgeRender`), each track's incoming and outgoing edges, the
   sidechains into each strip's devices, the taps after devices, and the `TaskGraph` the
   scheduler runs.
5. The master strip and each track (`TrackRender`): its chain and racks (`buildChainLocked()`,
   recursively), automation in samples (`buildAutomationLocked()`), notes and clips in samples,
   its input, monitoring and MIDI route.
6. Stretch voices: enough for the clips that need stretching, at most 64 per configuration
   (`ensureWarpVoicesLocked()`); they only grow until the sample rate changes.

Steps 2 to 5 are described in [routing.md](routing.md) and [rendering.md](rendering.md).

Then it publishes:

```
snapshotHold_ = snap                          // the edit side's shared_ptr
snapshot_.store(snap.get(), seq_cst)          // one atomic pointer swap: the audio thread sees it next callback
releasePool_.retire(old, audioEpoch_.load())  // the old one waits for the audio thread
collectGarbageLocked()
serviceTransportIfIdleLocked()
```

Rebuilding everything on each edit is simple and keeps the audio thread's view consistent: a
snapshot is never changed once published. The state that must survive a swap (fader smoothing,
meters, delay lines, edge buffers, track buffers, stretch voices, processors) lives outside it,
in objects the snapshot only points at.

## Epochs and retiring snapshots

The audio thread loads the snapshot pointer at the start of each callback and bumps
`audioEpoch_` at its end (`Engine::audioCallback`). `DeferredReleasePool`
([rt/RtUtils.h](../../engine/src/rt/RtUtils.h)) keeps a retired object together with the epoch
read *after* it was unpublished. Any callback that could still see it finishes before the epoch
moves past that value, so the object may be freed once `epoch > retiredEpoch`, or at once when
no device runs. Both the store and the epoch read are `seq_cst`, as is the callback's load.

The same pool retires the browser preview's source (`preview()` / `stopPreview()`).

`collectGarbageLocked()` frees what is due. It runs in `idle()`, after every snapshot rebuild,
and when the device closes, so retired snapshots are freed on whichever thread made the API
call (the UI thread in practice, but `loadSource()` runs on the bridge's loader threads).
Freeing a snapshot never destroys a plug-in: removed processors wait in `graveyard_`.

When a device runs and the engine must be sure the audio thread is out of something (switching
the scheduler, rendering offline), `waitForCallbackLocked()` waits until the epoch moves (at
most 500 ms), after setting `liveSuspended_` so later callbacks output silence.

## idle()

`Engine::idle()` must be called regularly from the UI thread (the bridge does it from a timer,
`poll_plugins()`). It:

- closes the device if the backend lost it (the UI learns of it from `takeDeviceEvent()`);
- frees retired snapshots and sources (`collectGarbageLocked()`);
- applies transport commands itself while no device runs (`serviceTransportIfIdleLocked()`:
  without a callback nobody else would, so `position_beats` and `is_playing` stay right);
- destroys removed processors that no snapshot holds any more (`use_count() == 1` in
  `graveyard_`), outside the lock, since plug-ins may take their time to go;
- calls every processor's `idle()` outside the lock: the main-thread work plug-ins asked for
  (restarts, parameter updates to their controller, editor events). If any reports a new
  latency, the snapshot is rebuilt, for new delay compensation.

## Transport

`play()`, `stop()` and `setPositionBeats()` push a `TransportCommand` into
`SharedState::commands` (an `SpscQueue` of 256; pushed only under `mutex_`, so there is one
producer). The live renderer drains them at the start of each callback. `requestedPlaying_`
answers `isPlaying()` at once, and `setPositionBeats()` stores the new position for immediate
feedback; the audio thread confirms it. Tempo, time signature, loop and clip fades are snapshot
values (a rebuild). The metronome switch is an atomic. See [rendering.md](rendering.md).

## Transport.h: what the threads share

[Transport.h](../../engine/src/Transport.h) defines `SharedState`, one per engine:

| API -> audio thread | |
|---|---|
| `commands` | play, stop, locate (`TransportCommand`; Play carries a count-in in beats) |
| `previewNotes` | notes played by hand (`PreviewNote`; velocity 0 is a note-off) |
| `metronome` | the metronome switch |
| `previewSource`, `previewSerial`, `previewGain` | the browser preview (0.8 gain) |
| `midiInput`, `midiInputMutex`, `midiInputDelay`, `midiSampleRate` | MIDI input; producers push holding the mutex ([midi.md](midi.md)) |

| Audio thread -> API | |
|---|---|
| `playing`, `countingIn`, `positionSamples`, `positionBeats` | the transport as the audio thread has it |
| `previewActive` | the preview is still playing |
| `cpuLoad` | the callback's time over its budget, smoothed (0.1 per callback) |
| `clock` | `AudioClock`: the device's sample clock against the host clock |
| `inputPeaks` | the peak of each open input channel (up to 256) |
| `scope`, `scopeWritten` | the last 8192 samples of the master (mono), for the oscilloscope |

## rt/RtUtils.h

Everything in [rt/RtUtils.h](../../engine/src/rt/RtUtils.h) is wait-free and allocation-free on
the real-time side.

- `ScopedNoDenormals`: sets flush-to-zero / denormals-are-zero for a scope. The audio callback,
  offline renders and each worker during a run use it.
- `SmoothedValue`: a linear ramp to a target. Faders and send levels ramp over 20 ms.
- `SpscQueue<T, Capacity>`: single-producer single-consumer ring; head and tail on separate
  cache lines. A full queue drops the push (the caller sees `false`).
- `DeferredReleasePool`: see [Epochs](#epochs-and-retiring-snapshots).
- `atomicStoreMax()`: meters are written with "store max" by the audio thread and reset with
  `exchange(0)` by the UI (`takeMeters()`, `takeInputMeters()`).
- `balanceGains()`: balance-style pan with a sine taper, unity at the centre; used for track,
  chain and clip pan. `dbToGain()`.
- `DisplayStream`: values one thread publishes for others to draw (a device's meters and
  curves; see [devices.md](devices.md)). The writer overwrites the oldest; each reader keeps its
  own position.

## Offline renders

`renderOffline()` and `exportWav()` ([EngineOffline.cpp](../../engine/src/EngineOffline.cpp))
hold `mutex_` throughout. They end a recording, suspend live output (silence, after one
callback has passed), reset every processor (`resetOffline()` and `requestReset()`) before and
after, and render with a fresh `Renderer` that shares the scheduler and the live renderer's cost
ordering but brings its own stretch voices and delay lines, so it neither disturbs live
playback nor depends on it. With delay compensation the output lags the timeline by the
snapshot's `outputLatency()`: they render that much first and drop it. `exportWav()` writes 16
(triangle dither), 24 or 32-bit float WAV through miniaudio's encoder, 16384 frames at a time.
See [rendering.md](rendering.md#offline-renders).

## bindings.cpp and API_VERSION

[bindings.cpp](../../engine/src/bindings.cpp) is the nanobind module `substation._engine`. It
wraps `Engine` method by method in snake_case (`add_track`, `set_track_output`,
`render_offline`, ...), with properties for `tempo`, `position_beats`, `metronome`,
`audio_threads`, `cost_ordering`, `is_playing` and so on, and the API's structs as read-only
classes. Module-level functions: `probe_file`, `driver_types`, `builtin_devices`,
`vst3_search_paths`, `host_time_ns`, `eq_response` (an EQ band's response, see
[devices.md](devices.md)), and `task_graph_order` (for tests: the scheduler's queue
order and ranks, see [scheduler.md](scheduler.md)). `MASTER` is track id 0.

- Long-running calls release the GIL (`ReleaseGil`, or an explicit `nb::gil_scoped_release`
  where arguments must be converted first): loading sources, opening devices (a driver may show
  a dialog whose message loop calls Python), plug-in state, recording, offline renders,
  `idle()`. The audio thread never calls into Python.
- Byte strings (processor state) and arrays (`render_offline` returns a `(frames, 2)` float32
  array, recorded notes an `(n, 5)` int64 array) are handed over with capsules that own the
  buffer.
- Leak warnings are turned off: Qt/PySide can keep engine objects alive until interpreter
  teardown, which is harmless.

`API_VERSION` (currently 15) is set on the module. It is bumped whenever the Python code comes
to depend on a change in the bindings; `ENGINE_API` in
[src/substation/\_\_init\_\_.py](../../src/substation/__init__.py) must be bumped with it. The app
and the tests refuse to start with an engine built from older (or newer) code, and say to
rebuild. See [building.md](../building.md) and [engine-bridge.md](../python/engine-bridge.md).

## Extending it

**A new API call.** Declare it in `Engine.h` with a comment saying what it does and what it
throws; implement it in the `Engine*.cpp` of its area, taking `std::lock_guard lock(mutex_)`;
change the edit model, and call `rebuildSnapshotLocked()` if the renderer reads what changed
(or store into an existing atomic if it is continuous). Never let the audio thread reach the
edit model. If the call talks to a plug-in, don't hold the lock across the plug-in call. Bind it
in `bindings.cpp` (release the GIL if it can take long), then bump `API_VERSION` and
`ENGINE_API` together.

**New per-track render state** that must survive snapshots (a smoother, a buffer): allocate it
on the edit side with the track (as `TrackBuffers` and `EdgeState` are), keep it in
`TrackModel`, and hand the snapshot a `shared_ptr` to it.

## Gotchas

- `mutex_` is recursive because plug-ins re-enter; don't "fix" it into a plain mutex.
- `rebuildSnapshotLocked()` is not cheap for big projects: it rebuilds everything. Calls the UI
  makes continuously (fader drags) must stay atomics.
- `setTrackSend()` to the master is refused (everything reaches the master anyway).
- A removed processor lives until no snapshot holds it *and* `idle()` runs. Tests that check
  plug-in destruction call `idle()`.
- Without a device, preview notes are discarded and transport commands applied by the edit
  side, so state stays consistent in tests that render offline.

## Tests

Engine behaviour is tested through the bindings: [test_engine_render.py](../../tests/test_engine_render.py)
(clips, gain/pan/mute/solo, loop, tempo changes, metronome, fades, sources, export, transport
state without a device, chains and moves), and the area tests listed in each engine doc. The
API version check is in `src/substation/__init__.py`. See [testing.md](../testing.md).

## The other engine docs

| Doc | What it covers |
|---|---|
| [rendering.md](rendering.md) | `Renderer` (chunk prologue, graph, epilogue), strips, faders, solo and mute, metronome, count-in, preview, loop, `Snapshot.h` |
| [routing.md](routing.md) | `Routing.h`, edges of every kind, groups, returns and sends, sidechains, racks, delay compensation, cycle checks |
| [scheduler.md](scheduler.md) | `Scheduler`, `TaskGraph`, workers, MMCSS, heaviest-path ordering, determinism |
| [audio-devices.md](audio-devices.md) | `AudioDevice`, `DeviceConfig`, WASAPI and ASIO backends, driver events and resets, COM apartments, `EngineDevice.cpp` |
| [recording.md](recording.md) | `Recorder`, takes and their placement, resampling, input monitoring, `EngineInput.cpp` |
| [midi.md](midi.md) | MIDI playback (notes to events, note-offs, preview queue) and MIDI input |
| [automation.md](automation.md) | `Automation.h`, `ParamInfo`'s normalized mapping, `Processor::automate`, faders, sends, overrides |
| [warp.md](warp.md) | `Warp.cpp`, stretch voices, the Re-Pitch resampler, `AudioSource` decoding and peaks |
| [devices.md](devices.md) | `Processor.h`, `BuiltinProcessor`, `BuiltinRegistry`, the built-in devices; adding a device |
| [plugins.md](plugins.md) | VST3 hosting, the threading handshake, state, latency, the CLAP plan |
