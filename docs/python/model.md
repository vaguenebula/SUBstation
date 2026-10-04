# Project model and undo

The project model is the single source of truth for the UI, undo and saving. It lives in
[src/substation/model/](../../src/substation/model): plain dataclasses for tracks, clips,
notes, devices and envelopes, a `Project` QObject that holds them and signals every change,
pure functions that do the editing maths, and a `ProjectEditor` that turns each user action
into undo commands. The audio engine only mirrors this model (see
[engine-bridge.md](engine-bridge.md)); saving and loading are in
[serialization.md](serialization.md).

## Overview

```
 UI widget ──calls──► ProjectEditor (editor.py)
                          │  works out the new state with the pure modules:
                          │  edits.py (clips), notes.py (notes), automation.py (envelopes)
                          ▼
                     QUndoCommand (commands.py) ──pushed onto──► QUndoStack (owned by MainWindow)
                          │ redo() / undo()
                          ▼
                     Project mutators (project.py) ──emit──► Qt signals
                                                               │
                              ┌────────────────────────────────┼─────────────────────┐
                              ▼                                ▼                     ▼
                        UI repaints                  EngineBridge pushes      the title bar's "*"
                                                     the change to C++        (undo stack not clean)
```

- **Undoable state** changes only through commands. `Project`'s mutating methods are meant
  to be called by `redo()`/`undo()` and nothing else, so every edit is undoable and every
  change is signalled.
- **View state** (track heights, folding, which automation shows, arming, solo, Lock
  Envelopes) is saved with the project but changes directly, without an undo step, as in
  Ableton.
- Everything runs on the Qt main thread. Nothing in `model/` touches the engine except
  `editor.py`, which asks `substation._engine` for the list of built-in devices at import
  time.

## Files

| File | What it holds |
|---|---|
| [project.py](../../src/substation/model/project.py) | `Project` (QObject, signals), `Track`, `Clip`, `MidiClip`, `Note`, `Device`, `Chain`, `PluginRef`, `Sidechain`, `MacroMapping`, `Send`, `MidiInput`; the group tree (`tree_problem`, `repair_tree`); the routing graph (`routing_graph`, `feeds`, `would_cycle`, `input_would_cycle`, `sidechain_would_cycle`); device-tree helpers (`iter_devices`, `iter_chains`, `device_path`, `find_device`, `chain_devices`, `container_of`, `rack_depth`, `rack_height`, `refresh_ids`); constants (`WARP_MODES`, `LEGACY_WARP_MODES`, `TRACK_COLORS`, `MAX_RACK_DEPTH`, `MACRO_COUNT`...) |
| [edits.py](../../src/substation/model/edits.py) | Pure clip maths, for audio and MIDI clips alike: overlaps, cuts, trims, splits, ranges, tempo fitting, consolidate |
| [notes.py](../../src/substation/model/notes.py) | Pure note maths for the piano roll: overlaps on a key, moves, resizes, velocity, legato, ×2/÷2, quantize, humanize; note names |
| [automation.py](../../src/substation/model/automation.py) | Envelopes (`AutomationPoint`, `Envelope`), target keys, the mixer's normalized mappings, evaluation (`value_at`), and every envelope edit (points, curves, ranges); `AutomationView` |
| [params.py](../../src/substation/model/params.py) | `ParamSpec`: any automatable parameter (mixer, built-in device, plug-in, rack chain fader) described alike, with the engine's normalized mapping; `mixer_specs`, `send_spec`, `chain_specs`, `format_value` |
| [editor.py](../../src/substation/model/editor.py) | `ProjectEditor`: every undoable operation the UI uses; device helpers (`new_device`, `device_name`, `device_is_instrument`, `BUILTIN_DEVICES`...); clipboard types (`ClipboardContent`, `CopiedAutomation`); `RecordedTake` |
| [commands.py](../../src/substation/model/commands.py) | The `QUndoCommand` subclasses; merging of continuous gestures |
| [timebase.py](../../src/substation/model/timebase.py) | `TimeSignature`, beats/seconds, bar.beat.sixteenth formatting and parsing, dB and pan text |
| [keys.py](../../src/substation/model/keys.py) | Musical keys (`Key`), reading tempo and key from file names (`parse_filename`), what a dropped clip starts with (`clip_settings`) |
| [device_state.py](../../src/substation/model/device_state.py) | A built-in device's state besides its parameters (a sampler's sample), in the engine's text format, base64 in `Device.state` |
| [serialization.py](../../src/substation/model/serialization.py) | `.gilproj` and `.gilpreset` files: see [serialization.md](serialization.md) |
| [sidechain_fit.py](../../src/substation/model/sidechain_fit.py) | Fitting the Sidechain device's curve to a kick: `Capture` (its displays, by absolute index, and the hits), `spectra` (where the kick and the input clash), `clash_envelope`, `reduction`, `fit_points`, `analyze` → `Fit`; pure numpy |
| [presets.py](../../src/substation/model/presets.py) | The user's preset library: `library_dir`, `save_to_library`, `list_presets`, `rename_preset`; see [serialization.md](serialization.md#the-library-presetspy) |

