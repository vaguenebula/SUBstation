# Project files and presets

[io/Serialization.h](../../app/src/io/Serialization.h) saves the [project model](model.md) as a `.gilproj` file
(JSON) and loads it back. It also saves a single device (a rack with everything in it) as a `.gilpreset` file;
[io/Presets.h](../../app/src/io/Presets.h) keeps the user's library of them. Nothing else is written: audio stays in
its own files, and preferences are in QSettings (see [engine-bridge.md](engine-bridge.md#settings-storage) and
[session.md](session.md#settings)).

Files saved by earlier versions of SUBstation load unchanged: the keys and the structure are the same (see
[versions and migrations](#versions-and-migrations) for what each format version added).

## Overview

```
save:  Session::saveProject() / saveProjectAs(path)
         ├─ EngineBridge::storePluginStates()   plug-ins' current states → Device::state (base64)
         └─ saveProject(project, path)
              └─ projectToJson → QJsonDocument (indented) → QSaveFile: a temporary file, renamed into place
                 → Project::setPath(path)

load:  Session::openProject(path)
         └─ loadProject(project, path)
              └─ read, parse the JSON
                   ├─ check "format" and "version"
                   ├─ tracksFromJson (+ repairTree), returnsFromJson, the master
                   ├─ repairRouting   (drops sends, inputs and sidechains that can't be)
                   └─ Project::replaceContents(...)  → Project::reset → the bridge rebuilds the engine
```

- A save never leaves a half-written file behind: it writes a temporary file beside the old one and renames it into
  place when it is complete (`QSaveFile::commit`). Presets do the same.
- Loading is tolerant: missing fields take their defaults, values are clamped, unknown automation targets are
  dropped, and routing a project can't have is repaired. A file that isn't a project, is from a newer version, or is
  broken throws `ProjectFileError` ([model/Errors.h](../../app/src/model/Errors.h)) with a message for the user; the
  session emits it as a `warning`, which the UI shows in a message box. A file that can't be written throws it too.
- After loading, `Project::reset` makes the [engine bridge](engine-bridge.md) remove every engine track and build the
  project again; plug-ins load from their saved state, after the project shows
  ([engine-bridge.md](engine-bridge.md#opening-a-project)).

## Files

| File | What it holds |
|---|---|
| [io/Serialization.h](../../app/src/io/Serialization.h) / [.cpp](../../app/src/io/Serialization.cpp) | `saveProject`, `loadProject`, `projectToJson`, `loadInto`, `tracksFromJson`, `returnsFromJson`, `repairRouting`; `deviceToJson`, `deviceFromJson`; presets: `deviceToPreset`, `presetDevice`, `savePreset`, `loadPreset`; constants `kProjectFormat`, `kProjectVersion`, `kProjectExtension`, `kPresetFormat`, `kPresetVersion`, `kPresetExtension`. Its header comment lists every version's additions. |
| [io/Presets.h](../../app/src/io/Presets.h) / [.cpp](../../app/src/io/Presets.cpp) | The preset library: `libraryDir`, `presetFileName`, `groupOf`, `presetPath`, `saveToLibrary`, `listPresets`, `renamePreset`; default presets: `defaultPath`, `saveDefault`, `hasDefault`, `clearDefault`, `defaultDevice`; `kDefaultsFolder` |
| [model/](../../app/src/model) | The types saved; `repairTree` ([Routing.h](../../app/src/model/Routing.h)), `legacyWarpMode` ([Clip.h](../../app/src/model/Clip.h)), `refreshIds` ([Device.h](../../app/src/model/Device.h)), `ProjectFileError` ([Errors.h](../../app/src/model/Errors.h)) |
| [model/DeviceState.h](../../app/src/model/DeviceState.h) | the text format inside a built-in device's `state` |

## The .gilproj format

UTF-8 JSON, indented. (Qt writes each object's keys in alphabetical order, indented by 4; earlier versions kept their
own order and indented by 2. Neither matters to reading.) Beats are quarter notes; times in seconds are
source-audio seconds; volumes are dB; pan is -1..1; automation values are normalized 0..1.

### Top level

| Field | Type | Meaning |
|---|---|---|
| `format` | `"gilstudio-project"` | must match, or the file is refused ("Not a SUBstation project") |
| `version` | int | `kProjectVersion`, now 18; a larger one is refused ("This project was saved by a newer version of SUBstation") |
| `tempo` | float | BPM (default 120) |
| `key` | string or null | the project key as `Key::name()` (`"Am"`, `"F#"`, `"Bb"`); null: *No Key* |
| `time_signature` | `[numerator, denominator]` | default `[4, 4]` |
| `loop` | `{"enabled", "start", "end"}` | the loop brace, in beats (default off, 0..16) |
| `automation_locked` | bool | Lock Envelopes |
| `master` | object | the master track (below) |
| `folded_devices` | list of device ids | devices shown folded; only ids of devices that exist are saved |
| `chain_lists_shown` | list of rack ids | racks whose chain list shows (hidden by default); only ids of devices that exist are saved |
| `rack_devices_hidden` | list of rack ids | racks whose chain's devices don't show beside them (shown by default); only ids of devices that exist are saved |
| `tracks` | list | the arrangement's tracks, in order, flat (below) |
| `returns` | list | the return tracks, in order (below) |

### Tracks

Each entry of `tracks`:

| Field | Meaning |
|---|---|
| `id`, `name`, `color` | required on load |
| `kind` | `"audio"`, `"midi"` or `"group"` (default `"audio"`; anything else makes the file "damaged") |
| `volume_db`, `pan`, `mute`, `solo`, `height` | mixer and row height (without one: `kDefaultTrackHeight`, 96, or a group's `kDefaultGroupHeight`, 104) |
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
| `frozen` | only when frozen: `{"path", "relative_path", "duration_sec", "tempo"}`, its frozen audio (the file found as a clip's is); an incomplete one loads unfrozen. Once a time selection over it was edited, also `"segments"`: what of it plays, `[{"id", "start_beat", "offset_sec", "duration_sec"}]` (`Freeze::segments`: clips into the render, the rest of each is the freeze's: its file, warped from its tempo); none plays all of it, `[]` nothing; a damaged segment is left out |

The tracks are stored flat in arrangement order; the group tree is only the `parent` fields. On load, `repairTree`
takes any track out of a group it can't be in (a group's tracks must follow it, together), keeping the order.

### Return tracks

Each entry of `returns` has `id`, `kind` (`"return"`), `name`, `color`, `volume_db`, `pan`, `mute`, `solo`,
`height`, `devices`, `automation`, `automation_view` and `sends` (a return can send on into another return), and
`frozen` when it is. No clips, inputs, arming, parent or folding.

### Master

`master` holds `volume_db`, `pan`, `devices`, `automation` and `automation_view`. Its id is always `"master"`
(`kMaster`), which is also the automation owner and the input source for resampling.

### Audio clips

| Field | Meaning |
|---|---|
| `id`, `name` | `name` defaults to the file's stem |
| `path` | absolute path when saved |
| `relative_path` | the path relative to the project file's folder, or null (unsaved project, or another drive) |
| `start_beat` | where it starts on the timeline |
| `duration_sec`, `offset_sec`, `source_duration_sec` | the window onto the source, in source seconds |
| `gain_db`, `pan` | clip gain and pan |
| `warp`, `warp_mode`, `segment_bpm` | warping (`segment_bpm` 0: not set) |
| `transpose`, `detune` | semitones (int), cents |
| `fade_in_sec`, `fade_in_curve`, `fade_out_sec`, `fade_out_curve` | only where the clip has that fade: its length in source seconds, and its curve (-1..1, 0 a straight line). On load they are held to the clip (`Clip::fitFades`) |
| `reversed_from`, `reversed_from_relative` | only for a reversed clip: the file `path` (its reversed copy) was made from, absolute and relative |

On load, `path` is used if it exists; otherwise, if the project file's folder plus `relative_path` exists, that is
used (and the same for `reversed_from`). So a project folder (with its samples and its `Recordings`, `Freeze` and
`Reversed` folders) can be moved or copied to another computer.

### MIDI clips

`id`, `name` (default `"MIDI"`), `start_beat`, `duration_beats`, `offset_beats`, and `notes` inline as
`[pitch, start, length, velocity]` (beats from the clip's content start). On load, notes with a length of 0 or less
are dropped, pitch is clamped to 0..127, velocity to 1..127, start to 0 and up, and the notes are sorted and
de-duplicated (`notes::normalize`).

### Devices

| Field | Meaning |
|---|---|
| `id` | unique in the project; automation keys and macro mappings refer to it |
| `kind` | a built-in device id (`"synth"`, `"sampler"`, ...), `"plugin"` or `"rack"` |
| `enabled` | on/off (default on) |
| `params` | `{param id: plain value}`; a rack's are its macros (`"macro1"`.., one per macro, normalized) |
| `plugin` | plug-ins only: `{"format": "VST3", "uid", "name", "vendor", "path", "instrument"}` |
| `state` | base64: a plug-in's whole state (a `.vstpreset`), or a built-in device's non-parameter values; omitted when a built-in device has none |
| `sidechain` | only when set: `{"track": source id, "tap": "post" / "pre" / "pre-fx" / a device id}` |
| `name` | racks only, when set: the preset it was saved as or loaded from |
| `chains` | racks only: `[{"id", "name", "volume_db", "pan", "mute", "solo", "devices": [...]}]` |
| `macros` | racks only: `[{"macro": 0..15, "device", "param", "low", "high"}]` (`low` > `high`: the other way round) |
| `macro_names` | racks only: one name per macro (`""`: named by its number), so how many it has (1..16) |

### Automation

Per owner (each track, return and the master), `automation` maps a target key to an envelope saved as
`[beat, value, curve]` points (a point without `curve` loads with 0). Keys are those of
[Automation.h](../../app/src/model/Automation.h): `mixer:volume`, `mixer:pan`, `send:<return id>`,
`device:<device id>:<param id>`, and `device:<rack id>:chain:<chain id>:volume|pan`. Empty envelopes aren't saved.
On load, keys this version doesn't know are dropped, and points are sorted and clamped (`automation::normalize`).
`automation_view` keeps what the arrangement shows of it; keys in it that aren't valid are dropped.

### Plug-in state

A plug-in's state lives in the plug-in while the project is open. Before saving, the session calls
`EngineBridge::storePluginStates()`, which asks the engine for each loaded plug-in's state (a standard
`.vstpreset`), stores it base64 in `Device::state` (`Project::storePluginState`: no signal, no undo step), and
updates `PluginRef::path` if the plug-in was found somewhere else. On load, the bridge finds the plug-in by `path`,
or by class id (`uid`) among the scanned plug-ins if it moved, loads it and restores the state. A plug-in that isn't
installed keeps its place and settings in the project (its `state` is saved again unchanged) and loads when it is
back after a rescan (`EngineBridge::setKnownPlugins`). See [engine/plugins.md](../engine/plugins.md) and
[plugin-scanner.md](plugin-scanner.md).

### Built-in device state

A built-in device's `state` is base64 of the engine's text format (`name=value` lines; see
[DeviceState.h](../../app/src/model/DeviceState.h)). The Sampler stores its sample as `sample=<path>`. Its
parameters are in `params`.

## Versions and migrations

`kProjectVersion` counts the format's changes. There is no per-version migration code: each addition has a default
that makes an older file load as it was, and saving writes the current version.

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
| 13 | rack names (`"name"` on a rack) | none: racks named by their kind |
| 14 | frozen tracks (`frozen`) | none frozen |
| 15 | reversed clips (`reversed_from`) | none reversed |
| 16 | what of frozen audio plays (`frozen.segments`) | all of it (a freeze as rendered) |
| 17 | clip fades (`fade_in_sec`, `fade_out_sec` and their curves) | no fades |
| 18 | racks' macros: how many and their names (`macro_names`) | the macros a rack uses (mapped, or turned from 0), and at least 4, named by number (racks had eight) |

`folded_devices` has no version of its own: files without it load with no device folded. Nor have
`chain_lists_shown` and `rack_devices_hidden`: files without them show no chain list, and every rack's devices. The
project key, `automation_locked` and the clip fields default the same way.

**Legacy warp mode names.** Projects saved with the earlier Ableton-style names load into the mode that plays the same
way (`legacyWarpMode`): Beats → Transients, Tones → Standard, Complex → Standard, Texture → Smooth, Complex Pro →
Formants. An unknown name loads as Standard.

## Repairs on load

A file edited by hand (or by an older bug) may hold routing the project can't have. Loading fixes it rather than
refusing the file:

- `repairTree` takes tracks out of groups they can't be in.
- `repairRouting(tracks, returns, master)`:
  - sends to a return that isn't there are dropped;
  - returns' sends are put back one by one, each checked against those before it; one that would close a cycle is
    dropped;
  - each audio track's `input_track` is kept only if the source exists (a track, a return or the master) and doesn't
    close a cycle; then its device channels are cleared;
  - sidechains are put back one by one on every owner's devices (racks too), dropped if the source isn't a track or
    return of the project or would close a cycle.
- Rack macro mappings to a device not in the rack, or to a macro the rack doesn't have, are dropped. A rack has 1 to
  16 macros (`macro_names` longer than 16 is cut there; an empty one gives the default 4), and a value for each.
- Sends' and chains' levels are clamped to the faders' range (-70..+6 dB), pans to -1..1; a MIDI channel outside
  0..16 loads as 0 (every channel); an input of more than two channels loads as none; `monitor` must be a known mode.
- `armed` is ignored on a group.

A field of the wrong type, or a required one missing, makes the file damaged: `loadProject` throws
`ProjectFileError("<file name> is damaged: <what>")` (`loadInto`, given JSON rather than a file: "The project is
damaged: ..."), and a file that isn't JSON "Could not read <file name>: ...". The details say what was
wrong ("could not convert string to float: ...").

## What is saved and what isn't

Saved: everything in the model, including view state that isn't undone: track heights, folded tracks and devices,
which automation shows, arming, solo (tracks and rack chains), Lock Envelopes.

Not saved:

- Undo history (`QUndoStack`).
- Automation overrides: a target changed by hand plays its automation again after loading (the override lives in
  the bridge, not the model).
- The selection, insert marker, playhead, zoom and scroll.
- Decoded audio, waveforms and peaks (decoded again on load).
- Audio files themselves: clips refer to them (`path` and `relative_path`). Recorded takes are WAV files in the
  project's `Recordings` folder, referred to the same way; frozen audio and reversed copies are in its `Freeze` and
  `Reversed` folders.
- Preferences (audio device, MIDI inputs, audio threads, record quantization, count-in, browser places, plug-in
  folders, recent projects): QSettings, per user.
- A plug-in's `Device::state` is only as recent as the last `storePluginStates()`; the project file is always written
  right after one.

## Presets (.gilpreset)

A device's save button saves it (a rack with everything in it) as a preset in the user's library (below); the browser
lists them, and right-clicking beside the devices loads a file (see [guide/devices.md](../guide/devices.md#presets)).

```json
{"format": "gilstudio-preset", "version": 1, "device": { ...a device, as in a project... }}
```

- `deviceToPreset(device)`: the device as the project file stores it (a rack with its chains, the devices in them
  with plug-ins' states as last stored, its macros), with every `sidechain` stripped (they name tracks of the project
  it came from). The session stores the plug-ins' states first.
- `presetDevice(data)`: checks `format` ("Not a SUBstation preset") and `version` (`kPresetVersion` = 1; a newer one is
  refused), refuses racks nested deeper than `kMaxRackDepth` ("The preset nests racks too deep"), makes a damaged one
  `ProjectFileError("The preset is damaged: ...")`, clears sidechains and gives the device and everything in it
  **fresh ids** (`refreshIds`, which also renames macro mappings). So a preset loaded twice is two racks.
- `savePreset` / `loadPreset` write and read the file (`QSaveFile`, as projects). `loadPreset` names a rack after the
  file.
- A preset of a plug-in that isn't installed (or of a built-in device this version doesn't have) loads as is: the
  bridge gives it no processor and reports it in `pluginErrors`, as for a project, and the rest of a rack around it
  works.

### The library (io/Presets.h)

- `libraryDir()`: `Documents/SUBstation/Presets` in the user's home folder, or `SUBSTATION_PRESETS` (the tests set
  it).
- A folder per kind of device, named by `groupOf(device)`: `kindName` as a file name (a plug-in's name, *Utility*,
  *Audio Effect Rack*, *Instrument Rack*).
- `presetFileName(name)`: characters Windows refuses become `_`; trailing dots and spaces go; an empty or reserved
  name (`CON`, `NUL`...) throws `EditError`.
- `presetPath(device, name)`, `saveToLibrary(device, name)` (makes the folder, replaces a preset of that name),
  `listPresets()` (`PresetFile{path, name, group}`, by group then name, ignoring case; files straight in the library
  have group `""`), `renamePreset(path, name)` (`EditError` on a clash; a change of case is none).
- The browser's Presets section is the library as `PresetIndex` ([app/src/browser](../../app/src/browser)) lists it:
  at start, after the application saves, renames or deletes a preset, and when the library's folders change on disk
  (it watches them). The session hands the list to the browser (`BrowserController::setPresets`).
- Loading into an existing device: `loadsInto(preset, device)` ([Devices.h](../../app/src/model/Devices.h): the same
  kind; the same plug-in uid; a rack into a rack, instrument racks only into instrument racks) and
  `ProjectEditor::loadPresetInto(track, device, preset, text)`: one undo step. A plug-in takes the preset's state
  (`SetDeviceStateCommand`; the state before is the model's, so the plug-in's is stored first), a built-in device its
  parameters (`SetDeviceParamsCommand`) and state, a rack its chains, macros and macro values (dropping the automation
  of the devices that leave). The device keeps its id, place, switch and sidechain.
- Rack names: `Device::name` (racks only; `deviceName` shows it, `kindName` is the kind's name, which groups presets).
  `loadPreset(path)` names a rack after the file; saving a rack from the device view names it after the preset
  (`renameRack`, an undo step); `loadPresetInto` gives the rack the preset's name. Saved in projects and presets as
  `"name"` (version 13; absent before).
- Default presets: `defaultPath(kind, plugin)` (`Defaults/<device name>.gilpreset`, or
  `Defaults/<plug-in name> (<uid>).gilpreset`; none for racks), `saveDefault`, `hasDefault`, `clearDefault`, and
  `defaultDevice(kind, plugin)`: a new device as the default has it (fresh ids; a plug-in keeps the `PluginRef` asked
  for; a built-in device's missing parameters at their defaults), or none if there is none, it can't be read, or it is
  for another device. `listPresets` skips the `Defaults` folder (a device whose name is "Defaults" groups under
  "Defaults (Device)"). `ProjectEditor::setDeviceDefaults` takes it (the session wires `defaultDevice`); `addDevice`
  and `addMidiTrack` make new devices through it. Pasted, duplicated and loaded devices don't use it.

Plug-ins' own `.vstpreset` files (*Load VST3 Preset…* / *Save VST3 Preset…* on a plug-in) are a different thing: the
engine reads and writes them (`EngineBridge::applyPluginState`, `pluginState`), and loading one is an undoable
`SetDeviceStateCommand` (`DeviceSelection`, [session.md](session.md#the-device-view-deviceselection)).

## Extending the format

1. Add the field to the model type with a default.
2. Write it in `projectToJson` (or `deviceToJson`, or the clip's or track's writer in `Serialization.cpp`) and read it
   with its default when it is missing, validating and clamping.
3. If it refers to other tracks or devices, drop what can't be in `repairRouting` (or where the device is read), and
   in `presetDevice` if it can't travel with a preset.
4. Bump `kProjectVersion`, add the change to the comment beside it and to the header's comment, and add a test that
   an older file still loads.

## Gotchas

- Loading replaces the project only after everything parsed, so a damaged file leaves the open project as it was.
- `projectToJson` writes `relative_path` relative to the folder it is *saved to*, so Save As to another folder writes
  paths relative to the new place.
- A built-in device's state may hold absolute paths (the Sampler's sample), which are not made relative.
- A plug-in device's `params` are saved too, but loading restores a plug-in from its `state` only; its `params` serve
  undo.
- `folded_devices` drops ids of devices that no longer exist, so the list never grows stale.

## Tests

- [test_serialization.cpp](../../tests/app/test_serialization.cpp): round trips, the relative path fallback when a
  folder moves, foreign files refused, old warp mode names, older versions loading unchanged (and saving as the
  current version), the master's devices, inputs, monitoring and arming, what an edited file can't have; a file using
  every feature, as earlier versions wrote it, loading and saving the same.
- [test_presets.cpp](../../tests/app/test_presets.cpp): the library (saving by name, listing by device, renaming),
  default presets, presets as new devices, rack names.
- [test_midi_model.cpp](../../tests/app/test_midi_model.cpp): MIDI tracks and inputs round trip.
- [test_editor_groups.cpp](../../tests/app/test_editor_groups.cpp),
  [test_editor_sends.cpp](../../tests/app/test_editor_sends.cpp),
  [test_editor_resampling.cpp](../../tests/app/test_editor_resampling.cpp),
  [test_editor_sidechain.cpp](../../tests/app/test_editor_sidechain.cpp),
  [test_editor_racks.cpp](../../tests/app/test_editor_racks.cpp),
  [test_editor_freeze.cpp](../../tests/app/test_editor_freeze.cpp): each feature saved and loaded.
- [test_editor_presets.cpp](../../tests/app/test_editor_presets.cpp),
  [test_session_devices.cpp](../../tests/app/test_session_devices.cpp),
  [test_session_engine.cpp](../../tests/app/test_session_engine.cpp): presets loaded into devices, saved from the
  device view, rendering as the devices they were saved from.
- [test_session_files.cpp](../../tests/app/test_session_files.cpp): saving and opening through the session.
