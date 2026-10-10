# Project model and undo

The project model is the single source of truth for the UI, undo and saving. It lives in
[app/src/model](../../app/src/model): value types for tracks, clips, notes, devices and envelopes, a `Project`
QObject that holds them and signals every change, pure functions that do the editing maths, and the undo
commands. The `ProjectEditor` ([app/src/editor](../../app/src/editor)) turns each user action into those
commands. The audio engine only mirrors this model (see [engine-bridge.md](engine-bridge.md)); saving and loading
are in [serialization.md](serialization.md); what acts on the selection (the clipboard, the Edit menu) is the
session's ([session.md](session.md)).

## Overview

```
 QML control / scene-graph item ──calls──► ProjectEditor (editor/)
                                              │  works out the new state with the pure functions:
                                              │  Edits.h (clips), Notes.h (notes), Automation.h (envelopes)
                                              ▼
                                         QUndoCommand (Commands.h) ──pushed onto──► QUndoStack (the session's)
                                              │ redo() / undo()
                                              ▼
                                         Project mutators (Project.h) ──emit──► Qt signals
                                                                                   │
                              ┌────────────────────────────┬───────────────────────┼──────────────────────┐
                              ▼                            ▼                       ▼                      ▼
                       the UI repaints        EngineBridge pushes the      the session prunes      the title's "*"
                                              change into the engine       the selection           (undo stack not clean)
```

- **Undoable state** changes only through commands. `Project`'s mutating methods are meant to be called by a
  command's `redo()`/`undo()` and nothing else, so every edit is undoable and every change is signalled.
- **View state** (track heights, folding, which automation shows, arming, solo, Lock Envelopes, folded devices) is
  saved with the project but changes directly, without an undo step, as in Ableton.
- Everything runs on the Qt GUI thread. Nothing in `model/` calls the engine; `Devices.h` reads the engine's list
  of built-in devices (`sub::BuiltinRegistry`), and `ParamSpec` is made from the engine's `ParamInfo`.
- It is `sub_app`'s: Qt Core and Gui (`QUndoCommand` and `QUndoStack` are in Qt Gui), never QML. QML reads the
  `Project`'s properties and calls the editor's `Q_INVOKABLE`s through the `Session` singleton.

## Files

| File | What it holds |
|---|---|
| [Project.h](../../app/src/model/Project.h) | `Project` (QObject: queries, mutators, signals), `ProjectContents` (everything replaced at once), `ChainField`/`ChainValue` and `SettingsField`/`SettingsValue` (what `updateChain` and `updateSettings` change), `DeviceParam`, `LaneRef` |
| [Track.h](../../app/src/model/Track.h) | `Track`, `Freeze`, `MidiInput`, `Send`, `SendMap`, `EnvelopeMap`; `TrackField`/`TrackValue` (what `updateTrack` changes) and `trackFieldName`; the kinds (`kAudioKind`, `kMidiKind`, `kGroupKind`, `kReturnKind`, `kMasterKind`), `kMonitorModes`, `kTrackColors`, track heights; `newMaster`, `returnLetter` |
| [TrackNames.h](../../app/src/model/TrackNames.h) | Track names: `#` for its number (`numberedName`), audio and MIDI tracks named by what they hold (`contentsLabel`, `contentsName`, `namedByContents`), names older projects had (`hasPlainName`, `plainNameTemplate`), `takeName` |
| [Clip.h](../../app/src/model/Clip.h) | `Clip` (audio and MIDI alike), `Note`, `BendPoint`, `Vibrato`, `PlayedNote`; `kWarpModes`, `legacyWarpMode`, `kMinSegmentBpm`, `kMaxSegmentBpm` |
| [Device.h](../../app/src/model/Device.h) | `Device`, `Chain`, `PluginRef`, `Sidechain`, `MacroMapping`; the device-tree helpers (`iterDevices`, `iterChains`, `devicePath`, `deviceAt`, `findDevice`, `findChain`, `chainIndex`, `chainDevices`, `containerOf`, `rackDepth`, `rackHeight`, `refreshIds`); `kPluginKind`, `kRackKind`, `kMaxRackDepth`, `kDefaultMacroCount`, `kMaxMacroCount`, `macroParam`, `macroIndex`, `macroCount`, `macroName`, the taps `kPostFader`, `kPreFader`, `kPreFx` |
| [Devices.h](../../app/src/model/Devices.h) | Kinds of devices: `builtinDevices()`, `builtinDevice()`, `builtinCategories()` (from the engine), `kDefaultInstrument`; `isInstrument`, `deviceIsInstrument`, `loadsInto`, `deviceName`, `kindName`; `newDevice`, `newRack`, `newChain`; `builtinParamInfo`, `deviceIdsOf`, `deviceIdsOfList`, `addDeviceIds` (into a set of many lists' ids), `innerDeviceIds` (what is in a rack) |
| [Routing.h](../../app/src/model/Routing.h) | The group tree (`TreeEntry`, `TrackTree`, `treeProblem`, `repairTree`) and the routing graph (`routingGraph`, `feeds`, `wouldCycle`, `inputWouldCycle`, `sidechainWouldCycle`); `isDefaultOutput` (an output that goes where the default does: its own group, or the master from outside a group) |
| [Automation.h](../../app/src/model/Automation.h) | `AutomationPoint`, `Envelope`, `AutomationView`, `kMaster`; in `sub::app::automation`: target keys, the mixer's normalized mappings, evaluation (`valueAt`, `leftValue`, `shape`), every envelope edit |
| [ParamSpec.h](../../app/src/model/ParamSpec.h) | `ParamSpec`: any automatable parameter described alike, with the engine's normalized mapping; `mixerSpecs`, `sendSpec`, `chainSpecs`, `formatValue` |
| [Edits.h](../../app/src/model/Edits.h) | `sub::app::edits`: pure clip maths for audio and MIDI clips alike: overlaps, cuts, trims, splits, ranges, tempo fitting, consolidating, reversing, stretching, slipping, fades; `sortByStart`, how clip lists are kept |
| [Notes.h](../../app/src/model/Notes.h) | `sub::app::notes`: pure note maths for the piano roll: overlaps on a key, moves, resizes, velocity, legato, ×2/÷2, quantize, humanize timing, bends and vibratos; note names |
| [Commands.h](../../app/src/model/Commands.h) | The `QUndoCommand` subclasses and what they share: `ValueCommand` (a target, its old and new value), `MergeableCommand` (one that merges continuous gestures: `kMergeId`, `mergeId`), `InsertCommand`/`RemoveCommand` (a track into or out of the tracks or the returns) |
| [Timebase.h](../../app/src/model/Timebase.h) | `TimeSignature`, beats and seconds, bar.beat.sixteenth formatting and parsing, dB and pan text |
| [Keys.h](../../app/src/model/Keys.h) | Musical keys (`Key`), tempo and key from file names (`parseFilename`), what a dropped clip starts with (`clipSettings`) |
| [DeviceState.h](../../app/src/model/DeviceState.h) | `sub::app::deviceState`: a built-in device's state besides its parameters (a sampler's sample), in the engine's text format, base64 in `Device::state` |
| [RecordedTake.h](../../app/src/model/RecordedTake.h) | `RecordedTake`, `RecordedTakeNote`: what a recording hands the editor |
| [Errors.h](../../app/src/model/Errors.h) | `EditError` (an edit the user can't make), `ProjectFileError` (a file that can't be read or written); both are `UserError`s, carrying a `QString` message for the user (one catch takes either) |
| [Paths.h](../../app/src/model/Paths.h) | Paths as the application compares them: `absoluteCleanPath`, `pathIdentity` (absolute and clean, case folded where the system ignores case: `sub::platform::kCaseSensitivePaths`), `samePath`. Decoded sources, the File Manager's files and the recent projects are keyed by it. `withSafeCharacters`: the characters Windows forbids in file names made `_` (takes' and presets' file names) |
| [Ids.h](../../app/src/model/Ids.h), [OrderedMap.h](../../app/src/model/OrderedMap.h), [Numbers.h](../../app/src/model/Numbers.h) | `newId()`, `optionalId()` (`""`: none); a map that keeps its keys in insertion order; rounding half to even, floor division that stays exact (`floorDiv`), Python's whole-number `%` (`floorMod`), fixed-point text |
| [editor/](../../app/src/editor) | `ProjectEditor` ([ProjectEditor.h](../../app/src/editor/ProjectEditor.h)), one source file per area: `EditorTracks.cpp` (tracks, returns and sends, groups, inputs, recordings), `EditorSettings.cpp` (tempo, time signature, key, loop), `EditorClips.cpp` (clips and time selections), `EditorDeviceChains.cpp` (devices in chains), `EditorRacks.cpp` (racks, their chains and macros), `EditorDeviceSettings.cpp` (a device's parameters, state, presets, switch, sidechain), `EditorAutomation.cpp` (envelopes and the lanes shown), `EditorFreezing.cpp` (freezing, unfreezing, flattening, and what frozen tracks refuse), `EditorFiles.cpp` (the files clips and devices play: one put in another's place, files found somewhere else). Its value types: `ClipRef`/`ClipRefs`, `TimeRange`, `MovedRange`, `ClipboardContent`/`CopiedTrack`/`CopiedFreeze`, `CopiedTracks`, `CopiedAutomation`, `TrackParent`/`InsertionPoint` |
| [io/Serialization.h](../../app/src/io/Serialization.h), [io/Presets.h](../../app/src/io/Presets.h) | Project and preset files, the preset library: see [serialization.md](serialization.md) |

