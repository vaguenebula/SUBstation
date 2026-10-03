# Project files and presets

[serialization.py](../../src/substation/model/serialization.py) saves the
[project model](model.md) as a `.gilproj` file (JSON) and loads it back. It also saves a
single device (a rack with everything in it) as a `.gilpreset` file. Nothing else is
written: audio stays in its own files, and preferences are in QSettings (see
[engine-bridge.md](engine-bridge.md#settings-storage)).

## Overview

```
save:  MainWindow._save_to
         ├─ EngineBridge.store_plugin_states()   plug-ins' current states → Device.state (base64)
         └─ save_project(project, path)
              └─ project_to_dict → json.dumps(indent=2) → <path>.tmp → os.replace → project.path = path

load:  MainWindow.open_project
         └─ load_project(project, path)
              └─ json.loads → load_into
                   ├─ check "format" and "version"
                   ├─ tracks_from_dict (+ repair_tree), returns_from_dict, _master
                   ├─ repair_routing   (drops sends/inputs/sidechains that can't be)
                   └─ project.replace_contents(...)  → Project.reset → bridge rebuilds the engine
```

- A save never leaves a half-written file behind: it writes `<name>.gilproj.tmp` and
  replaces the old file with it in one `os.replace`. Presets do the same.
- Loading is tolerant: missing fields take their defaults, values are clamped, unknown
  automation targets are dropped, and routing a project can't have is repaired. A file that
  isn't a project, is from a newer version, or is broken raises `ProjectFileError` with a
  message for the user (`MainWindow` shows it in a message box).
- After loading, `Project.reset` makes the [engine bridge](engine-bridge.md) remove every
  engine track and build the project again; plug-ins load from their saved state.

## Files

| File | What it holds |
|---|---|
| [serialization.py](../../src/substation/model/serialization.py) | `save_project`, `load_project`, `project_to_dict`, `load_into`, `tracks_from_dict`, `returns_from_dict`, `repair_routing`; presets: `device_to_preset`, `preset_device`, `save_preset`, `load_preset`; `ProjectFileError`; constants `FORMAT`, `VERSION`, `EXTENSION`, `PRESET_FORMAT`, `PRESET_VERSION`, `PRESET_EXTENSION` |
| [project.py](../../src/substation/model/project.py) | the types saved; `repair_tree`, `LEGACY_WARP_MODES`, `refresh_ids` |
| [device_state.py](../../src/substation/model/device_state.py) | the text format inside a built-in device's `state` |

## The .gilproj format

UTF-8 JSON, indented by 2. Beats are quarter notes; times in seconds are source-audio
seconds; volumes are dB; pan is -1..1; automation values are normalized 0..1.

### Top level

| Field | Type | Meaning |
|---|---|---|
| `format` | `"gilstudio-project"` | must match, or the file is refused ("Not a SUBstation project") |
| `version` | int | `VERSION`, now 12; a larger one is refused ("saved by a newer version") |
| `tempo` | float | BPM (default 120) |
| `key` | string or null | the project key as `Key.name` (`"Am"`, `"F#"`, `"Bb"`); null: *No Key* |
| `time_signature` | `[numerator, denominator]` | default `[4, 4]` |
| `loop` | `{"enabled", "start", "end"}` | the loop brace, in beats (default off, 0..16) |
| `automation_locked` | bool | Lock Envelopes |
| `master` | object | the master track (below) |
| `folded_devices` | list of device ids | devices shown folded; only ids of devices that exist are saved |
| `tracks` | list | the arrangement's tracks, in order, flat (below) |
| `returns` | list | the return tracks, in order (below) |

### Tracks

Each entry of `tracks`:

| Field | Meaning |
|---|---|
| `id`, `name`, `color` | required on load |
| `kind` | `"audio"`, `"midi"` or `"group"` (default `"audio"`; anything else makes the file "damaged") |
| `volume_db`, `pan`, `mute`, `solo`, `height` | mixer and row height |
| `devices` | list of devices (below) |
| `clips` | audio or MIDI clips (below), by the track's kind; a group's are ignored |
| `automation` | `{target key: [[beat, value, curve], ...]}` |
| `automation_view` | `{"shown": bool, "key": key or null, "lanes": [keys]}` |
| `input` | device channels, `[]`, `[c]` or `[l, r]` (0-based) |
| `input_track` | only when set: the id of the track (group, return) whose output it takes, or `"master"` (resampling) |
| `midi_input` | MIDI tracks only: `{"device": name or "" for all, "channel": 0 (all) or 1..16}`, or null for *No Input* |
| `monitor` | `"off"`, `"in"` or `"auto"` (default `"auto"`) |
| `armed` | bool (never true for a group on load) |
| `parent` | the id of the group it is in, or null |
| `folded` | bool |
| `sends` | `{return id: {"level_db": float, "pre_fader": bool}}` |

The tracks are stored flat in arrangement order; the group tree is only the `parent`
fields. On load, `repair_tree` takes any track out of a group it can't be in (a group's
tracks must follow it, together), keeping the order.

### Return tracks

Each entry of `returns` has `id`, `kind` (`"return"`), `name`, `color`, `volume_db`, `pan`,
`mute`, `solo`, `height`, `devices`, `automation`, `automation_view` and `sends` (a return
can send on into another return). No clips, inputs, arming, parent or folding.

### Master

`master` holds `volume_db`, `pan`, `devices`, `automation` and `automation_view`. Its id is
always `"master"`, which is also the automation owner and the input source for resampling.

### Audio clips

| Field | Meaning |
|---|---|
| `id`, `name` | `name` defaults to the file's stem |
| `path` | absolute path when saved |
| `relative_path` | the path relative to the project file's folder, or null (unsaved project, or another drive) |
| `start_beat` | where it starts on the timeline |
| `duration_sec`, `offset_sec`, `source_duration_sec` | the window onto the source, in source seconds |
| `gain_db`, `pan` | clip volume and pan |
| `warp`, `warp_mode`, `segment_bpm` | warping (`segment_bpm` 0: not set) |
| `transpose`, `detune` | semitones (int), cents |

On load, `path` is used if it exists; otherwise, if the project file's folder plus
`relative_path` exists, that is used. So a project folder (with its samples and its
`Recordings` folder) can be moved or copied to another computer.

### MIDI clips

`id`, `name` (default `"MIDI"`), `start_beat`, `duration_beats`, `offset_beats`, and
`notes` inline as `[pitch, start, length, velocity]` (beats from the clip's content start).
On load, notes with a length of 0 or less are dropped, pitch is clamped to 0..127, velocity
to 1..127, start to 0 and up, and the notes are sorted and de-duplicated
(`notes.normalize`).

### Devices

| Field | Meaning |
|---|---|
| `id` | unique in the project; automation keys and macro mappings refer to it |
| `kind` | a built-in device id (`"synth"`, `"sampler"`, ...), `"plugin"` or `"rack"` |
| `enabled` | on/off (default on) |
| `params` | `{param id: plain value}`; a rack's are its macros (`"macro1"`..`"macro8"`, normalized) |
| `plugin` | plug-ins only: `{"format": "VST3", "uid", "name", "vendor", "path", "instrument"}` |
| `state` | base64: a plug-in's whole state (a `.vstpreset`), or a built-in device's non-parameter values; omitted when a built-in device has none |
| `sidechain` | only when set: `{"track": source id, "tap": "post" / "pre" / "pre-fx" / a device id}` |
| `chains` | racks only: `[{"id", "name", "volume_db", "pan", "mute", "solo", "devices": [...]}]` |
| `macros` | racks only: `[{"macro": 0..7, "device", "param", "low", "high"}]` |

### Automation

Per owner (each track, return and the master), `automation` maps a target key to an
envelope saved as `[beat, value, curve]` points (a point without `curve` loads with 0).
Keys are those of [automation.py](../../src/substation/model/automation.py):
`mixer:volume`, `mixer:pan`, `send:<return id>`, `device:<device id>:<param id>`, and
`device:<rack id>:chain:<chain id>:volume|pan`. Empty envelopes aren't saved. On load,
keys this version doesn't know are dropped, and points are sorted and clamped
(`automation.normalize`). `automation_view` keeps what the arrangement shows of it; keys in
it that aren't valid are dropped.

### Plug-in state

A plug-in's state lives in the plug-in while the project is open. Before saving,
`MainWindow` calls `EngineBridge.store_plugin_states()`, which asks the engine for each
loaded plug-in's state (`processor_state`: a standard `.vstpreset`), stores it base64 in
`Device.state`, and updates `PluginRef.path` if the plug-in was found somewhere else. On
load, the bridge finds the plug-in by `path`, or by class id (`uid`) among the scanned
plug-ins if it moved, loads it and restores the state. A plug-in that isn't installed keeps
its place and settings in the project (its `state` is saved again unchanged) and loads when
it is back after a rescan (`EngineBridge.set_known_plugins`). See
[engine/plugins.md](../engine/plugins.md) and [plugin-scanner.md](plugin-scanner.md).

### Built-in device state

A built-in device's `state` is base64 of the engine's text format (`name=value` lines; see
[device_state.py](../../src/substation/model/device_state.py)). The Sampler stores its
sample as `sample=<path>`. Its parameters are in `params`.

## Versions and migrations

`VERSION` counts the format's changes. There is no per-version migration code: each
addition has a default that makes an older file load as it was, and saving writes the
current version.

| Version | Added | An older file loads with |
|---|---|---|
| 1 | audio tracks and clips | (tracks without `kind` are audio) |
| 2 | MIDI tracks | |
| 3 | plug-ins | |
| 4 | automation, master pan | no automation |
| 5 | master devices | a master without devices (very old files have no `master` at all: defaults) |
| 6 | audio inputs, monitoring, arming | no input, Auto, not armed |
| 7 | MIDI inputs | every MIDI input heard |
| 8 | group tracks (`parent`), folding | no groups, unfolded |
| 9 | return tracks and sends | none |
| 10 | inputs from tracks (`input_track`, resampling) | none |
| 11 | sidechains | none |
| 12 | racks (chains, macros) | none |

`folded_devices` has no version of its own: files without it load with no device folded.
The project key, `automation_locked` and the clip fields default the same way.

**Legacy warp mode names.** Projects saved with the earlier Ableton-style names load into the
mode that plays the same way (`LEGACY_WARP_MODES`): Beats → Transients, Tones → Standard,
Complex → Standard, Texture → Smooth, Complex Pro → Formants. An unknown name loads as
Standard.

## Repairs on load

A file edited by hand (or by an older bug) may hold routing the project can't have. Loading
fixes it rather than refusing the file:

- `repair_tree` takes tracks out of groups they can't be in.
- `repair_routing(tracks, returns, master)`:
  - sends to a return that isn't there are dropped;
  - returns' sends are put back one by one, each checked against those before it; one that
    would close a cycle is dropped;
  - each audio track's `input_track` is kept only if the source exists (a track, a return
    or the master) and doesn't close a cycle; then its device channels are cleared;
  - sidechains are put back one by one on every owner's devices (racks too), dropped if the
    source isn't a track or return of the project or would close a cycle.
- Rack macro mappings to a device not in the rack, or with a macro outside 0..7, are dropped.
- Sends' and chains' levels are clamped to the faders' range (-70..+6 dB), pans to -1..1,
  MIDI channels to 0..16, inputs to at most two channels, `monitor` to a known mode.
- `armed` is ignored on a group.

## What is saved and what isn't

Saved: everything in the model, including view state that isn't undone: track heights,
folded tracks and devices, which automation shows, arming, solo (tracks and rack chains),
Lock Envelopes.

Not saved:

- Undo history (`QUndoStack`).
- Automation overrides: a target changed by hand plays its automation again after loading
  (the override lives in the bridge, not the model).
- The selection, insert marker, playhead, zoom and scroll.
- Decoded audio, waveforms and peaks (decoded again on load).
- Audio files themselves: clips refer to them (`path` and `relative_path`). Recorded takes
  are WAV files in the project's `Recordings` folder, referred to the same way.
- Preferences (audio device, MIDI inputs, audio threads, record quantization, count-in,
  browser places, plug-in folders): QSettings, per user.
- A plug-in's `Device.state` is only as recent as the last `store_plugin_states()`; the
  project file is always written right after one.

## Presets (.gilpreset)

A rack's save button saves it, with everything in it, as a preset; right-clicking beside the
devices loads one (see [guide/devices.md](../guide/devices.md)).

```json
{"format": "gilstudio-preset", "version": 1, "device": { ...a device, as in a project... }}
```

- `device_to_preset(device)`: the device as the project file stores it (a rack with its
  chains, the devices in them with plug-ins' states as last stored, its macros), with every
  `sidechain` stripped (they name tracks of the project it came from). The UI stores the
  plug-ins' states first.
- `preset_device(data)`: checks `format` and `version` (`PRESET_VERSION` = 1), refuses racks
  nested deeper than `MAX_RACK_DEPTH`, turns `KeyError`/`TypeError`/`ValueError`/
  `AttributeError`/`RecursionError` into `ProjectFileError("The preset is damaged: ...")`,
  clears sidechains and gives the device and everything in it **fresh ids**
  (`refresh_ids`, which also renames macro mappings). So a preset loaded twice is two racks.
- `save_preset` / `load_preset` write and read the file (temporary file and `os.replace`).

Plug-ins' own `.vstpreset` files (*Load Preset…* / *Save Preset…* on a plug-in) are a
different thing: the engine reads and writes them, and loading one is an undoable
`SetDeviceStateCommand`.

## Extending the format

1. Add the field to the model type with a default.
2. Write it in `project_to_dict` (or `_device_to_dict`, `_clip_to_dict`, ...) and read it
   with `.get(name, default)`, validating and clamping.
3. If it refers to other tracks or devices, drop what can't be in `repair_routing` (or
   where the device is read), and in `preset_device` if it can't travel with a preset.
4. Bump `VERSION`, add the change to the comment beside it and to the module docstring, and
   add a test that an older file still loads.

## Gotchas

- `load_into` replaces the project only after everything parsed, so a damaged file leaves
  the open project as it was. `load_project` maps `KeyError`, `TypeError` and `ValueError`
  to `ProjectFileError("<name> is damaged: ...")`.
- `project_to_dict` writes `relative_path` relative to the folder it is *saved to*, so Save
  As to another folder writes paths relative to the new place.
- A built-in device's state may hold absolute paths (the Sampler's sample), which are not
  made relative.
