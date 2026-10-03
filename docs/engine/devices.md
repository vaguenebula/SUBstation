# Devices and the `Processor` interface

Every device on a track's chain (built-in instruments and effects, VST3 plug-ins, racks) is a `Processor`
([Processor.h](../../engine/src/Processor.h)), so the renderer never needs to know where one came from. Built-in
devices share a base class, `BuiltinProcessor`, and register themselves in `BuiltinRegistry`
([engine/src/builtin/](../../engine/src/builtin)); each one is a single `.cpp` in
[builtin/devices/](../../engine/src/builtin/devices).

How the user works with devices (the device view, racks, presets, folding, cut/copy/paste) is in
[guide/devices.md](../guide/devices.md). Plug-in hosting is in [plugins.md](plugins.md); racks and chains in
[routing.md](routing.md); the device view's widgets in [ui/device-view.md](../ui/device-view.md).

## Files

| File | What it holds |
|---|---|
| [Processor.h](../../engine/src/Processor.h) | `ProcessEvent`, `EventList`, `ProcessContext`, `ParamInfo`, `DisplayInfo`, `ParamAutomation`, `ProcessorEvent`, `Processor` |
| [builtin/BuiltinProcessor.h](../../engine/src/builtin/BuiltinProcessor.h) / [.cpp](../../engine/src/builtin/BuiltinProcessor.cpp) | parameters as atomics, automation by splitting blocks, displays, state as named text values, `loadSource()` |
| [builtin/BuiltinRegistry.h](../../engine/src/builtin/BuiltinRegistry.h) / [.cpp](../../engine/src/builtin/BuiltinRegistry.cpp) | `BuiltinRegistry`, `BuiltinInfo`, `BuiltinCategory`, `SUB_REGISTER_BUILTIN` |
| [builtin/devices/Synth.cpp](../../engine/src/builtin/devices/Synth.cpp) | the Synth instrument |
| [builtin/devices/Sampler.cpp](../../engine/src/builtin/devices/Sampler.cpp) | the Sampler instrument |
| [builtin/devices/Utility.cpp](../../engine/src/builtin/devices/Utility.cpp) | Utility: gain, pan, width |
| [builtin/devices/Ott.cpp](../../engine/src/builtin/devices/Ott.cpp) | Over The Top: multiband upward/downward compression |
| [builtin/devices/Compressor.cpp](../../engine/src/builtin/devices/Compressor.cpp) | Compressor, with sidechain and displays |
| [Rack.h](../../engine/src/Rack.h) | `RackProcessor`: a rack's place in a chain (its chains are run by the renderer) |
| [rt/RtUtils.h](../../engine/src/rt/RtUtils.h) | `SmoothedValue`, `SpscQueue`, `DisplayStream`, `dbToGain`, `balanceGains` |
| [EngineChains.cpp](../../engine/src/EngineChains.cpp) | `addBuiltinProcessor()`, the processor API the bindings expose |
| [bindings.cpp](../../engine/src/bindings.cpp) | `builtin_devices()`, `BuiltinDevice`, `ParamInfo`, `DisplayInfo`, `processor_displays`, `read_processor_display`, `processor_state` |
| [ui/device_editors/](../../src/substation/ui/device_editors) | built-in devices' own editors (Python), registered by kind |

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

The bindings: `Engine.processor_displays(id)` and `Engine.read_processor_display(id, index, position)` (a float32
array and the next position).

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
  all. The model keeps the state base64-encoded in `Device.state`, as it keeps a plug-in's
  ([model/device_state.py](../../src/substation/model/device_state.py) has the same encoding), so it saves, loads and
  undoes as a plug-in's does.
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
calls `prepare()` before the audio thread can see the processor, and inserts it). The UI's device list, names,
categories and parameter defaults all come from the registry through `builtin_devices()` in the bindings: the model's
`BUILTIN_DEVICES`, `BUILTIN_CATEGORIES` and `BUILTIN_INSTRUMENTS` ([model/editor.py](../../src/substation/model/editor.py))
and the browser's *Built-in* category are built from it. A model `Device` of a built-in device has the registry id
as its `kind`.