## Key types

### Time

Timeline positions are quarter-note **beats** (`timebase.py`). `TimeSignature` gives
`beats_per_bar` and `beat_length` in quarter notes; `format_position`/`parse_position` use
Ableton's one-based `bars.beats.sixteenths`. `db_to_gain` treats -70 dB and below as silence
(the faders' floor, `automation.MIN_VOLUME_DB`). Pan is -1..1 in the model and shows as
`25L`/`C`/`30R` (±50).

### Clips and notes

Both kinds of clip are frozen dataclasses: an edit makes a new clip (`dataclasses.replace`).
Each is a *window* onto its content, and editing moves the window's edges while the content
stays put on the timeline.

- `Clip` (audio): `path`, `start_beat`, and in **source seconds** `offset_sec`,
  `duration_sec` and `source_duration_sec`; clip-view settings `gain_db`, `warp`,
  `warp_mode`, `segment_bpm`, `transpose`, `detune` (cents), `pan`. Unwarped audio plays at
  its own speed, so the clip's length in beats follows the tempo; warped
  (`is_warped`: `warp` and `segment_bpm > 0`), the audio is taken to be at `segment_bpm`, so
  its length in beats is fixed. `source_tempo()`, `beats_to_source()` and
  `source_to_beats()` convert between the two. `segment_bpm` 0 means "not set" (shown as the
  project tempo).
- `MidiClip`: `start_beat`, `duration_beats`, `offset_beats` (the content beat at the clip's
  start) and `notes`, a tuple of `Note` sorted by start then pitch. Its length is in beats,
  so it doesn't change with the tempo. Notes outside the window are kept but not played, so
  trimming or splitting never deletes them. `played_notes()` gives the notes that start
  inside the window, cut at its end, in timeline beats: as in Ableton.
- `Note`: `pitch` (60 = C3, Ableton's octave numbering), `start` and `length` in beats from
  the clip's content start, `velocity` 1..127.
- `WARP_MODES` is in the engine's `WarpMode` order: Transients, Standard, Smooth, Formants,
  Re-Pitch. `LEGACY_WARP_MODES` maps the earlier Ableton-style names (Beats, Tones, Complex,
  Texture, Complex Pro) to the mode that plays the same way; serialization applies it.

### Tracks

`Track` is a mutable dataclass. Its `kind` is fixed for its life:

| kind | Where it lives | Clips | Notes |
|---|---|---|---|
| `"audio"`, `"midi"` | `project.tracks` | yes | `has_clips` |
| `"group"` (`GROUP_KIND`) | `project.tracks` | no | holds other tracks |
| `"return"` (`RETURN_KIND`) | `project.returns` | no | fed by sends; named by letter (`return_letter`: A..Z, AA...) |
| `"master"` (`MASTER_KIND`) | `project.master` | no | id `MASTER` (`"master"`), effects only |

Fields: `name`, `color`, mixer (`volume_db`, `pan`, `mute`, `solo`), `height`, `clips`
(sorted by `start_beat`), `devices`, `automation` (target key → envelope, never empty),
`automation_view`, audio input (`input`: `()`, `(c,)` or `(l, r)` device channels, or
`input_track`: another track's id or `MASTER` for resampling), `midi_input` (`MidiInput`, or
None for *No Input*; new MIDI tracks hear every input), `monitor` (`MONITOR_MODES`: `off`,
`in`, `auto`), `armed`, `parent` (the group it is in), `folded`, `sends` (return id →
`Send`).

The master and the returns are `Track`s too, so whatever works on a track's devices, mixer
or automation works on theirs: `project.track(id)` finds an arrangement track, a return, or
the master (`MASTER`). `has_track()` is true only for arrangement tracks; `has_owner()` for
anything with devices, a mixer and automation. `all_tracks()` is tracks, returns, master;
`owners()` the same ids; `senders()` tracks then returns.

### Devices, racks, chains

`Device` (mutable): `id`, `kind`, `enabled`, `params` (param id → plain value), `plugin`,
`state`, `sidechain`, and for a rack `chains` and `macros`.

- **Built-in** (`kind` `"synth"`, `"utility"`, ...): its state is its `params`, and `state`
  for what isn't a parameter (see `device_state.py`). The engine follows the model. The
  list of built-in devices, their names, categories, default parameters and which are
  instruments come from the engine (`ge.builtin_devices()` → `BUILTIN_DEVICES`,
  `BUILTIN_CATEGORIES`, `BUILTIN_INSTRUMENTS` in editor.py), so a new device needs nothing
  in Python. `DEFAULT_INSTRUMENT` is `"synth"`.
- **Plug-in** (`kind` `"plugin"`, `PLUGIN_KIND`): `plugin` is a `PluginRef` (`format`
  `"VST3"`, `uid` the class id, `name`, `vendor`, `path` where it was last loaded,
  `instrument`). The plug-in keeps its own state; `state` holds it (base64 `.vstpreset`) as
  last stored, for saving and copying. Its `params` only record values changed from the
  host, for undo.
- **Rack** (`kind` `"rack"`, `RACK_KIND`): `chains` is a list of `Chain` (`id`, `name`,
  `devices`, and a mixer: `volume_db`, `pan`, `mute`, `solo`; while any chain of a rack is
  soloed, only those are heard). Its `params` are its macros' values, `"macro1"`..`"macro8"`
  (`macro_param(i)`, `MACRO_COUNT` = 8), normalized 0..1. `macros` is a tuple of
  `MacroMapping(macro, device_id, param_id, low, high)`: the macro's 0..1 maps to the
  parameter's normalized `low`..`high` (`low > high` turns it the other way). Racks nest at
  most `MAX_RACK_DEPTH` = 8 deep (the engine's limit). A rack with an instrument in it is an
  *Instrument Rack* (`device_is_instrument`), otherwise an *Audio Effect Rack*.

Device ids are unique in the whole project, so a device is found wherever it sits:
`project.device(track_id, device_id)` and `find_device()` search racks too,
`project.device_owner(device_id)` finds its track. Tree helpers:

- `iter_devices(devices)`: every device depth first (each, then what is in its chains).
- `iter_chains(devices)`: every `(rack, chain)` depth first.
- `device_path` / `device_at`: a device's place as `(index, chain, index, chain, ...)`.
- `chain_devices(devices, chain_id)`: the list object of a chain (None = the track's own),
  which edits mutate in place on a deep copy.
- `container_of`: the chain a device is in (None = the track's own).
- `rack_depth(devices, chain)`: how many racks a chain is in; `rack_height(device)`: how deep
  racks nest inside a device. `rack_depth + rack_height ≤ MAX_RACK_DEPTH` is checked before
  any insert or move.
- `refresh_ids(device)`: new ids for a device, everything in it and its chains, in place;
  macro mappings follow their devices and mappings to devices not in it go. Used for presets,
  pasted devices, duplicated chains and duplicated tracks.

### Groups

Groups hold other tracks. `project.tracks` stays **flat**; the hierarchy is each track's
`parent`. The invariant (`tree_problem`) is that a track's parent is a group listed before
it, and everything between a group and its last descendant is a descendant of it: a group's
tracks follow it, together, and no group is in itself. `repair_tree` takes tracks out of
groups they can't be in (an edited file). Queries: `subtree_end`, `descendants`, `children`,
`ancestors`, `is_descendant`, `depth`, `is_hidden` (a group it is in is folded),
`parent_at(index)` (a track inserted there goes into the group of the track it goes before),
`tree()` (every track's `(id, parent)`: a `TrackTree`). The engine sees only where each
track's output goes.

### Returns, sends, inputs, sidechains: the routing graph

`Send(level_db, pre_fader)`: a track's (group's, return's) send to a return, after its fader
and pan, or before it. Silent at `MIN_VOLUME_DB` (the default for a new send). A muted track
sends nothing either way. `Track.sends` is replaced whole, never changed in place.

`Sidechain(track_id, tap)`: what a device's aux input hears. `tap` is `POST_FADER`
(`"post"`, Ableton's *Post Mixer*), `PRE_FADER` (`"pre"`, *Post FX*), `PRE_FX`
(`"pre-fx"`: before all the source's devices; on a MIDI track, after its instrument), or a
device id (after that device; before the fader while that device isn't on the source).
`tap_device` is that id or None.

`routing_graph(tracks, returns)` maps each track and return to what its signal goes into: its
group, the returns it sends to, the tracks taking their input from it, and the tracks whose
devices take it as their sidechain. The master isn't in it (everything reaches it).
`feeds(graph, a, b)` is reachability. The cycle checks:

- `would_cycle(track, return)`: a send would close a cycle (the return is the track or feeds it).
- `input_would_cycle(track, source)`: never for `MASTER` (resampling; a track recording it
  never plays it back into it, since it can't monitor it).
- `sidechain_would_cycle(track, source)`: always for `MASTER` as source; never for a device
  on the master.

`Project` wraps them and adds the menus' lists: `send_targets`, `input_sources`,
`sidechain_sources`, `input_name` (`"Resampling"` for the master).

### Automation

See [automation.py](../../src/substation/model/automation.py) and, for the engine side,
[engine/automation.md](../engine/automation.md).

- Every target is automated in **normalized** values (0..1). An envelope is a tuple of
  `AutomationPoint(beat, value, curve)`; it belongs to an owner (a track id or `MASTER`) and
  is keyed by target:
  - `mixer:volume`, `mixer:pan` (`MIXER_VOLUME`, `MIXER_PAN`)
  - `send:<return id>` (`send_key`): the level of the owner's send to a return
  - `device:<device id>:<param id>` (`device_key`)
  - `device:<rack id>:chain:<chain id>:volume` / `:pan` (`chain_key`): a rack chain's fader,
    a target of its rack
- `parse_key` splits a key; `key_device`, `key_send`, `key_chain`, `key_chain_control`,
  `is_mixer_key`, `is_key` classify it.
- Before the first breakpoint an envelope holds the first one's value, after the last the
  last one's; two points at one beat make a step (from that beat on, the later one counts:
  `value_at` vs `left_value`).
- `curve` (-1..1) bends the segment starting at that point; segments are exponential with
  `CURVATURE` = 6 (the engine's `kAutomationCurvature`), so `split_at` can split one into two
  that follow it exactly. `shape()` must match the engine.
- Mixer mappings: volume is a cube-law fader, 1 = `MAX_VOLUME_DB` (+6 dB), 0 dB at about
  0.79, 0 = silence (`volume_to_normalized`, `normalized_to_volume`); pan maps -1..1 to 0..1.
  Sends and chain volumes map as volume.
- Edits (each returns a new envelope): `add_point`, `move_points_mapped` (one point stays
  between its neighbours; several override what they land on), `delete_points`, `set_curve`,
  `remove_range`, `copy_range`, `paste_range`, `move_range`, `drop_redundant`, `shift`,
  `normalize` (sort, clamp).
- `AutomationView(shown, key, lanes)`: what the arrangement shows of an owner's automation
  (view state).

### Parameters

`ParamSpec` (params.py) describes a parameter whatever it belongs to: `key` (its automation
key), `name`, `group`, range, `default`, `unit`, `scale` (`linear`, `log`, or `fader` for a
mixer volume), `steps` (discrete), `labels` and an optional `text` formatter.
`to_normalized`/`from_normalized` are the same mapping as the engine's `ParamInfo`
(evenly, in log(value), or in whole steps as VST3 maps stepped parameters), and the tests
hold the two together. `ParamSpec.from_info()` builds one from an engine `ParamInfo`;
`mixer_specs`, `send_spec` and `chain_specs` describe the mixer, sends and rack chain
faders. The bridge hands them to the UI (`EngineBridge.param_groups`).

### Keys and file names

`Key(tonic, minor)`; `Key.name` (`"F#m"`, `"Bb"`) is what projects save and `key_from_name`
reads. `transpose_to(source, target)` is the smallest shift (a minor key and its relative
major count as the same; at a tritone it goes down), 0 if either is unknown.
`parse_filename` finds a tempo (`128bpm`, `bpm128`, or a lone number in 50..250) and a key
(spelled `A minor`, `Ebmaj`; short `Am`, `F#m`; a bare capital only next to a tempo or after
"key", never as the first word). `clip_settings(name, duration, tempo, project_key)` gives
a new clip `warp` and `segment_bpm` when the name has a tempo or the file is at least
`AUTO_WARP_MIN_SEC` (6 s) long (then at the project tempo), and a `transpose` to the project
key. Used by `ProjectEditor.add_clips`. User-facing behaviour:
[guide/audio-clips.md](../guide/audio-clips.md).

### Built-in device state

`device_state.py` encodes a built-in device's non-parameter values as the engine does
(`BuiltinProcessor::encodeState`): `name=value` lines, a backslash escaping a backslash or a
newline. `to_model`/`from_model` wrap it in base64 for `Device.state`. The Sampler's one
value is `"sample"`, its file path (see
[ui/device-view.md](../ui/device-view.md)).

## The Project object and its signals

`Project(QObject)` holds `tempo`, `time_signature`, `key`, the loop (`loop_enabled`,
`loop_start`, `loop_end`, beats), `automation_locked`, `master`, `tracks`, `returns`,
`folded_devices` (ids, view state) and `path`.

| Signal | When |
|---|---|
| `track_inserted(id, index)`, `track_removed(id, index)` | an arrangement track came or went |
| `return_inserted(id, index)`, `return_removed(id, index)` | a return came or went |
| `track_changed(id)` | name, colour, mixer, sends, input, monitoring, arming, height, folding (`MASTER`: the master's mixer) |
| `tracks_arranged()` | the order or groups changed (not which tracks there are) |
| `clips_changed(track id)` | a track's clips were replaced |
| `devices_changed(track id)` | devices added, removed, moved, toggled (in racks too), a sidechain or macros changed |
| `chain_changed(track id, chain id)` | a rack chain's name or mixer |
| `device_param_changed(track id, device id, param id)` | one parameter |
| `device_state_changed(track id, device id)` | a device's state was set (a preset, a sample) |
| `devices_folded(track id)` | devices on it folded or unfolded |
| `settings_changed()` | tempo, time signature, key, loop, automation lock |
| `automation_changed(owner, key)` | an envelope was set or removed |
| `automation_view_changed(owner)` | what its automation shows |
| `reset()` | everything replaced (new, open) |

Mutators, called only from commands (or directly for view state): `insert_track`,
`remove_track`, `insert_return`, `remove_return`, `arrange_tracks(tree)` (validates with
`tree_problem` and raises `ValueError` changing nothing), `update_track(**attrs)` (refuses
`id`, `kind`, `clips`, `devices`, `automation`, `automation_view`, `parent`), `set_clips`
(sorts), `set_devices`, `set_chains` (several tracks' devices at once, all changed before
any signal: a device moving between tracks), `update_chain`, `update_device` (macros only),
`set_device_param(s)`, `set_device_enabled`, `set_device_sidechain`, `set_device_state`,
`set_devices_folded`, `update_settings`, `set_envelope` (an empty envelope removes the
target's automation), `set_automation_view`, `replace_contents` (used by loading; emits
`reset`), `clear`.

## The undo design

`MainWindow` owns one `QUndoStack` and one `ProjectEditor(project, undo_stack)`. The editor
pushes commands from [commands.py](../../src/substation/model/commands.py):

| Command | Changes |
|---|---|
| `SetClipsCommand` | clip lists of one or more tracks (`before`/`after` by track id) |
| `InsertTrackCommand`, `RemoveTrackCommand` | an arrangement track |
| `InsertReturnCommand`, `RemoveReturnCommand` | a return track |
| `ArrangeTracksCommand` | order and groups (`TrackTree` before/after) |
| `UpdateTrackCommand` | one track attribute (also `sends`, `input_track`, `midi_input`, `monitor`) |
| `UpdateTrackFieldsCommand` | several attributes of one track as one change (input channels and input track) |
| `UpdateTracksCommand` | one mixer setting on several tracks |
| `SetTempoCommand` | tempo plus the clip trims that keep unwarped clips from overlapping |
| `UpdateSettingsCommand` | time signature, key, loop |
| `SetDevicesCommand` | a track's whole device tree |
| `SetChainsCommand` | several tracks' devices at once (a device moving between tracks stays the same device) |
| `SetDeviceParamCommand`, `SetDeviceParamsCommand` | one parameter; several at once (a macro and what it moves) |
| `UpdateChainCommand`, `SetMacrosCommand` | a rack chain's name or mixer; a rack's macro mappings |
| `SetDeviceStateCommand` | a device's state (a plug-in preset, a sampler's sample) |
| `SetDeviceEnabledCommand`, `SetDeviceSidechainCommand` | on/off; sidechain |
| `SetEnvelopeCommand`, `SetEnvelopesCommand` | one envelope; several (a range moved on several lanes) |

How it fits together:

- **Snapshots, not deltas.** Commands store whole before/after values (a clip list, a
  device tree, an envelope). Commands that hold mutable devices or tracks deep-copy them on
  every `redo()`/`undo()` (`SetDevicesCommand`, `SetChainsCommand`, `InsertTrackCommand`,
  `InsertReturnCommand`), so the stack's copies are never aliased by the live model.
  `RemoveTrackCommand` keeps the removed object itself and puts it back.
- **Compound steps** use `undo_stack.beginMacro()`/`endMacro()`: deleting tracks (inputs and
  sidechains from them dropped, the tracks, the sends into deleted returns and their
  automation), grouping, ungrouping, adding clips (with a new track), moving clips with
  their automation, deleting devices with their automation, recording.
- **Gestures merge.** A command given a `merge_key` returns id `_MERGE_ID` (0x6E1), and
  `mergeWith` accepts the next command of the same class with the same `merge_key` and the
  same target (`key`), keeping the first `old` and the latest `new`. A fader drag, a knob
  drag, a breakpoint drag, a plug-in editor knob drag (`("plugin edit", device id, param id,
  gesture)`) are each one undo step. `SetClipsCommand` also needs the same set of tracks.
- **Baselines for drags that trim.** `set_tempo` and `update_clips` (segment BPM, warp) trim
  clips that would run into the next one (`edits.fit_to_tempo`). While a drag merges, they
  fit from the clips as they were when the drag began (the previous merged command's `old`),
  so dragging up and back down doesn't leave clips trimmed.
- **Not undone** (changed directly on the project; saved with it): track `height`, `folded`,
  `folded_devices`, `armed`, track and chain `solo` (`solo_tracks`, `set_chain_param("solo")`),
  `automation_view`, and `automation_locked`. These push nothing, so they don't mark the
  project as changed either.
- **Clean state.** `MainWindow` uses `QUndoStack.isClean()` for the title's `*` and the save
  prompt, `setClean()` after saving, and `resetClean()` when a plug-in reports a change no
  parameter shows (`EngineBridge.plugin_state_dirty`).

### ProjectEditor

[editor.py](../../src/substation/model/editor.py) is the API the UI calls. Its signals:
`plugin_added(track id, device id)` when the user adds a plug-in (the UI opens its editor;
not on undo or redo), and `parameter_touched(owner, key)` when the user changes or clicks a
parameter that can be automated (the arrangement shows its lane).

Main operations, by area:

- **Tracks**: `add_audio_track`, `add_midi_track` (with the default Synth or a plug-in
  instrument), `delete_tracks` (a group with what is in it, a return with the sends into it
  and their automation; inputs and sidechains from them go: one step), `duplicate_tracks`
  (new ids for tracks, clips, devices and rack chains; automation keys, automation view,
  sidechains and inputs among the copied tracks renamed to the copies; not armed),
  `rename_track`, `set_track_color`, `set_track_param` / `set_tracks_param` (mixer;
  the master only volume and pan), `solo_tracks`, `arm_tracks`, `set_track_height`,
  `set_folded`, `insertion_point`.
- **Inputs**: `set_track_input` (device channels), `set_track_input_track` (raises
  `ValueError` for a cycle or a non-audio track), `set_track_monitor`, `set_track_midi_input`.
- **Groups**: `group_tracks` (Ctrl+G), `ungroup` (Ctrl+Shift+G), `move_tracks` /
  `can_move_tracks`. They go through `_arrange`, which first drops any input or sidechain the
  new tree would turn into a cycle, in the same undo step.
- **Returns and sends**: `add_return_track`, `set_send` (raises `ValueError` for a cycle),
  `remove_send`.
- **Settings**: `set_tempo` (20..999, rounded to 0.01), `set_time_signature`, `set_key`,
  `set_loop`, `set_loop_enabled`, `set_automation_locked`.
- **Clips**: `add_clips` (audio files one after another, set up by `clip_settings`; a new
  audio track if needed), `add_midi_clip`, `add_midi_clips_over`, `midi_clip_span`,
  `set_clip_notes`, `move_clips` (with `clamp_track_delta`: only onto tracks of the same
  kind), `replace_clip`, `update_clips`, `delete_clips`, `split_clips`, `duplicate_clips`,
  `consolidate_clips` (Ctrl+J), time selections (`delete_range`, `duplicate_range`,
  `copy_range`, `cut_range`, `paste`, `paste_targets`, `move_range`, `clips_area`,
  `clips_in_range`, `clips_at`), and `add_recordings` (takes become clips in one step; MIDI
  takes quantized to the record grid).
- **Automation carried by clips**: unless `automation_locked`, moving, copying, duplicating,
  cutting and pasting clip content takes the automation under it along
  (`_carried_automation`): only envelopes with breakpoints under the clips; across tracks only
  the mixer's (a device's automation belongs to its track).
- **Devices**: `add_device`, `insert_device(s)` (an instrument only on a MIDI track, first,
  replacing the one there; effects never before it), `copy_devices`, `paste_devices`
  (sidechains kept unless the source is gone or would close a cycle; folded copies stay
  folded), `move_device(s)`, `move_devices_to_track` (the same devices, so plug-ins keep
  their state; their automation moves with them in the same step), `remove_device(s)`,
  `set_device_param` (`old` passed in for plug-ins, whose values the model doesn't hold),
  `set_device_params` (several of a built-in device's at once: one step, merging per gesture
  while the same parameters change),
  `set_device_state`, `set_device_enabled`, `set_device_sidechain`, `set_devices_folded`,
  `touch_parameter`, `load_preset_into` (a preset into a device of its kind: `loads_into`),
  `add_midi_track_with` (a new MIDI track with an instrument preset, one step),
  `rename_rack`, `set_device_defaults` (where new devices come from: default presets). `_set_devices` deletes the automation of devices (and rack chains) that
  left the track, and macro mappings to them, in the same step.
- **Racks**: `group_devices` (Ctrl+G: devices of one chain into a new rack), `ungroup_rack`
  (refused if several instruments would come out), `add_rack_chain`, `remove_rack_chains`,
  `duplicate_rack_chain`, `move_rack_chain`, `rename_chain`, `set_chain_param`,
  `map_macro`, `unmap_macro`, `macro_of`, `macro_targets`, `set_macro` (the macro and every
  mapped parameter in one `SetDeviceParamsCommand`).
- **Envelopes**: `set_envelope`, `add_automation_point`, `move_automation_points`,
  `delete_automation_points`, `set_automation_curve`, `clear_envelope`,
  `delete_automation_range`, `move_automation_range`, `duplicate_automation_range`,
  `copy_automation_range`, `cut_automation_range`, `paste_automation`,
  `automation_paste_targets`.
- **Automation view** (not undone): `show_automation`, `hide_automation`,
  `toggle_all_automation` (A), `add_automation_lane`, `set_automation_lane`,
  `remove_automation_lane`, `reset_automation_view`, `default_automation_key`.

Two hooks connect it to the engine without importing the bridge: `set_param_info(describe)`
(how `param_info()` learns a plug-in's parameter mapping, for macros) and
`set_own_value(read)` (a plug-in parameter's current value, set in its own editor, for a
macro's undo). `MainWindow` wires them to `EngineBridge.device_param_info` and
`EngineBridge.own_value`.

## Pure editing modules

`edits.py` and `notes.py` take and return immutable values and never touch Qt or undo, so
they are tested directly.

- `resolve_overlaps(clips, winners, tempo)`: Ableton's rule. Winners keep their place; every
  other clip on the track is trimmed, split or removed where a winner covers it
  (`cut_clip`, `subtract_intervals`). The first piece keeps the clip's id.
- `remove_range`, `slice_range` (new clips holding just a range; `keep_ids` for clips wholly
  inside), `split_clip`, `trim_start` (a MIDI clip revealing time before its content shifts
  its notes so they stay put), `trim_end` (audio limited by the source), `fit_to_tempo`
  (returns the same list object when nothing changed), `consolidate_midi`.
- Minimum sizes: `MIN_CLIP_SEC` (5 ms), `MIN_MIDI_CLIP_BEATS` and `MIN_NOTE_BEATS` (1/64).
- `notes.place(notes, removed, added)` is how the piano roll commits: added notes win where
  they overlap others on their key (`resolve_overlaps`); notes changed together are made
  consistent by `untangle`. `normalize` sorts and removes exact duplicates. `legato`,
  `time_scaled`, `quantized` (`QUANTIZE_GRIDS`), `humanized` (`HUMANIZE_BEATS` = a 32nd,
  `HUMANIZE_VELOCITY` = 24 at 100 %; a triangular random, more often a little than a lot).

## Invariants

- `project.tracks` satisfies `tree_problem(...) is None` at all times; `arrange_tracks`
  refuses anything else.
- The routing graph has no cycles: every place that adds an edge (`set_send`,
  `set_track_input_track`, `set_device_sidechain`, `_arrange`, `move_devices_to_track`,
  `paste_devices`) checks it first, and loading repairs edited files
  (`serialization.repair_routing`).
- Device ids, chain ids and track ids are unique in the project (`new_id()`: 12 hex digits
  of a uuid4); copies always go through `refresh_ids`.
- A track's `automation` never holds an empty envelope; a device's automation lives on the
  track the device is on, keyed by its id.
- On a MIDI track an instrument (or instrument rack) is first in its chain, and there is at
  most one; audio tracks, groups, returns and the master take no instrument.
- Racks never nest deeper than `MAX_RACK_DEPTH`.
- Macro mappings only name devices inside their rack (`_prune_macros`).

## Extending it

- **A new undoable edit**: add a `ProjectEditor` method that computes the new value with the
  pure modules and pushes an existing command; only add a command when no existing one
  replaces the right thing. If it needs a new kind of change on `Project`, add a mutator
  that emits a signal, and handle that signal in `EngineBridge` and the UI.
- **A new track field**: add it to `Track` (with a default so old files load), decide
  whether it is undoable (a command) or view state (direct `update_track`), and add it to
  [serialization.py](../../src/substation/model/serialization.py) (bump `VERSION`; see
  [serialization.md](serialization.md)).
- **A new automation target kind**: a key form in `automation.py` (`parse_key` and the
  classifiers), a `ParamSpec`, its own-value read in `EngineBridge.own_value`, and the
  engine lane in `EngineBridge._engine_lane`.
- **A new built-in device**: nothing here. It comes from the engine
  ([engine/devices.md](../engine/devices.md)).

## Gotchas

- `Project` mutators don't check that a change is undoable; calling them from the UI skips
  undo. Only view state may do that.
- `editor.py` imports `substation._engine` at import time (for the built-in devices), so the
  model can't be imported without a built engine.
- A plug-in's `Device.state` is only as fresh as the last `EngineBridge.store_plugin_states()`.
  Copying devices, duplicating tracks or chains and saving presets must store states first
  (the UI does).
- A plug-in's `params` only hold values the host changed; its real values live in the
  plug-in. Use `EngineBridge.own_value` to read them.
- `Clip.segment_bpm` 0 means "not set": a clip with `warp` on but no BPM isn't warped.
- `MidiClip` methods accept a `tempo` they ignore, so both kinds of clip can be edited alike.
- `Track.sends` must be replaced whole (`{**sends, id: send}`), never changed in place: the
  commands compare old and new dictionaries.

## Tests

- [test_edits.py](../../tests/test_edits.py): overlaps, trims, splits, ranges, tempo fitting,
  moves across tracks, gesture merging, inputs undoable and arming not, recorded takes,
  copy/paste and automation carried with clips.
- [test_midi_model.py](../../tests/test_midi_model.py): MIDI clips and notes, the piano roll's
  tools, MIDI takes.
- [test_automation_model.py](../../tests/test_automation_model.py): envelope maths, the
  parameter mappings against the engine's, undoable edits, deleted devices' automation, lanes
  shown, saving.
- [test_duplicate_tracks_copy_automation.py](../../tests/test_duplicate_tracks_copy_automation.py),
  [test_groups_model.py](../../tests/test_groups_model.py),
  [test_sends_model.py](../../tests/test_sends_model.py),
  [test_resampling_model.py](../../tests/test_resampling_model.py),
  [test_sidechain_model.py](../../tests/test_sidechain_model.py),
  [test_racks_model.py](../../tests/test_racks_model.py): each feature's model rules, undo,
  cycles refused, and through the bridge.
- [test_keys.py](../../tests/test_keys.py), [test_timebase.py](../../tests/test_timebase.py):
  file names, keys, positions and formatting.

See [testing.md](../testing.md) for the whole suite.
