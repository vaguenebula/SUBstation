# Device view

The device view is the bottom panel: the selected track's (or a return's, or the master's) chain of devices, built-in
and plug-ins alike, with racks showing their macros and chains, and the built-in devices' own editors. It is QML in
[ui/qml/devices](../../ui/qml/devices) (the editors in [ui/qml/devices/editors](../../ui/qml/devices/editors)) over
small C++ objects and scene-graph items in [ui/src/devices](../../ui/src/devices). What the view selects and does
with its selection (the clipboard, grouping, presets, drops) is the application layer's
[DeviceSelection](../../app/src/session/DeviceSelection.h) (`Session.deviceSelection`); the signal maths of the
editors' analyzers is in [app/src/analysis](../../app/src/analysis) ([app/analysis.md](../app/analysis.md)). The clip
view, which used to share this page, is in [piano-roll.md](piano-roll.md#the-clip-view).

What the user does with it: [guide/devices.md](../guide/devices.md), [guide/plugins.md](../guide/plugins.md),
[guide/mixing.md](../guide/mixing.md) (sidechains). This page is about the code.

## Files

| File | What it holds |
|---|---|
| [DevicePanel.qml](../../ui/qml/devices/DevicePanel.qml) | The panel: the chain area, the hint, the drop markers, its height; the device, sidechain, chain, macro and "beside the devices" menus; the preset dialogs |
| [DeviceChainArea](../../ui/src/devices/DeviceChainArea.h) | The chain scrolled sideways; drops and drop targets; drags of devices; scrolling to a device added; the pages each device shows |
| [DeviceChain.qml](../../ui/qml/devices/DeviceChain.qml), [DeviceChainList](../../ui/src/devices/DeviceChainList.h) | One chain: a frame per device and, after a rack, the chain it shows; the device ids, changing only when the chain does |
| [DeviceFrame.qml](../../ui/qml/devices/DeviceFrame.qml), [DeviceInfo](../../ui/src/devices/DeviceInfo.h), [DeviceFrameInput](../../ui/src/devices/DeviceFrameInput.h) | What every device shares: the frame, the title bar, folding, the body; what the frame shows of the device; the frame's mouse |
| [DeviceKnobPages.qml](../../ui/qml/devices/DeviceKnobPages.qml), [DeviceParamKnob.qml](../../ui/qml/devices/DeviceParamKnob.qml), [ParamCell.qml](../../ui/qml/devices/ParamCell.qml), [DeviceParams](../../ui/src/devices/DeviceParams.h), [DeviceParam](../../ui/src/devices/DeviceParam.h), [ParamMenu.qml](../../ui/qml/devices/ParamMenu.qml) | A built-in device without an editor: a knob (or list) per parameter, four to a page; a parameter's cell (`ParamCell`, which `PluginParamKnob` shows a plug-in's parameter in too), its state and its menu |
| [PluginDeviceBody.qml](../../ui/qml/devices/PluginDeviceBody.qml), [PluginParamKnob.qml](../../ui/qml/devices/PluginParamKnob.qml), [PluginParams](../../ui/src/devices/PluginParams.h) | A plug-in's generic editor: its parameters (`PluginParams`, `PluginParam`), or why it shows none |
| [RackDeviceBody.qml](../../ui/qml/devices/RackDeviceBody.qml), [RackMacroKnob.qml](../../ui/qml/devices/RackMacroKnob.qml), [RackMacroMappings.qml](../../ui/qml/devices/RackMacroMappings.qml), [RackChainRow.qml](../../ui/qml/devices/RackChainRow.qml), [RackChainView.qml](../../ui/qml/devices/RackChainView.qml), [RackMacro](../../ui/src/devices/RackMacro.h) (`RackMacro`, `RackMacros`), [RackChain](../../ui/src/devices/RackChain.h), [RackChains](../../ui/src/devices/RackChains.h) | A rack: its buttons, macros and chain list, a macro's mappings and their ranges, a chain's row, the chain shown beside the rack |
| [DeviceCanvas](../../ui/src/devices/DeviceCanvas.h), [EditorPaint](../../ui/src/devices/EditorPaint.h) | The base of the editors' drawn items, and their drawing helpers (among them `LogAxis`, the graphs' logarithmic frequency and time axes, and `drawDecadeGrid()`) |
| [editors/DeviceEditors.qml](../../ui/qml/devices/editors/DeviceEditors.qml) | The editor registry (a singleton): `editorFor(kind)` |
| [editors/EditorCaption.qml](../../ui/qml/devices/editors/EditorCaption.qml), [editors/EditorReadout.qml](../../ui/qml/devices/editors/EditorReadout.qml) | The editors' captions (a control's name over it) and readouts (its value under it) |
| Compressor: [CompressorEditor.qml](../../ui/qml/devices/editors/CompressorEditor.qml), [ReductionGraph](../../ui/src/devices/ReductionGraph.h) | |
| Delay: [DelayEditor.qml](../../ui/qml/devices/editors/DelayEditor.qml), [FilterGraph](../../ui/src/devices/FilterGraph.h) | |
| Disperser: [DisperserEditor.qml](../../ui/qml/devices/editors/DisperserEditor.qml), [DispersionGraph](../../ui/src/devices/DispersionGraph.h) | |
| EQ: [EqEditor.qml](../../ui/qml/devices/editors/EqEditor.qml), [EqWindow.qml](../../ui/qml/devices/editors/EqWindow.qml), [EqWindows.qml](../../ui/qml/devices/editors/EqWindows.qml), [EqBandPanel.qml](../../ui/qml/devices/editors/EqBandPanel.qml), [EqCorner.qml](../../ui/qml/devices/editors/EqCorner.qml), [EqGraphMenus.qml](../../ui/qml/devices/editors/EqGraphMenus.qml), [EqGraph](../../ui/src/devices/EqGraph.h), [EqView](../../ui/src/devices/EqView.h), [EqTypeIcon](../../ui/src/devices/EqTypeIcon.h) | |
| Sidechain: [SidechainEditor.qml](../../ui/qml/devices/editors/SidechainEditor.qml), [CurveGraph](../../ui/src/devices/CurveGraph.h), [ClashView](../../ui/src/devices/ClashView.h) | |
| Sampler: [SamplerEditor.qml](../../ui/qml/devices/editors/SamplerEditor.qml), [SampleView](../../ui/src/devices/SampleView.h), the application layer's [SampleSlices.h](../../app/src/audio/SampleSlices.h) | |
| [PanelMenu.qml](../../ui/qml/devices/PanelMenu.qml), [DynamicMenu.qml](../../ui/qml/devices/DynamicMenu.qml), [ParamArea.qml](../../ui/qml/devices/ParamArea.qml), [ParamKnob.qml](../../ui/qml/devices/ParamKnob.qml), [ParamBox.qml](../../ui/qml/devices/ParamBox.qml), [ParamButton.qml](../../ui/qml/devices/ParamButton.qml), [ParamChoice.qml](../../ui/qml/devices/ParamChoice.qml), [DeviceHeaderButton.qml](../../ui/qml/devices/DeviceHeaderButton.qml) | Menus filled when they open; the editors' parameter-bound knobs, boxes, buttons and drop-downs; a title bar's buttons |

