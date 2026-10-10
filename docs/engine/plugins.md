# Plug-in hosting (VST3)

How the engine hosts VST3 plug-ins: finding what a file holds, loading a plug-in as a `Processor`, running it on the
audio thread without ever waiting for it, its parameters, state, latency, buses, transport and editor window. The code
is in [engine/src/plugins/](../../engine/src/plugins); the parts of the VST 3 SDK it uses are vendored in
`engine/third_party/vst3sdk` (MIT-licensed since SDK 3.8), so nothing else needs installing.

Scanning installed plug-ins happens in child processes, `substation-scan` ([tools/scanner](../../tools/scanner/main.cpp),
which links the engine and nothing of Qt), driven by the application layer's plug-in index: see
[app/plugin-scanner.md](../app/plugin-scanner.md). How the user finds and uses plug-ins is in
[guide/plugins.md](../guide/plugins.md). The `Processor` interface itself is described in [devices.md](devices.md).

Plug-ins load and play on Linux too, but without their editor windows (see [`EditorWindow`](#editorwindow)).

## Files

| File | What it holds |
|---|---|
| [PluginFormat.h](../../engine/src/plugins/PluginFormat.h) | `PluginDescription` and the `PluginFormat` interface: `name()`, `defaultSearchPaths()`, `scanFile()`, `instantiate()` |
| [Vst3Format.h](../../engine/src/plugins/Vst3Format.h) / [.cpp](../../engine/src/plugins/Vst3Format.cpp) | the host context plug-ins see ("SUBstation"), loaded modules (shared by instances, unloaded with the last), scanning, instantiation |
| [Vst3Processor.h](../../engine/src/plugins/Vst3Processor.h) / [.cpp](../../engine/src/plugins/Vst3Processor.cpp) | a VST3 plug-in as a `Processor`: buses, events, parameters, automation, state, restarts, the component handler |
| [Vst3Support.h](../../engine/src/plugins/Vst3Support.h) | allocation-free building blocks for the audio thread: `ProcessGuard`, `HostEventList`, `HostParamQueue`, `HostParamChanges`, `ParamChangeQueue`, `SpinLock` |
| [EditorWindow.h](../../engine/src/plugins/EditorWindow.h) / [.cpp](../../engine/src/plugins/EditorWindow.cpp) | the window holding a plug-in's editor (`IPlugView`, `IPlugFrame`), and what every system's window does with the view |
| [EditorWindowWin32.cpp](../../engine/src/plugins/EditorWindowWin32.cpp) | `EditorWindow` on Windows: a Win32 window |
| [EditorWindowNone.cpp](../../engine/src/plugins/EditorWindowNone.cpp) | `EditorWindow` off Windows: it never opens |
| [Vst3Platform.h](../../engine/src/plugins/Vst3Platform.h) | what hosting does differently on each system: `prepareThreadForModules()`, `systemPluginFolders()`, `binaryInBundle()`; [Vst3PlatformWin32.cpp](../../engine/src/plugins/Vst3PlatformWin32.cpp), [Vst3PlatformPosix.cpp](../../engine/src/plugins/Vst3PlatformPosix.cpp) |
| [EngineChains.cpp](../../engine/src/EngineChains.cpp) | `addPluginProcessor()`, `retireProcessorLocked()`, the processor and editor calls (made without the engine lock) |
| [Engine.cpp](../../engine/src/Engine.cpp) | `Engine::idle()`: plug-ins' main-thread work, destroying removed ones, realigning on latency changes |
| [tools/scanner/main.cpp](../../tools/scanner/main.cpp) | `substation-scan`: reads plug-in files with `Vst3Format::scanFile()` for the application, in a process of its own |

## `PluginFormat`

```cpp
struct PluginDescription { format, path, uid, name, vendor, version, category, isInstrument };

class PluginFormat {
    virtual std::string name() const = 0;
    virtual std::vector<std::string> defaultSearchPaths() const = 0;
    virtual std::vector<PluginDescription> scanFile(const std::string& path) = 0;  // loads its code; throws
    virtual std::shared_ptr<Processor> instantiate(const std::string& path, const std::string& uid,
                                                   double sampleRate, int maxBlockSize) = 0;  // main thread
};
```

`uid` is the VST3 class id (32 hex digits); `category` the VST3 sub-categories (`"Instrument|Synth"`, `"Fx|Delay"`).

## `Vst3Format`

A singleton (`Vst3Format::instance()`).

- **Host context.** `SubHostApplication` derives from the SDK's `HostApplication` and names itself "SUBstation". It is
  created once and never freed: plug-ins may keep it until they unload.
- **Modules.** `loadModule(path)` keeps a `weak_ptr` per module, keyed by the path's `platform::fileKey()` (normalised,
  and in any case on Windows), so every instance from one file shares the module and the module unloads with the last
  instance. It sets the host context on the module's factory.
- **COM.** Some plug-ins need COM on the thread that loads them: `prepareThreadForModules()` calls `OleInitialize`
  once per thread on Windows (and does nothing elsewhere). Qt has set it up on the UI thread already; the scanner
  process has not.
- **`defaultSearchPaths()`** (`systemPluginFolders()`): on Windows `FOLDERID_ProgramFilesCommon\VST3` and
  `FOLDERID_UserProgramFilesCommon\VST3` (`C:\Program Files\Common Files\VST3` and
  `%LOCALAPPDATA%\Programs\Common\VST3`); on Linux `~/.vst3`, `/usr/lib/vst3` and `/usr/local/lib/vst3`; on macOS
  `~/Library/Audio/Plug-Ins/VST3` and `/Library/Audio/Plug-Ins/VST3`. The application layer's
  `standardPluginFolders()` ([app/src/plugins/PluginPaths.h](../../app/src/plugins/PluginPaths.h)) takes these,
  unless `SUBSTATION_VST3_PATH` says otherwise (see [app/plugin-scanner.md](../app/plugin-scanner.md)).
- **`binaryInBundle(name)`**: where a bundle's code is on this system (`Contents/x86_64-win/<name>.vst3`,
  `Contents/<arch>-linux/<name>.so`, `Contents/MacOS/<name>`), for the application's scan cache, which signs a bundle
  by its code's file.