## Key types

All of them are values: copying a `Track` copies its clips, devices and automation. Containers are `std::vector`
(the device tree is recursive), keyed maps `QMap` (sorted) unless an order shows somewhere, then `OrderedMap`
(insertion order). Ids, names and paths are `QString`; an optional field is a `std::optional`.

### Time

Timeline positions are quarter-note **beats** ([Timebase.h](../../app/src/model/Timebase.h)). `TimeSignature` gives
`beatsPerBar()` and `beatLength()` in quarter notes; `formatPosition`/`parsePosition` use Ableton's one-based
`bars.beats.sixteenths`. `dbToGain` treats -70 dB and below as silence (the faders' floor,
`automation::kMinVolumeDb`). Pan is -1..1 in the model and shows as `25L`/`C`/`30R` (±50: `formatPan`,
`parsePan`). Texts shown to the user round half to even (`formatFixed`), as they always have.

### Clips and notes

`Clip` ([Clip.h](../../app/src/model/Clip.h)) is one struct for both kinds (`kind`: `Clip::Kind::Audio` or `Midi`;
`isAudio()`, `isMidi()`); an edit makes a new clip (copy it, set what changes). Each is a *window* onto its content,
and editing moves the window's edges while the content stays put on the timeline. Its methods behave per kind, so
both kinds are edited alike.

- **Audio**: `path`, `startBeat`, and in **source seconds** `offsetSec`, `durationSec` and `sourceDurationSec`;
  clip-view settings `gainDb`, `warp`, `warpMode`, `segmentBpm`, `transpose` (semitones), `detune` (cents), `pan`.
  Unwarped audio plays at its own speed, so the clip's length in beats follows the tempo; warped (`isWarped()`:
  `warp` and `segmentBpm > 0`), the audio is taken to be at `segmentBpm`, so its length in beats is fixed.
  `sourceTempo()`, `beatsToSource()` and `sourceToBeats()` convert between the two. `segmentBpm` 0 means "not set"
  (shown as the project tempo). A reversed clip plays a reversed copy of a file (its `path`, written by the
  bridge); `reversedFrom` is the file it was made from (reversing again goes back to it), empty for a clip that
  isn't reversed. Its fades, `fadeInSec` and `fadeOutSec`, are measured in its audio too (so they stretch with it;
  `fadeInBeats()` / `fadeOutBeats()` on the timeline), each bent by `fadeInCurve` / `fadeOutCurve` (-1..1, 0 a
  straight line, positive bulging up: `fadeGain()`, which is `automation::shape()`, as the engine's
  `automationShape()`). `fitFades()` holds them to the clip: where together they are longer, both shorten in
  proportion so they meet (a MIDI clip has none). Without a fade of its own, the engine fades an edge that cuts
  into the file for a few milliseconds against clicks, but not one at the file's own start or end, so a one-shot's
  attack plays as it is ([engine/rendering.md](../engine/rendering.md)).
