# Automation (engine side)

How the engine plays automation envelopes: of device parameters (built-in devices and plug-ins alike), of the
mixer's volume, pan and send levels, and of a rack chain's fader. The envelope maths is in
[Automation.h](../../engine/src/Automation.h); parameters are described by `ParamInfo` in
[Processor.h](../../engine/src/Processor.h); the snapshot side is in
[EngineSnapshot.cpp](../../engine/src/EngineSnapshot.cpp) and the playing side in
[Renderer.cpp](../../engine/src/Renderer.cpp).

The user's view of automation (lanes, editing, lock envelopes, overrides) is in
[guide/automation.md](../guide/automation.md). The application layer's side (envelope editing, keys, `ParamSpec`, undo)
is in [app/model.md](../app/model.md); overrides are the engine bridge's ([below](#overrides-engine-side)).

## Overview

- Every automatable thing is automated the same way, in **normalized values (0..1)**. A device parameter is a
  `ParamInfo`, whatever the device: `toNormalized()` / `fromNormalized()` map plain values evenly, in log(value) or
  in whole steps (as VST3 maps stepped parameters, so plug-in values round-trip).
- The application layer describes every parameter, the mixer's included, as a `ParamSpec`
  ([app/src/model/ParamSpec.h](../../app/src/model/ParamSpec.h)) with the same mapping, and the tests hold the two to
  each other. Its copy of the envelope rules (curves, values between points) is namespace `automation` in
  [app/src/model/Automation.h](../../app/src/model/Automation.h): both must agree.