- **`scanFile(path)`**: loads the module and lists its `kVstAudioEffectClass` classes (controllers and other helper
  classes are skipped). The vendor falls back to the factory's. `isInstrument` is true if the sub-categories include
  `Instrument`. A module with several plug-ins (an instrument and its FX version) lists each. The application calls
  it only in its child process, `substation-scan`, so a plug-in that crashes or hangs while loading takes down only
  that process.
- **`instantiate(path, uid, sampleRate, maxBlockSize)`**: finds the class by id, makes a `Vst3Processor` and prepares
  it. Errors for the user: not a class id; "does not contain this plug-in any more".

## `Vst3Processor`

### Life cycle (main thread)

The constructor creates the component, initialises it with the host context and gets its `IAudioProcessor`. The
controller is the component itself (single-component plug-ins, like most JUCE ones) or a class of its own, created
and initialised from the component's controller class id. With a separate controller it:

- connects the two `IConnectionPoint`s *directly*, not through the SDK's `ConnectionProxy`, which drops messages sent
  from any thread but the main thread (plug-ins send them from their own threads too, such as FabFilter editors
  asking for analyser data from their drawing thread);
- gives the controller the component's state (`setComponentState`), so it starts in step.

It then sets the component handler, finds the event input bus, builds the parameter list and the MIDI controller map.
Failures throw `std::runtime_error` with a message for the user ("could not be created", "could not be
initialized", "does not process audio").

`prepare(sampleRate, maxBlockSize)` takes the plug-in from the audio thread (`ScopedSuspend`), deactivates it, sets up
buses, calls `setupProcessing` (real-time, 32-bit float), allocates buffers and activates it again (`setActive`,
`setProcessing`, reading latency and tail).

The destructor closes the editor, deactivates, disconnects and terminates. The module pointer is the first member,
so the module outlives everything the plug-in made.

### Threads and the engine lock

Plug-ins are created, configured, asked about and destroyed on the main thread, as VST3 requires; only `process()`
(and `reset()`, which only sets a flag) runs on the rendering thread (the audio thread, its workers, or a render in
the background). Loading them on another thread isn't an option: JUCE plug-ins take the thread that creates their
first instance for their message thread (their timers and async calls run there), and Komplete Kontrol hung when
its module was loaded off the main thread. A project's plug-ins load after it opens, one at a time between the
UI's events, instead (the engine bridge's [BridgeLoading.cpp](../../app/src/audio/BridgeLoading.cpp)).

