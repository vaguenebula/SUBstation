# Devices and the `Processor` interface

Every device on a track's chain (built-in instruments and effects, VST3 plug-ins, racks) is a `Processor`
([Processor.h](../../engine/src/Processor.h)), so the renderer never needs to know where one came from. Built-in
devices share a base class, `BuiltinProcessor`, and register themselves in `BuiltinRegistry`
([engine/src/builtin/](../../engine/src/builtin)); each one is a single `.cpp` in
[builtin/devices/](../../engine/src/builtin/devices).

How the user works with devices (the device view, racks, presets, folding, cut/copy/paste) is in
[guide/devices.md](../guide/devices.md). Plug-in hosting is in [plugins.md](plugins.md); racks and chains in
[routing.md](routing.md); the device view (QML and its scene-graph items) in [ui/device-view.md](../ui/device-view.md).

## Files

| File | What it holds |
|---|---|
| [Processor.h](../../engine/src/Processor.h) | `ProcessEvent`, `EventList`, `ProcessContext`, `ParamInfo`, `DisplayInfo`, `ParamAutomation`, `ProcessorEvent`, `Processor` |
| [builtin/BuiltinProcessor.h](../../engine/src/builtin/BuiltinProcessor.h) / [.cpp](../../engine/src/builtin/BuiltinProcessor.cpp) | parameters as atomics (`param`, `isOn`, `choice`, `choiceIndex`, `offOnLabels`), automation by splitting blocks, displays, state as named text values, `loadSource()` |
| [builtin/Dsp.h](../../engine/src/builtin/Dsp.h) | DSP the devices share, all inline: the state-variable filter section (`dsp::Svf`, `dsp::SvfCoefficients`), `hermite`, `followPeak`, `flushTiny`, the instruments' envelopes (`riseStep`, `fallCoefficient`, `kSilent`, `kFadeTo`), `renderBetweenNotes` |
| [builtin/DspBlocks.h](../../engine/src/builtin/DspBlocks.h) | Building blocks the effects share, beside Dsp.h's, all inline: one-pole and DC filters (`dsp::OnePole`, `onePoleCutoff`, `DcBlocker`), a delay line (`DelayLine`, read linearly or with Hermite interpolation), an envelope follower (`EnvelopeFollower`), white noise (`Noise`), `fastTanh`, an LFO with Ableton's shapes and synced rates (`Lfo`, `LfoShape`, `syncedDivisionLabels`, `syncedCycleBeats`), RBJ biquads (`BiquadCoefficients`, `Biquad`), a Linkwitz-Riley crossover and its all-pass (`Crossover`, `CrossoverAllpass`), a sliding maximum for lookahead (`SlidingMax`), linear-phase oversampling by 2, 4 or 8 (`Oversampler`) |
| [builtin/BuiltinRegistry.h](../../engine/src/builtin/BuiltinRegistry.h) / [.cpp](../../engine/src/builtin/BuiltinRegistry.cpp) | `BuiltinRegistry`, `BuiltinInfo`, `BuiltinCategory`, `SUB_REGISTER_BUILTIN` |
| [builtin/devices/Synth.cpp](../../engine/src/builtin/devices/Synth.cpp) | the Synth instrument |
| [builtin/devices/Sampler.cpp](../../engine/src/builtin/devices/Sampler.cpp) | the Sampler instrument |
| [builtin/SampleSlicing.h](../../engine/src/builtin/SampleSlicing.h) / [.cpp](../../engine/src/builtin/SampleSlicing.cpp) | The Sampler's slicing (`detectOnsets`, `sliceStarts`) and Snap (`nearestZeroCrossing`), shared with the application layer's `sampleSlices` ([app/src/audio/SampleSlices.h](../../app/src/audio/SampleSlices.h)) for the editor |
| [builtin/devices/Utility.cpp](../../engine/src/builtin/devices/Utility.cpp) | Utility: gain, pan, width |
| [builtin/devices/Ott.cpp](../../engine/src/builtin/devices/Ott.cpp) | Over The Top: multiband upward/downward compression |
| [builtin/devices/Compressor.cpp](../../engine/src/builtin/devices/Compressor.cpp) | Compressor, with sidechain and displays |
| [builtin/devices/Gate.cpp](../../engine/src/builtin/devices/Gate.cpp) | Gate: threshold, return, hold, attack and release, floor, flip, lookahead; keyed by its input or a sidechain through an EQ; listening to the key; displays |
| [builtin/GateDesign.h](../../engine/src/builtin/GateDesign.h) | The Gate's maths (`gate::floorGain`, `closeDb`, `pass`, `gain`, `lookaheadSamples`, `keyFilter`, `keyFilterDb`, the key EQ's types, the parameters' ranges), shared with the application layer's [GateResponse.h](../../app/src/audio/GateResponse.h) for the editor's meter, key EQ curve and drags; and, not shared with it, how a key EQ filter starts warm (`kWarmSeconds`, `kWarmPace`, `warmFrames`: the device's and its tests') |
| [builtin/devices/Limiter.cpp](../../engine/src/builtin/devices/Limiter.cpp) | Limiter: brick-wall lookahead limiting, Soft Clip, True Peak, L/R or M/S with Link, Maximize, its displays |
| [builtin/LimiterDesign.h](../../engine/src/builtin/LimiterDesign.h) | The Limiter's scales, Soft Clip's knee, the M/S ceiling, the true-peak interpolator and its parabola (`limiter::scales`, `knee`, `shape`, `sharedCeiling`, `truePeakPhases`, `refinedPeak`), shared with the application layer's `LimiterResponse.h` for the editor |
| [builtin/devices/Multiband.cpp](../../engine/src/builtin/devices/Multiband.cpp) | Multiband Dynamics: a three-way Linkwitz-Riley split, Above and Below per band, Peak and RMS detectors, split switches, activators and solos, a per-band sidechain and Listen, glides on a 32-sample grid, displays |
| [builtin/MultibandDesign.h](../../engine/src/builtin/MultibandDesign.h) | Multiband Dynamics' gain law (`multiband::aboveGainDb`, `belowGainDb`, `bandGainDb`, `staticGainDb`), ranges, display rate and detector windows (`windowSeconds`), shared with the application layer's `MultibandResponse.h` for the editor |
| [builtin/devices/Spectral.cpp](../../engine/src/builtin/devices/Spectral.cpp) | Spectral Compressor: an STFT (Hann, 4x overlap, each frame's work spread over the hop after it, latency N + H), per-bin levels smoothed across frequency and followed over time, Stereo Link, the gain computer both ways with the Focus band, Delta, sidechain keying, displays |
| [builtin/SpectralDesign.h](../../engine/src/builtin/SpectralDesign.h) | The Spectral Compressor's maths (`spectral::frameSize`, `latencySamples`, `calibrationDb`, `pinkDb`, `thresholdDb`, `belowDb`, `focusWeight`, `smoothingOctaves`, `gainDb`, `displayFrequency`), shared with the application layer's `SpectralResponse.h` for the editor |
| [builtin/devices/Saturator.cpp](../../engine/src/builtin/devices/Saturator.cpp) | Saturator: eight curves, Color's emphasis and its inverse, Post Clip on the dry/wet blend, DC, 4x oversampling of all of it |
| [builtin/SaturatorDesign.h](../../engine/src/builtin/SaturatorDesign.h) | The Saturator's curves (`saturator::makeShape`, `params`, `curve`, `transfer`), Post Clip and Color's design (`colorDesign`, `inverse`, `between`, `colorResponseDb`, `colorTimeConstant`), shared with the application layer's `saturatorCurve()` and `saturatorColorDb()` for the editor |
| [builtin/devices/Amp.cpp](../../engine/src/builtin/devices/Amp.cpp) | Amp: seven amp models, the tone stack, sag, Mono and Dual, sleep |
| [builtin/AmpDesign.h](../../engine/src/builtin/AmpDesign.h) | The Amp's design (`amp::kVoicings`, `blend`, the stages' curve and `Adaa`, the tone stack, the dials' mappings, `Transfer`: a tone's steady state by harmonic balance, from tables a rate and a voicing set that can be worked out beforehand (`BasicTransfer`'s `Rate` and `Voice`), the displays' `kDisplaySamples` and `kDisplayFloorDb`), shared with the application layer's `ampToneResponseDb()`, `ampTransfer()` and `AmpTransferCurve` for the editor's curves (and `ampModelNames()`, `ampDisplayFloorDb()`); the device keeps its model morph level-matched with a `BasicTransfer<64>` |
| [builtin/devices/Erosion.cpp](../../engine/src/builtin/devices/Erosion.cpp) | Erosion: a 2 ms delay modulated by a sine or band-passed noise, Noise Blend and Stereo Width, chunked glides and ramps, displays |
| [builtin/ErosionDesign.h](../../engine/src/builtin/ErosionDesign.h) | Erosion's maths (`erosion::band`, `noisePowerGain`, `bandMagnitude`, `bandEdges`, `excursionSamples`, `blendWeights`, `stereoSpread`, the instances' noise salts), shared with the application layer's `ErosionResponse.h` for the editor |
| [builtin/devices/Delay.cpp](../../engine/src/builtin/devices/Delay.cpp) | Delay: synced or free times per side, filter, modes, ping pong, freeze |
| [builtin/devices/Chorus.cpp](../../engine/src/builtin/devices/Chorus.cpp) | Chorus-Ensemble: modulated delays in three modes (Chorus with Taps and Time, Ensemble, Vibrato), feedback with Invert, Warmth, the high-pass split, Width |
| [builtin/ChorusDesign.h](../../engine/src/builtin/ChorusDesign.h) | The Chorus-Ensemble's voices (`chorus::layout`, `centreMs`, `swingMs`, `voicePhase`, `lfoValue`, `delayMs`, the detune), its warmth's curve and filter and its feedback's limiter, shared with the application layer's `chorusDelayMs()` and neighbours for the editor's graph |
| [builtin/devices/Phaser.cpp](../../engine/src/builtin/devices/Phaser.cpp) | Phaser-Flanger: a phaser, flanger and doubler, two LFOs and an envelope follower, feedback, Warmth, Safe Bass, displays |
| [builtin/PhaserDesign.h](../../engine/src/builtin/PhaserDesign.h) | The Phaser-Flanger's maths (`phaser::` modes and LFO shapes `waveValue`, the modulation's mappings `phaserCenterHz`, `phaserQ`, `delayMs`, the response `transfer`/`responseDb`, `notchFrequencies`, `curve`), shared with the application layer's `PhaserResponse.h` for the editor's graph |
| [builtin/devices/Reverb.cpp](../../engine/src/builtin/devices/Reverb.cpp) | Reverb: input filter, early reflections with Spin, diffusers, a 4/8/16-line feedback delay network with per-band decay, chorus, Freeze/Cut/Flat, the guard, Density's crossfade, sleeping |
| [builtin/ReverbDesign.h](../../engine/src/builtin/ReverbDesign.h) | The Reverb's design: its constants and layouts, the loops' decay per band (`reverb::rates`, `loop`, `decaySecondsAt`, `decaySeconds`), the input filter (`inputFilterDb`), the reflections (`earlyTaps`, `spinPan`), sizes (`sizeFactor`, `onsetMs`, ...), shared with the application layer's `ReverbResponse.h` for the editor |
| [builtin/devices/Disperser.cpp](../../engine/src/builtin/devices/Disperser.cpp) | Disperser: up to 64 all-pass stages, glides and fades, bypass |
| [builtin/DisperserDesign.h](../../engine/src/builtin/DisperserDesign.h) | The Disperser's stages (`disperser::design`, `process`, `groupDelayMs`), shared with the application layer's `disperserGroupDelayMs()` for the editor's graph |
| [builtin/devices/Eq.cpp](../../engine/src/builtin/devices/Eq.cpp) | EQ: 24 bands, placement, output gain, gain scale |
| [builtin/EqDesign.h](../../engine/src/builtin/EqDesign.h) | The EQ's filter design (`eq::design`, `eq::responseDb`), shared with the application layer's `eqResponseDb()` for the editor's curves; `eq::Biquad::tick` (a sample through a section, transposed direct form II: the EQ's and the Sidechain's crossover) |
| [builtin/devices/Sidechain.cpp](../../engine/src/builtin/devices/Sidechain.cpp) | Sidechain: a curve from each hit in the key (or on the beat), lookahead, lows only |
| [Rack.h](../../engine/src/Rack.h) | `RackProcessor`: a rack's place in a chain (its chains are run by the renderer) |
| [rt/RtUtils.h](../../engine/src/rt/RtUtils.h) | `SmoothedValue`, `SpscQueue`, `DisplayStream`, `dbToGain`, `gainToDb`, `expDbToGain`, `onePoleCoefficient`, `balanceGains` |
| [EngineChains.cpp](../../engine/src/EngineChains.cpp) | `addBuiltinProcessor()`, and the processor calls the engine bridge makes (`setProcessorParam`, `processorState`, `setProcessorState`, `processorDisplays`, `readProcessorDisplay`, ...) |
| [app/src/model/Devices.h](../../app/src/model/Devices.h) | The application layer's view of the registry: `BuiltinDevice`, `builtinDevices()`, `builtinCategories()`, `builtinParamInfo()` |
| [ui/qml/devices/editors/](../../ui/qml/devices/editors) | Built-in devices' own editors (QML, drawing with items in [ui/src/devices](../../ui/src/devices)), registered by kind |

## The `Processor` interface

### Threads

`process()` and `reset()` run on the rendering thread (the audio thread, a worker, or the thread rendering offline).
Everything else is called from the main (UI) thread, as plug-in formats require, unless noted. `getParam()`,
`requestReset()`, `takeResetRequest()`, `setEnabled()`/`isEnabled()` and `readDisplay()` are thread-safe.

### Members

| Member | Meaning |
|---|---|
| `typeId()` | `"builtin:utility"`, `"vst3:<class id>"`, `"rack"` |
| `name()` | shown name |
| `prepare(sampleRate, maxBlockSize)` | before the processor is first published to the audio thread, and again with audio stopped whenever the sample rate changes. `maxBlockSize` is `Renderer::kMaxBlock` (1024) |
| `reset()` | real-time: silences the processor (notes, tails, filter state). Only the renderer calls it, before a block, when a reset was requested |
| `resetOffline()` | clears what a real-time reset can't (a plug-in's reverb tail), not being processed; around offline renders |
| `process(ctx, channels, numChannels, numFrames)` | real-time: planar buffers in place (the renderer passes 2 channels) |
| `latencySamples()` / `tailSamples()` | latency is compensated by delaying the other tracks (and sidechains) |
| `params()`, `getParam()`, `setParam()` | plain (not normalized) values |
| `paramText(index, value)` | the processor's own text for a value; `""` leaves it to the UI |
| `getState()` / `setState()` | everything needed to restore it; empty for processors whose parameters are their whole state |
| `idle()` | main-thread housekeeping, called regularly; true if the latency changed (the engine realigns) |
| `takeEvents(out)` | `ProcessorEvent`s for the UI (edits in a plug-in's editor, closed editors, ...) |
| `hasEditor()`, `openEditor()`, `closeEditor()`, `isEditorOpen()`, `setEditorVisible()`, `setEditorTitle()` | a plug-in's own editor window |
| `setEnabled(bool)` | switching off also requests a reset, so when it comes back on it can't resume notes whose note-offs it missed |
| `requestReset()` / `takeResetRequest()` | asks whichever renderer processes it next to `reset()` it first |
| `automate()`, `clearAutomation()`, `automation()`, `numAutomation()` | automation for one `process()` call; see [automation.md](automation.md) |
| `hasSidechain()`, `setSidechain()`, `setSidechainConnected()`, `sidechain(c)`, `sidechainConnected()` | the sidechain (aux) input; see below |
| `displays()`, `readDisplay()` | streams of values for a device's own editor |

### Events and context

- `ProcessEvent`: `NoteOn`, `NoteOff` (`data[0]` key, 60 = C3; `data[1]` velocity, 0 for note-offs; `data[2]`
  channel) or raw `Midi` bytes, at a `sampleOffset`. The renderer sends each track's notes to every processor on the
  track (audio effects ignore them), sorted by offset, note-offs before note-ons at the same offset. A note-on with
  velocity 0 counts as a note-off (`startsNote()`, `endsNote()` say which an event is). Plug-in adapters translate
  them to their format's events.
- `ProcessContext`: sample rate, `samplePos` and `beatPos` of the block's first frame, tempo, time signature,
  `playing`, `looping` with loop start and end beats, `offline`, and `inEvents`; `samplesPerBeat()` and
  `beatsPerBar()` from them. The renderer splits blocks where the playhead jumps (a loop wrap), so a block is always
  one continuous stretch.

### Sidechain

A processor with an aux input returns true from `hasSidechain()`. Before each `process()` call the renderer hands it
what goes into that input over the call's frames (two channels, `setSidechain(left, right)`), or null for silence (no
source, or solo leaves it out); after the call, null again. `setSidechainConnected()` says whether a source was chosen
at all, so a device can key from its own input without one, yet hear silence when solo leaves its source out. Routing
and alignment of sidechains are in [routing.md](routing.md).

### Displays

What a device's own editor draws besides its parameters (a meter's readings, a curve, samples for an analyser) is a
stream of floats: `displays()` lists them as `DisplayInfo {id, samplesPerValue}` (1 for samples, more for a meter);
`readDisplay(index, position, out)` appends stream `index`'s values since `position` (0 at first) and returns where
to read from next. Any thread may call it, and any number of readers can follow a stream.

Built-in devices back each display with a `DisplayStream` ([rt/RtUtils.h](../../engine/src/rt/RtUtils.h)): the writer
never waits and never fails (it overwrites the oldest values); each reader keeps its own position, and one that falls
behind skips to the latest 8192 values (`kDisplayCapacity`). A reader overtaken while it reads may get a newer value
in an older one's place, which is harmless for drawing.

The engine's API has them by processor id: `Engine::processorDisplays(id)` and
`Engine::readProcessorDisplay(id, index, position, out)` (appends to a `std::vector<float>` and returns the next
position). The engine bridge wraps them by track and device id (`EngineBridge::processorDisplays()`,
`readProcessorDisplay()`) for the editors.

## `BuiltinProcessor`

What every built-in device shares ([BuiltinProcessor.h](../../engine/src/builtin/BuiltinProcessor.h)):

- **Parameters.** A fixed `ParamInfo` list (a static the device owns: it must outlive the processor) is passed to the
  constructor. Values are atomics initialised to the defaults; the UI sets them (`setParam` clamps to the range),
  the rendering thread reads them with `param(index)`, a switch with `isOn(index)` (on from 0.5, as automation plays
  it: `automationSwitchOn`), a list or stepped value with `choice<Enum>(index)` or `choiceIndex(index)` (rounded).
  A switch's labels are `offOnLabels()`. `params()`, `getParam()`, `setParam()`, `process()`,
  `displays()`, `readDisplay()`, `getState()` and `setState()` are `final`.
- **Automation.** `process()` renders the block in stretches between the points where automation changes a value,
  calling the device's `render()` for each, with that stretch's events (offsets relative to it), position and beat
  position, and sidechain pointers (`sidechain(c)` in a `BuiltinProcessor` already points at the stretch). Up to 8
  channels; up to 2048 events per stretch. Without automation it calls `render()` once. See
  [automation.md](automation.md).