## The panel

[DevicePanel.qml](../../ui/qml/devices/DevicePanel.qml) shows the chain of `Session.deviceSelection.trackId` (the
selection's `trackId`: a track, a return or the master; none while nothing is selected):

```
DevicePanel (WINDOW, the info view at its left in the main window)
└─ DeviceChainArea (panelMargin, 8 px, all round; clips; scrolls sideways by contentX)
   └─ Row
      ├─ DeviceChain ("": the track's own)
      │    [DeviceFrame] [DeviceFrame] [DeviceFrame (rack)] [RackChainView: DeviceChain (its chain) ...] [DeviceFrame]
      └─ the hint
```

As in Ableton, the devices stand on the window's colour, 8 px apart (`DeviceChainArea::kSpacing`), with a grip in
each gap (`DeviceGrip`: two short dark lines upright in the middle, drawn by the frame before it); the info view at
the panel's left (main window) has one after it too.

`DeviceChain` is a `Row` over a `DeviceChainList` (`deviceIds` of one chain, changing only when the chain's devices
do, so the `Repeater` makes its frames again only then, not as devices are selected, switched or edited: the frames
follow those themselves). After a rack that isn't folded, has chains and shows its devices (`DeviceInfo::rackDevicesShown`),
a `RackChainView` (an accent-bracketed frame, "Drop devices here" while empty) holds a `DeviceChain` of the chain the
rack shows, recursively, so racks in it show theirs further along. Which chain a rack shows is `DeviceSelection::shownChain(rackId)` (the one last
clicked in its chain list, else its first). A frozen track (or one in a frozen group) shows no devices.

The hint (`DeviceSelection::hint`) says what can be dropped (an instrument on a MIDI track without one, otherwise
effects), that no track is selected, or that the track is frozen.

What the panel is to the main window: `startChainRename(rackId, chainId)` (Ctrl+R on a rack chain: its row's name
edited in place, after scrolling to the rack; the rack's chain list is shown first if it is hidden, and the rename
starts once its rows are made; false for a rack not shown or folded), `focusDevices()` (the device view takes the
focus), the `statusMessage(text)` signal, and its `implicitHeight`.

### Height

The panel never scrolls vertically. Its height is worked out once from a measured, hidden `DeviceParamKnob` (the
`probe`) and the title font: the tallest device is its border, a title bar and a page of knobs in two rows 14 px
apart with the body's margins, 6 px above and below (`deviceHeight`); the panel adds `panelMargin` (8 px) above and
below. Every
device is that tall, so the tallest page of knobs is as far from its title bar as from the frame's bottom, and an
editor's graphs take the height there is, with the same margins. (The Python panel also added `EXTRA_HEIGHT` and a
scroll bar's width, which left more room below a device's knobs than above them; the chain has no scroll bar.) The
main window fixes the device view's split to it. That way it fits whatever the fonts and the screen's scale. A plug-in's message is cut to four lines for the same reason (the whole text is in its
tooltip).

### Scrolling

[DeviceChainArea](../../ui/src/devices/DeviceChainArea.h) scrolls the chain sideways without a scroll bar
(`contentX`, `maxContentX`): Shift+wheel anywhere over the chain scrolls it (`kWheelScroll` 80 px a notch), and the
wheel never turns a knob or a value box here; Ctrl+Alt-drag anywhere on the chain scrolls it by hand, as in the
arrangement. The press usually lands on a device or a knob, so the area watches its window's mouse and wheel events
before they reach them (an event filter on the window). A device added (not by a drop on the chain, where it is
already in view) is scrolled to once laid out (`scrollTo()`, as `QScrollArea::ensureWidgetVisible`).

