# Audio devices

The engine plays through one audio device at a time, of either driver type: WASAPI (through
miniaudio) or ASIO (Steinberg's driver model, compiled only when the ASIO SDK was found). On
platforms other than Windows the WASAPI backend opens miniaudio's default backend instead, as the
"System" driver ([below](#the-system-driver-elsewhere)). This
part lives in [AudioDevice.h](../../engine/src/AudioDevice.h) / [AudioDevice.cpp](../../engine/src/AudioDevice.cpp),
the two backends in [engine/src/backends/](../../engine/src/backends/), and the engine's side of it
(opening, resets, driver events, the audio callback) in [EngineDevice.cpp](../../engine/src/EngineDevice.cpp).

For the preferences the user sees, see [guide/audio-setup.md](../guide/audio-setup.md); for how the
application opens devices and polls their events (the engine bridge), [app/engine-bridge.md](../app/engine-bridge.md);
for building with or without the ASIO SDK, [building.md](../building.md).

## Overview

- Each driver type is an `AudioBackend`. `AudioDevice` owns one of each and opens a `DeviceConfig`
  (driver type, device, rate, buffer size, the device's input and output channels) with whichever
  one it names. The engine only talks to `AudioDevice`.
- Devices run duplex: each callback gets the open inputs and fills the open outputs, as separate
  (planar) float buffers of one length, with the device's sample position and the steady-clock time
  the callback began (`AudioIO`). Recording and input monitoring use the inputs; MIDI input is
  timestamped against the same clock ([midi.md](midi.md)). WASAPI opens outputs only so far.
- A driver's events (the device went away, it was rerouted, a reset request, new latencies) are flags
  the audio side sets; the UI takes them one at a time from `Engine::takeDeviceEvent()` and answers a
  reset with `Engine::reopenDevice()`, which asks the driver for the buffer size and sample rate it
  now has before closing it.
- ASIO drivers are COM objects created on the UI thread, which has to be a single-threaded COM
  apartment for that. The audio thread calls nothing on the driver but `outputReady()`.

```
 main thread (the engine bridge)                       backend's real-time thread
 ------------------------------------                  --------------------------
 Engine::openDevice(DeviceConfig)                      WASAPI: miniaudio data callback
   -> AudioDevice::open -> backend.open                ASIO:   bufferSwitch(TimeInfo)
   -> renderer_.prepare, processors prepare                 |
   -> rebuild snapshot, device_.start()                     v  (driver format -> float, ASIO)
                                                       Engine::audioCallback(AudioIO)
 Engine::takeDeviceEvent()  <-- pendingDeviceEvents_ <--  deviceEvent(flag)  (any thread)
 Engine::reopenDevice()                                    |
   -> AudioDevice::resetConfig (asks the open driver)      v
   -> close, open again                                Renderer::processLive -> outputs
                                                            |  (float -> driver format, ASIO)
                                                            v
                                                       outputReady() (ASIO, if supported)
```

## Files

| File | What it holds |
|---|---|
| [AudioDevice.h](../../engine/src/AudioDevice.h) | `DeviceConfig`, `DeviceCaps`, `DeviceState`, `AudioIO`, `DeviceEvent`, the `AudioCallback` and `AudioBackend` interfaces, and `AudioDevice` |
| [AudioDevice.cpp](../../engine/src/AudioDevice.cpp) | `AudioDevice`: makes the backends (ASIO first, for COM), picks one by driver name, remembers the last config for resets |
| [backends/WasapiBackend.h](../../engine/src/backends/WasapiBackend.h), [.cpp](../../engine/src/backends/WasapiBackend.cpp) | WASAPI output through miniaudio, shared or exclusive; elsewhere the "System" driver |
| [backends/AsioBackend.h](../../engine/src/backends/AsioBackend.h), [.cpp](../../engine/src/backends/AsioBackend.cpp) | ASIO through `IASIO`: finding and loading drivers, channels, rate, buffer size, the callbacks, resets, control panel. Built only with `SUBSTATION_HAS_ASIO` |
| [backends/AsioSupport.h](../../engine/src/backends/AsioSupport.h) | The parts of ASIO hosting that need no SDK: the sample formats and their conversion, the buffer sizes to offer and to ask for (namespace `sub::asio`) |
| [miniaudio_impl.c](../../engine/src/miniaudio_impl.c) | The single translation unit that compiles miniaudio's implementation (`MINIAUDIO_IMPLEMENTATION`) |
| [EngineDevice.cpp](../../engine/src/EngineDevice.cpp) | `Engine`'s device API (`openDevice`, `reopenDevice`, `closeDevice`, `deviceStatus`, `deviceCapabilities`, `showDeviceControlPanel`, `takeDeviceEvent`, `takeInputMeters`, `masterScope`) and `Engine::audioCallback` |

miniaudio itself is vendored in `engine/third_party/miniaudio` (MIT). CMake builds it as a static
library of its own, as C, with `MA_NO_ENGINE MA_NO_NODE_GRAPH MA_NO_RESOURCE_MANAGER MA_NO_GENERATION`;
the engine uses it for WASAPI and also for decoding and WAV encoding (see [warp.md](warp.md) and
[recording.md](recording.md)).

## Key types

**`DeviceConfig`**: a device to open.

| Field | Meaning |
|---|---|
| `driver` | `"WASAPI"` (default) or `"ASIO"`, one of `AudioDevice::driverTypes()` |
| `name` | empty: the system default output (WASAPI), the first driver listed (ASIO) |
| `sampleRate` | 0: the rate the device runs at |
| `bufferFrames` | 0: the device's preferred size |
| `exclusive` | WASAPI exclusive mode |
| `inputChannels` | device channels to open, 0-based; none by default |
| `outputChannels` | empty: the first two (or the only one) |
| `window` | the main window's HWND; ASIO drivers own their dialogs by it |

**`DeviceCaps`**: what the open device offers: the names of all its input and output channels (open or
not), the sample rates it can run at and the buffer sizes it offers (empty: any), its preferred buffer
size, and whether it has a control panel.

**`DeviceState`**: the open device: driver type, name, rate, buffer size, input and output latency in
frames (as the driver reports them), the open input and output channels *in the callback's order*,
exclusive, and its `DeviceCaps`. `Engine::deviceStatus()` turns it into a `DeviceStatus` for the
application (latencies in ms; `backend` is the driver type).

**`AudioIO`**: one callback's audio: `inputs`/`numInputs`, `outputs`/`numOutputs` (planar float
buffers), `frames`, `sampleTime` (the device's sample clock at the first frame) and `hostTimeNs`
(`std::chrono::steady_clock`, i.e. QueryPerformanceCounter, when the callback began). The callback
must write every output sample.

**`DeviceEvent`**: flags, so several can be pending at once: `Stopped` (the device went away),
`Rerouted` (the system moved the output to another device; WASAPI), `ResetRequest` (the driver's
settings changed: close it and open it again; ASIO), `LatencyChanged` (ASIO).

**`AudioCallback`**: what a backend calls. `audioCallback(const AudioIO&)` runs on the real-time thread
and never locks, allocates or waits; `deviceEvent(DeviceEvent)` may come on any thread and must only
set flags. `Engine` implements it privately.

**`AudioBackend`**: a driver type: `name()`, `devices()`, `open()` (opens but doesn't start; throws
`std::runtime_error` with a message for the user, leaving nothing open), `start()`, `close()` (returns
once the callback can no longer run), `isOpen()`, `state()`, and the optional `showControlPanel()` /
`inControlPanel()`, `refreshLatencies()` (main-thread work after `LatencyChanged`) and
`adjustForReset(DeviceConfig&)` (before a reset: what to open instead).

**`AudioDevice`**: the device the engine plays through. `open()` closes the open device (of any driver
type), then opens the new one and remembers its config; `resetConfig()` returns that config adjusted by
the open backend's `adjustForReset()`. Asking for `"ASIO"` in a build without it throws "This build of
SUBstation has no ASIO support (the ASIO SDK was missing)".

## How it works

### Threads

- A backend is opened, queried and closed on one thread: the thread that created the engine (the UI
  thread). ASIO drivers are COM objects in its apartment. All `Engine` device calls take the engine's
  recursive `mutex_`.
- The audio callback runs on the backend's real-time thread (miniaudio's WASAPI thread, or the ASIO
  driver's own thread).
- Device events may arrive on any thread; `Engine::deviceEvent()` only ORs them into the atomic
  `pendingDeviceEvents_`.
- `openDevice`, `reopenDevice`, `closeDevice` and `showDeviceControlPanel` block the main thread, and
  a driver may show a dialog whose message loop calls back into the application meanwhile (the
  engine bridge doesn't poll the device from inside one).

### Opening a device (`Engine::openDeviceLocked`)

1. Refuses while a driver's control panel is up ("Close the driver's control panel first"): its
   message loop may call back in, and its driver has to stay until the panel closes.
2. `closeDeviceLocked()`: closes the device (waiting for the audio thread to exit), stops the
   `AudioClock` (MIDI input is dropped from here on), ends any recording (the takes so far are kept
   for `stopRecording()`; see [recording.md](recording.md)), frees retired snapshots and services the
   transport on the edit side.
3. `AudioDevice::open(config, this)`.
4. If the rate differs from the engine's, sets `sampleRate_`, reloads every source at the new rate
   (`reloadSourcesLocked`) and drops the live stretch voices (they are sized for the old rate; the next
   snapshot makes new ones).
5. Records the open input channels (`openInputChannels_`, in callback order; track inputs name device
   channels and are mapped to these indices in `inputEdgeLocked`), clears the input meters, drops the
   browser preview.
6. `renderer_.prepare(rate)` and `prepare()` on every processor; sets `midiInputDelay` to the device's
   buffer size (512 if it reports none) and `midiSampleRate`.
7. Rebuilds the snapshot, clears pending device events (they were about the device just closed), and
   starts the device. If anything throws, the device is closed again and the error passes on.

`closeDevice()` refuses while a control panel is up, too. `Engine::idle()` closes the device itself
when a `Stopped` event is pending; the UI hears of it from `takeDeviceEvent()`.

### The callback (`Engine::audioCallback`)

On the real-time thread, with denormals off:

1. `shared_.clock.update(hostTimeNs, sampleTime)`: the anchor MIDI input is stamped against.
2. Meters the open inputs (up to `SharedState::kMaxInputMeters`, 256): a peak per channel, kept as a
   running maximum until `takeInputMeters()` exchanges it with 0 (in the order of
   `DeviceStatus::inputChannels`).
3. Loads the snapshot and the live recording. With no snapshot, or while live output is suspended
   (offline renders, changing the number of audio threads), it writes silence. Otherwise
   `Renderer::processLive()` renders, and the CPU load (time taken over the buffer's duration) is
   smoothed into `shared_.cpuLoad`.
4. Advances `audioEpoch_`, which the edit side uses to know a callback has passed
   (`waitForCallbackLocked`, retired snapshots).

`processLive()` renders in chunks of at most `Renderer::kMaxBlock` (1024) frames. **Output channel
selection**: the master goes to the first two open outputs; any others are silent; with only one output
open it gets the mean of left and right. Which device outputs those are is the config's
`outputChannels` (ASIO), so the preferences' *Output Channels* (1/2, 3/4, ...; an odd last one alone,
which then plays in mono) is just the pair (or single channel) opened. The master's output also goes
into the oscilloscope ring (`SharedState::pushScope`, read by `masterScope()`).

### WASAPI (`WasapiBackend`)

- One miniaudio context, WASAPI only (any backend if that fails). `devices()` lists playback devices
  with their default flag. A name is matched exactly; "Audio device not found" otherwise.
- Opens a playback-only device: f32, 2 channels (miniaudio mixes the stereo output into the device's
  own channel layout), shared or exclusive, the asked rate and period size (0: the device's),
  `ma_performance_profile_low_latency`, `ma_wasapi_usage_pro_audio` (MMCSS "Pro Audio"),
  `noPreSilencedOutputBuffer` (the engine writes every frame) and `noFixedSizedCallback` (the renderer
  handles any block size). `inputChannels` and `outputChannels` are ignored.
- The data callback splits a device buffer into engine callbacks of at most `kChunk` (4096) frames,
  all stamped with the host time the device callback began, and interleaves the planar output into
  miniaudio's buffer (zeroing channels past two). Its `sampleTime` counts frames from 0 at open.
- miniaudio's notifications become `DeviceEvent::Stopped` and `Rerouted`, unless the backend is
  closing (`closing_`).
- `state()`: output latency is `internalPeriodSizeInFrames * internalPeriods` at the device's internal
  rate, converted to the engine's; input latency 0; output names `Left`, `Right`.
- `close()` calls `ma_device_uninit`, which blocks until the audio thread has exited.

A capture device (to record WASAPI inputs) would open miniaudio's duplex mode and fill
`AudioIO::inputs`; it isn't done yet (resampling records without inputs, so it works on WASAPI).

### The System driver (elsewhere)

On platforms other than Windows the same backend is the driver type every build has
(`kDefaultDriver`, "System"; `AudioDevice::driverTypes()` lists it, and ASIO never): its miniaudio
context takes the system's backends in miniaudio's order (Core Audio, PulseAudio, ALSA, JACK, sndio,
audio4, OSS, AAudio, OpenSL), but never miniaudio's null backend, which "plays" faster than real time,
so the playhead would race with no sound. Everything else is as for WASAPI: playback devices by name,
output only, the same callback. Exclusive mode is WASAPI's: the application doesn't offer it here. With
no sound server and no device, nothing opens; the application then runs without audio and says so.

### ASIO (`AsioBackend`)

**Finding drivers.** `listDrivers()` reads `HKLM\SOFTWARE\ASIO`: each key's `CLSID` and `Description`
(or the key name), with the DLL registered for the class (`InprocServer32`). Entries whose DLL is gone
(uninstalled drivers often leave them behind) and duplicate names are skipped. If the environment
variable `SUBSTATION_ASIO_DRIVERS` is set, it lists the drivers to use instead, loaded straight from
their DLLs, unregistered: `Name|{CLSID}|C:\path\driver.dll`, several separated by `;`. The tests'
fake driver comes this way, so tests never see the installed drivers ([testing.md](../testing.md)).

**COM apartments.** Drivers are apartment-threaded COM objects, which COM can only create directly in a
single-threaded apartment (`IASIO` can't be marshalled). So:

- `AudioDevice`'s constructor creates the ASIO backend *first*, and `AsioBackend`'s constructor calls
  `CoInitializeEx(COINIT_APARTMENTTHREADED)` on the engine's thread, before miniaudio's context would
  make it multithreaded. In the application Qt has already made the UI thread single-threaded.
- `open()` initialises COM again (balanced in `close()`), checks `CoGetApartmentType` and, in a
  single-threaded apartment, creates the driver with `CoCreateInstance` (an ASIO driver answers to its
  class id as the interface id).
- In a thread that is multithreaded all the same (and for `SUBSTATION_ASIO_DRIVERS` entries), it loads
  the driver from its DLL: `LoadLibraryW`, `DllGetClassObject`, `IClassFactory::CreateInstance`.

**One per program.** The ASIO callbacks carry no context, so they find the open backend through a
global `g_open`; only one ASIO device can be open per process ("Another ASIO device is open (only one
can be, in one program)"). `close()` clears `g_open`, then waits (up to 2 s) for callbacks already
running (`g_inCallback`, counted by `CallbackScope`) before disposing the buffers and releasing the
driver.

**Opening** (`open` then `setUp`):

1. `init(window)`, with the main window (or the desktop window); on failure, the driver's own
   `getErrorMessage()` goes into the exception text. Driver strings are in the ANSI code page and are
   converted to UTF-8.
2. Channels: `getChannels`, then `getChannelInfo` for every channel (its name, or "Input n"/"Output n";
   its sample type). Outputs default to the first two (or one). Channels out of range or listed twice
   are refused; so is a config with no channels at all, and any open channel whose sample type the
   engine can't play (DSD).
3. Sample rate: those of 32000..384000 Hz the driver says it can run at (`canSampleRate`) become the
   capabilities. The rate asked for, else the one it runs at; a driver with no rate until one is set
   gets 48000, else 44100, else the first it takes. If that differs from its current rate it is set
   (and read back). The running rate is always listed.
4. Buffer size: `getBufferSize` gives min, max, preferred and granularity. The sizes offered are
   `asio::bufferSizeOptions()`: granularity -1 means powers of two between the limits; a positive one,
   those of a list of common sizes (16..16384) that are a whole step from the minimum, plus the minimum
   and maximum; 0, only the preferred size. The preferred size is always offered. The size asked for is
   `asio::chooseBufferSize()`: the request if the driver takes it, else the nearest size offered (the
   larger on a tie); 0 asks for the preferred size. If `createBuffers` fails with it, the preferred size
   is tried.
5. `getLatencies` (the buffer size for both if it fails) and whether `outputReady()` is supported.
6. One float buffer per open channel (inputs, then outputs, in one scratch block), and the driver's
   output halves zeroed: the driver starts from silence.

**The callbacks.** `bufferSwitch` and `bufferSwitchTimeInfo` both call `process(index, time)`: it takes
the sample position from `ASIOTime` when it is valid (otherwise it keeps counting), converts each open
input's half-buffer to float, calls the engine, converts each output back into the driver's half-buffer,
and calls `outputReady()` if the driver supports it (so it can play the buffer as soon as it's filled).
`asioMessage` answers which selectors are supported, engine version 2, time info supported; a resync
request (the driver lost samples) needs nothing, since each callback brings its own timing.

**Sample-format conversion** (`AsioSupport.h`). Every sample format drivers use is converted: 16, 24
(packed, 3 bytes) and 32-bit integers, 32-bit containers holding 16, 18, 20 or 24-bit samples in their
low bits, 32 and 64-bit floats, in either byte order (`SampleType`, ASIO's own numbers). DSD drivers
aren't supported (`SampleFormat::bytes == 0`).

- `toDevice()`: floats are stored as they are (64-bit: widened); integers are clipped at full scale
  (±1.0, so +1.0 is `2^(bits-1) - 1` and doesn't wrap round to negative), scaled and rounded. NaN (from
  a broken plug-in) plays as silence.
- `fromDevice()`: integers are read in their byte order, sign-extended from their significant bits
  (whatever the driver left in the unused high bits is ignored) and scaled by `1 / 2^(bits-1)`.

**Driver events and resets.**

- `kAsioResetRequest` → `ResetRequest`. `kAsioBufferSizeChange` → `ResetRequest`, remembering the size
  asked for. `sampleRateDidChange` with a rate other than the one set (its clock follows another device)
  → `ResetRequest`, remembering the rate. `kAsioLatenciesChanged` → `LatencyChanged`.
- `showControlPanel()` sets `inControlPanel_` around the driver's `controlPanel()` (which may run a
  modal message loop). Not every driver asks for a reset after its settings changed, so afterwards it
  compares the buffer sizes and rate with those it opened with and raises `ResetRequest` itself.
  `Engine::showDeviceControlPanel()` holds the engine's lock throughout: nothing may close the driver
  while its panel is up.
- `adjustForReset()` puts into the config the buffer size the driver asked for (or, if none, its new
  preferred size when its sizes changed) and the rate it asked for (or the one it runs at now, if that
  changed). It runs before the driver is closed, since only the open driver can say.
- `refreshLatencies()` reads `getLatencies` again.

### Events, as the UI sees them (`Engine::takeDeviceEvent`)

One string per call, in this order of priority:

- `"stopped"`: clears every other pending flag (they were about the device that is gone).
- If no device runs, any other flags are cleared and `""` returned.
- `"reset"`, but not while the control panel is open (it waits until the panel returns).
- `"rerouted"`.
- `"latency"`, after calling `refreshLatencies()`, so `deviceStatus()` has the new values.

`Engine::reopenDevice()` opens `AudioDevice::resetConfig()`: the last config (driver, name, inputs and
outputs kept) with the driver's new buffer size and rate. It throws if no device has been opened. The
engine bridge polls `takeDeviceEvent()` with the meters and answers `"reset"` with it
([app/engine-bridge.md](../app/engine-bridge.md)). At start-up, if the saved settings can't be opened
(another clock, fewer outputs), the application tries the device's own settings (rate, buffer and
channels 0/empty), then the system default output (WASAPI; "System" elsewhere)
(`EngineBridge::startAudio()`).

The status line shows the input and output latency the driver reports (`DeviceStatus::latencyMs`,
`inputLatencyMs`).

### Inputs

A track's input names device channels; the bridge reopens an ASIO device with the extra input channels
it needs (`DeviceConfig::inputChannels`) when a track chooses one the driver hasn't open
(not while recording, which a reopen would end). Every buffer of the open inputs reaches the audio
callback (with its sample position and time), where the engine meters them, and the renderer monitors
and records them ([recording.md](recording.md)). A track input on a channel that isn't open reads
silence.

## Invariants and real-time rules

- `audioCallback` never locks, allocates, frees or calls into the application. Backends convert into buffers
  allocated at open.
- `deviceEvent` only sets atomic flags; all reaction happens on the UI thread.
- Device open, close, reset and control panel calls come from the thread that created the engine (an
  STA for ASIO). The audio thread calls nothing on an ASIO driver but `outputReady()`.
- A backend's `close()` returns only once its callback can't run any more, so the engine may then
  touch what the callback used (finish the recording, reset the clock).
- A driver's control panel may run a message loop: the device can't be closed or reset until it
  returns (`openDevice`, `closeDevice` throw; `"reset"` waits).
- Pending device events are cleared when a device opens: they belonged to the one before.

## Extending it

- **A new driver type** is a new `AudioBackend`: implement `name()`, `devices()`, `open()`, `start()`,
  `close()` (blocking until the callback stops), `isOpen()` and `state()`, call
  `AudioCallback::audioCallback` with planar float buffers, a running `sampleTime` and the steady-clock
  `hostTimeNs`, and report events as flags. Add it to `AudioDevice`'s constructor and to
  `AudioDevice::driverTypes()`. Mind COM: if it initialises COM, do it after the ASIO backend has.
- **WASAPI capture**: open miniaudio's duplex (or capture) device, fill `AudioIO::inputs`/`numInputs`,
  report `inputChannels`, `inputLatency` and input names in `state()`; recording and monitoring then
  work as they do for ASIO.
- **Another ASIO sample format**: add it to `SampleType` and `sampleFormat()`; `toDevice()` and
  `fromDevice()` work from `SampleFormat` alone.

## Gotchas

- Creating the ASIO backend after miniaudio would leave the thread multithreaded: drivers then load
  from their DLL rather than through COM, which works for in-process drivers but bypasses COM's own
  loading.
- Only one ASIO device per process: a second engine (in tests) can't open ASIO while another has it.
- `AudioDevice::resetConfig()` must run before closing: it asks the open driver.
- A sample-rate change reloads every decoded source and remakes the stretch voices; the bridge also
  refreshes its sources (`EngineBridge::refreshSources()`).
- WASAPI's `sampleTime` restarts at 0 on every open; ASIO's comes from the driver when it is valid.
- WASAPI may hand the engine several callbacks per device buffer with the same `hostTimeNs`;
  `AudioClock` keeps the first one's anchor ([midi.md](midi.md)).
- Recording ends whenever the device is opened again, including a driver's reset.

## Tests

- [tests/engine/test_asio.cpp](../../tests/engine/test_asio.cpp), with the fake driver
  [tests/asio_driver/](../../tests/asio_driver/): listing the driver, status and capabilities, output in
  every sample format (decoded from the bytes the driver got), full scale clipped, a single output
  mixing the master to mono, open inputs metered, sample rates, buffer sizes (powers of two, steps, a
  fixed size), reset requests, buffer-size change requests, the driver changing its clock, new
  latencies, the questions drivers ask (`asioMessage`), the control panel, errors leaving no driver
  open, one ASIO device per program, playing on the driver's own thread. Skipped when the engine was
  built without the ASIO SDK. The preferences and the start-up fallback in the application are
  [tests/app/test_bridge_settings.cpp](../../tests/app/test_bridge_settings.cpp)'s and
  [tests/app/test_ui_dialogs.cpp](../../tests/app/test_ui_dialogs.cpp)'s.
- The fake driver runs in manual mode unless a test says otherwise: each `driver.process(n)` is `n`
  buffer switches on the test's thread. Its exported hooks set the sample type, buffer sizes,
  latencies, input levels and loopback, make `init` fail, change settings from the control panel, send
  `asioMessage`s, change the sample rate, and read back the bytes written to its outputs.
- [tests/engine/test_recording.cpp](../../tests/engine/test_recording.cpp) checks that a device change
  ends a recording; [tests/engine/test_midi_input.cpp](../../tests/engine/test_midi_input.cpp) that
  MIDI input is dropped without a running device.