- Plug-ins may run a message loop inside a call (a licence dialog) that calls back into the engine or the UI. The
  engine's lock (`Engine::mutex_`) is recursive, and slow plug-in calls don't hold it: `addPluginProcessor()` loads
  the plug-in without the lock (then checks the chain still exists and the rate didn't change), and
  `setProcessorParam`, `processorParamText`, `processorState`, `setProcessorState`, the editor calls,
  `takeProcessorEvents` and each processor's `idle()` are made without it. The engine bridge doesn't act on plug-in
  reports until the call returns (its `busy` counter).
- A removed plug-in waits until no snapshot uses it and is destroyed in `Engine::idle()`, on the main thread
  (`retireProcessorLocked()` closes its editor and moves it to `graveyard_`; `idle()` destroys those only the
  graveyard holds, outside the lock, since plug-ins may take their time to go: 20 ms of them a call, the rest
  in the next calls, so closing a project full of plug-ins doesn't hold up the UI).
- The component handler may be called from anywhere by badly behaved plug-ins, so what it touches is thread-safe
  (`mutex_` for the parameter lists and pending events; atomics for restart flags).

### The lock-free handshake: `ProcessGuard`

The audio thread never waits for a plug-in's main-thread work. When the main thread must take a plug-in away for a
moment (restarting it for a new latency or bus layout, loading its state, flushing parameters, re-reading the MIDI
map) it takes it with a `ProcessGuard`:

```
 state:   Idle --tryEnter()--> Processing --leave()--> Idle          (rendering thread)
          Idle --suspend()---> Suspended  --resume()-> Idle          (main thread; nests)
```

- `process()` calls `tryEnter()` (one compare-exchange). If the main thread has the plug-in, it returns at once and
  the track's audio passes by it unchanged.
- `suspend()` spins (yielding) until the state is `Idle`, then marks it `Suspended`: it waits at most for one
  `process()` call to finish. `ScopedSuspend` is the RAII form; suspensions nest.

### Buses: mono, stereo, sidechain

The track is stereo. `setupBuses()` (plug-in inactive):

1. Finds the main input and output audio buses (`kMain`, else the first) and the first `kAux` input (the sidechain).
2. Asks for stereo main buses and a stereo sidechain; if refused, stereo main buses and the plug-in's own sidechain
   layout; else its own layout as it was. A refused arrangement may leave any bus changed, so each try asks for every
   bus.
3. Activates the main buses, the event input and the aux input. If the plug-in won't activate the aux bus, or settles
   on no channels there, there is no sidechain (`auxInput_ = -1`).
4. Reads back the channel count of every bus and sizes a buffer per channel (`allocateBuffers`, `maxBlock` frames).

In `process()`:

- The track's signal goes into the main input; into a mono bus it is mixed down (`0.5 * (L + R)`). Extra channels of
  a wider bus get silence.
- The sidechain (`Processor::sidechain()`) goes into the aux input (stereo if it takes it, mixed to mono if not). A
  missing sidechain reaches the plug-in as silence, flagged as such (`silenceFlags`).
- Every other bus gets a buffer of its own: silence in (flagged), output ignored. So extra outputs are not used (no
  multi-output instruments yet).
- Each input bus's silence flags are set per channel by checking for all-zero samples.
- The main output is copied back to the track; a mono output goes to both sides.

`hasSidechain()` is `auxInput_ >= 0`; `acceptsMidi()` is `eventInput_ >= 0` (an instrument, or an effect with an
event input, as a vocoder or a pitch corrector has: it can take another track's notes, [midi.md](midi.md#a-devices-midi-input-from-another-track)). Routing and alignment of sidechains are in [routing.md](routing.md).

### Events: notes and MIDI

`buildEvents()` turns the block's `ProcessEvent`s into VST3 events on the first event input bus (instruments play the
MIDI clips, and the piano roll's notes, sample-accurately):

- Note-ons (velocity / 127) and note-offs, with the note's id (`ProcessEvent::noteId`, -1 for none); a note-on
  with velocity 0 releases. `held_[channel][key]` counts notes without a note-off yet.
- A note's bend (`NoteBend`, MIDI 2.0's per-note pitch bend) becomes a `kNoteExpressionValueEvent` of
  `kTuningTypeID` for the note it names: VST3's per-note pitch, normalized as 0.5 + semitones / 240 (0.5 is none,
  ±120 semitones across). A bend without a note id is dropped (note expressions address notes by id). A plug-in
  that doesn't support tuning expressions ignores them.
- `reset()` sets `releaseAll_`; the next block starts with a note-off for every held note. (Real-time resets come on
  stop, locate, loop wrap and around offline renders; see [midi.md](midi.md).)
- Raw MIDI: control change, channel pressure and pitch bend are mapped to parameters through the controller's
  `IMidiMapping` (`buildMidiMap()` asks for every channel and controller once, and again on
  `kMidiCCAssignmentChanged`), and added as parameter changes at their offset. Polyphonic key pressure becomes a
  `kPolyPressureEvent`. Other messages are dropped.
- Up to 2048 events per block (`kMaxEvents`).

### Transport

`fillContext()` gives the plug-in, per block: tempo, time signature, position in samples (`projectTimeSamples`) and in
beats (`projectTimeMusic`), bar position (the last bar start, from the time signature), playing state, the loop
(`kCycleActive`, cycle start and end in beats) when looping, the sample rate, a continuous sample count and the system
time. Blocks are split where the loop wraps (the renderer's slices), so tempo-synced plug-ins stay in time.

### Parameters

`buildParams()` reads the controller's parameter list into `ParamInfo`s:

- `id` is the VST3 `ParamID` as text; the name falls back to the short title, then "Parameter N".
- Values are presented as **plain**: 0..1 for continuous parameters, the step index for stepped ones
  (`minValue` 0, `maxValue` the step count). `ParamInfo::fromNormalized` / `toNormalized` therefore match VST3's own
  stepped mapping, so values round-trip ([automation.md](automation.md)).
- Lists (`kIsList`, or one step) of up to 128 steps get their value names (`getParamStringByValue`). Some plug-ins
  name only the current value whatever they are asked; if two names repeat, the names are dropped (a one-step list
  becomes Off/On). Longer lists show as a knob with the plug-in's text.
- `automatable` from `kCanAutomate`, `readOnly` from `kIsReadOnly`; `hidden` for `kIsHidden` and for the plug-in's
  bypass (`kIsBypass`): the device's on/off switch stands in for it. The device view leaves out read-only and hidden
  parameters.
- `paramText()` asks the controller for its text (`-3.0 dB`, `Bell`); the engine bridge adds the unit if the text lacks
  it (`EngineBridge::pluginParamText()`).

**Parameter queues.**

```
 UI setParam()        -> controller.setParamNormalized + toAudio_ (ParamChangeQueue) --+
 editor performEdit() -> toAudio_ + ParamEdited event                                   |
 automation           -> Processor::automate() --------------------------------------+ |
                                                                                      v v
                                     process(): inputChanges_ (HostParamChanges) at their offsets
                                                 | plug-in's outputParameterChanges
                                                 v
                                     fromAudio_ (SpscQueue, last value per param per block)
                                                 | idle() (main thread)
                                                 v
                                     values_, controller.setParamNormalized, ParamsChanged event
```

- `toAudio_` is a `ParamChangeQueue`: a single-consumer queue of 4096 changes whose producers (the UI, a plug-in
  calling back from elsewhere) share a `SpinLock`. The audio thread only pops. `process()` adds what it pops at
  offset 0.
- `inputChanges_` / `outputChanges_` are `HostParamChanges`: one `HostParamQueue` per parameter in the block, found by
  an open-addressing hash on the id (plug-ins may report hundreds of output parameters per block), sized to the
  parameter count plus 64 (128..4096). Each queue holds 16 points sorted by offset; a second point at one offset
  replaces the first; when full, the latest value replaces the last point.
- The plug-in's output parameters (meters, values it changed itself) go into `fromAudio_` (the last value of each
  per block) and reach its controller, `values_` and the UI in `idle()`.
- `values_` (atomics) is what `getParam()` reads, from any thread.
- Before its state is saved, queued changes are handed to it in a `process()` call without audio
  (`flushParameters()`, `numSamples` 0, with the plug-in suspended from the audio thread), so the state includes them
  even when no audio device runs.

**Edits in the plug-in's editor** come through the component handler: `beginEdit` starts a gesture (a serial and the
value before it) and reports `ParamTouched` (its automation shows); `performEdit` sends the value to the processor and
reports `ParamEdited` with the value, the value before the gesture and the gesture serial, so the UI records one undo
step per knob drag; `endEdit` ends the gesture. The engine bridge treats `ParamEdited` (and `ParamTouched`) as an
edit only while the plug-in's editor is open (some plug-ins report their own changes as edits while their state is
restored).