- Envelopes belong to a track or the master. The model keys them by target: `mixer:volume`, `mixer:pan`,
  `send:<return id>`, or `device:<device id>:<parameter id>` (and a rack chain's fader). Device ids are unique in a
  project, so a key finds its device wherever it sits. Deleting a device deletes its automation (in the same undo
  step; a model rule).
- The engine gets each track's envelopes (`setTrackAutomation`, track 0 is the master) and converts them to samples
  in the snapshot, as it does notes. The renderer then plays them sample-accurately: volume, pan and send levels
  sample by sample; device parameters at each breakpoint and every 64 samples along a slope.
- Automation is delayed along with a track's audio by the plug-in latency before it (delay compensation), and it is
  in exports.

## Files

| File | What it holds |
|---|---|
| [Automation.h](../../engine/src/Automation.h) | `AutomationPoint` (beats, as the UI sends it), `AutomationLaneDesc` (one envelope of a target), `AutomationNode` (samples, for the renderer); curve shape, segment value, index search, `fillAutomation`, `automationQuantize`; the mixer's mappings `automationVolumeGain` and `automationPan` |
| [Processor.h](../../engine/src/Processor.h) | `ParamInfo` and its normalized mapping; `ParamAutomation`; `Processor::automate()`, `clearAutomation()`, `automation()`, `numAutomation()` |
| [builtin/BuiltinProcessor.cpp](../../engine/src/builtin/BuiltinProcessor.cpp) | Built-in devices apply automation by rendering their block in pieces between changes |
| [plugins/Vst3Processor.cpp](../../engine/src/plugins/Vst3Processor.cpp) | Plug-ins get automation as sample-accurate VST3 parameter changes; the controller hears the last values in `idle()` |
| [Snapshot.h](../../engine/src/Snapshot.h) | `AutomationRender` (an envelope as the renderer plays it), `TrackParams` (fader atomics and smoothing), `EdgeRender::level` (a send's envelope), `StripRender::automation`, `volume`, `pan` |
| [EngineSnapshot.cpp](../../engine/src/EngineSnapshot.cpp) | `automationNodes()`, `buildAutomationLocked()` (faders), `buildChainLocked()` (device parameters and rack chain faders), send levels on edges |
| [EngineTracks.cpp](../../engine/src/EngineTracks.cpp) | `Engine::setTrackAutomation()` |
| [Renderer.h](../../engine/src/Renderer.h), [Renderer.cpp](../../engine/src/Renderer.cpp) | `automateInsert()`, `fillLane()`, `applyFader()`, `sumEdge()`, `automationTime()`, `kAutomationStep` |

## Key types

### `ParamInfo` and the normalized mapping

`ParamInfo` ([Processor.h](../../engine/src/Processor.h)) describes one parameter of any processor: `id`, `name`,
`unit`, `minValue`, `maxValue`, `defaultValue`, `logScale`, `valueLabels`, `steps`, `automatable`, `readOnly`,
`hidden`. Values are *plain*, in the parameter's own units, from `minValue` to `maxValue`.

- `stepCount()`: `steps` if set; else, for a list (`valueLabels` not empty), the number of labels minus one; else 0
  (continuous).
- `isLog()`: `logScale` and `minValue > 0` and `maxValue > minValue`. A log flag on a range that starts at 0 is ignored.
- `toNormalized(plain)`:
  - discrete: `round(plain - minValue)`, clamped to `0..count`, divided by `count`;
  - log: `log(plain / min) / log(max / min)`;
  - otherwise: `(plain - min) / (max - min)`.
- `fromNormalized(n)`:
  - discrete: `min + min(count, floor(n * (count + 1)))`. Each step owns an equal share of 0..1, which is how
    VST3 maps stepped parameters, so a plug-in's value round-trips;
  - log: `min * (max / min) ^ n`;
  - otherwise linear.

The application layer's `ParamSpec::fromInfo()` copies these fields into a `ParamSpec`, which maps the same way
(`toNormalized()`, `fromNormalized()`, `quantize()`). `ParamSpec` also has a `Scale::Fader` for the mixer's volume,
which uses `automation::volumeToNormalized()`.

Plug-ins present their parameters as plain values too: 0..1 for continuous ones and the step index for stepped ones
(see [plugins.md](plugins.md)). Their `minValue` is 0 and `maxValue` is 1 or the step count, so the mapping above is
VST3's own.

### Envelopes

- `AutomationPoint {beat, value, curve}`: what the UI sends. `value` is normalized; `curve` (-1..1) bends the segment
  that starts at this point (0 is a straight line).
- `AutomationLaneDesc {processorId, param, points}`: one envelope. `processorId` 0 is the mixer of the track (or
  master), with `param` `"volume"`, `"pan"` or `"send:<engine track id>"`. Otherwise `param` is the processor's
  parameter id; for a rack, `"chain:<chain id>:volume"` or `":pan"` (the chain's fader).
- `AutomationNode {time, value, curve}`: the same in timeline samples. `Engine::automationNodes()` converts at the
  current tempo (`llround(beat * samplesPerBeat)`, beats below 0 clamped), clamps value and curve, and
  stable-sorts by time, so two points at one time (a step) keep their order.
- `AutomationRender` ([Snapshot.h](../../engine/src/Snapshot.h)): the nodes plus where they go: `processor` (null for
  a mixer control), `param` (index), `steps` (its `stepCount()`), `insert` (its place in the strip's chain) and
  `latency` (see [Latency](#latency-and-delay-compensation)).

### Curve and value rules

- `automationShape(x, bend)`: `expm1(a*x) / expm1(a)` with `a = -bend * kAutomationCurvature` (6). Positive bend
  moves early and levels off. The shape is exponential, so a piece of a curved segment is again one (with the bend
  scaled by its share); this is what lets the model split segments exactly.
- `automationSegment(from, to, t)`: a breakpoint's curve bulges the line *upward* when positive, whichever way the
  segment goes (the bend is negated on a falling segment).
- `automationIndex(nodes, t)`: the number of breakpoints at or before `t` (an `upper_bound`).
- `automationValueAt()`: before the first breakpoint the envelope holds the first one's value, after the last the
  last one's; where two share a time (a step), the later one's from then on.
- `fillAutomation(nodes, t, n, out)`: values for `n` samples, one rule per run between breakpoints.
- `automationQuantize(value, steps)`: snaps to one of `steps + 1` values with the same equal-share rule as
  `fromNormalized`, so a discrete parameter moves in steps.

### The mixer's mappings

- Volume: `automationVolumeGain(v) = v^3 * kMaxVolumeGain`. 1 is +6 dB (`kMaxVolumeGain` = 1.99526231), about 0.79
  is 0 dB, 0 is silence, and a straight line is a smooth fade. The application's `automation::volumeToNormalized()`
  is the inverse, in dB, with a floor of -70 dB (`automation::kMinVolumeDb`).
- Pan: `automationPan(v) = v * 2 - 1`: -1 (left) at 0, 1 (right) at 1.
- A send's level maps as volume does (0..1, +6 dB at 1).

The application layer keeps its own copies (`automation::kCurvature`, `automation::kMaxVolumeDb`); its tests check them,
and its curve, against the engine's `kAutomationCurvature`, `kMaxVolumeGain` and `automationShape()`.

## How it works

```
 app layer                      edit side (main thread, engine lock)          audio thread
 ---------                      ------------------------------------          ------------
 envelopes (beats, 0..1)
   | EngineBridge::pushAutomation()
   v
 setTrackAutomation(track, lanes) --> TrackModel.automation
                                          | rebuildSnapshotLocked()
                                          v
                                   automationNodes(): beats -> samples
                                   buildAutomationLocked(): strip.volume / pan
                                   buildChainLocked():  device lanes, chain faders
                                   edges: send levels (EdgeRender::level)
                                          | atomic snapshot swap
                                          v
                                                                    processChain(): per slice,
                                                                      automateInsert() -> Processor::automate()
                                                                      process(); clearAutomation()
                                                                    processRack(): chain faders (applyFader)
                                                                    applyFader(): volume, pan per sample
                                                                    sumEdge(): send level per sample
```

### Building the snapshot

`Engine::setTrackAutomation(trackId, lanes)` replaces a track's lanes and rebuilds the snapshot. When a snapshot is
built:

- **Faders** (`buildAutomationLocked`): lanes with `processorId` 0 and `param` `volume` or `pan` become the strip's
  `volume` / `pan`. A track's fader latency is its input latency plus its devices' (`render.inputLatency +
  render.latency`); the master's is `snap->outputLatency()`.
- **Device parameters** (`buildChainLocked`): for each chain, each lane whose processor is in *that* chain becomes
  an `AutomationRender` with the parameter's index and step count, sorted by insert. The function recurses into
  rack chains, so a device inside a rack gets its lanes directly, however deep. Lanes of devices that are gone (or
  are on another track), of parameters a device doesn't have, or of read-only parameters are kept in the model but
  not played.
- **Rack chain faders**: a lane on the rack's processor id with `param` `chain:<chain id>:volume` or `:pan` becomes
  that `ChainRender`'s `volume` / `pan`. Its latency is the strip's input latency plus the end of the chain's devices.
- **Send levels**: for each send edge, the sending track's lane `send:<to track id>` becomes `EdgeRender::level`.

### Playing device parameters

Before each stretch a processor processes (the renderer splits a chunk into slices where the playhead jumps, such
as a loop wrap), `Renderer::processChain()` calls `automateInsert()` for each lane of that insert, then
`process()`, then `clearAutomation()`.

`automateInsert(lane, position, length, moving)`:

- While stopped (`moving` false), it hands over just the value at the playhead, at offset 0. So automated values
  follow the playhead when it is placed.
- While playing, it hands over the value at offset 0, then a value wherever it changes: at each breakpoint and
  every `kAutomationStep` (64) samples along a slope. Values are quantized for discrete parameters, and a value
  equal to the last one isn't sent again.

`Processor::automate(index, value, sampleOffset)` appends a `ParamAutomation {sampleOffset, index, value, order}` to
a buffer preallocated with `kMaxAutomation` (2048) entries; further ones in the same call are dropped (`automate`
returns false and `automateInsert` stops). How a processor applies them is its own business:

- `BuiltinProcessor::process()` sorts them by offset (then `order`, so each parameter's changes stay in order),
  applies the changes at the start, and calls `render()` for each stretch up to the next change, with the stretch's
  events (offsets made relative to it), `samplePos`, `beatPos` and sidechain pointers adjusted. Changes at or after
  the block's end are applied after it. The plain value is `fromNormalized(value)`, stored into the parameter's
  atomic, so a device's knob shows it too. See [devices.md](devices.md).
- `Vst3Processor::process()` adds each change to the block's input `IParameterChanges` at its offset (clamped into
  the block), so the plug-in gets sample-accurate VST3 parameter changes, and notes the last value per parameter.
  In `idle()` (main thread) it sends those last values to the controller, so the plug-in's editor follows, and
  reports `ParamsChanged` to the UI. Values a plug-in's controller echoes back with `performEdit` while it is told
  them are ignored, as they are no user edit. See [plugins.md](plugins.md).
- A new plug-in format's `Processor` applies what `automate()` hands it in `process()`.

### Playing faders and sends

Tracks, the master and rack chains share one fader, `Renderer::applyFader()`:

- With a volume lane it fills a buffer with the lane's value per sample (`fillLane`) and maps it with
  `automationVolumeGain`; with a pan lane, `automationPan` then `balanceGains`. These replace the manual gain and
  pan sample by sample.
- Live renders smooth the manual gain, pan and the audible (mute/solo) gain with `SmoothedValue`s that live in
  `TrackParams`, so they survive snapshot swaps. Where automation stops (or is overridden), the smoothing carries on
  from its last automated value (`snapTo` of the last sample), so there is no jump.
- Offline renders hold the engine lock, so nothing changes mid-render; they apply values directly and leave the live
  ramps alone.

`fillLane()` fills one value at the playhead while stopped. While playing it fills each timeline segment of the
chunk; the part of a chunk before the first segment (the end of a count-in) holds the first segment's start value.

A send's level is applied in `Renderer::sumEdge()`, where the return sums the send, after the edge's delay. With a
level lane the per-sample gain replaces the smoothed manual level, and the level snaps to the last automated value
afterwards, as a fader does. So a send's envelope is as late as the return hears its inputs.

### Latency and delay compensation

Each `AutomationRender` has a `latency`: how late its target hears the timeline. The renderer reads the envelope at
`automationTime(t, latency)` = `t - latency` (0 before that), so automation stays with the audio:

| Target | Latency |
|---|---|
| A device parameter | the strip's input latency plus the latency of the devices before it on the strip (`deviceLatency` of its slot; in a rack, the devices before it in its chain and before the rack) |
| A track's fader | input latency plus all its devices |
| The master's fader | `snap->outputLatency()` |
| A rack chain's fader | the strip's input latency plus where the chain's devices end |
| A send's level | as late as the return hears its inputs (it is applied after the edge's delay) |

A sidechained device that waits for its sidechain delays everything after it on its track; automation after it is
that much later too (tested in the sidechain tests). Offline renders render ahead by the output latency and drop it,
so exports are aligned.

## Overrides (engine side)

The engine has no notion of an override. The application layer's engine bridge
([app/src/audio/BridgeParameters.cpp](../../app/src/audio/BridgeParameters.cpp); see
[app/engine-bridge.md](../app/engine-bridge.md)) does it:

- `EngineBridge::overrideAutomation(owner, key)`: when an automated target is changed by hand (a knob, a fader, a
  plug-in's own editor, a chain's fader, a send knob), its `(owner, key)` goes into the bridge's `overridden` set and
  `pushAutomation()` sends the owner's lanes again *without* that one.
- `pushAutomation()` sends every envelope with points that isn't overridden (`engineLane()` turns each key into an
  `AutomationLaneDesc`), remembers which keys play (`automating`), and for keys that stopped playing calls
  `pushOwnValue()`: the model's own value of the target counts again (a device parameter is set back; the mixer is
  pushed again). Sends and chain faders keep the level the engine already has.
- `reEnableAutomation(owner)` clears the overrides (everywhere, with no owner, or for one owner) and pushes again.
- `isAutomated()` and `isOverridden()` drive the red dots and the grey envelopes in the UI
  (`automationStateChanged(owner)` tells it when they change).
- A send automated before it was set is made, silent, so that its automation plays (`onAutomationChanged()` pushes
  the owner's sends first).

The playing side needs nothing for this: a target without a lane falls back to its atomic value, and the fader and
send smoothing carry on from the last automated value.

## Racks and macros

- Devices in racks are automated like any of the track's devices; their lanes stay keyed by device id, so they
  keep their automation when grouped, ungrouped or moved within the track. A processor moved to another track keeps
  its state, but in the engine its automation stays with the strip it came from (and plays again if it comes back).
  When the user drags a device to another track, the model moves its lanes to that track, and the bridge sends both
  tracks' automation again.
- A chain's volume and pan are automated on the rack's processor id (`chain:<chain id>:volume` / `:pan`), in time
  with the latency before them. The model describes them as `ParamSpec`s with `chainSpecs()` (*Chain Volume*,
  *Chain Pan* under the rack in a lane's device chooser).
- **Macros are not engine parameters.** The engine's `RackProcessor` ([Rack.h](../../engine/src/Rack.h)) has no
  parameters. A rack's eight macros (`macro1`..`macro8`) live in the model (`Device::params` of the rack and its
  `MacroMapping`s); turning one sets the mapped parameters through the editor, as one undo step. The bridge offers
  only a rack's chain faders for automation (`EngineBridge::deviceParamSpecs()`), so macros themselves can't be
  automated today.

## The mixer's control names

The mixer is `processorId` 0 with a name: `volume`, `pan`, `send:<engine track id>`. More (mute, say) would be more
names: a branch in `buildAutomationLocked()` and a target in `StripRender`, plus, in the application layer, a
`ParamSpec` in `mixerSpecs()` ([ParamSpec.h](../../app/src/model/ParamSpec.h)), a key in
[Automation.h](../../app/src/model/Automation.h) (`automation::kMixerKeys`) and its lane in
`EngineBridge::engineLane()`.

## Invariants and real-time rules

- Nothing on the audio thread allocates: `automate()` writes into a preallocated buffer; fader and send lanes fill
  per-worker scratch (`autoGain`, `autoPanLeft`, `autoPanRight`, `edgeGain`).
- Envelopes are immutable once in a snapshot. A tempo change rebuilds the snapshot, so breakpoints move with their
  beats.
- A processor's automation buffer belongs to whichever thread is processing it; a processor is processed by one
  thread at a time (the scheduler runs a track's strip on one thread).
- Changes beyond `kMaxAutomation` per call are dropped. With 64-sample steps and `Renderer::kMaxBlock` = 1024 that is
  far more than one lane needs; it bounds pathological cases (hundreds of breakpoints in a block).

## Extending it

- **A new built-in device**: list its `ParamInfo`s and subclass `BuiltinProcessor`. Automation, the device view's
  knobs and saving then work at once. See [devices.md](devices.md).
- **A new plug-in format**: its `Processor` reads `automation()` / `numAutomation()` in `process()` and applies the
  changes (they are normalized; `ParamInfo::fromNormalized` gives plain values). See [plugins.md](plugins.md).
- **A new mixer target**: see [above](#the-mixers-control-names).

## Gotchas

- The engine's and the application layer's curve maths must stay identical: `automationShape()` and
  `automation::shape()` ([app/src/model/Automation.h](../../app/src/model/Automation.h)).
- `ParamInfo::isLog()` silently ignores `logScale` on a range that starts at or below 0.
- A VST3 parameter queue (`HostParamQueue`) holds 16 points per block; when full, the latest value replaces the last
  point. With 64-sample steps a 1024-sample block fits.
- A discrete parameter is quantized on the engine side (`automationQuantize`) even if a lane's values lie between
  steps.
- Stopped, the renderer still hands processors the value at the playhead each block, so moving the playhead moves
  automated controls.

## Tests

- [tests/engine/test_automation_engine.cpp](../../tests/engine/test_automation_engine.cpp): volume and pan (tracks
  and master) sample by sample, a volume lane replacing the fader until removed, mute over automation, curves, device
  parameters split exactly where they change, discrete parameters in whole steps, envelopes of missing devices or
  parameters ignored, following tempo, and the normalized mapping.
- [tests/app/test_automation_model.cpp](../../tests/app/test_automation_model.cpp): the application layer's curve and
  volume law against the engine's, and device parameters mapping as the engine does.
- [tests/engine/test_sends_engine.cpp](../../tests/engine/test_sends_engine.cpp): send automation in time.
- [tests/engine/test_racks_engine.cpp](../../tests/engine/test_racks_engine.cpp): automation of a nested device and of
  a chain's fader in time behind latent devices.
- [tests/engine/test_sidechain_engine.cpp](../../tests/engine/test_sidechain_engine.cpp): a device after one that waits
  for its sidechain keeps its automation in time.
- [tests/engine/test_vst3_engine.cpp](../../tests/engine/test_vst3_engine.cpp): automation reaching the plug-in and its
  controller, and the master's device automation in time.
- [tests/app/test_ui_arrangement_automation.cpp](../../tests/app/test_ui_arrangement_automation.cpp) and
  [tests/app/test_bridge_tracks.cpp](../../tests/app/test_bridge_tracks.cpp): overriding and re-enabling, controls
  following automation, a send's level changed by hand overriding its envelope.

See [testing.md](../testing.md) for the whole suite.
