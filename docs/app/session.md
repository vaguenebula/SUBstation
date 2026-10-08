# The session

`Session` ([app/src/session](../../app/src/session)) is one open project and everything that works on it: the main
window's logic, without its drawing. It owns the project, the undo stack, the editor, the selection, the engine
bridge, the browser and the plug-in index, and wires them together. Its parts hold the logic behind the views that
isn't drawing: the arrangement's clipboard and its Edit commands, the device view's selection and actions, renders in
the background, the computer MIDI keyboard, the audio and MIDI preferences.

The UI reaches all of it through the session: QML as the `Session` singleton (`import SUBstation`; registered by
`sub::ui::registerSession` in [ui/src/app/AppTypes.cpp](../../ui/src/app/AppTypes.cpp)), the UI's C++ items through a
`session` property (`Q_PROPERTY(sub::app::Session* session ...)`, set from QML as `session: Session`) and its
accessors. The UI shows dialogs and asks the session to act; the session itself never asks the user anything.

## Files

| File | What it holds |
|---|---|
| [Session.h](../../app/src/session/Session.h) | `Session`: its parts, properties, invokables and signals; its header comment is the overview. `Session.cpp`: making the parts and wiring them, the title, start-up and shut-down |
| [SessionFiles.cpp](../../app/src/session/SessionFiles.cpp) | New, open, save, the recent projects, the last folder, Export Audio |
| [SessionEditing.cpp](../../app/src/session/SessionEditing.cpp) | The Create and Edit commands on what is selected (dispatched to the arrangement's actions, or the device view's while it has the focus), adding to the selected track, freezing |
| [SessionTransport.cpp](../../app/src/session/SessionTransport.cpp) | Play, record (with the count-in), stop, locate |
| [Selection.h](../../app/src/session/Selection.h) | `Selection`: what is selected in the arrangement |
| [ArrangementActions.h](../../app/src/session/ArrangementActions.h) | `ArrangementActions`: the clipboard for clips, automation and tracks, and what acts on the arrangement's selection |
| [DeviceSelection.h](../../app/src/session/DeviceSelection.h) | `DeviceSelection`: the device view's selection, clipboard, presets and drops |
| [RenderProgress.h](../../app/src/session/RenderProgress.h), [Renders.h](../../app/src/session/Renders.h) | The render progress the UI shows as a modal dialog; the renders in the background (`ExportAudioRender`, `FreezeTracksRender`, `ReverseClipsRender`) |
| [ComputerKeyboard.h](../../app/src/session/ComputerKeyboard.h) | The computer MIDI keyboard |
| [AudioPreferences.h](../../app/src/session/AudioPreferences.h), [MidiPreferences.h](../../app/src/session/MidiPreferences.h) | Preferences › Audio and › MIDI, without their widgets |
| [SessionSupport.h](../../app/src/session/SessionSupport.h) | Small helpers the parts share (plug-in refs from QML maps, clip refs for QML) |

## What it owns

`Session(engine, options)` makes, in order: the `Project`, the `QUndoStack`, the `ProjectEditor`, the `Selection`, the
`EngineBridge` (on the engine it is given, which outlives it), the `PluginIndex`, the `SoundSimilarity` (which
analyses the browser's files: [intelligence.md](../intelligence.md)), the `Harmony` (the song's chords and key), the `BrowserController` (which starts the plug-in
scan, and points the sound similarity at its index), the `PresetIndex`, the `RenderProgress`, and the parts `ArrangementActions`, `DeviceSelection`,
`ComputerKeyboard`, `AudioPreferences`, `MidiPreferences`. They are its children, and QML reads them as constant
properties:

