# UI overview

The UI is PySide6 (Qt 6) code in [src/substation/ui](../../src/substation/ui), plus the
look in [theme.py](../../src/substation/theme.py). Nearly everything that shows time
(the ruler, the lanes, the piano roll, envelopes, waveforms) is custom-painted with
`QPainter`; the rest is ordinary Qt widgets styled by one stylesheet. This page covers
the main window and the small parts every view shares; the big views have pages of
their own (see [the map](#the-other-ui-pages)).

For what the user sees and does, read the [user guide](../guide/README.md); this page is
about how the code does it.

## Files

| File | What it holds |
|---|---|
| [main_window.py](../../src/substation/ui/main_window.py) | `MainWindow`: builds the model, undo stack, editor, selection and bridge; lays out the panes; every menu action and shortcut; transport, recording, files, export, recent projects |
| [../theme.py](../../src/substation/theme.py) | colours (as module constants), `ui_font()`, `MONO_FONT`, the Fusion style, palette and `STYLESHEET`; `apply(app)` |
| [icons.py](../../src/substation/ui/icons.py) | vector icons drawn with `QPainter` at 64 px, cached (`functools.cache`), with a disabled variant |
| [widgets/](../../src/substation/ui/widgets) | `Knob`, `ValueBox`, `MeterWidget`, `Oscilloscope`, `ToggleButton` |
| [transport_bar.py](../../src/substation/ui/transport_bar.py) | `TransportBar`: tempo, time signature, metronome, project key, computer keyboard button, position, play/stop/record, count-in, Re-Enable Automation, Lock Envelopes, oscilloscope, loop, follow, CPU, device status |
| [dialogs.py](../../src/substation/ui/dialogs.py) | `PreferencesDialog` (Audio tab), `MidiPage`, `PluginsPage`, `ExportDialog`; `output_choices()` |
| [rendering.py](../../src/substation/ui/rendering.py) | `RenderProgress`: a render in the background's progress dialog, with Cancel; `active()` |
| [freezing.py](../../src/substation/ui/freezing.py) | freezing (in the background), unfreezing and flattening the selected tracks |
| [computer_keyboard.py](../../src/substation/ui/computer_keyboard.py) | `ComputerKeyboard`: letter keys as a MIDI input, through an application event filter |
| [plugin_keys.py](../../src/substation/ui/plugin_keys.py) | `PluginEditorShortcuts`: the main window's shortcuts while a plug-in's (Win32) editor has the focus |
| [arrangement/](../../src/substation/ui/arrangement) | the arrangement view: [arrangement.md](arrangement.md) |
| [piano_roll/](../../src/substation/ui/piano_roll) | the MIDI clip editor: [piano-roll.md](piano-roll.md) |
| [device_panel/](../../src/substation/ui/device_panel), [rack_view.py](../../src/substation/ui/rack_view.py), [device_editors/](../../src/substation/ui/device_editors), [clip_view.py](../../src/substation/ui/clip_view.py) | the device view and the clip view: [device-view.md](device-view.md) |
| [browser/](../../src/substation/ui/browser) | the browser panel: [browser.md](../browser.md) |

## How the UI talks to the rest

```
   user input
       │
       ▼
  widget (header, lanes, device view, ...)
       │ calls                         reads (never writes)
       ▼                                     │
  ProjectEditor (model/editor/)   ──► QUndoCommand ──► Project ──► Qt signals ──┐
                                                                                 │
       ┌───────────────────────── repaint / refresh ◄────────────────────────────┤
       │                                                                         ▼
  widgets                                             EngineBridge (audio/engine_bridge/)
       ▲                                                         │ substation._engine
       │  position_changed (16 ms), meters_updated (33 ms),       ▼
       └─ automation_state_changed, plugin_* signals, ...  ◄──  Engine
```

- **Edits go through the editor.** A widget never changes the `Project` itself: it calls
  a `ProjectEditor` method (`set_track_param`, `move_range`, `set_clip_notes`,
  `set_device_param`, ...). The editor pushes a `QUndoCommand`; the command mutates the
  project; the project emits a signal (`clips_changed`, `track_changed`,
  `devices_changed`, `automation_changed`, `automation_view_changed`, `settings_changed`,
  `reset`, ...); widgets repaint from the signal. See [python/model.md](../python/model.md)
  and the life of an edit in [architecture.md](../architecture.md#the-life-of-an-edit).
- **Continuous gestures are one undo step.** `Knob.valueChanged` and
  `ValueBox.valueChanged` carry `(value, gesture key)`: every value of one drag (or a
  run of wheel notches less than 0.6 s apart, in a `ValueBox`) shares a key object, and
  the widget passes it to the editor as `merge_key`. Gestures in the lanes and the piano
  roll do the same with their own key (often the gesture object itself).
- **View state skips undo.** Track heights, folding, which automation lanes show, the
  zoom and the scroll change the project (or `ViewState`) directly.
- **The engine is read through the bridge.** Widgets ask the `EngineBridge` for what
  only the engine knows: `position`, `is_playing`, `meters`, `chain_meters`,
  `live_takes`, decoded `source(path)` and `load_error(path)`, parameter metadata
  (`param_spec`, `param_groups`, `can_automate`), automation state (`is_automated`,
  `is_overridden`, `current_value`, `own_value`), plug-in editors
  (`open_plugin_editor`, `request_plugin_editor`, `show_plugin_editors`). Some views use
  `bridge.engine` directly for reads (`processor_params`, `processor_param`,
  `read_processor_display`, `master_scope`, `cpu_load`, `device_status`). The bridge
  polls the playhead every 16 ms and the meters every 33 ms and emits
  `position_changed` and `meters_updated`; the UI never waits on the audio thread. See
  [python/engine-bridge.md](../python/engine-bridge.md).
- **Some actions talk to the bridge, not the editor**: transport (`play`, `stop`,
  `locate`, `start_recording`), the metronome, previews (`preview_note`, `send_midi`),
  re-enabling automation, opening devices. They aren't project edits.
- **Status text.** The bridge, the browser, the arrangement and the device view each
  have a `status_message(str)` signal; `MainWindow.show_message` shows it in the status
  bar for 8 seconds.

## MainWindow

`MainWindow(engine)` is made by [app.py](../../src/substation/app.py) after
`theme.apply(app)`; `app.py` then shows it and calls `start_audio()` on the next turn of
the event loop.

The constructor owns the shared objects and hands them to the views:

| Object | Kind | Shared with |
|---|---|---|
| `project` | `Project` | everything (through the editor) |
| `undo_stack` | `QUndoStack` | the editor; Edit › Undo/Redo; the title's `*` |
| `editor` | `ProjectEditor` | every view |
| `selection` | `Selection` ([view_state.py](../../src/substation/ui/arrangement/view_state.py)) | arrangement, device view, piano roll |
| `bridge` | `EngineBridge` | every view |

Layout: the `TransportBar` on top; below it a horizontal splitter with the
`BrowserPanel` on the left and, on the right, a vertical splitter of the
`ArrangementView` over the `DevicePanel`. The window geometry and the outer splitter's
state are saved in `QSettings` (`window/geometry`, `window/splitter`).

Wiring done here:

- `selection.changed` → `bridge.show_plugin_editors(selection.track_id)`: only the
  selected track's plug-in editors show. `editor.plugin_added` opens a new plug-in's
  editor on the next event-loop turn (after the add and any drop finish).
- `bridge.plugin_param_edited` → `editor.set_device_param(..., merge_key=("plugin edit",
  device_id, param_id, gesture), old=...)`: knob drags in a plug-in's own editor become
  undo steps, one per gesture. `plugin_param_touched` → `editor.touch_parameter` (shows
  its automation). `plugin_state_dirty` → `undo_stack.resetClean()` (the project shows
  as changed).
- `editor.set_param_info(bridge.device_param_info)` and
  `editor.set_own_value(bridge.own_value)`: what the editor needs from the engine for
  macros.
- `bridge.takes_recorded` → `editor.add_recordings(takes, record_quantize())`, then
  the new clips are selected.

### Actions and the focus

Every shortcut is a `QAction` on the menu bar, made by `_action()`. Context menus show
the same shortcuts with `setShortcutVisibleInContextMenu`, only as a tip: the window's
action handles the key.

Several actions mean different things depending on what the user is working on. That
is `Selection.focus` (`"clips"`, `"track"`, `"devices"` or `"automation"`) plus what is
selected:

| Action | `focus == "devices"` | lane range with `lanes` | `points` | time range over tracks (clips and their automation) | `focus == "track"` |
|---|---|---|---|---|---|
| Delete | `devices.delete_selected()` | `editor.delete_automation_range` | `editor.delete_automation_points` | `lanes.delete_area()` | `delete_track()` |
| Cut / Copy | `devices.cut_selected()` / `copy_selected()` | `lanes.cut_automation()` / `copy_automation()` | refused, with a message | `lanes.cut_area()` / `copy_area()` | — |
| Paste | `devices.paste()` | `lanes.paste()` (the arrangement keeps one clipboard for clips and automation) | | | |
| Ctrl+D | `devices.duplicate_selected()` | `editor.duplicate_automation_range`, selects the copy | | `lanes.duplicate_area()` | `duplicate_tracks()` |
| Ctrl+G / Ctrl+Shift+G | `devices.group_selected()` / `ungroup_selected()` | | | | `editor.group_tracks` / `editor.ungroup` |
| R | | | | `lanes.reverse_selection()` (its audio clips) | |

The piano roll takes Delete, Ctrl+A, Ctrl+D and Ctrl+U before these actions fire (see
[piano-roll.md](piano-roll.md#keys)).

### Transport

- `toggle_play()` (Space): playing → `stop()` and `locate()` back to `_play_start`, as
  Ableton does; stopped → `_play_start = selection.insert_beat`, locate, `play()`.
- `stop_button()`: while playing, as above; stopped, locates to 0.
- `toggle_record()` (F9): while recording, `stop_recording()` (punch out, playing goes
  on). Otherwise records from the insert marker after the count-in
  (`transport.count_in_beats()`) when stopped, or from the playhead with no count-in
  while playing.
- `locate(beat)` sets the insert marker, `_play_start` and the engine's position; the
  rulers' `locate_requested` signals end up here.

### Files

`new_project`, `open_project`, `save_project`, `save_project_as` and `_save_to` use
[serialization](../python/serialization.md) (`load_project`, `save_project`). Before
saving, `bridge.store_plugin_states()` asks every plug-in for its state. Discarding
unsaved changes asks first (`_confirm_discard`, from `undo_stack.isClean()`).
`_reset_session()` stops, clears the undo stack, the selection and the insert marker.

Recent projects are a list in `QSettings` (`files/recent`, at most 10, compared with
`casefold()`); `files/last_dir` is the folder dialogs start in (the Music folder at
first). `QSettings` gives a one-item list back as a plain string, which
`recent_projects()` turns back into a list.

`export_audio()` shows the `ExportDialog` (range: the arrangement, or the loop if it is
on and not empty; 16-bit, 24-bit or 32-bit float), stops playback, and renders in the
background (`engine.start_export`) with its progress in a `RenderProgress` (see
[Renders in the background](#renders-in-the-background)): "Exported …", "Export cancelled",
or a message box saying why it failed.

Opening a project shows it at once: its plug-ins load after it
([engine-bridge.md](../python/engine-bridge.md#opening-a-project)), and the status bar's
right end says how far they got ("Loading plug-ins: 3 of 12", with a bar) until they all
have. The selected track's load first.

### Renders in the background

Exporting and freezing (Ctrl+Shift+F, the track menus: [freezing.py](../../src/substation/ui/freezing.py))
render on the engine's thread, a `RenderJob`
([engine/README.md](../engine/README.md#in-the-background)), while a `RenderProgress`
([rendering.py](../../src/substation/ui/rendering.py)) shows. It is application-modal: the
window goes on (it repaints, its meters and timers run, plug-ins still waiting to load go on
loading) but takes no edits, since the render is of the project as it was when it started.
Used as a context manager:

- `wait_for_devices(bridge)`: a busy bar ("Loading plug-ins (3 to go)…") until
  `bridge.devices_ready()`;
- `follow(job, label, (i, n))`: the bar at part `i` of `n` as the job goes, until it ends;
  the caller then finishes it (`job.finish()`, `bridge.finish_freeze()`): None is cancelled.
- Cancel (the button, Esc, the close button) only sets `cancelled`: the job is cancelled at
  the next poll (every 30 ms) and the dialog stays until its caller is done. The button takes
  no focus, so Space doesn't cancel.
- Freezing several tracks renders them one after another in the same dialog ("Freezing Bass
  (2 of 3)…"); cancelled or failed, the renders done are deleted (`discard_freeze`) and
  nothing is frozen (no undo step).
- Closing the main window while a render runs cancels the render instead
  (`rendering.active()`); the window stays.
- Each poll runs a local `QEventLoop` (`_spin`), so the callers read as straight code.

### Audio start-up

`start_audio()` applies the audio thread count, opens the MIDI inputs and opens the
saved device (`AudioSettings.load()`). If that fails it tries the device's own
settings (rate, buffer and channels left to the driver), then the system default
output, and says which in the status bar. See
[python/engine-bridge.md](../python/engine-bridge.md).

## Theme

[theme.py](../../src/substation/theme.py) sits beside `ui/`, not in it.
`theme.apply(app)` sets the Fusion style, `ui_font()` (Segoe UI, 9 pt), a dark palette
and `STYLESHEET`.

- Colours are module constants (`WINDOW`, `PANEL`, `SURFACE`, `ACCENT`, `LANE`,
  `GRID_BAR`, `PLAYHEAD`, `KEY_WHITE`, `METER_LOW`, `SCOPE_LINE`, ...), hex strings or
  `QColor`s with alpha. Painting code uses them directly (`QColor(theme.LANE)`).
- Buttons are coloured by a dynamic property, `role`: the stylesheet has rules for
  `QPushButton[role="activator"]`, `"solo"`, `"play"`, `"record"`, `"arm"`,
  `"re-enable"`, `"tool"`, `"flat"`, `"small"` and `"device-header"`. `ToggleButton`
  takes `role=` in its constructor.
- A stylesheet is slow to apply. Code that changes a widget's colour often checks
  first whether it changed (`SendControls.refresh` keeps the colour in a
  `sendColor` property).

## Icons

[icons.py](../../src/substation/ui/icons.py) draws each icon on a 64 px transparent
pixmap with antialiasing, once in its colour and once in `TEXT_DISABLED` for the
disabled mode, so icons stay sharp at any scale. Each function is `@cache`d by its
arguments. `lock_envelopes()` has On and Off states (a closed and an open padlock);
`fold(folded)` points right while folded, down while open. Add an icon by writing a
`draw(p, colour)` function and returning `_icon(draw, colour)`.

## Widgets

| Widget | File | Notes |
|---|---|---|
| `Knob` | [knob.py](../../src/substation/ui/widgets/knob.py) | Drag vertically (150 px for the whole range, 1000 px with Shift), wheel (1/50 of the range a notch, unless `wheel=False`), double-click resets to `default`. `log_scale` moves evenly in log(value) (if `minimum > 0`); `step` snaps to multiples from the minimum; `bipolar` draws the arc from the middle. With a `parser`, typing a digit opens a small line edit over it. `set_automation(None/"on"/"off")` draws the dot (red automated, grey overridden): `draw_automation_dot()` is shared with `ValueBox`. `relative` says whether the last user change was a drag or wheel (rather than typed or reset). |
| `ValueBox` | [value_box.py](../../src/substation/ui/widgets/value_box.py) | Ableton-style number: drag vertically (`step` per pixel, a tenth with Shift), wheel, double-click to type (or, with `default`, to reset; then typing a digit edits). `choices` limits it to a list (the time signature's denominator). `_parse_float` strips `dB`, `bpm`, `%`, and reads `-inf` as -70. Also has `relative` and the automation dot. |
| `MeterWidget` | [meter.py](../../src/substation/ui/widgets/meter.py) | Stereo peak meter from -60 to +6 dB, falling 3.5 % of the scale per update (updates come at about 30 Hz); a clip light at full scale, cleared by a click. |
| `Oscilloscope` | [oscilloscope.py](../../src/substation/ui/widgets/oscilloscope.py) | See below. |
| `ToggleButton` | [toggle_button.py](../../src/substation/ui/widgets/toggle_button.py) | Checkable `QPushButton` that never takes keyboard focus, so Space stays play/stop. `set_checked_silently()` changes it without emitting `toggled`, for showing state that came from the model or the engine. |

### Oscilloscope

The scope beside the transport shows the master output, as FL Studio's does. Every
33 ms its `QTimer` reads `engine.master_scope_written` (a counter); only when it moved
does it fetch `engine.master_scope(2 * WINDOW)` (both never block). `_trigger()` finds
the last rising zero crossing that still leaves a full `WINDOW` (1024 samples, about
21 ms at 48 kHz) after it, so steady tones stand still; with none, it shows the
newest window. `_trace()` turns the samples into one column per pixel (the column's
highest then lowest sample) with numpy, building the `QPolygonF` only when the samples
change, not on every paint. When no new audio comes, the trace shrinks by 0.64 each
update and stops repainting once flat. It skips its work while hidden.

`WINDOW` is a fixed number of samples because the engine's sample rate is behind its
edit lock, which the scope shouldn't take 30 times a second. The glow is drawn without
antialiasing (it is soft anyway); only the thin line has it.

## Transport bar

[transport_bar.py](../../src/substation/ui/transport_bar.py), left to right:

- **Tempo** (`ValueBox`, 20 to 999 BPM) → `editor.set_tempo(v, key)`; **time
  signature** (two `ValueBox`es; the denominator from `VALID_DENOMINATORS`) →
  `editor.set_time_signature`.
- **Metronome** → `bridge.set_metronome`. **Project key** (a combo of "No Key" and
  `ALL_KEYS`) → `editor.set_key`: audio added with a key in its name is transposed to
  it (see [guide/audio-clips.md](../guide/audio-clips.md)).
- **⌨** (`computer_keys`): toggles the `ComputerKeyboard`; the main window keeps it, the
  menu action and the keyboard's state in step.
- **Position** (bar. beat. sixteenth, from `split_position`), updated from
  `bridge.position_changed` only when the text changes.
- **Play**, **Stop**, **Record**: they emit `play_requested`, `stop_requested`,
  `record_requested`; the main window acts. Play's and Record's checked state follows
  the bridge (`transport_changed`, `recording_changed`), never the click itself.
- **Count-in** (none, 1, 2 or 4 bars): kept in `QSettings` under
  `transport/count_in_bars` (`count_in_bars()` reads it, falling back to none).
  `count_in_beats()` converts bars to beats in the project's time signature.
- **Re-Enable Automation**: enabled and lit while `bridge.has_overrides`
  (`automation_state_changed`).
- **Lock Envelopes** → `editor.set_automation_locked` (saved with the project).
- **Oscilloscope**, **Loop** (→ `editor.set_loop_enabled`), **Follow** (sets
  `ViewState.follow` on the arrangement's view).
- **CPU**: `engine.cpu_load`, refreshed every 15th `meters_updated` (about twice a
  second). **Device**: name and rate from `engine.device_status`; click it to open
  Preferences.

`refresh()` shows the project's settings on `settings_changed` and `reset`.

## Dialogs

[dialogs.py](../../src/substation/ui/dialogs.py). What the preferences mean to the user:
[guide/audio-setup.md](../guide/audio-setup.md).

### PreferencesDialog

Changes apply at once: what a device offers (rates, buffer sizes, channels, a control
panel) is only known while it is open, so each change reopens the device
(`bridge.open_device`) and the lists are filled from what it reports
(`engine.device_capabilities`).

- `_current_settings()`: the saved `AudioSettings`, unless another kind of device runs
  (the saved one didn't open), or the saved driver type isn't in this build.
- `_refresh(error)`: fills every combo inside `_quiet(...)` (a context manager that
  blocks the widgets' signals, so filling them applies nothing). With the device
  running it lists the driver's own rates and buffer sizes; otherwise the defaults
  (`SAMPLE_RATES`, `BUFFER_SIZES`). ASIO shows *Output Channels*
  (`output_choices()`: each stereo pair, and an odd last output alone, in mono) and
  *Hardware Setup*; WASAPI shows *Exclusive mode*. A driver offering one buffer size
  greys the list out. The status line shows the running device and its latencies (input
  and output for ASIO).
- `_driver_chosen`, `_device_chosen`, `_setting_chosen` build new settings and call
  `_open(settings, fall_back)`: with `fall_back`, a device that won't take them opens
  with its own settings instead. Settings are saved only when the device opens.
- ASIO is greyed out in the driver list when `ge.driver_types()` lacks it (a build
  without the ASIO SDK), with a tooltip saying so.
- *Hardware Setup* (`_show_control_panel`) disables the dialog while the driver's panel
  runs, since it may run a message loop of its own.
- *Audio Threads* lists 1 to the number of cores; the default
  (`Engine.default_audio_threads()`) is saved as 0 (`set_audio_threads`), then
  `bridge.apply_audio_threads()`.
- `bridge.device_changed` refreshes the dialog (a driver that reset, a device that
  went).

### MidiPage

A checkable list of `bridge.midi_inputs()`; unchecking one calls
`bridge.set_midi_input_enabled` (the disabled ones are kept by
`disabled_midi_inputs()`). Inputs that failed to open are red with the reason as their
tooltip (`bridge.midi_errors`). *Refresh* calls `bridge.open_midi_inputs()` again.

### PluginsPage

The standard VST3 folders (dimmed, not removable) and the custom ones
(`custom_folders()`, red if missing). Adding or removing a folder saves the list and
calls `PluginIndex.scan()` (only new files are read; a removed folder's plug-ins
leave). *Rescan Plug-ins* calls `scan(rescan=True)`. The status shows the count and the
files that failed (their reasons in the tooltip, at most 30). See
[python/plugin-scanner.md](../python/plugin-scanner.md).

## Computer MIDI keyboard

[computer_keyboard.py](../../src/substation/ui/computer_keyboard.py). Behaviour:
[guide/midi.md](../guide/midi.md).

`ComputerKeyboard` installs itself as an event filter on the whole application, so it
sees keys before any widget or shortcut does.

- `NOTE_KEYS` maps A W S E D F T G Y H U J K O L P ; ' to semitones 0 to 17 above the
  octave's C; `OCTAVE_KEYS` maps Z and X to -1 and +1. `DEFAULT_OCTAVE` 5 puts C3
  (note 60) on A; `MAX_OCTAVE` is 9.
- A note is `bridge.send_midi([0x90, note, 100], COMPUTER_KEYBOARD)`: it arrives as one
  more MIDI input, "Computer Keyboard", which tracks on *All Ins* hear too. `_held` maps
  each key to the note it started, so a note ends correctly even if the octave changed
  while it was held.
- While it is on, a `ShortcutOverride` for one of its keys is accepted, so Qt delivers
  the key as a key press instead of firing the window's shortcut (S, A, Z). Keys with
  modifiers (other than the keypad flag) and keys typed into text inputs
  (`TEXT_INPUTS`: line edits, text edits, spin boxes, combos) are left alone.
- A key event reaches an application event filter once per widget it is delivered to;
  it is taken the first time (and auto-repeats are ignored). Key-ups always go through
  for held keys, even if a modifier was pressed meanwhile.
- Turning it off, or the application losing the keyboard
  (`applicationStateChanged`), releases every held note.
- `takes_key()` tells [plugin_keys](#shortcuts-from-plug-in-editors) which keys it
  plays.

## Shortcuts from plug-in editors

[plugin_keys.py](../../src/substation/ui/plugin_keys.py). The rules as the user sees them:
[guide/shortcuts.md](../guide/shortcuts.md).

Plug-in editors are plain Win32 windows (`EditorWindow.cpp`, window class
`SUBstationPluginEditor`; see [engine/plugins.md](../engine/plugins.md)), so Qt never
sees their keys as key events. Their messages still pass through Qt's event loop, so
`PluginEditorShortcuts`, a `QAbstractNativeEventFilter` installed on the application
(Windows only: `supported()`), sees each `WM_KEYDOWN` / `WM_SYSKEYDOWN`:

1. Is the window (or its root ancestor) a plug-in editor (`is_plugin_editor`)? If not,
   Qt handles it as usual.
2. `action_for(vk, modifiers, text_field)` maps the virtual-key code to a Qt key
   (`qt_key`: letters, digits, F-keys and a few others), reads the modifiers with
   `GetKeyState`, and decides:
   - Without Ctrl or Alt, only `DAW_KEYS` (Space and S) are taken, and not with Shift,
     not while the focus is in a Windows text field (an `Edit` or `RichEdit` class:
     `is_text_field`), and not a key the computer keyboard plays while it is on.
   - `KEEP_FOR_PLUGIN` (Ctrl+A/C/V/X/Z/Y, Ctrl+Shift+Z) always stay with the plug-in:
     it may be typing into a field of its own.
   - Otherwise the first enabled `QAction` among the window's children whose shortcuts
     match exactly.
3. If there is an action, the message is swallowed and the action triggered, except
   for auto-repeats (lParam bit 30) of keys without Ctrl or Alt: Space held down acts
   once.

`close_foremost_editor()` (the main window's Ctrl+W, *View › Close Plug-in Editor*)
walks the top-level windows with `EnumWindows`, top down, and posts `WM_CLOSE` to the
first visible editor of this process.

## Gotchas

- **Never let a button take the focus.** Space is the window's play/stop shortcut; a
  focused `QPushButton` would take it. Use `ToggleButton`, or `setFocusPolicy(NoFocus)`.
- **Show model state silently.** Updating a control from the model with `setChecked` or
  `setValue` must not emit the signal that edits the model: use
  `set_checked_silently`, `blockSignals`, or `_quiet()`. Knobs and value boxes only
  emit `valueChanged` for user changes, never from `setValue`.
- **Hide before `deleteLater()`.** A widget waiting to be deleted is still painted
  where it was; the views call `hide()` first.
- **Parent before showing.** A widget shown before it has a parent becomes a window of
  its own for a moment; add it to its layout first.
- **Connect to methods, not lambdas, on widgets that can go before the signal's
  sender.** A return track's header and lane are deleted while the bridge's signals
  live on; they connect bound methods so Qt drops the connection with the object.
- **Native event filters see raw messages.** Anything in `plugin_keys` must stay cheap:
  it runs for every message of the process.

## Tests

The UI tests build the real `MainWindow` offscreen (`QT_QPA_PLATFORM=offscreen`, see
[testing.md](../testing.md)) and drive it with `QTest`: mouse drags, drops, header
controls, dialogs, the piano roll, devices' own editors and automation lanes.

| Test file | Covers here |
|---|---|
| [test_ui_smoke.py](../../tests/test_ui_smoke.py) | the window mirrored into the engine, edit commands, transport and locate, zoom, scroll and follow, save/open, `test_header_controls_and_dialogs` (Preferences, Export), `test_audio_threads_preference`, `test_shortcuts_from_plugin_editor` (`plugin_keys.action_for`), `test_open_recent` |
| [test_ui_rendering.py](../../tests/test_ui_rendering.py) | exporting and freezing in the background (the dialog, Cancel, closing the window meanwhile), a project's plug-ins loading after it opens (the selected track's first, moved or deleted meanwhile, renders waiting for them, saving meanwhile) |
| [test_computer_keyboard.py](../../tests/test_computer_keyboard.py) | M, notes and octaves, text fields and modifiers keeping their keys, recording from it |
| [test_ui_plugins.py](../../tests/test_ui_plugins.py) | Ctrl+W closing editors, edits in an editor becoming undo steps, editors following the selected track, the Plug-ins page of Preferences |
| [test_ui_recording.py](../../tests/test_ui_recording.py) | the record button, count-in, the MIDI page of Preferences |

## The other UI pages

| Page | Covers |
|---|---|
| [arrangement.md](arrangement.md) | `ui/arrangement/`: `ViewState`, `Selection` and `TrackLayout`, the grid, the ruler, the lanes canvas and its gestures, track/return/master headers, automation lanes and choosers, waveform tiles, folding |
| [piano-roll.md](piano-roll.md) | `ui/piano_roll/`: the keys, ruler, note grid, velocity lane and note tools |
| [device-view.md](device-view.md) | `device_panel/`, `rack_view.py`, `device_editors/`, and the clip view (`clip_view.py`) |
| [../browser.md](../browser.md) | `ui/browser/` and its native backend |