QML tells the area what its items are with a `panelRole` property: "device" (a frame: `deviceId`, and `chainId` the
chain it is in), "chain" (a `RackChainView`: `chainId`), "chainRow" (a chain list's row: `chainId`, `rackId`). The
area finds them by walking its content, so frames, chain views and rows need no registration.

The page each device shows is view state kept by id in the area (`pageOf`, `setPage`), across the frames' rebuilds.

## Selecting

The selection is `DeviceSelection`'s ([app/session.md](../app/session.md)): `selected` is a list of device ids in
chain order, all of one chain (the track's own or a rack's).

- `selectDevice(id, modifiers)`: Shift selects the range from the anchor in that chain, Ctrl toggles, a plain click
  selects just it.
- `press(id, modifiers)` and `release(id, modifiers)`: a plain press on a device already selected keeps the others
  (to drag them all), and the release selects just it if no drag followed.
- Selecting devices calls `Selection::focusDevices()`, so Delete, Ctrl+C/X/V/D and Ctrl+G go to the device view
  (see [README.md](README.md#what-an-action-acts-on)). A click beside the devices (`clickBeside()`) clears the device
  selection but keeps the focus here, so Ctrl+V pastes into this track. When the `Selection` moves on (another track,
  or the focus elsewhere), the device selection is dropped; devices no longer shown are no longer selected.
- A frame shows it is selected by its title bar and edge in the selection's teal (`kDeviceHeaderSelected`), as in
  Ableton.

## Commands

`DeviceSelection` acts on the selected devices through the editor:

| Method | Editor call | Notes |
|---|---|---|
| `deleteSelected()` | `removeDevices` | one undo step |
| `groupSelected()`, `groupDevice(id)` | `groupDevices` | the new rack is selected; refused past 8 levels of nesting |
| `ungroupSelected()`, `ungroupRack(id)` | `ungroupRack` per rack | a rack holding several instruments can't be ungrouped |
| `copySelected()` | `copyDevices` | stores the plug-ins' states first (`bridge.storePluginStates`), and remembers which copied devices were folded |
| `cutSelected()` | copy, then delete | |
| `paste()`, `pasteAt(chain, index)`, `pasteAfter(id)` | `pasteDevices(track, clipboard, index, chain, folded)` | after the selected devices by default, else at the end; refused instruments and nesting limits are reported |
| `duplicateSelected()` | copy and paste | the clipboard is left as it was |
| `toggleFold(id)` | `setDevicesFolded` | all the selected if it is one of them; view state, not undone |
| `loadPresetFile(path, chain, index)` | `insertPreset` of a file asked for | right-click beside the devices, *Load Preset…*; the folder remembered in `QSettings` `presets/dir` (else the preset library) |
| `insertPreset(path, chain, index)` | `loadPreset` ([app/serialization.md](../app/serialization.md)), then `insertDevice` | a new device; a plug-in's editor opens, a rack's plug-ins' don't |
| `loadPresetInto(id, path)` | `bridge.storePluginStates({device})`, then `editor.loadPresetInto` | only into a device of the preset's kind (`presetLoadsInto`) |
| `savePreset(id, name)` | `saveToLibrary` | stores the plug-in states of it and everything in it first; a rack is renamed after the preset (`editor.renameRack`), so its title is the preset's name; `presetSaved(path)` lets the browser list it |
| `saveAsDefault(id)`, `clearDefault(id)` | the preset library's default presets | not racks |
| `loadVst3Preset(id, path)`, `saveVst3Preset(id, path)` | `setDeviceState` with the states before and after (undoable) | a plug-in's own `.vstpreset`; the folder remembered (`plugins/preset_dir`), else `Documents/VST3 Presets/<vendor>/<name>` |

The device view's clipboard is its own: the arrangement's clips, automation and tracks have theirs.

## Dragging and dropping

- **Starting a drag**: a frame's `DeviceFrameInput` calls `DeviceChainArea::startDrag()` once the mouse moves past
  the start-drag distance (never for an instrument, which stays first). It drags `DeviceSelection::dragDevices(id)`
  (the selected devices if it is one of them, else it; instruments left out) under `kDeviceMoveMime`
  (`application/x-substation-device-move`: the track's id, then the device ids, a line each;
  [BrowserMime.h](../../app/src/browser/BrowserMime.h)), with a picture of the device (`kDragPictureHeight` 48 px).
  The arrangement reads the same format, to move devices to another track.
- **Where it drops**: `dropTarget(x, y)` returns `{chain, index}`: a rack's chain row under the mouse (last in that
  chain), else the innermost rack chain shown there, else the track's own chain; the index counts the chain's frames
  whose middles are left of the mouse. `dropMarker` is a 2 px line there.
- **Auto-scroll**: within `kAutoscrollEdge` (40 px) of either side, a 16 ms timer scrolls the chain, faster nearer the
  edge.
- **Dropping**: devices of this track → `DeviceSelection::dropMoved(ids, chain, index)` (`editor.moveDevices`);
  built-in kinds and plug-in refs from the browser → `dropDevices(kinds, plugins, chain, index)`, one after the other,
  each after the last (an instrument goes first whatever the index). Presets → `dropPresets(paths, chain, index,
  intoDeviceId)`: as new devices, unless one preset is dropped onto a device of its kind, which it then loads into.
- **Onto a device**: `presetTarget(x, y, paths)` is the device under the mouse if a single preset is dragged and
  loads into it (not on a rack's chain list row, which takes it as a new device in that chain). The presets are read
  once per drag (`presetLoadsInto`, forgotten by `dragEnded()`). While there is one, `loadMarker` outlines it instead
  of the drop line.

## DeviceFrame

[DeviceFrame.qml](../../ui/qml/devices/DeviceFrame.qml) is what every device shares, with `DeviceInfo` (what the frame
shows of the device: its name, tooltip, kind, on/off, folded, its chain, Move Left/Right, a plug-in's loading state
and editor, its sidechain) reading the project again whenever that may have changed:

- **Frame**: `kPanelAlt` in a 1 px line of its title bar's colour, 3 px corners. Its width is `DEVICE_WIDTH` (216 px),
  a rack's or an editor's own (`body.implicitWidth + 2`: a rack's grows with its macros and its chain list), or 26 px
  folded.
- **Title bar** (`kDeviceHeader`, teal while selected): the fold button, the on/off switch (round, as Ableton's:
  `DeviceInfo::setEnabled`, overriding the switch's automation while it plays; following that automation, with the
  automation dot: `enabled`, `enabledAutomation`; right-click: Show, Delete and Re-Enable Automation), the name
  (elided; its tooltip: a plug-in's name, vendor, file and latency, a rack's name
  and latency), a plug-in's editor button (`plugin_window` icon, lit while its editor shows), the sidechain button (a
  device with a sidechain input), the page arrows and "n/m" (only with more than one page), or, for an editor that
  names its pages (`pageNames`: the Sampler's *Sample* and *Controls*, as Simpler's), a tab per page instead
  (`pageTab_<name>`, lit while it shows), the save button.
- **Body**: a `Loader` taking all the height there is: a rack's `RackDeviceBody`, a plug-in's `PluginDeviceBody`,
  the device's editor (`DeviceEditors.editorFor(kind)`), or `DeviceKnobPages`. A body may have `pages` and `page`
  (the title bar pages through them; the page is restored from the area when the body is made again). Its content
  starts 6 px below the title bar and what fills the height (a graph, a rack's chain list) ends 6 px above the
  frame's bottom (the EQ's curve and the Sidechain's fill it all, 5 px for their side columns).
- **Folded**, the frame is a 26 px strip instead: the fold button, the switch and the name reading upwards.
- **Mouse** ([DeviceFrameInput](../../ui/src/devices/DeviceFrameInput.h), under the frame's controls, so it gets the
  clicks on the background, the title bar and labels): a press selects (`DeviceSelection::press`), the release
  `release`; a drag past the start distance starts a drag of the devices; a double-click folds or unfolds a folded
  device (or any, with Ctrl), else opens a plug-in's own editor; a right press selects it (unless it is) and asks
  the panel for its menu.
- **Context menu** (`DevicePanel.showDeviceMenu`): a plug-in's Show Editor, Load VST3 Preset…, Save VST3 Preset…; a
  rack's Add Chain (which shows its chain list), Show/Hide Chain List, Show/Hide Devices; the body's own `menuActions`
  (the Sampler's); Fold/Unfold; Cut, Copy, Paste (after it),
  Duplicate; Move Left/Right (not an instrument: nothing goes before it); Save Preset…, Save as Default Preset /
  Clear Default Preset (not racks); Group (Ctrl+G), Ungroup (racks); Delete.
- **Save button** (and *Save Preset…*): the panel asks for a name (the device's to start with,
  `presetName(id)`), checks it (`checkPresetName`: a name that can't be a file's is refused with a message; one that
  exists asks before replacing it), then `savePreset(id, name)`.
- **Sidechain**: the button exists when `bridge.hasSidechainInput(track, device)`; it is lit while there is one, and
  its tooltip names the source and the tap. Its menu (`DeviceInfo::sidechainMenu()`) lists *No Sidechain* and the
  tracks, groups and returns it can come from, those that would close a cycle disabled, then, with a sidechain, where
  it is taken (`tapChoices()`: Pre FX, *After* each of the source's effects along its signal, those in a rack's chains
  before the rack and named by it, "After Rack › Chain › Device" (the chain only if the rack has several), Post FX,
  Post Mixer; devices with the same name in a chain are numbered). `tapOf()` shows a tap after a device that left the
  source as Post FX, and after its instrument as Pre FX, as the engine treats them. Choosing calls `editor.trySetDeviceSidechain`, which reports a refusal (the
  source went meanwhile) as a status message. Engine side: [engine/routing.md](../engine/routing.md).

### Built-in devices

[DeviceKnobPages.qml](../../ui/qml/devices/DeviceKnobPages.qml): `DeviceParams` lists the processor's parameters
(`bridge.deviceParams`), four to a page in a 2 × 2 grid, each a
[DeviceParamKnob](../../ui/qml/devices/DeviceParamKnob.qml) (84 px wide, a 34 px knob) over a
[DeviceParam](../../ui/src/devices/DeviceParam.h). The cell itself is [ParamCell](../../ui/qml/devices/ParamCell.qml),
which shows what it is given and says what the user does; `PluginParamKnob` wraps it around a `PluginParam` the same
way:

- A parameter with named values gets a list; the others a `Knob`, log-scaled where the engine says so, bipolar when
  the range spans 0 and the unit is none, st or ct, in whole steps when stepped, with a readout from `format()`.
- `value` is the parameter as it is now: its envelope's value while automation plays (with the red dot; the model's
  value and a grey dot while overridden), refreshed as the playhead moves while it follows automation.
- `set(value, mergeKey)` → `editor.setDeviceParam(track, device, param, value, key)`; pressing the cell or the knob
  calls `touch()` (`editor.touchParameter`), so the arrangement shows its automation.
- Right-click: [ParamMenu](../../ui/qml/devices/ParamMenu.qml): Show Automation, Delete Automation, Re-Enable
  Automation (`DynamicMenu.automationEntries()`, as a macro's menu has them; `AutomationTarget` in C++) and, inside
  a rack, *Map to Macro* (the rack's macros by name: `DeviceParam::macroNames()`) / *Unmap from <macro>*
  (`editor.mapMacro`, `editor.unmapMacro`, `editor.macroOf`).
- A parameter mapped to an automated macro is automated as far as the cell knows (`bridge.isAutomated`: the red dot)
  and follows the macro's envelope over its range (`bridge.currentValue`).

### Plug-ins

A plug-in's parameters live in the plug-in. [PluginParams](../../ui/src/devices/PluginParams.h) lists those a
generic editor offers (automatable and neither hidden nor read-only, or, if that leaves none, every one that isn't
hidden or read-only), a page of four at a time ([PluginDeviceBody.qml](../../ui/qml/devices/PluginDeviceBody.qml));
each `PluginParam` reads its value as the plug-in has it now, with the plug-in's own text. Stepped parameters move in
whole steps; a knob is bipolar when its default is the middle of its range. Edits call `editor.setDeviceParam(...,
old)` with the value before, so undo can restore it.

- The editor button and Show Editor toggle the plug-in's own editor (`DeviceInfo::showEditor`, which brings an open
  one to the front); its state follows `bridge.isPluginEditorOpen` on `pluginEditorChanged`.
- A plug-in that isn't loaded shows why instead of its parameters (`DeviceInfo::pluginMessage`): loading (a project
  just opened), missing or failed (it keeps its place).
- Values are read again when the plug-in rebuilds its parameters, changes them itself, loads a preset, or an undo
  sets them.

How plug-ins are hosted: [engine/plugins.md](../engine/plugins.md).

### Racks

[RackDeviceBody.qml](../../ui/qml/devices/RackDeviceBody.qml) has no parameter pages: a strip of buttons, its macros,
and its chain list while shown. Its width is its own (`implicitWidth`, at least 200 px with its frame, about 430 with
the chain list). Its save button saves the rack, with everything in it, as every device's does. The frame hands it
its `DeviceInfo` (`info`: `chainListShown`, `rackDevicesShown`, read from the project's view state, following
`Project::rackViewChanged`).

- **The strip** (`DeviceHeaderButton`s, a column at the left): `chainListButton` (icon `chain_list`, lit while the
  list shows: `deviceSelection.toggleChainList`), `devicesButton` (icon `rack_devices`, lit while the chain's devices
  show: `toggleRackDevices`), `addMacroButton` (+) and `removeMacroButton` (−) (`RackMacros::add()`, `remove()`:
  `editor.setMacroCount`), disabled at 16 and at 1.
- **Macros** ([RackMacroKnob.qml](../../ui/qml/devices/RackMacroKnob.qml) over
  [RackMacro](../../ui/src/devices/RackMacro.h)): a `Grid` of `RackMacros::count` cells in two rows (`columns` =
  half the count, rounded up), each 60 px wide: its name (`macroName`, elided) over a 30 px 0..1 knob reading the
  rack's `macroParam(i)` as it is now (its envelope's value while its automation plays: `bridge.currentValue`, read
  again as the playhead moves), with the automation dot (`automation`). Turning one calls `editor.setMacro(track,
  rack, i, value, key)`, which moves what it is mapped to as one undo step; pressing it calls `touch()`
  (`editor.touchParameter`: its lane shows). The name lights up while something is mapped, the tooltip lists the
  mappings. Double-clicking the name (a `TapHandler`, so the press still reaches the frame) opens a `TextField` over
  it (`startRename()`; Enter, Escape or leaving it keeps the text: `RackMacro::rename` → `editor.renameMacro`).
- **A macro's menu** (`DevicePanel.showMacroMenu(cell, at, x, y)`): Show Automation, Delete Automation, Re-Enable
  Automation (while overridden); Rename (the cell's `startRename()` once the menu closes); Edit Mappings… (while
  something is mapped); an *Unmap …* entry per mapping (`unmapEntries()`), or a disabled "Nothing mapped" one; Add
  Macro, Remove Last Macro (`RackMacro::addMacro()`, `removeLastMacro()`).
- **Its mappings** ([RackMacroMappings.qml](../../ui/qml/devices/RackMacroMappings.qml), the panel's `macroMappings`
  popup, opened under the macro by `show(macro, at)`): a row per mapping (`mappingKeys`, which changes only when the
  mappings do, so a row isn't made again while its range is dragged; each row reads its `mappingList` entry): its
  name, `low` and `high` `ValueBox`es in percent (`RackMacro::setRange(device, param, low, high, key)` →
  `editor.setMacroRange`, one undo step per drag), the parameter's values there in its units (`lowText`,
  `highText`: `formatTarget`), Invert (swaps them) and Unmap. Escape or a click outside closes it; it closes when its
  macro goes.
- **Chains** ([RackChains](../../ui/src/devices/RackChains.h), [RackChainRow.qml](../../ui/qml/devices/RackChainRow.qml)
  over [RackChain](../../ui/src/devices/RackChain.h)), while the chain list shows (no rows are made while it is
  hidden, so `DeviceChainArea` finds none to drop on): a 22 px row per chain (an empty-rack hint while there are none,
  and **+ Chain**: `editor.tryAddRackChain`), lit with an accent bar while its chain shows beside the rack. A row
  has an activator, its name (renamed in place: `startRename()`, kept on Enter, Escape or leaving the field; empty
  keeps the old name), solo, volume, pan and meter (`meterUpdated`, from `bridge.chainMeters`). Edits go through
  `editor.setChainParam(track, chain, field, value, key)`. Chain volume and pan are automatable and follow their
  automation as the headers' do. A click shows the chain (`DeviceSelection::clickChain`, which also makes it the one
  Ctrl+R renames). Right-click: Rename, Duplicate, Delete, Add Chain, Show Volume Automation, Show Pan Automation;
  after the menu (unless Rename, or the chain went) the chain shows beside its rack.

Racks in the engine: [engine/routing.md](../engine/routing.md).

## Device editors

A built-in device can have an editor of its own instead of the knob pages: a QML file in
[ui/qml/devices/editors](../../ui/qml/devices/editors), named in the `DeviceEditors` singleton's table for the
device's kind (its engine id):

```js
readonly property var editors: ({
    "compressor": "CompressorEditor.qml",
    "delay": "DelayEditor.qml",
    "disperser": "DisperserEditor.qml",
    "eq": "EqEditor.qml",
    "sampler": "SamplerEditor.qml",
    "sidechain": "SidechainEditor.qml"
})
```

`DeviceEditors.editorFor(kind)` gives its URL (or "" for the knob pages), and the frame loads it with
`setSource(url, {trackId, deviceId})`. What an editor is, for the frame:

- `required property string trackId` and `deviceId`.
- It is the device's body: the frame around it (the border, the title bar, the menu) is the panel's. Its
  `implicitWidth` is the body's width (Compressor 658, Delay 532, Disperser 544, Sampler 760, Sidechain 720, EQ 580,
  or 756 with its band controls); it may change. It gets the body's whole height and grows its graphs into it (6 px from the top
  and the bottom), while its knobs stay at the top; `implicitHeight` is the least it needs.