**Automation** goes to the processor with the block's other parameter changes, at its sample offsets; the last
automated value of each parameter is kept in `automated_` and sent to the controller in `idle()`
(`applyAutomatedValues()`), so the plug-in's editor follows. While doing so `syncingAutomation_` is set, and
`performEdit` calls the plug-in makes in answer are ignored (they would override the automation).

### `idle()`: main-thread work

The engine calls each processor's `idle()` from `Engine::idle()`, which the engine bridge calls on a timer. For a
plug-in it:

1. Drops a closed editor window and reports `EditorClosed`.
2. Applies automated values and output parameters to the controller.
3. Handles restarts the plug-in asked for (`restartComponent` only records the flags: restarts deactivate the
   plug-in, which it may not expect during that call):
   - `kReloadComponent`, `kIoChanged`, `kLatencyChanged`, `kPrefetchableSupportChanged`: suspend, deactivate, set up
     buses and buffers again if the I/O changed, reactivate; if the latency changed, report `LatencyChanged` and
     return true, so the engine rebuilds the snapshot with new delay compensation.
   - `kParamTitlesChanged` (also when a program list changes) or `kReloadComponent`: rebuild the parameter list and
     the queues, report `ParamInfoChanged`.
   - `kParamValuesChanged`: re-read every value.
   - `kMidiCCAssignmentChanged`: rebuild the MIDI map.
