# Intelligence layer

`src/substation/intel/` is the layer that understands the song and acts on it for
callers other than the mouse: suggestions, an in-app assistant and outside agents
(through MCP). [INTELLIGENCE.md](../../INTELLIGENCE.md) is its design; this page is
what is built: **phase 1**, the operations, the rails they run on, and the song
context. Suggestions, the MCP server, analysis and the assistant come in later phases.

```
 caller (test, suggestion, agent)          other threads
        │ run(name, args, actor)                │ call_from_thread
        ▼                                       ▼
 ops/runner.py OpRunner  ◄────────── qt/dispatcher.py MainThreadDispatcher
   checks args (schemas from types), if_revision, recording
   one undo macro "Agent (Claude Code): Add Device"; all or nothing
        │ op.func(ctx, **kwargs)
        ▼
 ops/*.py operations ──writes──► ProjectEditor ─► QUndoStack ─► Project ─► EngineBridge
        │ reads                                                   │ signals
        ├── OpContext.engine: EngineFacts (qt/engine_facts.py on the bridge)
        ├── OpContext.ui: UiFacts (ui/intel/facts.py on the main window)
        └── context/: SongContext, roles, IntelRevision ◄──────────┘
```

## Rules

Checked by [test_intel_imports.py](../../tests/test_intel_imports.py), walking the
imports with `ast`:

- Nothing in `intel/` imports `substation.ui` or QtWidgets; PySide6 only in `intel/qt/`.
  Everything else is plain Python, tested without a window.
- `substation._engine` only in `intel/qt/engine_facts.py`. Engine reads go through the
  `EngineFacts` protocol, which `BridgeFacts` implements on the bridge.
- Writes go only through operations and the `ProjectEditor`: never `Project` mutators,
  never the engine. So an agent's edit is undoable, saved and mirrored into the engine
  exactly like a mouse edit.
- Only the main thread touches the project; other threads come through the dispatcher.

## Files

| File | Contents |
|---|---|
| [facts.py](../../src/substation/intel/facts.py) | `EngineFacts` and `UiFacts` protocols; `TransportState`, `MeterStats`, `SampleView`, `AvailablePlugin`, `SelectionFacts` |
| [activity.py](../../src/substation/intel/activity.py) | `ActivityLog`: what actors other than the user did, in memory and in `activity.jsonl` |
| [ops/registry.py](../../src/substation/intel/ops/registry.py) | `@operation`, `Risk`, `Operation` (schema, `bind`), the constraints (`Doc`, `MinLen`, `MaxLen`, `Ge`, `Le`, `Pattern`), `REGISTRY`, `schemas()` |
| [ops/errors.py](../../src/substation/intel/ops/errors.py) | `OpError(code, message, **details)`; codes `not_found`, `invalid`, `conflict`, `denied`, `busy`, `frozen` |
| [ops/context.py](../../src/substation/intel/ops/context.py) | `OpContext` (project, editor, actor, facts, lookups, `check_unfrozen`, `check_path`); actors and `undo_label`; the shared argument types (`TrackId`, `Beat`, `NoteRow`, ...) |
| [ops/runner.py](../../src/substation/intel/ops/runner.py) | `OpRunner`: `run`, `call` (errors as results), `run_batch` |
| [ops/params.py](../../src/substation/intel/ops/params.py) | Any target as a `ParamSpec` (`spec_for`, `device_specs`), its own value and value at a beat, plain ↔ normalized |
| [ops/reads.py](../../src/substation/intel/ops/reads.py) | `get_project`, `list_tracks`, `get_track`, `get_meter_history`, `get_selection`, `describe_range` |
| [ops/tracks.py](../../src/substation/intel/ops/tracks.py), [devices.py](../../src/substation/intel/ops/devices.py), [clips.py](../../src/substation/intel/ops/clips.py), [midi.py](../../src/substation/intel/ops/midi.py), [automation.py](../../src/substation/intel/ops/automation.py), [presets.py](../../src/substation/intel/ops/presets.py), [project.py](../../src/substation/intel/ops/project.py), [freezing.py](../../src/substation/intel/ops/freezing.py), [batch.py](../../src/substation/intel/ops/batch.py) | The operations, by what they work on |
| [context/song.py](../../src/substation/intel/context/song.py) | `SongContext` and its parts, `build_context` (pure), `SongContextBuilder` (incremental), `describe_track` |
| [context/roles.py](../../src/substation/intel/context/roles.py) | `classify` → `Role(label, confidence, evidence)`; `clean_name`, `name_roles`, `midi_stats`, `midi_role` |
| [context/revision.py](../../src/substation/intel/context/revision.py) | `IntelRevision`: a counter bumped by every Project signal but view state's |
| [qt/controller.py](../../src/substation/intel/qt/controller.py) | `IntelController`: what the main window makes (`window.intel`) |
| [qt/dispatcher.py](../../src/substation/intel/qt/dispatcher.py) | `MainThreadDispatcher`: queued call + Future + timeout |
| [qt/engine_facts.py](../../src/substation/intel/qt/engine_facts.py) | `BridgeFacts` (EngineFacts on the bridge), `MeterHistory` |
| [ui/intel/facts.py](../../src/substation/ui/intel/facts.py) | `WindowFacts` (UiFacts on the main window): selection, device shown, scanned plug-ins, places |

