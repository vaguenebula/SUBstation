# Engine bridge and start-up

The engine bridge keeps the audio engine ([Engine.h](../../engine/src/Engine.h)) in step with the
[project model](model.md), and feeds the UI with what the engine knows (playhead, meters, plug-in reports, device
events) through Qt signals. It lives in [app/src/audio](../../app/src/audio), next to the persistent audio and MIDI
preferences ([AudioSettings.h](../../app/src/audio/AudioSettings.h)) and where the bridge puts the files it makes
([AudioFiles.h](../../app/src/audio/AudioFiles.h)). This page also covers how the application starts and stops.

## Overview

```
  Project (Qt signals)              EngineBridge                     sub::Engine (Engine.h)
  trackInserted / returnInserted ─► addEngineTrack ───────────────► addTrack, trackChain, ...
  trackRemoved / returnRemoved   ─► onTrackRemoved ───────────────► removeTrack
  trackChanged                   ─► onTrackChanged: mixer, sends,
                                    routes, editor titles ────────► setTrackGain/Pan/Mute/Solo, setTrackInput...,
                                                                    setTrackSend / removeTrackSend, setTrackOutput...
  tracksArranged                 ─► pushRoutes ───────────────────► setTrackOutput, setTrackOutputSidechain,
                                                                    setTrackInMonitored, setTrackInputTrack...
  clipsChanged                   ─► pushClips ────────────────────► setTrackClips / setTrackNotes (+ decoding)
  devicesChanged                 ─► syncDevices ──────────────────► addBuiltinProcessor, addPluginProcessor,
                                                                    moveProcessor, setChainOrder, addRack,
                                                                    addRackChain, setProcessorSidechain...
  freezeChanged                  ─► onFreezeChanged ──────────────► setTrackFrozen, the frozen audio as its clips
  chainChanged                   ─► onChainChanged ───────────────► setChainGain/Pan/Mute/Solo
  deviceParamChanged             ─► onDeviceParamChanged ─────────► setProcessorParam
  deviceStateChanged             ─► pushDeviceState ──────────────► setProcessorState
  settingsChanged                ─► pushSettings ─────────────────► setTempo, setTimeSignature, setLoop
  automationChanged              ─► onAutomationChanged ──────────► setTrackAutomation
  reset                          ─► onReset: everything again

  QTimer 16 ms ─► pollPosition ─► positionChanged, transportChanged, countingInChanged
  QTimer 33 ms ─► pollMeters ───► pollRecording; takeMeters → metersUpdated;
                                  pollPlugins (Engine::idle, processor events); pollDevice
  QThreadPool (2 threads) ─► Engine::loadSource(path) ─► sourceReady / sourceFailed (on the GUI thread)
  QThreadPool (1 thread)  ─► Engine::setProcessorState(...) (built-in devices' states, in order)
```

- Everything in the bridge runs on the **Qt GUI thread**, except decoding audio files and restoring built-in
  devices' states, which run in two small thread pools; their results come back to the GUI thread.