4. Reports `ParamsChanged` if any value changed.

Other component-handler calls become events: `setDirty(true)` reports `StateDirty` (the plug-in changed in a way no
parameter shows, such as a preset picked in its editor: the project is marked as changed); `requestOpenEditor`
reports `EditorRequested`. The engine bridge collects them with `Engine::takeProcessorEvents()`.

### State

`getState()` returns a `.vstpreset` in memory: the component's state plus, for plug-ins with a separate controller,
the controller's own state (`PresetFile::savePreset`, with the class id). It flushes queued parameter changes first.
`setState()` suspends the plug-in, drops parameter changes queued from before (they must not undo the state), loads
the preset into the component (and controller), then re-reads the values and reports `ParamsChanged`. It throws "These
settings are not for <name>" if the preset is for another class.

Projects save each plug-in's complete state (base64) and which plug-in it is; the same bytes are what *Load Preset…*
and *Save Preset…* read and write as standard `.vstpreset` files. A device's plug-in lives as long as the device is in
its chain: reordering or changing the chain around it never reloads it, and a plug-in moved to another track moves as
it is (`Engine::moveProcessor`). When a plug-in device goes away (deleted, or its track) the engine bridge keeps its
state, so undo brings it back as it was. See [app/engine-bridge.md](../app/engine-bridge.md) and
[app/serialization.md](../app/serialization.md).

