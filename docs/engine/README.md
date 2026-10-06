# The engine

The real-time audio engine is C++ in [engine/src/](../../engine/src), built as the static
library `sub_engine` (namespace `sub`), with no Qt in it. It holds everything on the audio thread
and the plug-in hosting. This page is about the `Engine` class: the edit model behind its API, how
edits become an immutable `RenderSnapshot` the audio thread plays, how old snapshots are freed,
and the small shared headers. The other engine docs go into each part (see [the map](#the-other-engine-docs)).

For the layers above it (the application layer's model, the engine bridge, the UI) and the
threads of the whole program, see [architecture.md](../architecture.md); for how the library is
built, [building.md](../building.md). The application layer calls the engine only through
`Engine.h` and the few headers it exposes (`BuiltinRegistry`, `ParamInfo` in `Processor.h`,
`AudioSource`, `RenderJob`, `Vst3Format` for the default search paths): its engine bridge,
[app/src/audio/EngineBridge](../../app/src/audio/EngineBridge.h)
([app/engine-bridge.md](../app/engine-bridge.md)), mirrors the project into it. The UI never
includes the engine's headers.

## Files

| File | What it holds |
|---|---|
| [Engine.h](../../engine/src/Engine.h) | The public API (what the engine bridge calls), the API's plain structs (`ClipDesc`, `NoteDesc`, `RecordTarget`, `SendInfo`, `SidechainInfo`, `MeterReading`, `ProcessorInfo`, `DeviceStatus`, `TrackCost`, ...), and the private edit model (`TrackModel`, `ChainModel`, `ProcessorEntry`, `SendModel`, `SidechainModel`). Its header comment is the threading model. |
| [Engine.cpp](../../engine/src/Engine.cpp) | Lifetime, sources (`loadSource`, the source cache), transport calls, browser preview, audio threads (`setAudioThreads`, cost ordering), and housekeeping (`idle()`). |
| [EngineDevice.cpp](../../engine/src/EngineDevice.cpp) | Opening and closing the audio device, device events, input meters, the master scope, and `audioCallback()`. See [audio-devices.md](audio-devices.md). |
| [EngineTracks.cpp](../../engine/src/EngineTracks.cpp) | Tracks, their mixer, routing (outputs, sends, the list of edges, cycle checks), meters, `setTrackAutomation`. See [routing.md](routing.md). |
| [EngineInput.cpp](../../engine/src/EngineInput.cpp) | Track inputs (device channels or another track's output), monitoring, arming, recording, MIDI input. See [recording.md](recording.md) and [midi.md](midi.md). |
| [EngineChains.cpp](../../engine/src/EngineChains.cpp) | Device chains, racks, sidechains, and the processor calls (parameters, state, editors, events). See [routing.md](routing.md), [devices.md](devices.md), [plugins.md](plugins.md). |
| [EngineSnapshot.cpp](../../engine/src/EngineSnapshot.cpp) | Building the `RenderSnapshot` from the edit model, and publishing it. See [rendering.md](rendering.md) and [routing.md](routing.md). |
| [EngineOffline.cpp](../../engine/src/EngineOffline.cpp) | `renderOffline()` and `exportWav()`: suspending live output, a separate `Renderer`, fresh delay lines and stretch voices; renders in the background (`startExport()`, `startTrackRender()`) and `RenderJob`. |
| [RenderJob.h](../../engine/src/RenderJob.h) | A render on a thread of its own: its progress, `cancel()`, `finish()`. |
| [Snapshot.h](../../engine/src/Snapshot.h) | The snapshot's types. See [rendering.md](rendering.md). |
| [Renderer.h](../../engine/src/Renderer.h) / [.cpp](../../engine/src/Renderer.cpp) | Turns a snapshot into audio. See [rendering.md](rendering.md). |
| [Routing.h](../../engine/src/Routing.h), [Rack.h](../../engine/src/Rack.h) | The routing graph, delay compensation, racks. See [routing.md](routing.md). |
| [Scheduler.h](../../engine/src/Scheduler.h) / [.cpp](../../engine/src/Scheduler.cpp) | The task graph and worker threads. See [scheduler.md](scheduler.md). |
| [Transport.h](../../engine/src/Transport.h) | `TransportCommand`, `PreviewNote` and `SharedState`: what the API threads and the audio thread share. |
| [Metronome.h](../../engine/src/Metronome.h) / [.cpp](../../engine/src/Metronome.cpp) | The click generator. See [rendering.md](rendering.md#metronome-and-count-in). |
| [rt/RtUtils.h](../../engine/src/rt/RtUtils.h) | Real-time helpers: `ScopedNoDenormals`, `SmoothedValue`, `SpscQueue`, `DeferredReleasePool`, `atomicStoreMax`, `dbToGain`, `balanceGains`, `DisplayStream`. |
| [PathUtils.h](../../engine/src/PathUtils.h) | `pathFromUtf8()` and `widen()`: the engine takes paths as UTF-8, Windows file APIs want UTF-16. |

The `Engine` class is declared once in `Engine.h` and implemented by area across the
`Engine*.cpp` files (the comment at the top of `Engine.cpp` lists them). They all build into
the static library `sub_engine` ([engine/CMakeLists.txt](../../engine/CMakeLists.txt)); a new
`Engine*.cpp` has to be added to its source list there.

## Threads

The header comment of [Engine.h](../../engine/src/Engine.h) is the rule book. In short:

- **The audio thread** (the device callback) reads the published `RenderSnapshot`, which is
  never changed once published, and atomics. It also writes the render state the snapshot
  points at but doesn't own: `TrackBuffers`, the edges' `EdgeState`s and `DelayLine`s, and the
  meters and fader smoothing in `TrackParams` (allocated on the edit side; only the audio thread
  and its workers write them while a snapshot is live). It never locks, allocates, frees or
  calls into the application, so nothing there can cause dropouts.
  (That is the host's part; what a plug-in does in its `process()` is up to the plug-in.)
- **API calls** may come from any thread (the application's main thread, and the engine
  bridge's decoding threads for `loadSource()`). They serialise on `mutex_` (a
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

The engine keeps its own model of the arrangement, separate from the application layer's model
([app/src/model](../../app/src/model), see [app/model.md](../app/model.md)); the engine bridge
([app/engine-bridge.md](../app/engine-bridge.md)) mirrors one into the other. The engine's
model is in engine terms: ids, samples-to-be, edges. It never sees groups or returns as such,
only tracks and the edges between them.

- **`TrackModel`** (in `tracks_`, plus `master_`): an id, its `TrackParams` (gain, pan, mute,
  solo, meters, fader smoothing; shared with every snapshot that contains the track), its
  clips (`ClipDesc`, with `clipKeys` for the source cache) and notes (`NoteDesc`), its main
  chain id, its output (`kMaster` or a track), its sends (`SendModel`), its automation lanes,
  its input (device channels, or `inputTrack`), MIDI input route, monitor mode and arm state.
  It also owns state the renderer keeps across snapshots: `buffers` (`TrackBuffers`),
  `outputState` and the sends' `EdgeState`s, and the edges' `DelayLine`s.
- **Track id 0 is the master** (`Engine::kMaster`). It has devices, a
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

Errors for the caller are exceptions: `std::invalid_argument` for a call that can't be made
(unknown ids, a route that would close a cycle, racks nested too deep), `std::runtime_error` for
a message for the user (a plug-in that can't load, no device for recording). The engine bridge
catches them where the user can cause them and turns them into what the application shows.

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
call (the main thread in practice, but `loadSource()` runs on the bridge's decoding threads).
Freeing a snapshot never destroys a plug-in: removed processors wait in `graveyard_`.

When a device runs and the engine must be sure the audio thread is out of something (switching
the scheduler, rendering offline), `waitForCallbackLocked()` waits until the epoch moves (at
most 500 ms), after setting `liveSuspended_` so later callbacks output silence.

## idle()

`Engine::idle()` must be called regularly from the main thread (the engine bridge does it on its
meter timer, every 33 ms: `EngineBridge::pollPlugins()`). It:

- closes the device if the backend lost it (the UI learns of it from `takeDeviceEvent()`);
- frees retired snapshots and sources (`collectGarbageLocked()`);
- applies transport commands itself while no device runs (`serviceTransportIfIdleLocked()`:
  without a callback nobody else would, so `positionBeats()` and `isPlaying()` stay right);
- destroys removed processors that no snapshot holds any more (`use_count() == 1` in
  `graveyard_`), outside the lock, since plug-ins may take their time to go: for 20 ms at most
  a call (closing a project with many plug-ins doesn't hold up the UI), the rest go back to
  `graveyard_` for the next calls. `idle(releaseAll = true)` destroys them all (shutting down);
- calls every processor's `idle()` outside the lock: the main-thread work plug-ins asked for
  (restarts, parameter updates to their controller, editor events). If any reports a new
  latency, the snapshot is rebuilt, for new delay compensation. Not while a render runs in the
  background: a plug-in restarting then would leave a gap in it (the work waits for the next
  call after it).

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

`renderOffline()` and `renderTrackOffline()` ([EngineOffline.cpp](../../engine/src/EngineOffline.cpp))
hold `mutex_` throughout. They end a recording, suspend live output (silence, after one
callback has passed), reset every processor (`resetOffline()` and `requestReset()`) before and
after, and render with a fresh `Renderer` that shares the scheduler and the live renderer's cost
ordering but brings its own stretch voices and delay lines, so it neither disturbs live
playback nor depends on it. With delay compensation the output lags the timeline by the
snapshot's `outputLatency()`: they render that much first and drop it.

### In the background

`startExport()` and `startTrackRender()` do the same on a thread of their own and return a
`RenderJob` ([RenderJob.h](../../engine/src/RenderJob.h)) at once; `exportWav()` and
`renderTrackToWav()` are those, waited for. Under the lock, on the calling (main) thread, they
create the file (a `WavWriter`: miniaudio's encoder, 16-bit with triangle dither, 24-bit or
32-bit float), suspend live output, reset the processors and prepare an `OfflineRender`: the
renderer, its lines, and a `shared_ptr` to the snapshot as it is, which keeps its processors,
sources and buffers alive whatever the UI changes meanwhile. The job's thread then renders that
snapshot without the lock (4096 frames at a time), so the UI's calls (meters, the playhead,
`idle()`) don't wait for it:

- `progress()` is the frames rendered over the frames to render; `done()` says the thread has
  ended.
- `cancel()` stops it before its next chunk. The file is deleted when the render's state goes
  (on its thread) unless it was kept at the end: a cancelled or failed render leaves nothing.
- `finish()`, on the main thread, joins the thread, resets the processors again and gives live
  output back (`endJob()`), then returns the frames written, `nullopt` if cancelled, or throws
  what the render threw (a short write: "Could not write").
- One job at a time (`job_`). Until it is finished, what would disturb it is refused
  (`checkNotRenderingLocked()`, std::runtime_error "Wait for the render to finish"): another
  render, opening a device (its processors would be prepared anew), the audio threads (the job
  uses the scheduler), recording. Other changes are taken but not heard in the render.
- A job let go of unfinished cancels and finishes itself; an engine going while its job runs
  cancels it and waits for its thread first.

See [rendering.md](rendering.md#offline-renders).

## The API and its callers

[Engine.h](../../engine/src/Engine.h) is the API: the `Engine` class's methods (`addTrack`,
`setTrackOutput`, `renderOffline`, ...), its plain structs, and `Engine::kMaster` (track id 0).
Its callers are the application layer's engine bridge (on the main thread, and its decoding threads
for `loadSource()`), the scanner process `substation-scan` (`Vst3Format` only) and the engine's tests.
What the application needs besides an `Engine` is plain functions and singletons:

| What | Where |
|---|---|
| A file's format and length, without decoding it | `AudioSource::probe(path)` ([AudioSource.h](../../engine/src/AudioSource.h)) |
| The driver types this build has | `Engine::driverTypes()` |
| The built-in devices, instruments first, then by name | `BuiltinRegistry::instance().devices()` ([devices.md](devices.md)) |
| The default VST3 folders | `vst3::Vst3Format::instance().defaultSearchPaths()` ([plugins.md](plugins.md)) |
| The clock MIDI input is stamped with | `hostTimeNs()` ([MidiInput.h](../../engine/src/MidiInput.h)) |
| An EQ band's response | `eq::design()` and `eq::responseDb()` ([builtin/EqDesign.h](../../engine/src/builtin/EqDesign.h); the application's `eqResponseDb()` wraps them, see [devices.md](devices.md)) |

- Long-running calls (loading sources, opening devices, plug-in state, recording, offline renders,
  `RenderJob::finish()`, `idle()`) block the thread that calls them; the bridge decodes on threads
  of its own and renders in the background with a `RenderJob`. The audio thread and a
  `RenderJob`'s thread never call into the application.
- Processor state is a `std::vector<uint8_t>`; offline renders are interleaved stereo
  `std::vector<float>`; recorded notes are `RecordedNote`s.
- The engine and the application are built together, so there is no API version to check: what
  the bridge calls is what the engine has. See [building.md](../building.md) and
  [app/engine-bridge.md](../app/engine-bridge.md).

## Platforms

The engine builds on Windows (the product's platform) and on Linux; what differs is chosen in
[engine/CMakeLists.txt](../../engine/CMakeLists.txt) by file, not by `#ifdef` scattered through it:

| | Windows | Elsewhere (Linux) |
|---|---|---|
| Audio drivers | WASAPI (through miniaudio), and ASIO with the SDK ([audio-devices.md](audio-devices.md)) | "System": miniaudio's default backend (PulseAudio, ALSA, JACK...; never its null backend, which "plays" faster than real time), output only, no exclusive mode (`kDefaultDriver`) |
| MIDI input devices | WinMM ([backends/MidiWinMM.cpp](../../engine/src/backends/MidiWinMM.cpp)) | none ([backends/MidiNone.cpp](../../engine/src/backends/MidiNone.cpp)): no device is ever connected, but messages sent with `Engine::sendMidiInput()` (the computer keyboard, tests) still arrive ([midi.md](midi.md)) |
| Plug-in editor windows | Win32 windows ([plugins/EditorWindow.cpp](../../engine/src/plugins/EditorWindow.cpp)) | none ([plugins/EditorWindowNone.cpp](../../engine/src/plugins/EditorWindowNone.cpp)): `openEditor()` returns false ([plugins.md](plugins.md)) |
| VST3 modules | the SDK's `module_win32.cpp` | the SDK's `module_linux.cpp`; default folders `~/.vst3`, `/usr/lib/vst3`, `/usr/local/lib/vst3` |

The built-in devices register themselves from their own files, which nothing else refers to; a
program linking the static library would lose them. So each defines an anchor function, and
`BuiltinRegistry::instance()` calls them all (the build writes the list; see
[devices.md](devices.md#builtinregistry)).

## Extending it

**A new API call.** Declare it in `Engine.h` with a comment saying what it does and what it
throws; implement it in the `Engine*.cpp` of its area, taking `std::lock_guard lock(mutex_)`;
change the edit model, and call `rebuildSnapshotLocked()` if the renderer reads what changed
(or store into an existing atomic if it is continuous). Never let the audio thread reach the
edit model. If the call talks to a plug-in, don't hold the lock across the plug-in call. Then
call it from the engine bridge ([app/engine-bridge.md](../app/engine-bridge.md)); if it can take
long, from a thread of the bridge's own or as a `RenderJob`, not on the main thread.

**New per-track render state** that must survive snapshots (a smoother, a buffer): allocate it
on the edit side with the track (as `TrackBuffers` and `EdgeState` are), keep it in
`TrackModel`, and hand the snapshot a `shared_ptr` to it.

## Gotchas

- `mutex_` is recursive because plug-ins re-enter; don't "fix" it into a plain mutex.
- `rebuildSnapshotLocked()` is not cheap for big projects: it rebuilds everything. Calls the UI
  makes continuously (fader drags) must stay atomics.
- `setTrackSend()` to the master is refused (everything reaches the master anyway).
- A removed processor lives until no snapshot holds it *and* `idle()` runs (a render in the
  background holds its snapshot until it is finished). Tests that check plug-in destruction
  call `idle()`; many slow plug-ins take more than one call.
- Without a device, preview notes are discarded and transport commands applied by the edit
  side, so state stays consistent in tests that render offline.

## Tests

Engine behaviour is tested by the engine's own tests, [tests/engine](../../tests/engine) (no Qt;
one executable, `engine_tests`): [test_engine_render.cpp](../../tests/engine/test_engine_render.cpp)
(clips, gain/pan/mute/solo, loop, tempo changes, metronome, fades, sources, export, transport
state without a device, chains and moves), [test_render_jobs.cpp](../../tests/engine/test_render_jobs.cpp)
(renders in the background), and the area tests listed in each engine doc. See
[testing.md](../testing.md).

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
| [background-freeze.md](background-freeze.md) | A design (not implemented): caching unchanged strips' output in the background and playing it instead of their devices |