- **Displays.** A device passes `DisplayInfo`s to the constructor and calls `publish(index, value)` from `render()`.
- **State besides parameters** (a sampler's sample) is kept as named text values: the device implements
  `stateValues()` / `setStateValues()`. `getState()` encodes them as lines `name=value` (UTF-8; a backslash escapes a
  backslash or a newline: `\\`, `\n`); empty means none, the defaults. `setStateValues()` is called on the main
  thread or on the thread the UI loads states on; it may take its time (loading files) and throws if it can't do it
  all. The model keeps the state base64-encoded in `Device::state`, as it keeps a plug-in's
  ([app/src/model/DeviceState.h](../../app/src/model/DeviceState.h), namespace `deviceState`, has the same encoding),
  so it saves, loads and undoes as a plug-in's does.
- **Audio files** come through `loadSource(path)` (not real-time). The engine sets a `SourceLoader` on every built-in
  device it creates (`Engine::addBuiltinProcessor`), so files are shared with the engine's cache and decoded at the
  engine's rate; without one (a device made outside an engine) it decodes the file at its own rate. See
  [warp.md](warp.md#the-source-cache).

## `BuiltinRegistry`

A device registers itself where it is defined: each `.cpp` in `builtin/devices/` ends with

```cpp
SUB_REGISTER_BUILTIN(MyProcessor, AudioEffect);   // or Instrument
```

The macro makes a static `BuiltinRegistrar` that adds a factory (`std::make_shared<MyProcessor>()`) and a
`BuiltinCategory` to `BuiltinRegistry::instance()` (a function-local static, built on first use, since registrars run
in no fixed order).

- `devices()` builds the list once (`std::call_once`): it makes one prototype of each device and records its id
  (its `typeId()` without the `builtin:` prefix), name, category and parameters as a `BuiltinInfo`; instruments come
  first, then by name.
- `create(id)` makes a new device of that id; `std::invalid_argument` for an unknown one.

The engine creates devices by id (`Engine::addBuiltinProcessor(chainId, type, index)`: it sets the source loader,
calls `prepare()` before the audio thread can see the processor, and inserts it). The application's device list,
names, categories and parameter defaults all come from the registry (`BuiltinRegistry::instance().devices()`): the
application layer's `builtinDevices()`, `builtinCategories()` and `isInstrument()`
([app/src/model/Devices.h](../../app/src/model/Devices.h)) and the browser's *Built-in* category are built from it. A
model `Device` of a built-in device has the registry id as its `kind`.

Each device's translation unit only registers itself: nothing refers to it. A linker takes from a static library
only the objects something refers to, so a program linking `sub_engine` would leave the devices out and they would
never register. So [engine/CMakeLists.txt](../../engine/CMakeLists.txt) collects the files with
`file(GLOB BUILTIN_DEVICE_SOURCES CONFIGURE_DEPENDS src/builtin/devices/*.cpp)`, gives each one an empty anchor
function named after it (`SUB_BUILTIN_ANCHOR`, such as `subBuiltinAnchor_Utility`, which `SUB_REGISTER_BUILTIN`
defines), and writes `generated/BuiltinDevices.cpp`, whose `linkBuiltinDevices()` calls them all.
`BuiltinRegistry::instance()` calls `linkBuiltinDevices()`, so every device file is linked in.

## The built-in devices

All parameter tables below are the devices' `ParamInfo`s: id, name, unit, range, default; "log" means `logScale`.
Mixer-like controls (`gain`, `volume`, `width`, `depth`) are smoothed with `SmoothedValue` ramps (20 or 30 ms) so
turning or automating them doesn't zipper.

### Synth (`builtin:synth`, Instrument)

A polyphonic subtractive synth: 16 voices (`kMaxVoices`), one oscillator each, an ADSR amplitude envelope and a
resonant low-pass filter. Mono, on both channels. Velocity sets the level.

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `wave` | Wave | | 0..3: Sine, Triangle, Saw, Square (a list) | Saw |
| `attack` | Attack | ms | 1..5000, log | 3 |
| `decay` | Decay | ms | 1..5000, log | 300 |
| `sustain` | Sustain | % | 0..100 | 70 |
| `release` | Release | ms | 1..10000, log | 200 |
| `cutoff` | Cutoff | Hz | 20..20000, log | 4000 |
| `resonance` | Resonance | % | 0..100 | 10 |
| `volume` | Volume | dB | -60..6 | 0 |

- Oscillators: sine, triangle, and band-limited saw and square (PolyBLEP: the step at phase 0 is smoothed over one
  sample on each side, which removes most of the aliasing of a naive saw or square).
- Envelope: attack is a linear ramp; decay and release are exponential, with their times to -60 dB; decay ends at,
  and holds, the sustain level (and follows it if it changes). A releasing voice ends at -80 dB.
- Filter: a TPT state-variable low-pass (Zavalishin/Simper), stable at any cutoff and resonance. The cutoff glides to
  its target (about 5 ms) with coefficients per sample, so turning the knob doesn't step; it is limited to 0.45 of the
  sample rate. Resonance maps to damping from 2 (none) to 0.1 (Q = 10) and applies per block.
- Voices: a note-on takes a free voice, else the quietest releasing one, else the oldest. With the same key held
  twice, the older note ends first. One voice's peak at full velocity is 0.25.
- It *writes* (does not add) its output: it is the first device on a MIDI track. With no events and no active voice it
  writes silence and skips the work. `tailSamples()` is the release time.

### Sampler (`builtin:sampler`, Instrument)

Plays one audio file as Ableton's Simpler does, in one of three modes. Up to 32 voices
sound at once (`kMaxVoices`), plus 8 slots for voices cut short (`kVoiceSlots`): a voice
stolen, or a mono note replaced, fades out over 4 ms (`kKillSeconds`) instead of
clicking.

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `mode` | Mode | | Classic, 1-Shot, Slice | Classic |
| `root` | Root Key | note | 0..127, 127 steps | 60 |
| `tune` | Transpose | st | -48..48, 96 steps | 0 |
| `fine` | Detune | ct | -100..100 | 0 |
| `start` | Start | % | 0..100 | 0 |
| `end` | End | % | 0..100 | 100 |
| `gain` | Gain | dB | -24..24 | 0 |
| `reverse` | Reverse | | Off, On | Off |
| `snap` | Snap | | Off, On | Off |
| `warp` | Warp | | Off, On | Off |
| `warp_beats` | Warp Length | beats | 1..256, whole beats | 4 |
| `warp_mode` | Warp Mode | | Transients, Standard, Smooth, Formants, Re-Pitch | Standard |
| `loop` | Loop | | Off, On | Off |
| `loop_start` | Loop Start | % | 0..100 | 0 |
| `loop_fade` | Loop Fade | % | 0..100 (of the loop) | 0 |
| `attack` | Attack | ms | 0.1..5000, log | 1 |
| `decay` | Decay | ms | 1..10000, log | 1000 |
| `sustain` | Sustain | % | 0..100 | 100 |
| `release` | Release | ms | 1..10000, log | 50 |
| `voices` | Voices | # | 1..32, whole | 32 |
| `glide` | Glide | ms | 0..2000 | 0 |
| `trigger` | Trigger Mode | | Trigger, Gate | Trigger |
| `fade_in` | Fade In | ms | 0.1..2000, log | 0.1 |
| `fade_out` | Fade Out | ms | 0.1..2000, log | 0.1 |
| `slice_by` | Slice By | | Transient, Beat, Region | Transient |
| `sensitivity` | Sensitivity | % | 0..100 | 50 |
| `slice_beat` | Slice Division | | 1/16, 1/8, 1/4, 1/2, 1 Bar, 2 Bars, 4 Bars | 1/8 |
| `regions` | Regions | # | 2..64, whole | 8 |
| `playback` | Playback | | Mono, Poly, Thru | Mono |
| `filter` | Filter | | Off, On | Off |
| `filter_type` | Filter Type | | Low-pass, High-pass, Band-pass, Notch | Low-pass |
| `filter_slope` | Filter Slope | | 12 dB, 24 dB | 24 dB |
| `filter_freq` | Filter Freq | Hz | 20..22000, log | 22000 |
| `filter_res` | Resonance | % | 0..100 | 0 |
| `lfo` | LFO | | Off, On | Off |
| `lfo_wave` | LFO Wave | | Sine, Triangle, Saw Up, Saw Down, Square, Random | Sine |
| `lfo_sync` | LFO Sync | | Off, On | Off |
| `lfo_rate` | LFO Rate | Hz | 0.01..30, log | 1 |
| `lfo_beats` | LFO Synced Rate | | 1/32, 1/16, 1/8, 1/4, 1/2, 1 Bar, 2 Bars, 4 Bars, 8 Bars | 1/4 |
| `lfo_retrig` | LFO Retrigger | | Off, On | Off |
| `lfo_volume` | LFO > Volume | % | 0..100 | 0 |
| `lfo_pitch` | LFO > Pitch | ct | 0..1200 | 0 |
| `lfo_filter` | LFO > Filter | % | 0..100 | 0 |
| `lfo_pan` | LFO > Pan | % | 0..100 | 0 |
| `pan` | Pan | | -1..1 | 0 |
| `velocity` | Vol < Vel | % | 0..100 | 50 |
| `volume` | Volume | dB | -60..6 | 0 |

The defaults are the Sampler before it had modes (Classic, everything else off), so a
project saved then plays as it did: a parameter it doesn't have takes its default.

- **The sample as it plays** (`Reader`, built per block): frames are counted in the
  order the sample plays, so Reverse reads it backwards (`frames - 1 - i`) and every
  marker (Start, End, Loop Start, slices) is a place in it as it plays. Outside the file
  it is silent. Start and End are `percent / 100 * frames` (truncated); with Snap each
  moves to the nearest zero crossing of the channels summed within 10 ms
  (`slicing::nearestZeroCrossing`, the earlier of two as near; never off either end).
- **Modes**:
  - *Classic*: a voice plays from Start, pitched by `key - root`; with Loop (and End
    after Start) it wraps from End to `max(Loop Start, Start)` (snapped), else it ends
    at End. Loop Fade crossfades the loop's last `fade` frames (`loop_fade` % of the
    loop, at most as many as come before Loop Start) into the frames before Loop Start,
    equal power, so the frame after End is the one at Loop Start. ADSR as the Synth's.
    Voices limits the notes: the quietest releasing one, else the oldest, is cut short
    to make room. One voice: a new note cuts the last; with Glide too, notes overlapping
    are legato: the voice goes on, its pitch moving linearly (in semitones) to the new
    key over the glide time, and back to the newest key still held when the one playing
    is let go (the keys held are kept in order, even while there is no sample).
  - *1-Shot*: one voice at a time, pitched; the envelope is Fade In (linear from 0),
    then held. Trigger ignores the note-off; Gate fades out over Fade Out (linear) from
    it. Either way it fades out linearly over the Fade Out before End, in played time.
  - *Slice*: at a note-on the slices of Start..End are worked out
    (`slicing::sliceStarts`): every Onset with a strength of at least `1 - sensitivity`
    and at least 10 ms from the slice before and from End (Transient); the region's
    beats (`warp_beats * (End - Start) / frames`) divided by the division (Beat); or
    `regions` equal parts (Region); always Start first, at most 128. Key 36 (C1) plays
    the first, each key up the next; keys below or past the last play nothing. A slice
    plays from its start to the next's (Thru: to End), at the root's pitch plus
    Transpose and Detune. Mono and Thru cut the slice before; Poly limits notes as
    Classic does. Fades, Trigger and Gate as 1-Shot's; with Snap, slices snap too.
- **Transients** (`slicing::detectOnsets`, at load, both ways): the sample summed to
  mono; per hop of 128 samples at 48 kHz (`sampleRate / 375`), the energy over two hops,
  of the whole band and of the first difference (the highs), in dB floored 60 dB below
  the loudest; a hop's rise is how far either band climbs above the loudest of the
  three hops before. Rises of 3 dB or more that are the biggest within 30 ms either side
  are transients; each starts at the first sample around it reaching a quarter of its
  peak, moved back to the zero crossing before it (within 2 ms). Strength: its rise
  over the biggest rise.
- **Pitch and speed**: unwarped, a voice steps through the sample at `(file rate / engine
  rate) * 2^(semitones / 12)` (4-point Hermite interpolation; the loop's wrap and its
  crossfade read through `Reader::at`, so the interpolation is continuous across it),
  `semitones` being its pitch plus `round(tune) + fine / 100` plus the LFO's. Warped,
  the whole sample lasts `warp_beats` beats at the block's tempo: `rate = frames /
  (beats * 60 / tempo * sampleRate)`. Re-Pitch resamples at `rate * 2^(semitones / 12)`;
  the other modes stretch: each voice gets a Signalsmith stretcher of its own, configured
  with the clips' block sizes for that mode and split computation (each block's work
  spread over the interval after it; [warp.md](warp.md); Formants keeps the formants),
  fed `rate` frames a frame from where it plays (wrapping a Classic voice's loop; silent
  past its end), transposed by `semitones`. A note-on seeks it (`outputSeek`, its
  latency computed ahead), so its first frame is the slice's or Start's at once. Faster
  than 4 times (`kMaxStretchRate`: a long sample in few beats) notes are resampled, as
  a stretcher's work grows with the rate; a stretched note that gets there as it plays
  (Warp Length or the tempo changing) gives its stretcher up and goes on resampled.
- **Stretchers** are made on the main side (`updatePool()`: `idle()`, `prepare()`,
  `resetOffline()`), only while Warp is on with a stretching mode: one for a mono mode,
  else as many as Voices allows up to 8 (`kMaxStretched`, which then limits the notes
  too), plus 2 for notes fading out; each is about 0.9 MB. They are handed to the
  rendering thread as samples are (`pendingPool_`, `retiredPools_`); voices of the pool
  it replaces fade out (resampled). `resetOffline()` makes fresh ones (with the fixed
  seed), and `reset()` counts notes from 0 again (a Random LFO restarted by notes takes
  its values from them), so offline renders come out the same every time. Until they come (or with none free) a warped note
  is resampled as Re-Pitch would.
- **Filter**, on the voices' mix: TPT state-variable sections (Zavalishin/Simper);
  12 dB one, 24 dB two (low- and high-pass a Butterworth pair, Q 0.54 and 1.31; band-pass
  and notch two of Q 0.71). Resonance raises the resonant sections' Q towards 10
  geometrically (the last of a low- or high-pass; both of a band-pass or notch). The band-pass is normalized to unity at its centre. The cutoff glides to
  its target (5 ms, in log) a chunk of 32 samples at a time, limited to 0.45 of the
  sample rate; switched on, it starts at its cutoff from silence. After the last note
  the device goes on rendering until the filter has rung out.
- **LFO**, one for the device, evaluated at each chunk's ends: the gains glide between
  them, the pitch and cutoff take the middle. Sine, triangle, saw up and down, square,
  and random (a new value each cycle, from a hash of the cycle's number). Synced while
  the song plays and Retrigger is off, its cycles are the song's (`beatPos` over the
  rate in beats), so a note starting off the beat finds it where the song is; otherwise
  it runs at its rate (synced: from the tempo), and with Retrigger each new note starts
  it at the beginning of a cycle. Volume: `1 - amount * (1 - lfo) / 2`;
  Pitch: `amount * lfo` cents; Filter: the cutoff times `2^(4 * amount * lfo)`; Pan: added to the pan.
- **Output**: voices mix into the output (both sides into one channel if that's all
  there is, at half gain); then the filter; then Gain, the LFO's tremolo, the pan
  (balance, as Utility's) and Volume, all ramped over 20 ms. It writes, not adds.
  Velocity: gain = `1 - sensitivity * (1 - velocity / 127)`. `tailSamples()` is the
  longer of Release and Fade Out.
- **State**: `"sample"`: the file's path. `setStateValues()` stores the path, loads the
  file through `loadSource()` and finds its transients both ways, *without holding its
  mutex* (it may take a while), and publishes it, unless another state came meanwhile or
  it is the file already playing (notes go on). If the file can't be loaded it publishes
  nothing (silent until it can be loaded) and rethrows, but keeps the path in its state,
  so the project keeps it. Unknown state values are ignored.
- **Handing the sample to the audio thread without a lock**: the main side puts a new
  `Sample` (the source and its transients) into `pending_` (an atomic pointer; a sample
  still pending and never taken is freed there). At the start of each `render()` the
  rendering thread takes it, pushes the one it replaces into `retired_` (an `SpscQueue` of
  8), and silences its voices (they played the old one). If `retired_` is full it waits
  for the next block. The main side frees retired samples (and stretchers) in `idle()`
  and whenever it publishes.
- **Display**: `position`, one value per 256 samples: where the newest note plays in the
  sample (0..1 of its length, as it plays), or -1 while none plays. The editor draws the
  playhead from it, and lights the slice it is in.

The engine bridge restores a built-in device's state in the background (`EngineBridge::setBuiltinState()`, on a pool
of one thread, so states are set in the order they came and the last one wins), since it may load files, and waits
for all of them before rendering offline (`waitForDeviceStates()`; `devicesReady()` says whether any are pending).
See [app/engine-bridge.md](../app/engine-bridge.md).

### Utility (`builtin:utility`, AudioEffect)

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `gain` | Gain | dB | -60..24 | 0 |
| `pan` | Pan | | -1..1 | 0 |
| `width` | Width | % | 0..200 | 100 |

Mid/side width (`side * width`), then gain times the balance pan's left and right gains (sine taper, unity at
centre), all ramped over 20 ms. On one channel it applies only the left gain.

### Over The Top (`builtin:ott`, AudioEffect)

A multiband upward and downward compressor in the style of the one every drop uses, with a single big *Soundgoodize*
knob (depth) and an output trim. At 0 % it passes the audio through untouched (apart from the crossovers' all-pass
phase, see below).

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `depth` | Soundgoodize | % | 0..100 | 100 |
| `output` | Output | dB | -24..24 | 0 |

- Three bands split by Linkwitz-Riley crossovers at 88.3 Hz and 2.5 kHz, each half two Butterworth TPT state-variable
  filters in a row. The low band also goes through an all-pass equal to the upper crossover's LP + HP sum, so every
  band has the same phase shift. The dry signal is the bands summed unprocessed, so in-between settings don't
  comb-filter against the crossover's phase shift.
- Each band's envelope is followed on the louder channel (linked stereo) with its own attack and release. Above its
  downward threshold it is squashed at 66:1 (effectively a limiter); below its upward threshold it is pulled up at
  1:4.17, by at most 30 dB, fading out towards -90 dB so silence stays silent. Then each band's make-up plus 6 dB.

| Band | Down threshold | Up threshold | Make-up | Attack | Release |
|---|---|---|---|---|---|
| Low | -33.8 dB | -40.8 dB | 10.3 dB | 47.8 ms | 282 ms |
| Mid | -30.2 dB | -41.8 dB | 5.7 dB | 22.4 ms | 282 ms |
| High | -35.5 dB | -40.8 dB | 10.3 dB | 13.5 ms | 132 ms |

- Depth blends each band's gain from 1 to its computed gain; the output trim applies last.

### Compressor (`builtin:compressor`, AudioEffect, sidechain)

A feed-forward soft-knee compressor, keyed by its own input or by its sidechain (a ducker, with another track there).
Linked stereo: both channels get the gain the louder one calls for.

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `threshold` | Threshold | dB | -60..0 | -18 |
| `ratio` | Ratio | :1 | 1..20, log | 4 |
| `attack` | Attack | ms | 0.1..200, log | 10 |
| `release` | Release | ms | 5..2000, log | 150 |
| `knee` | Knee | dB | 0..24 | 6 |
| `makeup` | Makeup | dB | -12..24 | 0 |
| `mix` | Dry/Wet | % | 0..100 | 100 |

- The key's level follows its peaks at once and falls at the release time, so it holds across a low note's cycles.
- The gain computer works in dB on that level: unchanged below the knee, the ratio's slope above it, and a quadratic
  blend across it. The gain reduction it asks for is smoothed with the attack as it grows (and follows at once as it
  falls).
- With a sidechain connected the key is what the sidechain hears (silence while solo leaves its source out); without
  one, the device's own input.
- Gain = `1 + mix * (10^((makeup - reduction) / 20) - 1)`, so Dry/Wet blends towards the untouched signal.
- **Displays**, one value per 256 samples (`kMeterSamples`): `input` (the key's peak in dB, floor -90), `reduction`
  (the most gain reduction in dB, positive) and `output` (the output's peak in dB). Its editor shows every knob at
  once and, beside them, the gain reduction over the last second and meters of what keys it (the threshold marked)
  and of its output.

### Gate (`builtin:gate`, AudioEffect, sidechain)

After Ableton's Gate: it passes only what is louder than the threshold and turns everything else down to the floor
(silence at the bottom of its range); flipped, only what is quieter passes. It is keyed by its own input or by its
sidechain, optionally through an EQ, and can play the key instead of its output, to hear what opens it. Its maths
the editor shares is in [GateDesign.h](../../engine/src/builtin/GateDesign.h) (namespace `gate`).

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `threshold` | Threshold | dB | -70..6 | -12 |
| `return` | Return | dB | 0..24 | 3 |
| `attack` | Attack | ms | 0.02..150, log | 3.5 |
| `hold` | Hold | ms | 1..1500, log | 10 |
| `release` | Release | ms | 0.1..3000, log | 15 |
| `floor` | Floor | dB | -75..0 (-75: silence, shown as -inf) | -40 |
| `lookahead` | Lookahead | | 0 ms, 1 ms, 10 ms (not automatable: it is the latency) | 1 ms |
| `flip` | Flip | | Off, On | Off |
| `sc_gain` | S/C Gain | dB | -70..24 | 0 |
| `sc_mix` | S/C Mix | % | 0..100 | 100 |
| `sc_listen` | S/C Listen | | Off, On | Off |
| `sc_eq` | S/C EQ On | | Off, On | Off |
| `sc_eq_type` | S/C EQ Type | | Low Shelf, Bell, High Shelf, Low-pass, Band-pass, High-pass | High-pass |
| `sc_eq_freq` | S/C EQ Freq | Hz | 30..15000, log | 80 |
| `sc_eq_q` | S/C EQ Q | | 0.1..12, log | 0.71 |
| `sc_eq_gain` | S/C EQ Gain | dB | -15..15 | 0 |

- **The key** (what opens it): without a sidechain, its own input. With one, the sidechain times S/C Gain blended
  with the input by S/C Mix (100 %: the sidechain alone; 0 %: the input alone); silence while solo leaves its source
  out. The blend glides over 20 ms when a sidechain is chosen and back to the input when it goes (once removed, the
  sidechain's sound is gone at once, as when its source stops). S/C Gain and Mix ramp over 20 ms. A sidechain sample
  that isn't audio (NaN, infinity, beyond ±1e30: what a broken source can leave in the engine's buffer, which
  `BuiltinProcessor` cleans only for the device's own input) is taken as silence where the key reads it, so it never
  reaches the EQ's state or the output.
- **The key EQ** (S/C EQ On): one RBJ biquad per channel (`gate::keyFilter`, `dsp::BiquadCoefficients`), the six
  types in Live's order; the shelves have the cookbook's plain slope (Q 0.7071: Q is the bell's width and the pass
  filters' resonance); the frequency is held below 0.45 of the rate. Freq and Q glide in log, Gain in dB (a 15 ms
  one-pole, the filter designed again every 32 frames while they move, counted per frame so automation splitting
  the block doesn't change its pace). Switched on, it fades in over 10 ms (and out likewise); a change of type
  crossfades over 10 ms from the old filter (its state kept) to the new. A filter that starts (switched on, or a
  new type) starts warm: the device keeps the key before the EQ for its last 100 ms (`gate::kWarmSeconds`), and the
  new filter is run from rest over it, for as long as its slowest pole takes to fall 60 dB (`gate::warmFrames`, at
  most those 100 ms), so it goes on as if it had been running all along and the fade is only ever between two
  settled outputs. Carried on from the old filter's state instead, a low shelf at 30 Hz overshot the key by about
  5 dB for tens of milliseconds, enough to open the gate. It runs over that history 16 frames a frame
  (`gate::kWarmPace`) until it has caught up with the key, so no one block pays for it all (100 ms is 19 200 frames
  a channel at 192 kHz: run at once, a 32-frame block cost 200 µs where it lasts 167 µs), and is heard only from
  then: the crossfade (or the fade in) starts that much later (it gains 15 frames on the key a frame: about 1.3 ms
  for the default high-pass, 6.7 ms at most), with the old filter (or the key unfiltered) heard until then. Nothing
  of it but that short history is kept while it is off.
- **Detection**: peak, linked stereo (the louder key channel's level opens the gate for both), with no smoothing
  besides the lookahead window; chatter on low notes is what Return and Hold are for, as in Live (the default 10 ms
  hold holds across a 50 Hz note's zero crossings).
- **Opening and closing**: it opens on the first sample whose level is at or above the threshold, stays open while
  the level is at or above the threshold less Return (hysteresis), then for Hold more, then closes. How far open
  it is moves linearly over Attack while open and over Release while closed, from wherever it is (opening again
  during a release ramps up from there). The hold counts up from the fall, so a Hold changed while it counts
  applies at once (shortened below what has passed, the gate closes there). Threshold, Return, Attack, Hold and
  Release need no smoothing: they move a decision or a ramp's speed, never the gain itself. Ableton's own ramp
  curves are unpublished; these are designed.
- **The gain**: the openness eased in and out (smoothstep, so the ramps start and end without a corner), flipped,
  then between the floor and unity, `1 - (1 - floor)(1 - pass)`: exactly 1 fully open, so an open gate without
  lookahead passes its input bit for bit. The ramps are linear in amplitude: at the default -40 dB floor a release
  spends most of its time in the top 20 dB, heard as a fade of about its length.
- **Flip** inverts what passes, not the detector (the state, Return and Hold work as unflipped): flipped, the attack
  is the sound being turned down as the key rises above the threshold, the release the sound coming back. Switching
  it fades over 10 ms. **Floor** is ramped over 20 ms; at its bottom (-75 dB) the gain is 0.
- **Lookahead** (0, 1 or 10 ms, in whole samples: 0, 48, 480 at 48 kHz) delays the audio, and the level is the key's
  largest over the lookahead window plus the current sample: the gate opens that much before a transient reaches
  the output, and the hold counts from when the fall reaches it. Each choice with a window has its own
  `dsp::SlidingMax`, exactly as long as it (at 0 ms the level is the key's peak as it is); a change refills the new
  one from a short delay line of the key's peaks, so its window holds what the key did before it was chosen, and
  crossfades (an S-curve, 10 ms) from the old delay tap to the new. `latencySamples()` is the lookahead (48 at the
  default, as Live's); `idle()` returns true once when it changes, so the engine realigns the tracks.
  `tailSamples()` is the lookahead too (what is still in the delay).
- **Listen** puts out the key (after S/C Gain, Mix and the EQ, delayed by the lookahead as the audio is) instead
  of the gated audio, crossfading over 10 ms; on one channel, the mean of the key's two channels. At either end of
  the crossfade the output is the one alone, so nothing of the key is in it while nobody listens.
- A change of lookahead or EQ type that comes during its crossfade (or while the new type's filter catches up)
  waits until it is done.
- **Channels**: two as described; on one, the key's right channel is the input itself (a sidechain still brings
  two). A channel that comes back after one-channel processing starts its delays from silence.
- **reset()** clears the delays, the detector and the EQ, and closes the gate; the next `render()` starts every
  ramp, the lookahead and the EQ at the parameters as they are then (whether a sidechain is connected is only known
  there), so an offline render's first transient sees the key as set. `prepare()` (a new rate) sizes the delays for
  10 ms and works out the ramps again.
- **Denormals**: the only recursive state is the key EQ's, and `dsp::Biquad` zeroes a section's two states together
  once both are below 1e-20 (low down or narrow, at 30 Hz and Q 0.1 or 12, the two nearly cancel: zeroing one alone
  would kick the other back up, and it would go on at about 1e-19 for minutes). So silence rings out to exact zeros
  as soon as the filter's slowest pole takes it there: 0.11 s for the default high-pass after loud noise, 5.5 s for a
  bell 15 dB down at 30 Hz and Q 0.1 (a real pole near 1 Hz), 12 s at most (a bell 15 dB up at 30 Hz and Q 12, the
  key 24 dB up), all of it below -300 dB after the first moments.
- **Displays**, one value per 256 samples (`gate::kDisplaySamples`), pushed together so they stay in step: `input`
  (the input's peak in dB as it reaches the gain, after the lookahead), `output` (the output's peak in dB, what is
  heard while listening too), `key` (the key's peak in dB, before the lookahead: what the threshold is compared
  with, so it leads `input` by the lookahead), all floored at -90 dB; and `open` (the mean of how much passed, 0 at
  the floor .. 1 fully through, after Flip). The editor draws the levels' history with the threshold and return
  lines, how far the gate turns the sound down, and the key EQ's curve (the response of `gate::keyFilter`, the
  engine's own biquad, through the application layer's [GateResponse.h](../../app/src/audio/GateResponse.h)).
- It costs about 0.13 % of one core at 48 kHz stereo at its defaults
  ([builtin_devices_bench](../../benchmarks/builtin_devices_bench.cpp)), 0.07 % without lookahead, and about 0.18 %
  with the key EQ (a Q 12 bell), 10 ms of lookahead, listening and flipped. A key EQ filter catching up costs 16
  biquad steps a channel a frame while it does (for at most 6.7 ms): the 32-frame block where it starts, about 7 µs
  against 1.2 µs, at any rate (run at once, it cost 30-55 µs at 48 kHz and 100-400 µs at 192 and 384 kHz).

### Limiter (`builtin:limiter`, AudioEffect)

A brick-wall lookahead limiter after Ableton Live 12.1's: no sample comes out above the ceiling, and in True Peak mode
no peak between samples does either. Gain pushes the input into it; Soft Clip rounds peaks off as they near the ceiling
instead of turning them all down; Routing limits left and right, or mid and side, with Link sharing the gain reduction
between the two; Maximize turns the ceiling into a threshold. Its latency is the lookahead.

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `gain` | Gain | dB | -24..24 | 0 |
| `ceiling` | Ceiling | dB | -24..0 | -0.3 |
| `release` | Release | ms | 0.1..3000, log | 300 |
| `auto_release` | Auto Release | | Off, On | On |
| `lookahead` | Lookahead | | 1.5 ms, 3 ms, 6 ms (not automatable: it is the latency) | 3 ms |
| `mode` | Mode | | Standard, Soft Clip, True Peak | Standard |
| `routing` | Routing | | L/R, M/S | L/R |
| `link` | Link | % | 0..100 | 100 |
| `maximize` | Maximize | | Off, On | Off |
| `threshold` | Threshold | dB | -24..0 | -0.3 |
| `output` | Output | dB | -24..0 | -0.3 |

- **A normalized domain.** Everything runs where the line (the ceiling, or with Maximize the threshold) is 1
  (`limiter::scales()`, [LimiterDesign.h](../../engine/src/builtin/LimiterDesign.h)): `pre` multiplies the input before
  the lookahead and the detector, `post` the output. Without Maximize `pre` = gain / ceiling and `post` = ceiling; with
  it `pre` = 1 / threshold and `post` = output, so the gain is Output - Threshold, the Output is the ceiling, and Gain
  doesn't apply. At the defaults (Threshold and Output at the Ceiling's -0.3 dB, Gain 0 dB) switching Maximize on
  changes nothing. `pre` and `post` glide over 20 ms along a cubic in dB (`GainGlide`: from rest a smoothstep; a new
  target mid-glide carries the slope on, within Fritsch and Carlson's bound, so it never overshoots), so a level change
  has no slope kink where it starts or stops for limiting to turn into a click; and the two glide along the same curve,
  so their product stays the gain and moving the ceiling over material that doesn't reach it changes nothing (linear
  ramps would bulge by about 1 dB halfway). A cubic's second difference is constant: three products a sample, a log and
  three `exp` per new target. `post` and Soft Clip's amount are written into rings as long as the lookahead when their
  sample comes in and read back when it comes out, so each sample is detected and scaled with the same values: a change
  is a smooth level change and never breaks the brick wall. Input that isn't finite (NaN, ±inf, beyond 1e30) never
  reaches it: `BuiltinProcessor::process()` takes it as 0, as for every built-in device, so one bad sample can't
  silence the rest.
- **Detection**, 12 samples behind the input (`kDetectorDelay`), of left, right, mid and side. Standard and Soft Clip
  take each sample's magnitude. True Peak interpolates seven points between each sample and the next (24 taps each, a
  Kaiser-windowed sinc with β = 6, `dsp::besselI0` for its window, each phase passing DC exactly: flat within ±0.01 dB
  to 0.42 of the rate), and refines the largest of the eight by a parabola through it and its neighbours
  (`limiter::refinedPeak`, never below the point, within 0.002 dB of a sine's crest at 0.4 of the rate), aiming 0.02 dB
  under the ceiling (`kTruePeakMargin`). Its output, by a 32x reference meter, stays at or under the ceiling for content
  up to about 0.42 of the rate (20 kHz at 48 kHz). The seven dot products run as one vector of eight points per tap (AVX
  intrinsics, as compilers vectorize the plain loop across the taps with a shuffle per value; the plain loop elsewhere),
  and the parabola is skipped where 1.25 times the largest of the ten points around the sample is under the target (the
  most it can add).
- **Required gains.** L/R: each channel's own (target / peak, for peaks over it) blended by Link with the pair's (the
  lower). M/S: |left| and |right| are at most |mid| + |side|, so their sum must fit: own gains share the room out
  (`limiter::sharedCeiling`: the quieter keeps its level while the louder takes what is left; both meet at half when
  both are loud), the linked gain turns both down alike, by what left and right need: |mid| + |side| is the louder of
  |left| and |right| at every moment, between samples too, so at Link 100 % M/S is exactly L/R linked, in True Peak as
  well (the sum of the mid's and the side's own true peaks, found at different moments, would limit 0.4 to 1.2 dB more
  than needed). Any blend of feasible pairs is feasible, so every Link and Routing obeys the ceiling. What M/S costs: on
  wide, uncorrelated material each M/S pipeline holds its own worst sample, and their peaks rarely coincide, so the
  peaks that come out fall short of the ceiling: measured on uncorrelated noise driven 12 to 20 dB into it, 4.3 to 5 dB
  short at Link 0 % and 1.7 to 2 dB at 50 % for uniform noise (the densest peaks), about 1.4 dB at 0 % for Gaussian
  noise, none at 100 % or in L/R. On a loud centre with quieter sides the side keeps its level, which is what M/S is
  for.
- **Four pipelines** (L, R, M, S), each on the required depth (1 - gain): a hold over the attack's length S = L + 1 - 12
  samples (`dsp::SlidingMax`, one per lookahead, each exactly its window), a release (Release's one-pole; Auto: a
  one-pole whose time constant follows how dense the limiting has lately been, a 200 ms one-pole of whether it was
  limiting: 50 ms after a lone peak, slowing with that density's square to 600 ms under sustained limiting
  (`limiter::autoReleaseMs`; the coefficient from a table of 33, read in between linearly, no `exp` per sample). So the
  gain doesn't follow each cycle of a bass note's limited crests, which distorts it: a 30 Hz sine 6 dB over comes out
  with 0.40 % THD (a manual 300 ms: 0.56 %, 50 ms: 2.6 %), 50 Hz with 0.11 %; and once the limiting stops it speeds up
  again (a lone 5 ms peak 6 dB over is back within 0.25 dB 150 ms after; after 2 s of limiting by 6 dB, 3 dB are left
  150 ms after and none 600 ms after). Auto's switch is a 20 ms blend, and the release is at least the hold whatever the
  blend), then two box filters whose lengths add up to S. The gain each sample gets is an average of values that are
  each at least what that sample needs, so it is never above it, and reaches it exactly when a lone peak comes out,
  after an S-curve S - 1 samples long (nothing earlier). The boxes sum in fixed point (depths rounded up to multiples of
  2^-30, summed in 64 bits, the first's average divided out by integer ceiling division): exact, so they never drift,
  and silence brings the gain back to exactly 1. Both routings' pipelines always run.
- **The output** is the delayed input times the routing's gains (Routing crossfades between the two routings' outputs,
  each obeying the ceiling), through Soft Clip's knee, clamped to the line, times `post`.
- **Soft Clip**: a knee (`limiter::knee`, `shape`) linear up to 6.02 dB under the ceiling and a quadratic from there to
  the ceiling at 3.52 dB over it (slope 1 to 0: continuous in value and slope, never over); the limiter aims at that top
  instead of the ceiling, so peaks up to 3.52 dB over are rounded off (louder, with some crunch: a sine at the ceiling
  comes out at -1.16 dB with its 3rd harmonic at -27 dB) and only what goes further is turned down. Not oversampled, so
  every mode has the same latency and attack; what that costs: content above a sixth of the rate (8 kHz at 48 kHz) that
  itself reaches the knee folds its 3rd harmonic back as an inharmonic tone, 27 dB under it at the ceiling and 19 dB
  pushed 3 dB in (11 kHz → 15 kHz, 9 kHz → 21 kHz); below that only the 5th folds, 44 dB under or less. On a mix, whose
  bass carries the peaks, bright content riding through the knee up to about 13 kHz folds back 54 dB or more under
  itself; above that its sums fold into the top octave (about 20 kHz), 24 to 36 dB under it. (A 2x oversampled knee
  would take 5 to 31 samples from the attack or add them to the latency, and still leave about -29 dB pushed 3 dB in:
  the brick wall's clamp at the base rate clips what the decimation filter overshoots.)
- **Smoothing.** Gain, Ceiling, Threshold, Output and Maximize glide `pre` and `post` (20 ms in dB); Release sets its
  coefficient per stretch; Link glides linearly (20 ms, before the pipelines, which smooth it further); Mode's Soft Clip
  amount, Routing and Auto glide over 20 ms eased in and out (`dsp::sCurve`, a smoothstep: a crossfade has no kink
  where it starts or stops); switching to or from True Peak only changes what is measured, which the pipelines
  smooth.
- **Lookahead changes** fade the output out (2 ms, a smootherstep), start again from the new length (the delay line,
  rings, holds, boxes and releases cleared: fills of a few thousand values at most), wait for the line to refill and
  fade back in: a dip of about L + 4 ms, never a click. `latencySamples()` and `tailSamples()` are the lookahead (72,
  144 or 288 samples at 48 kHz); `idle()` reports a change once, so the engine realigns the tracks. `reset()` starts
  from the new length at once.
- **One channel**: only the left pipeline; Routing and Link don't apply. Channels past two are left untouched.
- **Denormals**: the release stages and Auto's density are flushed below 1e-9 and the boxes are integers, so silence
  comes out as exact zeros once the delay line has emptied, and the gain reduction returns to exactly 0 (with Auto about
  1.1 s after the input stops; manual, about 21 Release times).
- **Displays**, seven, one value per 128 samples (`kMeterSamples`), published together, all measured where the sample
  comes out (so a peak's input, reduction and output line up): `input_l`, `input_r` (the input's peak in the line's
  domain: dBFS after Gain, or the raw input with Maximize; floor -90), `output_l`, `output_r` (the output's peak in
  dBFS), `reduction_a`, `reduction_b` (the most gain reduction of the routing's two channels, L and R or M and S, in
  dB, positive, as the Compressor's `reduction`; exactly 0 when the gain stayed 1) and `clip` (what Soft Clip's knee
  took off the loudest peak, dB; exactly 0 in Standard and True Peak). One channel: the right's are the left's.
- **Shared with the editor** ([LimiterDesign.h](../../engine/src/builtin/LimiterDesign.h), through the application
  layer's [LimiterResponse.h](../../app/src/audio/LimiterResponse.h)): `limiter::scales` (the line, and Maximize's shift
  of the output), `softKneeDb` and `softTopDb` (Soft Clip's band), `kMeterSamples` (the history's time axis) and
  `kFloorDb` (the displays' silence), so the line and Soft Clip's band drawn are where the device limits and rounds
  off. `lookaheadSamples`, `autoReleaseMs`, `sharedCeiling`, the true-peak phases, `refinedPeak` and `reductionDb` are
  the device's own (and its tests'); `interpolate` (the phases applied one point at a time) is the tests'.
- On the machine it was written on (a 2.1 GHz Xeon, AVX2, `builtin_devices_bench`, 48 kHz stereo) it took 0.27 % of one
  core at its defaults (Standard), 0.32 % in Soft Clip with 12 dB of gain, 0.49 % in True Peak (0.65 % limiting 12 dB),
  and 0.91 % at its heaviest (True Peak, M/S at Link 50 %, 6 ms, 24 dB of gain).

### Multiband Dynamics (`builtin:multiband`, AudioEffect, sidechain)

After Live's Multiband Dynamics: up to three bands, each with two thresholds, *Above* and *Below*, and a ratio for
each, so one band can be compressed and expanded, downwards and upwards, at once
([MultibandDesign.h](../../engine/src/builtin/MultibandDesign.h)). Over The Top is one setting of it. A sidechain
keys each band by the same band of the key. Its fresh state (every ratio 1:1) does nothing: the bands summed, an
all-pass of the input. Ranges are Live's (its set files' parameters).

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `xover_low` | Low-Mid Crossover | Hz | 30..3000, log | 120 |
| `xover_high` | Mid-High Crossover | Hz | 300..15000, log | 2500 |
| `low_on` | Low Band On | | Off, On (not automatable, as Live's split switches) | On |
| `high_on` | High Band On | | Off, On (not automatable) | On |
| `<band>_active` | `<Band>` Activator | | Off, On | On |
| `<band>_in` | `<Band>` Input | dB | -24..24 | 0 |
| `<band>_out` | `<Band>` Output | dB | -24..24 | 0 |
| `<band>_above` | `<Band>` Above Threshold | dB | -80..0 | -20 |
| `<band>_above_ratio` | `<Band>` Above Ratio | ratio | 0.25..100, log | 1 |
| `<band>_below` | `<Band>` Below Threshold | dB | -80..0 | -40 |
| `<band>_below_ratio` | `<Band>` Below Ratio | ratio | 0.25..100, log | 1 |
| `<band>_attack` | `<Band>` Attack | ms | 0.1..5000, log | 10 |
| `<band>_release` | `<Band>` Release | ms | 0.1..5000, log | 100 |
| `<band>_solo` | `<Band>` Solo | | Off, On (not automatable, as Live's) | Off |
| `amount` | Amount | % | 0..100 | 100 |
| `time` | Time | % | 10..1000, log | 100 |
| `output` | Output | dB | -24..24 | 0 |
| `soft_knee` | Soft Knee | | Off, On | Off |
| `mode` | Peak/RMS | | Peak, RMS | RMS |
| `sc_gain` | S/C Gain | dB | -70..24 | 0 |
| `sc_mix` | S/C Mix | % | 0..100 | 100 |
| `sc_listen` | S/C Listen | | Off, On (not automatable, as Live's) | Off |

The ten `<band>_...` rows come three times, for `low`, `mid` and `high` (named "Low Activator", "Mid Input", "High
Input"...), in that order after `high_on`: band b's field f is parameter 4 + 10b + f. A ratio R is stored as the
distance past the threshold the level change divides (at 4, a level 4 dB past it comes out 1 dB past it), and the
`ratio` unit prints it as Live does, "1:R" either side of 1:1, to three significant digits (`formatValue`:
"1:4.00", "1:66.7", "1:100"; under 1 "1:0.500", "1:0.250"). So Live's numbers mean the same here: its "1 : 66.7"
(Over The Top's Above) is 66.7.

- **The split**: two Linkwitz-Riley crossovers (`dsp::Crossover`, as Over The Top's) at Low-Mid and Mid-High, the
  low band through the upper crossover's all-pass (`dsp::CrossoverAllpass`), so the three add up to an all-pass:
  flat (within 0.01 dB) whatever their gains are when they are equal. The dry signal is never mixed in, so nothing
  comb-filters against the crossovers' phase. A Low-Mid at or above the Mid-High splits both there: the mid band
  narrows to a band around it (peaking at −12 dB), and the sum stays flat.
- **Low and High, the split switches, switch the outer bands off as Live's do**: the split doesn't change (nor the
  phase), but a band switched off takes the mid band's whole chain (Input, dynamics, Output) and its signal feeds the
  mid band's detector, so with both off the mid band shapes the whole spectrum as one band. The switches crossfade
  the gains; every band's detector, gain computer and smoothing keep running, switched off or not, so a band
  switched back on picks up where its own level is. Not automatable, as Live's. Solo: only the soloed bands are
  heard; a band switched off can't be soloed and follows the mid band's solo.
- **Each band's activator** (Live's band buttons, the Mid band's too) bypasses it: its Input, dynamics and Output
  glide to unity (its whole gain in dB scaled by the activator's glide), while its split stays, so its frequencies
  are its own, not another band's to shape; the device's Output still applies. A band switched off takes the mid
  band's gains, so with the Mid band bypassed nothing shapes it either. Its detector runs on.
- **Detectors**, linked stereo (the louder channel; one channel: that one), per band, in dB after the band's Input
  (Input drives the detector as it drives the audio, so the thresholds stay where the level is drawn):
  - Peak: the largest |x| over a window of the band's lowest period (`multiband::windowSeconds`: 25 ms for the low
    band, 1 / Low-Mid for the mid band (25 ms while the low band is merged into it), 1 / Mid-High for the high band,
    each within 1..25 ms; a `dsp::SlidingMax`). A rectified sine peaks every half period, so a steady tone holds its
    peak and isn't modulated (distorted); the level falls once a peak leaves the window, then the release takes over.
  - RMS: each channel's mean square through a one-pole of that window or 10 ms, whichever is longer, the larger of
    the two taken: the louder channel's power (the mean of the larger square each sample would read up to 2.1 dB
    hot on stereo whose sides differ). A sine reads 3.01 dB under its peak.
  - Both run all the time; Peak/RMS crossfades their levels (a glide), so switching never clicks.
- **The gain law** (`multiband::aboveGainDb`, `belowGainDb`, `bandGainDb`): past either threshold the distance from
  it is divided by the ratio, `out = T + (L − T) / R`; so over 1:1 a side narrows its region's range (Above:
  downward compression; Below: upward compression), under 1:1 it widens it (Above: upward expansion; Below: downward
  expansion). Soft Knee bends each side in over 6 dB around its threshold, quadratically (the Compressor's shape).
  Upward compression fades out between −72 and −96 dB, so silence, and whatever lies under −96 dB, isn't lifted; a
  noise floor above that is, as Over The Top's is (−80 dB under a Below of −40 at 1:4 comes up 20 dB). Each side's
  change is held to −96..+30 dB, and so is the band's (Amount times the sum of its two sides), so a gate-like
  expansion lets go from at most 96 dB down. Both sides apply as their formulas say even with Above set under Below.
- **Attack and release** as Live defines them, for each side on its own: a side's gain change (dB) moves with the
  attack while it grows and with the release while it shrinks (one-poles). So Above's attack is how fast it acts as
  the level rises past the threshold, Below's as the level drops under it, and each release how fast it lets go.
  Time scales every band's attack and release (10..1000 %).
- **Every control glides**: two one-poles of 5 ms in a row, so a jump eases in and out (a linear ramp's kinks show
  on a band's share of a tone), within 1 % after about 33 ms: thresholds, ratios (as their slopes), gains, Amount,
  Output, S/C Gain and Mix, Listen, Soft Knee (the knee's width), Peak/RMS, the band switches and activators, solos
  (each band's audibility) and the sidechain's connection (`dsp::Glide`, in double, a thin `Control` holding each
  one's target and its value as a float for the loop). Crossovers glide in log, a step per 32 samples, each crossover's
  `g = tan(πf/sr)` interpolated sample by sample between the steps and its coefficients remade while it moves; the
  windows follow per step. Attack, release and Time make new coefficients per stretch (a time constant changing
  can't click). Only glides still moving are stepped (in groups: the device's controls, and each band's), so a
  device left alone does no smoothing work. The 32-sample grid runs across `render()` calls, so the output and the
  displays don't depend on where automation cuts a block (bit-identical in blocks of 1, 13, 256 and 1024).
- **The sidechain** (`hasSidechain()`; Live 9's): the key, times S/C Gain, goes through a split of its own, so each
  band is keyed by the same band of the key (a kick keys the low band, a vocal the mid). S/C Mix blends what each
  detector hears from the device's own band (0 %) to the key's (100 %); at 0 % the output is exactly the unkeyed
  one. One main channel: the key is the mean of its two. Connected but silent (solo leaving its source out): the key
  is silence. A key sample that isn't audio (NaN, infinity, beyond `BuiltinProcessor::kMaxInput`) is taken as
  silence where it is read: the input is cleaned by `process()`, the key isn't (it is the engine's buffer). The
  key's split runs only while some of the key is heard (the sidechain connected, or fading in or out, and S/C Mix
  over 0 %: at 0 % it costs nothing), and is cleared once none is, so a key heard again starts from silence. Its
  connection glides, but after a `reset()` the first `render()` takes it as it is (the renderer sets the flag after
  resetting, and a reset has cleared every state, so nothing can click): an offline render after a sidechain was
  removed starts unkeyed.
- **Listen** (S/C Listen, as Live's headphones): the output crossfades (a glide) to what the detectors hear, the
  three bands of the trigger summed (an all-pass of it): the key after S/C Gain, blended with the device's own input
  by S/C Mix; without a sidechain, the input.
- **Denormals and the loudest input**: on the 32-sample grid the crossovers' states (and the key's), the mean
  squares and the gain changes are flushed to 0 below 1e-20 (`Crossover::flush()`, `dsp::flushTiny`), so silence
  rings out to exact zeros. The detectors' squares are held to 1e10 (+100 dB, far over any level the bands carry):
  the loudest input `process()` lets in, 1e30, squared would be infinite, and an infinite mean square would never
  come down; held there, the RMS detector is back from it within about a second.
- `reset()` clears every state and snaps every glide to the parameters; `prepare()` (a new rate) sizes the sliding
  maxima for 25 ms, works out the glides again and resets. `latencySamples()` is 0: no lookahead, as Live's (its
  latency article lists lookahead only for Compressor, Gate and Limiter). `tailSamples()`: the crossovers' ringing,
  14 time constants of the slowest Butterworth pole, `ceil(14 √2 / (2π f) · sr)` with f the lower crossover (105 ms
  at 30 Hz, 26 ms at 120 Hz); the dynamics add none.
- **Displays**, one value per `multiband::kDisplaySamples` (256) each, all nine published together:

  | id | Each value |
  |---|---|
  | `low_in`, `mid_in`, `high_in` | the band's detector level (dB, after Input; Peak or RMS as set; the trigger's when keyed), the largest over the 256 samples, −90 floor |
  | `low_out`, `mid_out`, `high_out` | that level plus the gain change the dynamics are applying (before the Outputs): the static curve's output for it once attack and release have settled, the largest, −90 floor |
  | `low_gain`, `mid_gain`, `high_gain` | the dynamics' gain change (dB, signed: negative a cut, positive a boost; Amount and the limits applied), the one of largest magnitude |

  A band switched off (done fading) publishes −90, −90, 0 (the mid band shows what it does). A band bypassed shows
  its level as it comes (no Input) and no change. With every ratio 1:1, in and out are the same.
- **The shared design**: [MultibandDesign.h](../../engine/src/builtin/MultibandDesign.h) (`sub::multiband`, inline, no
  Qt) holds the ranges, the display rate (`kDisplaySamples`), `slope`, `upwardFade`, `aboveGainDb`, `belowGainDb`,
  `bandGainDb`, `staticGainDb` (the curve a steady level settles on) and `windowSeconds`; the device runs these very
  functions per sample. The application layer's [MultibandResponse.h](../../app/src/audio/MultibandResponse.h)
  (`multibandGainDb`; the ranges, `multibandMinThresholdDb()`... `multibandMaxRatio()`; and `multibandParseRatio`,
  `multibandParseMs` for typed ratios and times) hands them to the editor, so its readouts and handles are the sound.
  (The displays' rate reaches the editor with them: `DeviceCanvas::readRecent` takes it from the device's `displays()`.)
- About 0.66-0.69 % of one core at 48 kHz stereo at the defaults, and 0.70-0.78 % with every band's ratios working, Soft
  Knee and Peak (`builtin_devices_bench`, best of three, on a 2.1 GHz Xeon; Over The Top 0.40 % there). A sidechain
  keying it brings it to about 1 % (none at S/C Mix 0 %, where the key's split doesn't run), and everything at once (a
  sidechain, both crossovers swept all the time, Peak/RMS switched every 50 ms so it is nearly always crossfading) to
  about 1.05 %.

### Spectral Compressor (`builtin:spectral`, AudioEffect, sidechain)

A compressor per frequency, after the best-known spectral dynamics processors (nih-plug's Spectral Compressor,
soothe2, M-Compressor), in Live's terms: it splits the sound into about a thousand bins (a short-time Fourier
transform), measures each bin's level, and compares it with a threshold curve that follows a pink spectrum
([SpectralDesign.h](../../engine/src/builtin/SpectralDesign.h)). Each bin is turned down above the curve (Ratio)
and, if Upward is past 1:1, brought up below a second curve (Below), so resonances and harsh regions are tamed where
they occur and the rest is left alone; with Upward too the spectrum is pulled towards the curve. Keyed by a
sidechain, the key's spectrum sets the gains: one track's frequencies duck where another's clash with them.

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `threshold` | Threshold | dB | -72..12 | -18 |
| `ratio` | Ratio | :1 | 1..20, log | 2 |
| `below` | Below | dB | -72..12 | -48 |
| `upward` | Upward | :1 | 1..10, log | 1 |
| `tilt` | Tilt | dB/oct | -6..6 | 0 |
| `knee` | Knee | dB | 0..24 | 6 |
| `range` | Range | dB | 0..48 | 24 |
| `smooth` | Smoothing | % | 0..100 | 40 |
| `focus_lo` | Focus Low | Hz | 20..20000, log | 20 |
| `focus_hi` | Focus High | Hz | 20..20000, log | 20000 |
| `attack` | Attack | ms | 1..1000, log | 20 |
| `release` | Release | ms | 10..5000, log | 150 |
| `link` | Stereo Link | % | 0..100 | 100 |
| `mix` | Dry/Wet | % | 0..100 | 100 |
| `output` | Output | dB | -24..24 | 0 |
| `delta` | Delta | | Off, On | Off |

- **The STFT**: a periodic Hann window of N samples, `spectral::frameSize()`: 2048 at 44.1 and 48 kHz, doubling per
  octave of rate (4096 at 88.2/96, 8192 at 176.4/192, 1024 at 22.05..32 kHz, down to 256 at 8 kHz), so a bin is
  about 23 Hz wide at any rate. The input goes into a ring of 2N samples; every N/4 samples (a hop, H: 10.7 ms at
  48 kHz) the last N are a frame, windowed and transformed (signalsmith-linear's `RealFFT<float>`), gained,
  transformed back and added through the window again (scaled by 1 / 1.5N: Hann² at 4x overlap sums to 1.5) into
  an accumulator that is read, and cleared, as the output.
- **The work is spread over the hop**: a frame's work (its transforms, the per-bin loops, the displays) is cut into
  steps, a transform in ten (signalsmith-linear's split computation, `RealFFT<float, true>`) and a loop over the
  bins in parts of about 256, and the steps run evenly through the hop after the frame went in, so an audio
  callback carries its share of a frame, not the whole of it (at 192 kHz a frame takes about 250 µs, longer than a
  32-sample callback lasts; spread, the largest step is about 4.5 µs). The cost is that hop: a frame's output is
  added a hop after it went in, so input sample t comes out at t + N + H exactly: `latencySamples()` is N + H
  (`spectral::latencySamples()`: 2560, 53.3 ms at 48 kHz and 58.0 ms at 44.1 kHz) and `tailSamples()` N; with every
  gain at 0 dB it is transparent to float rounding (about 1e-6). The latency changes only with the rate, in
  `prepare()`.
- **Levels** are pink-referenced: each bin's power |X|², plus `calibrationDb()` (-34.26 dB at 48 kHz and 2048) and
  `pinkDb()` (3.01 dB per octave about 1 kHz, held below 20 Hz), so pink noise of RMS L dBFS reads L dB at every
  frequency and a threshold means about what a compressor's does on a full mix. A pure tone reads about 20 dB above
  its peak in dBFS (a 0 dBFS sine at 1 kHz reads +19.9 dB): its power is all in one bin.
- **Smoothing** measures each bin over a band constant in octaves, centred on it in log frequency: the mean power
  over bins `k / 2^(w/2) .. k · 2^(w/2)` (prefix sums in double), w from 1/48 octave just above 0 % growing
  exponentially to 2 octaves at 100 % (`smoothingOctaves()`; 40 %: about 1/8 octave); at 0 % each bin alone. A mean,
  not a sum: noise reads the same at any Smoothing, while a tone reads lower the wider the band, which is what makes
  wide Smoothing gentle with tones. (At 100 % the 2-octave box reads pink noise 0.34 dB low.)
- **Envelopes**: per bin and hop, on the magnitude (the square root of Smoothing's mean power), Attack and Release
  as one-poles at the hop rate (`env = x + a (env - x)`, the attack's coefficient while rising), floored at a
  magnitude of 1e-6 (and NaN lands there too: a bin's power overflowed by an absurd sample, one still under
  `BuiltinProcessor::kMaxInput`, can't stick). Magnitude, as the Compressor's follower
  and nih-plug's, so a time means the same as there: a rise from silence reaches 63 % of the new magnitude in an
  attack time, and a fall far below decays at 8.69 dB per release time constant (a follower of power would take
  half as much). Times under a hop act at once, and the frame itself smears an onset over about N/2 (21 ms at
  48 kHz). They read noise about 1 dB under its mean power at Attack = Release where Smoothing measures a bin alone
  (the mean of a magnitude; less the more bins), and 1.5-2 dB over it with an attack faster than the release (the
  defaults) where few bins are measured (low frequencies, low Smoothing), as any peak-leaning detector does. Every
  channel's envelopes run whatever the link, so turning it finds them current.
- **Stereo Link** blends each channel's level, after the envelopes, towards the louder channel's in dB (0: each its
  own; 100: both the louder's, so both get the same gains; 50: halfway in dB, so the knob is even to the ear). From
  99.99 % one set of gains is worked out, from the louder envelope, and copied (as cheap as mono). Keyed by a
  sidechain, each main channel is compared with the key's channel of its side (one main channel: the louder key's).
- **Gains**: the shared gain computer `spectral::gainDb()` (in float): the Compressor's quadratic knee, both ways;
  down by 1 - 1/Ratio dB per dB above the threshold `Threshold + Tilt · octaves from 1 kHz`; up by 1 - 1/Upward per
  dB under Below (held at or under the threshold: `min(Below, Threshold)`, tilted the same), the lift fading in from
  -90 dB over 20 dB so silence and dither stay put; the sum held within ±Range. Then times the **Focus** weight
  (`focusWeight()`: all of it from Focus Low to Focus High, none a third of an octave outside each, linear in octaves
  between; crossed edges: nothing; at the defaults everything from 20 Hz up is processed and nothing under 16 Hz,
  so DC and subsonics pass untouched), and smoothed once across bins, [1 2 1] / 4 (the ends taking themselves for
  the missing neighbour): a raised cosine over the gain's impulse response, so a jagged gain smears less in time.
  Each bin is multiplied by its gain (DC and Nyquist, packed in the first bin, each by its own).
- **Changes don't click**: each frame's gains are faded in and out by the synthesis window and overlap four ways, so
  a jump in the gains becomes a crossfade over about N samples. Threshold, Below, Tilt, Knee, Range, the ratios (as
  slopes), Stereo Link and the Focus edges (in octaves) also glide per hop (a one-pole of 30 ms), so a sweep doesn't
  step; Smoothing, Attack and Release act through the envelopes. Dry/Wet, Output and Delta ramp per sample (20 ms).
  At Smoothing 0 with fast times and high ratios some "musical noise" is the device's character, as nih-plug's.
- **Automation stays with the audio**: the engine hands a device its parameters in step with its input, so the
  device keeps them in step with its latency itself. Each frame works with the parameters of two hops before it,
  when its centre went in (so a step in Threshold crossfades in about the sample it came at); Dry/Wet, Output and
  Delta go through rings, as the dry path does, and reach the output the latency on, with the input they came
  with. Through an engine, an Output step at 1 s ramps from the sample at 1 s in the mix.
- **Dry/Wet and Delta**: the dry path reads the input ring the latency back (the input N + H ago), so
  `y = ((1 - δ) dry + (1 - 2δ) m (wet - dry)) · out`, with m Dry/Wet's share and δ Delta's 0 or 1 (ramped). At
  Dry/Wet 0 % it is the input delayed by the latency, bit for bit. With Delta it is `Dry/Wet × (dry - wet)`: what the
  device takes away as it is, what it adds inverted (which the ear can't tell alone), after Output; for setting
  Threshold and Smoothing by ear.
- **Silence**: a frame whose input and key are all exact zeros skips its transforms (its spectrum is zeros); its
  envelopes release, and once they are all at their floor their levels come from a table (no logarithms). So silence
  costs almost nothing and comes out as exact zeros from 2N + H after the last sound (the latency, and the last frames
  to hear it). Nothing recursive runs on the audio path but the envelopes (floored), and the accumulators are cleared as
  they are read.
- **Sidechain**: with a source chosen the key's two channels are transformed too (two more forward transforms a
  hop) and their levels set the gains; a source that is silent (null pointers) keys nothing, so the input passes as
  it is, delayed, even with Upward on (silence is under its floor). Without one, its own input keys it. The key is
  the engine's buffer, which `BuiltinProcessor::process()` doesn't sanitise as it does the input: the device takes a
  key sample that isn't audio (NaN, infinity, beyond `kMaxInput`) as silence as it reads it.
- **One channel**: that one, exactly as the left of a stereo pair with the same signal on both. More than two: the
  first two; the rest untouched.
- **Reset** clears the rings, accumulators and spectra, puts the envelopes at their floor, and snaps every glide,
  ramp and parameter snapshot to the parameters; `prepare()` (a new rate) sizes everything again (about 350 KB at
  48 kHz, 1.4 MB at 192 kHz, the display streams apart) and resets.
- **Displays**: per hop, 128 values each of four spectral streams, log-spaced from 20 Hz to 20 kHz
  (`displayFrequency()`), then one value each of the two levels:

  | id | Values | Each |
  |---|---|---|
  | `input` | 128 a hop | the input's spectrum, pink-referenced dB (floor -150): per bin the louder channel's power, per point the mean |
  | `key` | 128 a hop | the levels the gains are worked out from (after the envelopes and Stereo Link; the key's when keyed): per bin the louder channel's, per point the highest |
  | `output` | 128 a hop | the output's spectrum: each channel's power times the square of what is heard of its gain, ((1 - δ) + (1 - 2δ) m (g - 1))² · out² (δ, m, out: Delta, Dry/Wet and Output as the frame's parameters set them; with Delta, the difference's spectrum), the louder, per point the mean |
  | `gain` | 128 a hop | the gains applied in dB (the Focus and the smoothing included): per bin the deeper of the channels' cuts, else the bigger lift; per point the same |
  | `in_level` | 1 a hop | the peak of the dry input as heard (delayed by the latency) over the hop, dBFS (floor -90) |
  | `out_level` | 1 a hop | the peak of the output over the hop, dBFS (floor -90) |

  A display point with two bins or more inside its own width (±2.8 %; from about 770 Hz up at 48 kHz, and a few points
  from 550 Hz) takes them all as the table says; below that it interpolates between the two bins around its frequency
  (power for the spectra, dB for the levels and gains); above Nyquist (at rates under 40 kHz) the floor, or 0 dB of
  gain. Each hop's spectral frame starts at a multiple of 128 in each stream (the editor reassembles frames by the index
  `readDisplayAt` gives), and the four are published three hops late (the frame published at hop h went in at hop h - 3
  and is centred on input sample hH - N - H), so a frame shows when its centre is heard, in step with the level streams.
  At 8192 values a stream holds 64 frames (0.68 s at 48 kHz). The `samplesPerValue` figures (4 and 512) are for 44.1 and
  48 kHz. Every value is finite and at most +300 dB (far above any audio: a full-scale tone reads +20 dB): a bin's power
  overflowed by an absurd sample would make a spectrum infinite, and one just short of overflowing reads far over it.
- **The shared maths**: `spectral::frameSize`, `latencySamples`, `calibrationDb`, `pinkDb`, `thresholdDb`, `belowDb`,
  `focusWeight`, `smoothingOctaves`, `kneed`, `gainDb`, `displayFrequency` (inline, no FFT): the engine runs float
  copies of them per bin. [SpectralResponse.h](../../app/src/audio/SpectralResponse.h), in the application layer, wraps
  them for the editor (with the display points per frame and the pivot, checked against these), so the threshold drawn
  is the one that plays and the Focus band is dimmed by the weights the sound gets.
- About 0.6 % of one core at 48 kHz stereo at the defaults; 0.75 % at the heaviest settings (unlinked, Upward 10:1,
  Smoothing 100 %, everything over the threshold); about 0.1 % more keyed by a sidechain; 1.2 % at 96 kHz (1.4 %
  heaviest) and 2.3 % at 192 kHz (2.7 %), the frame doubling with the rate (measured with `builtin_devices_bench` on a
  2.1 GHz Xeon with the work done in one go; spread over the hop it costs up to about a seventh more). Two forward and
  two inverse transforms of 2048 take about 34 µs of a hop's 55-60 µs; the rest is a square root, a `log10` and an `exp`
  per bin (one of each linked). Spread over the hop, a 32-sample callback carries about 8 µs of it at 48 kHz and 14 µs
  at 192 kHz (99th percentiles; in one go, it was 80-130 µs and 250-400 µs, more than a 192 kHz callback's 167 µs), and
  many instances in step add up to about their average.

### Saturator (`builtin:saturator`, AudioEffect)

Waveshaping after Ableton Live 12's Saturator: Drive into one of eight curves, Color's EQ around the curve, Post
Clip, Output and Dry/Wet, a DC filter, and 4x oversampling (Hi-Quality). The maths it shares with its editor is in
[SaturatorDesign.h](../../engine/src/builtin/SaturatorDesign.h) (namespace `saturator`).

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `drive` | Drive | dB | -36..36 | 0 |
| `type` | Type | | Analog Clip, Soft Sine, Bass Shaper, Medium Curve, Hard Curve, Sinoid Fold, Digital Clip, Waveshaper | Analog Clip |
| `threshold` | Threshold | dB | -50..0 (the Bass Shaper's) | -18 |
| `output` | Output | dB | -36..0 | 0 |
| `mix` | Dry/Wet | % | 0..100 | 100 |
| `clip` | Post Clip | | No Clip, Soft Clip, Hard Clip | No Clip |
| `color` | Color | | Off, On | Off |
| `base` | Color Amt Low | dB | -36..36 | 0 |
| `freq` | Color Freq | Hz | 30..18500, log | 1000 |
| `width` | Color Width | % | 0..100 | 50 |
| `depth` | Color Amt Hi | dB | -36..36 | 0 |
| `dc` | DC | | Off, On | Off |
| `hq` | Hi-Quality | | Off, On (not automatable: it is the latency) | Off |
| `ws_drive` | WS Drive | % | 0..100 | 50 |
| `ws_lin` | WS Lin | % | 0..100 | 50 |
| `ws_curve` | WS Curve | % | 0..100 | 50 |
| `ws_damp` | WS Damp | % | 0..100 | 0 |
| `ws_depth` | WS Depth | % | 0..100 | 0 |
| `ws_period` | WS Period | % | 0..100 | 0 |

Color's four are named as in Live 12.1 (its editor captions them Amt Lo, Freq, Width and Amt Hi); their ids are the
names they had before it (Base, Frequency, Width, Depth).

- **Signal flow**, per channel (the channels are independent): x is the input after the DC filter (as DC says);
  the dry signal is x. u is x times Drive (`expDbToGain`) through Color's emphasis; the curve shapes u; then Color's
  de-emphasis gives the wet signal, and the output is `Output × PostClip((1 - mix) x + mix wet)` (that form, so fully
  wet the dry signal adds exactly nothing). With Hi-Quality all of it but DC and Output runs at 4x: x is brought up
  (the dry signal with it) and the result brought back down after Post Clip. At the defaults, audio below 0.75
  (-2.5 dBFS) comes out bit for bit.
- **The curves** are odd (odd harmonics only, no DC from a symmetric input) and memoryless, of u:
  - *Analog Clip*: u up to 0.75, then `|u| - (|u| - 0.75)²` (its slope falling smoothly to 0) to 1 at 1.25, then 1.
  - *Soft Sine*: `sin(pi/2 u)` up to 1 (a gain of pi/2 for quiet input), then 1.
  - *Bass Shaper*: u up to the threshold t (Threshold as a level), then `t + (1 - t) tanh((|u| - t) / (1 - t))`
    towards 1: a hard clip at 0 dB, all but a tanh at -50 dB.
  - *Medium Curve*: `tanh(u)`. *Hard Curve*: `u - 4/27 u³` to 1 at 1.5, then 1. *Digital Clip*: u clamped to ±1.
  - *Sinoid Fold*: `sin(pi/2 u)` for any u, so past full scale it folds back (0 at 2, -1 at 3).
  - *Waveshaper*: with its controls as fractions, v = u held within ±1000,
    `s = 2 lin v + 2 curve v³ + depth sin(pi (1 + 15 period) v)`; Damp multiplies s by `v² / (v² + k²)`,
    k = 0.25 damp² (a gate near 0, -6 dB at |v| = k); the output is `u + drive (tanh(s) - u)`. WS Drive 0 leaves
    the input as it is; below 100 % the curve isn't bounded (Post Clip is the cure, as in Live).
  - tanh is `dsp::fastTanh`; sin is `saturator::sine` (reduced in double to ±pi/2, then an odd polynomial to r¹¹,
    within 6e-8 there).
- **Color** is an EQ before the curve and its exact inverse after it: a low shelf at 150 Hz (Q 0.7071) of Amt Lo dB,
  then a peak at Freq (kept below 0.45 of the sample rate) of Amt Hi dB, Width wide (0.25 · 16^(width/100)
  octaves, Q 5.77 at 0 %, 1.41 at 50 %, 0.27 at 100 %), all RBJ sections; after the curve the peak's inverse, then
  the shelf's (`saturator::inverse`: numerator and denominator swapped, stable since the sections are minimum
  phase). Where the curve is straight they cancel, so a clean sound comes through unchanged (within 1e-5); where it
  bends, a band boosted before it saturates harder and is turned back down after (less of it: Amt Hi +24 at 2 kHz
  takes 9.4 dB out of that band of a hard-clipped noise), and a band cut before it stays clean and is restored on
  top, ringing like a resonance (Amt Hi -24 adds 16.7 dB; Amt Lo ±24, -5.4 and +17.8 dB below 100 Hz). Switched off,
  Amt Lo and Amt Hi glide to 0 dB (where RBJ's sections are exactly 1), and once every state is below 1e-6 the filters
  are switched out and cleared; on at 0 dB they are switched out too, bit for bit as off.
- **Post Clip** comes after the de-emphasis (so it catches what that adds back) and after Dry/Wet, on the blend:
  Soft is the Analog Clip curve again, Hard a clip at ±1, so at any Dry/Wet the output never passes the Output level
  (Ableton's manual: "Saturator's output will never exceed the level set by the Output control"; a dry sound under
  the Analog Clip curve's knee passes Soft Clip untouched). With Hi-Quality it clips at 4x, so its own harmonics
  don't fold back either: a 5 kHz tone's worst alias is -72 dB rather than -35 under Soft Clip after Analog Clip at
  +12 dB, and -59 rather than -22 under Hard Clip after the Waveshaper at +18 dB. Brought back down, band-limited, a
  bright clipped sound can then pass the ceiling a little: a hard-clipped 5 kHz tone by 15 %, full-scale noise
  driven hard by up to 57 %, never more than the down filters' worst gain (1.84), as the clipping curves' own flat
  tops do with Hi-Quality.
- **DC** is a 5 Hz `dsp::DcBlocker` at the input; it always runs (so it is warm when switched on), and DC crossfades
  between the input and its output.
- **Hi-Quality** runs Color's emphasis, the curve, the de-emphasis, Dry/Wet and Post Clip at 4x
  (`dsp::Oversampler`, factor log2 2), on x brought up; the dry signal goes up and down with it (flat within
  0.003 dB to 20 kHz at 48 kHz: the input 36 samples late). `latencySamples()` reports the 36 samples (as the
  parameter is, at once); `idle()` returns true once when it changes, so the engine realigns the tracks. Color has
  filters for each rate, designed for it. What folds back below Nyquist drops from -25 to -118 dB of the output
  (Medium Curve, +18 dB, 5 kHz) and from -12 to -45 dB (Digital Clip, +24 dB, 7.1 kHz). Switched on, the 4x path
  starts from silence and runs unheard for 80 samples (its filters hold 74) before it fades in, so once faded it
  plays bit for bit as if it had been on all along (Color's 4x filters, if Color runs, start from silence too: on a
  clean sound they still cancel exactly); switched off, it fades out to the 1x path, whose Color filters likewise
  start from silence.
- **Smoothing**: the work goes in chunks of up to 16 samples. Drive, Output, Dry/Wet, Threshold and the six WS
  controls glide through two one-poles in a row of 10 ms each (`dsp::Glide`; Color's four, Freq in log, 20 ms), so
  a jump eases in and out; a glide moves a chunk at a time in closed form (after n samples its errors are
  `e1 cⁿ` and `cⁿ (e2 + n (1 - c) e1)`, exact at each chunk's end however automation splits the block) and lands
  exactly once within 1e-4 dB (1e-6 for fractions and log Hz). Across a chunk the gains and the curve's settings
  ramp linearly, sample by sample. Type, Post Clip, DC and Hi-Quality crossfade over 10 ms (an S-curve) between
  both sides (a type's two curves, at 4x too); a change during a fade starts when it is done.
- **Color while it glides** is designed anew at each chunk's end, and its coefficients move there in a straight line
  base sample by base sample (`saturator::between`; with Hi-Quality, each held over its four). The de-emphasis follows
  the same line at the same samples, since the curve between them has no memory. A section H and its inverse fed H's
  output (transposed direct form II) give back H's input exactly while the inverse's states are -1/b0 times H's; a new
  b0 breaks that, so each time the de-emphasis's coefficients move its states are rescaled by b0 old / b0 new. Through
  any glide a clean sound stays itself to float rounding (1e-8 on a 0.05 noise, also with Hi-Quality against Hi-Quality
  without Color), and switching Hi-Quality on or off mid-glide leaves it clean once the fade is done; without the
  rescale it would zipper. Coefficients held over each chunk would step 3000 times a second, which a saturated sound
  makes audible.
- `tailSamples()`: what the curve leaves ringing in Color's filters once it bends (the slowest of the peak's poles
  and zeros, Q·max(A, 1/A) / (pi f) seconds with A = 10^(depth/40), and the shelf's lower corner,
  0.7071 / (pi · 150 · 10^(-|base|/80)); only a section whose gain isn't 0) or the DC filter (1 / (2 pi 5 Hz)):
  8 time constants of the slowest, plus 80 samples with Hi-Quality (its filters' memory), at most 5 s. 0 at the
  defaults; 3.9 s at its longest (Amt Hi -36 dB at 30 Hz, Width 0 %).
- **Denormals**: every curve gives exactly 0 for 0; the DC filter's states are flushed below 1e-20, and Color's
  sections (`dsp::Biquad`) clear both their states together once both are below it (one at a time, a slow section,
  at 4x or at a low Freq, could ring at about 1e-19 for ever: what clearing one takes out, its partner puts back);
  the FIRs aren't recursive and the glides land, so silence rings out to exact zeros without the renderer's
  flush-to-zero. `reset()` clears every state and snaps every glide and fade to the parameters (no pre-roll: from
  silence a 4x path is already in step); `prepare()` (a new rate) works out the glides and fades again and resets.
  On one channel it processes that one, as a stereo pair's left.
- **Displays**: `in_peak` and `out_peak`, one value per 128 samples (`kMeterSamples`, counted across stretches): the
  louder channel's peak of x (after DC, before Drive) and of the output, linear, since the editor's curve has linear
  axes; `input` and `output`, one value per sample, x and the output summed to mono. The editor draws the live level
  on its curve from `in_peak` and the In/Out strips, and the input and output spectra behind Color's EQ curve.
- **Input that isn't audio** (NaN, infinity) never reaches its states: `BuiltinProcessor::process()` takes it as
  silence first, so the DC filter (which runs while DC is off too), Color's sections and the 4x path's filters can't
  keep it; what comes out is exactly what zeros there would give.
- **Shared with the editor**: the curves (`saturator::makeShape`, `params`, `curve`, `transfer`), Post Clip, Color's
  design (`colorDesign`, `colorResponseDb`) and Hi-Quality's factor and latency (`kHqFactorLog2`, `hqLatency()`) are
  inline in SaturatorDesign.h; the application layer's `saturatorCurve()`, `saturatorSlope()`, `saturatorColorDb()` and
  `saturatorHqLatency()` ([app/src/audio/SaturatorResponse.h](../../app/src/audio/SaturatorResponse.h)) call them, so
  the curve drawn is the curve played, from the same float arithmetic. The editor also takes the Type list's Bass Shaper
  and Waveshaper entries and the parameters' ranges (`saturatorRange()`, what its drags stay within) from there.
- At 48 kHz stereo on the machine it was written on (`builtin_devices_bench`): 0.09 % of one core at the defaults,
  0.12 % at Medium Curve +12 dB, 0.31 % with the Waveshaper's ripples and gate, Color and DC; with Hi-Quality too,
  and Drive at +12 dB, 0.67 % ([benchmarks/README.md](../../benchmarks/README.md)). Before `dsp::Oversampler`'s stages
  were vectorised, Hi-Quality took about 1 % (the 4x filters most of it), 1.7 % at its heaviest (Hi-Quality,
  Waveshaper, Color at 4x, DC, Soft Clip, half wet) and 2.0 % with every glide moving all the time.

### Amp (`builtin:amp`, AudioEffect)

A guitar amplifier after Ableton Live's Amp: seven models, an amp's dials, Mono or Dual and Dry/Wet. Each model is a
voicing of one structure ([AmpDesign.h](../../engine/src/builtin/AmpDesign.h)): an input stage, three triode stages
around a passive tone stack, Presence, a power stage whose supply sags, and the output transformer. Softube's models
are unpublished: this is a behavioural design, built to do what Live's manual says each control does and to sound like
the amps the models are known to be. As Live's, it has no speaker simulation: alone it sounds bright and fizzy, as a
real amp's line output does, and wants a cabinet or a low-pass after it.

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `type` | Amp Type | | Clean, Boost, Blues, Rock, Lead, Heavy, Bass (a list) | Clean |
| `gain` | Gain | dial | 0..10 | 5 |
| `bass` | Bass | dial | 0..10 | 5 |
| `middle` | Middle | dial | 0..10 | 5 |
| `treble` | Treble | dial | 0..10 | 5 |
| `presence` | Presence | dial | 0..10 | 5 |
| `volume` | Volume | dial | 0..10 | 5 |
| `dual` | Dual Mono | | Mono, Dual (a list) | Mono |
| `mix` | Dry/Wet | % | 0..100 | 100 |

The names are Live's automation names ("Dual Mono" is its Output switch). The dials' unit `dial` reads "5.0" (one
decimal, no sign: `formatValue` in [ParamSpec.cpp](../../app/src/model/ParamSpec.cpp)).

- **Signal flow** (one amp): at the base rate an input coupling high-pass and a bright cap's shelf (first order);
  then, 4x oversampled (`dsp::Oversampler`), V1, V2, the tone stack, V3, Presence, the power tubes' input (a one-pole
  low-pass at 12 kHz), the power stage and the transformer (a one-pole low-pass at 7.5 to 12 kHz); back at the base
  rate, a 10 Hz DC blocker and the model's output trim.
- **A triode stage** is a Miller low-pass, a gain, a cubic soft clipper (1.5 x − 0.5 x³ inside ±1, ±1 outside) around
  its bias point, and a coupling high-pass. Its small-signal gain is exactly the voicing's (the curve's slope at the
  bias is divided out); a positive bias clips the top first, so it makes the even harmonics of a triode driven into
  grid current, and the coupling high-pass takes away the DC the asymmetry makes. The curve's antiderivative is a
  polynomial, so first-order antiderivative anti-aliasing (ADAA: the curve averaged over the line from the last input
  to this one) costs a division per sample. It is written without branches (every case worked out, the right one
  chosen), and two clipped samples give exactly ±1. With the 4x oversampling it keeps what folds back of a 997 Hz
  sine at −12 dBFS 82 to 85 dB down at noon and 66 to 83 dB at Gain 10 (Lead the most), 57 dB with Gain, Presence and
  Volume at 10 on Lead (77 to 83 on the others); 4x without it measured −36 dB on Lead at Gain 10.
- **The tone stack** is the passive Fender/Marshall/Vox stack as Yeh and Smith model the '59 Bassman's: a third-order
  filter whose coefficients are polynomials in the three pots' positions, so the controls interact as a real amp's do
  (Treble and Middle are linear pots, Bass an audio taper: 9 % at noon). Each model has its own component values
  (`kVox`, `kFender`, `kMarshall`, `kModern`, `kVintage`, `kPa`) and a make-up gain that puts its response at noon at
  0 dB at its peak. It is the bilinear transform at the oversampled rate (within 0.02 dB of the analog response from 30
  Hz to 16 kHz), run in double as transposed direct form II. It sits between V2 and V3, so turning a band up drives V3
  harder in it. Its three states are flushed together, never one at a time: with its poles near z = 1 they nearly
  cancel, and clearing some of them kicks the stack back into ringing.
- **Presence** is a first-order high shelf (±8 or ±9 dB at the ends) before the power stage, as a power amp's
  feedback presence is, so turning it up drives the power stage's highs harder. Between them, the power tubes' input
  (their grids' Miller capacitance) is a one-pole low-pass at 12 kHz (`amp::kGridHz`): without it a preamp already
  clipped, its edges boosted by Presence, reached the power stage's curve at full height, and with Gain, Presence and
  Volume at 10 what folded back was only 44 dB down on Lead. It takes 0.03 dB off 1 kHz.
- **The power stage** is one more such stage with a push-pull's slight bias. Its supply **sags**: per base-rate
  sample its output's peak (of its four oversampled samples) drives an envelope (attack 5 to 15 ms, release 80 to 300
  ms), and the drive is divided by `1 + sag × env`: a loud chord pulls it down by up to 20 log10(1 + sag) dB and lets
  it back over the release, compression and bloom.
- **The voicings** (`amp::kVoicings`, every number in [AmpDesign.h](../../engine/src/builtin/AmpDesign.h)):

  | Model | Stack | Gain 0..10 | Character at noon (220 Hz, −12 dBFS) |
  |---|---|---|---|
  | Clean | Vox | −18..+18 dB | chiming, V1 well under clipping (4 % THD, the 2nd harmonic 12 dB over the 3rd), V2 a clean cathode follower |
  | Boost | Vox | −6..+30 dB | V1 at the knee, V2 past it: crunch (31 %) |
  | Blues | Fender | −12..+24 dB | a clean preamp, the deep Bassman scoop, a power stage near its knee with the deepest sag: Volume distorts it (9 % THD at noon, 28 % at Volume 10) |
  | Rock | Marshall | −6..+30 dB | V1 at the knee into a cascaded V2: the Plexi's crunch (28 %) |
  | Lead | Modern | 0..+30 dB, half at V1, half at V3 | tight (an 80 Hz input high-pass, 120 Hz coupling), V2 and V3 far over (45 %), little sag |
  | Heavy | Vintage | −6..+32 dB, half at V1, half at V3 | looser and darker, V2 and V3 at their knees: a crunch (34 %, the 2nd harmonic 12 dB over the 3rd) into a power stage that sags and is driven 9 dB past clipping at Volume 10 (44 %) |
  | Bass | PA | −12..+30 dB | big asymmetric bias at V1 and V2 (fuzz: the 2nd harmonic 18 dB over the 3rd), the PA stack's lows, a power stage that distorts with Volume |

- **The dials**: Gain is linear in dB from the model's Gain 0 to its Gain 10 (on Lead and Heavy split between V1 and
  V3, around an early tone stack). Volume is the power stage's drive, ±12 dB around the model's noon (Volume 0 is
  quieter, not silent): on Blues, Heavy and Bass it takes the power stage 5 to 9 dB past clipping at 10 and adds
  distortion (Blues 9 to 28 % THD from noon, Heavy 34 to 44, Bass 26 to 32), on the others it stays at or under it,
  as Live's manual says. Each model's trim level-matches it at the defaults: a −12 dBFS sine comes out at its own RMS
  (−15 dBFS) on every model.
- **Smoothing**: control-rate work happens per chunk of up to 16 samples (`kChunk`), on a grid counted from the
  meters' 256-sample windows (so a window always ends with a chunk). The six dials glide (one-poles of 20 ms); while
  anything moves, every gain, bias and filter coefficient (the one-poles', the tone stack's) and the sag's amount
  ramps linearly across each chunk from where the last one ended, per oversampled sample in the oversampled section:
  a coefficient's step is a step in the signal's slope, which a cleanly played sine shows. A stage's offset follows
  its bias sample by sample (the curve half a step back, where the anti-aliasing averages it as the bias moves), so a
  stage at rest stays at 0: ramped, it left a step of DC at every chunk's join. While nothing moves the chunks reuse
  the last controls and run loops without ramps. A dial moving recomputes only what it sets (the stages' input gains,
  the tone stack, Presence's shelf or the drive).
- **Amp Type** morphs over 50 ms (an S-curve) from the voicing as it is to the new one: dB values, biases and the sag
  linearly, frequencies, times and the tone stack's parts geometrically, so every coefficient moves continuously. A
  new model during a morph starts a new one from the voicing in between. A blend of two voicings clips where neither
  does (the heads, the gains and the trim stop agreeing midway), so on the way it came out up to 6 dB louder than both
  (Lead to Bass); the trim keeps it level-matched: `amp::Transfer`'s harmonic balance (below; a `BasicTransfer<64>`)
  gives each voicing's level (the RMS of a 1 kHz tone at the input's recent peak, 64 samples a period), and the blend
  is turned by how far its level is from the two models' in between, worked out at 17 points of the morph as it
  reaches them and joined by a smooth curve. What the rate sets (every sine and cosine those levels take) and what
  each model sets (its filters' coefficients and their responses: `amp::BasicTransfer`'s `Rate` and `Voice`), and the
  logarithms a blend takes of each model's frequencies, are worked out in `prepare()` and at construction: a level
  from a model's costs what the dials add and the period's FFTs (a real period through a complex FFT of half its
  length), about the work the amp does for 16 samples, a blend's half as much again. They come one per 16-sample cell
  of the control grid at most: the morph sets off once its first four are known (1 ms after the change at 48 kHz,
  during which the amp plays on as the old model), a change's first 32-frame block (the two models' levels) costs
  about two steady ones, and no block of a morph more than about three (measured with glibc: a blend's level still
  calls exp and pow, so a slower maths library costs more). It goes by the grid, so a morph sets off and
  moves at the same samples whatever the blocks. Through every one of the 42 morphs the level stays within +0.9 /
  −1.4 dB of the two models (a −12 dBFS sine, 5 ms windows). Each chunk of a morph applies the compensation to the
  voicing's own trim (never to the last chunk's, which carries it already), so dials moving while a morph waits change
  nothing in its level, and changes coming faster than a morph can set off hold the blend they found, at its level.
- **Mono and Dual** (the Output switch): Mono runs one amp on (L + R) / 2 and writes it to both channels (half the
  work); Dual runs one per channel. The switch crossfades over 20 ms (an S-curve on the amps' inputs and on the right
  channel's output); the second amp starts from rest, its input fading in over 5 ms, and stops once Mono is back. A
  switch during a fade starts when it is done. On one channel, one amp, and the switch does nothing.
- **Dry/Wet** (20 ms ramp) mixes in each channel's input delayed by the latency, so at 0 the output is the input
  delayed, bit for bit, while the amps go on running (it comes back without a seam).
- **Latency**: 37 samples at any rate (`latencySamples()`, constant): the oversampler's 36, and one for the four stages'
  anti-aliasing (half an oversampled sample each) with a two-sample FIFO after the transformer. Besides it the wet has
  its filters' own phase delay (the one-pole low-passes a fraction of a sample each, as an analog amp's;
  `amp::Transfer::phaseDelay()` at 1 kHz). **Tail**: the latency and half a second. The slowest pole is the tone
  stack's: 53 ms at most (the PA stack with Bass and Middle at 10, Treble at 0); half a second takes it 80 dB down.
- **Silence and sleep**: every stage's anti-aliasing is primed at its bias wherever it starts from silence (`reset()`,
  the second amp starting, waking), so a stage at rest puts out exact zeros (`amp::restValue`); every recursive state
  is flushed when tiny (under 1e-20) at the chunk grid's points, and the supply's envelope under 1e-8 every sample
  (where `1 + sag × env` rounds to 1), so silence rings out to exact zeros (within a second at noon, 2.5 s at most:
  the PA stack's slowest pole) and blocks of any size play alike, bit for bit (flushed at the blocks' ends, they
  didn't). An amp whose input has been exactly silent for 4096 frames, whose output has been exact zeros for 256 (its
  down-sampler and DC blocker, whose states it can't read, hold nothing more) and whose states are all at rest
  **sleeps**: it puts out the zeros it would, its supply's sag recovering over its release meanwhile (sample by
  sample, as awake), and costs next to nothing until its input sounds again. Once the sag has recovered, a woken amp
  plays as a freshly reset one, bit for bit. `reset()` snaps the dials and the model to the parameters and silences
  both amps; `prepare()` sizes the buffers (blocks longer than its maximum are worked in slices) and resets.
  Input that isn't audio (NaN, infinity, beyond ±1e30) reaches it as silence (`BuiltinProcessor::process()`), so
  none of its states ever holds it: a bad sample plays as a zero would, bit for bit. One that is audio but absurd
  (+600 dBFS) blocks it as it would a real amp: the input coupling holds the spike's tail far over the signal until
  it has died away (0.15 s on Lead's 80 Hz, 0.7 s on Bass's 15 Hz), and it plays on.
- **Displays**, one value each per 256 samples (`amp::kDisplaySamples`), published together at each window's end
  (with two amps, the larger): `input` (the input's peak, dBFS), `drive1`, `drive2`, `drive3` (each preamp stage's
  peak against its clipping point, dB: 0 is where it clips), `power` (the power stage's, the same), `sag` (the
  supply's sag at the window's end, dB, ≥ 0) and `output` (the output's peak after Dry/Wet, dBFS); floored at −90
  (`amp::kDisplayFloorDb`). The editor takes the floor from the design through the application layer
  (`ampDisplayFloorDb()`), as it takes the models' names (`ampModelNames()`), and the rate from the displays
  themselves (`DeviceCanvas::readRecent`).
- **Shared with the editor**: `amp::toneResponseDb()` (the tone stack's digital response with its make-up, times
  Presence's shelf, at the oversampled rate: the tone controls' part; the fixed roll-offs of the power tubes' input
  and the transformer aren't in it) and `amp::Transfer`, the transfer for a 1 kHz tone worked out by harmonic balance:
  one period of the tone (256 samples) through each stage's curve sample by sample and through each filter between
  them harmonic by harmonic, by its own response there (the anti-aliasing's half-sample average among them; harmonics
  above the oversampled rate's half dropped, and above the base rate's at the output), the supply's sag a lower power
  drive. For a tone of each peak it gives the output's highest and lowest values: played, a tone's peaks land within
  0.1 dB of them on every model at the defaults (−40 to −3 dBFS), and within 0.6 dB with Gain, Presence and Volume at
  10. Its preamp's part doesn't depend on the sag, so the editor keeps it and remakes only the power stage's as the
  sag moves. Both through the application layer's [AmpResponse.h](../../app/src/audio/AmpResponse.h)
  (`ampToneResponseDb`, `ampTransfer`, `AmpTransferCurve`): the curves drawn are the sound.
- **Cost** (48 kHz, `builtin_devices_bench`, the best of three on a 2.1 GHz VM, where OTT measured 0.44 % in the
  same runs; [benchmarks/README.md](../../benchmarks/README.md)): 0.68 % of one core at the defaults (Mono), 1.36 % in
  Dual (Lead, Gain 10: the same at any setting, the structure never changes), in proportion to the rate. Measured
  before `dsp::Oversampler`'s stages were vectorised (when the defaults took 1.0 %, Dual 1.9 %, and about 40 % of it
  was the oversampler's up and down): automating a dial added next to nothing (0.04 %), a model change every 100 ms
  0.3 % (each morph works its levels out), and a model change's first block cost about 3.3 times a steady one in
  blocks of up to 64 frames (3 % of the block's time), 2.3 times at 128, 1.9 times at 256. A silent track costs next
  to nothing once its amp sleeps.

### Erosion (`builtin:erosion`, AudioEffect)

After Live 12.4's Erosion: the input read out of a 2 ms delay whose read position a sine or band-passed noise
wobbles at audio rate ([ErosionDesign.h](../../engine/src/builtin/ErosionDesign.h)). That phase-modulates every
partial, the more the higher it is: lows survive, highs turn to grit and hiss, and the sidebands past Nyquist fold
back as the "digital" aliasing the device is for. There is no Dry/Wet (Live's has none either): at Amount 0 it is a
clean delay of its latency.

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `freq` | Frequency | Hz | 20..18000, log | 1000 |
| `width` | Filter Width | oct | 0.1..10, log | 2.5 |
| `amount` | Amount | % | 0..100 | 25 |
| `blend` | Noise Blend | % | 0..100 | 100 |
| `stereo` | Stereo Width | % | 0..100 | 0 |

- **The delay**: its centre D is 2 ms in whole samples (`centreDelaySamples`: 88 at 44.1 kHz, 96 at 48 kHz, 384 at
  192 kHz), read with 4-point Hermite interpolation at D + E·m, m the modulator (RMS 1/√2) and E the excursion:
  Amount's square times the most (`excursionSamples`; 25 % moves it ±87 µs, 100 % ±1.38 ms). The most is the limit
  D − 2 over √2, so a sine never reaches the limit, where the read is clamped (it never reads the future or past the
  line), and noise peaks only near the top of Amount. No dry path; with E exactly 0 for a chunk the line is read at
  D as it is (what Hermite gives at a whole delay, and cheaper).
- **The modulators**: the sine is one quadrature phasor in double (renormalised each chunk), its sides
  sin(phase ± h), h = Stereo × π/4: a quarter cycle apart at 100 %, uncorrelated as the noise is. The noise is two
  white noises (mid and side, `dsp::Noise`), each through two identical band-pass sections (TPT
  state-variable, `k·v1`: the RBJ band-pass at 0 dB peak) whose −3 dB edges are Width octaves apart around
  Frequency (each section's Q is √(√2 − 1) / (h − 1/h), h = 2^(Width/2), so each is 1.5 dB down there); two
  sections give 12 dB/octave skirts, so a narrow band is selective. The noise is scaled to RMS 1/√2 exactly: its
  power gain through the pair is worked out in closed form (`noisePowerGain`: the sum of the squares of the pair's
  impulse response, from its autocorrelation), so Frequency and Width move where its energy is, never how hard it
  modulates. Stereo mixes mid and side to the sides with gains 1/√(1 + w²) and w/√(1 + w²) (w = Stereo/100): each
  side keeps its RMS, their correlation is (1 − w²)/(1 + w²). Noise Blend crossfades sine and noise at equal power
  (the weights exactly 1 and 0 at the ends). The modulator plays at most 0.45 of the sample rate. Live's legacy
  modes are settings: Sine is Noise Blend 0 %, Noise 100 %, Wide Noise 100 % with Stereo 100 %.
- **Its own noise**: each instance salts the noises' seeds (`kMidSeed`, `kSideSeed`) with a number of its own
  (`dsp::hash32` of the count of Erosions made before it, `instancesMade`), kept for its life. Two Erosions set
  alike therefore don't modulate alike: on double-tracked parts they would otherwise wobble in lockstep, and an
  export, which resets every device at once, would line them all up. Its renders still repeat (reset() reseeds it
  with its own seeds); a project loaded again gets new noise, statistically the same. (The tests set the count
  back to 0 to make devices that play alike.)
- **Smoothing**: the work runs in chunks of 16 samples on a grid that goes on across blocks and stretches, so the
  output never depends on how a block was split (offline renders are bit-identical at any block size). At each chunk's
  start every control's glide (`dsp::Glide`: two one-poles of 10 ms in a row, so a jump eases in and out; Frequency
  and Width in log) moves on a chunk, and what it makes ramps linearly across the chunk: the excursion, the blend's
  weights, the noise's gain and its mid and side gains, the sine's offset, and the sections' g and k, from which the
  sections' coefficients are made sample by sample while they move. While the band moves, the sections' states ease
  across each chunk by as much as their steady level changes (k × scale over the chunk's: the noise, scale k v1, keeps
  RMS 1/√2): the energy they built at the old band would otherwise modulate several times too hard after a long move
  down or a widening, and too weakly after a narrowing, for a few hundred ms, until it decayed at the new band's rate.
  The phasor's rate steps per chunk (its phase stays continuous). Once a glide has landed nothing that depends on it
  is worked out again (Frequency's and Width's logs are taken only when the parameter changes): a settled chunk does
  no transcendental work. The modulators never stop or restart, so there is no phase jump to hide, and there are no
  lists or switches to crossfade.
- **One channel** gets the left modulator, worked out exactly as in stereo (both noises run), so a mono run is the
  left side of a stereo one. More than two: the first two. A channel that wasn't processed starts from silence.
- **Latency and tail**: `latencySamples()` is D (2 ms), so the engine delays the other tracks by it and at Amount 0
  the device is transparent, to the sample. `tailSamples()` is 2D: no sample stays longer. Nothing in it decays (the
  delay has no feedback, the sections always hear noise), so silence comes out as exact zeros 2D samples on. It
  reads no sidechain; NaN or infinity in its input is silence by the time it hears it (`BuiltinProcessor`), so its
  line never holds one.
- **Reset**: the line and sections cleared, the noises reseeded with the instance's own seeds and the phasor
  restarted (its renders repeat exactly, noise included), every glide and ramp where the parameters are.
  `prepare()` (a new rate) works out D, the limit and the glides again, and resets.
- **Displays**:

  | id | Values | Each |
  |---|---|---|
  | `input` | one per sample | the input summed to mono ((L + R)/2, or the one channel) |
  | `output` | one per sample | the output summed to mono |
  | `erosion` | one per 256 samples (`erosion::kMeterSamples`) | what the device changed, in dB: the power of the output less the input D samples ago; floor −90 (`erosion::kFloorDb`; exactly that at Amount 0) |
  | `mod_l`, `mod_r` | one per sample | the left and right modulators (before the excursion and the clamp; one channel: the left twice), pushed together so they stay in step |

  The modulators go out every sample: decimated, a sine near a multiple of the decimated rate's Nyquist would look
  like two points on a line, and the editor's scope (and the tests' correlations) would mislead.
- **The shared design**: `erosion::band`, `bandMagnitude` and `bandEdges` give the noise band's sections, its
  magnitude and its −3 dB edges from the same maths the device plays; `excursionMs` the excursion; `blendWeights`
  and `stereoSpread` the blend's and Stereo's gains. The application layer's
  [app/src/audio/ErosionResponse.h](../../app/src/audio/ErosionResponse.h) (`erosionBandMagnitude`,
  `erosionBandEdges`, `erosionExcursionText`...) wraps them for the editor, so the band drawn is the filter that
  plays; `erosionPeakDb` reads the `erosion` display's values against its floor (`kFloorDb`: the application
  layer's `kErosionFloorDb` is checked equal to it).
- About 0.21 % of one core at 48 kHz stereo, at the defaults and at Amount 100 %, Noise Blend 50 %, Stereo 100 %
  (0.12 % at Amount 0); about 0.26 % with all five controls gliding all the time (measured with
  `builtin_devices_bench` on a 2.1 GHz Xeon).

### Delay (`builtin:delay`, AudioEffect)

A stereo delay after Ableton's.

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `l_sync` | L Sync | | Off, On (a list) | On |
| `l_division` | L 16th | | 1, 2, 3, 4, 5, 6, 8, 16 (a list) | 3 |
| `l_time` | L Time | ms | 1..5000, log | 250 |
| `l_offset` | L Offset | % | -33..33 | 0 |
| `r_sync`, `r_division`, `r_time`, `r_offset` | R ... | | as the left's | On, 4, 375, 0 |
| `link` | Link | | Off, On | Off |
| `feedback` | Feedback | % | 0..95 | 50 |
| `freeze` | Freeze | | Off, On | Off |
| `filter` | Filter | | Off, On | On |
| `freq` | Filter Freq | Hz | 50..18000, log | 1000 |
| `width` | Filter Width | | 0.5..9 (octaves) | 8 |
| `mode` | Mode | | Repitch, Fade, Jump | Repitch |
| `ping_pong` | Ping Pong | | Off, On | Off |
| `mix` | Dry/Wet | % | 0..100 | 50 |

- **Times**: synced, `division × (15 / tempo) × (1 + offset / 100)` seconds; free, the
  time in ms. Linked, the right side uses the left's four parameters. At most 10 s
  (`kMaxDelaySeconds`); each delay line is that long (rounded up to a power of two),
  allocated in `prepare()`. Read with 4-point Hermite interpolation.
- **Per side**: the line is read (`echo`), filtered (`wet`: the output, and what feeds
  back), and written with the input plus feedback × wet. Ping pong: the input summed to
  mono goes into the left line, and each side's wet feeds the other's line. Frozen,
  each line is written with its own unfiltered echo (an exact loop) and the input is
  ignored; feedback is 1.
- **Filter**: a TPT state-variable high-pass at `freq / 2^(width/2)` then low-pass at
  `freq × 2^(width/2)`, both Butterworth, limited to 0.49 of the sample rate.
- **Modes**: Repitch glides the delay to its target with a one-pole (120 ms), so the
  read speed (pitch) changes; Fade crossfades (equal power, 60 ms) from the old time to
  the new, starting a new fade only when the last one is done; Jump switches at once.
- Dry/Wet and feedback are ramped over 20 ms (snapped on `reset()`). On one channel the
  two sides' wet are averaged. `tailSamples()`: until feedback takes the echoes down
  60 dB, at most 60 s.
- **Display**: `input`, one value per sample: the input summed to mono. The editor
  draws its spectrum behind the filter curve.

### Chorus-Ensemble (`builtin:chorus`, AudioEffect)

Modulated delays added to the sound, after Ableton Live 12's Chorus-Ensemble: from light thickening through flanging
to vibrato. Three modes: **Chorus** (Live 11's Classic: one or two delays a side, with Live 12.4's Taps and Time),
**Ensemble** (three a side, their modulation a third of a cycle apart: thicker and smoother) and **Vibrato** (one a
side, moved further, its wave from a sine to a triangle, the right side Offset behind the left). Where each voice's
delay sits and how it moves is in [ChorusDesign.h](../../engine/src/builtin/ChorusDesign.h), which the editor's
graph shares.

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `mode` | Mode | | Chorus, Ensemble, Vibrato (a list) | Chorus |
| `taps` | Tap Count | | 1, 2 (a list; Chorus only) | 2 |
| `time` | Delay Time | | Auto, 7 ms, 10 ms, 20 ms, 35 ms, 50 ms (a list; Chorus only) | Auto |
| `rate` | Rate | Hz | 0.1..15, log | 0.8 |
| `amount` | Amount | % | 0..100 | 50 |
| `feedback` | Feedback | % | 0..100 (not in Vibrato) | 0 |
| `fb_invert` | Feedback Invert | | Off, On (not in Vibrato) | Off |
| `width` | Width | % | 0..200 (not in Vibrato) | 100 |
| `offset` | Offset | ° | 0..180 (Vibrato only) | 0 |
| `shape` | Shape | % | 0..100 (Vibrato only) | 0 |
| `warmth` | Warmth | % | 0..100 | 0 |
| `hp` | High-pass | | Off, On | Off |
| `hp_freq` | High-pass Freq | Hz | 20..2000, log | 100 |
| `output` | Output | dB | -36..6 | 0 |
| `mix` | Dry/Wet | % | 0..100 | 50 |

A control a mode doesn't use is still listed, kept and automated (as Ableton keeps it); it plays again when its mode
comes back.

- **Voices.** Each reads its side's delay line at `centre + swing × lfo` (lfo -1..1), Amount being `a` (0..1):

  | Layout | Voices a side | Centre | Swing | Their phases, left / right (cycles) |
  |---|---|---|---|---|
  | Chorus, Time Auto | 1 or 2 (Taps) | 1.5 + 5a ms | 5a ms | 1 tap: 0 / ½; 2 taps: 0, ½ / ¼, ¾ |
  | Chorus, Time 7..50 ms | 1 or 2 | that time | 4a ms | as Auto's |
  | Ensemble | 3 | 7.5 ms | 5a ms | 0, ⅓, ⅔ / ⅙, ½, ⅚ |
  | Vibrato | 1 | 6 ms | 5.5a ms | 0 / Offset |

  Auto's centre grows with Amount (a short, gentle chorus at low Amount, a deep one at high), as the device did before
  Live 12.4; a fixed Time holds still whatever the Amount (no phasing creeping into a bass or a guitar). Chorus and
  Ensemble move by a sine; Vibrato by a sine that Shape morphs towards a triangle in step with it. The voices are
  averaged, so at Amount 0 the wet is the input delayed by the centre, exactly. Two taps a side are opposite (equal and
  opposite detune, their mean delay steady), and the two sides' voices are always out of step, so a mono input comes
  out wide. A delay shrinking at speed m (ms a ms) plays 1 + m times as fast, and m is at most `swing × 2π × rate`
  (swing in seconds): ±22 cents at the defaults; never so fast that a voice plays backwards (at most 0.52: Vibrato
  at 15 Hz).
- **Per channel, a sample at a time:**
  - The High-pass splits the input with a 4th-order Linkwitz-Riley crossover (`dsp::Crossover`; 20..2000 Hz, at most
    0.45 of the rate): the highs go into the delay line, the lows to the wet unmodulated, so a bass stays solid and a
    fully wet Vibrato keeps its lows. The dry goes through the same crossover (its lows and highs summed: an all-pass,
    flat in level), so it is in phase with the wet's lows and Dry/Wet blends the two whole (left as it was, the dry
    would cancel the crossover's lows round the frequency, phase-shifted by up to 180°: 13 dB at it half wet). The
    switch blends between the whole input going in and the highs, and the dry between the input and the all-pass.
    The crossover always runs, so switching it on finds it warm.
  - The voices are read before the line is written (an echo of d samples comes exactly d samples later, and feedback
    adds none), 4-point Hermite between samples, at a delay kept in double (a 50 ms line is read to a millionth of a
    sample), and averaged.
  - **Warmth** (`w` = Warmth / 100): into the line, the input blended by `w` with a soft sigmoid, `u / sqrt(1 + u²)`
    of `u = 0.65 (x + 0.15)` (the bias for even harmonics), its offset taken out and its slope at silence brought back
    to 1: a bucket-brigade chip's input stage, so echoes going round the loop saturate and darken instead of growing.
    After the voices, a one-pole low-pass at `4 kHz × w^-0.6` (about 16 kHz at 10 %, 6 kHz at 50 %, 4 kHz at 100 %;
    none at 0), and a 5 Hz DC blocker on the wet while it plays (the bias makes DC). Subtle, a colour rather than a
    limiter: at Warmth 100 a 200 Hz sine at -6 dBFS comes out 0.3 dB down with 2.5 % distortion (its 2nd harmonic at
    -33 dBc, its 3rd at -39 dBc), at 0 dBFS 1.1 dB down with 5.4 %; quiet sounds keep their level; at Amount 0 it is a
    gentle saturation of its own. There is no oversampling (the curve is inside the feedback loop, where an
    oversampler's latency can't go): the curve's bend (it less the identity) is anti-aliased through its
    antiderivative, `sqrt(1 + u²)` (first-order ADAA: the bend averaged over each step, half a sample late; a square
    root a sample), which takes the folded harmonics down 10 to 17 dB more, while the identity part has no delay or
    droop. A loud (-6 dBFS) 13 kHz tone's folded harmonics come out 47 dB down, a 9 kHz one's 53 dB, a 1760 Hz saw's
    63 dB. At Warmth 0 the wet is untouched, bit for bit.
  - **Feedback**: the wet goes back into the line through a second 5 Hz DC blocker (a DC offset passes once, as through
    any delay, and doesn't build up round the loop) and a limiter (the identity within ±1, `sign(x) (1 + tanh(|x| -
    1))` beyond it: never past ±2), so even a loud tone on the loop's resonance at Feedback 100 % stays within the
    input and ±2 (without it, it would head for 33 times the tone). 100 % is a loop gain of 0.97.
    Invert flips its sign (hollow, nasal combs). Vibrato has none, as Live's (whose Feedback and Ø are greyed
    there): going into it takes the gain down to 0, and Feedback and Invert wait for Chorus or Ensemble.
  - The wet (with the lows added back while the high-pass is on): Width scales its side (0 % mono, 100 % as it is,
    200 % twice as wide; 100 % in Vibrato), Output its level, and Dry/Wet blends it with the dry as the Delay's does
    (fully dry, the input passes bit for bit; with the high-pass on, through the crossover's all-pass).
- **Smoothing**, so no control clicks or zippers (measured: see the tests):
  - The modulation is worked out per chunk of up to 16 samples (`chorus::kChunk`). Rate (in log), Amount, Shape,
    Offset, Warmth and the high-pass frequency (in log) each glide through two one-poles in a row (of 10, 25, 10, 50,
    10 and 10 ms), solved exactly in continuous time at each chunk's middle and end, so a jump eases in and out. Each
    voice's delay is worked out there and follows the parabola through the chunk's start, middle and end: the delay
    has no corners, so the pitch never steps (a straight line per chunk would step it at every chunk's end). Warmth's
    amount and filter and the crossover's coefficients follow their glides the same way, sample by sample. The LFO's
    phase integrates the rate, so a jump of Rate never clicks.
  - Dry/Wet, Output, Width and the high-pass switch glide per sample through two one-poles of 6 ms each (a jump is
    90 % there in 23 ms, and lands exactly within about 120 ms), the feedback's signed gain (Invert passes it through
    0, Vibrato takes it to 0) and the DC blocker's blend through two of 10 ms (`dsp::Glide`; the per-chunk glides
    keep their own, solved over a fraction of a sample as well, so they land alike however a block cuts the chunks).
  - A change of Mode, Taps or Time cross-fades over 30 ms (an S-curve) from the old voices' reading of the lines to
    the new voices' (both read the same lines, so the feedback carries the blend); a change during a fade starts when
    it is done. A change that doesn't apply to the mode (Taps in Ensemble) changes nothing.
  - A chunk never crosses a display value's end, so the displays come every 128 samples however the blocks fall. A
    block's end (or automation) cuts a chunk short, but the delays follow the same curve however the chunks fall:
    renders in different block sizes differ by about 1e-5 at most.
- **Channels**: one or two; any more pass through untouched. One plays as the left of two (Chorus's one or two taps,
  Ensemble's three, Vibrato's one), without Width. A change of the channel count clears the lines and filters (a
  fill: nothing is allocated).
- `prepare()` allocates the lines (60 ms each, rounded up to a power of two), works out the glides' steps and resets.
  `reset()` clears every line and filter, starts the LFO again at phase 0 (so offline renders repeat exactly) and snaps
  every glide and ramp to its parameter. The lines then hold none of the sound, so what goes into them fades in over
  5 ms (an S-curve, as long as the renderer's switch fade): a device switched on in the middle of a sound (reset as
  it comes on, its output faded in) starts each voice's copy of it smoothly, a delay later, instead of with a step
  mid-waveform after the renderer's fade is over. (The first 5 ms of a sound that starts with a render reach the
  delays faded too.) A change of the channel count, which clears them, does the same.
- Input that isn't audio (NaN, infinity, beyond 1e30) is silence (`BuiltinProcessor::process()`), so nothing of it
  stays in the lines, the loop or the filters.
- **Denormals**: the crossovers' (`Crossover::flush()`) and the low-passes' states are flushed below 1e-20 after each
  stretch, the DC blockers flush their own, and what is fed back is gated below 1e-15, so silence rings out to exact
  zeros (after Feedback 90 %, in about 2 s).
- `latencySamples()` is 0: the delay is the effect. `tailSamples()`: the layout's longest delay at any Amount
  (`chorus::highestMs`) times one plus the repeats until the feedback has taken an echo down 60 dB (`-3 / log10(0.97 ×
  feedback)`, at most 1000; none in Vibrato), and 50 ms for the filters; at most 60 s. 2952 samples at the defaults
  (48 kHz); 12.4 s at 50 ms with Feedback 100 %. Fully dry, 0 (with the high-pass on, 50 ms for the dry's crossover).
- **Displays**, a value per 128 samples each (`chorus::kDisplaySamples`), published together so value k of each stands
  for the same samples: `phase` (the LFO's phase, 0..1, at the end of those samples: the editor draws every voice
  from it with the shared maths, so its traces move as the delays do) and `level` (the wet's peak after Output, both
  sides, in dB, floor -90 (`kLevelFloorDb`): the traces' glow). The editor takes the newest phase, and of `level`
  the values its last tick covers (`DeviceCanvas::readRecent`, which knows each display's samples per value).
- **Design**: [ChorusDesign.h](../../engine/src/builtin/ChorusDesign.h) (namespace `sub::chorus`, inline, no Qt)
  holds the layouts (`layout()`, which normalises what a mode doesn't use), `centreMs`, `swingMs`, `lowestMs` /
  `highestMs`, `voicePhase`, `lfoValue`, `delayMs`, the detune (`detuneUpCents`, `peakDetuneCents`), the warmth's
  curve (`WarmCurve`, its antiderivative, `warm`; `WarmStage`, the device's anti-aliased stage) and filter
  (`warmLowpassCoefficient`, through `dsp::onePoleCutoff`), and `limitFeedback`. The application layer's
  `chorusDelayMs()` and its neighbours ([app/src/audio/ChorusVoices.h](../../app/src/audio/ChorusVoices.h)) wrap it
  for the editor's graph, so the delays drawn are the delays that play.
- At 48 kHz on two channels it took about 0.3 % of one core at its defaults, and under 0.6 % at its heaviest
  (Ensemble, Amount 100 at 15 Hz, Feedback 100 inverted, Warmth 100, the high-pass on, Width 200; Warmth's
  anti-aliased curve and filter about 0.15 % of that), on the machine it was written on (`builtin_devices_bench
  --device chorus`, where the Disperser took 0.23 %); with every glide moving and Mode changing five times a second,
  about 0.5 %.

### Phaser-Flanger (`builtin:phaser`, AudioEffect)

Ableton's Phaser-Flanger: a phaser, a flanger and a doubler in one device, swept by two LFOs and an envelope follower
([PhaserDesign.h](../../engine/src/builtin/PhaserDesign.h), shared with its editor's graph).

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `mode` | Mode | | Phaser, Flanger, Doubler (a list) | Phaser |
| `notches` | Notches | # | 1..42, 41 steps | 4 |
| `center` | Center | Hz | 70..18500, log | 1000 |
| `spread` | Spread | % | 0..100 | 50 |
| `blend` | Blend | | 0..1 | 0 |
| `flange_time` | Flanger Time | ms | 0.1..20, log | 2.5 |
| `doubler_time` | Doubler Time | ms | 20..150, log | 30 |
| `amount` | Amount | % | 0..100 | 50 |
| `feedback` | Feedback | % | 0..100 | 50 |
| `fb_invert` | Feedback Invert | | Off, On | Off |
| `lfo_sync` | LFO Sync | | Off, On | Off |
| `lfo_freq` | LFO Freq | Hz | 0.01..40, log | 0.5 |
| `lfo_rate` | LFO Rate | | Live's 22 divisions, 1/64 .. 8 Bars (`dsp::syncedDivisionLabels()`, a list) | 1 Bar |
| `lfo_wave` | LFO Waveform | | Sine, Triangle, Triangle Analog, Triangle 8, Triangle 16, Saw Up, Saw Down, Rectangle, Random, Random S&H | Triangle |
| `lfo_duty` | Duty Cycle | % | -100..100 | 0 |
| `spin_on` | Spin On | | Off, On | Off |
| `phase` | Phase | ° | 0..360 | 180 |
| `spin` | Spin | % | 0..50 | 10 |
| `lfo2_mix` | LFO 2 Mix | % | 0..100 | 0 |
| `lfo2_sync` | LFO 2 Sync | | Off, On | Off |
| `lfo2_freq` | LFO 2 Freq | Hz | 0.01..40, log | 2 |
| `lfo2_rate` | LFO 2 Rate | | as `lfo_rate` | 1/4 |
| `env_on` | Env Follow | | Off, On | Off |
| `env_amount` | Env Amount | % | -100..100 | 50 |
| `env_attack` | Env Attack | ms | 0.1..30, log | 10 |
| `env_release` | Env Release | ms | 0.1..400, log | 200 |
| `safe_bass` | Safe Bass | Hz | 5..3000, log (5: off) | 5 |
| `warmth` | Warmth | % | 0..100 | 0 |
| `output` | Output | dB | -36..6 | 0 |
| `mix` | Dry/Wet | % | 0..100 | 50 |

The ranges are Live's (its Output a gain of 0..2, here in dB down to -36). The LFOs' ids begin `lfo_` and `lfo2_`, so
`freq` keeps meaning a filter's frequency on every device.

- **Phaser**: Notches identical second-order all-passes in a row, the Disperser's normalized lattice stages
  (`disperser::design`), tuned to Center, their Q the Spread (`qOfSpread`: 5 · 0.03^spread, 5 at 0 %, 0.866 at 50 %,
  0.15 at 100 %). Blended with the input each cuts a notch where the cascade turns the phase an odd number of half
  circles (all the way down at Dry/Wet 50 %); Spread moves them apart. Each channel has its own states, in double.
  Its feedback is taken a sample late (the last wet sample goes back in). The modulation moves Center ±3 octaves per
  unit (Blend 0) or Spread by half its range (Blend 1), or both in between. The stages are kept between 10 Hz and
  0.45 of the rate softly: within a quarter of an octave of either the swept frequency bends (a tanh knee in log2 Hz,
  `softCenterLog2`) towards the limit, and a Spread modulated past its range bends the same way (at most 0.1 beyond),
  so a sweep run into a limit turns smoothly rather than stopping with a corner (which the stages would turn into a
  tick at the LFO's rate). Only the top of the Center parameter's own range reaches the knee: at 48 kHz 18.5 kHz
  plays 1.3 Hz lower, at 44.1 kHz at 18.3 kHz (at 96 kHz and above, none of it).
- **Flanger and Doubler**: a delay line per channel (`dsp::DelayLine`, 250 ms), read with 4-point Hermite
  interpolation at the delay, its fraction in double (a float delay resolves only 1/2048 of a sample at 150 ms, which
  a moving read turns into jitter), exact at whole samples: 1 ms at 48 kHz is the input 48 samples later. Amount moves
  the flanger's delay ±2 octaves per unit, the doubler's ±15 %; the delay is held to at least 2 samples. The line is
  written in every mode (in Phaser mode with the input alone), so a delay mode coming in reads real history at once.
  After a reset the line is empty; if the first frame the device then hears isn't silent (it was switched on in the
  middle of a sound), what the line is written with fades in over 5 ms (`kStartFadeSeconds`, an S-curve, sample by
  sample whatever the blocks), so the copy read a delay later comes in smoothly instead of with a step (which would
  come after the renderer's own 5 ms switch fade had ended: a 30 ms Doubler's measured 0.134 on a 0.3 sine). A sound
  that starts after silence goes in untouched. (An offline render starts with a reset too: a sound already playing at
  its first frame, or a hit right on it, reaches the line faded the same way.)
- **Feedback**: `feedbackGain` (±0.95 at 100 %, negative with Ø) into the core's input, through a soft limit (as it
  is up to +6 dBFS, then bent to stay below ±4): 95 % with any modulation can't run away.
- **Modulation**: `(1 - mix2) · LFO 1 + mix2 · LFO 2`, times Amount, plus the envelope times Env Amount, held to ±2.
  Control works per chunk of 16 frames: the LFOs and the envelope give the modulation at each chunk's end; it is
  drawn across the chunk and smoothed sample by sample (two one-poles of 1 ms in a row). The Phaser's stages move
  linearly from one chunk end's coefficients to the next's (a lattice's coefficients in between are points inside the
  unit circle: it can only lose energy while they move), unless that straight line would miss their true way at the
  chunk's middle by more than a millionth of an octave (of the stage's frequency or its bandwidth): a fast, deep
  sweep, a triangle's turn, Amount or Duty gliding, the envelope's attack, where straight pieces would kink at every
  chunk's end. Then they are designed sample by sample from the smoothed modulation, as while Center glides. The
  delays follow the smoothed modulation sample by sample (a long delay, swept, would turn a chunk's corners into a
  zipper).
- **LFOs**: Ableton's ten shapes (`waveValue`), Duty bending the phase (`warpPhase`: the first half of the shape takes
  5..95 % of the cycle; Rectangle's width). Triangle 8 and 16 step the triangle 8 or 16 times a cycle. Triangle Analog
  is a ±1 rectangle through a 100 ms one-pole in its steady state (`analogShape`, `analogValue`): nearly square when
  slow, a quieter rounded triangle when fast (0.46 high at 5 Hz), its shape following the rate (glided, so a jump of
  Freq doesn't jump its level). Random glides from one cycle's value to the next's, Random S&H holds one a cycle,
  both from the cycle's number (renders repeat exactly). LFO 2 is a triangle. Free, they run at 0.01..40 Hz (at
  40 Hz, a chunk is 1/75 of a cycle). The right channel runs Phase ahead of the left (gliding to it the short way
  round, 50 ms), or with Spin `1 + spin` times as fast (at most 1.5). Synced while the song plays an LFO is at the
  song's position (`(beat / division)`, cycles and all); stopped, it runs free at the tempo's rate. The divisions
  are Live's: 1/64, 1/48, 1/32, 1/24, 1/16, 1/12, 1/8, 1/6, 3/16, 1/4, 5/16, 1/3, 3/8, 1/2, 3/4 of a bar, then 1,
  1.5, 2, 3, 4, 6 and 8 bars (`dsp::syncedCycleBeats`).
  Where its phase jumps (a new waveform, Sync switched on, a synced division changed, the transport starting or
  looping) its value crossfades from where it was over 20 ms (an S-curve) rather than jump.
- **Envelope follower** (Env Follow): the input's peak (the louder channel; before Safe Bass) with Attack and Release,
  as `-48..0 dBFS → 0..1` (`envelopeAmount`). Off, once its amount has glided to 0, it is reset, so switching it on
  starts from silence.
- **Warmth**: `y + w (L(tanh(2y) / 2) - y)`, `L` a 5 kHz one-pole: the effect's output saturated and darkened, inside
  the feedback loop, so echoes and resonances darken as they recirculate. Skipped while 0. The saturation runs at the
  sample rate, antialiased by its antiderivative (first-order ADAA: each sample the curve's mean between the last input
  and this one, `(F(y) - F(y₋₁)) / (y - y₋₁)` with `F = ln cosh(2y) / 4`; its value at the middle when the two are
  within 1e-5), so the harmonics it makes beyond Nyquist fold back far quieter: a 9 kHz tone at -1 dBFS (48 kHz) folds
  its 3rd to 21 kHz 31 dB under the tone and its 5th to 3 kHz 49 dB under (20 and 26 dB unantialiased), a 7 kHz tone its
  5th to 13 kHz 49 dB under (34). Only what the curve adds to `y` goes through the mean; `y` itself passes as it is, so
  a quiet signal has exactly the response the design draws (no half-sample delay).
- **Safe Bass**: a Linkwitz-Riley crossover (`dsp::Crossover`) at its frequency per channel; the lows stay dry
  (`out = low + (1 - mix) high + mix wet(high)`: the bands add up to an all-pass, flat in level). It fades in and out
  over 20 ms, its crossover's frequency moving sample by sample as it glides; at 5 Hz it is off and costs nothing.
- **Output and Dry/Wet**: `out = Output · ((1 - mix) dry + mix (s low + wet))`. Fully dry with Output at 0 dB and Safe
  Bass off it passes the input bit for bit, whatever the rest does.
- **Nothing steps**: Center, Spread and Blend glide (two one-poles of 20 ms in a row), the delay times too (50 ms: a
  gentle swoop of pitch), and while they glide what they move is worked out sample by sample (the stages designed, as
  the Disperser's are; the delays), landing exactly on what is set (the delay times within 1e-9 of an octave: a
  coarser landing would be a jump of 0.005 samples at 150 ms). Amount, Env Amount, LFO 2 Mix, Duty and Safe Bass's
  frequency glide per chunk (20 ms each pole); Feedback, Warmth, Dry/Wet and Output per sample (5 ms each pole). A
  change of Mode crossfades the cores over 30 ms (into the Phaser, its cascade starts from silence and its input
  fades in; a delay core's feedback fades with its share; Flanger and Doubler crossfade two reads of one line, with
  no swoop and no gap). A change of Notches fades over 20 ms as the Disperser's Amount does (fewer: to the tap after
  the stages kept; more: the stages added hear their input fade in). A change during a fade waits for it. Every one
  of the 30 controls changing in turn where it is heard (81 changes over 16 s), then a 5 Hz sweep run into the
  stages' top and bottom limits, a 220 Hz tone's 6th difference stays below 2e-5 (the Disperser's limit: 1e-4):
  1.9e-5 at Dry/Wet's jump from 0 to 100 %, 1.7e-5 at Output's 30 dB, every other change 1.4e-5 at most and most of
  them about 2e-6, which is also what a steady sweep measures.
- **Denormals**: it doesn't rely on the renderer's flushing. Recursive states (the cascade's, Warmth's, the
  crossovers') are flushed per chunk (`dsp::flushTiny`, `Crossover::flush()`); the cascade's output and the delay
  line's writes below 1e-20 are zero, and so is an output sample that would be a denormal float (gains gliding to 0
  together, Dry/Wet and Warmth turned down at once, take their product through that range). Silence rings out to
  exact zeros. NaN and infinity in its input never reach it (`BuiltinProcessor::process()` takes them as silence): it
  plays on as if they had been zeros.
- `reset()` clears every state, starts the LFOs from phase 0 and snaps every glide and fade to the parameters (an
  offline render starts the same every time), and arms the line's start fade; `prepare()` sizes the lines (250 ms) and
  resets. On one channel the left LFO plays; channels past the second pass untouched.
- `latencySamples()` is 0 (the delay and the sweep are the effect). `tailSamples()` (0 fully dry): the feedback's
  passes to -60 dB (`ln 1e-3 / ln |g|`) times the loop's longest delay (Phaser: the slowest stage the modulation
  reaches, its largest group delay times Notches, plus its own ring, with half as much again to spare; the delay
  modes: the longest delay), plus Safe Bass's ring; at most 60 s.
- **Displays**, one value per 256 samples each, pushed together: `phase` and `phase_r` (LFO 1's phase on the left and
  the right, 0..1), `lfo` (LFO 1's value on the left, after the shape, Duty and any jump's fade), `mod` (the left's
  smoothed modulation, -2..2), `env` (the envelope's 0..1; 0 while Env Follow is off), `sweep_l` and `sweep_r` (Phaser:
  the stages' frequency in Hz, as tuned; Flanger and Doubler: the delay in ms; during a mode's fade, the incoming
  mode's), `q_l` and `q_r` (the stages' Q; 0 in the delay modes), `input` and `output` (the peaks, dB, floor
  `kLevelFloorDb`, -90). The editor reads the rate (`kDisplaySamples`) and the parameters' ranges through the
  application layer (`phaserDisplaySamples()`, `phaserRanges()`).
- **Response**: what plays, as a linear filter (Warmth's saturation left out), is `phaser::transfer()` and
  `responseDb()`: the cascade's `A^N` through the feedback loop and its extra sample (`P / (1 - g z⁻¹ P)`), or the
  comb's `D / (1 - g D)`, with Warmth's low-pass, Safe Bass's bands, Dry/Wet and Output. `notchFrequencies()` puts the
  notches in closed form (the bilinear transform keeps the analog all-pass's phase at the warped frequency:
  `Ω² + (cot θ / Q) Ω - 1 = 0`), `stagePhase()` a stage's phase, unwrapped. `phaser::curve()` is what the editor draws:
  per column of the plot, up to 12 points and every notch inside it where the response turns less than a cycle; where a
  comb is finer than the columns, its top and bottom at 48 fixed angles of the turning factor (the same each time, so
  the band stands still as the sweep moves). It designs the Phaser's stage and works out Output's gain once per curve
  (the same sums `responseDb()` does per point): 0.1-0.3 ms for the editor's 226 columns. The application layer's
  `phaserResponseDb()` and `phaserCurvePoints()` ([PhaserResponse.h](../../app/src/audio/PhaserResponse.h)) wrap them,
  so the curve drawn is the sound; `curve()` also gives each column's turn (the radians the wet path turns across it,
  what tells the dense columns), with which the graph draws a comb finer than it can show as a line as a band too.
  `wetTransfer()` is the wet path alone (what Dry/Wet at 100 % plays, before Output).
- **Cost** (`builtin_devices_bench`, 48 kHz stereo): about 0.2 % of one core at the defaults (0.21 %, the
  Disperser 0.21 % in the same runs); the Flanger 0.30 % and the Doubler 0.25 %; 42 notches, Feedback 95 %, Amount
  100 %, Random S&H 0.65 %; a deep, fast sweep (Amount 100 %, a 5 Hz sine: the stages designed every sample) 0.49 %
  (at 40 Hz, Live's fastest, 0.48 %); Warmth at 100 % 0.43 % (the Flanger's 0.68 %: the antialiased saturation's exp
  and log, per sample and channel); everything on at once (42 notches, Triangle Analog, Warmth, Safe Bass, Env
  Follow, LFO 2, Spin) 1.3 %. Center automated without pause 0.49 %, with 42 notches 1.0 %.

### Reverb (`builtin:reverb`, AudioEffect)

An algorithmic reverb after Ableton Live 12's Reverb: an input filter, early reflections, and a diffusion network with a
decay time per band, chorus, Freeze, and three output levels. Mono in, stereo out, as Live's is: the input's two sides
are summed before the reverb (a source panned hard left reverberates in the middle), and Stereo sets how wide the reverb
comes out. The maths it shares with its editor is in [ReverbDesign.h](../../engine/src/builtin/ReverbDesign.h)
(namespace `reverb`).

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `predelay` | Predelay | ms | 0.5..250, log | 2.5 |
| `lo_cut` | Lo Cut | | Off, On | On |
| `hi_cut` | Hi Cut | | Off, On | On |
| `in_freq` | In Filter Freq | Hz | 50..18000, log | 830 |
| `in_width` | In Filter Width | oct | 0.5..9 | 7.5 |
| `spin` | ER Spin | | Off, On | On |
| `spin_rate` | ER Spin Rate | Hz | 0.07..1.3, log | 0.3 |
| `spin_amount` | ER Spin Amount | % | 0..100 | 25 |
| `shape` | ER Shape | % | 0..100 | 50 |
| `density` | Density | | Sparse, Low, Mid, High | High |
| `smooth` | Size Smoothing | | None, Slow, Fast | Slow |
| `size` | Room Size | size | 0.22..500, log (a bare number) | 100 |
| `stereo` | Stereo Image | ° | 0..120 | 100 |
| `lo_shelf` | Lo Shelf | | Off, On | On |
| `lo_freq` | Lo Shelf Freq | Hz | 20..15000, log | 90 |
| `lo_gain` | Lo Shelf Gain | % | 20..100 (of Decay) | 75 |
| `hi_filter` | Hi Filter | | Off, On | On |
| `hi_type` | Hi Filter Type | | Shelf, Low-pass | Shelf |
| `hi_freq` | Hi Filter Freq | Hz | 20..16000, log | 4500 |
| `hi_gain` | Hi Shelf Gain | % | 20..100 (of Decay) | 70 |
| `decay` | Decay Time | ms | 200..60000, log | 1200 |
| `freeze` | Freeze | | Off, On | Off |
| `flat` | Flat | | Off, On | On |
| `cut` | Cut | | Off, On | On |
| `diffusion` | Diffusion | % | 0..100 | 70 |
| `scale` | Scale | % | 0..100 | 50 |
| `chorus` | Chorus | | Off, On | On |
| `chorus_rate` | Chorus Rate | Hz | 0.01..8, log | 0.8 |
| `chorus_amount` | Chorus Amount | % | 0..100 | 20 |
| `reflect` | Reflect Level | dB | -30..6 | 0 |
| `diffuse` | Diffuse Level | dB | -30..6 | 0 |
| `mix` | Dry/Wet | % | 0..100 | 40 |

- **Input filter**: a TPT state-variable high-pass (Lo Cut) at `freq / 2^(width/2)` then low-pass (Hi Cut) at
  `freq * 2^(width/2)`, both Butterworth, each held to 10 Hz..0.45 of the rate. Each switch cross-fades its section in
  or out; a section switched off (and done fading) is skipped. Its exact magnitude is `reverb::inputFilterDb`.
- **Size** scales every delay of the reverb (reflections, diffusers, lines) by `s = sqrt(size / 100)`: 0.047 at
  0.22 (tiny and metallic), 2.24 at 500. It glides at Smooth's pace (two one-poles of 2 ms, 350 ms or 50 ms: None,
  Slow, Fast), a sample at a time; with None the tail's pitch sweeps fast, but never steps. A change of Smooth glides
  the pace itself (in log, over about 50 ms), so a sweep under way speeds up or slows down without a corner.
- **Early reflections**: 12 taps of the filtered input from the predelay on, at 0..53.6 ms times `s` (the first at
  the predelay itself: Predelay is the time to the first reflection, as Live's), each with a sign and a pan
  (`reverb::kTapMs`, `kTapPan`, `kTapSign`; Sparse uses every other one), read with 4-point Hermite interpolation.
  Shape sets their envelope, `exp(-a t / t_last)` with `a = 0.4 + 5.6 shape`, the gains sharing an energy of 0.49
  (`earlyTaps`): low, they fade slowly; high, fast. Spin drifts each tap's time later (0 to 2 ms at 100 %, times
  `min(1, s)`: never before the predelay; `reverb::spinDriftMs`) and swings its pan, each tap a twelfth of the LFO's
  cycle on from the one before. The pan swings in the angle of an equal-power pan (left `cos t`, right `sin t`; at rest
  exactly `sqrt((1 -+ pan) / 2)`), by up to `asin(0.7) / 2` (a tap in the middle swings +-0.7), less near the field's
  edges, so no tap swings past one and its gains never turn a corner (`reverb::spinPan`). Reflect sets their level.
- **The network's input** is read from the same line Shape's onset after the predelay (`0.8 * 53.6 ms * s * shape`:
  the diffuse sound starts sooner when the reflections fade slowly, later when they fade fast), drifting with the
  first reflection (so Spin's motion reaches the tail, as Live's manual has it), then blurred by 2, 3 or 4 Schroeder
  all-passes in a row (Dattorro's lengths, 3.6..12.7 ms, times `s` and Scale's `0.25..1.75`; gains 0.75 / 0.625 times
  the square root of Diffusion, so its default blurs the echoes smooth by about 150 ms).
- **The network**: a feedback delay network of 4, 8 or 16 lines (Sparse, Low, Mid and High; 29..98 ms times `s`,
  about geometric and off exact ratios). Each line is read through a first-order Thiran all-pass (lossless, so
  every pass loses exactly what the design says at every frequency; linear or Hermite reads would dull the highs a
  little each pass), then through two TPT one-pole shelves: the high one passes `kHi` above its corner (or, as a
  low-pass, nothing), the low one `kLo` below its own. Each line's gain and shelves are worked out from its own
  length (`reverb::loop`), so every line loses the same dB per second in each band: the middle `-60 / Decay`, the
  shelved bands `-60 / (Decay * share)` at their far end. High adds a Schroeder all-pass in each loop (11 % of the
  line times Scale's factor). A Walsh-Hadamard transform, rotated by one line (a bare Hadamard is its own inverse:
  every second pass would undo the mixing), mixes them and writes them back with the input (signs per line). The
  tail's left and right are two orthogonal Hadamard rows of the lines' reads, times 1/4 whatever the Density (the
  sum carries all the network's energy however many lines share it, so every Density is as loud) and a trim of 1.3
  (`reverb::kDiffuseGain`: the tail about as loud as the reflections at the defaults), times Diffuse.
- **Stereo Image** (degrees, as Live's) scales the wet's side: 0 is mono, 120 the network's own two sides, independent
  of each other (late-tail correlation about 0), the default 100 a little narrower (`reverb::stereoWidth`: stereo /
  120).
- **Moving delays**: Chorus drifts each line's delay (up to +-2 ms, a quarter of the line at most; each line at its
  own rate, 0.885..1.115 of Chorus Rate, and phase); Size and Scale glide them. A Thiran all-pass's state was worked
  out for the delay a sample before, so it is moved with the delay (by the step times the input's slope there),
  which leaves an error of the step's square: a sweep reads about 20 times cleaner. When the whole number of samples
  changes, the state is worked out again for the new split, running the new all-pass over the last 4 samples, so the
  change leaves no transient.
- **Freeze, Cut, Flat**: Freeze glides the tail's decay to 1000 s with Cut (the input no longer reaches the network:
  new sound makes reflections only) and to 60 s without (the input keeps adding, and a held input settles at a level
  rather than growing). Flat glides the shelves flat while frozen, so every band holds; off, the shelves go on taking
  their bands away (the frozen tail darkens while its middle holds).
- **Guard**: while the tail's output (before Diffuse) peaks above 8 (+18 dBFS), the loops' gains are eased down 3 %
  a sub-chunk, at most to half, and back once it doesn't (`gain += 0.002 (1 - gain)`). Ordinary material never gets
  near it: full-scale noise into a frozen tail that takes its input settles about +0.5 dBFS RMS.
- **Density** crossfades from the network as it is to the new one over 20 ms, both running (the lines of either,
  read and filtered once; each network's own diffusers, loop all-passes, matrix, input signs and outputs). Each line is
  written the crossfade (cos and sin of a quarter turn, on an S-curve) of what the two networks write into it, kept at
  its power (the two writes' correlation, worked out at the switch, divided out), so the lines that stay are never
  faded through silence: a frozen tail keeps what they hold (High and Mid share every line; Low's are all High's).
  Lines, loop all-passes and diffusers that join start from silence (let go, as below) and hear their input ease in
  (one that heard a step would play it back a delay later); those that leave are faded out, taking what they hold. A
  loop all-pass's length moves into or out of the loops' gains over the crossfade. A change during one waits for it
  to end. The reflections glide to the new Density's taps.
- **Smoothing**: what moves a delay (Predelay, the onset, Size, Scale) and the levels on the audio (Reflect, Diffuse,
  Stereo, Dry/Wet, the input filter's switches) glide a sample at a time, two one-poles in a row (`dsp::Glide`; 25 ms,
  Smooth's, 50 ms, 5 ms), so nothing has a corner. Everything else glides every 32 samples (two one-poles of 20 or
  30 ms; the frequencies, times and LFO rates in log) and what it sets (the loops' and shelves' gains and corners, the
  input filter's corners, the all-passes' gains, the taps' gains and Spin's drift, Chorus's drift) is ramped linearly
  across the 32. Every glide lands on its target exactly once within a hair of it, so a settled control is exactly its
  target and nothing is worked out again. Dry/Wet is `dry (1 - mix) + wet mix`: exactly dry at 0, exactly wet at 1.
- **Sleeping**: once the input has been below -160 dB for longer than the input line reaches, and the tail and
  reflections are below -120 dB (not frozen, no Density change under way), it sleeps: the wet is exactly 0, the
  network is skipped, and the output of silence is exact zeros. The glides and LFOs go on (the Spin and Chorus
  displays keep stepping, or go to -1 if switched off meanwhile); what they set is snapped where it is when sound
  wakes it. Its buffers are let go as it falls asleep: they read as silence from then on, so a reverb that wakes
  reaching further back (Predelay, Size or Shape raised while it slept) hears silence, not what it held. No buffer is
  cleared whole at once (falling asleep, waking, a reset or a change of Density alike): each 32 samples clear only
  what their reads will reach (as far as the delays' glides go in those 32, worked out a sample at a time, plus a few
  samples: a jump of Size clears as its glide travels, not to its target at once), and asleep a slice of 16384 floats
  of the rest goes each 32 samples. States below 1e-20 are flushed after each sub-chunk.
- `reset()` lets every buffer go and clears every state, restarts the LFOs and snaps every glide: renders after it are
  the same every time. On one channel, that channel is the input and the wet's middle the output; channels past two
  are left alone.
- `latencySamples()` is 0. `tailSamples()`: the predelay, the onset and Spin's drift of it, the diffusers, a pass of
  the longest loop and Chorus's depth, 1.15 times Decay and 0.1 s (then the tail is 60 dB down); frozen, or at most, a
  minute.
- **Displays**, the 256-sample ones pushed together: `input` (the mono input's peak, dB, floor -90:
  `reverb::kMeterFloorDb`), `early` (the reflections' peak after Reflect), `diffuse` (the tail's RMS after Diffuse),
  `spin` and `chorus` (the LFOs' phases 0..1 after the value's last sample, so they step by exactly `rate * 256 / fs`;
  -1 while switched off and faded, asleep too; the chorus one is the first active line's), and per sample `signal`
  (the mono input, before the filter) and `tail` (the tail's mid after Diffuse). Asleep, the levels read -90 and the
  per-sample ones publish nothing.
- **Shared design**: `reverb::decaySeconds(settings, f, fs)` is the tail's decay time per frequency as the network
  plays it (the loop of a line of the active lines' mean length, through the same `rates`, `loop` and one-pole
  responses: in a network that mixes every line into every other each pass, energy decays at the lines' mean loss
  over their mean length); `inputFilterDb`, `earlyTaps`, `spinPan`, `onsetMs`, `stereoWidth` give the editor the
  band, the reflections, Spin's swing and Stereo's width. The application layer wraps them for the editor
  ([app/src/audio/ReverbResponse.h](../../app/src/audio/ReverbResponse.h)).
- It took about 1.4 % of one core at the defaults (High, Spin and Chorus on) at 48 kHz stereo on the machine it was
  written on (`builtin_devices_bench`), 1.8 % at its heaviest (Size 500, Predelay 250 ms, Decay 60 s, Spin and Chorus
  at 100 % and their fastest, Diffusion and Scale 100 %, Shape 100 %); Mid 1.4 %, Low 1.0 %, Sparse 0.65 %, Chorus off
  1.3 % (measured while the steady network still had one more multiply a line); about twice its cost
  for the 20 ms of a Density change; asleep 0.05 %. No block costs much more than its neighbours: at 192 kHz in blocks
  of 256, one that wakes it just after it fell asleep takes about what any other does (it took three times as long
  when the rest of the buffers were cleared then), also with Size jumping from its least to its most as it wakes
  (about 1.8 times as long when the clearing reached the glide's target at once); a Density change's first block
  about what the rest of its crossfade does (up to three times as long when the lines that join were cleared whole);
  so does a reset. Its buffers take about 1.4 MB at 48 kHz (6 MB at 192 kHz).

### Disperser (`builtin:disperser`, AudioEffect)

Phase dispersion after Kilohearts' Disperser: identical second-order all-passes in a row
([DisperserDesign.h](../../engine/src/builtin/DisperserDesign.h)). Every frequency keeps its
level; what is around the stages' frequency comes out later (a transient becomes a chirp).

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `amount` | Amount | stages | 0..64, 64 steps | 16 |
| `freq` | Frequency | Hz | 20..20000, log | 1000 |
| `pinch` | Pinch | Q | 0.1..10, log | 1 |
| `mix` | Dry/Wet | % | 0..100 | 100 |
| `bypass` | Bypass | | Off, On | Off |

- **A stage** is the bilinear transform of the analog all-pass (s² - s/Q + 1) / (s² + s/Q + 1),
  prewarped to the frequency (as the RBJ cookbook's): (A2 + A1 z⁻¹ + z⁻²) / (1 + A1 z⁻¹ + A2
  z⁻²), magnitude exactly 1. The frequency is kept below 0.45 of the sample rate
  (`stageFrequency`). Every stage and channel share the coefficients; each channel has its
  own states (in double).
- **It runs as a normalized lattice** (Gray and Markel): two rotations, by k1 = -cos w0 and
  k2 = (1 - a) / (1 + a) (a = sin w0 / 2Q), each with its c = sqrt(1 - k²). Rotations keep
  energy, so a stage's output and states hold exactly what went in however its coefficients
  move: modulating Frequency or Pinch never puts out more energy than the device was given.
  (A state-variable filter's band-pass state holds Q times the signal, and dropping Q while it
  rings throws that out: tried while choosing, one put out up to 40 times the energy it had been
  given, where the lattice put out none more.) The
  output is one multiply-add from the input, so the cascade's path through 64 stages is short.
- **Frequency and Pinch glide** in log, through two one-poles of 20 ms in a row (a jump eases
  in and out), and the coefficients follow sample by sample while they move. Swept fast through
  many narrow stages the glide is heard as the sweep itself (what the stages held catches up as
  the delay shrinks), never louder than what went in.
- **Amount** fades over 20 ms (an S-curve) to the output after the new number of stages: fewer,
  from the longer cascade's output to the tap after the stages kept; more, the stages added start
  from silence and their *input* fades in while the old output fades out (fading their output
  instead would let them smear the signal's abrupt start into a chirp longer than the fade). Stages
  no longer heard are cleared. A change during a fade starts when it is done. At 0 stages (and
  none coming) the input passes untouched, bit for bit.
- **Dry/Wet and Bypass** glide (two one-poles of 5 ms each: they can turn back halfway without a
  kink); what is heard of the stages' output is Dry/Wet's share times Bypass's (0 or 1), the rest
  the input. With no latency the two line up to the sample, so in between they add up as a
  phaser's do, |(1 - mix) + mix H|: notches where the stages turn the phase half a circle (all the
  way down at an even blend). The level is flat only fully wet. Fully dry or bypassed, once there,
  it passes the input bit for bit. The stages keep running meanwhile, so it comes back without a
  seam; switching the device off saves them.
- **Denormals**: states below 1e-20 are flushed to 0 after each stretch, so silence rings out to
  exact zeros. `reset()` clears every stage and snaps the glides and fades to the parameters;
  `prepare()` (a new sample rate) works out the glides and fades again and resets.
- `latencySamples()` is 0: the dispersion is the effect, and the engine doesn't delay the other
  tracks against it. `tailSamples()` (0 with no stages, bypassed or fully dry): until what is left of the impulse response is 60 dB down,
  1.25 times the largest group delay (`maxGroupDelaySamples`, which looks at the poles' angle too:
  high up the peak is narrower than any grid) plus 7 time constants of the slowest pole
  (`decayPerSample`), a bound measured over the parameters' range at 44.1 and 192 kHz; at most 60 s.
- **Group delay**: `disperser::groupDelayMs(stages, freq, pinch, sampleRate, frequency)` works it
  out from the stage's biquad coefficients (each polynomial's Re(Σ n cₙ z⁻ⁿ / Σ cₙ z⁻ⁿ)). The
  application layer's `disperserGroupDelayMs()`
  ([app/src/audio/DisperserResponse.h](../../app/src/audio/DisperserResponse.h)) wraps it for the
  editor's graph, so the curve drawn is the delay that plays. Its peak is about 2Q / (pi f) seconds
  per stage.
- 64 stages on two channels took about 1.5 % of one core at 48 kHz on the machine it was written on.

### EQ (`builtin:eq`, AudioEffect)

An equalizer after Pro-Q: 24 bands, then an output gain.

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `b1_used` .. `b24_used` | Band n Used | | Off, On (hidden, not automatable) | Off |
| `bn_on` | Band n On | | Off, On | On |
| `bn_type` | Band n Type | | Bell, Low Shelf, Low Cut, High Shelf, High Cut, Notch, Band Pass, Tilt Shelf | Bell |
| `bn_freq` | Band n Freq | Hz | 10..22000, log | 1000 |
| `bn_gain` | Band n Gain | dB | -30..30 | 0 |
| `bn_q` | Band n Q | | 0.025..40, log | 1 |
| `bn_slope` | Band n Slope | | 6, 12, 18, 24, 30, 36, 48, 72, 96 dB/oct (a list) | 12 |
| `bn_place` | Band n Placement | | Stereo, Left, Right, Mid, Side | Stereo |
| `output` | Output | dB | -36..36 | 0 |
| `scale` | Gain Scale | % | 0..200 | 100 |

- A band plays while it is `used` and `on`. Its gain is times `scale` / 100 (bells,
  shelves, tilt).
- **Design** ([EqDesign.h](../../engine/src/builtin/EqDesign.h), `eq::design`): each band
  is a cascade of analog sections: a bell, notch or band pass is one second-order
  section; a cut or shelf of slope `6n` dB/octave is of order `n` (one first-order
  section if odd, then pairs) with Butterworth Qs, the most resonant times `Q / 0.7071`;
  a shelf's gain is shared out over its sections; a tilt shelf is a high shelf down
  half its gain. Each section is made digital by matching (Vicanek, 2016): poles by
  impulse invariance (their frequency kept below 0.97 of Nyquist), the numerator so
  that the magnitude is the analog one's at 0 Hz, Nyquist and the section's frequency.
  Where no real numerator has all three (a resonant cut, a notch high up), Nyquist's
  gives way. So bells and shelves high up keep their analog shape, not the bilinear
  transform's squeezed one.
- **Processing**: per 32 samples, each band glides its frequency and Q (in log) and gain
  towards its parameters (a one-pole, about 15 ms) and is designed again while they
  move; a band switched on starts from its parameters, silent. Sections run in double,
  transposed direct form II; their states are flushed below 1e-20. Placement Mid or
  Side converts the chunk to mid and side and back around the band; on one channel,
  every band but Side's applies. The output gain is ramped over 20 ms.
- `tailSamples()`: 7 Q / (π f) for the narrowest band (at most 5 s).
- **Displays**: `input` and `output`, one value per sample, each summed to mono: the
  editor's analyzer.
- **Response**: `eq::responseDb(design, frequency, sampleRate)` gives a band's
  response in dB at a frequency, from the same `eq::design()`. The application layer's
  `eqResponseDb(type, freq, gain, q, slope, sampleRate, freqs)`
  ([app/src/audio/EqResponse.h](../../app/src/audio/EqResponse.h)) wraps it for the
  editor's curves, so the curve drawn is the sound.

### Sidechain (`builtin:sidechain`, AudioEffect, with a sidechain input)

Ducks its input along a curve from each hit: for a bass under a kick.

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `trigger` | Trigger | | Sidechain, Every Bar, Every 1/2, Every 1/4, Every 1/8, Every 1/16 | Sidechain |
| `threshold` | Threshold | dB | -60..0 | -24 |
| `depth` | Depth | % | 0..100 | 100 |
| `sync` | Sync | | Off, On | Off |
| `length` | Length | ms | 10..2000, log | 250 |
| `rate` | Length (Synced) | | 1/32, 1/16, 1/8, 3/16, 1/4, 3/8, 1/2, 1 Bar | 1/4 |
| `smooth` | Smooth | ms | 0..30 | 1 |
| `lookahead` | Lookahead | ms | 0..20 (not automatable: it is the latency) | 0 |
| `range` | Range | | Full, Lows | Full |
| `crossover` | Crossover | Hz | 30..1000, log | 150 |
| `autofit` | Auto Fit | | Off, On (hidden, not automatable; the editor's) | Off |
| `character` | Fit Character | | Tight, Natural, Loose (hidden, not automatable; the editor's) | Natural |
| `p1_used` .. `p16_used` | Point n Used | | Off, On (hidden, not automatable, as are the three below) | On for 1..3 |
| `pn_x` | Point n Time | | 0..1 of the length | 0, 0.1, 1 |
| `pn_y` | Point n Level | | 0 (ducked by the depth)..1 (untouched) | 0, 0, 1 |
| `pn_curve` | Point n Curve | | -1..1: how the segment after it bends | 0, -0.45, 0 |

- **Hits**: with `trigger` Sidechain and a sidechain chosen, the first sample whose
  key (the louder channel) is at the threshold, once re-armed: the key's level (falling
  over 10 ms) has dropped 3 dB below the threshold, and 20 ms have passed. Without a
  sidechain, none (a ducker keyed by its own input would only duck its own notes). On
  the beat: at every multiple of the interval (a bar from the time signature) while
  playing, sample-accurately; a beat that rounds to both sides of a stretch's start
  hits once.
- **The curve**: the used points sorted by x; at `t` samples after a hit, x = t /
  length (in ms, or `rate` at the tempo with `sync`), evaluated as automation is
  (`automationShape`, a point's curve bending the segment after it); before the first
  point its y, after the last (and until the next hit, and before any) the last's.
  The gain is `1 - depth (1 - y)`, smoothed by a one-pole of `smooth` ms (none at 0).
- **Lookahead** delays the input (a delay line for 20 ms at most); `latencySamples()`
  reports it and `idle()` asks for the tracks to be realigned when it changes. The key
  isn't delayed, so the curve starts that much before the kick.
- **Range** Lows: the delayed input is split by a Linkwitz-Riley crossover (two
  Butterworth sections per band, in double), and only the low band is ducked
  (`high + gain * low`); the bands add up to the input's magnitude, flat.
- **Displays**: `key` (the sidechain summed to mono), `input` (the input before the
  lookahead, summed to mono) and `phase` (samples since the latest hit; -1 once its curve
  is over), one value per sample each, pushed together so they stay in step. The
  editor finds the hits (the phase's 0s) and fits the curve to the kick
  ([app/src/analysis/SidechainFit.h](../../app/src/analysis/SidechainFit.h)).

### Racks

A rack is a `RackProcessor` ([Rack.h](../../engine/src/Rack.h), `typeId()` `"rack"`): only its place in a chain, its
on/off switch, its id and its latency (as the last snapshot worked it out, for the UI). It has no parameters, and its
`process()` is never called: the renderer runs its chains (`Renderer::processRack`). See [routing.md](routing.md).

## Built-in devices' own editors

A built-in device shows a knob per parameter (the device view's knob pages: a list for parameters with labels, log
knobs for log parameters) unless it has an editor of its own: a QML file in
[ui/qml/devices/editors/](../../ui/qml/devices/editors), named for the device's kind (its engine id) in the
`DeviceEditors` singleton's table ([DeviceEditors.qml](../../ui/qml/devices/editors/DeviceEditors.qml)):

```js
readonly property var editors: ({
    "amp": "AmpEditor.qml",
    "chorus": "ChorusEditor.qml",
    "compressor": "CompressorEditor.qml",
    "delay": "DelayEditor.qml",
    "disperser": "DisperserEditor.qml",
    "eq": "EqEditor.qml",
    "erosion": "ErosionEditor.qml",
    "gate": "GateEditor.qml",
    "limiter": "LimiterEditor.qml",
    "multiband": "MultibandEditor.qml",
    "phaser": "PhaserEditor.qml",
    "reverb": "ReverbEditor.qml",
    "sampler": "SamplerEditor.qml",
    "saturator": "SaturatorEditor.qml",
    "sidechain": "SidechainEditor.qml",
    "spectral": "SpectralEditor.qml"
})
```

- `DeviceEditors.editorFor(kind)` gives the editor's URL, or `""` for the knob pages.
- An editor sets parameters as the knobs do (through the application layer's `ProjectEditor::setDeviceParams()`), so
  undo, automation and saving work alike.
- It draws with [DeviceCanvas](../../ui/src/devices/DeviceCanvas.h) items: `refreshDisplays()` is called as the meters
  update (while the item is visible), and `readDisplay(id)` returns the values the device published since the last
  call (through `EngineBridge::readProcessorDisplay()`; the item keeps the position per processor and display).
- Today there are sixteen. The Compressor draws its gain reduction over the last 240 display values (about 1.3 s at
  48 kHz) and In/Out meters with the threshold marked; the Gate the last 2.5 s of `input`, `output`, `key` and `open`
  with its threshold and Return lines to drag, and in its sidechain section the key EQ's curve (from `gate::keyFilter`,
  the engine's own biquad); the Limiter the level over the last 1.5 s with the gain reduction hanging from the top,
  In, GR and Out meters, and the Ceiling (or with Maximize the Threshold) as a line to drag, Soft Clip's band under
  it (from `limiter::scales` and its knee); Multiband Dynamics a lane per band, its Above and Below regions with their
  thresholds and ratios to drag, and each band's level before and after its dynamics (`<band>_in`, `_out`, `_gain`;
  the static curve from `multiband::staticGainDb`); the Spectral Compressor the spectra of `input` and `output` with
  the threshold and Below lines (from `spectral::thresholdDb` and `belowDb`) and the Focus band to drag, the cut or
  lift at each frequency (`gain`) and where the levels compared (`key`) are over the threshold; the Saturator its
  curve (from `saturator::transfer`) lit by `in_peak`, and Color's EQ (from `saturator::colorResponseDb`) over spectra
  of `input` and `output`; the Amp its transfer and tone curves (from `amp::Transfer` and `amp::toneResponseDb`),
  its tubes glowing as hard as each stage is driven (`drive1`..`drive3`, `power`, `sag`), a pilot lamp and an output
  meter; Erosion its noise band (from `erosion::bandMagnitude`) on an X-Y field of Frequency and Amount over spectra
  of `input` and `output`, shimmering with `erosion`, and a scope of `mod_l` against `mod_r`; the Delay its filter
  over a spectrum of `input`; the Chorus-Ensemble each voice's delay as it moves (from `ChorusDesign.h`'s maths at
  the LFO's `phase`, glowing with `level`), dragged for the Rate and the Amount; the Phaser-Flanger its response (from
  `phaser::curve`) at the sweep the engine publishes (`sweep_l`, `sweep_r`, `q_l`, `q_r`), its notches marked, and
  the LFO's shape and phase; the Reverb its input filter's band over a spectrum of `signal`, its early reflections as
  particles Spin swings (`early`, `spin`), and its decay time per frequency (from `reverb::decaySeconds`) over the
  tail's spectrum (`tail`, `diffuse`), each with handles to drag; the Disperser its group delay in ms on a fixed log
  axis (from `disperser::groupDelayMs`, the engine's own stages); the EQ its bands' curves (from `eq::responseDb`)
  over an analyzer of `input` and `output`; the Sidechain its curve, its
  playhead from `phase` and its fit to the kick (`key`, against `input`); the Sampler the sample's waveform as it plays
  with its markers, its loop, fades or slices (the engine's own, through the application layer's `sampleSlices`) and
  the playhead from `position` (drop or double-click to load a sample; loading is an undoable state change through
  `ProjectEditor::setDeviceState()`, the path kept in the device's state).

See [ui/device-view.md](../ui/device-view.md#device-editors) for the editors and how to add one.

## Adding a built-in device, end to end

1. **Write the device**: `engine/src/builtin/devices/MyDevice.cpp`.

   ```cpp
   #include "builtin/BuiltinProcessor.h"
   #include "builtin/BuiltinRegistry.h"
   #include "rt/RtUtils.h"

   namespace sub {
   namespace {

   class MyProcessor final : public BuiltinProcessor {
   public:
       enum Param { Amount = 0, NumParams };
       MyProcessor() : BuiltinProcessor(infos()) {}   // + displays, if its editor draws any
       std::string typeId() const override { return "builtin:mydevice"; }
       std::string name() const override { return "My Device"; }
       void prepare(double sampleRate, int maxBlockSize) override { /* allocate, set up smoothing */ }
       void reset() override { /* clear filter state, voices */ }

   protected:
       void render(const ProcessContext& ctx, float* const* ch, int numChannels, int numFrames) override {
           const float amount = param(Amount);   // plain value, already automated for this stretch
           // process in place; no locks, no allocation
       }

   private:
       static const std::vector<ParamInfo>& infos() {
           static const std::vector<ParamInfo> kInfos = {
               {"amount", "Amount", "%", 0.f, 100.f, 50.f},
           };
           return kInfos;
       }
   };

   }  // namespace
   SUB_REGISTER_BUILTIN(MyProcessor, AudioEffect);
   }  // namespace sub
   ```

   - Parameter ids are saved in projects and automation keys (`device:<device id>:<param id>`): don't rename them
     once released. Use `logScale` for frequencies and times, `valueLabels` for lists, `steps` for whole numbers.
   - What other devices already do is in [builtin/Dsp.h](../../engine/src/builtin/Dsp.h) (filter sections,
     interpolation, envelopes, rendering between note events),
     [builtin/DspBlocks.h](../../engine/src/builtin/DspBlocks.h) (one-pole and DC filters, a delay line, an envelope
     follower, noise, a fast tanh, LFOs and synced rates, biquads, a crossover, a sliding maximum, oversampling) and
     [rt/RtUtils.h](../../engine/src/rt/RtUtils.h) (smoothing, dB, one-pole coefficients): use them rather than
     writing them again. They are inline, so they cost what the same code written out would.
   - An instrument writes its output (it is first on a MIDI track) and reads notes from `ctx.inEvents`; an effect
     processes in place.
   - A sidechain: override `hasSidechain()` and read `sidechain(c)` / `sidechainConnected()` in `render()`.
   - Latency: override `latencySamples()`; tails: `tailSamples()`.
   - Displays: pass `{{"id", samplesPerValue}, ...}` to the constructor and `publish()` from `render()`.
   - State besides parameters: override `stateValues()` / `setStateValues()`; load files with `loadSource()`; hand
     anything big to the rendering thread without locks (see the Sampler).
2. **Build it**: `ninja -C build` ([building.md](../building.md)). The `CONFIGURE_DEPENDS` glob in
   [engine/CMakeLists.txt](../../engine/CMakeLists.txt) picks up a new file in `builtin/devices/` (and writes its
   anchor) on the next build; nothing needs listing.
3. **It appears**: the registry lists it, so `BuiltinRegistry::instance().devices()`, the application layer's
   `builtinDevices()` and the browser's *Built-in* category have it, with its parameters' defaults. Instruments go on
   MIDI tracks. The device view shows its knobs; automation and saving work.
4. **An editor of its own (optional)**: add `ui/qml/devices/editors/MyDeviceEditor.qml` and list it under
   `"mydevice"` (the id without `builtin:`) in [DeviceEditors.qml](../../ui/qml/devices/editors/DeviceEditors.qml); draw
   displays in a `DeviceCanvas` item's `refreshDisplays()`. See [ui/device-view.md](../ui/device-view.md#adding-an-editor).
5. **Tests**: render it offline (as [tests/engine/test_engine_render.cpp](../../tests/engine/test_engine_render.cpp)
   does for Utility and Over The Top, or a file of its own like
   [tests/engine/test_compressor_engine.cpp](../../tests/engine/test_compressor_engine.cpp); a block added to
   DspBlocks.h gets a case in [tests/engine/test_dsp_blocks.cpp](../../tests/engine/test_dsp_blocks.cpp)), and if it
   has an editor, give the editor a test file of its own, `tests/app/test_ui_device_editors_mydevice.cpp` (as
   [test_ui_device_editors_gate.cpp](../../tests/app/test_ui_device_editors_gate.cpp) is), on the host in
   [tests/app/support/EditorHarness.h](../../tests/app/support/EditorHarness.h) (each `test_*.cpp` in tests/app is a
   program of its own; nothing needs listing), and add its kind to `registry()` in
   [tests/app/test_ui_device_editors.cpp](../../tests/app/test_ui_device_editors.cpp), which checks the registry. See
   [testing.md](../testing.md).
6. **Docs**: add it to this page and to [guide/devices.md](../guide/devices.md).

## Invariants and real-time rules

- `render()` never locks, allocates or frees. Buffers are sized in `prepare()` (the Synth's per-sample filter
  coefficients and mix are `maxBlockSize` long; a block is never longer than `prepare()` said).
- Parameter values are atomics read with relaxed order; a stretch reads a consistent value per parameter because
  automation is applied between stretches on the same thread.
- `ParamInfo` lists are static and never change for a built-in device (unlike a plug-in's).
- `setStateValues()` may run on a pool thread at the same time as `render()` on the audio thread; anything it shares
  with `render()` goes through lock-free handoffs.

## Gotchas

- The id in `typeId()` must start with `builtin:`; the registry strips it to get the id the UI uses.
- `BuiltinRegistry::devices()` instantiates every device once to describe it: a constructor must be cheap and
  side-effect free.
- `setParam()` clamps to the range; automation goes through `fromNormalized()`, so discrete parameters land on steps.
- Devices are shared between live playback and offline renders; the engine resets them before and after an offline
  render (a flag the rendering thread acts on, plus `resetOffline()`).
- A device that is switched off is reset when it comes back on.

## Tests

The engine's tests are in [tests/engine](../../tests/engine) (one executable, `engine_tests`).

- [test_engine_render.cpp](../../tests/engine/test_engine_render.cpp): the Utility device, Over The Top, chains per
  strip, moving processors between chains.
- [test_dsp_blocks.cpp](../../tests/engine/test_dsp_blocks.cpp): the shared blocks of DspBlocks.h, each measured
  against what it is meant to do: a one-pole smoothing and the DC blocker removing DC, the delay line's reads (between
  samples too), the envelope follower's times, noise uniform and repeatable and `fastTanh` against tanh, the LFO's
  shapes and cycles, biquads responding as their magnitude says, the crossover's bands adding up flat (as its
  all-pass does), the sliding maximum over a changing window, oversampling transparent in band and delayed by its
  latency, oversampled saturation folding back far less, and a low, narrow biquad ringing out to exact zeros.
- [test_compressor_engine.cpp](../../tests/engine/test_compressor_engine.cpp): its curve (hard knee follows the ratio;
  nothing below the knee), attack holding on low notes, sidechain keying, and its displays.
- [test_gate_engine.cpp](../../tests/engine/test_gate_engine.cpp): its listing; opening at the threshold and closing
  below Return, after Hold, over Attack and Release, to the sample (and opening again from where a release had got
  to; a Hold changed while it counts applying at once); Floor (silence, no effect, in between) and Flip; lookahead as
  latency (opening early, the hold counted from the output, a new window holding what came before it, `idle()`, the
  engine lining the output up with the timeline); keying from a sidechain, its gain and mix; the key EQ (each type
  plays as `gate::keyFilterDb` draws it; a new type, or the EQ switched on, heard once its filter has caught up with
  the key, to the frame whatever the blocks, then fading between two settled outputs, with nothing to settle);
  listening; every control moving without a jump (the largest steps) and click-free (a 6th-difference measure against
  the same change made at once); automation to the sample whatever the block size, and through the engine; reset and
  a new rate; the extremes at 8 to 192 kHz; NaN, infinity and absurd levels in the input or on the sidechain (the
  output what a zero there gives, to the bit, with the EQ on or off, listening or not); stability, and silence
  ringing out to exact zeros (bounded by the key EQ's own poles); one channel (and back to two without the right's
  old delay); the displays.
- [test_limiter_engine.cpp](../../tests/engine/test_limiter_engine.cpp): its listing, latency and tail; below the
  ceiling the input, the lookahead late; no sample over the ceiling for every mode, routing and link and other settings
  (sines, noise, lone spikes, a square, Nyquist); a lone peak caught exactly, the gain falling only over the S - 1
  samples before it; the manual release's time constant; Auto's release, quick after a short peak, slow after sustained
  limiting, and a sustained bass note's distortion no worse than a manual 300 ms's; True Peak against a 32x reference
  meter of the test's own (a quarter of the rate at 45°, band-limited noise, short bursts up to 18 kHz at 12 sub-sample
  offsets) and a 4x meter from the device's own phases; the interpolator's flatness and the parabola; Soft Clip's peak,
  harmonics, reduction and curve sample by sample; Maximize (at its defaults changing nothing); L/R, M/S and Link (M/S
  at Link 100 % exactly L/R linked, in Standard and True Peak); every control changing without a click (a 6th-difference
  measure, against an unfaded 6 dB step; each change must change the output, the release's on a falling tone; Auto's
  20 ms blend) and the lookahead's dip; glides smooth in dB, and a moving ceiling leaving quiet material untouched;
  automation through the engine; reset and new rates; the extremes, and input that isn't audio (NaN, ±inf, beyond
  1e30, in either routing) leaving no trace; silence ringing out to exact zeros; one channel; latency compensated
  through the engine; its seven displays. The device runs alone on `harness/Standalone.h`.
- [test_multiband_engine.cpp](../../tests/engine/test_multiband_engine.cpp): its listing; flat doing nothing (any
  crossovers, Amount 0 bit for bit); each side's law (Above and Below, compressing and expanding, the +30 and −96 dB
  limits, the upward fade, both at once), the knee, Amount; attack, release and Time as Live defines them (timed on
  the displays); Peak and RMS and switching between them; Input and the Outputs; each band, switched off (into the mid
  band), bypassed by its activator (its split kept, its displays showing the level as it comes) and soloed; Listen
  (the input unkeyed; keyed, the key after S/C Gain and S/C Mix's blend); a sidechain keying each band by its own
  band (Gain, Mix, silence, one channel, connected again from silence, the displays reading the trigger); every
  control changing without a click (a 6th-difference measure, against a splice); automation through the engine, to
  the sample; reset and new rates; silence ringing out to exact zeros; NaNs and infinities in the input and in the
  key (Listen too), and the loudest input let in (1e30); the extremes at 44.1 and 192 kHz; one channel and linked
  stereo (RMS reading the louder channel's power on sides that differ); the tail; the displays; its cost (in the
  thread's CPU time); and an output that doesn't depend on how blocks are cut. Its device runs on
  [harness/Standalone.h](../../tests/engine/harness/Standalone.h), with a sidechain fed a block at a time.
- [test_spectral_engine.cpp](../../tests/engine/test_spectral_engine.cpp): its listing, the defaults gentle on pink
  noise at a mix's level, and the shared maths (frame sizes, calibration, the knee, the gain computer's curve rising
  everywhere, Below held at the threshold, the Focus's weights); its latency, the frame and a hop, at every rate and an
  impulse coming out that late; transparent under the threshold and with Range 0, and Dry/Wet 0 % the input delayed bit
  for bit; pink noise reading its own level at 44.1, 48 and 96 kHz; downward compression, Range, upward compression
  under Below (held at the threshold, nothing under the upward floor), Tilt following a spectrum both ways, Smoothing
  spreading a tone's gain, the Focus band and its edges, Delta as the difference, all with numbers; attack and release
  in time against the magnitude envelopes' formulas (which a follower of power would miss by a factor of two to four);
  Stereo Link (linked, each its own, halfway in dB); keying by a sidechain (alone, mono, silent, not connected, and
  through an engine); every control changing without a click (a 6th-difference measure, the ramped ones against a
  spliced switch); automation to the sample, alone and through the engine, in step with the audio; reset; silence
  ringing out to exact zeros; stability at the extremes; NaN and infinity in the input and the key playing exactly as
  zeros there, keyed or not, and absurd levels (+600 dBFS) leaving everything finite and the sound as it was 2 s on, and
  +360 dBFS reading at most +300 dB in every display; one channel the left of two; the displays (counts, whole frames
  for a reader that fell behind, published three hops late to the hop, the meters, the output display with Delta); the
  engine lining other tracks up with it; and bounds on its cost in the thread's CPU time, in all and per audio callback
  (no callback carries a whole frame).
- [test_saturator_engine.cpp](../../tests/engine/test_saturator_engine.cpp): its listing; quiet audio untouched at
  the defaults, bit for bit; each curve played as its editor draws it (eight types, four drives, the Bass Shaper's
  thresholds, the Waveshaper's settings) and the curves' numbers; the Waveshaper's controls (no effect at WS Drive 0,
  Lin's slope, Curve's third harmonic, Damp's gate, Depth's and Period's ripples); odd harmonics only; Post Clip under
  the Output level at any Dry/Wet (a hot input's dry share too); Output and Dry/Wet; Color leaving a clean sound
  alone (also while every control glides, with and without Hi-Quality, and with Hi-Quality switched off mid-glide)
  and moving the saturation, its design's exact inverse; DC; Hi-Quality's latency (and `idle()`), its aliasing (the
  curves' and Post Clip's), the tracks lined up through the engine, and its pre-roll (bit for bit as if on all
  along); every control changing without a click (a 6th-difference measure, against the settings either side and
  those a glide crosses, and an unfaded splice; Hi-Quality with Color and with Post Clip too); automation through the
  engine, to the sample; reset and a new rate; silence ringing out to exact zeros without flush-to-zero (slow Color
  sections at 1x and 4x too); the tail; extremes at 44.1 to 192 kHz (Hard Clip's ceiling at any Dry/Wet, within the
  4x filters' worst gain with Hi-Quality); NaN and infinity in the input coming out as zeros there would (DC and
  Color running, at 4x, DC switched on after); one channel, and channels independent; its displays (also in blocks
  that split the meters' 128 samples).
- [test_amp_engine.cpp](../../tests/engine/test_amp_engine.cpp): its listing; the design (each stack's make-up and its
  digital response against the analog one, the curve, its anti-aliasing exact at rest and when clipped, the morph's
  ends, the transfer's parts); each model's character and level-matched defaults; Gain, Volume (the power stage it
  drives, and the distortion it adds on Blues and Heavy) and the sag with its recovery; NaN, infinity and absurd
  levels in its input playing as zeros would, bit for bit (Mono, Dual and dry), and a +600 dBFS sample blocking it for
  under a second; the tone curve as played; the transfer as played (its slope, and a 1 kHz tone's peaks at −26 and −12
  dBFS on every model: within 0.25 dB, 1 dB with Gain, Presence and Volume at 10); the tone controls driving V3; Mono
  and Dual; the Output switch and every model morph click-free (a 6th-difference measure against an unfaded splice, at
  −12 dBFS and at −80, where the tone is pure) and the morphs level-matched; Rock's and Lead's dials click-free at −12
  dBFS and every model's every dial at −80; automation through the engine to the sample (against an amp alone: a
  sample off fails); Dry/Wet with the dry delayed; latency (a small tone's phase on every model, and the engine's
  compensation exact); the tail and silence ringing out to exact zeros (the slowest stack too); reset and a new rate;
  extremes at 22 to 192 kHz and +12 dBFS; no DC; aliasing (Gain 10, noon, and Gain, Presence and Volume at 10); the
  displays; its cost (the thread's CPU time, Release only; and a model change's first block against a steady one's); a
  model change's levels a cell at a time (the old model bit for bit until the morph sets off, at the same sample in
  any blocks); model changes faster than a morph, and a change in a morph, level-matched while a dial moves; blocks of
  1 to 1024 frames and slices bit for bit (soft onsets, silence and sleep in Dual too); sleep (its cost, Release only)
  and waking as a fresh amp.
- [test_erosion_engine.cpp](../../tests/engine/test_erosion_engine.cpp): its listing and 2 ms latency at any rate;
  Amount 0 a clean delay of its latency, bit for bit, and transparent through the engine (its modulators running on
  as at any Amount); a sine's sidebands as Bessel functions of the modulation index (within 0.1 %), Frequency moving
  them, Width doing nothing to the sine, Stereo putting the sides a quarter cycle apart; the noise's RMS at any
  Frequency and Width (and its closed-form power gain against the impulse response's), and while the band moves a
  long way; two devices set alike modulating apart; Stereo decorrelating it, highs eroded before lows, Width
  spreading the sidebands; the same output however the block is split; every control changing without a click (a
  6th-difference measure, against a spliced switch) and landing where it was turned; automation through the engine,
  to the chunk; reset and a new rate; stability at the extremes and under random automation; silence as exact
  zeros; NaN and infinity in the input as silence (bit for bit as a 0 there); one channel the left of two (and two
  again); the displays; the band the editor draws against the filter's impulse response.
- [test_delay_engine.cpp](../../tests/engine/test_delay_engine.cpp): synced and free times, offset, link,
  feedback, ping pong, freeze, the filter, the modes (with automation changing the time), and its display.
- [test_chorus_engine.cpp](../../tests/engine/test_chorus_engine.cpp): its listing and the design's figures (the
  warmth's curve, its antiderivative and its stage among them); passing the input bit for bit fully dry (with the
  high-pass on, the crossover's all-pass); each layout a plain delay of its centre at Amount 0 (the impulses after
  the lines' fade-in, as the tests' sounds that must reach the delays whole);
  the delays, sample by sample on a ramp, in every mode (Auto's centre following Amount, two taps and Ensemble's three
  averaging out, Vibrato's Offset and triangle, the fixed Times, fractional delays at 44.1 kHz, and at 96 kHz); Ensemble
  beating; the detune; feedback's echoes, Invert, no feedback in Vibrato (Feedback and Invert changing nothing
  there), the limiter holding a tone on the loop's
  resonance and DC not building up; Warmth's gentle distortion and level at -6 and 0 dBFS, its low-pass and its
  aliasing at 5, 9 and 13 kHz; the high-pass split (to 1e-5), the lows unchorused and, half wet, the lows below it
  whole; Width (ignored in Vibrato); one channel as the left of two, a third untouched; Output and Dry/Wet; every
  control changing without a click, both ways for the continuous ones (a 6th-difference measure, against an unfaded
  switch); switched on in the middle of a 440 Hz tone (reset there, its output faded in as the renderer does) with
  no step larger than the tone's or the device's always on, in every mode and at the longest delays; NaN, infinity
  and 3e38 in the input as silence, bit for bit; automation through the engine, to the sample; any block size;
  reset, repeatability and a
  new rate; stability at the extremes at 44.1 to 192 kHz, under random automation of every control and with Mode
  turning round every 64 samples; silence ringing out to exact zeros; the tail; the displays (the level after
  Output).
- [test_phaser_engine.cpp](../../tests/engine/test_phaser_engine.cpp): its listing; the notches where the design puts
  them (in closed form, against the stages' own phase and the measured response), Spread moving them apart, the LFO
  sweeping them, stereo Phase and Spin; the flanger's comb and its feedback (and Ø) to the sample, the doubler's copy;
  the delays swept; the envelope follower, Safe Bass, Warmth, Output and Dry/Wet (bit for bit fully dry); a change of
  mode or of Notches landing exactly on the new setting; every control changing without a click, each where it is
  heard, and a fast sweep into the stages' limits (a 6th-difference measure, against unfaded switches); every LFO
  shape at Duty 0 and ±80 % as its design draws it, LFO 2 alone, synced and half mixed in, Triangle Analog's level
  following its rate; Warmth's aliases far down; automation through the engine, to the sample; synced LFOs following
  the song (and Spin automated deep into one), random shapes repeating, an LFO's jumps crossfading; reset and a new
  rate; silence ringing out to exact zeros (no denormal output on the way, gains gliding to 0 too); the tail;
  stability at the extremes and at 22.05 to 192 kHz; one channel playing as either of two; the displays; its cost (in
  the thread's CPU time, so a busy machine doesn't fail it); the editor's curve (`phaser::curve`: notches drawn at
  their depth, a dense comb as a steady band).
- [test_reverb_engine.cpp](../../tests/engine/test_reverb_engine.cpp): its listing; exact silence for silence; fully
  dry, the input untouched bit for bit; the decay per band (125 Hz, 1 kHz, 8 kHz; three Decays, shelf and low-pass;
  every Density; both shelves off) against `reverb::decaySeconds`, by Schroeder integration in steep bands; the shelves
  damping their bands; the reflections placed, signed and weighted as `earlyTaps` says at four predelays and sizes, the
  first at the predelay; Shape moving the diffuse onset and the reflections' envelope; the input filter as
  `inputFilterDb` draws it; mono in; Stereo from mono to two independent sides; one channel; Reflect, Diffuse and the
  overall level; Freeze holding (where the same unfrozen dies away), Cut keeping new sound out (and without Cut letting
  it in), the release decaying at Decay's pace; Flat; the guard (left alone by full-scale noise, holding a +12 dBFS
  input); each Density's decay and echo density, a change of it keeping a frozen tail, and what joins starting from
  silence (after holding a loud tail); Spin swinging and drifting each reflection as `spinPan` and `spinDriftMs` say
  (Doppler included), and reaching the tail; Chorus spreading a tone, Diffusion blurring the echoes sooner, Scale
  setting the diffusers' lengths; no metallic ringing; every control and switch changing without a click (a
  6th-difference measure, against the steady render and an unfaded gap; Smooth switched mid-glide too); automation
  through the engine, to the sample; reset and a new rate (44.1 to 192 kHz; Size's glide at the new rate); extremes;
  silence ringing out to exact zeros, denormal input, waking as a fresh device (also with Predelay, Size and Shape
  raised while it slept, and woken just after it fell asleep, the delays jumping as it wakes at each Smooth); a NaN, an
  infinity or 1e31 in its input playing exactly as a 0 there would (at the defaults, fully dry, and frozen with the
  input feeding the tail), and the loudest input it takes staying finite; its tail; its displays (levels, the signal
  sample for sample, the tail's level falling at Decay's pace, the LFOs' phases however blocks fall, and when switched
  while it sleeps); the design's helpers for the editor; what it costs (in the thread's CPU time).
- [test_disperser_engine.cpp](../../tests/engine/test_disperser_engine.cpp): its listing; passing through
  untouched (no stages, bypassed, fully dry); Dry/Wet's blend, to the sample, and its notches; a flat magnitude (every bin within 0.01 dB, all its energy) and the
  design's group delay, at the extremes (20 s at 20 Hz), at 8 to 192 kHz and kept below Nyquist;
  stability; never more energy out than in, however Frequency and Pinch jump; each channel its own;
  Amount landing exactly on the new number of stages; every control changing without a click (a
  6th-difference measure, against an unfaded switch); automation through the engine, to the sample;
  reset and a new rate; silence ringing out to exact zeros; the tail; no latency (the other tracks not
  delayed, the dispersion where it should be).
- [test_eq_engine.cpp](../../tests/engine/test_eq_engine.cpp): each band type plays as `eq::responseDb` draws it,
  matching the analog filters, placement (left, mid, side), output gain and gain scale, extremes, the displays.
- [test_sidechain_device_engine.cpp](../../tests/engine/test_sidechain_device_engine.cpp): hits to the sample, the
  curve sample by sample (straight and bent), depth and smoothing, the threshold and re-arming, lookahead (as
  latency), Lows Only keeping the highs, hits on the beat, the synced length, the displays, extremes.
- [test_sampler_engine.cpp](../../tests/engine/test_sampler_engine.cpp): listing and parameters (old projects' defaults),
  pitch from key, root and tuning at any file rate, start, end and loop, velocity, its state's text (escaping), a
  missing file, unknown values, swapping samples while notes play, and the position display; then transients found
  where hits start and slices at each sensitivity, beats and regions, snapping; Slice (by region, beat and transient,
  Mono, Poly, Thru), 1-Shot (Trigger, Gate, fades, one note at a time), Classic's Loop Start and Loop Fade, reverse,
  snap, gain and pan, the filter's four types and slopes and resonance, the LFO (tremolo, vibrato, synced, restarted,
  pan, filter), voices and legato glide, warping (Re-Pitch, every stretching mode, keys, tempo, repeatable renders,
  resampled past 4 times as fast, also a note already playing), the display of a reversed note, a resonant filter ringing out after the last note,
  and a random LFO restarted by notes rendering the same every time.
- [test_midi_engine.cpp](../../tests/engine/test_midi_engine.cpp): the Synth plays the right pitch and level.
- [test_automation_engine.cpp](../../tests/engine/test_automation_engine.cpp): built-in blocks split where automation
  changes values.

In the application's tests ([tests/app](../../tests/app)):
[test_ui_device_editors.cpp](../../tests/app/test_ui_device_editors.cpp) checks the editor registry and drives the
first six editors (the Compressor's graph, the Sampler's loading, undo, playhead, markers, drop and saving, the Delay,
the Disperser, the EQ, the Sidechain) and what the editors share (`EditorKnob` and `DeviceParamMap`, `MeterBallistics`,
`Eased` and `dbToY`); each of the others has a file of its own, `test_ui_device_editors_<kind>.cpp`, on the same host
([support/EditorHarness.h](../../tests/app/support/EditorHarness.h)):
[the Gate's](../../tests/app/test_ui_device_editors_gate.cpp),
[the Limiter's](../../tests/app/test_ui_device_editors_limiter.cpp),
[Multiband Dynamics'](../../tests/app/test_ui_device_editors_multiband.cpp),
[the Spectral Compressor's](../../tests/app/test_ui_device_editors_spectral.cpp),
[the Saturator's](../../tests/app/test_ui_device_editors_saturator.cpp),
[the Amp's](../../tests/app/test_ui_device_editors_amp.cpp),
[Erosion's](../../tests/app/test_ui_device_editors_erosion.cpp),
[the Chorus-Ensemble's](../../tests/app/test_ui_device_editors_chorus.cpp),
[the Phaser-Flanger's](../../tests/app/test_ui_device_editors_phaser.cpp) and
[the Reverb's](../../tests/app/test_ui_device_editors_reverb.cpp); and
[test_sidechain_fit.cpp](../../tests/app/test_sidechain_fit.cpp) the Sidechain's fit.