| Property | Type | What it is |
|---|---|---|
| `project`, `undoStack`, `editor`, `selection`, `bridge` | `Project`, `QUndoStack`, `ProjectEditor`, `Selection`, `EngineBridge` | [model.md](model.md), [engine-bridge.md](engine-bridge.md) |
| `browser`, `plugins` | `BrowserController`, `PluginIndex` | the browser ([browser.md](../browser.md)); the plug-ins, also Preferences › Plug-ins ([plugin-scanner.md](plugin-scanner.md)) |
| `similarity` | `SoundSimilarity` | finding similar sounds ([intelligence.md](../intelligence.md)); the browser's Find Similar uses it |
| `harmony` | `Harmony` | the song's chords and key, from its MIDI ([intelligence.md](../intelligence.md#harmony-the-application-side)); the piano roll shows them and Generate writes from them |
| `arrangement` | `ArrangementActions` | [below](#the-arrangements-actions-arrangementactions) |
| `deviceSelection` | `DeviceSelection` | [below](#the-device-view-deviceselection) |
| `render` | `RenderProgress` | [below](#renders-in-the-background) |
| `computerKeyboard` | `ComputerKeyboard` | [below](#the-computer-midi-keyboard) |
| `audioPreferences`, `midiPreferences` | `AudioPreferences`, `MidiPreferences` | [below](#preferences) |

`Session::Options` is for the tests: `scanner` (the plug-in scanner's program; empty: `substation-scan` beside the
application), `scanPlugins` (scan at once), `browserIndex` (keep the browser's index and the sounds' fingerprints on
disk) and `analyseSounds` (fingerprint the browser's files in the background).

`setOwnerWindow(handle)` (from `ui/main.cpp`) gives the bridge the main window's native handle, which plug-in editors
and ASIO drivers' dialogs belong to. `start()` (queued by `ui/main.cpp` once the window shows) starts audio
(`EngineBridge::startAudio`: the render threads, the MIDI inputs, the saved device). `shutdown()` (after the event
loop, or when the session goes) aborts a render that runs, stops the transport and the preview, closes the plug-in
editors, stops the browser's threads, then the sound similarity's (saving the fingerprints), closes the device and shuts the bridge down (the plug-ins unload while the
application is still whole).

## Wiring

The session connects its parts to each other in `Session::wire()`:

- **The editor's hooks**: `setParamInfo` ← `EngineBridge::deviceParamInfo`, made a `ParamSpec` (`ParamSpec::fromInfo`),
  for macros; `setOwnValue` ← `EngineBridge::ownValue`; `setDeviceDefaults` ← `defaultDevice` (the user's default
  presets, [io/Presets.h](../../app/src/io/Presets.h)).
- **Messages**: the bridge's, the browser's, the plug-in index's, the arrangement's, the device view's and the computer
  keyboard's `statusMessage`, and the editor's `refused`, all go out on the session's `statusMessage`.
- **Plug-ins**: the plug-in index's results go to `EngineBridge::setKnownPlugins` (so projects find plug-ins that
  moved); the editor's `pluginAdded` opens the plug-in's editor on the next turn of the event loop (the add, a drop,
  has finished, and the track may be selected just after), if its track is still there; a plug-in editor's
  `pluginParamEdited` becomes `ProjectEditor::setDeviceParam` with the merge key
  `"plugin edit|<device id>|<param id>|<gesture>"` (one undo step per knob drag), `pluginParamTouched` becomes
  `touchParameter`; `pluginStateDirty` calls `QUndoStack::resetClean()` (the project has changes no edit shows).
- **The selection**: when it changes, only the selected track's plug-in editors show
  (`EngineBridge::showPluginEditors`) and its plug-ins load first (`prioritizePlugins`). The selection drops what no
  longer exists whenever tracks, clips or automation change (`Selection::prune`), and a parameter changed by hand (or
  taken hold of: `parameterTouched`) shows its automation lane, if it can be automated.
- **Recording**: `takesRecorded` → `ProjectEditor::addRecordings(takes, recordQuantize())`, one undo step; the new
  clips are selected.
- **The browser**: previews go to the bridge (`previewFile`, `stopPreview`); what it activates (a double-click, Enter)
  goes on the selected track (`addFileAtInsert`, `addDeviceToSelectedTrack`, `addPluginToSelectedTrack`,
  `addPresetToSelectedTrack`); its presets are the `PresetIndex`'s, listed again when the library changes.
- **State for the UI**: `automationOverridden` follows `EngineBridge::automationStateChanged`; `pluginsLoaded`,
  `pluginsTotal` and `pluginsLoadingText` follow `pluginsLoading`; `clean` and `title` follow the undo stack's clean
  state and the project's path.
- **Renders**: the arrangement's reversing starts its renders through the session (`startRender`), so there is one
  render at a time.

## The QML-facing API, in outline

The full list, with what each does, is in [Session.h](../../app/src/session/Session.h).

| Area | Properties and invokables |
|---|---|
| Window | `title` ("<project name>[*] - SUBstation"), `clean`, `statusTimeout` (8000 ms), `aboutTitle`, `aboutText`, `requestClose()` |
| Transport | `togglePlay()` (Space: plays from the insert marker; again, stops and goes back to where playback started), `toggleRecord()` (F9: records the armed tracks from the insert marker after the count-in, or from the playhead while playing; again, stops recording while playing goes on), `stop()` (the stop button; stopped, it goes to the start), `locate(beat)`, `countInBars` (0, 1, 2 or 4) with `countInChoices`, `recordQuantize` with `recordQuantizeChoices`, `automationOverridden` |
| Create | `insertAudioTrack()`, `insertMidiTrack()` (after the selected track, in its group; selected), `insertReturnTrack()`, `insertMidiClip(gridStep)`, `groupSelected()`, `ungroupSelected()` (tracks, or devices while the device view has the focus), `deleteSelectedTracks()` |
| Adding | `addDeviceToSelectedTrack(kind)`, `addPluginToSelectedTrack({format, uid, name, vendor, path, instrument})`, `addPresetToSelectedTrack(path)` (an instrument with no MIDI track selected: on a new MIDI track), `addFileAtInsert(path)`, `presetSaved(path)` |
| Edit | `cut()`, `copy()`, `paste()`, `duplicate()`, `whatIsCopied(verb)`, `deleteSelection()`, `split()`, `selectAll()`, `consolidate()`, `reverseClips()`, `toggleClipActivation()`, `soloSelectedTracks()`, `renameTarget(browserListFocused)` |
| Freezing | `toggleFreeze()` (Ctrl+Shift+F), `flattenSelectedTracks()`, `freezeActions(ids)` (a track menu's texts and states), `freezeTracks(ids)`, `unfreezeTracks(ids)`, `flattenTracks(ids)` |
| Files | `newProject()`, `openProject(path)`, `saveProject()`, `saveProjectAs(path)`, `suggestedSavePath()`, `recentProjects`, `recentProjectAvailable(path)`, `recentMenuItems()`, `clearRecentProjects()`, `lastFolder`, `projectFilter`, `projectExtension`, `confirmDiscardText` |
| Export | `exportRangeChoices()`, `exportProblem(range)`, `suggestedExportPath()`, `exportBitDepthChoices`, `defaultExportBitDepth` (24), `exportAudio(path, range, bitDepth)` |
| Plug-ins loading | `pluginsLoaded`, `pluginsTotal`, `pluginsLoadingText` ("Loading plug-ins: 1 of 3") |

Signals: `statusMessage(text)` (the status line), `warning(text)` and `information(text)` (what were message boxes:
the UI shows them), `saveAsRequested()`, `projectOpened()` (the arrangement shows all of the project), and the
properties' notifications.

The Edit commands act on what has the focus (`Selection::focus()`): the device view's selected devices while it has
it (`DeviceSelection`), else the arrangement: selected breakpoints, a lane range's automation, a clip range, or the
selected tracks (`ArrangementActions`). `whatIsCopied()` says which ("devices", "automation", "clips", "tracks", or ""
with a status message saying why not, for breakpoints, returns and the master).

`renameTarget()` (Ctrl+R) says what to rename, for the UI to edit in place: the preset current in the browser's list
(if the list has the focus), the rack chain last clicked in the device view, or the track (return) last clicked:
`{kind: "preset", path, name}`, `{kind: "chain", trackId, rackId, chainId, name}` or `{kind: "track", trackId, name}`;
`{}` if none (a status message says so). The UI then calls `browser.renamePreset`, `editor.renameChain` or
`editor.renameTrack`.

## Files and unsaved changes

The session never asks the user anything; the UI does, and then calls it:

- **New, Open, Open Recent and Quit**: if the project isn't `clean`, the UI asks `confirmDiscardText` ("Save changes to
  the current project?": Save, Discard, Cancel). On Save it saves (below) and goes on only if that returned true. For
  Open Recent it first calls `recentProjectAvailable(path)`: a file that is gone is taken off the list, with a
  `warning`, and nothing opens.
- **Save** (Ctrl+S): `saveProject()` saves to the project's file and returns true; for a project never saved it emits
  `saveAsRequested()` and returns false, and the UI asks where (starting at `suggestedSavePath()`:
  `<last folder>/Untitled.gilproj`) and calls `saveProjectAs(path)`. Saving stores the plug-ins' states first
  (`EngineBridge::storePluginStates`), writes the file ([serialization.md](serialization.md)), marks the undo stack
  clean, remembers the folder and the file (the recent projects), and says "Saved <file>". A file that can't be
  written is a `warning`.
- **Open**: `openProject(path)` loads the file (one that can't be read: a `warning`, and the open project stays as it
  was), clears the undo stack, the selection and the insert marker, remembers the folder and the file, emits
  `projectOpened()` and says "Opened <file>". Its plug-ins load after it shows ([engine-bridge.md](engine-bridge.md#opening-a-project)).
- **New**: `newProject()` empties the project, the undo stack and the selection.
- **Close**: `requestClose()` is false while a render runs: it cancels the render instead, and the window stays (as
  the render dialog's Cancel would). Otherwise the UI asks about unsaved changes as above.
- **Recent projects**: at most `kMaxRecent` (10), the latest first, each stored absolute with links resolved, in the
  system's form; one that differs only in case from another (casefolded) replaces it. `recentMenuItems()` gives the
  Open Recent menu's entries (`"&1  song.gilproj"`, with `&` doubled in names). A list of one, which QSettings can hand back
  as a single string, is read as a list too.

## The selection (Selection)

`Selection` ([Selection.h](../../app/src/session/Selection.h)) is what is selected in the arrangement: the selected
track (or tracks, a return track, or the master), the insert (start) marker, and a time selection: a beat range over
one or more adjacent tracks. Selecting is always on the grid: selecting a clip selects the area it covers.

- A time selection made in the clips' band (or by clicking clips) is a *clip range*: `clips()` holds the clips it
  touches (for the clip view), and Delete cuts out just that range. Made lower in the lanes it is a *lane range*;
  made in automation lanes, `lanes()` holds them (owner, key), and Delete clears their automation in the range.
- A time selection acts on `rangeTrackIds()` (its `TimeRange::trackIds`): the tracks it covers and what is in the
  groups among them. `rangeRows()` are the tracks whose rows it covers, top to bottom, where the arrangement shows it
  (`setTimeRange(..., rows)`; by default all of its tracks): a selection made over a group's row acts on the group's
  tracks too, but shows only over the rows it was made over.
- Automation breakpoints can be selected too (`points()`: owner, key, indices); Delete deletes them.
- Several tracks can be selected (`selectTrack(id, focusTrack, mode, order)`: `Mode::Toggle` for Ctrl-click,
  `Mode::Range` for Shift-click from the last one clicked); `trackIds()`. `trackId()` is the one the device view
  shows, the last one clicked.
- `focus()` (`Focus::Clips`, `Track`, `Devices`, `Automation`) is what Delete, Cut and Copy act on.
- `prune(project)` drops what no longer exists (the session calls it whenever tracks, clips or automation change).

## The arrangement's actions (ArrangementActions)

`ArrangementActions` (`Session.arrangement`, [ArrangementActions.h](../../app/src/session/ArrangementActions.h)) acts on
the arrangement's selection. The arrangement view calls it (a paste at a clicked beat on a clicked track, its context
menus, drops), and the session dispatches the Edit menu's commands to it.

- **One clipboard** holds the last thing copied or cut (Ctrl+C / Ctrl+X): clip content from a clip range
  (`ClipboardContent`), automation from a lane range (`CopiedAutomation`), or tracks (`CopiedTracks`; their plug-ins as
  they were: their states are stored first). Ctrl+V pastes it (`paste()`, or `paste(atBeat, trackId)`;
  `pasteAutomationAt(owner, key)`). A cut that was refused (nothing taken out) leaves the clipboard as it was.
  `hasClipboard`, `clipboardKind`.
- An area command the editor refuses (a stretch of only some of a frozen group, a paste into a frozen track of what
  wasn't copied from it: see [model.md](model.md#freezing)) changes nothing, the selection included; the editor's
  `refused` says why in the status line. What it selects afterwards keeps the rows the selection showed over.
- `deleteArea()`, `duplicateArea()`, `copyArea()`, `cutArea()`, `copyAutomation()`, `cutAutomation()`,
  `copyTracks(ids)`, `cutTracks(ids)`, `duplicateTracks(ids)`, `consolidate()`, `splitAt(beat)`, `canConsolidate()`,
  `canReverse()`, `insertMidiClip(trackId, beat, gridStep)`, `insertTrackAfter(trackId, midi)`.
- **Deactivating** (0, `toggleActivation()`): the clips in the selected area, audio and MIDI, deactivated
  (`ProjectEditor::setRangeActive`: just the stretch of each inside it, one undo step), or activated again if they
  all are; frozen tracks' clips stay as they are (their frozen audio holds them). `activates()` says which (none:
  nothing to change), for the menu; `canToggleActivation()` and `areaDeactivated()` are its QML forms.
- **Reversing** (R, `reverseSelection()`) writes a reversed copy of each file once (as Ableton does): less than
  `kReverseInPlaceSeconds` (30 s) of audio in all is written at once; more in the background (`ReverseClipsRender`),
  followed in the session's render progress: Cancel makes none, and nothing is reversed. Then
  `ProjectEditor::reverseRange` turns the clips to the copies, one undo step.
- **Drops** on the lanes: `dropSources(paths)` (what a drag of files would add: name and length, for the drop's
  preview), `dropFiles(paths, trackId, beat)`, `dropDevices(kinds, plugins, trackId)` (`""`: below the tracks, a new
  track), `dropPresets(paths, trackId)`, `dropMovedDevices(fromTrack, ids, toTrack)`.
- `clipViewRequested(refs, leadTrackId, leadClipId)` asks the UI to open clips in the clip view (a MIDI clip inserted,
  a clip double-clicked).

## The device view (DeviceSelection)

`DeviceSelection` (`Session.deviceSelection`, [DeviceSelection.h](../../app/src/session/DeviceSelection.h)) is the
device view without its widgets. It shows the selected track's chain (`trackId`: a track, a return or the master), and
after each rack, unless it is folded or hides its devices, the chain it shows (the one last clicked in its chain list:
`shownChain()`, else its first), and so on inside: `shownDevices`. A frozen track (or one in a frozen group) shows none
(`hint`). `toggleChainList(rack)` and `toggleRackDevices(rack)` show or hide a rack's chain list (hidden by default)
and its chain's devices (shown by default): view state (`ProjectEditor::setChainListShown`, `setRackDevicesShown`).
Clicking a chain (`clickChain`) shows its rack's devices again.

- **Selecting**: a click selects a device, Ctrl adds or removes it, Shift selects the range from the last one clicked
  (`anchor`); a selection is of one chain's devices. A press on a device already selected keeps the others (to drag
  them all), and if no drag follows, the release selects just it (`press()`, `release()`). Selecting devices focuses
  the device view (`Selection::focusDevices`: Delete and the clipboard act on them); selecting anything else
  deselects them. `selected` is in chain order. `modifiers` arguments are `Qt.KeyboardModifiers` as an int.
- **The clipboard** is the device view's own: the devices copied or cut (plug-ins in their state then), and which of
  them were folded: their copies are folded too. `copySelected`, `cutSelected`, `paste()`, `pasteAt(chain, index)`,
  `pasteAfter(id)`, `duplicateSelected`.
- **Racks**: `groupSelected`, `groupDevice`, `ungroupSelected`, `ungroupRack`, `toggleFold`, `clickChain`.
- **Presets**: `savePreset(id, name)` (after `checkPresetName(id, name)`: an error, or the question before replacing a
  preset of that name), `saveAsDefault`, `clearDefault`, `hasDefault`, `insertPreset`, `loadPresetInto`,
  `presetLoadsInto`, `loadPresetFile` (*Load Preset…*, starting in `presetFolder()`). A plug-in's own `.vstpreset`
  files are loaded (`loadVst3Preset`: an undoable `SetDeviceStateCommand`) and saved here too, starting in
  `vst3PresetFolder()`. The folders last used are remembered ([Settings](#settings)).
- **Drops**: `dropMoved(ids, chain, index)`, `dropDevices(kinds, plugins, chain, index)`,
  `dropPresets(paths, chain, index, intoDeviceId)` (onto a device of the preset's kind: loaded into it), `dragEnded()`.
- Texts the view shows: `kEffectsHint`, `kInstrumentHint`, `kInstrumentRefused`.

## Renders in the background

Exporting, freezing and reversing long files render on threads of their own while the window goes on (it repaints, its
meters move) but takes no edits: the render is of the project as it was when it started. One render runs at a time
(`startRender` refuses another).

The renders ([Renders.h](../../app/src/session/Renders.h)) are state machines driven by their tasks' signals
(`RenderTask::progressChanged`, `ended`: [engine-bridge.md](engine-bridge.md#renders-in-the-background-rendertask))
and by Cancel. There are **no nested event loops**: nothing waits in a local loop for a render to end, so nothing can
re-enter the session meanwhile. Each render:

1. shows itself in the session's `RenderProgress` (`begin(title)`);
2. waits, if it must, until every device is ready to render offline (a project's plug-ins still loading, samples): a
   busy bar meanwhile, saying what it waits for, polling `EngineBridge::devicesReady()` every 30 ms (the bridge goes on
   loading them meanwhile);
3. starts its task(s) and follows each, its part of the progress bar, until it ends;
4. finishes it (`finishExport`, `finishFreeze`, `finishReversed`), says what came of it (`statusMessage`, `warning`),
   ends the progress, and deletes itself.

| Render | What it does |
|---|---|
| `ExportAudioRender` | the arrangement (or the loop) into a WAV file, once the devices are ready (`Session::exportAudio`; playback stops first) |
| `FreezeTracksRender` | several tracks, one after another, then frozen in one undo step: all or nothing (cancelled, or one failing, none is frozen, and the renders made so far are deleted) |
| `ReverseClipsRender` | reversed copies of long files, written side by side and followed one after another; cancelled, none is used |

`RenderProgress` (`Session.render`, [RenderProgress.h](../../app/src/session/RenderProgress.h)) is what the UI binds its
modal dialog to: `active` (show it), `title`, `label`, `progress` (0..1), `busy` (a busy bar while plug-ins or samples
load), `cancelled` ("Cancelling…": Cancel is disabled until the render has stopped). `cancel()` is the dialog's Cancel
(and Esc, and closing it); Cancel never takes the keyboard focus, so Space, meant for the transport, doesn't cancel.
`requestClose()` cancels a render too, and `shutdown()` aborts one (waiting for its threads; nothing changes and its
files go).

## The computer MIDI keyboard

`ComputerKeyboard` (`Session.computerKeyboard`, [ComputerKeyboard.h](../../app/src/session/ComputerKeyboard.h)), as in
Ableton: while it is on (M: `toggle()`, `enabled`), the letter keys play notes into the MIDI tracks that hear it, as a
MIDI input called "Computer Keyboard" (`kComputerKeyboard`; every track on All Ins hears it too).

- The middle row is the white keys from C (A, S, D, F, G, H, J, K, L, ;, '), the row above it the black keys between
  them (W, E, T, Y, U, O, P). Z and X move the octave down and up (`shiftOctave`; `octave`, `octaveLabel`: the note A
  plays, C3 to start with). Notes play at velocity 100.
- It is an application event filter on Qt Gui's key events, so it sees the keys wherever they go. The keys reach it
  before the window's shortcuts (S, A and Z do other things while it is off: it accepts their `ShortcutOverride`, so
  they come as key presses), but not while typing in a text input (the focus object answers `Qt::ImEnabled`), nor with
  modifiers held. `takesKey(key)` tells the UI which keys it takes.
- Notes still held when it is turned off, or when the application loses the keyboard, are released.
- Its notes go to `EngineBridge::sendMidi` (dropped while no audio device runs).

## Preferences

**Preferences › Audio** is `AudioPreferences` (`Session.audioPreferences`,
[AudioPreferences.h](../../app/src/session/AudioPreferences.h)): the driver type, the device, its outputs (ASIO), sample
rate, buffer size, exclusive mode (WASAPI), the audio threads, the driver's own settings (*Hardware Setup*), and a
status line. Changes apply at once: what a device offers (its sample rates, buffer sizes and channels) is only known
while it is open. Each change opens the device with the new settings (another driver or device: falling back to the
device's own settings if it won't take them), and only settings that opened are saved (`AudioSettings`, so the program
opens the same next time).

The QML dialog calls `open()` when it shows and `close()` when it goes. Each combo box binds to a list of choices
(`[{label, value, enabled, toolTip}]`: `driverChoices`, `deviceChoices`, `outputChoices`, `sampleRateChoices`,
`bufferChoices`, `threadChoices`) and its current index (`driverIndex`...), and calls the matching `choose...(index)`
with the index the user picked (the same index again does nothing). A driver this build hasn't (ASIO without the SDK)
is listed disabled, saying why. `showControlPanel()` opens the ASIO driver's own settings (it may run a message loop:
the UI disables the dialog meanwhile). `status` is rich text: what runs, an error first in red.

**Preferences › MIDI** is `MidiPreferences` (`Session.midiPreferences`): the MIDI inputs connected
(`inputs: [{name, enabled, error}]`), each on (tracks can hear and record it) or off (`setInputEnabled`, saved);
`refresh()` finds inputs plugged in or taken out since; `status`.

**Preferences › Plug-ins** is the plug-in index itself (`Session.plugins`): the standard and the user's folders, adding
and removing folders, rescanning ([plugin-scanner.md](plugin-scanner.md#in-the-application)).

## Settings

Besides the audio and MIDI preferences ([engine-bridge.md](engine-bridge.md#settings-storage)), the session and its
parts keep these QSettings keys:

| Key | Holds | Code |
|---|---|---|
| `files/recent` | the recent projects, the latest first (at most 10) | `Session::kRecentKey` |
| `files/last_dir` | where the file dialogs start (default `~/Music`) | `Session::kLastFolderKey` |
| `transport/count_in_bars` | the count-in (0, 1, 2 or 4 bars) | `Session::kCountInKey` |
| `presets/dir` | where *Load Preset…* last loaded a preset file from (default: the library) | `DeviceSelection::presetFolder` |
| `plugins/preset_dir` | where a plug-in's own `.vstpreset` was last loaded from or saved to (default: `Documents/VST3 Presets/<vendor>/<plug-in>`, if it exists) | `DeviceSelection::vst3PresetFolder` |

## Gotchas

- The session never shows a dialog: what were message boxes are `warning` and `information`, and questions (unsaved
  changes, replacing a preset, where to save) are the UI's, which then calls the session.
- `saveProject()` returns false for a project never saved: that isn't a failure, the UI is asking where
  (`saveAsRequested`).
- Renders hold the project as it was when they started; the UI's modal dialog keeps the user from editing meanwhile.
  Code that starts a render must go through `startRender` (one at a time).
- `Selection::trackId()` and the device view's ids may name things gone after an undo: look them up again
  (`Project::findTrack`, `findDevice`).

## Tests

- [test_selection.cpp](../../tests/app/test_selection.cpp): the selection's rules.
- [test_session_edit.cpp](../../tests/app/test_session_edit.cpp): the Edit and Create commands, the clipboard,
  reversing.
- [test_session_devices.cpp](../../tests/app/test_session_devices.cpp): the device view's selection, clipboard, racks,
  drops and presets.
- [test_session_files.cpp](../../tests/app/test_session_files.cpp): files, the title, the recent projects, Export
  Audio's choices, count-in and record quantization, the preferences.
- [test_session_renders.cpp](../../tests/app/test_session_renders.cpp): renders in the background, Cancel, closing
  meanwhile, freezing and flattening, plug-ins loading after a project opens.
- [test_session_engine.cpp](../../tests/app/test_session_engine.cpp): what the engine hears of the edits.
- [test_session_keyboard.cpp](../../tests/app/test_session_keyboard.cpp): the computer MIDI keyboard.
- The UI's tests ([testing.md](../testing.md#the-ui-test_ui_-on-a-display)) drive all of it through the QML.

Test fixture: [SessionFixture.h](../../tests/app/support/SessionFixture.h) ([testing.md](../testing.md#testsappsupport)).