- A plug-in device's `params` are saved too, but loading restores a plug-in from its `state`
  only; its `params` serve undo.
- `folded_devices` drops ids of devices that no longer exist, so the list never grows stale.

## Tests

- [test_serialization.py](../../tests/test_serialization.py): round trip, the relative path
  fallback when a folder moves, foreign files refused, old warp mode names, the master's
  devices, a version 4 file loading unchanged (and saving again as the current version),
  inputs, monitoring and arming.
- [test_midi_model.py](../../tests/test_midi_model.py): MIDI tracks and inputs round trip;
  version 1 files load as audio tracks.
- [test_groups_model.py](../../tests/test_groups_model.py),
  [test_sends_model.py](../../tests/test_sends_model.py),
  [test_resampling_model.py](../../tests/test_resampling_model.py),
  [test_sidechain_model.py](../../tests/test_sidechain_model.py): each feature saved and
  loaded, and what can't be dropped on load.
- [test_racks_model.py](../../tests/test_racks_model.py): racks saved and loaded, presets as
  new devices (loaded twice), damaged presets, presets nesting too deep, a saved rack
  rendering as the original.
- [test_automation_model.py](../../tests/test_automation_model.py),
  [test_keys.py](../../tests/test_keys.py),
  [test_ui_device_view.py](../../tests/test_ui_device_view.py) (folding saved but not undone),
  [test_ui_device_editors.py](../../tests/test_ui_device_editors.py) (the Sampler's sample
  saved), [test_ui_plugins.py](../../tests/test_ui_plugins.py) (plug-in presets and projects),
  [test_ui_smoke.py](../../tests/test_ui_smoke.py) (save and open in the window).