## Operations

An operation is a function registered with its risk class and a summary:

```python
@operation(risk=Risk.COSMETIC, summary="Rename a track, group or return.")
def rename_track(ctx: OpContext, track_id: TrackId, name: TrackName) -> dict:
    ...
```

- **Schemas come from the type hints** (`str`, `int`, `float`, `bool`, `Literal`,
  `list[T]`, `X | None`, `dict`, each optionally `Annotated` with `Doc`, `MinLen`,
  `MaxLen`, `Ge`, `Le`, `Pattern`), strict (`additionalProperties: false`). The same
  types check and convert what a caller sends (`Operation.bind`). Every write's schema
  also takes `if_revision`.
- **Risk classes**: `read`, `cosmetic` (names, colours), `edit`, `destructive` (deleting,
  flattening), `transport` (not undo steps), `files` (a path outside the project).
  Permissions per class come with the MCP server.
- **Ids are the model's** (`"master"` for the master). Clips are found by id alone.
  Automation targets are the model's keys; `get_automation_targets` lists them.
- **Values**: parameters take plain units (dB, Hz, %, a list's index; a plug-in's own
  0..1) or `units: "normalized"`. Notes are `[pitch, start, length, velocity]`, starts
  in beats from the clip's start; points `[beat, value, curve]`.
- **Results** are JSON: ids of what was made or changed, plus `changed` (writes) and
  the project's `revision`.

| Area | Operations |
|---|---|
| Reads | `get_project`, `list_tracks`, `get_track`, `get_clip`, `get_device_params` (paged, `query`), `get_automation_targets`, `get_automation`, `get_transport`, `get_meter_history`, `get_selection`, `describe_range`, `list_available_devices`, `list_presets` |
| Tracks | `rename_track`, `set_track_color` (cosmetic); `add_audio_track`, `add_midi_track`, `add_return_track`, `group_tracks`, `ungroup_tracks`, `move_tracks`, `duplicate_tracks`, `set_track_mixer`, `set_send`; `delete_tracks` (destructive) |
| Devices | `add_device` (built-in id, plug-in uid, `rack`), `set_device_param`, `set_device_enabled`, `set_device_sidechain`; `remove_devices` (destructive) |
| Clips, MIDI | `add_clip_from_file` (files), `move_clips`, `set_clip_props`, `create_midi_clip`, `write_midi_notes` (`replace`, `add`, `replace_range`), `transform_notes` (`quantize`, `humanize`, `legato`, `transpose`, `velocity`); `delete_clips` (destructive) |
| Automation | `write_automation` (whole, or a stretch), `clear_automation`, `automate_shape` (`ramp`, `swell`, `dip`, `step`, `lfo`), `re_enable_automation` |
| Presets | `load_device_preset` (`.gilpreset` from the library, `.vstpreset` from a place), `insert_preset` |
| Freezing | `freeze_tracks`, `unfreeze_tracks`; `flatten_tracks` (destructive) |
| Project, transport | `set_tempo`, `set_key`, `set_time_signature`, `set_loop`; `play`, `stop`, `locate` |
| Batch | `batch`: calls as one step, `"$<i>.<path>"` taking a value from an earlier result |