### Latency

`latencySamples()` is the plug-in's `getLatencySamples()` (capped at 2^22), read when it activates. It is compensated
per routing edge: the other tracks (and the metronome) are delayed to line up, sidechains are lined up at their
device, and exports come out aligned ([routing.md](routing.md)). A latency change reaches the engine through
`idle()`. A switched-off device adds no latency (`Engine::insertLatency`). The device's tooltip shows it
(`ProcessorInfo.latency`). The tail (`getTailSamples()`) is capped at 60 seconds.

### Offline renders and resets

Offline renders share plug-ins with live playback. Around them the engine calls `resetOffline()` (suspend, deactivate
and reactivate: clears tails a real-time reset can't, and forgets held notes) and requests a real-time reset. Offline
blocks set `ProcessContext::offline`, but the plug-in is still processed in `kRealtime` mode.

## `EditorWindow`

`EditorWindow` is the same class on every system: what it does with the view (attaching it, telling it its size and
its content scale, detaching it) is [EditorWindow.cpp](../../engine/src/plugins/EditorWindow.cpp)'s, and the window
itself is a file per system's (`EditorWindow::Native`). `EditorWindow::canHold(view)` says whether this system's
window can hold a view (the platform type it supports: an HWND on Windows); `Vst3Processor::openEditor()` asks it
before making one. macOS's (`kPlatformTypeNSView`) and X11's (`kPlatformTypeX11EmbedWindowID`) windows go beside
Windows'.

On platforms other than Windows there are no editor windows yet: [EditorWindowNone.cpp](../../engine/src/plugins/EditorWindowNone.cpp)
stands in, `canHold()` is false and the window never opens, so `openEditor()` returns false (`hasEditor()` still says
whether the plug-in has one). The application then says the plug-in has no editor, and the device view's generic
editor (a knob per parameter) is the way to edit it. The rest of this section is about Windows
([EditorWindowWin32.cpp](../../engine/src/plugins/EditorWindowWin32.cpp)).

A plain Win32 top-level window (class `SUBstationPluginEditor`) holding the plug-in's `IPlugView`, owned by the main
window so it floats above it; Qt's event loop dispatches its messages like any other window's. Main thread only.

- It has a caption and close button only (no minimise or maximise, also refused from the system menu), and a sizing
  frame only if the view can resize.
- It is created where it will go (the last editor's position, kept on screen, or centred over the owner, cascading by
  28 px), so it has that screen's scale from the start; the content scale is set before attaching
  (`IPlugViewContentScaleSupport`), and it is sized to the view before and after attaching (some plug-ins only know
  their size once attached).
- `resizeView()` (the plug-in asks): resizes the window's client area, then calls `onSize` if the view's size differs.
- `WM_SIZING`: lets the user resize only to sizes the view accepts (`checkSizeConstraint`); `WM_SIZE` tells the view.
- DPI changes: on `WM_GETDPISCALEDSIZE` it tells the view the new scale first and sizes the window to what the view
  asks for; `WM_DPICHANGED` applies the suggested rectangle.
- `WM_SETFOCUS` passes focus to the plug-in's child window, so keys go to the plug-in.
- Closing: `WM_CLOSE` marks it closed and detaches the view; the processor notices in `idle()` and reports
  `EditorClosed`. Before it goes or hides while active, it activates its owner (otherwise Windows may pick another
  application's window after an Alt+Tab).
- `setVisible(false)` hides it, keeping its place and the plug-in's view; shown again, it doesn't take the focus.
- The processor remembers the last position (`editorPosition_`), and the next editor opens there.

Keyboard shortcuts while an editor has the focus are routed by the UI (`PluginEditorKeys`:
[ui/README.md](../ui/README.md#shortcuts-from-plug-in-editors), [guide/shortcuts.md](../guide/shortcuts.md)).

## Real-time rules

- The host never allocates, locks or waits in `process()`: event lists, parameter queues and bus buffers are
  allocated with the plug-in inactive (`allocateBuffers`). What a plug-in does in its own `process()` is up to the
  plug-in.
- `process()` returns at once if the block is longer than `maxBlock`, has no channels, or the main thread holds the
  plug-in.
- Anything that rebuilds buffers or queues does it under `ScopedSuspend`.
- `ParamChangeQueue::push` uses a spin lock on the producers' side only; the audio thread pops without it.

## CLAP (not implemented)

CLAP would be a second `PluginFormat`: its plug-ins become `Processor`s, its main-thread callbacks go in
`Engine::idle()`, and the scanner, device view and projects work as they do for VST3. In practice:

- a `ClapFormat : PluginFormat` with `scanFile()` and `instantiate()`, and a `ClapProcessor : Processor` that applies
  `automation()` in `process()`, maps the sidechain to its aux port, and reports `ProcessorEvent`s;
- `Engine::addPluginProcessor()` accepts only `"VST3"` today and calls `Vst3Format` directly; it would choose the
  format by name;
- the child-process scanner ([tools/scanner/main.cpp](../../tools/scanner/main.cpp)) would scan with it too, and
  the application's scan cache entries ([app/src/plugins/PluginIndex.h](../../app/src/plugins/PluginIndex.h)) would
  carry a format.

Multi-output instruments and MIDI effect plug-ins are not supported either: plug-ins get their main buses and a
sidechain only.

## Tests

The tests use four VST3 plug-ins built with the engine ([tests/vst3_plugins/](../../tests/vst3_plugins)): *SUB Test
Synth*, an instrument with a separate controller (it reports the transport it gets back as parameters, and a note's
tuning expression bends that note's sine); *SUB Test Effect*, a single-component effect with adjustable latency and,
on Windows, a Win32 editor; *SUB Test Mono*, a mono effect without a controller; and *SUB Test Note Effect*, an
effect with an event input whose output is its input plus each held note's velocity / 127 (DC); plus *SUB Test Sidechain* ([test_sidechain.cpp](../../tests/vst3_plugins/test_sidechain.cpp)),
whose output is its input plus its sidechain. The tests see only these, never the installed ones: the engine's tests
load the bundle by its path (`SUBSTATION_TEST_PLUGINS_BUNDLE`, [harness/Fixtures.h](../../tests/engine/harness/Fixtures.h)),
and the application's tests point `SUBSTATION_VST3_PATH` at folders of their own.

- [tests/engine/test_vst3_engine.cpp](../../tests/engine/test_vst3_engine.cpp): scanning a module's classes,
  sample-exact notes, pitch, velocity and chords, parameters as the plug-in describes them and changes reaching the
  processor, the transport and loop splitting, repeatable offline renders, effects, automation reaching the plug-in
  and its controller, mono plug-ins on a stereo track, latency compensation (also on the master), state, load errors,
  chain order, moving to another track as it is, editor edits, resizing and closing (Windows only; skipped
  elsewhere), plug-ins without an editor.
- [tests/engine/test_note_bends_engine.cpp](../../tests/engine/test_note_bends_engine.cpp) and
  [test_midi_routing_engine.cpp](../../tests/engine/test_midi_routing_engine.cpp): a note's bend reaching a plug-in
  as its tuning expression; an effect with an event input playing along with another track's notes.
- [tests/engine/test_sidechain_engine.cpp](../../tests/engine/test_sidechain_engine.cpp): sidechains into plug-ins, and
  a missing sidechain reaching the plug-in flagged as silence.
- [tests/app/test_plugin_index.cpp](../../tests/app/test_plugin_index.cpp): scanning with `substation-scan`, including
  a plug-in that crashes or hangs while loading.
- [tests/app/test_bridge_plugins.cpp](../../tests/app/test_bridge_plugins.cpp) and
  [tests/app/test_ui_device_panel_plugins.cpp](../../tests/app/test_ui_device_panel_plugins.cpp): plug-ins in the
  application: projects and their state, missing plug-ins, undo, what plug-ins report, loading after a project opens,
  the device view's parameters, editor button and presets. Editor tests briefly show real windows on Windows.