- **MIDI**: `startBeat`, `durationBeats`, `offsetBeats` (the content beat at the clip's start) and `notes`, sorted by
  start then pitch. Its length is in beats, so it doesn't change with the tempo. Notes outside the window are kept
  but not played, so trimming or splitting never deletes them. `playedNotes()` gives the notes that start inside the
  window, cut at its end, in timeline beats (`PlayedNote`), as in Ableton.
- Both: `id`, `startBeat`, `name` (an audio clip's: its file's name as it was placed; a MIDI clip has none: one
  in an earlier file is dropped on load), and `muted`: deactivated (Ableton's clip activator off, 0), the clip is
  kept, edited, copied and saved as any other but isn't heard (`plays()`; `heardNotes()`: none of its notes, and
  never a deactivated note), so the
  bridge leaves it out and the song's chords don't count it; what is split, trimmed or copied from it is deactivated
  too.
- `Note`: `pitch` (60 = C3, Ableton's octave numbering), `start` and `length` in beats from the clip's content start,
  `velocity` 1..127, and `muted`: deactivated (0 in the piano roll), kept and edited as any other but not heard.
  `notes::withActive()` deactivates (or activates) some of a clip's notes where they are.
- A note's **bend** (MIDI 2.0's per-note pitch bend): `bend`, a list of `BendPoint`s (`time` in beats from the
  note's start, `semitones` from its pitch, `curve` -1..1 bending the segment from it to the next as an automation
  breakpoint's does), sorted by time, and `vibrato`, `Vibrato`s on top of it (`start` and `length` in beats from the
  note's start, `depth` semitones either way, `rate` cycles a second, `fade` the share of its length it swells in
  over), in time order and not overlapping. The curve starts at the note's pitch at its start, goes through the
  points, then holds the last one's value; a point at (or before) the start sets where it starts. A vibrato swells
  in and dies away over its last tenth, so it begins and ends on the curve: drawn over a bend, it follows it.
  `bent()` says whether it has either; `bendAt(beat, tempo)` is what it sounds like there (the vibratos' rates are in
  seconds, so it takes the tempo) and `curveAt(beat)` the points' curve alone. Both are the engine's own rules,
  `sub::bend` in [engine/src/NoteBend.h](../../engine/src/NoteBend.h) (header-only, no engine types), so what the
  piano roll draws is what plays. The bend's times are the note's own, so it moves, copies and transposes with the
  note; points or vibratos past its end are kept, not heard. Bends are held to ±48 semitones
  (`notes::kMaxBendSemitones`, MIDI 2.0's per-note range).
- `kWarpModes` is in the engine's `WarpMode` order: Transients, Standard, Smooth, Formants, Re-Pitch.
  `legacyWarpMode()` maps the earlier Ableton-style names (Beats, Tones, Complex, Texture, Complex Pro) to the mode
  that plays the same way; loading applies it.

### Tracks

`Track` ([Track.h](../../app/src/model/Track.h)). Its `kind` is fixed for its life (flattening replaces the track):

| kind | Where it lives | Clips | Notes |
|---|---|---|---|
| `kAudioKind` `"audio"`, `kMidiKind` `"midi"` | `Project::tracks()` | yes | `hasClips()` |
| `kGroupKind` `"group"` | `Project::tracks()` | no | holds other tracks |
| `kReturnKind` `"return"` | `Project::returns()` | no | fed by sends; named by letter (`returnLetter`: A..Z, AA...) |
| `kMasterKind` `"master"` | `Project::master()` | no | id `kMaster` (`"master"`), effects only |

Fields: `name` (as shown: its `nameTemplate` with each `#` its number, its place in `Project::tracks()` from 1;
`Project` keeps it so, see [TrackNames.h](../../app/src/model/TrackNames.h)), `nameTemplate` (what renaming changes
and the project file saves: `"# Kick"`; `TrackField::Name`; empty: `name` as it is; `nameSource()`), `color`, mixer (`volumeDb`, `pan`, `mute`, `solo`), `height` (its lane's; a new track's is
`kDefaultTrackHeight`, 96 px, a new group's `kDefaultGroupHeight`, 104), `clips` (sorted by `startBeat`),
`devices`, `automation` (target key → envelope, never empty; an `EnvelopeMap`, which keeps the order targets were
first automated in), `automationView`, audio input (`input`: none, `{c}` or `{l, r}` device channels, or
`inputTrack`: another track's id or `kMaster` for resampling; `inputTap`: where that is taken, `kPostFader`,
`kPreFader` or `kPreFx`, as a sidechain's tap), `output` (an `Output`: where its output goes; see below), `midiInput` (a `MidiInput`, or none for *No Input*;
new MIDI tracks hear every input), `monitor` (`kMonitorModes`: `off`, `in`, `auto`), `armed`, `parent` (the group it
is in), `folded`, `sends` (return id → `Send`), `frozen`.

The master and the returns are `Track`s too, so whatever works on a track's devices, mixer or automation works on
theirs: `Project::track(id)` finds an arrangement track, a return, or the master (`kMaster`). `hasTrack()` is true
only for arrangement tracks; `hasOwner()` for anything with devices, a mixer and automation. `allTracks()` is the
tracks, the returns, then the master; `owners()` the same ids; `senders()` the tracks then the returns.

### Devices, racks, chains

`Device` ([Device.h](../../app/src/model/Device.h)): `id`, `kind`, `enabled`, `params` (param id → plain value),
`plugin`, `state`, `sidechain`, `midiFrom`, and for a rack `chains`, `macros` and `name`.

`midiFrom` is a MIDI track whose notes the device plays instead of its own track's (`""`: its own track's), for a
device that plays notes: an instrument, or an effect with a MIDI input (a vocoder, a pitch corrector: whether it has
one is its processor's, `EngineBridge::acceptsMidi`). It is no edge of the routing graph: notes carry no audio, so
no MIDI input closes a cycle, and a device on any track (a return, the master) may take any MIDI track's but its own.
`Project::midiSources(track)` lists those, in order.

- **Built-in** (`kind` `"synth"`, `"utility"`, ...): its state is its `params`, and `state` for what isn't a
  parameter (see `DeviceState.h`). The engine follows the model. The list of built-in devices, their names,
  categories, default parameters and which are instruments come from the engine (`builtinDevices()` reads
  `sub::BuiltinRegistry`), so a new device needs nothing here. `kDefaultInstrument` is `"synth"`.
- **Plug-in** (`kind` `kPluginKind`, `"plugin"`): `plugin` is a `PluginRef` (`format` `"VST3"`, `uid` the class id,
  `name`, `vendor`, `path` where it was last loaded, `instrument`). The plug-in keeps its own state; `state` holds it
  (base64 `.vstpreset`) as last stored, for saving and copying. Its `params` only record values changed from the
  host, for undo.
- **Rack** (`kind` `kRackKind`, `"rack"`): `chains` is a list of `Chain` (`id`, `name`, `devices`, and a mixer:
  `volumeDb`, `pan`, `mute`, `solo`; while any chain of a rack is soloed, only those are heard). It has 1 to
  `kMaxMacroCount` (16) macros, a new one `kDefaultMacroCount` (4): `macroNames` holds one name per macro (`""`: named
  by its number, "Macro N"; `macroName()`), so its size is how many it has (`macroCount()`). Its `params` are its
  macros' values, `"macro1"`.. (`macroParam(i)`; `macroIndex()` the other way), normalized 0..1, one per macro.
  `macros` is a list of `MacroMapping{macro, deviceId, paramId, low, high}`: the macro's 0..1 maps to the parameter's
  normalized `low`..`high` (`low > high` turns it the other way, 0.5..1 is its upper half; `target()`). Racks nest at most `kMaxRackDepth` = 8 deep (the
  engine's limit). A rack with an instrument in it is an *Instrument Rack* (`deviceIsInstrument`), otherwise an
  *Audio Effect Rack*. `name` is the preset it was saved as or loaded from (none: named by its kind; `deviceName`).

Device ids are unique in the whole project, so a device is found wherever it sits: `Project::device(track, id)` and
`findDevice()` search racks too, `Project::deviceOwner(id)` finds its track. The tree helpers:

- `iterDevices(devices)`: every device depth first (each, then what is in its chains).
- `iterChains(devices)`: every (rack, chain) depth first.
- `devicePath` / `deviceAt`: a device's place as (index, chain, index, chain, ...).
- `findChain(devices, chain)`: a rack chain anywhere in a chain, and its rack (`Project::chain` and `chainRack` are
  it on a track's devices); `chainIndex(rack, chain)`: where one of a rack's own chains is.
- `chainDevices(devices, chain)`: the vector of a chain (none = the track's own), which edits change in place on a
  copy of the track's devices.
- `containerOf`: the chain a device is in (none = the track's own).
- `rackDepth(devices, chain)`: how many racks a chain is in; `rackHeight(device)`: how deep racks nest inside a
  device. `rackDepth + rackHeight ≤ kMaxRackDepth` is checked before any insert or move.
- `refreshIds(device)`: new ids for a device, everything in it and its chains, in place; macro mappings follow their
  devices and mappings to devices not in it go. Used for presets, pasted devices, duplicated chains and duplicated
  tracks.

### Freezing

A track, group or return can be frozen: `Track::frozen` is a `Freeze` (its frozen audio's `path`, `durationSec`, and
the `tempo` it was rendered at; `Freeze::clip()` is the warped clip that plays all of it from beat 0).
`Project::setFrozen()` emits `freezeChanged`; `updateTrack()` can't set it (there is no `TrackField` for it).

What of the render plays are its **segments**: `Freeze::segments`, audio clips into the render file (warped from its
`tempo`, as `clip()` is; `Freeze::segment(id, start, offset, length)` makes one). None (as frozen, and as projects saved
before segments load) means all of it, from beat 0; `Freeze::playing(trackId)` is what plays either way (the bridge
plays it, flattening turns it into clips). Edits of a time selection over a frozen track change them as they change
its clips (below), so the arrangement and what plays stay in step. `Project::setFrozenSegments()` sets them (sorted)
and emits `clipsChanged`: what the track plays changed, not whether it is frozen. Unfreezing drops the freeze, segments
and all, and leaves the clips as edited.

- `Project::frozenBy(track)`: the frozen track holding a track's audio (itself, or the outermost frozen group it is
  in); `isFrozen(track)`: it has one.
- `freezeProblem(track)`: why it can't be frozen (the master, frozen already, in a frozen group, or a sidechain taps
  its signal Pre FX or after one of its devices). `flattenProblem(track)`: only frozen audio and MIDI tracks flatten.
- The editor's `freezeTracks(OrderedMap<id, Freeze>)` (one step; disarms them; a track in a group frozen with it is
  left out), `unfreezeTracks()` and `flattenTracks()` (a `ReplaceTrackCommand` per track: an audio track of the same
  id playing `Freeze::playing()` as clips named after it, no devices, no device automation). The render itself is
  the bridge's
  (`renderFreeze()`, or `startFreeze()` and `finishFreeze()` in the background); the session does both, its progress
  shown ([session.md](session.md#renders-in-the-background)).
- What frozen audio holds can't change. `ProjectEditor::push()` refuses (and says why on `refused`; the checks are in
  [EditorFreezing.cpp](../../app/src/editor/EditorFreezing.cpp)) commands changing a frozen track's (or a track in a
  frozen group's) clips, devices or device automation, and the mixer automation of a track in a frozen group
  (`laneFrozen()`); a frozen track's own mixer and every send stay live, and a sidechain may still go when its source
  does. Rearranging tracks, deleting, grouping and ungrouping refuse moving tracks into or out of a frozen group (a
  frozen group being ungrouped holds nothing any more); new tracks and duplicates meant for a frozen group go after it;
  `armTracks()` leaves frozen tracks unarmed.
- **Except time selections, which take the frozen audio along.** `deleteRange`, `moveRange` (and Ctrl-drag copies),
  `duplicateRange`, `cutRange` and `paste` over a frozen track (or what is in a frozen group: the group holds the
  render) do to the segments of each frozen render they touch (`frozenRenders()`: frozen tracks among them and frozen
  groups they are in) what they do to the clips, and move the automation baked into it (its devices', and the mixer's
  of what is in a frozen group) with it. One `SetClipsCommand` carries the clips and the segments (`frozenBefore`,
  `frozenAfter`), so it is one undo step restoring both; `frozenProblem()` lets a clip change on a frozen track through
  only if it carries all the frozen renders holding it (`carriesFrozen()`), and `commitMoved()` lets automation baked
  into frozen audio change only with that audio. The rules, kept simple so clips and audio can't drift apart:
  - On a frozen track a stretch that is moved, copied, duplicated or pasted **replaces everything where it lands**,
    empty parts too, as its audio does (on other tracks only the clips it brings replace what they land on).
  - A stretch of only some of a frozen group is refused (`frozenAreaProblem()`: *select the whole group*): its audio is
    one render of all of it. A selection over the group's row takes in all of it.
  - Clips don't move between a frozen track (or one in a frozen group) and another track: a move with a track delta
    is refused; in time only, it moves.
  - `copyRange` copies the frozen audio there of each frozen render the selection takes in whole
    (`ClipboardContent::frozen`, `CopiedFreeze{trackId, path, segments}`; a frozen track with audio there and no
    clips is a row of its own, to paste back onto). Pasted onto tracks that aren't frozen, the clips go as they are.
    Into a frozen track (or a frozen group's tracks) only content carrying its frozen audio, the same render (`path`),
    pastes, onto the tracks it came from; anything else is refused (*only what was copied from it can be pasted into
    it*).
  - Everything else that changes a frozen track's clips (trimming, splitting, consolidating, reversing, clip settings,
    `moveClips`, adding clips) stays refused, as do its devices.

### Groups

Groups hold other tracks. `Project::tracks()` stays **flat**; the hierarchy is each track's `parent`. The invariant
(`treeProblem`, [Routing.h](../../app/src/model/Routing.h)) is that a track's parent is a group listed before it, and
everything between a group and its last descendant is a descendant of it: a group's tracks follow it, together, and
no group is in itself. `repairTree` takes tracks out of groups they can't be in (an edited file). Queries on
`Project`: `subtreeEnd`, `descendants`, `withContents(ids)` (those tracks and what is in the groups among them, in
order), `children`, `ancestors`, `isDescendant`, `depth`, `isHidden` (a group it is in is folded), `parentAt(index)`
(a track inserted there goes into the group of the track it goes before), `tree()` (every track's id and parent: a
`TrackTree`). The engine sees only where each track's output goes.

### Outputs, returns, sends, inputs, sidechains: the routing graph

`Output{to, id}`: where a track's (group's, return's) output goes, Ableton's *Audio To*. `Group` (the default):
into its group, or the master outside one; `Master`: the master, past its group; `Track` (`id`: an audio
track's): into that track's input (*Track In*: the bridge has the engine take what goes into an audio track as its
input, heard while it monitors); `Sidechain` (`id`: a device's, on a track, a return or the master): into that
device's sidechain input, wherever the device is (it moves with it); `None`: nowhere (*Sends Only*).
`outputTarget(track)` is the track it goes into (its group, a track, a device's track), none for the master or
nowhere.

`Send{levelDb, preFader}`: a track's (group's, return's) send to a return, after its fader and pan, or before it.
Silent at `kMinVolumeDb` (the default for a new send). A muted track sends nothing either way. `Track::sends` is
replaced whole, never changed in place.

`Sidechain{trackId, tap}`: what a device's aux input hears. `tap` is `kPostFader` (`"post"`, Ableton's *Post
Mixer*), `kPreFader` (`"pre"`, *Post FX*), `kPreFx` (`"pre-fx"`: before all the source's devices; on a MIDI track,
after its instrument), or a device id (after that device; before the fader while that device isn't on the source).
`tapDevice()` is that id, or none.

`routingGraph(tracks, returns)` maps each track and return to what its signal goes into: where its output goes
(its group by default), the returns it sends to, the tracks taking their input from it, and the tracks whose
devices take it as their sidechain. The master isn't in it (everything reaches it). `feeds(graph, a, b)` is
reachability. The cycle checks:

- `outputWouldCycle(track, output)`: its output would go into itself (a device on itself) or a track it feeds.
- `wouldCycle(track, return)`: a send would close a cycle (the return is the track or feeds it).
- `inputWouldCycle(track, source)`: never for `kMaster` (resampling; a track recording it never plays it back into
  it, since it can't monitor it).
- `sidechainWouldCycle(track, source)`: always for `kMaster` as source; never for a device on the master.

`Project` wraps them and adds the menus' lists: `sendTargets`, `inputSources`, `sidechainSources`, `inputName`
(`"Resampling"` for the master), `outputTarget`, `outputWouldCycle`.

The editor keeps outputs valid ([EditorTracks.cpp](../../app/src/editor/EditorTracks.cpp)): `setTrackOutput`
refuses one into a track that isn't an audio track, a device that isn't there, or a cycle, and stores *Main* outside
a group, or its own group, as the default (`isDefaultOutput`; loading does the same); a track going into another group goes into it, as in Ableton (`arrange`:
the explicit outputs come back one by one, those that would close a cycle going into their groups); deleting the
track or device an output goes into (`dropOutputs`, `setDevices`), flattening a track, or moving a device where an
output into it would close a cycle sends those outputs into their groups, in the same undo step; copies of tracks
go into the copies of what they went into (`insertCopies`).

### Automation

See [Automation.h](../../app/src/model/Automation.h) and, for the engine side,
[engine/automation.md](../engine/automation.md).

- Every target is automated in **normalized** values (0..1). An envelope is a vector of
  `AutomationPoint{beat, value, curve}`; it belongs to an owner (a track id or `kMaster`) and is keyed by target:
  - `mixer:volume`, `mixer:pan`, `mixer:on` (`automation::kMixerVolume`, `kMixerPan`, `kMixerOn`: a track's
    activator; the master has none)
  - `send:<return id>` (`sendKey`): the level of the owner's send to a return
  - `device:<device id>:<param id>` (`deviceKey`)
  - `device:<device id>:device:on` (`deviceOnKey`, `kDeviceOn`): a device's (or a rack's) on/off
  - `device:<rack id>:chain:<chain id>:volume` / `:pan` (`chainKey`): a rack chain's fader, a target of its rack
- `parseKey` splits a key (a `Target`); `keyDevice`, `keySend`, `keyChain`, `keyChainControl`, `isMixerKey`,
  `isSwitchKey` (an activator or an on/off), `isKey` classify it.
- Switches are 0 (off) or 1 (on): `switchSpec` describes them (*Track Activator* in `mixerSpecs`, *Device On*:
  `deviceOnSpec`, every device's first parameter).
- Before the first breakpoint an envelope holds the first one's value, after the last the last one's; two points at
  one beat make a step (from that beat on, the later one counts: `valueAt` vs `leftValue`).
- `curve` (-1..1) bends the segment starting at that point; segments are exponential with `kCurvature` = 6 (the
  engine's `kAutomationCurvature`), so `splitAt` can split one into two that follow it exactly. `shape()` must
  match the engine.
- Mixer mappings: volume is a cube-law fader, 1 = `kMaxVolumeDb` (+6 dB), 0 dB at about 0.79, 0 = silence
  (`volumeToNormalized`, `normalizedToVolume`); pan maps -1..1 to 0..1. Sends and chain volumes map as volume.
- Edits (each returns a new envelope): `addPoint`, `movePointsMapped` (one point stays between its neighbours;
  several override what they land on), `deletePoints`, `setCurve`, `removeRange`, `copyRange`, `pasteRange`,
  `moveRange`, `dropRedundant`, `shift`, `normalize` (sort, clamp).
- `AutomationView{shown, key, lanes}`: what the arrangement shows of an owner's automation (view state). A lane with
  nothing chosen shows the first target automated (`ProjectEditor::defaultAutomationKey`: the envelope map's first),
  else the volume.

### Parameters

`ParamSpec` ([ParamSpec.h](../../app/src/model/ParamSpec.h)) describes a parameter whatever it belongs to: `key` (its
automation key), `name`, `group`, range (`minimum`, `maximum`), `defaultValue`, `unit`, `scale` (`Linear`, `Log`, or
`Fader` for a mixer volume), `steps` (discrete), `labels` and an optional `text` formatter. `toNormalized` /
`fromNormalized` are the same mapping as the engine's `ParamInfo` (evenly, in log(value), or in whole steps as VST3
maps stepped parameters), and [test_automation_model.cpp](../../tests/app/test_automation_model.cpp) holds the two
together. `ParamSpec::fromInfo()` makes one from an engine `ParamInfo`; `mixerSpecs`, `sendSpec` and `chainSpecs`
describe the mixer, sends and rack chain faders. The bridge hands them to the UI (`EngineBridge::paramGroups`).

### Keys and file names

`Key{tonic, minor}` ([Keys.h](../../app/src/model/Keys.h)); `Key::name()` (`"F#m"`, `"Bb"`) is what projects save and
`keyFromName` reads. `transposeTo(source, target)` is the smallest shift (a minor key and its relative major count as
the same; at a tritone it goes down), 0 if either is unknown. `parseFilename` finds a tempo (`128bpm`, `bpm128`, or a
lone number in 50..250) and a key (spelled `A minor`, `Ebmaj`; short `Am`, `F#m`; a bare capital only next to a tempo
or after "key", never as the first word). `clipSettings(name, duration, tempo, projectKey)` gives a new clip `warp`
and `segmentBpm` when the name has a tempo or the file is at least `kAutoWarpMinSec` (6 s) long (then at the project
tempo), and a `transpose` to the project key. Used by `ProjectEditor::addClips`. User-facing behaviour:
[guide/audio-clips.md](../guide/audio-clips.md).

### Built-in device state

[DeviceState.h](../../app/src/model/DeviceState.h) encodes a built-in device's non-parameter values as the engine
does (`BuiltinProcessor::encodeState`): `name=value` lines, a backslash escaping a backslash or a newline.
`deviceState::toModel`/`fromModel` wrap it in base64 for `Device::state`. The Sampler's one value is `"sample"`, its
file path (see [ui/device-view.md](../ui/device-view.md)).

## The Project object and its signals

`Project` ([Project.h](../../app/src/model/Project.h)) holds `tempo`, `timeSignature`, `key`, the loop
(`loopEnabled`, `loopStart`, `loopEnd`, beats), `automationLocked`, `master`, `tracks`, `returns`, `foldedDevices`,
`shownChainLists` (the racks whose chain list shows: hidden by default) and `hiddenRackDevices` (the racks whose
chain's devices don't show beside them: shown by default) (ids, view state), and `path`. QML reads the settings as properties (`tempo`, `loopEnabled`, `loopStart`, `loopEnd`,
`automationLocked`, `timeSignatureText`, `keyName`, `path`).

| Signal | When |
|---|---|
| `trackInserted(id, index)`, `trackRemoved(id, index)` | an arrangement track came or went |
| `returnInserted(id, index)`, `returnRemoved(id, index)` | a return came or went |
| `trackChanged(id)` | name (renamed; renumbered by a track coming, going or moving above it, after that track's signal; renamed by what it holds, after `clipsChanged` or `devicesChanged`), colour, mixer, sends, input, monitoring, arming, height, folding (`kMaster`: the master's mixer) |
| `tracksArranged()` | the order or groups changed (not which tracks there are) |
| `clipsChanged(track id)` | a track's clips were replaced, or what of its frozen audio plays |
| `devicesChanged(track id)` | devices added, removed, moved, toggled (in racks too), a sidechain, a MIDI input, macros or a rack's name changed |
| `chainChanged(track id, chain id)` | a rack chain's name or mixer |
| `deviceParamChanged(track id, device id, param id)` | one parameter |
| `deviceStateChanged(track id, device id)` | a device's state was set (a preset, a sample) |
| `devicesFolded(track id)` | devices on it folded or unfolded |
| `rackViewChanged(track id)` | a rack on it showed or hid its chain list, or its chain's devices |
| `freezeChanged(track id)` | it was frozen or unfrozen |
| `settingsChanged()` | tempo, time signature, key, loop, automation lock |
| `automationChanged(owner, key)` | an envelope was set or removed |
| `automationViewChanged(owner)` | what its automation shows |
| `reset()` | everything replaced (new, open) |
| `pathChanged()` | the file it is saved to changed |

Mutators, called only from commands (or directly for view state): `insertTrack`, `removeTrack`, `insertReturn`,
`removeReturn`, `arrangeTracks(tree)` (validates with `treeProblem` and throws `EditError`, changing nothing),
`updateTrack(id, field, value)` and `updateTrack(id, values)` (the settings a `TrackField` names; one `trackChanged`),
`replaceTrack` (a flattened track), `setFrozen`, `setFrozenSegments` (sorts; `clipsChanged`), `setClips` (sorts),
`setDevices`, `setChains` (several tracks' devices at once, all changed before any signal: a device moving between
tracks), `updateChain`, `setDeviceMacros`,
`setDeviceName`, `setDeviceParam(s)`, `setDeviceEnabled`, `setDeviceSidechain`, `setDeviceState`,
`setDevicesFolded`, `addFoldedDevices`, `setChainListShown`, `setRackDevicesShown`, `updateSettings`, `setEnvelope` (an empty envelope removes the target's
automation), `setAutomationView`, `replaceContents` (loading; emits `reset`), `clear`, `setPath`.
`storePluginState()` keeps a plug-in's current state (and where it was found) in the model without a signal or an
undo step: it isn't an edit, only what saving or copying the device needs (the bridge's `storePluginStates()`).

### Values held, references handed out

The project holds its tracks **by value**, in vectors. Its queries hand out references (`const Track&`,
`const Device&`) and pointers (`findTrack`, `findDevice`, `findClip`) into those vectors, valid **until the project
next changes**: a track inserted or removed moves the others, and any command replaces what it changes. Code that
keeps something across a change keeps its id and looks it up again. This matters most in slots reacting to the
project's signals (the id may be gone by then) and around edits that run while a reference is held.

Asking for a track, device, chain or clip that isn't there is a programming error: `track()`, `device()`, `chain()`,
`clip()` and `trackIndex()` throw `std::out_of_range`. Where an id may be gone, use `findTrack`, `findDevice`,
`findClip` (null) or `hasTrack`/`hasOwner`/`hasDevice`.

## The undo design

The session owns one `QUndoStack` and one `ProjectEditor(project, undoStack)`. The editor pushes commands from
[Commands.h](../../app/src/model/Commands.h):

| Command | Changes |
|---|---|
| `SetClipsCommand` | clip lists of one or more tracks (before and after by track id), and for a time selection over frozen tracks their frozen audio's segments |
| `InsertTrackCommand`, `RemoveTrackCommand` | an arrangement track |
| `ReplaceTrackCommand` | a track replaced by another of the same id (flattening) |
| `SetFreezeCommand` | a track frozen (its `Freeze`) or unfrozen |
| `InsertReturnCommand`, `RemoveReturnCommand` | a return track |
| `ArrangeTracksCommand` | order and groups (`TrackTree` before and after) |
| `UpdateTrackCommand` | one track setting (also `Sends`, `InputTrack`, `MidiInput`, `Monitor`) |
| `UpdateTrackFieldsCommand` | several settings of one track as one change (input channels and input track) |
| `UpdateTracksCommand` | one mixer setting on several tracks |
| `SetTempoCommand` | tempo plus the clip trims that keep unwarped clips from overlapping (`TempoState`) |
| `UpdateSettingsCommand` | time signature, key, loop |
| `SetDevicesCommand` | a track's whole device tree |
| `SetChainsCommand` | several tracks' devices at once (a device moving between tracks stays the same device) |
| `SetDeviceParamCommand`, `SetDeviceParamsCommand` | one parameter; several at once (a macro and what it moves) |
| `UpdateChainCommand`, `SetMacrosCommand`, `SetDeviceNameCommand` | a rack chain's name or mixer; a rack's macro mappings, with the values of the parameters a range change moves (merging per gesture); a rack's name |
| `SetDeviceStateCommand` | a device's state (a plug-in preset, a sampler's sample) |
| `ReplaceFilesCommand` | other files in some files' place: the clip lists of the tracks playing them and the states of the built-in devices naming them (`DeviceStates`), together; merging per hot swap (the same tracks and devices); `relink` for files only found somewhere else (frozen tracks take those) |
| `SetDeviceEnabledCommand`, `SetDeviceSidechainCommand`, `SetDeviceMidiFromCommand` | on/off; sidechain; MIDI input (`Device::midiFrom`, "Change MIDI Input" / "Remove MIDI Input") |
| `SetEnvelopeCommand`, `SetEnvelopesCommand` | one envelope; several (a range moved on several lanes) |

How it fits together:

- **Snapshots, not deltas.** Commands store whole before/after values (a clip list, a device tree, an envelope, a
  track) and put one or the other in place. They are values, so the project gets copies and the stack's are never
  the live model's. Most hold one such value: they are `ValueCommand`s (what they change, its `oldValue()` and
  `newValue()`), and each has only its `redo()` and `undo()`. `RemoveTrackCommand` keeps the track it removed and puts
  it back; it and `InsertTrackCommand` are the same classes as the returns' (`RemoveCommand`, `InsertCommand`) over
  the other list's `Project` methods.
- **Compound steps** are undo macros (`QUndoStack::beginMacro`/`endMacro`, opened by a scoped `Macro` in
  [EditorSupport.h](../../app/src/editor/EditorSupport.h), so it closes even when an edit throws): deleting tracks
  (inputs and sidechains from them dropped, the tracks, the sends into deleted returns and their automation),
  grouping, ungrouping, adding clips (with a new track), moving clips with their automation, deleting devices with
  their automation, recording.
- **Gestures merge.** A merge key is a `QString`; empty never merges. A command given one returns id `kMergeId`
  (0x6E1, `mergeId()`), and `mergeWith` accepts the next command of the same class with the same merge key and the same target,
  keeping the first's old value and the latest's new one (`MergeableCommand`). `SetClipsCommand` needs the same set
  of tracks; `ReplaceFilesCommand` the same tracks and devices, and turns obsolete (leaving the stack) when it is back
  where it began. The UI makes one key per gesture (`QUuid::createUuid().toString()`; the shared `Knob` and `ValueBox`
  controls hand one out with each `moved(value, gestureKey)`), so a fader drag, a knob drag or a breakpoint drag is
  one undo step. A plug-in editor's knob drag is one too: the session merges its edits by
  `"plugin edit|<device id>|<param id>|<gesture>"`.
- **Baselines for drags that trim.** `setTempo`, `updateClips` (segment BPM, warp) and `replaceFile` (a hot swap)
  trim clips that would run into the next one (`edits::fitToTempo`). While a drag (a hot swap) merges, they fit from
  the clips as they were when it began (the previous merged command's old value), so dragging up and back down (or
  trying a long sample, then a short one) doesn't leave clips trimmed.
- **Not undone** (changed directly on the project; saved with it): track `height`, `folded`, folded devices, `armed`,
  track and chain `solo` (`soloTracks`, `setChainParam(..., Solo, ...)`), the automation view, and
  `automationLocked`. These push nothing, so they don't mark the project as changed either.
- **Clean state.** The session uses `QUndoStack::isClean()` for the title's `*` and its `clean` property (the UI asks
  about unsaved changes before New, Open and Quit), `setClean()` after saving, and `resetClean()` when a plug-in reports a change no parameter shows
  (`EngineBridge::pluginStateDirty`).

### ProjectEditor

[ProjectEditor.h](../../app/src/editor/ProjectEditor.h) is the API the UI calls: one class, its operations grouped by
area in `Editor*.cpp`, all pushing their commands through `push()`. A new operation goes in the file of what it
edits. Its conventions:

- Ids are `QString`s; an empty one means none (no chain: a track's own; no group; no track: a new one). An index below
  0 means last.
- Operations that make something return its id (`""` if nothing was made). Results are copies, never references into
  the project.
- Continuous gestures pass a merge key.

Its signals: `pluginAdded(track id, device id)` when the user adds a plug-in (the session opens its editor; not on
undo or redo), `parameterTouched(owner, key)` when the user changes or takes hold of a parameter that can be
automated (the session shows its lane), and `refused(message)` (below).

Main operations, by area (`[Q]`: `Q_INVOKABLE`, callable from QML):

- **Tracks**: `addAudioTrack` [Q], `addMidiTrack` [Q] (with the default instrument, as its default preset has it; C++
  forms take a built-in instrument or a plug-in and a parent), `addMidiTrackWith` (a new MIDI track with an instrument
  preset, one step), `insertionPoint`, `deleteTracks` [Q] (a group with what is in it, a return with the sends into
  it and their automation; inputs, sidechains and MIDI inputs from them go: one step), `duplicateTracks` [Q] (new ids for tracks,
  clips, devices and rack chains; automation keys, automation view, sidechains, inputs and MIDI inputs among the
  copied tracks renamed to the copies; not armed), `copyTracks`, `cutTracks`, `pasteTracks` (`CopiedTracks`; a copy's name without a `#` is made unique),
  `renameTrack` [Q] (its name template: `"# Lead"`),
  `setTrackColor` [Q], `setTrackParam` / `setTracksParam` (mixer; the master only volume and pan), `soloTracks` [Q],
  `armTracks` [Q], `setTrackHeight` [Q], `setFolded` [Q].
- **Inputs**: `setTrackInput` (device channels), `setTrackInputTrack` (`EditError` for a cycle or a non-audio track),
  `setTrackMonitor`, `setTrackMidiInput`.
- **Groups**: `groupTracks` [Q] (Ctrl+G), `ungroup` [Q] (Ctrl+Shift+G), `moveTracks` [Q] / `canMoveTracks` [Q]. They
  go through one arranging step, which first drops any input or sidechain the new tree would turn into a cycle, in
  the same undo step.
- **Returns and sends**: `addReturnTrack` [Q], `setSend` (`EditError` for a cycle), `removeSend` [Q].
- **Settings**: `setTempo` [Q] (20..999, rounded to 0.01), `setTimeSignature` [Q], `setKey`, `setKeyByName` [Q],
  `setLoop` [Q], `setLoopEnabled` [Q], `setAutomationLocked` [Q].
- **Clips**: `commitClips`, `addClips` (audio files one after another, set up by `clipSettings`; a new audio track if
  needed), `addMidiClip`, `addMidiClipsOver`, `midiClipSpan`, `setClipNotes` (and `setClipsNotes`: several clips' notes in one
  undo step, for the piano roll editing them together, bends too), `moveClips` (with `clampTrackDelta`:
  only onto tracks of the same kind), `replaceClip`, `updateClips`, `deleteClips`, `splitClips`, `duplicateClips`,
  `consolidateClips` (Ctrl+J), time selections (`deleteRange` [Q], `duplicateRange`, `copyRange`, `cutRange`,
  `paste`, `pasteTargets`, `moveRange`, `movedRange` (what `moveRange` would make of the clips, and of frozen
  tracks' segments, without making it: a drag's preview), `reverseRange` (the audio clips in a range play reversed
  copies of their files, given by the
  caller; split at the range's edges), `setRangeActive` (0: the clips in a range deactivated, or activated again;
  split at the range's edges), `clipsArea`, `clipsInRange`, `clipsAt`), and `addRecordings` (takes become
  clips in one step; MIDI takes quantized to the record grid; the per-note bends played on a note, MIDI 2.0's, become
  its bend, drawn with as few points as keep its shape: `notes::simplifiedBend`).
- **A time selection is everything in it**: unless `automationLocked`, deleting, moving, copying, duplicating,
  cutting and pasting a range acts on the automation of every track in it as on its clips, whether the track has
  clips there or not (a group's too): only envelopes with breakpoints in the range; across tracks only the mixer's (a
  device's automation belongs to its track). `copyRange` copies a track with automation and no clips too. Over
  frozen tracks they take the frozen audio along ([Freezing](#freezing)); refused, `deleteRange` returns false,
  `duplicateRange`, `cutRange` and `paste` none, and `moveRange` where the range still is.
- **Devices**: `addDevice` [Q], `insertDevice(s)` (an instrument only on a MIDI track, first, replacing the one there;
  effects never before it), `copyDevices`, `pasteDevices` (sidechains kept unless the source is gone or would close a
  cycle; MIDI inputs kept unless the source is gone or is the track pasted onto; folded copies stay folded), `moveDevice(s)` [Q], `moveDevicesToTrack` [Q] (the same devices, so plug-ins
  keep their state; their automation moves with them in the same step), `removeDevice(s)` [Q], `setDevicesFolded`
  [Q], `setChainListShown` [Q], `setRackDevicesShown` [Q] (view state), `setDeviceDefaults` (where new devices come
  from: default presets). Changing a track's devices deletes the automation of devices (and rack chains) that left
  the track, and macro mappings to them, in the same step; and the mappings and automation of macros a rack no longer
  has.
- **A device's settings**: `setDeviceParam` [Q] (a C++ form takes `old` for plug-ins, whose values the model doesn't
  hold), `setDeviceParams` [Q] (several of a built-in device's at once: one step, merging per gesture while the same
  parameters change), `touchParameter` [Q], `setDeviceState`, `loadPresetInto` (a preset into a device of its kind:
  `loadsInto`), `renameRack` [Q], `setDeviceEnabled` [Q], `setDeviceSidechain`, `setDeviceMidiFrom` (`EditError`
  for a source that isn't a MIDI track; its own track, or `""`, is its own track's notes), `dropMidiInputs` (devices
  taking these tracks' notes take their own track's again: deleting tracks, and flattening them, which makes them
  audio tracks, do it in the same step).
- **Racks**: `groupDevices` [Q] (Ctrl+G: devices of one chain into a new rack), `ungroupRack` [Q] (refused if several
  instruments would come out), `addRackChain`, `removeRackChains` [Q], `duplicateRackChain` [Q], `moveRackChain` [Q],
  `renameChain` [Q], `setChainParam` [Q], `mapMacro`, `unmapMacro` [Q], `macroOf`, `macroTargets`, `setMacro` [Q]
  (the macro and every mapped parameter in one `SetDeviceParamsCommand`), `setMacroCount` [Q] (1..16: new macros
  last, at 0; those taken away with their values, mappings and automation, one step), `renameMacro` [Q] (`""` or
  "Macro N": by its number), `setMacroRange` [Q] (a mapping's `low`..`high`, held to 0..1; the parameter goes where
  its macro puts it in the new range, in the same `SetMacrosCommand`, merging per gesture).
- **Envelopes**: `setEnvelope`, `addAutomationPoint` [Q], `moveAutomationPoints`, `deleteAutomationPoints` [Q],
  `setAutomationCurve`, `clearEnvelope` [Q], `deleteAutomationRange`, `moveAutomationRange`,
  `duplicateAutomationRange`, `copyAutomationRange`, `cutAutomationRange`, `pasteAutomation`,
  `automationPasteTargets`.
- **Automation view** (not undone): `showAutomation` [Q], `hideAutomation` [Q], `toggleAllAutomation` [Q] (A),
  `addAutomationLane` [Q], `setAutomationLane` [Q], `removeAutomationLane` [Q], `resetAutomationView` [Q],
  `defaultAutomationKey` [Q].
- **Freezing**: `freezeTracks`, `unfreezeTracks` [Q], `flattenTracks` [Q], `laneFrozen` [Q].
- **Files**: `replaceFile(uses, path, seconds, text, mergeKey)` (a file in the place of whatever these clips and
  devices play, one `ReplaceFilesCommand`: each clip as `edits::replaceFile` has it, trimmed at the next one; refused
  on frozen tracks; with a merge key, from the clips as they were before the hot swap began), `relinkFiles({old:
  new})` (files found somewhere else: every clip playing one or reversed from it, every device naming it, frozen
  tracks too; "Locate Missing Files"). What plays a file (`FileUses`) is
  [files/ProjectFiles.h](../../app/src/files/ProjectFiles.h)'s ([session.md](session.md#the-file-manager-and-hot-swaps-filemanager-hotswap)).

Three hooks connect it to the rest without it knowing the bridge or the preset library; the session sets them
([session.md](session.md#wiring)): `setParamInfo` (how `paramInfo()` learns a plug-in's or a rack's parameter
mapping, for macros: the bridge's `deviceParamInfo`, made a `ParamSpec`), `setOwnValue` (a plug-in parameter's
current value, set in its own editor, for a macro's undo: `EngineBridge::ownValue`), and `setDeviceDefaults` (new
devices as the user's default preset has them: `defaultDevice` in [io/Presets.h](../../app/src/io/Presets.h)).

### Errors: EditError and the try forms

An edit the user can't make (a send that would close a cycle, an input of three channels, a channel out of range)
throws `EditError` ([Errors.h](../../app/src/model/Errors.h)) with a message for the user, as the C++ API. Nothing
callable from QML throws: the throwing operations have `try*` forms, `Q_INVOKABLE`, which report the message on
`refused` and return `false` (or `""`): `trySetTrackParam(track, field, value, key)` (field by its file name:
`"volume_db"`, `"pan"`, `"mute"`, `"solo"`), `trySetSendLevel`, `trySetSendPreFader`, `trySetTrackInput`,
`trySetTrackInputTrack`, `trySetTrackMonitor`, `trySetTrackMidiInput`, `trySetDeviceSidechain`, `trySetDeviceMidiFrom`, `tryAddRackChain`,
`tryMapMacro`. The `Q_INVOKABLE` operations also do nothing for ids that are gone (QML may hold stale ones). Edits
refused because of frozen audio are said on `refused` too. The session shows `refused` in the status line.

## Pure editing modules

[Edits.h](../../app/src/model/Edits.h) (`sub::app::edits`) and [Notes.h](../../app/src/model/Notes.h)
(`sub::app::notes`) take and return values and never touch Qt's objects or undo, so they are tested directly.

- `resolveOverlaps(clips, winners, tempo)`: Ableton's rule. Winners keep their place; every other clip on the track
  is trimmed, split or removed where a winner covers it (`cutClip`, `subtractIntervals`). The first piece keeps the
  clip's id.
- `removeRange`, `sliceRange` (new clips holding just a range; `keepIds` for clips wholly inside), `splitClip`,
  `trimStart` (a MIDI clip revealing time before its content shifts its notes so they stay put), `trimEnd` (audio
  limited by the source), `fitToTempo` (the same clips if nothing changes; `changed` says whether anything did),
  `consolidateMidi` (what the clips play: a deactivated clip's notes come deactivated, unless every clip is), `reverseClip` (a clip playing a reversed copy of its file: the same stretch of audio, its offset
  mirrored), `selectionSpan`.
- `replaceFile(clip, path, totalSec)` (another file in its place: where it is, with its settings; a clip playing all
  of its file, `playsWholeFile`, plays all of the new one, one playing a stretch plays the same stretch as far as the
  new file goes; named after the file, forwards, its fades held to its length), `relinkFile(clip, from, to)` (a file
  found somewhere else, also what it was reversed from; nothing else changes; paths compared by `samePath`,
  [Paths.h](../../app/src/model/Paths.h)).
- `stretchClip(clip, edgeBeat, left, tempo)` (Alt-dragging an edge): that edge moves, the other stays, and the
  content plays faster or slower to fill it: an audio clip is warped to the segment BPM that makes it that long
  (within `kMinSegmentBpm`..`kMaxSegmentBpm`, 20..999), a MIDI clip's notes and offset are scaled; never before
  beat 0. `slipClip(clip, deltaBeats, tempo)` (Ctrl+Shift-dragging the body): the clip stays, its content moves by
  the delta (audio within its file; a MIDI clip's notes anywhere, shifted as `trimStart` does when the offset would
  go below 0). `fadeClip(clip, out, beats, tempo)` and `curveFade(clip, out, curve)` set a fade (held to what the
  other leaves) and its curve.
- Fades stay at a clip's ends: `piece()` (so `cutClip`, `removeRange`, `sliceRange`, `fitToTempo`) gives a piece
  from inside a clip no fade at that side, `splitClip` gives the left half the fade in and the right the fade out,
  and the trims keep both, held to the new length.
- Minimum sizes: `kMinClipSec` (5 ms), `kMinMidiClipBeats` and `notes::kMinNoteBeats` (1/64).
- `notes::place(notes, removed, added)` is how the piano roll commits: added notes win where they overlap others on
  their key (`resolveOverlaps`); notes changed together are made consistent by `untangle`. `normalize` sorts and
  removes exact duplicates. `legato`, `timeScaled`, `quantized` (`kQuantizeGrids`), `humanizedTiming`
  (starts only, by up to `kHumanizeBeats` = a 32nd at 100 %; a triangular random, more often a little than a lot,
  from the `QRandomGenerator` given). Humanized velocities are the velocity model's
  ([intelligence.md](../intelligence.md#humanizing-velocities-by-machine-learning)).
- Bends, for the piano roll's bend mode: `withBendPoint` (a point added, kept sorted, after any other at its time;
  says where it went), `withBendPointsMoved` (points moved together stay in order between those not moved, and
  within the note), `withoutBendPoints`, `withBendCurve`, `withVibrato` (a new vibrato takes the stretch it covers
  from those already there: they are shortened, split, or go; held to the note, at least `kMinVibratoBeats`),
  `withoutVibrato`, `nextNote` (the note a slide goes to: the next to start after it, of a chord the nearest in
  pitch), `withSlide` (a slide over a stretch as two points, from the curve's value at its start to an interval at
  its end, replacing the points it covers), and `simplifiedBend` (Ramer-Douglas-Peucker: a recorded bend's many
  points drawn with few, within 0.05 semitones). Note edits carry bends: `withStart` (a trimmed start keeps the bend where it was in time:
  `resized` and `resolveOverlaps` use it), `timeScaled` scales them, and `lessFull` orders notes alike but for
  their bends (the piano roll's note sets are keyed by it).

## Invariants

- `Project::tracks()` satisfies `treeProblem(...)` being none at all times; `arrangeTracks` refuses anything else.
- The routing graph has no cycles: every place that adds an edge (`setSend`, `setTrackInputTrack`,
  `setDeviceSidechain`, arranging tracks, `moveDevicesToTrack`, `pasteDevices`) checks it first, and loading repairs
  edited files (`repairRouting`, [serialization.md](serialization.md#repairs-on-load)).
- Device ids, chain ids and track ids are unique in the project (`newId()`: 12 hex digits of a random UUID);
  copied devices always go through `refreshIds`, copied tracks and clips get new ids.
- A track's `automation` never holds an empty envelope; a device's automation lives on the track the device is on,
  keyed by its id.
- On a MIDI track an instrument (or instrument rack) is first in its chain, and there is at most one; audio tracks,
  groups, returns and the master take no instrument.
- Racks never nest deeper than `kMaxRackDepth`.
- Macro mappings only name devices inside their rack, and macros the rack has.
- A rack has 1 to 16 macros, and a value for each.
- A device's `midiFrom` names a MIDI track other than its own, or is empty: deleting or flattening the source takes
  it away in the same step, and loading repairs edited files.
- A note's bend points are sorted by time and its vibratos by start; edits keep vibratos from overlapping (each at
  least `kMinVibratoBeats` long) and every value within ±48 semitones, and loading sorts both and holds them to
  the range.

## Extending it

- **A new undoable edit**: a `ProjectEditor` method in the `Editor*.cpp` of its area that computes the new value with
  the pure functions and pushes an existing command; only add a command when no existing one replaces the right
  thing. If it needs a new kind of change on `Project`, add a mutator that emits a signal, and handle that signal in
  the engine bridge and the UI. If QML calls it, make it `Q_INVOKABLE` and make sure it can't throw (a `try*` form if
  it can refuse).
- **A new track field**: add it to `Track` (with a default, so older files load), and to `TrackField`,
  `trackFieldName` and `Track::value`/`setValue` if `updateTrack` sets it; decide whether it is undoable (a command)
  or view state (set directly); and add it to [Serialization.cpp](../../app/src/io/Serialization.cpp) (bump
  `kProjectVersion`; see [serialization.md](serialization.md)).
- **A new automation target kind**: a key form in `Automation.h` (`parseKey` and the classifiers), a `ParamSpec`, its
  own value in `EngineBridge::ownValue`, and the engine lane in `EngineBridge::engineLane`
  ([BridgeParameters.cpp](../../app/src/audio/BridgeParameters.cpp)).
- **A new built-in device**: nothing here. It comes from the engine ([engine/devices.md](../engine/devices.md)).

## Gotchas

- **References go stale.** A `const Track&` or `const Device*` from the project is invalid after the next change (see
  [above](#values-held-references-handed-out)). Keep ids, not references, across edits.
- `Project`'s mutators don't check that a change is undoable; calling them from the UI skips undo. Only view state may
  do that.
- The model links the engine: `Devices.h` includes the engine's `builtin/BuiltinRegistry.h` (the built-in devices come
  from there). That header is the engine's; the UI may not include it directly.
- A plug-in's `Device::state` is only as fresh as the last `EngineBridge::storePluginStates()`. Copying devices,
  duplicating tracks or chains and saving presets must store states first (the session does).
- A plug-in's `params` only hold values the host changed; its real values live in the plug-in. Use
  `EngineBridge::ownValue` to read them.
- `Clip::segmentBpm` 0 means "not set": a clip with `warp` on but no BPM isn't warped.
- MIDI clips accept a `tempo` they ignore, so both kinds of clip can be edited alike.
- `Track::sends` must be replaced whole, never changed in place: the commands compare old and new maps.
- `Track::automation` keeps the order targets were first automated in (an `OrderedMap`): the
  first one is what a lane shows by default.
- `sub_app` is built with `QT_NO_KEYWORDS`: write `Q_SIGNALS`, `Q_SLOTS` and `Q_EMIT`.

## Tests

- [test_project.cpp](../../tests/app/test_project.cpp), [test_commands.cpp](../../tests/app/test_commands.cpp): the
  project's queries, the group tree and routing graph, each command and its signals, merging.
- [test_edits.cpp](../../tests/app/test_edits.cpp), [test_midi_model.cpp](../../tests/app/test_midi_model.cpp),
  [test_automation_model.cpp](../../tests/app/test_automation_model.cpp),
  [test_timebase.cpp](../../tests/app/test_timebase.cpp), [test_keys.cpp](../../tests/app/test_keys.cpp): the pure
  functions, the parameter mappings against the engine's.
- [test_editor_edits.cpp](../../tests/app/test_editor_edits.cpp): overlaps, trims, splits, ranges, tempo fitting,
  moves across tracks, gesture merging, inputs undoable and arming not, recorded takes, copy and paste and automation
  carried with clips, time selections over groups and tracks without clips, `movedRange`, reversing.
- [test_editor_midi.cpp](../../tests/app/test_editor_midi.cpp),
  [test_editor_automation.cpp](../../tests/app/test_editor_automation.cpp),
  [test_editor_duplicate.cpp](../../tests/app/test_editor_duplicate.cpp),
  [test_editor_groups.cpp](../../tests/app/test_editor_groups.cpp),
  [test_editor_sends.cpp](../../tests/app/test_editor_sends.cpp),
  [test_editor_resampling.cpp](../../tests/app/test_editor_resampling.cpp),
  [test_editor_sidechain.cpp](../../tests/app/test_editor_sidechain.cpp),
  [test_editor_midi_from.cpp](../../tests/app/test_editor_midi_from.cpp),
  [test_editor_racks.cpp](../../tests/app/test_editor_racks.cpp),
  [test_editor_presets.cpp](../../tests/app/test_editor_presets.cpp),
  [test_editor_freeze.cpp](../../tests/app/test_editor_freeze.cpp): each feature's rules, undo, cycles refused.
- [test_editor_frozen_areas.cpp](../../tests/app/test_editor_frozen_areas.cpp): time selections over frozen tracks
  and groups (delete, move, copy, duplicate, cut, paste) with their frozen audio, in one undo step; what is refused;
  unfreezing and flattening afterwards.
- [test_note_bends.cpp](../../tests/app/test_note_bends.cpp): bends and vibratos, the pure functions (order, moves,
  vibratos taking their stretch, slides into the next note, note edits carrying bends), the model bending as the
  engine does, recorded bends
  drawn with few points and becoming the notes' bends, project files (and damaged bends refused), and bent notes
  playing at their bent pitch.
- [test_file_manager.cpp](../../tests/app/test_file_manager.cpp): files replaced and relinked (the pure functions,
  the editor's one step, a hot swap's merging and its baseline, frozen tracks), the File Manager and hot swaps.
- [test_session_engine.cpp](../../tests/app/test_session_engine.cpp) and the `test_bridge_*` tests: the engine
  hearing the editor's edits.

See [testing.md](../testing.md) for the whole suite.
