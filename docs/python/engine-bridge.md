# Engine bridge and start-up

The engine bridge keeps the C++ engine (`substation._engine`) in step with the
[project model](model.md), and feeds the UI with what the engine knows (playhead, meters,
plug-in reports, device events) through Qt signals. It lives in
[src/substation/audio/engine_bridge/](../../src/substation/audio/engine_bridge), next to
the persistent audio and MIDI preferences in
[audio/settings.py](../../src/substation/audio/settings.py). This page also covers how the
application starts: [app.py](../../src/substation/app.py),
[\_\_init\_\_.py](../../src/substation/__init__.py) and
[\_\_main\_\_.py](../../src/substation/__main__.py).

## Overview

```
              Project (Qt signals)                               EngineBridge                    ge.Engine (C++)
  track_inserted / return_inserted ────► _add_engine_track ──────► add_track, track_chain, ...
  track_removed / return_removed   ────► _on_track_removed ──────► remove_track
  track_changed                    ────► mixer, input, sends ────► set_track_gain/pan/mute/solo, set_track_input...,
                                                                    set_track_send / remove_track_send
  tracks_arranged                  ────► _push_outputs ──────────► set_track_output
  clips_changed                    ────► _push_clips ────────────► set_track_clips / set_track_notes (+ decoding)
  devices_changed                  ────► _sync_devices ──────────► add_*_processor, move_processor, set_chain_order,
                                                                    add_rack / add_rack_chain, set_processor_sidechain...
  chain_changed                    ────► _push_chain_mixers ─────► set_chain_gain/pan/mute/solo
  device_param_changed             ────► _push_device_param ─────► set_processor_param
  device_state_changed             ────► _push_device_state ─────► set_processor_state
  settings_changed                 ────► _push_settings ─────────► tempo, set_time_signature, set_loop
  automation_changed               ────► _push_automation ───────► set_track_automation
  reset                            ────► _on_reset: everything again

  QTimer 16 ms  ─► _poll_position ──► position_changed, transport_changed
  QTimer 33 ms  ─► _poll_meters ────► _poll_recording, take_meters → meters_updated,
                                      poll_plugins (engine.idle, processor events), _poll_device
  QThreadPool(2) ─► _LoadTask:  engine.load_source(path)       ─► source_ready / source_failed
  QThreadPool(1) ─► _StateTask: engine.set_processor_state(...) (built-in devices' states)
```

- Everything in the bridge runs on the **Qt main thread**, except decoding audio files and
  restoring built-in devices' states, which run in small thread pools (the engine releases
  the GIL while it works).
- The bridge never waits on the audio thread: the engine publishes immutable snapshots and
  the bridge reads atomics and queues (see [architecture.md](../architecture.md) and
  [engine/README.md](../engine/README.md)).
- The bridge only pushes what changed: it keeps the last value it gave the engine for
  mixers, inputs, outputs, sends, sidechains, device on/off and rack chain mixers, and
  compares before calling.

## Files

