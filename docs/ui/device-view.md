# Device view and clip view

The device view is the bottom panel: the selected track's (or a return's, or the
master's) chain of devices, built-in and plug-ins alike, with racks showing their
macros and chains. The clip view is the overlay that a double-clicked clip opens over
the arrangement: clip controls and waveforms for audio, the piano roll for MIDI. The
code is [device_panel/](../../src/substation/ui/device_panel),
[rack_view.py](../../src/substation/ui/rack_view.py),
[device_editors/](../../src/substation/ui/device_editors) and
[clip_view.py](../../src/substation/ui/clip_view.py).

What the user does with them: [guide/devices.md](../guide/devices.md),
[guide/plugins.md](../guide/plugins.md), [guide/mixing.md](../guide/mixing.md)
(sidechains), [guide/audio-clips.md](../guide/audio-clips.md). This page is about the
code.

## Files

| File | What it holds |
|---|---|
| [device_panel/panel.py](../../src/substation/ui/device_panel/panel.py) | `DevicePanel` (the chain: building it, selecting, the clipboard, drag and drop, scrolling); `_ChainView`; `preset_folder()` |
| [device_panel/frame.py](../../src/substation/ui/device_panel/frame.py) | `_DeviceFrame` (what every device shares: frame, title bar, parameter pages, sidechain button, menu, presets, folding); `device_height()`; the sizes (`DEVICE_WIDTH`, `PARAM_WIDTH`, `FOLDED_WIDTH`...) |
| [device_panel/device_widgets.py](../../src/substation/ui/device_panel/device_widgets.py) | `DeviceWidget` (built-in), `PluginDeviceWidget`, `RackWidget` |
| [rack_view.py](../../src/substation/ui/rack_view.py) | `MacroPanel` (a rack's eight macros), `ChainList` and `_ChainRow` (its chains with their mixers) |
| [device_editors/\_\_init\_\_.py](../../src/substation/ui/device_editors/__init__.py) | the editor registry: `@device_editor(kind)`, `editor_for(kind)` |
| [device_editors/compressor.py](../../src/substation/ui/device_editors/compressor.py) | `CompressorWidget` and `ReductionGraph` |
| [device_editors/delay.py](../../src/substation/ui/device_editors/delay.py) | `DelayWidget`, `FilterGraph`, `LogValueBox`, `filter_response()` |
| [device_editors/eq.py](../../src/substation/ui/device_editors/eq.py) | `EqWidget`, `EqEditor`, `EqGraph`, `BandPanel`, `Analyzer`, `EqWindow` |
| [device_editors/sidechain.py](../../src/substation/ui/device_editors/sidechain.py) | `SidechainWidget`, `CurveGraph`, `ClashView`, `Point`, `curve_value()`, `points_values()`, `SHAPES` |
| [device_editors/sampler.py](../../src/substation/ui/device_editors/sampler.py) | `SamplerWidget`, `SampleView`, `waveform_columns()` |
| [clip_view.py](../../src/substation/ui/clip_view.py) | `ClipView` (the overlay), `ClipWaveform`, `KnobControl` |

## DevicePanel

`DevicePanel(editor, selection, bridge)` shows the chain of `selection.track_id`:

```
QScrollArea (horizontal only)
└─ chain_layout: [device][device][rack][_ChainView: [device][rack][_ChainView: ...]][device] hint stretch
```

### Building the chain

`show_track(track_id)` throws every widget away and rebuilds: `_add_devices(layout,
track_id, devices)` makes a widget per device, choosing its class:

```
device.is_rack     → RackWidget
device.is_plugin   → PluginDeviceWidget
else               → editor_for(device.kind) or DeviceWidget
```

and connects its signals (`pressed`, `released`, `drag_started`, `menu_requested`,
`page_changed`) and its callbacks (`remove_selected`, `toggle_fold`, `clipboard_menu`,
`group_selected`), so a device's menu acts on all the selected devices. After an
unfolded rack with chains it adds a `_ChainView` (an accent-bracketed frame) holding
the devices of the chain the rack shows, recursively, so racks in it show theirs
further along. Which chain a rack shows is view state in `_shown_chains` (rack id →
chain id: the last clicked, else the first); `_pages` remembers each device's parameter
page across rebuilds. `widgets` holds every device widget shown, racks' contents
included; `_chain_ids()` is every device on the track depth first.

A hint label says what can be dropped (an instrument on a MIDI track without one,
otherwise effects) or "No track selected".

`_on_devices_changed(track_id)` (from `project.devices_changed`) avoids the rebuild
when the same device objects are in the same places (`widget.source is device`: a
device switched on or off, a macro mapped) and only refreshes them. Otherwise it
rebuilds, and scrolls to a device that was added, on the next event-loop turn (after
the layout), unless the device was dropped on the chain, where it is already in view
(`_dropping`). Other signals update widgets in place: `device_param_changed`,
`device_state_changed`, `chain_changed`, `track_changed` (a sidechain's source
renamed), `devices_folded` (rebuild), and from the bridge `plugin_params_changed`,
`plugin_params_rebuilt` (rebuild), `plugin_editor_changed`, `devices_loaded` (rebuild:
processors were recreated), `automation_state_changed`, `position_changed` (widgets
that `follows_automation()` refresh), `meters_updated` (`refresh_displays()`).

### Height

The panel never scrolls vertically. `device_height(editor)` builds a probe,
`_TallestDevice` (a page of knobs with page arrows: as tall as any device gets), asks
its minimum size, and deletes it; the panel's fixed height is that plus `EXTRA_HEIGHT`
(12 px, room for editors' graphs), its margins and the horizontal scroll bar. That way it fits whatever the fonts and the screen's scale.
A plug-in's error message is cut to `MESSAGE_LINES` (4) lines for the same reason
(the whole text is in its tooltip).

### Selecting

`selected` is a list of device ids in chain order, all of one chain (the track's own or
a rack's). `select_device(device_id, modifiers)`: Shift selects the range from
`_anchor` in that chain, Ctrl toggles, a plain click selects just it. A plain press on
a device already selected keeps the others (to drag them all) and the release selects
just it if no drag followed. Any selection calls `selection.focus_devices()`, so
Delete, Ctrl+C/X/V/D and Ctrl+G go to the device view (see
[README.md](README.md#actions-and-the-focus)). A click beside the devices clears the
device selection but keeps the focus here, so Ctrl+V pastes into this track. When the
`Selection` moves on (another track, or the focus elsewhere), the device selection is
dropped.

### Commands

| Method | Editor call | Notes |
|---|---|---|
| `delete_selected()` | `remove_devices` | one undo step |
| `group_selected()` | `group_devices` | the new rack is selected; refused past 8 levels of nesting |
| `ungroup_selected()` | `ungroup_rack` per rack, in one undo macro | a rack holding several instruments can't be ungrouped |
| `copy_selected()` | `copy_devices` | stores the plug-ins' states first (`bridge.store_plugin_states`), and remembers which copied devices were folded |
| `cut_selected()` | copy, then delete | |
| `paste(chain, index, after_selection)` | `paste_devices(track, clipboard, index, chain, folded)` | after the selected devices by default, else at the end; refused instruments and nesting limits are reported |
| `duplicate_selected()` | copy and paste | the clipboard is left as it was |
| `load_preset(chain, index)` | `insert_preset` of a file asked for | right-click beside the devices; folders remembered in `QSettings` `presets/dir` (else the preset library) |
| `insert_preset(path, chain, index)` | `load_preset` from [serialization](../python/serialization.md), then `insert_device` | a new device; a plug-in's editor opens, a rack's plug-ins' don't |
| `load_preset_into(device_id, path)` | `bridge.store_plugin_states({device})`, then `editor.load_preset_into` | only into a device of the preset's kind (`loads_into`) |
| `toggle_fold(device_id)` | `set_devices_folded` | all the selected if it is one of them; view state, not undone |

### Dragging and dropping

- **Starting a drag**: a frame emits `drag_started` once the mouse moves past the
  start-drag distance (never for an instrument, which stays first). `_start_drag` puts
  the track id and the selected device ids (instruments left out) in a `QMimeData`
  under `DEVICE_MOVE_MIME` (defined in
  [lanes_canvas.py](../../src/substation/ui/arrangement/lanes_canvas.py), which reads
  it too, for moving devices to another track), with a picture of the device.
- **Where it drops**: `drop_target(pos)` returns `(chain, index)`: a rack's chain row
  under the mouse (last in that chain), else the innermost `_ChainView` the mouse is in
  (`_ChainView.depth()`), else the track's own chain; the index counts the chain's
  widgets whose centres are left of the mouse. `_show_drop_marker` draws a 2 px line
  there (a frame over the chain, outside the layout).
- **Auto-scroll**: within `AUTOSCROLL_EDGE` (40 px) of either side, a 16 ms timer
  scrolls the chain, faster nearer the edge.
- **Dropping**: devices of this track → `editor.move_devices(track, ids, index,
  chain)`; built-in kinds and plug-in refs from the browser (`device_kinds`,
  `plugin_refs`) → `editor.add_device(..., index, plugin, chain)` one after the other,
  each after the last (an instrument goes first whatever the index, so the index is
  adjusted by how many devices went in before it). Presets (`preset_paths`) go in the
  same way (`insert_preset`), unless one preset is dropped onto a device of its kind:
  then it loads into that device (`load_preset_into`).
- **Onto a device**: `preset_target(pos, paths)` is the device under the mouse if a
  single preset is dragged and `loads_into` it (not on a rack's chain list row, which
  takes it as a new device in that chain). The presets are read once per drag
  (`_drag_presets`, cleared when it ends). While there is one, `load_marker` (a frame
  over the chain, transparent for the mouse) outlines it instead of the drop line.
- **Ctrl+Alt-drag** scrolls the chain by hand, as in the arrangement. The press usually
  lands on a knob or a device, so the panel installs itself as an application event
  filter and takes such presses on any widget inside its scroll area before they do.

## _DeviceFrame

The base of every device widget. Subclasses say how many parameters there are
(`_set_param_count(count, page)`) and build one parameter's cell
(`_param_widget(n)`); the frame does the rest:

- **Title bar** (`header_bar`, lighter while selected: `DEVICE_HEADER_SELECTED`): the
  fold button, the on/off switch (`editor.set_device_enabled`), the name
  (`_TitleLabel`, elided), the sidechain button (if any), the page arrows and page
  label (only with more than one page), the save button. Clicks on its background and
  name reach the frame (select, drag).
- **Save button** (and *Save Preset…* in the menu): `save_to_library(name=None)` stores
  the plug-in states in it, asks for a name (`QInputDialog`, the device's name to start
  with), asks before replacing a preset of that name (`QMessageBox`; not when `name` is
  passed), saves with `presets.save_to_library` and emits `preset_saved(path)`, which the
  panel passes on (`DevicePanel.preset_saved`) for the browser to list it. A rack is
  renamed after the preset (`editor.rename_rack`), so its title is the preset's name.
- **Default presets**: the menu's *Save as Default Preset* (`save_as_default`:
  `presets.save_default` after storing the plug-in's state) and *Clear Default Preset*
  (`clear_default`, enabled when there is one); not on racks.
- **Pages**: `params_per_page` (4) in a grid of `param_columns` (2), each cell
  `PARAM_WIDTH` (84 px). `set_page` rebuilds the page's widgets and emits
  `page_changed`. Device editors set other numbers.
- `_param_cell(name, param_id)`: a parameter's column with its name; right-click gives
  `_automation_menu` (Show Automation, Delete Automation, Re-Enable Automation and,
  inside a rack, *Map to Macro* / *Unmap from Macro N*: `editor.map_macro`,
  `editor.unmap_macro`, `editor.macro_of`). `_watch_touch` installs a `_TouchFilter`
  that calls `editor.touch_parameter` when the control is pressed, so the arrangement
  shows its automation.
- **Folding**: folded, the frame is `FOLDED_WIDTH` (26 px) and shows `folded_bar`
  instead: the fold button, the switch and the name reading upwards (`_VerticalTitle`,
  which paints the title label's text rotated). The fold and enable buttons are added
  to whichever bar shows. Double-clicking a folded device, or Ctrl+double-clicking any,
  calls `toggle_fold`; a plain double-click calls `open_editor()` (plug-ins show their
  editor).
- **Context menu**: device entries (`add_menu_actions`), Fold/Unfold, the clipboard
  entries from the panel, Move Left/Right (an instrument doesn't move, and nothing goes
  before it), Save Preset…, Save as Default Preset / Clear Default Preset (not racks),
  Group (Ctrl+G), Ungroup (racks), Delete.
- **Sidechain**: the button exists when `bridge.has_sidechain_input(track, device)`.
  `update_sidechain()` lights it and names the source and tap in its tooltip.
  `sidechain_menu()` lists *No Sidechain* and `project.sidechain_sources(track)`, those
  that would close a cycle disabled (`project.sidechain_would_cycle`), then, with a
  sidechain, where it is taken (`_tap_choices`: `PRE_FX`, *After* each of the source's
  effects by device id, `PRE_FADER` (*Post FX*), `POST_FADER` (*Post Mixer*); devices
  with the same name are numbered). `_tap_of()` shows a tap after a device that left
  the source as Post FX, and after an instrument as Pre FX, as the engine treats them.
  Choosing calls `editor.set_device_sidechain`; a `ValueError` (the source went
  meanwhile) becomes a status message. Engine side: [engine/routing.md](../engine/routing.md).
- Hooks the subclasses fill in: `refresh(device)`, `refresh_automation()`,
  `follows_automation()`, `refresh_displays()`, `open_editor()`,
  `add_menu_actions(menu)`.

### DeviceWidget (built-in devices)

Parameters come from the engine: `bridge.engine_device_id(track, device)` gives the
processor id, `engine.processor_params(id)` its `ParamInfo`s (id, name, range,
default, unit, `log_scale`, `value_labels`). A parameter with `value_labels` gets a
`QComboBox`; the others a `Knob` (log-scaled where the engine says so, bipolar when the
range spans 0 and the unit is none, st or ct) with a readout from
`params.format_value`. Values are read from the model (`device.params`) and written with
`editor.set_device_param(track, device, param, value, gesture)`.

`refresh_automation()` shows automated parameters at their envelope's current value
(`bridge.current_value`) with the red dot, or the model's value and a grey dot while
overridden. `read_display(display_id)` reads what the processor published since the
last call (`engine.read_processor_display(id, index, position)`, a float32 array),
keeping a read position per display. `refresh_state()` is called when the device's
state besides its parameters (`Device.state`) changes.

### PluginDeviceWidget

A plug-in's parameters live in the plug-in. The widget lists `engine.processor_params`
and shows those that are automatable and neither hidden nor read-only (or, if that
leaves none, every one that isn't hidden or read-only), a page of four at a time. Values
are `engine.processor_param(id, index)`, with the plug-in's own text
(`bridge.plugin_param_text`). Stepped parameters get `step=1`; a knob is bipolar when
its default is the middle of its range. Edits call `editor.set_device_param(...,
old=...)` with the value before, so undo can restore it.

- The **Edit** button (`plugin_window` icon) toggles `bridge.open_plugin_editor` /
  `close_plugin_editor`; its state follows `bridge.is_plugin_editor_open` on
  `plugin_editor_changed`.
- **VST3 presets**: *Load VST3 Preset…* (`load_vst3_preset`) reads a `.vstpreset`,
  applies it with `engine.set_processor_state` (which fails for another plug-in's),
  then records `editor.set_device_state(track, device, old, new, text)` with both states
  in base64, so it can be undone. *Save VST3 Preset…* (`save_vst3_preset`) writes
  `bridge.plugin_state`. The folder is remembered (`plugins/preset_dir`), else
  `Documents\VST3 Presets\<vendor>\<name>`. (The save button saves a SUBstation preset,
  as every device's does.)
- A plug-in that isn't loaded (missing, failed) shows `bridge.plugin_errors[device]` in
  its body and keeps its place.
- The title's tooltip has the name, vendor, path and the latency it reports.
- `refresh_values()` rereads every value (after a preset, or values the plug-in changed
  itself).

How plug-ins are hosted: [engine/plugins.md](../engine/plugins.md).

### RackWidget and rack_view

`RackWidget` (`RACK_WIDTH` 420) has no parameter pages; its content is a `MacroPanel`
and a `ChainList` from [rack_view.py](../../src/substation/ui/rack_view.py). Its save
button saves the rack, with everything in it, as every device's does. `chain_clicked` →
`DevicePanel._show_chain` rebuilds with that chain shown.

- `MacroPanel`: eight knobs (`MACRO_COUNT`), four to a row, 0..1, reading
  `rack.params[macro_param(i)]`; turning one calls `editor.set_macro(track, rack, i,
  value, key)`, which moves what it is mapped to as one undo step. The name lights up
  and the tooltip lists the mappings (`rack.macros`); right-click to unmap.
- `ChainList`: a `_ChainRow` per chain, an empty-rack hint, and **+ Chain**
  (`editor.add_rack_chain`). `refresh(rack)` rebuilds only when the chains changed.
  `chain_at(pos)` is used by the panel's drop target; `refresh_meters()` reads
  `bridge.chain_meters`.
- `_ChainRow`: activator, name (double-click to rename in place), solo, volume
  (`volume_box`) and pan (`pan_knob`) from
  [mixer_controls.py](../../src/substation/ui/arrangement/mixer_controls.py), meter.
  Edits go through `editor.set_chain_param(track, chain, attr, value, key)`. Chain
  volume and pan are automatable (`automation.chain_key(rack, chain, CHAIN_VOLUME /
  CHAIN_PAN)`) and follow their automation as rows do in headers. Right-click: rename,
  duplicate, delete, add, show automation; after the menu the row emits `clicked`
  (showing the chain rebuilds the panel, so it must come after the menu's action).

Racks in the engine: [engine/routing.md](../engine/routing.md).

## Device editors

A built-in device can have an editor of its own instead of the plain knob pages:
a `DeviceWidget` subclass in a module of
[device_editors/](../../src/substation/ui/device_editors), registered for the device's
kind (its engine id):

```python
@device_editor("compressor")
class CompressorWidget(DeviceWidget):
    params_per_page = 8
    param_columns = 4
    device_width = DEVICE_WIDTH + 2 * (PARAM_WIDTH + 16) + 12 + GRAPH_WIDTH
```

`editor_for(kind)` imports every module in the package the first time it is called (not
at import: the editors import the device panel, which imports the registry), so a new
module needs nothing else. Registering two editors for one kind raises.

An editor sets parameters as the knobs do (`editor.set_device_param`, or
`set_device_params` for several in one step), so undo, automation and saving work
alike. It can add widgets beside the parameters (`self.content.addWidget`); `content`
takes all the device's height (the view's, and the scroll bar's while that is hidden),
so an editor's graphs can grow into it, while the knobs stay at the top. It draws the
device's displays in `refresh_displays()`, which the panel calls as the meters update (about 30 times a second), from
`read_display()`. Displays are what the engine's built-in devices publish for their
editors (see [engine/devices.md](../engine/devices.md)).

- **Compressor** ([compressor.py](../../src/substation/ui/device_editors/compressor.py)):
  every knob on one page (8, in 4 columns) and a `ReductionGraph`: the gain reduction
  over the last `HISTORY` (240) values, growing downward to 24 dB, filled with a
  gradient; an *In* meter of what keys it with the threshold marked by an accent notch;
  an *Out* meter (both -60 to 0 dBFS); the current reduction and the threshold in
  figures. It reads the displays `reduction`, `input` and `output`.
- **Delay** ([delay.py](../../src/substation/ui/device_editors/delay.py)): no pages
  (`_set_param_count` only hides the arrows); its own layout, as Ableton's: per side a
  Sync button, then (a `QStackedWidget`) a grid of sixteenths and an offset field, or a
  time knob; the link button; a `FilterGraph` (the filter's response from
  `filter_response()` on a 20 Hz..20 kHz log axis, its dot dragged across for the
  frequency and up and down for the width, over a spectrum of the display `input`: a
  4096-point Hann FFT of the latest samples, falling 1 dB per refresh), the Filter
  button and the frequency (`LogValueBox`, dragging in log) and width fields; the Mode
  buttons and Ping Pong; Feedback with Freeze beside it over Dry/Wet. Each control is
  bound with `_bind(widget, param, update)`: `_sync()` shows every parameter as it is now
  (`value()`: the envelope's value while automation plays), right-click gives its
  automation menu, and pressing it touches the parameter.
- **EQ** ([eq.py](../../src/substation/ui/device_editors/eq.py)): no pages. `EqEditor`
  is the editor, for a host: the device's `EqWidget` (no body margins: the graph from the
  title bar to the bottom edge, the `BandPanel` beside it while `VIEW["panel"]`: it starts
  collapsed, the faders button beside the expand button shows it in every EQ, and
  `EqWidget.panel_shown` sets the device's width, `device_width` or `collapsed_width`),
  or an `EqWindow`
  (`open_window()`, from the expand button over the graph's top right: the graph over
  a bar with the panel; a top-level window per device in
  `_WINDOWS`, with its own display timer, that closes when the device goes). A host has
  `value(id)`, `set_params(values, gesture, text)`, `touch(id)`, `automation_state(id)`,
  `automation_menu(id, at)`, `sample_rate` and `read_display(id)`. `sync()` reads the
  bands into `Band`s; every change goes through `ProjectEditor.set_device_params`, so a
  drag (adding a band and dragging it on, too: the same parameters every move) is one
  undo step.
  - `EqGraph`: 10 Hz..22 kHz, ± `VIEW["range"]` dB; each band's curve and the total
    from `ge.eq_response` (cached per band), the selected and hovered bands filled in
    their colours (`band_color`), the total with a glow. Scale and Output are knobs over its bottom left and right
    corners (`_corner`, placed by `place_overlays()` as it resizes). The ghost shows within
    `CURVE_HIT` px of the curve, its type from `ZONES` (`type_at`). Dots ease between
    sizes on a 16 ms timer that stops when they settle. The wheel sets a band's Q, or
    while a cut is dragged its slope. Delete takes the ShortcutOverride while a band is
    selected. `VIEW` (range, analyzer mode) is shared by every editor and not saved.
  - `Analyzer`: an 8192-point Hann FFT of the displays `input` and `output`, rising
    0.55 and falling 0.09 of the way per refresh, mapped to columns (the loudest bin
    between columns, `np.maximum.reduceat`), smoothed and tilted 4.5 dB/octave (the tilt fading in over `TILT_FADE` dB above the
    floor, so a spectrum at or falling to the floor stays flat).
- **Sidechain** ([sidechain.py](../../src/substation/ui/device_editors/sidechain.py)): no
  pages; no margins (the graph from edge to edge). `CurveGraph` (the curve), then
  `ClashView` with Fit, Auto and the character, then the controls (Trigger and Sync,
  six small knobs; Lows Only over the crossover). The curve's points are always written
  whole (`points_values`: every slot's four parameters), through
  `ProjectEditor.set_device_params`, so a drag (adding a point and dragging it on, too)
  is one undo step, and Auto's fits merge into one while nothing else is done.
  - `refresh_displays()` reads the three displays itself (`_read`: with the absolute
    index of the first value) into a `sidechain_fit.Capture`, so the key, the input and
    the phase line up to the sample. The phase gives the playhead (`CurveGraph.tick`,
    a trail of the latest positions) and the hits; each hit's kick, once it has come,
    is analysed (`sidechain_fit.analyze`, over the latest `KEEP_KICKS`) into `fit`,
    which the graph (the kick's envelope, the dashed target) and `ClashView` (the
    spectra, the clash band) draw. Fit writes `fit_values(fit)`: the points, the
    length (sync off) and the crossover (1.5 times the clash band's top).
  - `CurveGraph`: points within `HIT_RADIUS`, else the curve within `CURVE_HIT` (a
    segment, to bend: up bulges up); elsewhere a press adds a point. The first and
    last points keep their x. The hint over the curve (no sidechain while triggered by
    it) opens the sidechain menu. Delete takes the ShortcutOverride while a point is
    selected.
- **Sampler** ([sampler.py](../../src/substation/ui/device_editors/sampler.py)): two
  pages (the sample's six parameters, then the amplitude's), three columns, and a
  `SampleView`: the sample's waveform from its peaks (`waveform_columns()`, cached per
  width), the part outside Start..End dimmed, the loop bracketed when it loops, and the
  newest note's position (display `position`). Drag the Start or End marker (within 5
  px) to set it (one gesture key, kept within the other marker); drop an audio file on
  it, or double-click it to browse. The sample path lives in the device's state
  (`device_state.from_model(device.state)["sample"]`); loading one is
  `editor.set_device_state(..., "Load Sample")`, undoable. The waveform comes from
  `bridge.source(path)` (requested with `bridge.request_source` if not decoded yet;
  quick, as the engine decoded it for the sampler). `value(param)` reads a parameter as
  it is now (its envelope's value while automation plays).

### Adding an editor

1. Create `device_editors/<name>.py` with a `DeviceWidget` subclass decorated with
   `@device_editor("<kind>")`.
2. Set `params_per_page`, `param_columns` and `device_width` as the layout needs.
3. Add widgets to `self.content`; read the model with `self.device()`, write with
   `self.editor.set_device_param(...)`; call `self.editor.touch_parameter(...)` when a
   control of yours is pressed.
4. Override `refresh_displays()` to read `self.read_display("<id>")`, and
   `refresh`/`refresh_automation`/`refresh_state` to repaint.
5. Add a case to [test_ui_device_editors.py](../../tests/test_ui_device_editors.py)
   (`test_registry` checks lookup).

## Clip view

[clip_view.py](../../src/substation/ui/clip_view.py). `ClipView` is a frame that
`ArrangementView` makes as a child covering itself, hidden until clips are opened
(double-click, Shift+Tab, or a new MIDI clip). Escape or × closes it (`close_view`,
emitting `closed`, which gives the lanes the focus back).

`open_clips(refs, lead)` orders the clips top track first, then by time. If the lead
clip (or the first) is a MIDI clip, only it opens, in the piano roll
([piano-roll.md](piano-roll.md)); otherwise every audio clip among `refs` opens
together. A `QStackedWidget` switches between `audio_page` and `piano_roll`. Deleted
clips or tracks are dropped (`_drop_missing`); with none left the view closes. A
project reset closes it.

### Audio clips

The left column (`CONTROLS_WIDTH` 260, scrollable) has three sections:

- **Warp**: the Warp switch, the warp mode (`WARP_MODES`, with a tooltip each; blank,
  "Mixed", when the clips differ), *Seg. BPM* (`ValueBox`, 20 to 999), and :2 / ×2.
- **Pitch**: Transpose (±48 semitones, whole numbers) and Detune (±50 cents). Disabled,
  with a note, when every clip is warped in Re-Pitch.
- **Mix**: clip gain (-70 to +24 dB; `ClipWaveform` draws each waveform scaled by it) and pan.

Every control edits all open clips through `editor.update_clips(refs, change, text,
merge_key)` with a function from clip to new clip:

- Switches, the mode and the BPM set the same value on all (`_set_all`).
- Turning Warp on sets a clip's `segment_bpm` to the project tempo if it was never set,
  so nothing moves until the tempo changes (`_set_warp`).
- :2 and ×2 scale each clip's own segment BPM (`_scale_bpm`).
- Knobs (`KnobControl`) sit on the first clip's value and show the range when clips
  differ. Turning one moves every clip by the lead's change (`_nudge_all`), measured
  from where the gesture started (`_baseline` keeps each clip's value at the gesture's
  first event), so a clip held at a limit keeps its offset to the others when the knob
  comes back.

The right side, `ClipWaveform`, draws each clip's whole source file fitted to the width
(with its own `WaveformCache` of 200 tiles), the part the clip plays tinted, the rest
dimmed, and S and E flags at its start and end. One clip gets a time ruler (steps from
`TIME_STEPS`, at least 70 px apart); several are stacked in bands of at least 40 px,
labelled, with "+N more" when they don't fit.

Warping and its modes in the engine: [engine/warp.md](../engine/warp.md).

## Gotchas

- **Rebuilds are wholesale.** `show_track` deletes every device widget; never keep a
  reference to one across a model change. Look widgets up in `DevicePanel.widgets` by
  device id. Keep view state (pages, the chain shown) in the panel, keyed by id.
- **Widgets go into a layout before they are shown**, or they flash up as windows of
  their own; and `hide()` comes before `deleteLater()`, or the old widget is painted
  where it was until deleted.
- **The model's value or the engine's.** Built-in devices read `device.params`;
  plug-ins read `engine.processor_param`, since a plug-in can change its own
  parameters. Both write through the editor.
- **A device's processor may be gone** before its widget is rebuilt: `read_display`
  catches the engine's `ValueError`, and widgets check `engine_id is not None`.
- **A chain row's menu rebuilds the panel**: anything after `menu.exec` must not use
  the row if its chain was deleted.
- **Saving stores plug-in states first.** `save_to_library` calls
  `bridge.store_plugin_states` for the device and everything in it, since a preset is
  the model's device: without it a plug-in would be saved as it was last stored.

## Tests

| Test file | Covers here |
|---|---|
| [test_ui_device_view.py](../../tests/test_ui_device_view.py) | folding devices (saved, not undone; the fold button; a folded rack hiding its chain), cut, copy, paste and duplicate, pasted instruments, switching a device off beside racks without a rebuild |
| [test_ui_device_editors.py](../../tests/test_ui_device_editors.py) | the registry; the Compressor's graph; the Sampler's loading, undo, playhead, markers, drop and saving |
| [test_ui_racks.py](../../tests/test_ui_racks.py) | Ctrl+G and Ctrl+Shift+G, macros and chains, the chain clicked shown beside the rack and dropped into, chain mixers, mapping to a macro, the view's height staying put |
| [test_ui_plugins.py](../../tests/test_ui_plugins.py) | knobs editing plug-ins undoably, parameter pages, automating plug-in parameters, devices fitting the device view, double-click and Ctrl+W, presets, dropping plug-ins, dragging a device to another track, selecting, deleting and reordering, auto-scroll and Ctrl+Alt-drag, scrolling to a new plug-in, the master's effects |
| [test_ui_presets.py](../../tests/test_ui_presets.py) | the save button on every kind of device (asking before replacing), presets listed in the browser, dropped between devices and onto a device of their kind (outlined; one undo step) or of another, double-clicked (an instrument making a MIDI track), dropped on a track, renamed and deleted |
| [test_ui_sidechain.py](../../tests/test_ui_sidechain.py) | the sidechain button and its menu: sources, cycles greyed out, taps |
| [test_ui_smoke.py](../../tests/test_ui_smoke.py) | devices and mixer reaching the engine, switching a device keeping the chain, the clip view editing several clips in unison, warping reaching the audio, double-click opening it |