Details worth knowing:

- **Frozen tracks.** Operations that would change what a frozen track's audio holds
  (clips, devices, parameters, device automation; mixer automation of a track in a
  frozen group) check first and fail with `frozen`. Names, colours, a frozen track's
  own mixer and sends still work. The editor's own refusals (its `refused` signal)
  during a run become `frozen` too, and win over whatever the operation made of them.
- **Setting an automated parameter** (a device's, or a track's volume or pan) overrides
  its automation, as turning its knob does, even when the value is the one it had; the
  result says `overrides_automation`. `re_enable_automation` undoes that.
- **`automate_shape` and `write_automation` with a range** paste over that stretch of
  the envelope (`automation.paste_range`). On a target with no automation yet, the
  envelope is first seeded with the value now at both edges, so it doesn't keep the
  shape's last value after the range. `restore: false` leaves the shape's end value in
  place after it (on a target with no automation yet).
- **Plug-in parameters** come from the engine (`EngineFacts.param_specs`), with the
  plug-in's own text for values; built-in devices' are described without the engine.
- **Paths** (`add_clip_from_file`, `.vstpreset`s) must be under a browser place or the
  project's folder (`.gilpreset`s: or the preset library), else `denied`. A `.vstpreset`
  is checked to be the device's plug-in's (its header's class id, in COM byte order).

## The runner

`OpRunner.run(name, args, actor)`:

1. Finds the operation (`not_found`), checks the arguments (`invalid`).
2. For a write: `if_revision` must be the current revision (`conflict`, with the
   revision now), and nothing may be recording (`busy`).
3. Runs it inside one undo macro labelled for its actor: `Rename Track` for the user,
   `Auto: …`, `Assistant: …`, `Agent (<client>): …`. A batch is one macro too, labelled
   with its `label`.
4. On failure (an `OpError`, a `ValueError` or `KeyError` from the editor, or a
   refusal) it ends the macro, undoes it, and takes it off the stack (`setObsolete` and
   `redo()`, which deletes an obsolete command instead of redoing it): no trace in the
   model or the undo stack. An edit that changed nothing (no Project signal) leaves no
   empty step either.
5. Adds `changed` and `revision`, and logs it: writes by actors other than the user,
   failures too, with the undo index after the step.

`call()` returns errors as `{"error": {"code", "message", "details"}}` instead of
raising, for transports.

Actors: `user`, `auto`, `assistant`, `agent:<client>` (`agent("Claude Code")`).

## The revision and the activity log

`IntelRevision` counts every Project signal except view state's
(`automation_view_changed`, `devices_folded`). A caller that read revision 41 passes
`if_revision: 41`; if the user (or anyone) changed something since, it gets
`conflict` instead of clobbering the edit.

`ActivityLog` keeps the last 500 entries `{time, actor, op, args, result, undo_index}`
and appends them to `%LOCALAPPDATA%\SUBstation\activity.jsonl` (or
`SUBSTATION_ACTIVITY_LOG`), rotated to `activity.1.jsonl` past 2 MB. Reads aren't
logged.

## The selection

`get_selection` describes what `UiFacts.selection()` reports: a time range (beats and
bar.beat.sixteenth) and the tracks it spans, the clips, automation lanes and
breakpoints, the selected tracks, the device shown in the device view, the insert
marker, and `applies_to` (the first of time range, clips, breakpoints, tracks, device,
project). Its `selection_id` is a hash of that; the last 32 are kept, so
`get_selection(selection_id=…)` reads one as it was. Operations never take "the current
selection": a caller reads it once and passes its numbers on, so a batch means the same
when the user clicks on meanwhile, and when replayed.

`describe_range(start, end, track_ids, context_bars)` reports around a range: each
track's clips in it, clips ending near its start or starting near its end, whether it
plays before, in and after, the automation there, and the effects on the tracks,
returns and master (reverb, delay, distortion, lofi, modulation, filter, eq, dynamics,
ducking: by built-in id, VST3 category or name).

## SongContext

`build_context(project, analysis_lookup, describe_params, revision, plugin_categories)`
describes the song as frozen dataclasses: `ProjectInfo` (tempo, time signature, key,
loop, length, revision) and a `TrackContext` per track, return and the master (kind,
group, mixer, frozen, `Role`, clips, MIDI statistics, devices with racks' chains, sends,
input, automated targets). `to_dict("summary")` is a few KB for prompts;
`to_dict("full")` adds evidence, every clip and built-in devices' parameters.
`SongContextBuilder` keeps one up to date: Project signals mark tracks dirty (a tempo
change, a reset or rearranging marks all), and `context()` rebuilds only those, or
returns the same object while the revision hasn't moved. Names in it are data, never
instructions.

### Roles

`classify(track, plugin_categories)` scores the evidence and keeps it:

| Evidence | Weight |
|---|---|
| File names (audio clips, Samplers' samples), cleaned by `clean_name` and mapped through `TOKENS`; mixed drum one-shots → `drums`; frozen audio is no evidence | 0.6 |
| Devices: a plug-in's name, a drum or piano VST3 category, a rack whose chains are drums | 0.35 (rack 0.6) |
| MIDI (`midi_stats` of the notes that play, `midi_role`): drum keys, a low line (bass, sub), chords (pad if held), a high line (lead), several notes (keys) | 0.35 × its confidence |
| The track's name, unless a default one ("1 Audio") | 0.2 |

The label is the highest score; its confidence is that score (at most 0.99) times its
share of all scores. `clean_name` drops the extension, tempo, key, numbering and an
all-capitals pack prefix: `Bass_Loop_128_Am.wav` → "Bass Loop", `KSHMR_Kick_07` → "Kick".

## EngineFacts on the bridge

`BridgeFacts` answers from the bridge: parameter specs (none for a device that isn't
loaded), own values, automation state (and overriding it), decoded sources, file
lengths, the transport, `render_track` (`Engine.render_track_offline`; refused while
recording), freezing renders, plug-in states. `program_name` is None until engine
request E5. Its `MeterHistory` records the bridge's meter peaks at every
`meters_updated` (after `_poll_meters`), a minute per strip, for `meter_history`.

## Tests

- [test_intel_ops.py](../../tests/test_intel_ops.py): schemas and argument checks, the
  schemas' snapshot ([tests/snapshots/intel_ops.json](../../tests/snapshots/intel_ops.json);
  `UPDATE_SNAPSHOTS=1` rewrites it), undo labels, a failing batch leaving no trace,
  stale revisions, frozen tracks, the activity log; every write operation in the window
  undoing back to the model and the engine's render as they were; a MIDI clip made with
  notes playing them; automation on a test plug-in's parameter in time, and setting it
  overriding it; a swell on an unautomated parameter; a batch made from the selection
  while the selection changes; calls from another thread, and a main thread that
  doesn't answer; the meter history.
- [test_intel_context.py](../../tests/test_intel_context.py): name cleaning, roles from
  file names, devices and MIDI, MIDI statistics of the notes that play, the context's
  details, and the builder rebuilding only what changed.
- [test_intel_imports.py](../../tests/test_intel_imports.py): the import rules.
