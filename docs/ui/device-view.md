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
| [DeviceCanvas](../../ui/src/devices/DeviceCanvas.h), [EditorPaint](../../ui/src/devices/EditorPaint.h) | The base of the editors' drawn items, and their drawing helpers (among them `LogAxis`, the graphs' logarithmic frequency and time axes, and `drawDecadeGrid()`), what they animate with (`MeterBallistics`, a meter's fall and held peak; `Eased` and `easeFraction()`, a value easing towards its target by the time since the last tick) and the meters and lines they share (`dbToY()`, `drawLevelMeter()`, `drawReductionMeter()`, `drawGlowPolyline()`) |
| [editors/DeviceEditors.qml](../../ui/qml/devices/editors/DeviceEditors.qml) | The editor registry (a singleton): `editorFor(kind)` |
| [editors/EditorCaption.qml](../../ui/qml/devices/editors/EditorCaption.qml), [editors/EditorReadout.qml](../../ui/qml/devices/editors/EditorReadout.qml), [editors/EditorKnob.qml](../../ui/qml/devices/editors/EditorKnob.qml), [editors/DeviceParamMap.qml](../../ui/qml/devices/editors/DeviceParamMap.qml) | The editors' captions (a control's name over it) and readouts (its value under it); `EditorKnob`, a knob between the two, bound to its parameter and dimmed while disabled; `DeviceParamMap`, a `DeviceParam` per id (`get(id)`) |
| Compressor: [CompressorEditor.qml](../../ui/qml/devices/editors/CompressorEditor.qml), [ReductionGraph](../../ui/src/devices/ReductionGraph.h) | |
| Gate: [GateEditor.qml](../../ui/qml/devices/editors/GateEditor.qml), [GateViews.qml](../../ui/qml/devices/editors/GateViews.qml), [GateGraph](../../ui/src/devices/GateGraph.h), [GateKeyGraph](../../ui/src/devices/GateKeyGraph.h), [GateFilterIcon](../../ui/src/devices/GateFilterIcon.h), the application layer's [GateResponse.h](../../app/src/audio/GateResponse.h) | |
| Limiter: [LimiterEditor.qml](../../ui/qml/devices/editors/LimiterEditor.qml), [LimiterGraph](../../ui/src/devices/LimiterGraph.h), the application layer's [LimiterResponse.h](../../app/src/audio/LimiterResponse.h) | |
| Multiband Dynamics: [MultibandEditor.qml](../../ui/qml/devices/editors/MultibandEditor.qml), [MultibandGraph](../../ui/src/devices/MultibandGraph.h), the application layer's [MultibandResponse.h](../../app/src/audio/MultibandResponse.h) | |
| Spectral Compressor: [SpectralEditor.qml](../../ui/qml/devices/editors/SpectralEditor.qml), [SpectralGraph](../../ui/src/devices/SpectralGraph.h), the application layer's [SpectralResponse.h](../../app/src/audio/SpectralResponse.h) | |
| Saturator: [SaturatorEditor.qml](../../ui/qml/devices/editors/SaturatorEditor.qml), [SaturatorCurve](../../ui/src/devices/SaturatorCurve.h), [SaturatorColorGraph](../../ui/src/devices/SaturatorColorGraph.h) (with [SaturatorPaint.h](../../ui/src/devices/SaturatorPaint.h), what they share), the application layer's [SaturatorResponse.h](../../app/src/audio/SaturatorResponse.h) | |
| Amp: [AmpEditor.qml](../../ui/qml/devices/editors/AmpEditor.qml), [AmpPanel](../../ui/src/devices/AmpPanel.h), [AmpDriveGraph](../../ui/src/devices/AmpDriveGraph.h), [AmpToneGraph](../../ui/src/devices/AmpToneGraph.h), [AmpDisplays.h](../../ui/src/devices/AmpDisplays.h) (reading its displays), the application layer's [AmpResponse.h](../../app/src/audio/AmpResponse.h) | |
| Erosion: [ErosionEditor.qml](../../ui/qml/devices/editors/ErosionEditor.qml), [ErosionGraph](../../ui/src/devices/ErosionGraph.h), [ErosionScope](../../ui/src/devices/ErosionScope.h), the application layer's [ErosionResponse.h](../../app/src/audio/ErosionResponse.h) | |
| Delay: [DelayEditor.qml](../../ui/qml/devices/editors/DelayEditor.qml), [FilterGraph](../../ui/src/devices/FilterGraph.h) | |
| Chorus-Ensemble: [ChorusEditor.qml](../../ui/qml/devices/editors/ChorusEditor.qml), [ChorusGraph](../../ui/src/devices/ChorusGraph.h), the application layer's [ChorusVoices.h](../../app/src/audio/ChorusVoices.h) | |
| Phaser-Flanger: [PhaserEditor.qml](../../ui/qml/devices/editors/PhaserEditor.qml), [PhaserGraph](../../ui/src/devices/PhaserGraph.h) (with `DisplayPlayback`), the application layer's [PhaserResponse.h](../../app/src/audio/PhaserResponse.h) | |
| Reverb: [ReverbEditor.qml](../../ui/qml/devices/editors/ReverbEditor.qml), [ReverbFilterPad](../../ui/src/devices/ReverbFilterPad.h), [ReverbSpinPad](../../ui/src/devices/ReverbSpinPad.h), [ReverbDecayGraph](../../ui/src/devices/ReverbDecayGraph.h), the application layer's [ReverbResponse.h](../../app/src/audio/ReverbResponse.h) | |
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
    "amp": "AmpEditor.qml",
    "chorus": "ChorusEditor.qml",
    "compressor": "CompressorEditor.qml",
    "delay": "DelayEditor.qml",
    "disperser": "DisperserEditor.qml",
    "eq": "EqEditor.qml",
    "erosion": "ErosionEditor.qml",
    "gate": "GateEditor.qml",
    "limiter": "LimiterEditor.qml",
    "multiband": "MultibandEditor.qml",
    "phaser": "PhaserEditor.qml",
    "reverb": "ReverbEditor.qml",
    "sampler": "SamplerEditor.qml",
    "saturator": "SaturatorEditor.qml",
    "sidechain": "SidechainEditor.qml",
    "spectral": "SpectralEditor.qml"
})
```

`DeviceEditors.editorFor(kind)` gives its URL (or "" for the knob pages), and the frame loads it with
`setSource(url, {trackId, deviceId})`. What an editor is, for the frame:

- `required property string trackId` and `deviceId`.
- It is the device's body: the frame around it (the border, the title bar, the menu) is the panel's. Its
  `implicitWidth` is the body's width (Compressor 658, Gate 566, or 833 with its sidechain section, Limiter 608,
  Multiband Dynamics 816, Spectral Compressor 936, Saturator 756, Amp 642, Erosion 532, Delay 532, Chorus-Ensemble
  534, Phaser-Flanger 732, or 906 with More open, Reverb 890, Disperser 544, EQ 580, or 756 with its band controls,
  Sidechain 720, Sampler 760); it may change. It gets the body's whole height and grows its graphs into it (6 px
  from the top and the bottom), while its knobs stay at the top; `implicitHeight` is the least it needs.
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
- **Gate** ([GateEditor.qml](../../ui/qml/devices/editors/GateEditor.qml)): no pages, laid out as Ableton's Gate:
  the display ([GateGraph](../../ui/src/devices/GateGraph.h), 280 px), then Threshold, Return and Floor over
  Attack, Hold and Release (`EditorKnob`s, centred in the body's height), and beside them Flip (level with the first
  row's knobs) and the Lookahead chooser (0, 1 or 10 ms) under its caption. Floor at its bottom reads "−inf dB" (a
  formatter of the editor's own, on the readout and the knob's tooltip). A strip down the left edge (`sidechainFold`,
  "Sidechain" reading upwards, in the accent while the key isn't the plain input) unfolds the sidechain section
  between it and the display, as Ableton's unfolds to the left of the device: its width animates (140 ms) and the
  editor's `implicitWidth` with it, the controls laid out at the full width so the clip reveals them. Which Gates
  show it is view state, kept by device id in the `GateViews` singleton (not saved), so a frame made again keeps it.
  - The section: a source button (the sidechain's track name, or "No Sidechain"; a click emits
    `sidechainMenuRequested()`, so the frame shows the device's sidechain menu), Listen (headphones), EQ, and the EQ's
    six types in Live's order (low shelf, bell, high shelf, low-pass, band-pass, high-pass: a button each, its shape
    drawn by [GateFilterIcon](../../ui/src/devices/GateFilterIcon.h), dimmed while the EQ is off, its parameter's menu
    on right-click); under them Gain and Dry/Wet (dimmed without a sidechain), Freq, Q and Gain (dimmed while the EQ is
    off; Q disabled for the shelves, Gain for the pass filters); under those the key filter's curve
    ([GateKeyGraph](../../ui/src/devices/GateKeyGraph.h)): its response on a 20 Hz..20 kHz log axis, ±18 dB, from the
    engine's own filter at the engine's rate (`sub::app::gateKeyFilterDb`, the application layer's wrapper of
    `sub::gate::keyFilter`, so the curve is what keys the gate), filled under it in the accent while the EQ is on and
    grey while it is off (easing between them, 100 ms). Drag its dot across for the frequency, up and down for the gain
    (shelves, bell) or the Q (the pass filters, and the bell with Ctrl: doubling every 60 px), Shift finely, one undo
    step per drag; the wheel over the dot sets the Q (the bell and the pass filters; notches closer than 400 ms are one
    undo step), as the EQ's bands; a double-click puts the three back to their defaults. Which types use Gain and Q
    (`usesGain`, `usesQ`, from `sub::app::gateKeyFilterUsesGain/Q`) also enables the knobs. The dot's level is worked
    out with the curve on the GUI thread, so `paint()` only reads members. One rule throughout: what is set for later
    (Gain and Dry/Wet without a sidechain; the EQ's knobs, type buttons and curve while it is off) is dimmed but stays
    settable, menus and all, as Live's greyed controls; only what the EQ's type doesn't use is disabled, and the curve's
    dot never sets that either.
  - `GateGraph` draws the last 2.5 s of the displays `input`, `output`, `key` and `open` (one value per 256 samples
    each, read with `readDisplayAt` into a ring per stream by absolute index, so they line up whatever a read got; a
    stream that doesn't go on from where it was, a new processor, starts the history again), newest at the right edge,
    on a −72..+6 dB axis: the input as a light grey band, the output over it darker with a white outline (so what the
    gate takes away is the light part showing above the dark), a faint blue shade over all of it where the gate let
    sound through (over the levels too, so it reads whatever the level), and the key in green while it isn't the plain
    input (a sidechain, or the EQ on it). Values are drawn in buckets aligned to their absolute index (as many as make a
    column a pixel wide: the max of the levels, the mean of `open`), with `fillBand`, so the history glides by fractions
    of a pixel without shimmering. Across it the threshold (a glowing blue line, its tab at the right) and where an open
    gate closes again (Return: a dashed orange line below it, the band between them tinted), each showing its value
    while hovered or dragged (drawn over everything else). Top left an LED and "Open"/"Closed", or "Idle" (the LED out)
    once no values have come for 0.25 s: the device switched off, the engine stopped; "Listening to the key" pulses at
    the top on a dark backing while Listen is on. On the right edge the newest key level as a dot (blue while the tick's
    key level, not the dot's falling one, is at or above the threshold, its halo as open as the gate; a ring ripples out
    from it as the gate opens, clipped to the plot), the dB figures, and two meters in `kPanel` wells (as the
    Compressor's), captioned "In" and "Gate", both on the plot's own axis (0 dB at their top, a tick every 12 dB where
    the plot has its grid lines): the input's level (green, yellow from -12 dB, red from -3 dB and above 0 dB) and how
    far the gate turns down what comes in, growing down from the top by as many dB (`sub::app::gateGainDb` of `open` and
    the floor: to the floor's depth when shut, the whole well at a silent floor).
  - Its animation, all stepped in `refreshDisplays()` by the time since the last tick: the drawing scrolls on at the
    values' rate, easing towards a steady 30 ms behind the newest value (which absorbs the audio's block-sized bursts;
    more than 100 ms behind, it jumps), and stops where the values stop; the input meter has ballistics
    (`MeterBallistics`: it rises at once, falls 24 dB/s, its peak held a second), the key dot falls at 300 dB/s, the
    LED, the reduction meter, the lines (when moved from elsewhere: automation, a knob, undo) and their hover all ease
    (`Eased`, `easeFraction`). It repaints only while something moves or changes what is drawn: scrolling repaints while
    something that differs is in sight (a history of silence scrolling by changes nothing), and the meters and the key
    dot once they are a twentieth of a pixel from where they were last drawn (a steady tone's peaks jitter by about 1e-4
    dB a tick) or the dot's colour changes, so an idle, silent or steady Gate draws nothing; `levelsChanged` likewise
    waits for a level to move 0.01 dB.
  - Drag the threshold line (or anywhere else in the plot; not the figures or the meters) up and down for the
    threshold, the return line for Return (down: it closes lower), relative to where the press was (nothing jumps),
    Shift finely; one undo step per drag ("Change Gate Threshold", "Change Gate Return"), the lines following the
    mouse at once. With the two lines on top of each other (Return 0), a press on or below them takes Return, above
    them the threshold. Double-click a line to put it back to its default. Right-click a line for its parameter's
    menu (`paramMenuRequested(id)`, opened by the editor's `ParamMenu`); elsewhere right-click goes on to the frame
    (the device's menu).
- **Limiter** ([LimiterEditor.qml](../../ui/qml/devices/editors/LimiterEditor.qml)): no pages; laid out as Ableton's:
  the Gain knob over Maximize, the display (a [LimiterGraph](../../ui/src/devices/LimiterGraph.h), 340 px), Release
  over Auto, then Lookahead and Mode (lists), the Routing buttons (L/R, M/S) and Link (a value box). The knobs stay at
  the top and the buttons at the bottom, level with the display's; the right column spreads over the body's height.
  Gain (±24 dB) is drawn bipolar. With Maximize on, the Gain knob crossfades to Output's (120 ms, EditorKnob's own
  `Behavior`; only the knob fading in is enabled, so a press mid-fade never turns the one going away) and the line's
  box at the display's top left (its caption too, as wide as "Threshold" so the box stays put) to the Threshold;
  Release dims while Auto is on (opacity 0.55, as Ableton greys it) and can still be set, for when Auto is off. The
  line's box and Link's are their widest text (the parameter's minimum and maximum, formatted) + 16 px and 4 more,
  so the automation dot clears a minus sign.
  - The display: the level over the last `kHistorySeconds` (1.5 s) on a fixed axis (+12 to -36 dB): the input
    (grey, red where it goes over the line), the output inside it (light), and the gain reduction hanging from the
    top, filled with a gradient and edged with a glow (24 dB at the bottom: twice the level's scale, so its 6, 12 and
    18 dB fall on the level's grid lines); beside it In, GR and Out meters (two bars each: L and R, or M and S for GR
    in M/S; EditorPaint's `drawLevelMeter`/`drawReductionMeter` in wells), and under them the peaks of the last
    second; the gain reduction of the last half second at the bottom left. Both gain reduction figures count Soft
    Clip's share, as the GR bars do, and read negative ("GR −6.3 dB", "-6.3"); the reduction's scale figures (6, 12,
    18) are drawn over the line on small backings, so the line passes behind them. The history is drawn in the line's
    domain, as the device works (`sub::app::limiterLine`, the application layer's wrapper of `sub::limiter::scales`):
    the input as the device hears it (after Gain; with Maximize the raw input) and the output moved down by what
    Maximize adds (Output - Threshold, converted as each value comes, so the history stays put when the parameters
    move), so the loudest output meets the line; the In meter is the history's input, while the Out meter and its
    figure read the output in dBFS, as the axis does. Its time axis and silence are the device's
    (`limiterMeterSamples()`, `limiterFloorDb()`). In Soft Clip a band shades where the knee rounds off (from
    `limiterSoftKneeDb()`, -6.02 dB, to `limiterSoftTopDb()`, +3.52 dB, where it reaches the line, dashed), and the
    knee's share of the reduction (display `clip`) is stacked under the gain reduction, lighter, in the history and
    on the GR bars. A badge names the mode (SOFT CLIP, TRUE PEAK), with MAX beside it while Maximize is on.
  - The line, across the plot and the meters to its handle, is the Ceiling (the Threshold with Maximize): drag it
    within `kLineGrab` (5 px) up and down (Shift finely: a quarter; pressing Shift mid-drag changes the rate from
    there on), to 0.1 dB within the parameter's range (the device's, read as the drag starts), one undo step per
    drag; double-click it for its default. Presses elsewhere go on to the frame. While held it follows the value at
    once; moved otherwise (automation, undo, its box, Maximize) it eases there, and the hover (the line lit, the
    resize cursor) is checked again as it moves under a still mouse.
  - `refreshDisplays()` reads the seven displays (`input_l`, `input_r`, `output_l`, `output_r`, `reduction_a`,
    `reduction_b`, `clip`: one value per 128 samples) by absolute index (`readDisplayAt`) into rings of `kRing`
    (4096) values, keeping what one stream has that another hasn't yet, so they stay in step; a gap is filled with
    silence, a new start (the device's processor made again) starts the history again. The history is drawn from fixed bins (a column holds
    the most of its values by absolute index, so a column never changes once complete), placed by a cursor: the
    index at the plot's right edge, moved on by the time since the last tick and eased towards `kLagSeconds`
    (40 ms) behind the newest value, so the history scrolls smoothly at a steady speed whatever the bursts the values
    come in, and holds still when they stop. The meters read what the cursor passes (`MeterBallistics`: In and Out
    fall 24 dB a second, peaks held 1 s; GR and the knee's share 30 dB a second, held 0.5 s), so they move with the
    history's edge.
  - It opens showing the device as it is: the first sync with a device snaps the line, Soft Clip's band and the
    badges to their values (DeviceCanvas syncs once before its track and device are set; that sync does nothing).
  - Everything moves in `advance()` (from `refreshDisplays()`, by `tickSeconds()`): the cursor,
    the meters, and, eased (`Eased`, `easeFraction`), the line (50 ms), Soft Clip's band and the badges (80 ms), the
    line's glow (warming from white to orange with the gain reduction, fully at 6 dB) and the hover (the line
    thickens, its handle lights). It repaints only while something moves or a figure changes: not while idle, nor
    while silence scrolls through once nothing in view is loud.
- **Multiband Dynamics** ([MultibandEditor.qml](../../ui/qml/devices/editors/MultibandEditor.qml)): no pages; laid
  out as Live's, a row per band with High on top (the rows are the graph's lanes: `rowHeight` is the graph's, the
  body's height less a 16 px header, in three): the band's button, its activator (lit while the band works; off, the
  band is bypassed), and its solo; under High's and Low's, the switch that splits the band off (lit while it is; off,
  the Mid band has its frequencies) beside the crossover (a log-scaled box reading "2.50 kHz", typed as "1.5k" or
  "800"); the band's Input; its lane of the display; two fields the **T**/**B**/**A** buttons in the header over the
  band column switch between, for every band at once (attack and release, or the Below or the Above threshold and
  ratio; all are made, one page shown, fading in over 120 ms, so their object names stay put), each column captioned
  ("Attack" and "Release", "Below" or "Above" and "Ratio"); its Output; then the device's Amount, Time and Output
  knobs (Output drawn bipolar), Soft Knee, Peak and RMS, and the sidechain's S/C Gain and S/C Mix knobs (24 px;
  dimmed to 0.55 without a sidechain, but settable first, as the Gate's) beside a Sidechain button (lit while there is
  one; it asks the frame for the sidechain menu under itself) over Listen, the three rows spread over the body's
  height. The page shown is the device's view state (`DeviceViews`, "page"), so it outlives the frames being made
  again. A band switched off or bypassed dims its boxes to 0.55 over 120 ms (they stay editable, as Live keeps them);
  one switched off dims its button and crossover too, and disables its solo (it has no sound of its own to solo). Ratio boxes print the `ratio` unit as Live does
  (`formatValue`: "1:4.00", "1:66.7", "1:100", "1:0.500": R dB past the threshold come out as one) and read it
  through `parseRatio` (the application layer's `multibandParseRatio`): Live's "1:R", R alone, or a compressor's
  "4:1"; to three decimals, so a typed "1:0.333" stays. The attack and release boxes read "250 ms", "1.5 s" or a
  bare number of milliseconds (`parseTime`, `multibandParseMs`). Boxes take their formatter from their parameter
  (`formatOf(param)`), so one whose value never changes still gets its text, and are a little wider than their
  sample text needs (its width + 16), so the automation dot at their left never covers a minus sign.
  - The display ([MultibandGraph](../../ui/src/devices/MultibandGraph.h), 268 px): a lane per band on a linear level
    axis from −80 to +6 dB (its figures, −80 to 0, in the header: a "+6" at the edge would run into the "0"; a faint
    line every 10 dB, 0 dB brighter). In each, the Below region is a block from the left edge to the Below threshold and
    the Above region one from the Above threshold to the right edge, tinted by what the side does to the level (the
    accent where it pulls it down: compression above, expansion below; teal where it pulls it up), as strongly as the
    ratio is far from 1:1 (and fainter with Amount), and hatched with vertical lines more densely the further the ratio
    is from 1:1 (Live's "density shows the ratio"); a side at 1:1 is only its handle, in grey. Each threshold is a line
    with a grip. Over them the band's level after its dynamics as a thick bar in the meters' colours, its level before
    them as a thin one under it, the change between the two in orange (a cut) or teal (a boost) with a bright edge where
    the level is now (`changeSpan`: from the out level by the eased change, never on past the level before it, since the
    out meter stops at the floor while a gate-like expansion goes far under it), the held peak, a small marker where the
    static curve is taking the level while attack or release catch up (`targetMarkerDb`, by the levels the displays last
    reported: `staticOutDb`, the application layer's `multibandGainDb`, the wrapper of the engine's
    `sub::multiband::staticGainDb`, so what is drawn is what plays; none once within 0.5 dB, a target under the floor
    counting as at it), and the change in figures at the lane's top right, while the band sounds. Hovering a lane shows
    the static curve there ("−30.0 → −34.5 dB") beside a hairline, ending short of the change's figure where it would
    meet it (`hoverLabelRect`, `gainLabelRect`).
  - Drags: within `kHandleGrab` (5 px) of a threshold's line, across for the threshold (rounded to 0.1 dB; Above and
    Below never cross: one pushes the other); inside a block, up or down for its ratio, doubling or halving every
    `kRatioPixels` (30 px), in the direction that makes the block's level follow the mouse (down in Above's compresses,
    up in Below's lifts), held to 1:0.250..1:100, 1:1 within 3 %, three significant digits. **Ctrl**: every band's
    threshold (or ratio) by as much; **Alt**: both thresholds of the band together, keeping the gap and stopping as a
    pair at either end; **Shift**: a fifth as far (read on every move, so it can change mid-drag). Ctrl+Alt is left to
    the device chain (its hand scroll, which takes the press first). Double-click a handle for its threshold's default
    (−20 dB Above, −40 dB Below, pushing the other) or a block for 1:1. The wheel moves a threshold half a dB a notch
    and a ratio by 2^(1/8), up being louder in that region; a run of notches on one target (less than `kWheelGesture`,
    0.4 s as the EQ's, apart) is worked out from where it began, as a drag is, so a high-resolution wheel's or a touchpad's small
    steps add up (and leave 1:1's detent) rather than each being rounded away. A run keeps its target while the mouse
    stays within `kWheelStill` (5 px) of its last notch: a threshold it moves slides out from under the mouse (about 1.5
    px a notch), onto its block or the gap between the thresholds, and the run goes on with the threshold. Shift+wheel
    is the chain's (it scrolls), so the graph leaves it. A switched-off band's lane takes drags as any other. One undo
    step per drag, per double-click and per run of notches (`setParams` with a merge key per gesture, the parameter
    grabbed first so its lane shows). Between the thresholds and in the header a press goes on to the frame. The cursor
    is a horizontal resize over a handle and a vertical one over a block; while dragging, a bubble over the handle (or
    at the mouse) reads the value ("Above −23.5 dB", "Below 1:2.00"); a switched-off lane's "→ Mid" under it makes way
    (fading out over 60 ms, back when the bubble leaves it).
  - Its animation, in `refreshDisplays()`, moved on by the time since the last tick (`tickSeconds()`): it reads all
    nine displays (`<band>_in`, `_out`, `_gain`) and looks at the recent values only (`kRecentSpan`, 100 ms, at the
    engine's display rate, `multibandDisplaySamples()`: 19 at 48 kHz, so a 2048-sample buffer's eight, which arrive at
    once, are all read), so after a stall it shows now rather than the backlog's loudest; with nothing new it keeps the
    last readings for `kHoldSeconds` (0.24 s), then takes the floor. The meters have ballistics
    (`MeterBallistics`: rising at once, falling 36 dB/s, the out peak held 1 s); the gain change eases (`Eased`, 30 ms)
    to the reading, and once the audio has stopped (`lettingGoGain`) to the change between the falling meters, so the
    bars and the figure agree all the way down (the meters take seconds to fall; the change alone would be home in a
    tenth of one); where one meter is at the floor already, the figure keeps the reading (the change goes on past it, as
    `changeSpan` draws it while playing) until the other is there too; each side glows (a halo on its handle, a brighter
    block; rising over 50 ms, falling over 250 ms) while it changes the level of a band that sounds, judged by the level
    the displays last reported, not by the falling meter (which, after the audio stops, would pass through a Below
    region the band isn't in); the handle and block under the mouse or dragged light up (60 ms; in a Ctrl or Alt drag,
    every one moved); a lane eases to 0.35 when its band is switched off (with "→ Mid" over it) or bypassed (its level
    shown as it comes, nothing working: no glow, marker or figure) and to 0.5 when a solo mutes it (80 ms). A device shown afresh is drawn as it is (the first `sync()` snaps them). Once everything has
    settled it stops calling `update()` (`animating()` false): an idle or silent editor doesn't repaint.
- **Spectral Compressor** ([SpectralEditor.qml](../../ui/qml/devices/editors/SpectralEditor.qml)): no pages: the
  thresholds' knobs at the left (Threshold, Ratio, Below, Upward over Tilt, Knee, Range, Smoothing); at the right the
  Focus band's edges as value boxes (Focus Low over Focus High, each a `ParamBox` under its name, level with the knobs
  beside it: typed, scrolled, automated and mapped as any control), then the time and output knobs (Attack, Release,
  Stereo Link over Dry/Wet, Output and the Delta button); the knobs in 64 px cells (wide enough for "Stereo Link" and
  "-0.5 dB/oct"), the Focus column as wide as its widest value ("20.00 kHz", measured with a `TextMetrics`) with 11 px
  either side, clear of the automation dot, and between them a [SpectralGraph](../../ui/src/devices/SpectralGraph.h)
  (376 px, its own implicit width), the sections parted by `EditorDivider`s. Tilt and Output are bipolar (symmetric
  about 0); Below is dimmed (0.55, still settable) while Upward is 1:1, when it does nothing.
  - The graph: a 20 Hz..20 kHz log axis against the engine's pink-referenced level (-78 to +18 dB; figures every 24 dB
    inside its left edge, drawn over the Focus dim so they read wherever the band starts, each fading out while a
    threshold line runs through it, as it would read as that line's level). It draws the input spectrum filled and the output's as a glowing line (the displays `input` and `output`,
    128 points a frame; the line turns from blue to red with Delta, as it then shows what is taken away, and the cut's
    curtain and glow, which show the same, fade back), the threshold (`sub::app::spectralThresholdDb`, the application
    layer's wrapper of the engine's `sub::spectral::thresholdDb`, so the line is the threshold that plays) with its
    pivot at 1 kHz and tilt handles at 100 Hz and 10 kHz, the green Below line (`spectralBelowDb`: never above the
    threshold) with its handle at 300 Hz, fading in while Upward is over 1:1 (each line drawn straight to the edge it
    leaves the plot by, steep and near the axis' ends; a handle whose own frequency is then off the axis sits where its
    line leaves, so it stays on it), and the Focus band's edges (outside them dimmed by the engine's own weights,
    `spectralFocusWeights` for the edges drawn, sampled across the plot as one gradient: fully where a frequency gets
    none of its gain, fading over each edge as the sound does; an edge left at 20 Hz or 20 kHz shows only its grip at
    the plot's bottom).
  - Drag the pivot or the line for Threshold, an end handle to tilt both lines about 1 kHz, the green line for Below,
    an edge sideways for the Focus (kept a third of an octave apart); each move adds its own distance, so Shift (a
    tenth as far) slows a drag from where it is pressed, as on the knobs; drags hold to the parameters' ranges (read
    from the engine's parameters); double-click resets, each drag is one undo step (`setParams` with one merge key),
    and the value hovered or dragged reads in the header, fading in and out.
  - What it does as it plays: the cut at each frequency hangs from the top (orange, `gain`, 24 dB reaching half the
    plot, with a line holding the deepest recent cut for 0.8 s and then falling at 18 dB/s, drawn for as long as it is
    there), the lift rises from the bottom (green), and where the level compared (`key`: after the envelopes and Stereo
    Link, the sidechain's while keyed, then also drawn dashed where it is on the axis) is over the threshold glows
    orange, while anything over it is turned down (not at Ratio 1:1, Range 0 or Dry/Wet 0: the glow fades out over
    80 ms); the header reads the deepest cut now (and the biggest lift, while Upward is on); In/Out meters
    (`in_level`, `out_level`) stand at its right edge. The Sidechain badge over its top left is lit while a sidechain
    keys it; a click asks the frame for the sidechain menu, under the badge (`sidechainMenuRequested(badge)`).
  - The engine writes 128 values a frame per spectral display, the first at an absolute index that is a multiple of
    128, and publishes them three hops late (in step with what is heard); the graph puts frames together again by the
    index `readDisplayAt` gives (a gap waits for the next whole frame) and merges those that came since the last tick
    (the highest; for the gains the deepest cut). In `refreshDisplays()` the spectra rise at once and fall back over
    250 ms (sinking to the floor 0.3 s after the frames stop), the gains flow (45 ms), the lines and edges glide to
    automated or undone values (60 ms, following a drag at once), the meters have the house ballistics, all moved on
    by `tickSeconds()`, and the item repaints only while something moves. The display points per frame and the pivot
    come from the application layer (`kSpectralDisplayPoints`, `kSpectralPivotHz`, checked against the engine's).
- **Saturator** ([SaturatorEditor.qml](../../ui/qml/devices/editors/SaturatorEditor.qml)): no pages; laid out as Live
  12.1's Saturator with its expanded view open beside the front panel, in columns 8 px apart, a 1 px line
  (`Theme.border`) after the front panel and after Color. The front panel: Drive (a 40 px knob) over the Type list (a
  `ParamChoice`), DC and HQ at the bottom; the curve ([SaturatorCurve](../../ui/src/devices/SaturatorCurve.h), 160 px)
  over the Post Clip list, where Live puts it; Output over Dry/Wet. Then Color: the Color switch in a row of its own,
  its graph ([SaturatorColorGraph](../../ui/src/devices/SaturatorColorGraph.h), 220 px) under it (not on it: there it
  would hide a handle dragged to the top), and under that Base, Freq, Width and Depth (24 px knobs, Base and Depth
  bipolar), dimmed to 55 % while Color is off (still editable, as Live's greyed knobs are). Last the curve's own
  controls under a title (Waveshaper or Bass Shaper, in the accent colour while that curve is chosen): the Waveshaper's
  six knobs in two rows (dimmed unless it is chosen), or the Bass Shaper's Threshold, the two cross-fading (150 ms) as
  the Type goes to or from Bass Shaper. The bottom rows (DC and HQ, Post Clip, Dry/Wet, Color's knobs, the Waveshaper's
  second row) all end 6 px above the body's bottom, the graphs growing into the height there is; `implicitHeight` is the
  tallest column's (Output over Dry/Wet: 8 px less than the body on any font).
  - The curve: input across (−1..1, ±0 dBFS before Drive), output up over ±`kOutputRange` (1.15, so full scale and a
    clip's flat top show inside the plot), two points a pixel, from the engine's own functions
    (`sub::app::saturatorCurve`, the application layer's wrapper of `saturator::transfer`: Drive, the curve and Post
    Clip; Color's emphasis cancels where the curve is straight, and Dry/Wet and Output aren't drawn), so the curve is
    the shaping that plays. The straight line is dashed; Post Clip's ceiling shows as red dashes at ±1, the Bass
    Shaper's threshold as accent dashes where its curve leaves the straight line (`saturatorThresholdInput`; not
    when Drive far above the threshold puts them within 3 px of the middle, where they would say nothing). Drive's
    value is in the bottom right corner (the quadrant an odd curve takes only when it folds: Sinoid Fold, the
    Waveshaper's ripples), HQ over it while on, each on a dark backing so that a grid line, Post Clip's ceiling or a
    fold behind doesn't run through it.
  - Its animation, in `refreshDisplays()`: the display `in_peak` (the input's peak every 128 samples, before Drive) puts
    two dots on the curve at ± its level, rising at once and falling back (τ 150 ms), fading out in silence (τ 120 ms).
    The stretch of the curve between them is lit (`drawGlowPolyline`), amber turning red as the curve bends away from
    its small-signal line there (1 − |f(x)| / (f′(0)·x), eased τ 60 ms; `saturatorSlope`), the dots' halos growing with
    it; beyond them an afterglow runs out to the highest the dots reached in the last 0.3 s (then falling, τ 250 ms),
    fading along its length, so a drum loop's hits leave a trail that breathes back. Input over full scale flashes red
    bars at the plot's sides (fading, τ 300 ms). The In strip under the plot (the dots' level on the x axis: its bar
    ends straight under them) and the Out strip at its right (`out_peak`, the device's real output, on the y axis,
    falling back as the dots do) are bars from the middle out, green, yellow from −12 dB, red from −3 dB, their peaks
    held 1 s as ticks (`MeterBallistics`). The displays' values come a block of audio at a time: a tick without any
    within 0.1 s of the last (a block longer than a tick, 1024 or 2048 frames) is a gap, not silence, so the levels hold
    over it (and Color's spectra, below) instead of dipping at the blocks' rate. A new shape (a Type, Drive, Threshold,
    Post Clip or Waveshaper change, undo, automation) morphs the drawn curve to the new one (τ 40 ms, landing on it
    exactly), so the ripples swell as WS Depth turns; under its own drag it follows at once. An editor that opens draws
    the device as it is (its first sync with the device there snaps, both graphs), not a morph from the defaults. The
    unlit curve is dimmer than the lit stretch. Once the dots, afterglow, meters and morph have settled it doesn't
    repaint.
  - Drags: up and down for Drive (0.25 dB a pixel), across for the Bass Shaper's Threshold (0.25 dB) or the
    Waveshaper's Curve (0.5 %), Shift five times as finely, pressed or let go mid-drag without a jump (each move adds
    its own distance); one undo step per drag, which shows the automation of the parameter it has moved most (Drive,
    or the Threshold or Curve for a drag mostly across). Double-click: Drive back to 0 dB.
  - Color's graph: Color's emphasis (the EQ before the curve; the inverse after it is its mirror image) on a
    20 Hz..20 kHz log axis, ±36 dB, from the engine's own design (`sub::app::saturatorColorDb`, the wrapper of
    `saturator::colorDesign` and `colorResponseDb`) at its sample rate (again when the audio device changes). It eases
    to new settings (τ 40 ms, the frequency in log, landing exactly on the parameters' curve) and glows in the accent
    colour while Color is on, grey while off (fading, τ 80 ms). Behind it the spectra of the displays `input`
    (filled) and `output` (a line) through `analysis::EqAnalyzer` (as the EQ's: rising 0.55 and falling 0.09 of the
    way per refresh), smoothed a little across columns; once a display's window is all silence and its spectrum is
    down, it isn't analysed. Two handles: Base on the shelf at 60 Hz (up and down, following the mouse: Base moves by
    the distance over the shelf's slope there), the peak at Freq and Depth (across and up and down, from where it was
    pressed; Depth's automation shows for a drag mostly up and down, Freq's else); a drag switches Color on in the
    same undo step, double-clicking a handle sets its gain to 0 dB, and a hovered or dragged handle fills with a glow
    ring and the pointing-hand cursor (set again on release, from where the mouse is). A press away from the handles
    goes on to the frame.
- **Amp** ([AmpEditor.qml](../../ui/qml/devices/editors/AmpEditor.qml)): no pages; laid out as Live's Amp, flat on the
  body: the seven models as a row of `ParamButton`s (`role: "monitor"`, 50 × 18, from the parameter's labels) over Gain,
  Bass, Middle, Treble, Presence and Volume (`EditorKnob`s in 58 px cells 4 px apart, reading "5.0": the `dial` unit),
  and set apart at the right the Output switch (Mono, Dual: the `dual` parameter, Live's "Dual Mono") over Dry/Wet.
  Under the chosen model a 2 px underline in the model's colour (`ampModelColor()`: the one colour the face has of its
  own, besides the accent) slides to a new one (160 ms, `OutCubic`) as its colour turns. The editor is three columns
  10 px apart (140, 368 and 80 px) and the output meter (10 px) at the right; the controls at the top, and a row of dark
  wells (`Theme.meterBg`, 4 px corners) fills the rest of the body's height, 6 px from its bottom: the transfer curve,
  the tone curve and the pilot lamp. `implicitHeight` is the controls' and 40 px of graphs (142 in the tests' host,
  whose body is 154).
  - [AmpPanel](../../ui/src/devices/AmpPanel.h) fills the editor under everything and draws only into the rects the
    layout gives it (`tubeRect`, `jewelRect`, `meterRect`), taking no mouse. The tube window: V1, V2, V3 and the power
    tube on a rail, each glowing as hard as its stage is driven (the displays `drive1..3` and `power`, its peak
    against its clipping point: an idle heater's 0.12 at −24 dB and below, full at +6 dB), rising with τ 30 ms and
    cooling with τ 250 ms: the glass warms, the cathode shows through the plate's slot, the heater whitens and a soft
    bloom (three layers, as the square of the glow, clipped to the window) spreads round it. The power tube's plate and
    glass turn blue with the supply's sag (display `sag`, τ 60 ms, full at 6 dB), as hard-driven power tubes do. The
    pilot lamp (a red jewel with a bezel, a halo and a facet) brightens with the output (display `output`) and dims as
    the supply sags (τ 50 ms), over the model's name as the amp's logo, bold italic in its colour. The output meter
    is `drawLevelMeter()` through `MeterBallistics` (falling 24 dB/s, the peak held 1 s), −60 to 0 dBFS. The lamp and
    the meter have a tooltip each (the meter's over its whole height).
  - [AmpDriveGraph](../../ui/src/devices/AmpDriveGraph.h): the transfer curve, input across (−1..1, 0 dBFS at the
    edges), output up: for a 1 kHz tone of peak x, the output's highest value at x and its lowest at −x, from the
    engine's own stages, filters and voicing (the application layer's `AmpTransferCurve`, wrapping `amp::Transfer`:
    the tone's steady state by harmonic balance, so a played tone's peaks land on it within 0.1 dB at the defaults), a
    point a column. Up is scaled to the curve's reach (its largest output without sag at 0.86 of the half height):
    the models are level-matched far below full scale, and the shape is what shows how hard and how lopsidedly it
    clips. A faint line carries the clean gain (the slope through 0) on. The display `input` puts two dots at ± its
    peak (`MeterBallistics`: up at once, falling 18 dB/s; fading out from −54 to −72 dB), with a trail of where they
    were over the last 8 ticks and the stretch of the curve between them lit (`drawGlowPolyline`): they sit where the
    tone's peaks come out. The curve is made (1.3 ms at 140 px: a tone's work for each x and −x together, the two
    exact opposites) only when what it is made from changes, the settings, the width or the rate, not at every sync:
    syncs come at every playhead move while any of the device's parameters follows automation. The sag (eased, τ
    60 ms) lowers the power stage's drive, and the curve is made again each time it moves 0.05 dB from the preamp's
    part kept (the power stage's alone: 0.35 ms), so its shoulder breathes down as the amp sags. The input's peak
    reads at the bottom right.
  - [AmpToneGraph](../../ui/src/devices/AmpToneGraph.h): the tone stack (with its make-up) and Presence as they sound,
    30 Hz..16 kHz, −24 to +12 dB (the figures in a 20 px gutter at the right, where the curve never goes), from the
    engine's own design at its sample rate (`sub::app::ampToneResponseDb`, the wrapper of `amp::toneResponseDb`; again
    when the audio device changes), a point a column plus the handles' frequencies, filled to the 0 dB line and
    glowing. A handle per tone control on the curve (B 100 Hz, M 700 Hz, T 3 kHz, P 6 kHz), its parameter's control
    as the knob is: drag it up and down for its dial (8 px a step, Shift a fifth of that, from where the mouse is: Shift
    pressed mid-drag changes the rate from there on), one undo step per drag, what it sets read out beside it while
    dragged; the wheel over it, 0.2 a notch (the knob's), Shift a fifth, notches within 400 ms one undo step;
    double-click one for 5 (its second press starts no drag); right-click one for its parameter's menu (the editor's
    `ParamMenu`, through `handleMenuRequested`); its automation dot beside its letter (red while automation plays,
    grey overridden). A press within 14 px across of one takes it; anywhere else, both presses of a double-click
    too, goes on to the frame. A handle under the mouse grows (4.5 to 6.5 px, τ 60 ms) with the up-and-down cursor.
    A new model's curve morphs from the one drawn (τ 40 ms through a smoothstep, landing exactly); dials, drags and
    automation move it at once.
  - Display ticks that read nothing (the engine hands its values over at its own pace: at 44.1 kHz in 1024-frame
    blocks, one 60 Hz tick in three) keep the last values, so nothing flickers with the block size; after 0.3 s with
    nothing at all (the device off, the engine stopped) everything cools and falls. A read counts what came since the
    last tick (at least its last 50 ms, at most 0.1 s; the panel's and the drive graph's alike, by
    [AmpDisplays.h](../../ui/src/devices/AmpDisplays.h)): a canvas made, or shown again, reads the whole backlog (up
    to 8192 values, 44 s at 48 kHz), whose loudest is history, not the level now. A canvas's syncs before its device is
    there (the session, track and device ids are set one by one) do nothing, so the first with it snaps: an editor
    opened on a model shows it at once, no colour turning from Clean's, no curve sweeping in. Each canvas calls
    `update()` only while something it draws moves: settled or silent, the face doesn't repaint.
- **Erosion** ([ErosionEditor.qml](../../ui/qml/devices/editors/ErosionEditor.qml)): no pages; laid out as Live
  12.4's: Width and Stereo at the left, the display in the middle, Frequency, Amount and Noise Blend at the right
  over the modulation's scope, in cells of 66 px (wide enough for "Noise Blend"). Beside Noise Blend a sine and a
  checkered noise glyph, pictures as Live's (not buttons), each as bright as its source plays; Width dims while Noise
  Blend is 0 (it does nothing to the sine).
  - The display ([ErosionGraph](../../ui/src/devices/ErosionGraph.h), 300 px): an X-Y field on a 20 Hz..20 kHz log
    axis, its dot at the Frequency (as the modulator plays it, below 0.45 of the rate) and the Amount (0 to 100 %
    up). Behind it the input's spectrum, filled, and the output's as a line over it (`analysis::EqAnalyzer` of the
    displays `input` and `output`, so the fizz Erosion adds shows at the top); on it the noise's band peaking at the
    dot, worked out at the engine's rate from its own filter (`sub::app::erosionBandMagnitude`, the application
    layer's wrapper of `sub::erosion::bandMagnitude`, so the band drawn is the one that plays): a point per pixel
    plus where it is tuned, so a band narrower than a pixel still peaks at the dot, and for its fill the most of each
    2 px column's edges and middle. Its −3 dB edges have a short whisker out from each, level with where the
    outline crosses them (an upright tick would lie along a narrow band's steep outline). The dot's travel up and
    down (`travel()`) is the plot less its reach: its halo's at the top, so it stays clear of the strip's texts, and
    its ring's at the bottom, so it stays in the well. The sine is a spike up to the dot; Noise Blend
    crossfades the band and the spike by the engine's equal-power weights (`erosionBlendWeights`). The strip over
    the field reads the source ("Noise 70 % · Stereo 60 %") and where and how far it modulates ("1.00 kHz ·
    ±87 µs": `erosionExcursionText`).
  - Drags: a press puts the dot there, and dragging moves it (Frequency across, Amount up and down); Shift starts
    from where the dot is and moves it at `kFine` (0.15) of the mouse; Alt (Option) starts from the dot too and drags
    up and down for the Filter Width (twice it every `kWidthPixels`, 40 px; across still sets the Frequency, as
    Live's). A drag from the dot leaves what it hasn't moved exactly as it was. The drag's position isn't held to the
    field, so dragged out and back the dot comes back under the mouse. The wheel sets the Filter Width: a notch
    multiplies it by 2^(1/4) (about 19 %), with Ctrl by 2^(1/16) (Shift+wheel is the device chain's: it scrolls
    the chain before the graph sees it). One undo step per drag (a merge key per gesture; a notch during an Alt drag
    joins it, one while the dot is dragged does nothing) and per burst of notches less than `kWheelGestureMs`
    (400 ms) apart. Hovered with Alt held, or in an Alt drag, the band's edges brighten.
  - Its animation, in `refreshDisplays()`: the display `erosion` (what the device changed, in dB) sets an activity,
    0 at −60 dB to 1 48 dB above, eased up 0.35 and down 0.08 of the way per refresh. Only the newest values a
    refresh read count (`erosionRecentDb`: the latest 35 ms): an editor shown, or shown again, after the sound
    stopped reads a backlog of up to 44 s of it, which would otherwise light it up in silence. With it the band's fill
    shimmers like heat haze (knots every 16 px glide towards random heights and now and then take fresh ones, the
    columns between following smoothly; the fill dips up to 40 % below the outline) and brightens, in two layers
    whose knots part as Stereo widens (one edge in mono, two of their own at 100 %); the dot gets a halo; the sine's
    spike trembles like a plucked string (a wave running up it, still at both ends). The spectra rise and fall as
    the EQ's. Silent (no sound in the displays, the spectra fallen to the floor, the activity at rest) it stops
    repainting (`animating()`); silence costs no FFT.
  - The scope ([ErosionScope](../../ui/src/devices/ErosionScope.h), 46 px, "L/R Mod"): the modulators left against
    right as a goniometer in a round well (mid up, side across, the radius easing into the rim: tanh), the latest
    5 ms as a trace fading with age over a soft cloud two RMS out (eased): an upright line in mono, an ellipse, then
    a round cloud (noise) or a circle (the sine, its sides a quarter cycle apart) at full Stereo. Blue for the sine,
    orange for noise, mixed by the blend (noise's scribble kept fainter than its cloud); dim at Amount 0, brighter as
    the sound is eroded. The device publishes its modulators every sample (`mod_l`, `mod_r`): decimated, a sine near
    a multiple of the slower rate's Nyquist would draw as two points on a line. They are read one after the other
    and paired by absolute index; values the second read holds beyond the first's (published between the two) wait
    for their partners in the next refresh. It repaints while the sound is eroded and for `kSettleTicks` (a third of
    a second) after a change, the trace catching up with it; in silence it holds still. `paint()` reads only what
    `refreshDisplays()` stored (the trace's length at the engine's rate too).
- **Delay** ([DelayEditor.qml](../../ui/qml/devices/editors/DelayEditor.qml)): no pages; laid out as Ableton's: per side
  a Sync button, then a grid of sixteenths and an offset field, or a time knob; the link button (the right side
  greyed out while linked); a [FilterGraph](../../ui/src/devices/FilterGraph.h) (the filter's response on a
  20 Hz..20 kHz log axis, its dot dragged across for the frequency and up and down for the width, over a spectrum of
  the display `input`: a 4096-point Hann FFT of the latest samples, falling 1 dB per refresh:
  `analysis::FallingSpectrum`), the Filter switch, the frequency (a log-scaled value box) and width; the Mode buttons
  and Ping Pong; Feedback with Freeze beside it over Dry/Wet.
- **Chorus-Ensemble** ([ChorusEditor.qml](../../ui/qml/devices/editors/ChorusEditor.qml)): no pages; laid out as
  Ableton's: the mode tabs (Chorus, Ensemble, Vibrato: `ParamButton`s on `mode`) over the display, and under it a
  strip: the High-pass switch (`filter_highpass` icon) and its frequency (a log-scaled value box, its implicit width:
  "2.00 kHz" and room for the automation dot; dimmed (0.55) while the switch is off but still settable), and in
  Chorus mode, at the strip's right end, Taps (1, 2) and Time (a `ParamChoice`: Auto, 7 to 50 ms, as wide as its
  longest choice with the arrow, measured with `FontMetrics`), which give way to "3 voices a side" / "1 voice a
  side" in the other modes. Beside it four columns of knobs in two rows: Rate over Amount, Feedback (with Ø, its
  polarity, in a 14 px slot of its own; both dimmed (0.55) in Vibrato, which has no feedback, but still settable)
  over Warmth, the mode's column (Width centred between the rows; in Vibrato Offset over Shape), Output over Dry/Wet.
  The sets a mode changes cross-fade one after the other (140 ms, eased: the old set out over the first half, the
  new in over the second, so their texts never overlap).
  - The display ([ChorusGraph](../../ui/src/devices/ChorusGraph.h), 234 px): each voice's delay as it moves, time
    running right to left: the column `kNowInset` (10 px) in from the right edge is now, where a dot rides each
    voice; the left edge is `windowCycles` of the modulation ago (rate × 1.2, held to 1..8: about 1.2 s across up to
    6.7 Hz, so the traces scroll at a steady pace and a fast rate shows tight cycles instead of strobing). The delay
    axis (ms) is the layout's range at Amount 100 with 12 % more each side, fixed per layout (turning the Amount
    visibly grows the swing), its ends and the centre (dashed; in Auto it rises and falls with the Amount) figured at
    the left, where the traces' oldest end fades into the ground (`kFadeWidth`, 36 px). Every delay is worked out
    by the engine's own maths (`sub::app::chorusDelayMs`, `chorusVoicePhase`, the application layer's wrappers of
    `ChorusDesign.h`), so the traces are the delays that play. Left voices orange, right ones blue (`kRightColour`),
    each voice of a side a little lighter; Warmth tints them towards red, Feedback thickens them (not in Vibrato,
    which has none), a narrow Width fades the right side; fully dry they are grey. The header names the axis and
    reads the largest detune (`chorusPeakDetuneCents`: "±22 ct", from 100 ct "±1.1 st"); while dragging, the Rate
    and the Amount.
  - Drags: up and down for the Rate (twice it every `kRatePixels`, 40 px), or across for the Amount (all of it in
    `kAmountPixels`, 150 px), from where the press was. The first move past `kLockPixels` (3 px) picks which, by its
    direction, and the drag sets (and touches) only that one for the rest of it: a hand's drift sideways leaves the
    Amount as it was, and the other's automation plays on. A press alone changes nothing; Shift four times as
    finely, pressed or let go mid-drag without a jump. One undo step per drag (a merge key per gesture).
  - Its animation, in `refreshDisplays()`, moved on by `tickSeconds()`: the display `phase` (the LFO's phase every
    `chorusDisplaySamples()`, 128, samples) drives an estimate that runs on at the rate each tick and is pulled
    towards the engine's newest value (τ 50 ms; the first values, those after `kSnapSeconds` (0.3 s) of ticks without
    any, or an error over a quarter cycle are taken as they are), so the traces scroll at the display's rate while
    the dots move as the delays do. The display `level` (the wet's peak after Output; the loudest of the values the
    last tick's time covers only: a read can bring a backlog, the display's history, seconds of it, when the editor
    opens or is shown again) goes through `MeterBallistics` (24 dB/s) into a glow (−48 dB dark to −6 dB
    full, eased τ 50 ms): the traces brighten and bloom and the dots grow halos as sound passes. Mode, Taps and Time
    changes cross-fade the old voices (traces, dots and the axis' figures) out and the new in while the axis eases to
    the new range (τ 40 ms, as the engine's 30 ms fade); each layout drawn has an alpha of its own (up to three), so a
    change during a fade goes on from what each shows, and a layout fading out comes back from where it is; the
    figures fade out over the first half and in over the second, never two over each other. The Amount, Shape and
    Offset shown ease too (τ 40 ms), the window τ 75 ms. With nothing rendered for 0.3 s or the device off the
    traces stop and dim to 60 % (`frozen`); with the wet below `kSilentDb` (−80 dB) they rest (`resting`): the phase
    drawn holds (the estimate still follows the engine, and the traces take it again when sound comes back; the
    first values, or those after 0.3 s without any, are taken at once), so a change then reshapes them in place.
    Once everything
    has settled, a frozen or resting graph doesn't repaint.
- **Phaser-Flanger** ([PhaserEditor.qml](../../ui/qml/devices/editors/PhaserEditor.qml)): no pages; laid out as
  Ableton's, in sections a thin line apart, two rows of 30 px knobs centred in the height: the mode tabs (Phaser,
  Flanger, Doubler: tall `ParamButton`s on `mode`, the body's full height); the mode's controls (Phaser: Notches,
  Center, Spread, Blend; Flanger and Doubler: one 44 px Time knob bound to `flange_time` or `doubler_time`, and under
  the Flanger's its comb's first notch, "Notch 200 Hz"), the two sets crossfading (120 ms) as the mode changes; the
  graph (240 px); the LFO (Freq, or with its ♪ switch (12 px, in the caption's row) Rate, a list knob over the synced
  divisions; the waveform, a `ParamChoice` showing its icon; Spin; Duty Cycle, bipolar; Phase, or with Spin on, Spin);
  the globals (Amount, Feedback, Ø over **More**; Warmth, Output, Dry/Wet). The tabs share the height in whole pixels
  (45, 44, 45 in the body's 154), so their borders are crisp; the Time knob sits where it would with the Flanger's
  notch under it in the Doubler too, so it doesn't move between the two. Controls that swap with a switch rebind in
  place (one knob, its parameter, caption and step changing); synced, a rate knob steps through the note values, and
  the wheel over it moves it one a notch (an undo step each; the knob's own wheel, a fiftieth of the range, would round
  back to the same division). **More** (a `RoleButton` lit while open, not a parameter) shows a further section
  (fading in over 150 ms): LFO 2 (its mix), LFO 2's Freq or Rate with its own ♪ (dimmed while LFO 2 has no share),
  Safe Bass ("Off" at 5 Hz); Env (the switch where the caption would be) over its amount (bipolar), Attack and
  Release, dimmed while Env Follow is off (they stay live: only the opacity changes, as Live's greyed controls).
  Whether it is open is view state kept per device id while the application runs (`PhaserGraph::expanded`, a static
  hash: a frame made again shows it as it was), never saved or undone.
  - The graph ([PhaserGraph](../../ui/src/devices/PhaserGraph.h)): the response on a 20 Hz..20 kHz log axis,
    +18..−30 dB, worked out from the engine's own maths (`sub::app::phaserCurvePoints`, the application layer's
    wrapper of `sub::phaser::curve`: the stages, the comb, the feedback, Warmth's filter, Safe Bass's bands, Dry/Wet
    and Output) at the sweep the engine publishes (displays `sweep_l`, `sweep_r`, `q_l`, `q_r`), so the notches or
    the comb move as the sound does. The curve is drawn with a glow over a fill fading downwards; every Phaser notch
    is a point of it, drawn at its true depth (`phaserNotchFrequencies`, closed form), and marked by a small triangle on
    the bottom edge, as bright as it is deep (the comb's first ones while at least `kMarkerSpacing`, 8 px, apart);
    the Phaser's centre is dashed. Where the comb is finer than the eye can follow (a cycle in under `kBandPeriod`,
    8 px: the engine's dense columns, and those its per-column `turn` puts there) it is a translucent band between
    its peaks and its notches (the highest and lowest over a cycle's worth of columns), the line running along its
    top: steady as the delay sweeps, where a line would alias into flicker. The right channel's curve, while its sweep
    differs (Phase, Spin), is a thin blue line. A bar at the plot's left edge is the envelope follower's level; In and
    Out meters at its right (`drawLevelMeter`, `MeterBallistics`: 24 dB/s, peaks held 1 s, on every tick: one that
    brings no values, as large audio blocks leave some, goes on from the last that came). The header names the
    mode (and the notches) and reads the sweep now ("1.24 kHz", "3.4 ms"). Under the plot, the LFO strip: the
    waveform's name; its shape over a cycle (`phaserLfoValue`, Triangle Analog at the rate it runs), lit up to the
    phase's dot (glowing discs), a comet tail along the shape behind it (the way the dot went over the last ten
    ticks, step by step, so a fast LFO keeps a tail, at most 0.4 of a cycle), the right LFO's dot in blue; the random
    shapes as a trace of the values that came, scrolling (started afresh when one is chosen: four cycles of the shape,
    dim, until its values come); and at its right edge a bar of the summed modulation the sweep follows (LFO 1, LFO 2
    and the envelope; Amount 100 % reaches the edge).
  - Its animation, in `refreshDisplays()`: the engine publishes eleven displays every 256 samples, a block at a time
    (a 1024-frame block brings four values at once, then none for 21 ms, while the screen ticks every 16.7 ms).
    `DisplayPlayback` plays them back at their own pace: a playhead runs through them at 187.5 values a second,
    interpolating (phases the short way round, sweeps in log), held about the largest recent batch behind the newest
    (measured after each tick's move, so it never starves), a little faster or slower to stay there, jumping only when
    it falls far behind or a batch comes after a pause. So the notches glide at the screen's rate instead of jumping a
    block at a time. A mode change fades the old curve out (τ 60 ms) and draws the new where the parameters put it
    until the engine's values of that mode arrive (frames already on their way are not taken for the new mode's);
    then, as when the values start coming again, the drawn sweep catches up with the engine's over a few ticks
    (τ 30 ms) rather than jumping. With no values for 0.3 s (the engine stopped, the device off) the curve eases back
    to the parameters (τ 50 ms), the dots dim to 35 % (τ 80 ms) and the meters fall; once everything has settled
    nothing repaints. A parameter's edit is drawn at once when not playing (it is what the user drags). A curve costs
    0.1-0.3 ms at 226 columns, worked out again only when the sweep moves.
  - Drags (in the plot only, which alone shows the drag's cursor; the strip and meters go to the frame): Phaser,
    across for the Center (jumping to the press, as the Disperser's), up and down for the Spread (`kSpreadPixels`,
    150 px for its whole range); Flanger, across puts the comb's first notch under the mouse (Time = 500 / f ms), up
    and down the Feedback; Doubler, across for the Time (log, 150 ms at the left to 20 ms at the right), up and down
    the Feedback. One undo step per drag (a merge key per gesture); a double-click sets the two back to their defaults
    under its first click's key, so the click's jump and the reset are one step, undone to what was there before.
- **Reverb** ([ReverbEditor.qml](../../ui/qml/devices/editors/ReverbEditor.qml)): no pages; laid out as Ableton's,
  sections side by side with a faint rule between them: Input (Lo Cut and Hi Cut over a
  [ReverbFilterPad](../../ui/src/devices/ReverbFilterPad.h), 108 px, its frequency and width in value boxes under
  it); the early reflections (Spin over a [ReverbSpinPad](../../ui/src/devices/ReverbSpinPad.h), 96 px, its amount and
  rate in boxes; Shape over Predelay); Global (Size and Stereo over Density and Smooth, lists under their captions in
  the middle of the second row); the diffusion network (the Lo and Hi switches and the high filter's type over a
  [ReverbDecayGraph](../../ui/src/devices/ReverbDecayGraph.h), 200 px, the shelves' frequencies and gains in boxes
  under it; Decay over Freeze, Flat and Cut; Diffusion over Scale; Chorus over its Amount and Rate, 26 px knobs); the
  output (Reflect over Diffuse, and Dry/Wet). Knobs are `EditorKnob`s in two rows, top and bottom; the switches sit
  level with the first row's captions, the boxes along the bottom, and the pads and the graph grow into the height
  between them (`height - 54`). What a switch leaves unused dims (opacity 0.55, over 120 ms) but stays editable, as
  in Live: the input's boxes while both cuts are off, Spin's boxes while it is off, the type and the Hi boxes while
  Hi is off (Hi's gain also while the type is Low-pass, which has none), the Lo boxes while Lo is off, Flat and Cut
  while Freeze is off (they only act frozen), Chorus's knobs while it is off. `implicitHeight` is 148 in the tests'
  host (whose body is 154).
  - The pads and the graph draw what the engine plays, from its own maths through the application layer
    (`sub::app::reverbInputFilterDb`, `reverbDecaySeconds`, `reverbEarlyTaps`, `reverbSpinPan`, `reverbStereoWidth`,
    `reverbDiffuseOnsetMs`, over `sub::reverb`'s functions in `ReverbDesign.h`), at the engine's sample rate (again
    when the audio device changes).
  - `ReverbFilterPad`: the band's response (−30 to +6 dB) on a 20 Hz..20 kHz log axis over the spectrum of the
    display `signal` (the input summed to mono; `analysis::FallingSpectrum`); its dot is the band, dragged across for
    In Filter Freq and up and down for its width (0.5 octaves at the bottom, 9 at the top, under a 13 px strip for the
    caption, which the dot never covers). The passband's fill and a halo round the dot glow with the display `input`
    (rising at once, fading over a quarter of a second).
  - `ReverbSpinPad`: Spin's handle (across for the rate, 0.07 to 1.3 Hz on a log axis; up and down for the amount,
    under a 13 px strip for the captions, which the handle never covers) over the reflections drawn as particles: one
    per reflection the Density plays, across by its pan (times Stereo's width), down by its time (the earliest at the
    top), as big as it is loud (Shape). While the reflections sound (the display `early`), and for 2 s after Spin's
    settings change, they orbit as Spin swings them, at the phase the device publishes (display `spin`; the pans are
    the engine's own law, `reverbSpinPan`), trailing their last six positions (started again only when the particles'
    homes move: Size, Shape, Density, Stereo), and light up, fading over a third of a second as the reflections do; in
    silence they ease back to rest. Switching Spin eases its swing in or out. At the top right, when the tail starts
    after the input (Predelay, Shape's onset and the shortest line, at Size: "tail +53 ms").
  - `ReverbDecayGraph`: the decay time per frequency (to −60 dB, on a log axis from 40 ms to 100 s, under a strip for
    its captions; its figures at 0.1, 1 and 10 s sit under their lines, and one the curve or a guide runs through goes
    over its line where there is room, else over them on a chip of the background), the curve of a line of the active
    lines' mean loop through its shelves, over the spectrum of the display `tail` dying away, with a meter of the tail's
    level (display `diffuse`, `MeterBallistics` falling at least as fast as the tail does) at its right. Three handles:
    Decay (between the shelves, up and down) and the shelves' (across for the frequency, up and down for how long that
    band rings, Decay times the gain; a dashed guide runs level from each to its edge of the plot, where the one-pole
    shelf's curve settles onto it); a Low-pass high filter's handle sits on the curve and moves across only, as a
    switched-off shelf's does (it sits on the Decay line, hollow and dim). Every handle is held 6 px inside the plot. A
    drag takes the handle within `kGrab` (9 px), else the one nearest across (so a low shelf set above the high one is
    still picked by its handle), and moves it relative to the press, as ratios (Shift: a quarter as far), one undo step
    per drag; double-clicking a shelf's handle switches it (its second press starts no drag). Hovering a handle grows it
    (4 to 6 px) and reads it out at the top right ("Hi 4.50 kHz · 70 %"; else the decay, or Frozen). Frozen, the guides
    fade out and the handles dim (they are what the tail thaws to). Fully dry, it is greyed.
  - Animation: each canvas moves in `advance()`, from `refreshDisplays()` with the real time since the last tick
    (`Eased`, `easeFraction`, `MeterBallistics`). A tick that brings no values (the audio's blocks longer than a tick)
    keeps the last ones for 100 ms (the spin pad's phase runs on at Spin's rate), so meters, glows and particles don't
    flicker or stall with long buffers. A switch that changes a curve (Lo Cut, Hi Cut; Lo, Hi, the type, Density,
    Flat, Cut) eases it from the curve as drawn to the new one over about 120 ms; Freeze lifts the decay curve to the
    top in the frozen colour, easing into the edge, tinting the plot (τ 0.15 s; without Flat its shelved bands stay
    low, as they still die away); the curve thickens and its fill brightens while the tail sounds, and a ripple runs
    along it at the chorus's rate (display `chorus`) as deep as Chorus Amount, fading with the tail. Dragged values
    never animate. Each repaints only while something moves or changed: nothing while idle or silent.
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
3. Bind its controls to `DeviceParam`s (`ParamKnob`, `ParamBox`, `ParamButton`, `ParamChoice`) so they read the
   parameter as it is now, set it undoably and touch it when pressed; or, for knob pages, use `DeviceParams` and
   `DeviceParamKnob` with `pages` and `page`. A [DeviceParamMap](../../ui/qml/devices/editors/DeviceParamMap.qml)
   makes the editor's `DeviceParam`s once, by id (`DeviceParamMap { id: p; trackId: ...; deviceId: ...; ids: [...] }`,
   then `p.get("<id>")`), and an [EditorKnob](../../ui/qml/devices/editors/EditorKnob.qml) is a `ParamKnob` with its
   caption over it and its readout under it (the parameter's text, or a `formatter` of the editor's own), dimmed
   (over 120 ms) while disabled, its tooltip after a moment.
4. Draw anything else in a `DeviceCanvas` subclass in `ui/src/devices` (`QML_ELEMENT`; a new header in a build folder
   configured before it may be skipped by AUTOMOC: see [building.md](../building.md#gotchas)): read what you draw in
   `sync()`, the device's displays in `refreshDisplays()` (`readDisplay("<id>")`, or `readDisplayAt()` for streams
   that must line up), write with `setParams()`. Animate in `refreshDisplays()` by the time since the last tick, with
   [EditorPaint](../../ui/src/devices/EditorPaint.h)'s helpers (`MeterBallistics` for a meter's fall and held peak,
   `Eased` stepped by `easeFraction(dt, seconds)` for a value easing towards its target), so it looks the same
   whatever the tick's rate; `paint()` only reads them, and draws with `dbToY()`, `drawLevelMeter()`,
   `drawReductionMeter()` and `drawGlowPolyline()` where they fit. Call `update()` only while something moves, so an
   idle or silent editor doesn't repaint.
5. Add a test file of its own, `tests/app/test_ui_device_editors_<kind>.cpp` (the tests' glob picks it up), on
   [support/EditorHarness.h](../../tests/app/support/EditorHarness.h): the host window showing one editor as the view
   does (`show(kind, trackId, deviceId)`, the body's size), its parts found by object name, the mouse and wheel as a
   user moves them, the displays' clock ticked by hand (`refreshDisplays()`), and the project, the undo stack and the
   engine to check. Add the kind to `registry()` in
   [test_ui_device_editors.cpp](../../tests/app/test_ui_device_editors.cpp) too.

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
| [test_ui_device_editors.cpp](../../tests/app/test_ui_device_editors.cpp) | The registry (every kind with an editor, and the generic knobs for the others); the Compressor's, Delay's, Disperser's, EQ's, Sidechain's and Sampler's editors, each loaded as the view loads it, driven with the mouse and keys, the project and (rendering offline) the engine checked; what the editors share (SgPainter's additions, the animation helpers, `EditorKnob` and `DeviceParamMap`); the parameter cell and its menu. Its host, and every editor test's, is [support/EditorHarness.h](../../tests/app/support/EditorHarness.h) |
| [test_ui_device_editors_gate.cpp](../../tests/app/test_ui_device_editors_gate.cpp) | The Gate's editor: fitting the body, every control bound and undoable (the lookahead reaching the engine's latency); the threshold and return lines' drags (relative, Shift, double-click, one step each; either taken when they are one), their right-click menus, the meters not a control; the displays reaching the graph, its scrolling and rest, going idle, silence and a steady tone drawing nothing; the key dot's colour following the key's level now and the dot falling below the line within a few ticks, the passing shade showing over the levels; the sidechain section (fold, what is dimmed but settable and what is disabled, the source button, renames, Freq's readout whole at 15 kHz); the key curve being the engine's filter, its dot following the mouse, Ctrl and the wheel for the bell's Q |
| [test_ui_device_editors_limiter.cpp](../../tests/app/test_ui_device_editors_limiter.cpp) | The Limiter's editor: fitting the view, every control bound to its parameter and undoable (Gain bipolar; Release dimmed while Auto is on and still settable; the boxes and lists wide enough for their widest text; the lookahead reaching the engine's latency; Maximize swapping Gain for Output and the line for the Threshold, a press mid-crossfade turning the knob coming in), the line dragged (one undo step, Shift finely, held to the parameter's range, double-click for the default, presses elsewhere ignored), the hover following the line as it moves, opening as the device is (nothing animating in), the displays reaching the graph (levels, gain reduction, Soft Clip's share in both figures; with Maximize, the history's output in the line's domain and the Out meter in dBFS), its animation and its rest, the maths shared with the engine |
| [test_ui_device_editors_multiband.cpp](../../tests/app/test_ui_device_editors_multiband.cpp) | Multiband Dynamics' editor: it fits the body (at its least height too), every box as wide as its text and automation dot need, every control bound and undoable (the engine has what they set: the activators and the split switches, each where Live has it), ratios and times typed and printed, the T/B/A pages and their captions (the page outliving the editor being made again); the graph's threshold and ratio drags (pushing, Ctrl, Alt, Shift; Ctrl+Alt left to the chain), double-clicks and wheel (a high-resolution wheel's steps adding up; a run staying on a threshold that slides from under the mouse; Shift+wheel left to the chain), one undo step each; the displays reaching the graph as the engine renders (a whole 2048-sample buffer's read), its meters, eased gain, glows (not after the audio stops), target marker, a cut under the floor drawn only as far as the level before it, the bars and the figure agreeing tick by tick while the meters let go (a lift, a cut, a cut under the floor), lanes and highlights, a switched-off lane's "→ Mid" making way for a drag's bubble, the hover readout clear of the change's figure, a bypassed band's lane (its level only), and its stopping once still; the sidechain's controls (dimmed but settable without a sidechain, whole readouts, the menu asked for under the button, Listen); the `ratio` unit and the typed texts (with no window, on any platform) |
| [test_ui_device_editors_spectral.cpp](../../tests/app/test_ui_device_editors_spectral.cpp) | The Spectral Compressor's editor: fitting the body, every name and value whole, its knobs and Delta bound and undoable, the lines the engine's, lines leaving the plot drawn where they are with their handles on them (and the mouse finding them only there), the level figures a line crosses fading, the threshold, tilt, Below and Focus dragged (one undo step, Shift, Shift pressed mid-drag, double-click), the Focus boxes (wide enough for their widest value clear of the automation dot), Below dimmed but settable while Upward is 1:1, the Focus dim the engine's weights with the level figures over it, the displays reaching the graph and sinking back without a bounce, the held cut outliving the curtain, nothing drawn while still, lifts, the glow only while cutting, Delta's spectrum and tint, the key line only where the key is, the Sidechain badge (its menu under it) |
| [test_ui_device_editors_saturator.cpp](../../tests/app/test_ui_device_editors_saturator.cpp) | The Saturator's editor: fitting the body (each control in its own column, with either shaper section showing; Color's switch off its graph), every control bound to its parameter, undoable and reaching the engine (each knob, the lists, the switches; Hi-Quality's latency), the Color and Waveshaper knobs dimmed and lit, the Waveshaper and Bass Shaper sections swapping; the curve and Color's EQ being the engine's own `saturator::transfer` and `colorResponseDb` (every type, both Post Clips, every shaper control; exactly, after the morph and the ease); an editor opening on the device as it is, without a morph; the graphs' drags (Shift mid-drag, double-clicks, presses off the handles, Base dragged near the top, the cursor after a drag) as single undo steps, showing the automation of what they move most; the displays reaching the curve (the dots, the saturation, the afterglow, the over-full-scale flash) and the spectra, holding over a tick without values, and both graphs settling without repaints in silence |
| [test_ui_device_editors_amp.cpp](../../tests/app/test_ui_device_editors_amp.cpp) | The Amp's editor: fitting the body (nothing past the margins or overlapping, the buttons' labels unclipped), every control bound to its parameter, undoable and reaching the engine; the model buttons and the underline sliding and turning to the model's colour; an editor opened on a model showing it at once; the tone curve and the transfer being the engine's maths (exactly, after a new model's morph); the tone handles as controls: drags (fine with Shift, Shift mid-drag, double-clicks, presses off them) as single undo steps, the wheel, the automation dot, the parameter's menu on the right button, a double-click off them the frame's, their hover; the drive curve made again only for what it is made from (not as the playhead moves under automation), its xs exact opposites; the displays reaching the tubes, the dots, the lamp and the meter, holding through ticks that read nothing, cooling after, a backlog counting for nothing, the sag lowering the drive curve and dimming the lamp, and the face settling without repaints |
| [test_ui_device_editors_erosion.cpp](../../tests/app/test_ui_device_editors_erosion.cpp) | The Erosion's editor: fitting the body, every knob bound and undoable, the band being the engine's filter and the dot's travel clear of the strip, the display's drags (Shift, Alt) and wheel (Ctrl finely; in a device chain, whose Shift+wheel scrolls it) as single undo steps, the displays reaching the graph and the scope (not a backlog's worth after the sound stopped, and a silent graph no longer repainting), the engine having what it set |
| [test_ui_device_editors_chorus.cpp](../../tests/app/test_ui_device_editors_chorus.cpp) | The Chorus-Ensemble's editor: fitting the body (margins, nothing overlapping or cut short; the high-pass box wide enough for its widest values and the automation dot, Time for each choice), every control bound to its parameter and undoable (each knob dragged), the modes (their sets cross-fading one after the other; Feedback and Ø dimmed in Vibrato and still settable), Taps, Time, the high-pass and Ø, the display's drags (Shift, mid-drag too) as single undo steps, each setting only the parameter its direction picked (the other's automation not overridden), the displays reaching the graph (the voices where the engine's delays are, the glow from the sound now and not the display's history, freezing, and resting without repaints with the traces held), layouts fading without pops, the engine having what it set; no text spilling out of its box; no QML warnings |
| [test_ui_device_editors_phaser.cpp](../../tests/app/test_ui_device_editors_phaser.cpp) | The Phaser-Flanger's editor: fitting the body (inside its margins), every control bound to its parameter and undoable (each knob dragged, each switch clicked, the waveform chosen), the swapped controls rebinding (Freq/Rate, Phase/Spin, the delay's Time), the mode tabs, More as view state (not undone, kept when shown again; Env dimming its controls); the curve the engine's design with the notches marked where it puts them, a fine comb drawn as a band; the graph's drags and double-click as single undo steps (undone to what was there before), its cursor over the plot only; a synced rate's wheel stepping a division a notch; the tabs on whole pixels, the Time knob staying put between the delay modes; the displays reaching the graph (the LFO's phase and value, the sweep, both channels, the levels), going quiet and then not repainting; `DisplayPlayback` smooth at 1024-frame blocks, wrapping phases, pauses; at 2048-frame blocks the meters still falling at their pace, a fast LFO's comet tail, a random shape's trace started afresh; what a curve costs; the engine having what it set; no QML warnings |
| [test_ui_device_editors_reverb.cpp](../../tests/app/test_ui_device_editors_reverb.cpp) | The Reverb's editor: fitting the view and its own least height with nothing overlapping, every control bound to its parameter (with a tooltip) and undoable, Size read as a bare number and Stereo in whole degrees, the boxes' log drags, dimming; the filter pad's, spin pad's and decay graph's drags (one undo step each, Shift finely, the engine having the values), the pads' handles clear of their captions, the decay graph's handles inside the plot and picked when the shelves cross, its double-click (whose second press, dragged, drags nothing), the Low-pass's and a switched-off shelf's handles moving across only; the curves being the engine's maths (frozen too); the displays reaching them (input level and spectrum, tail meter and spectrum, Spin's phase), held through a tick without values and falling back; the spin pad's trails kept through a sync; the transitions easing (Freeze into the top edge) and everything resting in silence |
| [test_session_devices.cpp](../../tests/app/test_session_devices.cpp) | `DeviceSelection` through the session: selecting, the focus, the clipboard, folding, racks, drops, presets |
| [test_sidechain_fit.cpp](../../tests/app/test_sidechain_fit.cpp) | The Sidechain's fit |