- Optional: `pages` (read) and `page` (read/write) for pages of knobs, and `pageNames` (the title bar's tabs instead of
  its arrows); `menuActions` (a list of `Action`s the
  device's menu starts with); the signal `sidechainMenuRequested()` (the frame shows the sidechain menu).
- Clicks it doesn't take go on to the frame (selecting the device, its menu).

An editor sets parameters as the knobs do, so undo, automation and saving work alike: its controls bind to
`DeviceParam`s ([ParamKnob.qml](../../ui/qml/devices/ParamKnob.qml), [ParamBox.qml](../../ui/qml/devices/ParamBox.qml),
[ParamButton.qml](../../ui/qml/devices/ParamButton.qml), [ParamArea.qml](../../ui/qml/devices/ParamArea.qml): the
value as it is now, set undoably, touched when pressed, the parameter's menu on right-click). Its drawn parts are
[DeviceCanvas](../../ui/src/devices/DeviceCanvas.h) items:

- `value(paramId)` reads a parameter as it is now (its envelope's value while automation plays), `automationState()`
  its dot; `sync()` is called on the GUI thread whenever they may have changed (the device's parameters, its
  automation, the playhead while any of its parameters follows automation, the device coming or going): subclasses
  read what they draw there; `paint()` only reads that.
- `setParams(values, mergeKey, text)` / `setParam()` go through `editor.setDeviceParams` (one undo step per merge key,
  the first parameter's lane shown); `touch()` shows a parameter's automation.
- `readDisplay(id)` reads what the device's processor published for its editor since the last call
  (`bridge.readProcessorDisplay`, a position kept per processor and display; `readDisplayAt()` with the absolute index
  of the first value). `refreshDisplays()` is called as the meters update (`metersUpdated`, about 30 times a second),
  only while the item is visible. Displays are what the engine's built-in devices publish for their editors (see
  [engine/devices.md](../engine/devices.md)).
- `secondPressOfDoubleClick()`: Qt Quick delivers a double-click's second press before the double-click; items whose
  first click changes what a double-click does skip it.

The editors:

- **Compressor** ([CompressorEditor.qml](../../ui/qml/devices/editors/CompressorEditor.qml)): every knob on one page,
  four to a row, and a [ReductionGraph](../../ui/src/devices/ReductionGraph.h) (232 px): the gain reduction over the
  last `kHistory` (240) display values (about 1.3 s at 48 kHz), growing downward to 24 dB, filled with a gradient; an
  *In* meter of what keys it with the threshold marked by an accent notch; an *Out* meter (both -60 to 0 dBFS); the
  current reduction and the threshold in figures. It reads the displays `reduction`, `input` and `output`.
- **Delay** ([DelayEditor.qml](../../ui/qml/devices/editors/DelayEditor.qml)): no pages; laid out as Ableton's: per side
  a Sync button, then a grid of sixteenths and an offset field, or a time knob; the link button (the right side
  greyed out while linked); a [FilterGraph](../../ui/src/devices/FilterGraph.h) (the filter's response on a
  20 Hz..20 kHz log axis, its dot dragged across for the frequency and up and down for the width, over a spectrum of
  the display `input`: a 4096-point Hann FFT of the latest samples, falling 1 dB per refresh:
  `analysis::FallingSpectrum`), the Filter switch, the frequency (a log-scaled value box) and width; the Mode buttons
  and Ping Pong; Feedback with Freeze beside it over Dry/Wet.
- **Disperser** ([DisperserEditor.qml](../../ui/qml/devices/editors/DisperserEditor.qml)): no pages: Amount (in whole
  stages), Frequency, Pinch and Dry/Wet knobs side by side, their names above and values below, over a Bypass button; beside
  them a [DispersionGraph](../../ui/src/devices/DispersionGraph.h) (260 px): the stages' group delay in ms on a
  20 Hz..20 kHz log axis, worked out at the engine's sample rate from the engine's own stages
  (`sub::app::disperserGroupDelayMs`, the application layer's wrapper of `sub::disperser::groupDelayMs`, so the curve
  is the delay that plays), one point per column plus its peak and where it is tuned (high up, the peak can be
  narrower than a column: `disperserPeakFrequency`), worked out again when the parameters or the audio device (its rate)
  change. Its delay axis is logarithmic and fixed, `kMinMs` (0.1 ms; less lies along the bottom) to `kMaxMs` (30 s), a
  line and a figure per decade: an octave lower the same settings delay twice as long, so the curve slides up by the
  same distance and keeps its shape (an axis fitted to the curve jumped as it was dragged). The dot
  sits on the curve where the stages are tuned (`disperserTunedFrequency`), with the delay there read out at the top
  right: drag across for the Frequency, up and down for the Pinch (doubling every `kPinchPixels`, 60 px), one undo
  step per drag. Bypassed or fully dry, the curve is greyed.
- **EQ** ([EqEditor.qml](../../ui/qml/devices/editors/EqEditor.qml)): no pages, no body margins: the curve
  ([EqGraph](../../ui/src/devices/EqGraph.h)) from the title bar to the bottom edge, Scale and Output knobs in its
  bottom corners ([EqCorner.qml](../../ui/qml/devices/editors/EqCorner.qml)), and the selected band's controls
  ([EqBandPanel.qml](../../ui/qml/devices/editors/EqBandPanel.qml), 162 px) beside it while `EqView.panel`: they
  start collapsed, and the sliders button over the curve's top right shows them in every EQ (the editor is wider
  then). The expand button opens the EQ bigger in a window of its own
  ([EqWindow.qml](../../ui/qml/devices/editors/EqWindow.qml), via the `EqWindows` singleton: one per device, made or
  brought to the front; titled after the track; reading its displays on a 16 ms timer of its own
  (`displayInterval`); closed when the device goes).
  - `EqGraph`: 10 Hz..22 kHz, ± `EqView.range` dB (3, 6, 12 or 30); 24 bands; each band's curve and the total from the
    engine's own filter design (`sub::app::eqResponseDb`, the application layer's wrapper of the engine's
    `sub::eq::responseDb`, so the curve is the sound), the selected and hovered bands filled in their colours
    (`bandColor()`), the total with a glow. Hovering within `kCurveHit` (12 px) of the curve shows a ghost band of the
    type for where it is (a low cut at the far left, then a low shelf, bells, a high shelf and a high cut); keep the
    button down to drag it on. Drag a band across for its frequency and up and down for its gain (a cut, notch or band
    pass: its Q; Ctrl: the Q), Shift finely; the wheel sets its Q (Alt, or while a cut is dragged: its slope).
    Double-click a band to switch it off and on, Alt-click it (or select it and press Delete, which takes the
    `ShortcutOverride` while a band is selected) to remove it; double-click elsewhere to add one. Dots ease between
    sizes on a 16 ms timer that stops when they settle. Right-click a band or the background for the menus
    ([EqGraphMenus.qml](../../ui/qml/devices/editors/EqGraphMenus.qml): a band's type, slope, placement, Enabled,
    Delete Band; the analyzer's mode, the range, Delete All Bands). The labels at the top switch the analyzer's mode
    and the range (`EqView`, shared by every EQ and not saved).
  - Every change goes through `editor.setDeviceParams`, so a drag (adding a band and dragging it on, too: the same
    parameters every move) is one undo step.
  - The analyzer (`analysis::EqAnalyzer`): an 8192-point Hann FFT of the displays `input` and `output`, rising 0.55 and
    falling 0.09 of the way per refresh, the loudest bin per column, tilted 4.5 dB/octave (the tilt fading in over
    18 dB above the floor, so a spectrum at or falling to the floor stays flat).
- **Sidechain** ([SidechainEditor.qml](../../ui/qml/devices/editors/SidechainEditor.qml)): no pages, no margins: the
  curve ([CurveGraph](../../ui/src/devices/CurveGraph.h)), then [ClashView](../../ui/src/devices/ClashView.h) with Fit,
  Auto and the character, then the controls (Trigger and Sync, six small knobs; Lows Only over the crossover).
  - The curve: drag a point to move it (the first and last only up and down), drag between points to bend the curve
    there, click anywhere else to add a point (and keep dragging it), double-click a point (or Alt-click it, or select
    it and press Delete) to remove it, double-click a bend to straighten it; the wheel bends too, Shift makes any of
    them fine. Right-click for the shapes to start from, the fit, flip and reset. The curve's points are always
    written whole (every slot's four parameters) through `editor.setDeviceParams`, so a drag (adding a point and
    dragging it on, too) is one undo step, and Auto's fits merge into one while nothing else is done.
  - `refreshDisplays()` reads the displays `key`, `input` and `phase` together (`readDisplayAt`: by absolute index)
    into a `sidechainFit::Capture` ([SidechainFit.h](../../app/src/analysis/SidechainFit.h)), so they line up to the
    sample. The phase gives the playhead (a trail of the latest positions riding the curve) and the hits; each hit's
    kick, once it has come, is analysed over the latest `kKeepKicks` (3) into the fit, which the graph (the kick's
    envelope where it clashes, the dashed target) and `ClashView` (the spectra, the clash band) draw. Fit writes the
    points, the length (sync off) and the crossover. The hint over the curve (no sidechain while triggered by one)
    asks the frame for the sidechain menu.
- **Sampler** ([SamplerEditor.qml](../../ui/qml/devices/editors/SamplerEditor.qml)), laid out as Ableton's Simpler:
  two pages named in the title bar (`pageNames`: Sample, Controls).
  - *Sample*: the mode tabs (Classic, 1-Shot, Slice: a `RoleButton` each, its icon over its name) beside the display: a
    [SampleView](../../ui/src/devices/SampleView.h) over a strip of the sample's settings on the same dark ground (Gain;
    by mode Loop and its Fade, or Trigger/Gate, or Slice By with its sensitivity, division or regions and Playback;
    Snap; Warp, *as* its length, its mode, `:2` and `*2`). Under the display a row: the filter (on, its shape as an
    icon, 12/24, Frequency, Res, dimmed while off), the LFO (on, Hz or synced, its shape, its rate or synced rate), the
    envelope (Classic's ADSR, the others' fades), Transp, Vol < Vel, Volume. The display takes the height there is.
  - *Controls*: sections of knobs and buttons: Pitch (Root Key, Transp, Detune, Voices, Glide), Sample (Start, End,
    Loop Start, Loop Fade; Reverse, Snap, Loop), LFO (on, Retrig, shape, sync and rate; its Volume, Pitch, Filter and
    Pan), Output (Pan, Gain).
  - Lists are [ParamChoice](../../ui/qml/devices/ParamChoice.qml)s (a small button with the value's name or icon and
    an arrow, its list a menu, filled when it opens); value boxes' texts are made again when their parameter comes
    (`formatOf(param)`: a box whose value doesn't change would keep the empty text it had without one).
  - The display ([SampleView](../../ui/src/devices/SampleView.h)): the waveform from its peaks
    (`waveformColumns()`), flipped while it plays reversed, so what is drawn is what plays left to right; the sample's
    name in its top left corner; Start and End as accent lines flagged at the top, the rest dimmed; by mode, the loop
    (bracketed over the top from Loop Start, its handle at the bottom, its crossfade shaded at its end and where it
    fades from), the fades (a line), or the slices (a line where each starts, numbered at the bottom while there is room, the one the playhead
    is in lit: `playingSlice`); the newest note's position (display `position`); a time ruler (m:ss:mmm) under it. It
    places everything as the engine does: frames from percent (truncated), snapped with Snap through the application
    layer's `sampleSlices::nearestZeroCrossing`, slices from `sampleSlices::sliceStarts` over the transients it finds
    once per sample and direction ([SampleSlices.h](../../app/src/audio/SampleSlices.h), the engine's own functions on
    the bridge's `Waveform`, which is the engine's decoded source). Drag Start, End, or Loop Start while Classic
    loops (within `kMarkerGrab` 5 px, the nearest; Loop Start by its handle in the bottom `kLoopHandle` 10 px first,
    so it can be pulled off Start; one undo step per drag, kept within the others); drop an audio file on it, or
    double-click it to browse.
  - The device's menu starts with Load Sample…, Clear Sample and Reverse (`menuActions`). The sample's path lives in
    the device's state ("sample"); loading one is `editor.setDeviceState(..., "Load Sample")`, undoable. The waveform
    is the bridge's (`waveform(path)`, requested if not decoded yet; quick, as the engine decoded it for the sampler).

### Adding an editor

1. Write `ui/qml/devices/editors/<Name>Editor.qml`: an `Item` with `required property string trackId, deviceId`, an
   `implicitWidth` (the device's width less 2) and an `implicitHeight`.
2. Add `"<kind>": "<Name>Editor.qml"` to the table in
   [DeviceEditors.qml](../../ui/qml/devices/editors/DeviceEditors.qml).
3. Bind its controls to `DeviceParam`s (`ParamKnob`, `ParamBox`, `ParamButton`) so they read the parameter as it is
   now, set it undoably and touch it when pressed; or, for knob pages, use `DeviceParams` and `DeviceParamKnob` with
   `pages` and `page`.
4. Draw anything else in a `DeviceCanvas` subclass in `ui/src/devices` (`QML_ELEMENT`): read what you draw in
   `sync()`, the device's displays in `refreshDisplays()` (`readDisplay("<id>")`), write with `setParams()`.
5. Add a case to [test_ui_device_editors.cpp](../../tests/app/test_ui_device_editors.cpp).

## Gotchas

- **Frames are made again only when the chain changes.** `DeviceChainList` keeps its ids unless the chain's devices
  change; everything else (selecting, switching, editing, a plug-in rebuilding its parameters) updates the frames in
  place. Keep view state (pages, the chain shown) by id in the area or the selection, never in a frame.
- **The model's value or the engine's.** Built-in devices read the model (`DeviceParam`, through the bridge while
  automation plays); plug-ins read the plug-in (`PluginParam`), since a plug-in can change its own parameters. Both
  write through the editor.
- **A device's processor may be gone** before its frame is: `DeviceParam` and `PluginParam` are `valid` only while it
  is there, and `DeviceCanvas::alive` goes false when the device does.
- **A chain row's menu can delete its chain**: what runs after the menu checks `area.hasChain()` first.
- **Saving stores plug-in states first.** A preset is the model's device: `savePreset` and `copySelected` call
  `bridge.storePluginStates` for the device and everything in it, or a plug-in would be saved as it was last stored.
- **Menus are filled when they open** (`PanelMenu`, `DynamicMenu`), so they show how things are then, and keep the
  items they made (or the garbage collector takes them).

## Tests

| Test file | Covers here |
|---|---|
| [test_ui_device_panel.cpp](../../tests/app/test_ui_device_panel.cpp) | Folding devices (saved, not undone; the fold button; double-clicks; several selected folding together; a folded rack hiding its chain), cut, copy, paste and duplicate with the focus, switching a device off without making the frames again; building the chain and its hint, the device's menu and the one beside the devices |
| [test_ui_device_panel_plugins.cpp](../../tests/app/test_ui_device_panel_plugins.cpp) | A plug-in's parameters in pages, edited undoably and following automation; devices fitting the view; the editor button, double-click and Show Editor; VST3 presets; plug-ins loading after a project opens, and missing ones; dropping plug-ins; selecting, deleting and reordering devices; dragging a device to another track |
| [test_ui_device_panel_presets.cpp](../../tests/app/test_ui_device_panel_presets.cpp) | The save button on every kind of device (asking before replacing), a rack taking the preset's name, presets dropped between devices and onto a device of their kind (outlined; one undo step) or of another, default presets |
| [test_ui_device_panel_racks.cpp](../../tests/app/test_ui_device_panel_racks.cpp) | Ctrl+G and Ctrl+Shift+G, macros and chains, the chain list and the chain's devices shown when asked for, the chain clicked shown beside the rack and dropped into, chain mixers, macros added and taken away, renamed in place, automated, mapping to a macro, its ranges and unmapping, Ctrl+R on a chain (its list hidden too), a chain's menu, the view's height staying put |
| [test_ui_device_panel_sidechain.cpp](../../tests/app/test_ui_device_panel_sidechain.cpp) | The sidechain button and its menu: sources, cycles greyed out, taps (after devices in racks too) |
| [test_ui_device_editors.cpp](../../tests/app/test_ui_device_editors.cpp) | Each editor loaded as the view loads it, driven with the mouse and keys, the project and (rendering offline) the engine checked; the parameter cell and its menu |
| [test_session_devices.cpp](../../tests/app/test_session_devices.cpp) | `DeviceSelection` through the session: selecting, the focus, the clipboard, folding, racks, drops, presets |
| [test_sidechain_fit.cpp](../../tests/app/test_sidechain_fit.cpp) | The Sidechain's fit |