Because each device's translation unit only registers itself (nothing refers to it), the build compiles the device
files straight into the `_engine` module: [CMakeLists.txt](../../CMakeLists.txt) collects them with
`file(GLOB BUILTIN_DEVICE_SOURCES CONFIGURE_DEPENDS engine/src/builtin/devices/*.cpp)`. Put in a static library,
unreferenced objects would be dropped and the device would never register.

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

Plays one audio file across the keyboard, pitched from its root key (as Ableton's Simpler does in Classic mode). 32
voices; plays from Start to End of the sample, or loops between them while a note holds (and as it releases).

| id | Name | Unit | Range | Default |
|---|---|---|---|---|
| `root` | Root Key | note | 0..127, 127 steps | 60 |
| `tune` | Transpose | st | -48..48, 96 steps | 0 |
| `fine` | Detune | ct | -100..100 | 0 |
| `start` | Start | % | 0..100 | 0 |
| `end` | End | % | 0..100 | 100 |
| `loop` | Loop | | Off, On (a list) | Off |
| `attack` | Attack | ms | 0.1..5000, log | 1 |
| `decay` | Decay | ms | 1..10000, log | 1000 |
| `sustain` | Sustain | % | 0..100 | 100 |
| `release` | Release | ms | 1..10000, log | 50 |
| `velocity` | Velocity | % | 0..100 | 50 |
| `volume` | Volume | dB | -60..6 | 0 |

- **Pitch**: each voice steps through the sample at `(source rate / engine rate) * 2^((key - root + transpose) / 12)`,
  with `transpose = round(tune) + fine / 100`. So a file at any rate plays at its pitch. Samples are read with
  4-point, 3rd-order Hermite interpolation.
- **Velocity sensitivity**: gain = `1 - sensitivity * (1 - velocity / 127)`.
- **Envelope and voice stealing** as the Synth's. A voice ends at End unless looping (or if the loop is shorter than a
  frame). A note-on with End not after Start plays nothing.