- The bridge never waits on the audio thread: the engine publishes immutable snapshots and the bridge reads atomics
  and queues (see [architecture.md](../architecture.md#real-time-rules-and-the-boundary-with-the-audio-thread) and
  [engine/README.md](../engine/README.md)).
- The bridge only pushes what changed: it keeps the last value it gave the engine for mixers, inputs, outputs, sends,
  sidechains, device on/off and rack chain mixers, and compares before calling.
- Its slots never let an exception reach Qt: each runs inside `guarded()`, which logs what was thrown
  (`qWarning`) and goes on.

## Files

| File | What it holds |
|---|---|
| [EngineBridge.h](../../app/src/audio/EngineBridge.h) | `EngineBridge`: its whole API, signals and QML properties; its header comment is the design. Constants: `kComputerKeyboard`, `kFreezeTailSeconds`, `kMaxHiddenEditors`, `kPluginGapMs`, `kPluginRetryMs`, `kPositionPollMs`, `kMeterPollMs`. |
| [EngineBridge.cpp](../../app/src/audio/EngineBridge.cpp) | Start-up (the project's signals it listens to, its timers and pools), `shutdown()`, the ids |
| [BridgePrivate.h](../../app/src/audio/BridgePrivate.h) | `EngineBridge::Private`: the maps from model ids to engine ids, and the last value given to the engine of everything it pushes only when it changes. What the bridge's files share: `BusyScope` (`busy` counted around a plug-in's or driver's call, however it ends), `stateBytes`/`stateData` (a processor's state as the engine and the model have it), `stringList`, `guarded`. The application layer's own (it includes `Engine.h`). |
| [BridgeTracks.cpp](../../app/src/audio/BridgeTracks.cpp) | Engine tracks, mixers, outputs, sends, clips and notes (and a drag's preview of them), tempo and loop, audio threads |
| [BridgeInputs.cpp](../../app/src/audio/BridgeInputs.cpp) | Tracks' audio and MIDI inputs and monitoring, the device's inputs, the MIDI inputs open |
| [BridgeDevices.cpp](../../app/src/audio/BridgeDevices.cpp) | Devices' processors in every chain, racks, sidechains, parameters, states |
| [BridgePlugins.cpp](../../app/src/audio/BridgePlugins.cpp) | Plug-ins' states, editors and reports; what a device's editor reads (`deviceParams`, `processorDisplays`...) |
| [BridgeLoading.cpp](../../app/src/audio/BridgeLoading.cpp) | A project's plug-ins loading after it opens, one at a time |
| [BridgeParameters.cpp](../../app/src/audio/BridgeParameters.cpp) | Automation pushed to the engine, overrides, `ParamSpec`s for the UI |
| [BridgeSources.cpp](../../app/src/audio/BridgeSources.cpp) | Decoding audio files in a thread pool; waveforms; file headers |
| [BridgeTransport.cpp](../../app/src/audio/BridgeTransport.cpp) | Play, stop, locate, metronome, previews; polling the playhead and meters |
| [BridgeRecording.cpp](../../app/src/audio/BridgeRecording.cpp) | Recording takes, live takes |
| [BridgeFreezing.cpp](../../app/src/audio/BridgeFreezing.cpp) | Frozen tracks in the engine, rendering a freeze, exporting |
| [BridgeReversing.cpp](../../app/src/audio/BridgeReversing.cpp) | Reversed copies of files, for reversed clips |
| [BridgeAudioDevice.cpp](../../app/src/audio/BridgeAudioDevice.cpp) | Opening the audio device, resets, its control panel, its events, `startAudio()` |
| [BridgeTypes.h](../../app/src/audio/BridgeTypes.h), [Waveform.h](../../app/src/audio/Waveform.h), [LiveTake.h](../../app/src/audio/LiveTake.h), [RenderTask.h](../../app/src/audio/RenderTask.h), [ReverseJob.h](../../app/src/audio/ReverseJob.h), [GateResponse.h](../../app/src/audio/GateResponse.h), [LimiterResponse.h](../../app/src/audio/LimiterResponse.h), [MultibandResponse.h](../../app/src/audio/MultibandResponse.h), [SpectralResponse.h](../../app/src/audio/SpectralResponse.h), [SaturatorResponse.h](../../app/src/audio/SaturatorResponse.h), [AmpResponse.h](../../app/src/audio/AmpResponse.h), [ErosionResponse.h](../../app/src/audio/ErosionResponse.h), [ChorusVoices.h](../../app/src/audio/ChorusVoices.h), [PhaserResponse.h](../../app/src/audio/PhaserResponse.h), [ReverbResponse.h](../../app/src/audio/ReverbResponse.h), [DisperserResponse.h](../../app/src/audio/DisperserResponse.h), [EqResponse.h](../../app/src/audio/EqResponse.h), [SampleSlices.h](../../app/src/audio/SampleSlices.h) | What the bridge hands the UI, as the application's own types (below); the Gate's, the Limiter's, Multiband Dynamics', the Spectral Compressor's, the Saturator's, the Amp's, Erosion's, the Chorus-Ensemble's, the Phaser-Flanger's, the Reverb's, the Disperser's and the EQ's curves and the Sampler's slices and snapping, the engine's own (for the editors) |
| [EngineDescs.h](../../app/src/audio/EngineDescs.h) | Model to engine descriptions (clips, notes); the application layer's own |
| [AudioSettings.h](../../app/src/audio/AudioSettings.h) | `AudioSettings` (QSettings), `audioThreads`/`setAudioThreads`, `disabledMidiInputs`/`setMidiInputDisabled`, `recordQuantize`/`setRecordQuantize`/`recordQuantizeChoices`, `defaultDriver`, `audioDrivers`, `kBufferSizes`, `kSampleRates` |
| [AudioFiles.h](../../app/src/audio/AudioFiles.h) | `recordingsFolder`, `takePath`, `freezeFolder`, `reversedFolder`, `reversedPath`, `makeFolder` (or why it can't be made), `floatWavHeader`, `writeFloatWav`, `isAudioFile`, `audioExtensions` |

### What the UI may include

The UI may not include the engine's headers ([architecture.md](../architecture.md#the-boundaries-checked)), so the
bridge's public headers include none: `EngineBridge.h` forward-declares the engine types it mentions, and what the
UI shows of the engine comes as application-layer types:

| Type | What it is |
|---|---|
| `Waveform` | a decoded file as the UI draws it: a handle on the engine's immutable `AudioSource` (frames, channels, sample rate, samples, min/max peaks at a few levels of detail), safe to keep and read from any thread, including the render thread |
| `LiveTake`, `LiveNote` | a take while it records: an audio take's peaks, or a MIDI take's notes so far |
| `MeterLevel`, `AudioFileInfo`, `AudioDeviceStatus`, `AudioDeviceCaps`, `AudioDeviceEntry` | meter readings, a file's header, the device running and what it offers (Q_GADGETs, so QML reads them as values) |
| `ProcessorParam`, `ProcessorDisplay`, `ParamGroup` | a processor's parameters as a device's editor shows them (with the engine's normalized mapping), the streams its own editor draws (meters, curves), and an owner's automatable parameters as `ParamSpec`s |
| `RenderTask` (`EngineRender`, `FreezeRender`), `ReverseJob` | a render or a reversed copy on a thread of its own, followed without waiting (below) |
| `gateKeyFilterDb()`, `gateKeyFilterUsesGain()`, `gateKeyFilterUsesQ()`, `gateFloorGain()`, `gateFloorIsSilent()`, `gateGainDb()`, `gateCloseDb()`, `gateDisplaySamples()` | the Gate's key EQ response (dB) from the engine's own filter and which of its types use the gain and the Q; the floor's gain, the gain the gate applies for how much it lets through, where an open gate closes again, and its displays' samples per value; so the key curve drawn is the one that keys the gate, and the gain shown the one it applies |
| `limiterLine()` (a `LimiterLine`), `limiterSoftKneeDb()`, `limiterSoftTopDb()`, `limiterMeterSamples()`, `limiterFloorDb()` | the Limiter's line (the Ceiling, or with Maximize the Threshold) and what to take off an output level to draw it in the line's domain; Soft Clip's knee (where it starts rounding off, where it reaches the line); its displays' samples per value and level for silence; from the engine's own maths, so the line drawn is where the device limits |
| `multibandGainDb()`, `multibandMinThresholdDb()`... `multibandMaxRatio()`, `multibandDisplaySamples()`, `multibandParseRatio()`, `multibandParseMs()` | Multiband Dynamics' gain change for a steady level once attack and release are done (a band's thresholds and ratios, Soft Knee, Amount), the engine's own, so what the graph shows a level getting is what plays; its thresholds' and ratios' ranges and its displays' rate, the engine's; and its ratios and times as typed ("1:4", "4:1", "0.5"; "250 ms", "1.5 s") |
| `spectralThresholdDb()`, `spectralBelowDb()`, `spectralFocusWeights()`, `spectralDisplayFrequencies()`, `kSpectralDisplayPoints`, `kSpectralPivotHz` | the Spectral Compressor's threshold and Below line (in its pink-referenced dB, tilted about 1 kHz), the share of the gain each frequency gets from the Focus band (the editor dims the band's outside by it), the frequencies its spectral displays' values stand for, their number per frame and the pivot (checked against the engine's), from the engine's own maths, so the line drawn is the threshold that plays |
| `saturatorCurve()`, `saturatorCurveAt()`, `saturatorSlope()` (of a `SaturatorShape`), `saturatorColorDb()`, `saturatorThresholdInput()` | the Saturator's shaping (Drive, the curve and Post Clip, in the engine's float arithmetic) and its slope at 0, Color's emphasis in dB, and where the Bass Shaper's curve leaves the straight line, by the engine's own functions, so the curve drawn is the shaping that plays and the EQ drawn is Color's |
| `ampToneResponseDb()`, `ampTransfer()`, `AmpTransferCurve` | the Amp's tone section (the model's tone stack with its make-up, and Presence) in dB, and its transfer for a 1 kHz tone through the engine's stages and filters (`AmpTransferCurve` makes it in two parts, so the sag moving alone costs a fraction), from the engine's own design, so what is drawn is what plays |
| `erosionBandMagnitude()`, `erosionBandEdges()`, `erosionModFrequency()`, `erosionExcursionMs()`, `erosionExcursionText()`, `erosionBlendWeights()`, `erosionRecentDb()` | the Erosion's noise band (its magnitude and its −3 dB edges) from the engine's own filter, the frequency the modulator plays, how far Amount moves the delay (and that as the graph reads it out), Noise Blend's equal-power weights, and how much its `erosion` display says it erodes now, so the band drawn is the filter that plays |
| `chorusVoices()`, `chorusCentreMs()`, `chorusSwingMs()`, `chorusLowestMs()`, `chorusHighestMs()`, `chorusVoicePhase()`, `chorusDelayMs()`, `chorusPeakDetuneCents()` (of a `ChorusLayout`); `chorusMinRate()`, `chorusMaxRate()`, `chorusDisplaySamples()` | the Chorus-Ensemble's voices: how many a side, where the delay sits and how far it swings, its range at Amount 100, each voice's place in the modulation's cycle, the delay at a phase, and the largest detune, by the engine's own maths, so the delays drawn are the delays that play; the Rate's range and the displays' samples per value, the engine's |
| `phaserResponseDb()`, `phaserCurvePoints()` (of a `PhaserCurve`), `phaserNotchFrequencies()`, `phaserCombNotchFrequencies()`, `phaserLfoValue()`, `phaserWaveIsRandom()`, `phaserSyncedRateHz()`, `phaserQ()`, `phaserCenterHz()`, `phaserDelayMs()`, `phaserFeedbackGain()`, `phaserWaveLabels()`, `phaserDisplaySamples()`, `phaserRanges()` | the Phaser-Flanger's response in dB (and as its editor draws it over its columns, a comb finer than a column as its highest and lowest), where the Phaser's and the comb's notches are, the LFO's shapes and synced rates, and the modulation's mappings, from the engine's own maths, so the curve drawn is what plays; and the engine's figures its editor needs (frames per display value, the parameters' ranges its drags hold to) |
| `reverbDecaySeconds()` (of `ReverbDecaySettings`), `reverbInputFilterDb()`, `reverbEarlyTaps()` (`ReverbTap`s), `reverbSpinPan()`, `reverbStereoWidth()`, `reverbDiffuseOnsetMs()` | the Reverb's decay time per frequency, its input filter's band, the early reflections at rest and where Spin swings them, Stereo Image's width and when the diffuse tail starts after the predelay, from the engine's own maths, so what is drawn is what plays |
| `disperserGroupDelayMs()`, `disperserTunedFrequency()`, `disperserPeakFrequency()` | the Disperser's group delay (ms) from the engine's own stages, where they are tuned (below Nyquist) and where the delay peaks, so the graph drawn is the delay that plays |
| `eqResponseDb()` | the EQ device's band response, computed by the engine's own filter design, so the curve drawn is the one that plays |