| File | What it holds |
|---|---|
| [audio/engine_bridge/\_\_init\_\_.py](../../src/substation/audio/engine_bridge/__init__.py) | `EngineBridge`: its signals, state and start-up (`__init__`), `shutdown`; made of one mixin per module below, and re-exports their public names |
| [engine_bridge/tracks.py](../../src/substation/audio/engine_bridge/tracks.py) | `TrackSync`: engine tracks, mixers, outputs, sends, clips, tempo and loop; `clip_desc`, `note_descs` (model → engine descriptions) |
| [engine_bridge/inputs.py](../../src/substation/audio/engine_bridge/inputs.py) | `InputSync`: tracks' audio and MIDI inputs and monitoring, the MIDI inputs open; `COMPUTER_KEYBOARD` |
| [engine_bridge/devices.py](../../src/substation/audio/engine_bridge/devices.py) | `DeviceSync`: devices' processors in every chain, racks, sidechains, parameters, states; the state task |
| [engine_bridge/plugins.py](../../src/substation/audio/engine_bridge/plugins.py) | `PluginHost`: plug-ins' states, editors (`MAX_HIDDEN_EDITORS`) and events |
| [engine_bridge/parameters.py](../../src/substation/audio/engine_bridge/parameters.py) | `ParameterSync`: automation pushed to the engine, overrides, `ParamSpec`s for the UI |
| [engine_bridge/sources.py](../../src/substation/audio/engine_bridge/sources.py) | `SourceLoader`: decoding audio files in a thread pool; `is_audio_file`, `AUDIO_EXTENSIONS` |
| [engine_bridge/transport.py](../../src/substation/audio/engine_bridge/transport.py) | `Transport`: play, stop, locate, metronome, previews; polling the playhead and meters |
| [engine_bridge/recording.py](../../src/substation/audio/engine_bridge/recording.py) | `Recorder`: recording takes; `LiveTake` (a take while it records), `recordings_folder`, `take_path` |
| [engine_bridge/freezing.py](../../src/substation/audio/engine_bridge/freezing.py) | `FreezeSync`: frozen tracks in the engine, `render_freeze`; `freeze_folder` |
| [engine_bridge/audio_device.py](../../src/substation/audio/engine_bridge/audio_device.py) | `AudioDevice`: opening the audio device, resets, its control panel, its events |
| [audio/settings.py](../../src/substation/audio/settings.py) | `AudioSettings` (QSettings), `audio_threads`/`set_audio_threads`, `disabled_midi_inputs`/`set_midi_input_disabled`, `record_quantize`/`set_record_quantize`, `RECORD_QUANTIZE`, `DRIVERS`, `BUFFER_SIZES`, `SAMPLE_RATES` |
| [app.py](../../src/substation/app.py) | `main()`: the QApplication, the engine API check, the main window, start-up and shut-down |
| [\_\_init\_\_.py](../../src/substation/__init__.py) | `__version__`, `APP_NAME`, `ENGINE_API`, `engine_mismatch()` |
| [\_\_main\_\_.py](../../src/substation/__main__.py) | `python -m substation` runs `main()` |

## Start-up

```
python -m substation [song.gilproj]
  __main__.py ─► app.main(argv)
    ├─ SetCurrentProcessExplicitAppUserModelID("SUBstation.DAW")   (taskbar groups under our icon)
    ├─ QCoreApplication organisation and application name "SUBstation"  (QSettings location)
    ├─ QApplication, theme.apply
    ├─ engine_mismatch()?  ─► print to stderr, critical message box, exit 1
    ├─ ge.Engine()
    ├─ MainWindow(engine)   ─► Project, QUndoStack, ProjectEditor, EngineBridge(engine, project)
    ├─ window.show()
    ├─ QTimer.singleShot(0, window.start_audio)
    ├─ QTimer.singleShot(0, open_project(argv[1]))   if a project was given
    ├─ app.exec()
    └─ engine.close_device(); window.bridge.shutdown()
```

`MainWindow` (see [ui/README.md](../ui/README.md)) imports after the `QApplication` exists.
It connects the bridge to the UI: `status_message` to the status line, the editor's
macro hooks to `device_param_info` and `own_value`, `plugin_param_edited` to an undoable
`set_device_param`, `plugin_param_touched` to `touch_parameter`, `plugin_state_dirty` to
`QUndoStack.resetClean`, `takes_recorded` to `ProjectEditor.add_recordings`, the plug-in
scan's results to `set_known_plugins`, and the selection to `show_plugin_editors`.

`MainWindow.start_audio` then:

1. `bridge.apply_audio_threads()`: the engine's render threads from the preferences.
2. `bridge.open_midi_inputs()`: every MIDI input connected but those turned off.
3. Opens the saved `AudioSettings`. If the device can't run as saved (an ASIO driver on
   another clock, fewer outputs), it opens with the device's own settings (rate, buffer,
   channels at their defaults); if the driver is gone, the system's default output (WASAPI)
   with the saved buffer size. If nothing opens, audio is off and the status line says to
   choose a device in *Options › Preferences*. The first launch has no saved settings, so it
   uses the system default output.

