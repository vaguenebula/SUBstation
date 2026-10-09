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
| [builtin/BuiltinProcessor.h](../../engine/src/builtin/BuiltinProcessor.h) / [.cpp](../../engine/src/builtin/BuiltinProcessor.cpp) | parameters as atomics, automation by splitting blocks, displays, state as named text values, `loadSource()` |
| [builtin/BuiltinRegistry.h](../../engine/src/builtin/BuiltinRegistry.h) / [.cpp](../../engine/src/builtin/BuiltinRegistry.cpp) | `BuiltinRegistry`, `BuiltinInfo`, `BuiltinCategory`, `SUB_REGISTER_BUILTIN` |
| [builtin/devices/Synth.cpp](../../engine/src/builtin/devices/Synth.cpp) | the Synth instrument |
| [builtin/devices/Sampler.cpp](../../engine/src/builtin/devices/Sampler.cpp) | the Sampler instrument |
| [builtin/SampleSlicing.h](../../engine/src/builtin/SampleSlicing.h) / [.cpp](../../engine/src/builtin/SampleSlicing.cpp) | The Sampler's slicing (`detectOnsets`, `sliceStarts`) and Snap (`nearestZeroCrossing`), shared with the application layer's `sampleSlices` ([app/src/audio/SampleSlices.h](../../app/src/audio/SampleSlices.h)) for the editor |
| [builtin/devices/Utility.cpp](../../engine/src/builtin/devices/Utility.cpp) | Utility: gain, pan, width |
| [builtin/devices/Ott.cpp](../../engine/src/builtin/devices/Ott.cpp) | Over The Top: multiband upward/downward compression |
| [builtin/devices/Compressor.cpp](../../engine/src/builtin/devices/Compressor.cpp) | Compressor, with sidechain and displays |
| [builtin/devices/Delay.cpp](../../engine/src/builtin/devices/Delay.cpp) | Delay: synced or free times per side, filter, modes, ping pong, freeze |
| [builtin/devices/Disperser.cpp](../../engine/src/builtin/devices/Disperser.cpp) | Disperser: up to 64 all-pass stages, glides and fades, bypass |
| [builtin/DisperserDesign.h](../../engine/src/builtin/DisperserDesign.h) | The Disperser's stages (`disperser::design`, `process`, `groupDelayMs`), shared with the application layer's `disperserGroupDelayMs()` for the editor's graph |
| [builtin/devices/Eq.cpp](../../engine/src/builtin/devices/Eq.cpp) | EQ: 24 bands, placement, output gain, gain scale |
| [builtin/EqDesign.h](../../engine/src/builtin/EqDesign.h) | The EQ's filter design (`eq::design`, `eq::responseDb`), shared with the application layer's `eqResponseDb()` for the editor's curves |
| [builtin/devices/Sidechain.cpp](../../engine/src/builtin/devices/Sidechain.cpp) | Sidechain: a curve from each hit in the key (or on the beat), lookahead, lows only |
| [Rack.h](../../engine/src/Rack.h) | `RackProcessor`: a rack's place in a chain (its chains are run by the renderer) |
| [rt/RtUtils.h](../../engine/src/rt/RtUtils.h) | `SmoothedValue`, `SpscQueue`, `DisplayStream`, `dbToGain`, `balanceGains` |
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
  velocity 0 counts as a note-off. Plug-in adapters translate them to their format's events.
- `ProcessContext`: sample rate, `samplePos` and `beatPos` of the block's first frame, tempo, time signature,
  `playing`, `looping` with loop start and end beats, `offline`, and `inEvents`. The renderer splits blocks where the
  playhead jumps (a loop wrap), so a block is always one continuous stretch.

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
  the rendering thread reads them with `param(index)`. `params()`, `getParam()`, `setParam()`, `process()`,
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

### Disperser (`builtin:disperser`, AudioEffect)

Phase dispersion after Kilohearts' Disperser: identical second-order all-passes in a row
([DisperserDesign.h](../../engine/src/builtin/DisperserDesign.h)). Every frequency keeps its
level; what is around the stages' frequency comes out later (a transient becomes a chirp).

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `amount` | Amount | stages | 0..64, 64 steps | 16 |
| `freq` | Frequency | Hz | 20..20000, log | 1000 |
| `pinch` | Pinch | Q | 0.1..10, log | 1 |
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
- **Bypass** glides between the input and the stages' output (two one-poles of 5 ms: it can turn
  back halfway without a kink) and, once there, passes the input bit for bit. The stages keep
  running while bypassed, so it comes back without a seam; switching the device off saves them.
- **Denormals**: states below 1e-20 are flushed to 0 after each stretch, so silence rings out to
  exact zeros. `reset()` clears every stage and snaps the glides and fades to the parameters;
  `prepare()` (a new sample rate) works out the glides and fades again and resets.
- `latencySamples()` is 0: the dispersion is the effect, and the engine doesn't delay the other
  tracks against it. `tailSamples()`: until what is left of the impulse response is 60 dB down,
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
    "compressor": "CompressorEditor.qml",
    "delay": "DelayEditor.qml",
    "disperser": "DisperserEditor.qml",
    "eq": "EqEditor.qml",
    "sampler": "SamplerEditor.qml",
    "sidechain": "SidechainEditor.qml"
})
```

- `DeviceEditors.editorFor(kind)` gives the editor's URL, or `""` for the knob pages.
- An editor sets parameters as the knobs do (through the application layer's `ProjectEditor::setDeviceParams()`), so
  undo, automation and saving work alike.
- It draws with [DeviceCanvas](../../ui/src/devices/DeviceCanvas.h) items: `refreshDisplays()` is called as the meters
  update (while the item is visible), and `readDisplay(id)` returns the values the device published since the last
  call (through `EngineBridge::readProcessorDisplay()`; the item keeps the position per processor and display).
- Today there are six. The Compressor draws its gain reduction over the last 240 display values (about 1.3 s at
  48 kHz) and In/Out meters with the threshold marked; the Delay its filter over a spectrum of `input`; the
  Disperser its group delay in ms (from `disperser::groupDelayMs`, the engine's own stages); the EQ its
  bands' curves (from `eq::responseDb`) over an analyzer of `input` and `output`; the Sidechain its curve, its
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
   [tests/engine/test_compressor_engine.cpp](../../tests/engine/test_compressor_engine.cpp)), and if it has an editor,
   add a case to [tests/app/test_ui_device_editors.cpp](../../tests/app/test_ui_device_editors.cpp) (`registry()`
   checks the registry). See [testing.md](../testing.md).
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
- [test_compressor_engine.cpp](../../tests/engine/test_compressor_engine.cpp): its curve (hard knee follows the ratio;
  nothing below the knee), attack holding on low notes, sidechain keying, and its displays.
- [test_delay_engine.cpp](../../tests/engine/test_delay_engine.cpp): synced and free times, offset, link,
  feedback, ping pong, freeze, the filter, the modes (with automation changing the time), and its display.
- [test_disperser_engine.cpp](../../tests/engine/test_disperser_engine.cpp): its listing; passing through
  untouched (no stages, bypassed); a flat magnitude (every bin within 0.01 dB, all its energy) and the
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
[test_ui_device_editors.cpp](../../tests/app/test_ui_device_editors.cpp) checks the editor registry and drives each
editor (the Compressor's graph, the Sampler's loading, undo, playhead, markers, drop and saving, the Delay, the
Disperser, the EQ, the Sidechain), and [test_sidechain_fit.cpp](../../tests/app/test_sidechain_fit.cpp) the Sidechain's fit.