- **Output**: voices mix into the output (both sides into one channel if that's all there is, at half gain); the
  volume applies after. It writes, not adds.
- **State**: `"sample"`: the file's path. `setStateValues()` stores the path, loads the file through `loadSource()`
  *without holding its mutex* (it may take a while), and publishes it, unless another state came meanwhile. If the
  file can't be loaded it publishes nothing (silent until it can be loaded) and rethrows, but keeps the path in its
  state, so the project keeps it. Unknown state values are ignored.
- **Handing the sample to the audio thread without a lock**: the main side puts a new `Sample` into `pending_` (an
  atomic pointer; a sample still pending and never taken is freed there). At the start of each `render()` the
  rendering thread takes it, pushes the one it replaces into `retired_` (an `SpscQueue` of 8), and silences its voices
  (they played the old one). If `retired_` is full it waits for the next block. The main side frees retired samples
  in `idle()` and whenever it publishes. Loading the same file again changes nothing, so notes go on.
- **Display**: `position`, one value per 256 samples: where the newest note plays in the sample (0..1 of its length),
  or -1 while none plays. The editor draws the playhead from it.

The bridge restores a built-in device's state on a thread pool (`_StateTask`), since it may load files, and waits for
all of them before rendering offline (`wait_for_device_states`). See [python/engine-bridge.md](../python/engine-bridge.md).

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

### Racks

A rack is a `RackProcessor` ([Rack.h](../../engine/src/Rack.h), `typeId()` `"rack"`): only its place in a chain, its
on/off switch, its id and its latency (as the last snapshot worked it out, for the UI). It has no parameters, and its
`process()` is never called: the renderer runs its chains (`Renderer::processRack`). See [routing.md](routing.md).

## Built-in devices' own editors (Python)

A built-in device shows a knob per parameter (`device_panel.DeviceWidget`; a list for parameters with labels, log
knobs for log parameters) unless a module in [ui/device_editors/](../../src/substation/ui/device_editors) gives it an
editor of its own: a `DeviceWidget` subclass registered for the device's kind (its engine id):

```python
from ..device_panel import DeviceWidget
from . import device_editor

@device_editor("compressor")
class CompressorWidget(DeviceWidget):
    def refresh_displays(self) -> None:
        self.graph.add(self.read_display("reduction"), self.read_display("input"), self.read_display("output"))
```

- `device_editor(kind)` registers the class; two editors for one kind raise `ValueError`.
- `editor_for(kind)` imports every module of the package the first time an editor is looked up (not at import: the
  editors import the device view, which imports the package), so a new module needs nothing else.
- An editor sets parameters as the knobs do (`ProjectEditor.set_device_param`), so undo, automation and saving work
  alike, and draws the device's displays in `refresh_displays()`, which the device view calls as the meters update.
  `DeviceWidget.read_display(display_id)` returns the values published since the last call (it keeps the position).
- Today there are two: [compressor.py](../../src/substation/ui/device_editors/compressor.py) (every knob at once, a
  gain-reduction history of 240 values, about 1.3 s at 48 kHz, and In/Out meters with the threshold marked) and
  [sampler.py](../../src/substation/ui/device_editors/sampler.py) (the waveform with Start/End markers and the playhead,
  drop or double-click to load a sample; loading is an undoable state change through `set_device_state`, with paths
  kept in `device_state`).

See [ui/device-view.md](../ui/device-view.md) for `DeviceWidget` itself.

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
2. **Build it**: re-run `python -m pip install --no-build-isolation -e .` ([building.md](../building.md)). The
   `CONFIGURE_DEPENDS` glob in [CMakeLists.txt](../../CMakeLists.txt) picks up a new file in `builtin/devices/` on the
   next configure; nothing needs listing. No `API_VERSION` bump is needed: the bindings don't change.
3. **It appears**: the registry lists it, so `ge.builtin_devices()`, the model's `BUILTIN_DEVICES` and the browser's
   *Built-in* category have it, with its parameters' defaults. Instruments go on MIDI tracks. The device view shows
   its knobs; automation and saving work.
4. **An editor of its own (optional)**: add `src/substation/ui/device_editors/mydevice.py` with a `DeviceWidget`
   subclass decorated `@device_editor("mydevice")` (the id without `builtin:`). Draw displays in
   `refresh_displays()`.
5. **Tests**: render it offline (as `tests/test_engine_render.py` does for Utility and Over The Top, or its own file
   like `tests/test_compressor_engine.py`), and if it has an editor, add to `tests/test_ui_device_editors.py`
   (`test_registry` checks the registry). See [testing.md](../testing.md).
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

- [tests/test_engine_render.py](../../tests/test_engine_render.py): the Utility device, Over The Top, chains per strip,
  moving processors between chains.
- [tests/test_compressor_engine.py](../../tests/test_compressor_engine.py): its curve (hard knee follows the ratio;
  nothing below the knee), attack holding on low notes, sidechain keying, and its displays.
- [tests/test_sampler_engine.py](../../tests/test_sampler_engine.py): listing and parameters, pitch from key, root and
  tuning at any file rate, start, end and loop, velocity, its state's text (escaping), a missing file, unknown
  values, swapping samples while notes play, and the position display.
- [tests/test_midi_engine.py](../../tests/test_midi_engine.py): the Synth plays the right pitch and level.
- [tests/test_automation_engine.py](../../tests/test_automation_engine.py): built-in blocks split where automation
  changes values.
- [tests/test_ui_device_editors.py](../../tests/test_ui_device_editors.py): the editor registry, the Compressor's graph,
  the Sampler's loading, undo, playhead, markers, drop and saving.