### The engine API check

The app (and the tests) won't start with an engine built from other code than the Python
side. `substation.ENGINE_API` is the bindings version this code needs;
`engine/src/bindings.cpp` exports `API_VERSION`. `engine_mismatch()` compares them and
returns a message saying whether the engine is older or newer and to re-run
`python -m pip install --no-build-isolation -e .`; `main()` shows it and exits with 1, and
[tests/conftest.py](../../tests/conftest.py) stops the test run with it. When Python code
comes to need a change in `bindings.cpp`, bump both together (see
[building.md](../building.md)). Both are 15 now.

### Shut-down

After the event loop ends, `main()` closes the audio device, then `EngineBridge.shutdown()`
closes every plug-in editor, waits for built-in devices' states to finish restoring,
removes every engine track (so plug-ins unload now, while the application is still whole,
not whenever the engine is garbage-collected), closes MIDI inputs and runs `engine.idle()`
once more (it destroys removed plug-ins on the main thread).

## How the model is mirrored

### Ids

The bridge maps model ids to engine ids:

| Map | From → to |
|---|---|
| `_track_ids` | model track id → engine track id (`MASTER` → `ge.MASTER`; the engine always has the master) |
| `_chains` | chain key → engine chain id. A track's own chain is keyed by the track's id, a rack chain by the chain's id (ids are unique in the project) |
| `_chain_owner` | chain key → the track it is on |
| `_rack_of_chain` | rack chain id → its rack's device id |
| `_devices` | chain key → `[(device id, processor id or None)]` in order (None: a plug-in that didn't load) |
| `_pids`, `_where` | device id → its processor; → the chain key its processor is in now |

`engine_device_id(track_id, device_id)` gives a device's processor if the device is on that
track (in a rack too) and loaded; `engine_chain_id(chain_id)` a rack chain's engine chain;
`engine_track_id(track_id)` a track's (group's, return's, the master's) engine track.

### Tracks, groups, returns, master

- Every track, group and return is an engine track; the master is the engine's own
  (`ge.MASTER`), and its devices, mixer and automation go the same way as a track's.