`BridgePrivate.h` and `EngineDescs.h` are the application layer's own.

## Start-up and shut-down

```
substation [song.gilproj]                                      ui/main.cpp
  ├─ SetCurrentProcessExplicitAppUserModelID("SUBstation.DAW")  (Windows: the taskbar groups under our icon)
  ├─ organisation and application name "SUBstation"             (where QSettings keeps the preferences)
  ├─ QGuiApplication; sub::ui::setUpApplication()
  ├─ sub::Engine
  ├─ sub::app::Session(engine) ─► Project, QUndoStack, ProjectEditor, Selection, EngineBridge(engine, project),
  │                               PluginIndex, BrowserController (its scan starts), ...; Session::wire()
  ├─ sub::ui::registerSession(&session); QQmlApplicationEngine loads Main.qml
  ├─ session.setOwnerWindow(the window's native handle)        (plug-in editors float above it)
  ├─ queued: session.start()            ─► EngineBridge::startAudio()
  ├─ queued: session.openProject(argv[1])   if a project was given
  ├─ app.exec()
  └─ session.shutdown()                 ─► a render running is aborted, transport and preview stop, editors close,
                                           the browser's threads stop, Engine::closeDevice(), EngineBridge::shutdown()
```

The session connects the bridge to the rest (the editor's hooks, messages, plug-ins, recording, previews): see
[session.md](session.md#wiring).

`EngineBridge::startAudio()` (from `Session::start()`, once the window shows):

1. `applyAudioThreads()`: the engine's render threads from the preferences.
2. `openMidiInputs()`: every MIDI input connected but those turned off.
3. Opens the saved `AudioSettings`. If the device can't run as saved (an ASIO driver on another clock, fewer outputs),
   it opens with the device's own settings (rate, buffer, channels at their defaults: "Using the device's own
   settings instead."); if the driver is gone, the system's default output with the saved buffer size ("Using the
   system default output instead."). If nothing opens, audio is off and the status line says to choose a device in
   *Options > Preferences*. The first launch has no saved settings, so it uses the system default output. On Linux
   with no sound server nothing opens: the application runs without audio and says so.

### Shut-down

`Session::shutdown()` closes the audio device, then `EngineBridge::shutdown()` forgets the plug-ins still waiting to
load (see [Opening a project](#opening-a-project)), closes every plug-in editor, waits for built-in devices' states to
finish restoring, removes every engine track (so plug-ins unload now, while the application is still whole, not
whenever the engine goes), closes the MIDI inputs and runs `Engine::idle(true)` once more (it destroys every removed
plug-in on the GUI thread; a normal `idle()` destroys only a few at a time). The engine and the project outlive the
bridge; its destructor waits for its thread pools.

## How the model is mirrored

### Ids

The bridge maps model ids to engine ids (`EngineBridge::Private`):

| Map | From → to |
|---|---|
| `trackIds` | model track id → engine track id (`kMaster` → `sub::Engine::kMaster`; the engine always has the master) |
| `chains` | chain key → engine chain id. A track's own chain is keyed by the track's id, a rack chain by the chain's id (ids are unique in the project) |
| `chainOwner` | chain key → the track it is on |
| `rackOfChain` | rack chain id → its rack's device id |
| `devices` | chain key → `[(device id, processor id or none)]` in order (none: a plug-in that didn't load, or waits to) |
| `pids`, `where` | device id → its processor; → the chain key its processor is in now |

`engineTrackId(track)`, `engineDeviceId(track, device)` (a device's processor if the device is on that track, in a rack
too, and loaded) and `engineChainId(chain)` (a rack chain's engine chain) give them out.

### Tracks, groups, returns, master

- Every track, group and return is an engine track; the master is the engine's own (`sub::Engine::kMaster`), and its
  devices, mixer and automation go the same way as a track's.
- `addEngineTrack` pushes, in order: mixer, input, frozen state, clips, devices, automation, then outputs (its own,
  and what is in it, back into it), sends (a return: every track's, since sends into it can exist only now), the
  inputs taken from it, and sidechains.
- **Outputs and groups**: the engine knows only where each track's output goes. `pushOutputs` sets every track's
  (and return's) output as `Track::output` says (`wantedOutput`): its group's engine track (by default) or the master,
  another track (`setTrackOutput`), a device's processor's sidechain (`setTrackOutputSidechain`; nowhere while that
  device's plug-in isn't there yet, or it has no sidechain input), or nowhere (`kNoOutput`). It has the engine take
  what goes into an audio track as that track's input (`setTrackInMonitored`: Ableton's Track In); groups and returns
  are buses. Changed routes go to the master first, then where they go, so that no step closes a cycle (a group
  moving into what was in it); one the engine refuses for now comes with the change in its way. `pushRoutes` pushes
  outputs, inputs and sidechains, in that order: after rearranging, a track's change, and device syncs (a device's
  processor is new, or an instrument a Pre FX tap is taken after).
- **Returns and sends**: a track's sends are engine sends into the returns' engine tracks, at the send's gain, before
  or after the fader. `pushSends` removes the sends going away first, then sets the others; one the engine refuses (a
  cycle with a send another track hasn't given up yet) is skipped and comes with that track's turn. A send automated
  without having been set yet is made, silent, so that its automation plays (`wantedSends`).
- **Inputs**: `pushInput` sets device channels (`setTrackInput`) or another track's output (`setTrackInputTrack`;
  `kMaster` for resampling; tapped where `Track::inputTap` says, as a sidechain's tap: `engineTap`), the monitoring mode, arming and a MIDI track's MIDI input (`setTrackMidiInput(engine id,
  enabled, device, channel)`). A source the engine refuses for now (a cycle another change hasn't undone yet) leaves
  the track with no input until that change comes. A track given ASIO input channels the device hasn't open makes the
  device open again with them too (`openInputs`), as it runs now, and saves that, so they open next time too.
- **Removing a track** removes its engine track (and its devices). The bridge then treats what went into it as going
  to the master, forgets sends into it, inputs from it, sidechains from it and its overrides; the model's own changes,
  in the same undo step, follow.

### Clips and notes

- Audio tracks: `setTrackClips(engine id, clip descriptions)`. A description keeps positions in beats and seconds (the
  engine converts them to samples at the current tempo and rate): path, start beat, duration, offset, gain, pan, warp
  (`Clip::isWarped`), segment BPM, warp mode (`sub::WarpMode`, in `kWarpModes` order) and `transpose + detune / 100`
  semitones. Every clip's file is requested for decoding first. A deactivated clip (`Clip::muted`) isn't among them
  (`clipDescs()`), but its file is decoded all the same (its waveform shows).
- MIDI tracks: the track's clips are flattened into the notes they play (`Clip::heardNotes()` in timeline beats: none
  of a deactivated clip, nor deactivated notes) and set with `setTrackNotes`. See [engine/midi.md](../engine/midi.md).
- A frozen track plays its frozen audio instead, as clips into the render: its segments (`Freeze::playing()`: all of
  it from beat 0 until a time selection over it is edited, then what the edits left, where they put it; see
  [model.md](model.md#freezing)); a MIDI track's notes go. Segments changing come as `clipsChanged` of the frozen
  track (or group), so `pushClips` plays them.
- Previews: while a clip is dragged (moved or trimmed), `previewClips(track → clips, frozen track → segments)` pushes
  what the drag would make of those tracks' clips (or notes) instead of the model's, and of frozen tracks' segments
  (`MovedRange::frozen`), so they are heard where they are going; the model changes once, when the drag ends.
  `endClipPreview()` pushes the model's clips again for every track previewed (the drop changed them, or not). A
  frozen track's clips aren't previewed (it plays its frozen audio), only its segments.

### Devices and racks

`syncDevices(track)` runs on `devicesChanged` (and when a track is added). It walks the track's device tree top down
(`place`, `placeRack`):

- A device already in that engine chain keeps its processor.
- A device whose processor is in another chain (of this track or another: it moved, into or out of a rack, or to
  another track) is **taken over** (`takeOver`): one `Engine::moveProcessor`, nothing loads again, so a plug-in keeps
  its state and its open editor. A rack moves with its chains and everything in them. A processor moving to another
  track gives up its sidechain in the engine until it is there.
- A rack chain that is now another rack's gets a new engine chain, and its processors move into it.
- A new device gets a processor (`createProcessor`): a rack (`addRack`, refused past the nesting limit with a status
  message), a plug-in (`loadPlugin`) or a built-in device (`addBuiltinProcessor`, then its `params`, then its `state`
  in the background). A built-in kind the engine doesn't know (a preset or project from a later version) gets none,
  and an entry in `pluginErrors`, as a missing plug-in does.
- Devices that left a chain and aren't anywhere else on this track are **disposed** (`dispose`): if the device is now
  on another track, that track is synced first so it takes the processor over; otherwise the processor goes
  (`forgetProcessor`), a rack with what is still in it.
- Then the chain orders (`setChainOrder`, `setRackChainOrder`), automation (the devices' envelopes go to their
  processors), editor titles, on/off (`pushEnabled`), rack chain faders and sidechains are pushed, and
  `devicesLoaded(track id)` is emitted.

A track isn't synced again from inside its own sync (`syncing`). A frozen track has no devices as far as the bridge is
concerned (see [Freezing](#freezing)).

### Plug-ins

- **Loading**: `pluginPath(ref)` is the saved path if it exists, else where the scan found that class id (the known
  plug-ins, from `setKnownPlugins`). Missing, or failing to load, the device keeps its place with no processor,
  `pluginErrors()` says why (by device id), and the status line shows it. `setKnownPlugins` (after every scan) tries
  those devices again.
- **State**: a plug-in's state is restored from what it had when its device went away (kept when a plug-in device is
  deleted, or its track, so undo brings it back as it was), else from `Device::state` (base64 `.vstpreset`).
  `storePluginStates()` copies every loaded plug-in's state (or those of some devices) back into the model
  (`Project::storePluginState`), for saving, copying devices, duplicating tracks and saving presets; it also records a
  moved plug-in's new path. `pluginState(track, device)` reads one directly; `applyPluginState` gives one a
  `.vstpreset` read from a file (the caller records the undo step).
- **Parameters**: a model parameter change goes to the plug-in (`setProcessorParam`) unless the plug-in already has
  that value (an edit made in its own editor). Parameter ids, `ParamInfo`s and `ParamSpec`s are cached per processor
  and dropped when the plug-in reports that its parameter list changed.
- **Editors**: `openPluginEditor`, `closePluginEditor`, `requestPluginEditor`, `isPluginEditorOpen`,
  `closeAllEditors`. Only the selected track's editors show (`showPluginEditors(track)`): the others are hidden but
  keep running, at most `kMaxHiddenEditors` (8); beyond that the ones hidden longest close, and open again where they
  were when their track is shown. The window `setOwnerWindow` names (the main window's native handle) owns the editor
  windows so they float above it. Titles are `"<plug-in> - <track>"`, updated when the track is renamed. Editors are
  windows on Windows only; elsewhere plug-ins show none.
- **Reports** (`dispatchProcessorEvents`, from `Engine::takeProcessorEvents()`):

  | Engine event | Bridge |
  |---|---|
  | `ParamEdited` (editor open) | `pluginParamEdited(track, device, param, value, old, gesture)`: the session makes it an undo step, one per knob drag |
  | `ParamTouched` (editor open) | `pluginParamTouched`: its automation lane shows |
  | `ParamEdited`/`ParamTouched` with no editor open | not the user (a plug-in restoring its state reports edits): `pluginParamsChanged` only |
  | `ParamsChanged`, `LatencyChanged` | `pluginParamsChanged` |
  | `ParamInfoChanged` | caches dropped, automation pushed again, `pluginParamsRebuilt` |
  | `EditorClosed` | `pluginEditorChanged` |
  | `EditorRequested` | shown with its track, like any editor |
  | `StateDirty` | `pluginStateDirty`: the project has changes no edit shows |

- **Re-entrancy**: a plug-in (or a driver) may run a message loop inside a call (a licence dialog, a control panel)
  that calls back into the UI. Around such calls the bridge counts `busy` (a `BusyScope`: it counts down however the
  call ends), and while it is above 0 it doesn't dispatch plug-in reports or device events, nor load plug-ins that
  wait.

See [engine/plugins.md](../engine/plugins.md) for the engine side.

### Built-in devices' state

A built-in device's `state` (a sampler's sample) is restored on a one-thread pool, so loading a sample never holds up
the window, and states are restored in the order they were set (the last one wins). The engine swaps the sample in
without stopping the audio. A failure goes to the status line. `waitForDeviceStates()` waits for them all; renders
wait for them first. See [engine/devices.md](../engine/devices.md).

### Sidechains

`pushSidechains` (after device syncs, outputs, track changes) works out every device's wanted sidechain
(`wantedSidechain`) as (source engine track, `sub::SidechainTap`, tap processor):

- `kPostFader` → `SidechainTap::PostFader`; `kPreFader` → `PreFader`;
- `kPreFx` → `PreFx`, except on a MIDI track whose first device is an instrument (or instrument rack): after that
  instrument (a MIDI track's own audio is its instrument's);
- a device id → after its processor (`AfterDevice`); while that device isn't on the source, `PreFader`.

None for a device without a sidechain input (`processorInfo(...).hasSidechain`), a source the engine hasn't, or the
master. Changing ones are cleared first (`clearProcessorSidechain`), then the rest set; one the engine refuses for now
(a cycle with a route another change hasn't undone yet) comes with that change. `hasSidechainInput(track, device)`
tells the device view whether to show the button.

### Opening a project

On `reset` (a project opened, or a new one) `onReset` builds the engine's tracks, clips, mixers and built-in devices at
once, but a plug-in device found where it was (or by its id) doesn't load then: while `deferring`, `loadPlugin` hands
it to the loading queue ([BridgeLoading.cpp](../../app/src/audio/BridgeLoading.cpp)), which keeps its device id in
`pendingPlugins`. Until its turn the device is in the engine as a missing plug-in is: in its chain with no processor.
A missing plug-in ("not installed") is said at once.

`loadNextPlugin`, on a single-shot timer, takes the next device that waits and loads it: it takes the device out of the
chain the bridge has for it (wherever it is by then: moved to another track or into a rack, it loads there) and syncs
that track's devices, so the device is new to its chain and gets its processor, state, parameters, automation and
sidechain as a device added does (and `devicesLoaded` rebuilds the device view). One a turn of the event loop,
`kPluginGapMs` (20 ms) apart, so the window goes on between them (it paints, takes the mouse and keys); not while a
plug-in's call runs a message loop (then `kPluginRetryMs` later) or a chain is being synced. A device that went
meanwhile is skipped; one that came back (undo) has loaded already, as any device added.

- `pluginsLoading(loaded, total)` reports the progress (the session's `pluginsLoadingText`, in the title bar), and
  `(0, 0)` once all are loaded.
- `prioritizePlugins(track)` puts a track's first (the session calls it with the selected track);
  `requestPluginEditor` loads its device's plug-in now (`loadPluginNow`); `loadPendingPlugins()` loads every one now.
- `pluginPending(device)` says a device's waits (the device view says it is loading), `pluginsPending` how many do.
- Renders wait for them: `waitForDeviceStates()` loads them (and waits for built-in devices' states);
  `devicesReady()` says, without waiting, whether nothing is left (the session's renders poll it, so the timer goes on
  loading them meanwhile).
- Saving meanwhile keeps a waiting device's state as it was in the file (its model `state` hasn't changed).

Plug-ins load on the GUI thread all the same: plug-ins must be created and set up there (VST3 says so, and JUCE
plug-ins take the thread that creates them for their message thread), and some hang when loaded on another. See
[engine/plugins.md](../engine/plugins.md#threads-and-the-engine-lock).

### Freezing

`renderFreeze(track)` renders the track's signal before its fader from beat 0 to the arrangement's end (and on for up
to `kFreezeTailSeconds`, while it sounds) into `freezeFolder()` (the project's *Freeze* folder, or one in the
recordings folder), decodes it at once (so it plays without a gap) and returns the `Freeze`. It is
`startFreeze(track)` (which starts the engine's `startTrackRender()` and returns a `FreezeRender`: the track, the
engine's `RenderJob`, the tempo) and `finishFreeze(render)` (the `Freeze`, or none if it was cancelled; `EditError` if
it failed), waited for; the session calls those two itself, showing the render's progress between them
([session.md](session.md#renders-in-the-background)). `discardFreeze` forgets a render no track plays and deletes its
file. While a render runs, `pollDevice` leaves the device's events for later: it can't be reopened then.

On `freezeChanged` (`onFreezeChanged`): the plug-ins' states go into the model (so a frozen track saves them), the
engine track is frozen (`setTrackFrozen`), its clips become the frozen audio's segments (a MIDI track's notes go), and
`syncDevices` takes its processors away: the bridge sees a frozen track as having no devices, so its devices go as if
deleted, their plug-ins' states kept, and come back on unfreezing. The tracks in a frozen group keep their processors;
the engine just doesn't render them. Frozen tracks don't record.

`startExport(path, start, end, bitDepth)` is the other render in the background: the arrangement (the loop, a time
selection) into a WAV file of 16, 24 or 32-bit float, through `Engine::startExport`; `startExport(path, start, end,
format)` takes an `AudioExportFormat` ([BridgeTypes.h](../../app/src/audio/BridgeTypes.h)): a WAV file of `bitDepth`, or
an MP3 file at `bitrate` kbps. `finishExport` gives the frames written (none if cancelled).

### Reversing

A reversed clip plays a reversed copy of its file, named `name R.wav` (`reversedPath`, numbered if taken) in
`reversedFolder()` (the project's *Reversed* folder, or one in the recordings folder):

- `reversedCopy(path)`: the copy there is already, if any: one made this session (forgotten on reset), or one a clip
  of the project plays (its `reversedFrom` is `path`: saved with the project), so reopening a project doesn't make a
  second one. Not decoded yet, it is asked for.
- `startReversed(path)` starts a `ReverseJob`: the decoded source (it must be decoded: otherwise `EditError`, with a
  message for the user) written backwards as a 32-bit float WAV, `kReverseChunk` frames at a time from its end, on a
  thread of its own, then decoded there. It is a `RenderTask` (progress, done, `cancel()`), so the session's renders
  follow it as they follow the engine's; cancelled or failed, its file goes. A `ReverseJob` is made, then started
  (`start()`, which `startReversed` calls at once, as the engine starts its `RenderJob`s): cancelled before that, it
  writes none of the copy (the tests' way to cancel one deterministically: a short copy can be written before a cancel
  reaches it); finished, it is started if it wasn't.
- `finishReversed(path, job)`: the copy's path and length in seconds (decoded, so it plays without a gap, and
  remembered for `path`), none if cancelled, or `EditError`.
- `renderReversed(path)` is the three of them, waited for.

The session's reversing (R) turns clips to it with `ProjectEditor::reverseRange`
([session.md](session.md#the-arrangements-actions-arrangementactions)).

### Renders in the background: RenderTask

`RenderTask` ([RenderTask.h](../../app/src/audio/RenderTask.h)) is how the UI follows a long job without waiting
for it, and without nested event loops: `EngineRender` (an export), `FreezeRender` (a track's freeze) and `ReverseJob`
each run on a thread of their own and say how far they got (`progress`, 0..1), whether their thread has ended
(`done`), and can be cancelled (`cancel()`: their file goes). While it runs a task looks at itself every `kPollMs`
(30 ms) on the thread that made it and emits `progressChanged` when it got further and `ended` when its thread ended;
its owner then finishes it (`finishExport`, `finishFreeze`, `finishReversed`), which no longer waits. Meanwhile (a
render of the engine's) live output is silent, and the project as it was when the render started is what renders.

### Settings and transport

`pushSettings` sets the engine's tempo, time signature and loop. Transport: `play`, `stop` (also ends a recording),
`locate(beat)`, `position`, `isPlaying`, `isCountingIn`, `setMetronome`. Previews: `previewFile` (decodes first; a
later preview or stop cancels a preview still loading), `stopPreview`, `previewNote(track, pitch, velocity)` (velocity
0 releases). Audio threads: `applyAudioThreads`, `chooseAudioThreads(n)` (saved; 0 when it is the engine's default),
`audioThreads`, `defaultAudioThreads` (one per core but one).

QML reads the transport and the device through properties: `position`, `playing`, `countingIn`, `recording`,
`metronome` (read and write), `cpuLoad`, `sampleRate`, `deviceStatus`, `deviceCapabilities`, `scopeWritten` (with
`scopeSamples(n)`, the oscilloscope's feed) and `pluginsPending`.

## Automation and overrides

- `pushAutomation(owner)` sends every envelope of an owner to the engine (`setTrackAutomation(engine track, lanes)`),
  except overridden ones. Lanes are `sub::AutomationLaneDesc{processorId, param, points}`: processor 0 for the mixer
  (`volume`, `pan`, `send:<engine return track id>`), a device's processor and its parameter id, or a rack's
  processor and `chain:<engine chain id>:volume|pan` for a chain fader (`engineLane`). Targets whose device isn't
  loaded are left out. It emits `automationStateChanged(owner)`.
- A target whose envelope stops playing (deleted, or overridden) goes back to the value the model holds for it
  (`pushOwnValue`: the mixer is pushed again, a device parameter is set, a device's on/off pushed again; sends and
  chain faders keep their own level in the engine).
- Switches: a track's activator plays as the lane `on` of its mixer (the engine then leaves its mute out); a device's
  on/off as the lane `device:on` of its processor, and `pushEnabled` switches on in the engine every device whose
  switch's automation plays (the lane switches it), whatever the model says (`pushAutomation` calls it).
- **Overrides**, as in Ableton: changing an automated target by hand (a track's volume, pan or mute, a send level, a
  chain's fader, a device parameter or its on/off: the bridge sees the model's value change, `mutes` and
  `ownEnabled` keeping what it was) calls `overrideAutomation(owner, key)`, which
  stops sending that envelope. `reEnableAutomation(owner)` (no owner: everywhere) plays them again. `isAutomated`,
  `isOverridden` and `hasOverrides` drive the red dots, grey envelopes and the Re-Enable button (the session's
  `automationOverridden`). Overrides are bridge state: not saved, not undone, cleared on `reset`.
- Describing parameters to the UI: `paramGroups(owner)` (the mixer with its sends, then each device, racks included),
  `deviceParamSpecs`, `mixerSpecs`, `paramSpec`, `canAutomate`. Plug-in parameters that aren't automatable, are
  hidden or read-only are left out. `ownValue(owner, key)` is a target's value set by hand (a plug-in's read from the
  plug-in), `currentValue(owner, key, beat)` what it is at a beat while automated. `deviceParamInfo` describes a
  device's parameter as the engine does (the editor's macros map through it). A rack's macros are targets too, and
  their automation plays as the parameters mapped to them (`macroEnvelopes()`: each one plays the macro's envelope
  over its range, instead of its own; `currentValue` follows it): see [engine/automation.md](../engine/automation.md#racks-and-macros).
  `pushAutomation` keeps what it pushed of them (`macroMoved`), and devices changing push automation again when that
  changes (a mapping or a range).

See [engine/automation.md](../engine/automation.md).

## Async decoding

Audio files (`audioExtensions()`: `.wav`, `.wave`, `.flac`, `.mp3`) are decoded at the engine's rate in a
`QThreadPool` of two threads (`Engine::loadSource`). `requestSource(path, then)` decodes a file once (keyed by
`pathIdentity(path)`, [model/Paths.h](../../app/src/model/Paths.h): absolute and clean, in any case on Windows, so
two spellings of one file share one source), queues
`then` callbacks while it loads, and emits `sourceReady(path)` or `sourceFailed(path, message)` on the GUI thread.
`waveform(path)` gives the decoded file as a `Waveform` (null while it isn't decoded); `isLoading`, `loadError`,
`fileInfo` (the header only, cached, `sub::AudioSource::probe`). A file that couldn't be decoded is said in the status
line, but one that isn't there at all: the File Manager lists missing files ([session.md](session.md#the-file-manager-and-hot-swaps-filemanager-hotswap)),
and a project opening with some says how many once. After a sample-rate change every source is decoded
again (`refreshSources`). On `reset` the bridge forgets sources the new project doesn't use and calls
`Engine::releaseUnusedSources()`. See [engine/warp.md](../engine/warp.md).

## Polling

- **Playhead**, every `kPositionPollMs` (16 ms, about 60 times a second): the position, playing and counting in;
  `positionChanged`, `transportChanged` and `countingInChanged` only when they changed. Also polled at once after
  play, stop and locate.
- **Meters**, every `kMeterPollMs` (33 ms, about 30 times a second), in this order:
  1. `pollRecording`: live takes' progress (peaks, or a MIDI take's notes so far) into `liveTakes()`,
     `recordingUpdated`; if the engine stopped recording on its own (a locate, a device change), the recording ends.
  2. `Engine::takeMeters()` into `meters()` (track id or `kMaster` → `MeterLevel`) and `chainMeters()` (rack chain id
     → `MeterLevel`); `metersUpdated`.
  3. `pollPlugins()`: `Engine::idle()` (the engine's GUI-thread housekeeping: freeing retired snapshots, destroying
     removed plug-ins a few at a time, plug-ins' main-thread work) and, unless busy, the plug-ins' reports.
  4. Unless busy, `pollDevice` (not while a render runs in the background).

None of these wait for a render in the background (`Engine::isRendering`): it renders without the engine's lock.

## Device events

`pollDevice` takes one event per poll from `Engine::takeDeviceEvent()`:

| Event | Bridge |
|---|---|
| `"stopped"` | status: the device stopped, choose one; `deviceChanged` |
| `"rerouted"` | status: output rerouted to another device; `deviceChanged` |
| `"reset"` | `resetDevice()`: `Engine::reopenDevice` opens the driver again with the buffer size and rate it now has (an ASIO driver's settings changed in its control panel, or its clock) |
| `"latency"` | `deviceChanged` (the status line shows the new latencies) |

`openDevice(settings)`, `resetDevice()` and `closeDevice()` go through `changeDevice`, which counts `busy` (a driver
may show a dialog), decodes sources again if the rate changed and emits `deviceChanged`; `showDeviceControlPanel()`
counts `busy` too. `openDevice` passes the owner window's handle (ASIO drivers want one). Closing the device ends a
recording. `driverTypes()`, `listDevices(driver)`, `deviceStatus()`, `deviceCapabilities()` and `inputNames()` describe
what there is. See [engine/audio-devices.md](../engine/audio-devices.md).

## Recording

- `recordTargets()`: armed tracks with an input (`Track::hasInput`), not frozen. `startRecording(countInBeats)` returns
  why it can't ("Arm a MIDI track, or an audio track that has an input, to record."; no device open; the folder can't
  be made) or `""`. It opens missing ASIO inputs, makes the recordings folder, names each audio take
  `takePath(folder, takeName(track), now)`: the track's name (characters Windows forbids replaced; a track named by
  what it holds: that, without its number, so its takes keep it so) and the local time,
  numbered if taken; MIDI takes have no file.
- `recordingsFolder(project)`: the project's `Recordings` folder once it is saved; else `SUBSTATION_RECORDINGS` if set
  (the tests use it), else `SUBstation/Recordings` in the user's Music folder.
- `liveTakes()` holds a `LiveTake` per track while it records, which the arrangement draws.
- `stopRecording()` ends it (playing goes on), reports takes' errors and dropped samples, and emits
  `takesRecorded(takes)`: `RecordedTake`s, start and length in seconds, a MIDI take's notes in seconds. The session
  adds them as clips in one undo step, quantized to `recordQuantize()`, and selects them.

See [engine/recording.md](../engine/recording.md) and [guide/recording.md](../guide/recording.md).

## MIDI input

`openMidiInputs()` opens every MIDI input connected except those turned off in the preferences, closes those turned
off or gone, and reports an input that can't be opened once (`midiErrors()`). `setMidiInputEnabled` saves the choice
and applies it. `midiInputs()` lists the inputs; `midiInputChoices()` adds `kComputerKeyboard` ("Computer Keyboard"),
the input the computer MIDI keyboard plays into, always there; `sendMidi(message, device)` plays a message as if that
input sent it (`Engine::sendMidiInput`; dropped while no device runs). On Linux the engine has no MIDI devices: only
the computer keyboard plays. See [engine/midi.md](../engine/midi.md).

## Settings storage

Preferences are per user, in `QSettings` under the organisation and application name "SUBstation" (on Windows, Qt
keeps them in the registry under `HKEY_CURRENT_USER\Software\SUBstation\SUBstation`; on Linux in
`~/.config/SUBstation/SUBstation.conf`); earlier versions' settings are read as they are. The tests use
"SUBstation Tests" and a folder of their own, so they never touch the user's.

| Key | Holds | Code |
|---|---|---|
| `audio/driver` | `"WASAPI"` or `"ASIO"` on Windows (`"System"` elsewhere: `defaultDriver()`) | `AudioSettings` |
| `audio/device` | device or driver name; empty: the system default (WASAPI), the first driver (ASIO) | |
| `audio/sample_rate` | 0: the rate the device runs at | |
| `audio/buffer_frames` | 0: the device's preferred size; default 256 | |
| `audio/exclusive` | WASAPI exclusive mode (`"true"` / `"false"`) | |
| `audio/output_channels` | `"2,3"`: the pair the master plays on; empty: the first two | |
| `audio/input_channels` | ASIO inputs to open; none until something records | |
| `audio/threads` | render threads; 0: the engine's default (one per core but one) | `audioThreads()` |
| `midi/disabled_inputs` | MIDI inputs turned off (every other input is used, so a new one just plays) | `disabledMidiInputs()` |
| `record/quantize` | record quantization grid in beats; 0: as played (`recordQuantizeChoices()` lists the choices) | `recordQuantize()` |

`AudioSettings::load()` falls back to the default driver for an unknown one; `save()` writes them all. The audio
preferences ([session.md](session.md#preferences)) save them when a change opens successfully, so the program opens the
same driver next time. Other parts keep their own keys: the session's (recent files, the last folder, the count-in:
[session.md](session.md#settings)), the browser's (places, sort: [browser.md](../browser.md)), the plug-in folders
([plugin-scanner.md](plugin-scanner.md)) and the UI's window state. The user-facing side is in
[guide/audio-setup.md](../guide/audio-setup.md).

## Signals

| Signal | Meaning |
|---|---|
| `sourceReady(path)`, `sourceFailed(path, message)` | decoding finished |
| `positionChanged(beat)`, `transportChanged(playing)`, `countingInChanged(countingIn)` | playhead polling |
| `metronomeChanged(enabled)` | the metronome was switched |
| `metersUpdated()` | `meters()` and `chainMeters()` were refreshed |
| `deviceChanged()` | the audio device opened, closed, changed or reported new latencies |
| `statusMessage(text)` | for the status line |
| `pluginParamEdited`, `pluginParamTouched`, `pluginParamsChanged`, `pluginParamsRebuilt`, `pluginEditorChanged`, `pluginStateDirty` | plug-in reports (above) |
| `devicesLoaded(track id)` | a track's processors were (re)created |
| `pluginsLoading(loaded, total)`, `pluginsPendingChanged()` | a project's plug-ins loading |
| `automationStateChanged(owner)` | which envelopes play or are overridden changed |
| `recordingChanged(recording)`, `recordingUpdated()`, `takesRecorded(takes)` | recording |

## Invariants

- Only the bridge talks to the engine about the project; the model never does.
- A device's processor lives as long as the device is in the project's chains: reordering or changing the chain around
  it, grouping it into a rack or moving it to another track never reloads it.
- Routing changes are pushed in an order where no single engine call closes a cycle: routes going away first. A call
  the engine refuses for now is retried when the change in its way arrives.
- The bridge doesn't hold model objects across calls; it reads the project by id (the project holds its tracks by
  value: see [model.md](model.md#values-held-references-handed-out)).

## Extending it

- **A new model signal**: connect it in the `EngineBridge` constructor (inside `guarded()`), push only what changed,
  and keep the last value pushed (in `Private`) if the engine call is expensive or must be ordered.
- **A new engine call**: add it to `Engine.h` ([engine/README.md](../engine/README.md#extending-it)) and call it from
  the bridge's file of its area. If the UI must show what it returns, give it an application-layer type in
  `BridgeTypes.h` (or a header like it), not the engine's.
- **A new preference**: a QSettings key with a function in `AudioSettings.h`, read where it applies, and a control in
  the preferences (`AudioPreferences`, [session.md](session.md#preferences)).

## Gotchas

- Plug-in reports and device events are not dispatched while `busy`: a plug-in's or driver's message loop must not
  change the chains under a call in progress.
- A plug-in that reports edits while restoring its state (a paste, a duplicate, an undo) would fill the undo stack;
  edits only count while its editor is open.
- `Device::state` of a plug-in is stale until `storePluginStates()`; read `pluginState()` for the current one.
- Overrides are lost on `reset` (opening a project) and are never saved.
- Decoding is keyed by `pathIdentity`, so two clips of one file share one source.
- `source(path)` hands out the engine's `AudioSource` (forward-declared): only application-layer code that includes
  the engine may use it; the UI uses `waveform(path)`.
- The timers' intervals are 16 and 33 ms; "60 and 30 times a second" are approximate.

## Tests

- [test_bridge_tracks.cpp](../../tests/app/test_bridge_tracks.cpp): clips and notes as the engine has them, groups as
  buses, returns and sends, inputs from tracks, the master's mixer, the settings, a drag's preview, overrides.
- [test_bridge_devices.cpp](../../tests/app/test_bridge_devices.cpp): racks keeping each device's processor as they
  are made, undone, moved and deleted; chain automation and overrides; macros moving plug-in parameters, and their
  automation moving what is mapped to them (over its range, overridden, re-enabled); sidechains;
  presets.
- [test_bridge_plugins.cpp](../../tests/app/test_bridge_plugins.cpp): plug-in loading (after a project opens too),
  missing and moved plug-ins, editors, edits for undo, automating plug-in parameters.
- [test_bridge_freeze.cpp](../../tests/app/test_bridge_freeze.cpp): frozen tracks in the engine (their frozen audio
  where a time selection moved it, and where a drag would take it), renders in the background, reversed copies.
- [test_bridge_recording.cpp](../../tests/app/test_bridge_recording.cpp): what records, inputs and monitoring in the
  engine, takes.
- [test_bridge_settings.cpp](../../tests/app/test_bridge_settings.cpp): `AudioSettings` and the other keys, the files'
  folders and names, `writeFloatWav`, the EQ's curve, device status, the scope, MIDI inputs, `startAudio`.
- [test_session_engine.cpp](../../tests/app/test_session_engine.cpp), [test_session_keyboard.cpp](../../tests/app/test_session_keyboard.cpp),
  [test_engine_render.cpp](../../tests/engine/test_engine_render.cpp) and the engine's MIDI input tests: the engine side
  of the same.

See [testing.md](../testing.md) for the suite and its fixtures.