- `_add_engine_track` pushes, in order: mixer, input, clips, devices, automation, then
  outputs, sends (a return: every track's, since sends into it can exist only now), the
  inputs taken from it, and sidechains.
- **Groups**: the engine knows only where each track's output goes. `_push_outputs` sets
  every track's output to its group's engine track or the master. Changed routes go to the
  master first, then to their new group, so that no step closes a cycle (a group moving into
  what was in it).
- **Returns and sends**: a track's sends are engine sends into the returns' engine tracks, at
  the send's gain (`db_to_gain`), before or after the fader. `_push_sends` removes the sends
  going away first, then sets the others; one the engine refuses (`ValueError`: a cycle with
  a send another track hasn't given up yet) is skipped and comes with that track's turn. A
  send automated without having been set yet is made, silent (`Send()`), so that its
  automation plays (`_wanted_sends`).
- **Inputs**: `_push_input` sets device channels (`set_track_input`) or another track's
  output (`set_track_input_track`; `ge.MASTER` for resampling), the monitoring mode, arming
  and a MIDI track's MIDI input (`set_track_midi_input(engine_id, enabled, device, channel)`).
  A source the engine refuses for now (a cycle another change hasn't undone yet) leaves the
  track with no input until that change comes. A track given ASIO input channels the device
  hasn't open makes the device open again with them too (`_open_inputs`), as it runs now, and
  saves that, so they open next time too.
- **Removing a track** removes its engine track (and its devices). The bridge then treats what
  went into it as going to the master, forgets sends into it, inputs from it, sidechains from
  it and its overrides; the model's own changes, in the same undo step, follow.

### Clips and notes

- Audio tracks: `set_track_clips(engine_id, [clip_desc(c) ...])`. `clip_desc` keeps positions
  in beats and seconds (the engine converts them to samples at the current tempo and rate):
  path, start beat, duration, offset, gain, pan, warp (`Clip.is_warped`), segment BPM, warp
  mode (`ge.WarpMode` in `WARP_MODES` order) and `transpose + detune / 100` semitones. Every
  clip's file is requested for decoding first.
- MIDI tracks: the UI flattens the track's clips into the notes they play
  (`note_descs`: `MidiClip.played_notes()` in timeline beats) and calls `set_track_notes`.
  See [engine/midi.md](../engine/midi.md).

### Devices and racks

`_sync_devices(track_id)` runs on `devices_changed` (and when a track is added). It walks
the track's device tree top down (`_place`, `_place_rack`):

- A device already in that engine chain keeps its processor.
- A device whose processor is in another chain (of this track or another: it moved, into or
  out of a rack, or to another track) is **taken over** (`_take_over`): one
  `engine.move_processor`, nothing loads again, so a plug-in keeps its state and its open
  editor. A rack moves with its chains and everything in them. A processor moving to another
  track gives up its sidechains in the engine until it is there.
- A rack chain that is now another rack's gets a new engine chain, and its processors move
  into it.
- A new device gets a processor (`_create_processor`): a rack (`add_rack`, refused past the
  nesting limit with a status message), a plug-in (`_load_plugin`) or a built-in device
  (`add_builtin_processor`, then its `params`, then its `state` in the background). A
  built-in kind the engine doesn't know (a preset or project from a later version) gets
  none, and an entry in `plugin_errors`, as a missing plug-in does.
- Devices that left a chain and aren't anywhere else on this track are **disposed**: if the
  device is now on another track, that track is synced first so it takes the processor over;
  otherwise the processor goes (`_forget_processor`), a rack with what is still in it.
- Then the chain orders (`set_chain_order`, `set_rack_chain_order`), automation (the devices'
  envelopes go to their processors), editor titles, on/off (`_push_enabled`), rack chain
  faders and sidechains are pushed, and `devices_loaded(track id)` is emitted.

`_syncing` stops a track being synced again from inside its own sync.

### Plug-ins

- **Loading**: `plugin_path(ref)` is the saved path if it exists, else where the scan found
  that class id (`known_plugins`, from `set_known_plugins`). Missing, or failing to load, the
  device keeps its place with no processor, `plugin_errors[device id]` says why, and the
  status line shows it. `set_known_plugins` (after every scan) tries those devices again.
- **State**: a plug-in's state is restored from what it had when its device went away
  (`_plugin_states`: kept when a plug-in device is deleted, or its track, so undo brings it
  back as it was), else from `Device.state` (base64 `.vstpreset`). `store_plugin_states()`
  copies every loaded plug-in's state (or those of some devices) back into the model, for
  saving, copying devices, duplicating tracks and saving presets; it also records a moved
  plug-in's new path. `plugin_state(track, device)` reads one directly.
- **Parameters**: a model parameter change goes to the plug-in (`set_processor_param`)
  unless the plug-in already has that value (an edit made in its own editor). `param_id`,
  `param_infos`, `plugin_param_text` (the plug-in's text, with its unit) are cached per
  processor and dropped when the plug-in reports that its parameter list changed.
- **Editors**: `open_plugin_editor`, `close_plugin_editor`, `request_plugin_editor`,
  `is_plugin_editor_open`, `close_all_editors`. Only the selected track's editors show
  (`show_plugin_editors`): the others are hidden but keep running, at most
  `MAX_HIDDEN_EDITORS` (8); beyond that the ones hidden longest close, and open again where
  they were when their track is shown. `owner_window` (the main window's HWND) owns the
  editor windows so they float above it. Titles are `"<plug-in> - <track>"`, updated when
  the track is renamed.
- **Reports** (`_dispatch_processor_events`, from `engine.take_processor_events()`):

  | Engine event | Bridge |
  |---|---|
  | `PARAM_EDITED` (editor open) | `plugin_param_edited(track, device, param, value, old, gesture)`: the window makes it an undo step, one per knob drag |
  | `PARAM_TOUCHED` (editor open) | `plugin_param_touched`: its automation lane shows |
  | `PARAM_EDITED`/`PARAM_TOUCHED` with no editor open | not the user (a plug-in restoring its state reports edits): `plugin_params_changed` only |
  | `PARAMS_CHANGED`, `LATENCY_CHANGED` | `plugin_params_changed` |
  | `PARAM_INFO_CHANGED` | caches dropped, automation pushed again, `plugin_params_rebuilt` |
  | `EDITOR_CLOSED` | `plugin_editor_changed` |
  | `EDITOR_REQUESTED` | shown with its track, like any editor |
  | `STATE_DIRTY` | `plugin_state_dirty`: the project has changes no edit shows |

- **Re-entrancy**: a plug-in (or a driver) may run a message loop inside a call (a licence
  dialog, a control panel) that calls back into the UI. Around such calls the bridge counts
  `_busy`, and while it is above 0 it doesn't dispatch plug-in reports or device events.

See [engine/plugins.md](../engine/plugins.md) for the engine side.

### Built-in devices' state

A built-in device's `state` (a sampler's sample) is restored by `_StateTask` on a
one-thread pool, so loading a sample never holds up the window, and states are restored in
the order they were set (the last one wins). The engine swaps the sample in without stopping
the audio. A failure goes to the status line. `wait_for_device_states()` waits for them all;
the export does so before rendering offline. See [engine/devices.md](../engine/devices.md).

### Sidechains

`_push_sidechains` (after device syncs, outputs, track changes) works out every device's
wanted sidechain (`_wanted_sidechain`) as (source engine track, `ge.SidechainTap`, tap
processor):

- `POST_FADER` → `SidechainTap.POST_FADER`; `PRE_FADER` → `PRE_FADER`;
- `PRE_FX` → `PRE_FX`, except on a MIDI track whose first device is an instrument (or
  instrument rack): `AFTER_DEVICE` of that instrument (a MIDI track's own audio is its
  instrument's);
- a device id → `AFTER_DEVICE` of its processor; while that device isn't on the source,
  `PRE_FADER`.

None for a device without a sidechain input (`processor_info(...).has_sidechain`), a source
the engine hasn't, or the master. Changing ones are cleared first
(`clear_processor_sidechain`), then the rest set; one the engine refuses for now (a cycle
with a route another change hasn't undone yet) comes with that change.
`has_sidechain_input(track, device)` tells the device view whether to show the button.

### Freezing

`render_freeze(track)` renders the track's signal before its fader from beat 0 to the
arrangement's end (and on for up to `FREEZE_TAIL_SECONDS`, while it sounds) with
`render_track_to_wav()`, into `freeze_folder()` (the project's *Freeze* folder, or one in the
recordings folder), decodes it at once (so it plays without a gap) and returns the `Freeze`.

On `freeze_changed` (`_on_freeze_changed`): the plug-ins' states go into the model (so a
frozen track saves them), the engine track is frozen (`set_track_frozen`), its clips become
the frozen audio (`_push_clips`; a MIDI track's notes go), and `_sync_devices` takes its
processors away: the bridge sees a frozen track as having no devices (`_loaded_devices`), so
its devices go as if deleted, their plug-ins' states kept, and come back on unfreezing. The
tracks in a frozen group keep their processors; the engine just doesn't render them. Frozen
tracks don't record.

### Settings and transport

`_push_settings` sets the engine's `tempo`, time signature and loop. Transport: `play`,
`stop` (also ends a recording), `locate(beat)`, `position`, `is_playing`, `set_metronome`.
Preview: `preview_file` (decodes first; a later click or stop cancels a preview still
loading), `stop_preview`, `preview_note(track, pitch, velocity)` (velocity 0 releases).

## Automation and overrides

- `_push_automation(owner)` sends every envelope of an owner to the engine
  (`set_track_automation(engine track, [AutomationLane])`), except overridden ones. Lanes
  are `AutomationLane(processor id, name, points)`: processor 0 for the mixer (`volume`,
  `pan`, `send:<engine return track id>`), a device's processor and its parameter id, or a
  rack's processor and `chain:<engine chain id>:volume|pan` for a chain fader. Targets whose
  device isn't loaded are left out. It emits `automation_state_changed(owner)`.
- A target whose envelope stops playing (deleted, or overridden) goes back to the value the
  model holds for it (`_push_own_value`: the mixer is pushed again, a device parameter is
  set; sends and chain faders keep their own level in the engine). A plug-in's own values
  are the engine's, which follow its envelopes while they play; so when a plug-in
  parameter's envelope starts playing its own value is kept (`_plugin_own`), `own_value`
  answers with it, and it is sent back when the envelope stops. A new state (a preset)
  replaces the values kept for that plug-in; while it isn't loaded (frozen, deleted) they
  are kept for when it comes back, since the state it comes back with holds the envelope's
  values. (Across saving and loading they aren't kept: the saved state holds what the
  plug-in was at.)
- **Overrides**, as in Ableton: changing an automated target by hand (a track's volume or
  pan, a send level, a chain's fader, a device parameter: the bridge sees the model's value
  change) calls `override_automation(owner, key)`, which stops sending that envelope.
  `re_enable_automation(owner=None)` plays them again. `is_automated`, `is_overridden` and
  `has_overrides` drive the red dots, grey envelopes and the Re-Enable button. Overrides are
  bridge state: not saved, not undone, cleared on `reset`.
- Describing parameters to the UI: `param_groups(owner)` (the mixer with its sends, then
  each device, racks included), `device_param_specs`, `mixer_specs`, `param_spec`,
  `can_automate`. Plug-in parameters that aren't automatable, are hidden or read-only are left
  out. `own_value(owner, key)` is a target's value set by hand (a plug-in's read from the
  plug-in), `current_value(owner, key, beat)` what it is at a beat while automated.

See [engine/automation.md](../engine/automation.md).

## Async decoding

Audio files (`AUDIO_EXTENSIONS`: `.wav`, `.wave`, `.flac`, `.mp3`) are decoded at the
engine's rate in a `QThreadPool` of two threads (`_LoadTask` → `engine.load_source`).
`request_source(path, then)` decodes a file once (by `os.path.normcase(abspath)`), queues
`then` callbacks while it loads, and emits `source_ready(path)` or `source_failed(path,
message)` on the UI thread. `source(path)` gives the decoded `ge.AudioSource` (waveform
peaks for the arrangement); `is_loading`, `load_error`, `file_info` (header only, cached,
`ge.probe_file`). After a sample-rate change every source is decoded again
(`refresh_sources`). On `reset` the bridge forgets sources the new project doesn't use and
calls `engine.release_unused_sources()`. See [engine/warp.md](../engine/warp.md).

## Polling

- **Playhead**, every 16 ms (about 60 Hz): `position_beats` and `is_playing`;
  `position_changed` and `transport_changed` only when they changed. Also polled at once
  after play, stop, locate and recording.
- **Meters**, every 33 ms (about 30 Hz), in this order:
  1. `_poll_recording`: live takes' progress (peaks, or a MIDI take's notes so far) into
     `live_takes`, `recording_updated`; if the engine stopped recording on its own (a locate,
     a device change), the recording ends.
  2. `engine.take_meters()` into `meters` (track id or `MASTER` → (left, right)) and
     `chain_meters` (rack chain id → (left, right)); `meters_updated`.
  3. `poll_plugins()`: `engine.idle()` (the engine's main-thread housekeeping: freeing
     retired snapshots, destroying removed plug-ins, plug-in main-thread work) and, unless
     busy, the plug-in reports.
  4. Unless busy, `_poll_device`.

## Device events

`_poll_device` takes one event per poll from `engine.take_device_event()`:

| Event | Bridge |
|---|---|
| `"stopped"` | status: the device stopped, choose one; `device_changed` |
| `"rerouted"` | status: output rerouted to another device; `device_changed` |
| `"reset"` | `reset_device()`: `engine.reopen_device` opens the driver again with the buffer size and rate it now has (an ASIO driver's settings changed in its control panel, or its clock) |
| `"latency"` | `device_changed` (the status line shows the new latencies) |

`open_device(settings)`, `reset_device()`, `close_device()` and
`show_device_control_panel()` go through `_change_device`, which counts `_busy` (a driver may
show a dialog), decodes sources again if the rate changed and emits `device_changed`.
`open_device` passes the window handle (ASIO drivers want one). Closing the device ends a
recording. See [engine/audio-devices.md](../engine/audio-devices.md).

## Recording

- `record_targets()`: armed tracks with an input (`Track.has_input`).
  `start_recording(count_in_beats)` returns why it can't (nothing armed, no device, the
  folder can't be made) or None. It opens missing ASIO inputs, makes the recordings folder,
  names each audio take `take_path(folder, track name, now)`: the track's name (characters
  Windows forbids replaced) and the local time, numbered if taken; MIDI takes have no file.
- `recordings_folder(project)`: the project's `Recordings` folder once it is saved; else
  `SUBSTATION_RECORDINGS` if set (the tests use it), else `Music\SUBstation\Recordings`.
- `live_takes` holds a `LiveTake` per track while it records, which the arrangement draws.
- `stop_recording()` ends it (playing goes on), reports takes' errors and dropped samples,
  and emits `takes_recorded([RecordedTake])`: start and length in seconds, a MIDI take's notes
  in seconds. The window adds them as clips in one undo step, quantized to
  `record_quantize()`.

See [engine/recording.md](../engine/recording.md) and
[guide/recording.md](../guide/recording.md).

## MIDI input

`open_midi_inputs()` opens every MIDI input connected except those turned off in the
preferences, closes those turned off or gone, and reports an input that can't be opened once
(`midi_errors`). `set_midi_input_enabled` saves the choice and applies it. `midi_inputs()`
lists the inputs; `midi_input_choices()` adds `COMPUTER_KEYBOARD` ("Computer Keyboard"), the
input the computer MIDI keyboard plays into, always there; `send_midi(message, device)` plays
a message as if that input sent it (`engine.send_midi_input`). See
[engine/midi.md](../engine/midi.md).

## Settings storage

Preferences are per user, in `QSettings` under the organisation and application name
"SUBstation" (on Windows, Qt keeps them in the registry under `HKEY_CURRENT_USER\Software\SUBstation\SUBstation`). The tests
use "SUBstation Tests" so they never touch the user's.

| Key | Holds | Code |
|---|---|---|
| `audio/driver` | `"WASAPI"` or `"ASIO"` | `AudioSettings` |
| `audio/device` | device or driver name; empty: the system default (WASAPI), the first driver (ASIO) | |
| `audio/sample_rate` | 0: the rate the device runs at | |
| `audio/buffer_frames` | 0: the device's preferred size; default 256 | |
| `audio/exclusive` | WASAPI exclusive mode | |
| `audio/output_channels` | `"2,3"`: the pair the master plays on; empty: the first two | |
| `audio/input_channels` | ASIO inputs to open; none until something records | |
| `audio/threads` | render threads; 0: the engine's default (one per core but one) | `audio_threads()` |
| `midi/disabled_inputs` | MIDI inputs turned off (every other input is used, so a new one just plays) | `disabled_midi_inputs()` |
| `record/quantize` | record quantization grid in beats; 0: as played (`RECORD_QUANTIZE` lists the choices) | `record_quantize()` |

`AudioSettings.load()` falls back to WASAPI for an unknown driver; `save()` writes them all.
The preferences dialog ([ui/README.md](../ui/README.md)) saves them when a change opens
successfully, so the program opens the same driver next time. Other parts of the UI keep
their own keys (recent files, browser places and sort, count-in, plug-in folders:
[plugin-scanner.md](plugin-scanner.md)). The user-facing side is in
[guide/audio-setup.md](../guide/audio-setup.md).

## Signals

| Signal | Meaning |
|---|---|
| `source_ready(path)`, `source_failed(path, message)` | decoding finished |
| `position_changed(beat)`, `transport_changed(playing)` | playhead polling |
| `meters_updated()` | `meters` and `chain_meters` were refreshed |
| `device_changed()` | the audio device opened, closed, changed or reported new latencies |
| `status_message(text)` | for the status line |
| `plugin_param_edited`, `plugin_param_touched`, `plugin_params_changed`, `plugin_params_rebuilt`, `plugin_editor_changed`, `plugin_state_dirty` | plug-in reports (above) |
| `devices_loaded(track id)` | a track's processors were (re)created |
| `automation_state_changed(owner)` | which envelopes play or are overridden changed |
| `recording_changed(recording)`, `recording_updated()`, `takes_recorded(takes)` | recording |

## Invariants

- Only the bridge talks to the engine about the project; the model never does.
- A device's processor lives as long as the device is in the project's chains: reordering or
  changing the chain around it, grouping it into a rack or moving it to another track never
  reloads it.
- Routing changes are pushed in an order where no single engine call closes a cycle: routes
  going away first. A call the engine refuses for now is retried when the change in its way
  arrives.
- The bridge doesn't hold model objects across calls; it re-reads the project by id.

## Extending it

- **A new model signal**: connect it in `EngineBridge.__init__`, push only what changed, and
  keep the last value pushed if the engine call is expensive or must be ordered.
- **A new engine binding**: bump `API_VERSION` in `engine/src/bindings.cpp` and `ENGINE_API`
  in `src/substation/__init__.py` together.
- **A new preference**: a QSettings key with a function in `audio/settings.py`, read where it
  applies, and a control in the preferences dialog.

## Gotchas

- Plug-in reports and device events are not dispatched while `_busy`: a plug-in's or
  driver's message loop must not change the chains under a call in progress.
- A plug-in that reports edits while restoring its state (a paste, a duplicate, an undo) would
  fill the undo stack; edits only count while its editor is open.
- `Device.state` of a plug-in is stale until `store_plugin_states()`; read `plugin_state()` for
  the current one.
- Overrides are lost on `reset` (opening a project) and are never saved.
- Decoding is keyed by the normalized path, so two clips of one file share one source.
- `QTimer` intervals are 16 and 33 ms; the README's 60 Hz and 30 Hz are approximate.

## Tests

- [test_groups_model.py](../../tests/test_groups_model.py),
  [test_sends_model.py](../../tests/test_sends_model.py),
  [test_resampling_model.py](../../tests/test_resampling_model.py),
  [test_sidechain_model.py](../../tests/test_sidechain_model.py),
  [test_racks_model.py](../../tests/test_racks_model.py): the engine taking each feature
  through the bridge; racks keeping each device's processor as they are made, undone, moved and
  deleted; chain automation and overrides; macros moving plug-in parameters.
- [test_ui_plugins.py](../../tests/test_ui_plugins.py): plug-in loading, editors, edits for
  undo, presets and projects in the window.
- [test_asio.py](../../tests/test_asio.py): the preferences and start-up with the fake ASIO
  driver, resets, the control panel.
- [test_ui_recording.py](../../tests/test_ui_recording.py): `recordings_folder`, `take_path`,
  record quantization, takes becoming clips.
- [test_computer_keyboard.py](../../tests/test_computer_keyboard.py),
  [test_midi_input.py](../../tests/test_midi_input.py): MIDI input.
- [test_automation_model.py](../../tests/test_automation_model.py): `engine_mismatch()`.
- [test_ui_smoke.py](../../tests/test_ui_smoke.py): `clip_desc`, `audio_threads`, the window
  as a whole.

See [testing.md](../testing.md) for the suite and its fixtures.
