# UI overview

The UI is Qt Quick: QML in [ui/qml](../../ui/qml) lays out and styles everything, and C++ scene-graph items in
[ui/src](../../ui/src) draw what shows time (the ruler, the lanes, the piano roll, envelopes, waveforms, meters,
the device editors' curves) on the GPU. Together they are the QML module `SUBstation` (the static library `sub_ui`);
the standard controls' look is a Qt Quick Controls style of its own, `SUBstation.Style` ([ui/style](../../ui/style)).
The UI holds no project state: it shows the application layer ([app/src](../../app/src)) and calls it. This page
covers how the UI is put together, the main window and the parts every view shares; the big views have pages of
their own (see [the map](#the-other-ui-pages)).

For what the user sees and does, read the [user guide](../guide/README.md); this page is about how the code does it.

## Files

| Where | What it holds |
|---|---|
| [ui/main.cpp](../../ui/main.cpp) | The `substation` executable: makes the `sub::Engine`, the `sub::app::Session` on it, registers the session for QML, loads `Main.qml`, starts audio and opens the project given on the command line (both on the next turn of the event loop), and shuts the session down after the event loop. |
| [ui/src/Ui.h](../../ui/src/Ui.h) | `setUpApplication()` (the style, the UI font, the palette, the window icon) and `setUpEngine(QQmlEngine&)` (the import path, the icon provider). |
| [ui/src/app/](../../ui/src/app) | `AppTypes`: `registerSession()` makes the session the `Session` singleton and registers the application layer's types QML sees (uncreatable). |
| [ui/src/sg/](../../ui/src/sg) | The scene-graph toolkit: `SgCanvas`, `SgPainter`, `SgTextureCache` ([below](#the-scene-graph-toolkit)). |
| [ui/src/theme/](../../ui/src/theme) | `Theme` (every colour, the fonts, the metrics, the buttons' looks) and `Icons` (vector icons, and the `image://icons` provider). |
| [ui/src/controls/](../../ui/src/controls) | `KnobItem`, `ValueBoxItem`, `Meter`, `OscilloscopeItem`, `DragCursor`. |
| [ui/src/timeline/](../../ui/src/timeline) | `timeline::Timeline` (zoom, scroll, the adaptive grid), `gridLines()`, `labelStep()`, `drawGrid()`, `drawLoopRegion()`: the arrangement's and the piano roll's time axis. |
| [ui/src/mainwindow/](../../ui/src/mainwindow) | `TransportState` (what the transport bar works out), `WindowState` (the window's place and splitters), `FileUrls` (paths and the file dialogs' URLs), `OutsidePresses`, `PagedRows`, `FolderTreeModel` (the browser panel's helpers). |
| [ui/src/platform/](../../ui/src/platform) | `PluginEditorKeys`: the window's shortcuts while a plug-in's editor has the focus (Windows). `WindowFrame`: the main window's title bar instead of the system's caption (Windows). |
| [ui/src/arrangement/](../../ui/src/arrangement), [ui/qml/arrangement/](../../ui/qml/arrangement) | The arrangement view: [arrangement.md](arrangement.md). |
| [ui/src/pianoroll/](../../ui/src/pianoroll), [ui/qml/pianoroll/](../../ui/qml/pianoroll), [ui/qml/clipview/](../../ui/qml/clipview) | The piano roll and the clip view: [piano-roll.md](piano-roll.md). |
| [ui/src/devices/](../../ui/src/devices), [ui/qml/devices/](../../ui/qml/devices) | The device view and the built-in devices' editors: [device-view.md](device-view.md). |
| [ui/qml/browser/](../../ui/qml/browser) | The browser panel: [browser.md](../browser.md). |
| [ui/qml/Main.qml](../../ui/qml/Main.qml), [ui/qml/TitleBar.qml](../../ui/qml/TitleBar.qml), [ui/qml/InfoView.qml](../../ui/qml/InfoView.qml), [ui/qml/Hints.qml](../../ui/qml/Hints.qml) | The main window, its title bar and its info view, which says the tooltips ([below](#the-main-window)). |
| [ui/qml/transport/](../../ui/qml/transport) | `TransportBar.qml`. |
| [ui/qml/dialogs/](../../ui/qml/dialogs) | Preferences (`PreferencesDialog`, `AudioPage`, `MidiPage`, `PluginsPage`), `ExportDialog`, `RenderDialog`, `AboutDialog`, `UnsavedChangesDialog`, and what they share: `MessageBox`, `ChoiceBox`. |
| [ui/qml/](../../ui/qml) (top level) | The shared controls: `Knob`, `ValueBox`, `Oscilloscope`, `RoleButton`, `ToggleButton`, `IconButton`, `Icon`, `ButtonBackground`, `ButtonContent`; `Placeholder`. |
| [ui/style/](../../ui/style) | The style: `ApplicationWindow`, `Button`, `CheckBox`, `ComboBox`, `Dialog`, `DialogButtonBox`, `Frame`, `GroupBox`, `ItemDelegate`, `Label`, `Menu`, `MenuBar`, `MenuBarItem`, `MenuItem`, `MenuSeparator`, `Pane`, `Popup`, `ProgressBar`, `ScrollBar`, `ScrollIndicator`, `SplitView`, `TabBar`, `TabButton`, `TextField`, `ToolButton`, `ToolTip`; the Basic style for the rest. |

Every QML file is part of the module `SUBstation`, so its type name is its file name, unique across folders. A QML
file starting with `pragma Singleton` is registered as a singleton (`DeviceEditors`, `EqWindows`). C++ items are
registered with `QML_ELEMENT` (`QML_SINGLETON` for `Theme`, `Icons`, `FileUrls`, `EqView`). Sources and QML files are
globbed ([ui/CMakeLists.txt](../../ui/CMakeLists.txt)): a new one is picked up by the next build. Every folder under
`ui/src` is an include directory, because Qt's QML registration includes each item's header by its bare name.

## How the UI talks to the rest

```
   user input
       │
       ├──────────────────────────────┐
       ▼                              ▼
  QML (layout, bindings)        C++ items (ui/src, SgCanvas subclasses)
       │ Session.editor.setTempo(...)  │ session()->editor()->moveRange(...)
       │ Session.togglePlay() ...      │ session()->selection(), ->bridge() ...
       ▼                              ▼
  Session (app/src/session) ─ ProjectEditor ─► QUndoCommand ─► Project ─► Qt signals ─┐
       │                                                                              │
       └─ EngineBridge (app/src/audio) ─► sub::Engine (engine/src)                    │
             positionChanged (16 ms), metersUpdated (33 ms),                          │
             automationStateChanged, plugin*, sourceReady ... ────────────────────────┤
                                                                                      ▼
                                   QML bindings re-evaluate, items update() and repaint
```

- **QML reaches the application through the `Session` singleton** (`import SUBstation`): `Session.project`,
  `Session.undoStack`, `Session.editor`, `Session.selection`, `Session.bridge`, `Session.browser`,
  `Session.plugins`, and the session's parts (`arrangement`, `deviceSelection`, `render`, `computerKeyboard`,
  `audioPreferences`, `midiPreferences`). It reads their properties and calls their `Q_INVOKABLE`s: what the
  session is and does is in [app/session.md](../app/session.md). `registerSession()` in
  [AppTypes.cpp](../../ui/src/app/AppTypes.cpp) registers each of their types as uncreatable ("made by the
  session"); a new application-layer type that QML uses is registered there too.
- **C++ items take the session as a property** (`Q_PROPERTY(sub::app::Session* session ...)`, set from QML as
  `session: Session`). They read the model and call the editor, the selection and the bridge in C++: that is where
  mouse gestures live (an item's mouse, hover, wheel and key handlers). Views with state of their own share it in a
  plain `QObject` the items point to: `Arrangement` for the arrangement's items, `PianoRoll` for the piano roll's.
- **Edits go through the editor.** Nothing in the UI changes the `Project` itself: it calls a `ProjectEditor`
  method (`setTrackParam`, `moveRange`, `setClipNotes`, `setDeviceParam`, ...), or the session's or a part's
  action. The editor pushes a `QUndoCommand`; the command changes the project; the project emits a signal
  (`clipsChanged`, `trackChanged`, `devicesChanged`, `automationChanged`, `automationViewChanged`,
  `settingsChanged`, `reset`, ...); the UI repaints from it. See [app/model.md](../app/model.md) and the life of an
  edit in [architecture.md](../architecture.md).
- **No editing logic in QML.** What an action does, which tracks a gesture affects, what a menu holds: C++ (the
  item, or the application layer if it isn't about the view). QML is layout, styling, bindings and calls. Menus
  that depend on the state are worked out in C++ as entries and shown by QML (`MenuEntries` and
  `ArrangementMenu.qml`, [arrangement.md](arrangement.md#menus)).
- **Continuous gestures are one undo step.** `Knob` and `ValueBox` emit `moved(value, gestureKey)`: every value of
  one drag (or a run of wheel notches less than 0.6 s apart, in a `ValueBox`) shares a key, a fresh
  `QUuid` string (`newGestureKey()`), and QML passes it to the editor as the merge key. The items' gestures do the
  same with a key of their own.
- **View state skips undo.** Track heights, folding and which automation lanes show change the project without an
  undo step (they are saved); the zoom and the scroll are the views' own (`Arrangement`, `PianoRoll`) and not saved.
- **Never the engine.** The UI may not include the engine's headers (`ctest -R boundaries` checks it, see
  [building.md](../building.md#the-layers-boundaries)). What only the engine knows comes through the application
  layer's types, whose headers don't include it: the bridge's `position`, `playing`, `meters()`, `chainMeters()`,
  `liveTakes()`, `waveform(path)` (a `Waveform`: frames, channels, peaks), `loadError(path)`, parameter metadata
  (`paramSpec`, `paramGroups`, `canAutomate`, `deviceParams`), automation state (`isAutomated`, `isOverridden`,
  `currentValue`, `ownValue`), processor displays (`readProcessorDisplay`), the scope (`scopeWritten`,
  `scopeSamples`), `cpuLoad`, `deviceStatus`. The bridge polls the playhead every 16 ms and the meters every 33 ms
  and emits `positionChanged` and `metersUpdated`; the UI never waits on the audio thread. See
  [app/engine-bridge.md](../app/engine-bridge.md).
- **Status text.** The session forwards what the bridge, the browser, the plug-in index, the arrangement's and the
  device view's actions, the computer keyboard and the editor's refusals (`refused`) say, as
  `Session.statusMessage`; the views' own `statusMessage` signals (the arrangement view, the device panel, the clip
  view) reach the main window directly. `Main.qml`'s `showMessage()` shows a message for `Session.statusTimeout`
  (8000 ms). What were message boxes come as `Session.warning` and `Session.information`.

## The main window

[Main.qml](../../ui/qml/Main.qml) is an `ApplicationWindow` (1440 × 860 at first):

```
┌───────────────────────────────────────────────────────────────────┐
│ ▮ File Edit … Help      song* - SUBstation      status   ─  □  ✕  │  TitleBar (menuBar)
│ TransportBar (header)                                             │
├────────────┬──────────────────────────────────────────────────────┤
│ Browser-   │ ArrangementView   (or the ClipView covering it)      │
│ Panel      │                                                      │
│ (300 px,   │                                                      │
│  min 120)  │                                                      │
├──────────┬─┴──────────────────────────────────────────────────────┤
│ InfoView │ DevicePanel (as tall as its tallest device needs)      │  bottomRow
└──────────┴────────────────────────────────────────────────────────┘
```

The title bar ([TitleBar.qml](../../ui/qml/TitleBar.qml), the window's `menuBar`, 32 px) holds the application's
icon and the menus on the left, the title (`Session.title`) in the middle of the window (moved aside and elided
when the window is too narrow for it), the status line (`showMessage()`) and a project's plug-ins loading
(*Loading plug-ins: 3 of 12* with a bar) on the right, then minimize, maximize and close. On Windows it is the
window's only title bar: [WindowFrame](../../ui/src/platform/WindowFrame.h), a native event filter, takes the
system's caption away (`WM_NCCALCSIZE`) and keeps the native frame (the shadow, the resizing edges, Aero Snap, the
system menu): `WM_NCHITTEST` makes what has no mouse handling of its own in the title bar (its background, the
title, the status line) the caption, which drags the window, and the maximize button the system's
(`HTMAXBUTTON`, so the snap layouts show over it; `maximizeHovered`/`maximizePressed` draw it). While a popup
shows (a menu open: the overlay is visible) all of it is the window's, so a press there closes the popup. The
frame changes when the native window is made, its client area staying put, so the window's geometry (Qt's is the
client area's) is what it was. Elsewhere `frame.active` is false: the system's title bar stays, with the window's
buttons and the title, and the title bar is a menu bar with the title and the status line.

The body is a vertical `SplitView` (`rows`, 4 px handles) of a horizontal one (`splitter`: the browser and the
arrangement area) over the bottom row (`bottomRow`), which spans the window as in Ableton: the info view at its left
(230 px) and the device view along the rest, its height fixed to the device view's `implicitHeight`. The
arrangement area holds the `ArrangementView` and the `ClipView` on top of each other: clips open in the clip view (a
double-click, Shift+Tab, a new MIDI clip: `Session.arrangement.clipViewRequested`) cover the arrangement until Esc,
× or Shift+Tab go back (`arrangementArea.closeClipView()`, which gives the lanes the keyboard again). View › Browser
(Ctrl+Alt+B), View › Device View (Ctrl+Alt+L: the whole bottom row) and View › Info View show and hide them.

The info view ([InfoView.qml](../../ui/qml/InfoView.qml)) says the tooltips. The style's `ToolTip` (one shared tip
behind every `ToolTip.visible`/`ToolTip.text` and every `RoleButton`'s `tooltip`) asks the `Hints` singleton
([Hints.qml](../../ui/qml/Hints.qml)) whether its item's tip goes there (`routes()`: the info view shows, in the
item's window, and the item isn't in a popup; a dialog's tips pop up as before): then it opens without its delay,
draws nothing and takes no room, and its text is `Hints.text` while it is open. The info view shows the first line as
its title and the rest under it (a single line too long for the title all under it), as a device does (a title bar,
a body), and "Info" while nothing is said. With the info view hidden (or the device view), the tips pop up again.

The window reaches the views only through their interfaces, each call guarded (a `Placeholder` standing in for a
view has none): the arrangement's `zoom(factor)`, `zoomToArrangement()`, `narrowGrid()`, `widenGrid()`,
`openClipView()`, `renameTrack(trackId)`, `focusLanes()`, `snap`, `follow`, `gridStep`, `gridLevel`; the device
panel's `startChainRename(rackId, chainId)` and `focusDevices()`; the browser panel's `startRename(path)` and
`listFocused`.

### Menus and actions

Every menu entry is an `Action` with an `objectName` (for the tests and `PluginEditorKeys`), and calls the session:

| Menu | Entries (shortcut → what it calls) |
|---|---|
| File | New Project (Ctrl+N), Open… (Ctrl+O), Open Recent, Save (Ctrl+S), Save As… (Ctrl+Shift+S), Export Audio… (Ctrl+Shift+R), Quit (Ctrl+Q) |
| Edit | Undo (Ctrl+Z) and Redo (Ctrl+Y, Ctrl+Shift+Z) on `Session.undoStack`, their text following `undoText`/`redoText`; Cut, Copy, Paste, Duplicate (Ctrl+D), Rename (Ctrl+R), Split (Ctrl+E), Consolidate (Ctrl+J), Reverse Clips (R); Freeze / Unfreeze Track (Ctrl+Shift+F), Flatten Track, Delete (Del, Backspace), Select All (Ctrl+A); Re-Enable Automation (enabled while `Session.automationOverridden`), Solo Selected Tracks (S); Play / Stop (Space), Record (F9), Record Quantization (a checkable entry per `Session.recordQuantizeChoices`), Go to Start (Home), Loop (Ctrl+L), Find in Browser (Ctrl+F) |
| Create | Insert Audio Track (Ctrl+T), Insert MIDI Track (Ctrl+Shift+T), Insert Return Track (Ctrl+Alt+T), Insert MIDI Clip (Ctrl+Shift+D, Ctrl+Shift+M: on the arrangement's grid when snapping), Group Tracks (Ctrl+G), Ungroup Tracks (Ctrl+Shift+G), Delete Selected Tracks |
| View | Browser, Device View, Info View, Clip View (Shift+Tab), Automation (A: `editor.toggleAllAutomation()`), Chords and Key (C: `harmony.shown`, checkable), Close Plug-in Editor (Ctrl+W, Windows only), Zoom In (+, =), Zoom Out (-), Zoom to Arrangement (Z), Narrow Grid (Ctrl+1), Widen Grid (Ctrl+2), Snap to Grid (Ctrl+4) |
| Options | Preferences… (Ctrl+,), Rescan Plug-ins, Computer MIDI Keyboard (M), Lock Envelopes |
| Help | About SUBstation |

An `Action` has one shortcut; the others (Ctrl+Shift+Z, Backspace, Ctrl+Shift+M, =, the platform's zoom keys) are
`Shortcut`s that trigger the action. Checkable entries show the model's state: their `checked` is a binding to it
(`Session.project.loopEnabled`, `Session.computerKeyboard.enabled`, ...), and a click asks for the change and then
binds `checked` again, so the menu shows what the model made of it.

Context menus show the same shortcuts as hints only: the window's actions handle the keys.

### What an action acts on

Several Edit and Create commands mean different things depending on what the user is working on. The session
decides (`Session.cut()`, `copy()`, `paste()`, `duplicate()`, `deleteSelection()`, `groupSelected()`,
`ungroupSelected()`; [app/session.md](../app/session.md)), from `Selection.focus` (`Clips`, `Track`, `Devices`,
`Automation`) and what is selected:

| Command | focus `Devices` | lane range with `lanes` | `points` | time range over tracks (clips and their automation) | focus `Track` |
|---|---|---|---|---|---|
| Delete | `deviceSelection.deleteSelected()` | `editor.deleteAutomationRange` | `editor.deleteAutomationPoints` | `arrangement.deleteArea()` | `deleteSelectedTracks()` |
| Cut / Copy | `deviceSelection.cutSelected()` / `copySelected()` | `arrangement.cutAutomation()` / `copyAutomation()` | refused, with a message | `arrangement.cutArea()` / `copyArea()` | `arrangement.cutTracks()` / `copyTracks()` (not returns or the master) |
| Paste | `deviceSelection.paste()` | `arrangement.paste()`: one clipboard for clips, automation and tracks | | | |
| Ctrl+D | `deviceSelection.duplicateSelected()` | `editor.duplicateAutomationRange`, the copy selected | | `arrangement.duplicateArea()` | `arrangement.duplicateTracks()` |
| Ctrl+G / Ctrl+Shift+G | `deviceSelection.groupSelected()` / `ungroupSelected()` | | | | `editor.groupTracks` / `editor.ungroup` |
| R | | | | `arrangement.reverseSelection()` (its audio clips) | |

Ctrl+R asks `Session.renameTarget(browser.listFocused)` what to rename (the preset current in the browser's list
while it has the focus, the rack chain last clicked in the device view, or the track last clicked) and starts the
rename where it shows. The piano roll's note grid takes Delete, Backspace, Ctrl+A, Ctrl+D, Ctrl+U and the arrows
before these actions fire while it has the focus (see [piano-roll.md](piano-roll.md#keys)), and the computer MIDI
keyboard takes its letters while it is on ([below](#the-computer-midi-keyboard)).

### Transport

Play / Stop, Record, the stop button and the rulers' clicks call `Session.togglePlay()`, `toggleRecord()`, `stop()`
and `locate(beat)`; the rules (back to where playback started, as Ableton does; recording from the insert marker
after the count-in, or from the playhead while playing; punch out) are the session's. Go to Start is
`Session.locate(0)`.

### Files and unsaved changes

The session reads and writes projects ([app/serialization.md](../app/serialization.md)); the window asks the user
what it needs:

- `confirmDiscard(then)`: before New, Open, Open Recent and Quit, if `Session.clean` is false, the
  `UnsavedChangesDialog` asks `Session.confirmDiscardText` ("Save changes to the current project?": Save, Discard,
  Cancel). Save saves first and goes on only if it worked; Discard goes on; Cancel (or Esc) stops.
- Save calls `Session.saveProject()`. A project never saved makes the session emit `saveAsRequested`; the window
  then shows the Save As dialog (starting at `Session.suggestedSavePath()`, `<last folder>/Untitled.gilproj`) and
  calls `saveProjectAs(path)`, then runs what was waiting for the save.
- Open… shows a `FileDialog` in `Session.lastFolder` (`files/last_dir`; ~/Music at first) filtered by
  `Session.projectFilter`. Open Recent is filled as it opens, from `Session.recentMenuItems()` ("&1  song.gilproj",
  the path as the tooltip; "No Recent Projects" when empty, and Clear List); a project whose file is gone is taken
  off the list with a warning (`recentProjectAvailable`). The list is `files/recent` in `QSettings`, at most 10,
  compared case-folded.
- Closing the window (Quit, or its close button) first asks `Session.requestClose()`: a render running is cancelled
  instead and the window stays. Then unsaved changes are asked about, and the window's state is saved.
- A project opened shows all of it (`Session.projectOpened` → the arrangement's `zoomToArrangement()`). Its
  plug-ins load after it ([app/engine-bridge.md](../app/engine-bridge.md)); the status line's right end says how far
  they got (`Session.pluginsLoadingText`, "Loading plug-ins: 3 of 12", with a bar) until they all have.
- File › Export Audio… shows the `ExportDialog`; its choice goes through `Session.exportProblem(range)` (an
  information box if there is nothing to export), then a save dialog starting at `Session.suggestedExportPath()`,
  then `Session.exportAudio(path, range, bitDepth)`.

Session warnings and informations queue in one `MessageBox`, shown one after another.

### Renders in the background

Exporting, freezing (Ctrl+Shift+F, the track menus) and reversing long clips render in the background on the
engine's render job ([engine/README.md](../engine/README.md#in-the-background)); the session drives them as state
machines (`session/Renders.h`, no nested event loops) and shows them in `Session.render` (a `RenderProgress`).
[RenderDialog.qml](../../ui/qml/dialogs/RenderDialog.qml) shows while `render.active`: modal, its `title`, its
`label` ("Freezing Bass (2 of 3)…"), a bar at `progress` or a busy bar while `busy` (plug-ins or samples still
loading). The window goes on behind it (it repaints, its meters move, plug-ins waiting to load go on loading) but
takes no edits, its shortcuts included, since the render is of the project as it was when it started. Cancel (the
button or Esc) calls `render.cancel()`: the label says "Cancelling…", Cancel is disabled, and the dialog goes once
the render has stopped. Cancel never takes the keyboard focus, so Space doesn't cancel. When the dialog closes the
window takes the keyboard again.

### Status line

In the title bar, right of the title (there is no status bar): `showMessage()` sets the title bar's `message`, shown
in `TEXT_DIM`, right-aligned against the window's buttons and elided (nothing at first; `messageTimer` clears it
after `statusTimeout`). After it, the plug-ins loading, shown while `Session.pluginsTotal` > 0. Its text, like the
title, drags the window.

### Window state

[WindowState](../../ui/src/mainwindow/WindowState.h) keeps the window between runs in `QSettings`:

| Key | What |
|---|---|
| `window/geometry` | The window's normal geometry (as last seen neither maximized nor full screen) and whether it was maximized. A saved place no screen shows any more is moved onto the primary screen. |
| `window/splitter` | The browser's split (`SplitView.saveState()`). |
| `window/device_splitter` | The browser and arrangement's over the bottom row (the info view and the device view). |

They are restored when the window is made and saved when it closes. What a widget version of the program saved
under those keys (`QWidget::saveGeometry`'s bytes, a `QSplitter`'s state) is ignored: the window starts at its
default size.

## The scene-graph toolkit

Everything that draws time is an [SgCanvas](../../ui/src/sg/SgCanvas.h): a `QQuickItem` whose subclass implements
`paint(SgPainter&)` the way a widget implemented `paintEvent` with a `QPainter`, and calls `update()` when it must
be drawn again. `updatePaintNode()` runs `paint()` and turns what it drew into scene-graph nodes, reusing the last
frame's nodes and buffers.

- **Threading.** `paint()` runs on the scene graph's render thread, during the sync step, while the GUI thread is
  blocked. It may read anything (the item's state, the application layer's models) but must change nothing: no
  property writes, no signals, no JavaScript, no `QObject`s made. What to draw is worked out on the GUI thread (in
  setters, slots, `updatePolish()`) and kept in members; `paint()` only reads them. The arrangement's lanes, for
  instance, work out the envelopes showing and how each looks (`EnvelopeLook`, from the bridge) in
  `updatePolish()`. The one exception is the arrangement's `WaveformCache`, which makes tiles as they are first
  drawn, behind a mutex.
- **Batching.** [SgPainter](../../ui/src/sg/SgPainter.h) records solid geometry (rects, lines, polygons, arcs,
  waveform columns) into as few vertex-coloured triangle nodes as it can, and text and images as textured nodes
  between them, in paint order. A frame whose vertices didn't change uploads nothing. Tens of thousands of rects a
  frame are fine (`test_ui_sg.cpp` benchmarks an arrangement's worth).
- **The 65532-vertex split.** The scene graph's renderer silently draws nothing of a node with more than 65535
  vertices (an EQ's two dozen curves made one). `SgPainter` closes a solid batch at `kMaxSolidVertices` (65532, a
  whole number of rects and triangles) and goes on in a new node.
- **Text and images** are textures from the window's [SgTextureCache](../../ui/src/sg/SgTextureCache.h): a text is
  rendered with `QPainter` once per (text, font, colour, layout, device pixel ratio), an image once per
  `QImage::cacheKey()`; at most 4096 entries and 64 MB, least recently used first out. An entry stays alive while a
  node still draws it, and the whole cache goes when the window's scene graph is invalidated. `drawText(rect,
  flags, ...)` clips to the rect unless `Qt::TextDontClip`, as `QPainter` does.
- **Clipping** is `SgPainter::setClipRect` (rectangles, intersected; done on the CPU, so it never splits a batch).
  Don't set QML's `clip: true` on parts of a canvas: it breaks batching. But a canvas draws where its geometry
  says, unclipped: what it scrolls past its edges (a selected range, notes, the playhead) would show over its
  neighbours, the browser among them. So a canvas whose content scrolls (the arrangement's ruler, lanes and bus
  lanes, the piano roll's ruler, keys, notes and velocities, the clip view's waveforms) has `clip: true` itself, a
  scissor for it and the items in it (its playhead).
- **Differences from `QPainter`**: no pen or brush state (each call takes its colour, a pen's width and cap);
  antialiasing is state (`setAntialiasing`, off by default) and feathers edges with a 1-pixel ramp; transforms are
  translations only; `drawArc` takes degrees (0 at 3 o'clock, counter-clockwise). Without antialiasing, lines and
  outlines land on whole pixels as `QPainter`'s aliased drawing puts them, so pixel-exact `QPainter` code looks the
  same; `fillRect`, `fillColumns` and `fillToBaseline` are never antialiased.
- **Per-frame things are items of their own.** Everything an item draws is drawn again when it repaints, so what
  changes every frame sits in a separate item above (or below) the static part: the playhead
  (`ArrangementPlayhead`, `RollPlayhead`), the takes being recorded (`LiveTakes`), meters. Following playback then
  redraws a line, not the lanes.

`SgCanvas::lastStats()` says what the last frame drew (solid and texture nodes, vertices, paint and build time,
nodes made): the tests read it.

## Theme, style and icons

[Theme](../../ui/src/theme/Theme.h) is the one source of every colour and font: `Theme::kLane` in C++,
`Theme.lane` in QML (a `QML_SINGLETON`). Base colours (`kWindow`, `kPanel`, `kSurface`, `kBorder`, `kText`,
`kTextDim`, `kAccent`, ...), the arrangement's (`kLane`, `kGridBar`, `kPlayhead`, `kInsertMarker`, `kSelection`,
`kWaveform`, ...), the piano roll's (`kKeyWhite`, `kBlackKeyRow`, `kOutsideClip`, ...) and the controls'
(`kActivatorOn`, `kSoloOn`, `kMeterLow`, `kScopeLine`, `kFrozen`, `kAutomationOn`, ...). Fonts: `uiFont(pt, bold)`
(Segoe UI, 9 pt by default), `monoFont(pt)` (Consolas), and for QML `Theme.font`, `Theme.smallFont` (8 pt, the
buttons with a role), `Theme.listFont` (10 pt: the browser's lists, as Qt Quick draws small text smaller than the
widgets did at the same size) and `Theme.listHeadingFont` (8.5 pt bold, the sidebar's headings), `Theme.uiFont()`. Metrics: `radius` (3), `controlHeight` (28, the transport bar's boxes and
buttons), `scrollBarWidth` (12), `iconSize` (14). `setUpApplication()` applies the font and a palette from these
colours.

Buttons are coloured by their **role**, as the old stylesheet's `QPushButton[role=...]` rules did:
`Theme::buttonLook(role, hovered, pressed, checked, enabled)` (`Theme.buttonStyle(...)` in QML) works out the
cascade for "" (plain), `activator`, `solo`, `play`, `record`, `arm`, `re-enable`, `tool`, `flat`, `small` and
`device-header`: background, text colour, border, radius, padding, minimum size, point size and weight.

The style ([ui/style](../../ui/style), module `SUBstation.Style`, selected by `setUpApplication()`) draws the
standard controls (buttons, check boxes, combo boxes, menus, the menu bar, scroll bars, split views, tab bars, text
fields, tool tips, dialogs, progress bars) as the old stylesheet did, from `Theme`; anything else falls back to the
Basic style. A style `Button` is a `RoleButton` that takes the focus, as a dialog's buttons do.

[Icons](../../ui/src/theme/Icons.h) are small vector drawings on a 64 × 64 grid, drawn with `QPainter` at the size
asked for, so they stay sharp at any scale and pixel ratio: play, stop, record, metronome, loop, follow,
re_enable_automation, lock_envelopes, headphones, folder, waveform, plugin, preset, plugin_window, sidechain,
snowflake, save, link, infinity, expand, sliders, fold, search, app_icon. QML gets them from the image provider,
`image://icons/<name>[?color=%23rrggbb][&state=on][&mode=disabled]` (`Icons.url()` builds it; `Icon { name; color;
checked; size }` wraps it): `state=on` picks the On picture of `lock_envelopes` (closed) and `fold` (pointing right:
folded), `mode=disabled` draws it in `kTextDisabled`. C++ items draw them with `Icons::image()` on the GUI thread
and keep the image. Add an icon by adding its drawing to the table in [Icons.cpp](../../ui/src/theme/Icons.cpp).

## Shared controls

| Control | Where | Notes |
|---|---|---|
| `Knob` | [Knob.qml](../../ui/qml/Knob.qml) over [KnobItem](../../ui/src/controls/KnobItem.h) | Drag vertically: 600 px for the whole range (`kDragPixels`), 6000 with Shift (`kFineDragPixels`); pressing or letting go of Shift mid-drag changes the rate from there on. The wheel moves 1/50 of the range a notch (`kWheelNotches`), unless `wheel` is false (the event goes on to a scrolling list). Double-click resets to `defaultValue` (if never set: the value it was made with). `logScale` moves evenly in log(value) (when `from` > 0); `step` takes multiples of it from `from`; `bipolar` draws the arc from the middle. With a `parser`, typing a digit (the knob has the focus after a click) opens a small text field over it. `automation` "on" or "off" draws the dot (red automated, grey overridden; `drawAutomationDot()`, shared with the value box). `moved(value, gestureKey)` comes for user changes only, never for `value` set from outside; `touched()` on every left press or double-click; `relative` says whether the last change was a drag or wheel (rather than typed or reset). |
| `ValueBox` | [ValueBox.qml](../../ui/qml/ValueBox.qml) over [ValueBoxItem](../../ui/src/controls/ValueBoxItem.h) | Ableton-style number: drag vertically, 0.25 steps a pixel (`kDragRate`), 0.025 with Shift; the wheel moves ten steps a notch, notches less than 0.6 s apart being one gesture. `choices` limits it to a list (the time signature's denominator: one choice per 10 px, or a notch). `logScale` drags and wheels in log(value) as a knob does. Double-click: with a `defaultValue` it resets (and typing a digit edits); without one it opens the text field with the value selected. `parseNumber` strips "dB", "bpm" and "%" and reads "-inf" as -70. Values are rounded to `decimals` and kept between `from` and `to`. Same signals as the knob. |
| `Meter` | [Meter](../../ui/src/controls/Meter.h) | Stereo peak meter from -60 to +6 dB, falling 3.5 % of the scale per update (`setLevels()` about 30 times a second, from the bridge's `metersUpdated`; the fall needs every update); a clip light at full scale, cleared by a click. |
| `Oscilloscope` | [Oscilloscope.qml](../../ui/qml/Oscilloscope.qml) over [OscilloscopeItem](../../ui/src/controls/OscilloscopeItem.h) | [Below](#the-oscilloscope). |
| `RoleButton` | [RoleButton.qml](../../ui/qml/RoleButton.qml) | A push button coloured by its `role`, with `iconName`, `iconColor`, `iconSize`, `tooltip` and `lit`. It never takes the keyboard focus, so Space stays play/stop. |
| `ToggleButton` | [ToggleButton.qml](../../ui/qml/ToggleButton.qml) | A checkable `RoleButton`; `toggled()` and `clicked()` come from the user only, and `setCheckedSilently()` (or a binding) shows state from the model without them. Play and Record are `checkable: false` with `checked` bound to the bridge: their state follows the engine, never the click. |
| `IconButton` | [IconButton.qml](../../ui/qml/IconButton.qml) | A `RoleButton` with an icon only, role `tool` unless set. |
| `ChoiceBox` | [ChoiceBox.qml](../../ui/qml/dialogs/ChoiceBox.qml) | A combo box over the application layer's `[{label, value, enabled, toolTip}]` lists: shows `chosenIndex` (the model's), says what the user picked with `chosen(index)`, then shows the model's choice again. Never takes the focus. |

While a knob or value box is dragged, [DragCursor](../../ui/src/controls/DragCursor.h) hides the mouse cursor (from
the first move), jumps it back to the middle at the top or bottom of the screen so a drag never runs out of room,
and puts it back where the drag started. A popup opening mid-drag (a right-click menu) ends the drag; an item also
ends it when it loses the mouse grab.

### The oscilloscope

The scope in the transport bar shows the master output, as FL Studio's does. Its feed is any `QObject` with a
`quint64 scopeWritten` property (samples written so far) and `Q_INVOKABLE QList<float> scopeSamples(int frames)`:
the bridge. Every 16 ms (`kUpdateMs`) its timer reads `scopeWritten`; only when it moved does it fetch
`scopeSamples(2 * kWindow)` (neither blocks). `trigger()` finds the last rising zero crossing that still leaves a
full `kWindow` (1024 samples, about 21 ms at 48 kHz) after it, so steady tones stand still; with none it shows the
newest window. `trace()` turns the samples into one column per pixel (the column's highest then lowest sample),
built when the samples change, not on every paint. When no new audio comes the trace shrinks by
`kFadePerUpdate` (0.8 per update, 0.64 per 33 ms) and stops repainting once flat. It skips its work while hidden.

`kWindow` is a fixed number of samples because the engine's sample rate is behind its edit lock, which the scope
shouldn't take 60 times a second. The glow is drawn without antialiasing (it is soft anyway); only the thin line has
it.

## Transport bar

[TransportBar.qml](../../ui/qml/transport/TransportBar.qml), 40 px high, every box, button and the scope
`Theme.controlHeight` (28 px) tall; nothing in it takes the keyboard focus. What needs working out is
[TransportState](../../ui/src/mainwindow/TransportState.h)'s. Left to right:

- **Tempo** (`ValueBox`, 20 to 999 BPM, 0.25 a step) → `editor.setTempo(value, gestureKey)`. **Time signature**: two
  value boxes, the numerator 1 to 32, the denominator from `TransportState.denominators` →
  `editor.setTimeSignature(n, d)`.
- **Metronome** → `bridge.metronome`. **Project key** (a `ChoiceBox` of "No Key" and every key,
  `TransportState.keys`) → `editor.setKeyByName(name)`: audio added with a key in its name is transposed to it (see
  [guide/audio-clips.md](../guide/audio-clips.md)).
- **⌨** toggles the computer MIDI keyboard (`Session.computerKeyboard`; its tooltip says which note A plays).
- **Position**: `TransportState.positionText` ("  1. 1. 1", bar right-aligned in three places), changed from the
  bridge's `positionChanged` only when the text changes.
- **Play**, **Stop**, **Record** → `Session.togglePlay()`, `stop()`, `toggleRecord()`; Play's and Record's lit state
  is the bridge's `playing` and `recording`.
- **Count-in** (`ChoiceBox` of `Session.countInChoices`: none, 1, 2 or 4 bars) → `Session.countInBars`, kept in
  `QSettings` (`transport/count_in_bars`).
- **Re-Enable Automation**: enabled and lit while `Session.automationOverridden` → `bridge.reEnableAutomation()`.
- **Lock Envelopes** → `editor.setAutomationLocked` (saved with the project).
- **Oscilloscope** (150 × 30, fed by `Session.bridge`). **Loop** → `editor.setLoopEnabled`. **Follow** → the
  arrangement view's `follow`.
- **CPU**: `TransportState.cpuText` ("CPU 12%"), from `bridge.cpuLoad` every 15th meter update (`kCpuEvery`, about
  twice a second). **Device**: `deviceText` ("Speakers · 48 kHz", the name cut to 28 characters; "No audio device"),
  with a tooltip; a click opens Preferences.

## Dialogs

[ui/qml/dialogs](../../ui/qml/dialogs). Each is a QML `Dialog` (modal, centred on the window's overlay) over an
application-layer object that holds the logic. What the preferences mean to the user:
[guide/audio-setup.md](../guide/audio-setup.md).

- **Preferences** (`PreferencesDialog`: Options › Preferences…, Ctrl+, or a click on the device's name): a tab per
  page and Close. Changes apply at once. Opening it calls `Session.audioPreferences.open()` and
  `Session.midiPreferences.open()`; closing it `audioPreferences.close()`.
  - **Audio** (`AudioPage` on [AudioPreferences](../../app/src/session/AudioPreferences.h)): a `ChoiceBox` per
    setting, bound to the controller's choices and index (`driverChoices`/`driverIndex`, `deviceChoices`,
    `outputChoices` (ASIO), `sampleRateChoices`, `bufferChoices`, `threadChoices`), and calling its `choose...(i)`;
    Exclusive mode (WASAPI only, `exclusiveVisible`); Hardware Setup (ASIO), which disables the dialog while the
    driver's panel runs (it may run a message loop of its own); the status (rich text: what runs, or why it didn't
    open). A driver this build lacks (ASIO without the SDK) is greyed out with a tooltip saying so. Each choice
    reopens the device, since what a device offers is only known while it is open; only settings that opened are
    saved.
  - **MIDI** (`MidiPage` on [MidiPreferences](../../app/src/session/MidiPreferences.h)): a check box per input
    (`inputs`: name, enabled, error: an input that couldn't be opened shows in red, why in its tooltip) →
    `setInputEnabled(name, on)`; Refresh → `refresh()`.
  - **Plug-ins** (`PluginsPage` on `Session.plugins`, the `PluginIndex`): the standard VST3 folders (dimmed, always
    searched) and the user's own (red if missing); Add Folder… and Remove (only new files are read; a removed
    folder's plug-ins leave the browser); Rescan Plug-ins; the scan's status, its failures in the tooltip. See
    [app/plugin-scanner.md](../app/plugin-scanner.md).
- **Export Audio** (`ExportDialog`): the range (`Session.exportRangeChoices()`: the arrangement, or the loop region
  while the loop is on and has a length) and the bit depth (`exportBitDepthChoices`: 16-bit, 24-bit, 32-bit float;
  24 at first); OK emits `exportChosen(range, bitDepth)` and the window goes on ([above](#files-and-unsaved-changes)).
- **Render progress** (`RenderDialog`): [above](#renders-in-the-background).
- **About** (`AboutDialog`): a `MessageBox` with the application's icon, `Session.aboutTitle` and
  `Session.aboutText`.
- **Unsaved changes** (`UnsavedChangesDialog`): Save, Discard, Cancel ([above](#files-and-unsaved-changes)).

`MessageBox` is the message box all of them share: a title, an icon ("information", "warning", "question",
"about"), rich text, and the buttons as `choices` (`[{text, value}]`); `answered(value)` says which was pressed
(Return: `defaultValue`, Esc: `escapeValue`).

## The computer MIDI keyboard

[ComputerKeyboard](../../app/src/session/ComputerKeyboard.h) is the application layer's
(`Session.computerKeyboard`): it needs no Qt Quick, only `QGuiApplication`'s key events. Behaviour:
[guide/midi.md](../guide/midi.md).

It installs itself as an event filter on the whole application, so it sees keys wherever they go, before any item
or shortcut does.

- `noteOffset()` maps A W S E D F T G Y H U J K O L P ; ' to semitones 0 to 17 above the octave's C; `octaveStep()`
  maps Z and X to -1 and +1. `kDefaultOctave` 5 puts C3 (note 60) on A; `kMaxOctave` is 9.
- A note is `bridge.sendMidi({0x90, note, 100}, kComputerKeyboard)`: it arrives as one more MIDI input, "Computer
  Keyboard", which tracks on *All Ins* hear too. `held_` maps each key to the note it started, so a note ends right
  even if the octave changed while it was held.
- While it is on, the `ShortcutOverride` of one of its keys is accepted, so Qt delivers the key as a key press
  instead of firing the window's shortcut (S, A, Z). Keys with modifiers (other than the keypad flag) are left alone,
  and so are keys typed into a text input: the object with the focus answers `Qt::ImEnabled` to an input method
  query, as Qt Quick's text fields do (`focusTakesText()`).
- A key event reaches an application event filter once for each object it is delivered to; it is taken the first
  time, and auto-repeats are ignored. Key-ups always go through for held keys, even if a modifier was pressed
  meanwhile.
- Turning it off, or the application losing the keyboard (`applicationStateChanged`), releases every held note.
- `takesKey()` tells [PluginEditorKeys](#shortcuts-from-plug-in-editors) which keys it plays.

## Shortcuts from plug-in editors

[PluginEditorKeys](../../ui/src/platform/PluginEditorKeys.h) (in Main.qml, `target: window`). The rules as the user
sees them: [guide/shortcuts.md](../guide/shortcuts.md).

Plug-in editors are plain Win32 windows (the engine's `EditorWindow.cpp`, window class `SUBstationPluginEditor`; see
[engine/plugins.md](../engine/plugins.md)), so Qt never sees their keys as key events. Their messages still pass
through Qt's event loop, so `PluginEditorKeys`, a `QAbstractNativeEventFilter` installed on the application on
Windows only (`supported`), sees each `WM_KEYDOWN` / `WM_SYSKEYDOWN`:

1. Is the window (or its root ancestor) a plug-in editor? If not, Qt handles it as usual.
2. `actionFor(virtualKey, modifiers, textField)` maps the virtual-key code to a Qt key (`qtKey`: letters, digits,
   F-keys and a few others), reads the modifiers with `GetKeyState`, and decides:
   - Without Ctrl or Alt only Space and S are taken, and not with Shift, not while the focus is in a Windows text
     field (an `Edit` or `RichEdit` class), and not a key the computer MIDI keyboard plays while it is on.
   - Ctrl+A/C/V/X/Z/Y and Ctrl+Shift+Z always stay with the plug-in: it may be typing into a field of its own.
   - Otherwise the window's first enabled `Action` or `Shortcut` whose key sequence matches exactly (read from what
     QML holds: a string, a `StandardKey`, a `QKeySequence`, or a list of them).
3. `keyPressed()`: if there is one, the message is swallowed and the action triggered (a `Shortcut`'s `activated`),
   except for the repeats (lParam bit 30) of keys held without Ctrl or Alt: Space held down acts once. While a
   render's dialog is up the window takes no keys, so the plug-in keeps them.

Posting the key to the main window instead wouldn't do: Qt Quick's shortcuts only fire in the window that has the
focus. `closeForemostEditor()` (Ctrl+W, *View › Close Plug-in Editor*, shown on Windows only) walks the top-level
windows with `EnumWindows`, top down, and posts `WM_CLOSE` to the first visible editor of this process. The rules
are plain code, so the tests run `actionFor()` and `keyPressed()` on any platform.

## Gotchas

- **`paint()` reads only.** It runs on the render thread while the GUI thread waits; changing anything there
  (a property, a signal, a cache without a lock) is a data race. Work things out on the GUI thread and call
  `update()` (or `polish()`, for `updatePolish()`).
- **Never let a control take the focus** in the views: Space is the window's play/stop shortcut. `RoleButton`,
  `ChoiceBox` and the scroll bars use `Qt.NoFocus`.
- **Show model state without editing it.** A control showing the model binds to it; its user signal (`moved`,
  `toggled`, `chosen`, an `Action`'s `triggered`) is what edits. After a user change, bind again (`checked =
  Qt.binding(...)`), so it shows what the model made of it.
- **Menus made in JavaScript need an owner.** Items created without a parent are taken by the garbage collector, a
  submenu even while it shows: `ArrangementMenu` and `PanelMenu` keep what they made until the next `show()`, and
  `ActionMenu` and Open Recent give theirs the menu's `contentItem` as parent.
- **The second press of a double-click.** Qt Quick delivers it as a press as well as the double-click (widgets got
  only the double-click). The arrangement's and the piano roll's items ignore a press flagged
  `Qt::MouseEventCreatedDoubleClick` (the double-click stands for it); the device view's frames and editors work it
  out from the presses' times and places (`secondPressOfDoubleClick()`). Where each click of a double-click
  counts (automation lanes, fold buttons), the double-click starts the gesture again.
- **Losing the mouse grab ends a gesture.** A popup opening mid-drag takes the mouse: items end their gesture in
  `mouseUngrabEvent()`, where what it previewed goes.
- **Native event filters see every message.** `PluginEditorKeys::nativeEventFilter` runs for every message of the
  process: what it does for messages that aren't key presses in an editor stays cheap.
- **Raw strings and moc.** moc (Qt 6.4) stops at a raw string literal, so the tests' inline QML goes after the test
  class (see [building.md](../building.md#gotchas)).

## Tests

The UI tests are Qt Test programs, `tests/app/test_ui_*.cpp`, one executable each, linked with the UI. They load
QML on a real `Session` (no audio device) with [UiTestSupport.h](../../tests/app/support/UiTestSupport.h) and drive
it with mouse, wheel and key events as a user would. They run on a display (xvfb with the xcb platform here: the
offscreen platform renders Qt Quick in software, without the items' geometry); with `SUBSTATION_UI_SCREENSHOTS` (or
`SUBSTATION_SCREENS`) set they save screenshots there. See [testing.md](../testing.md).

| Test file | Covers here |
|---|---|
| [test_ui_mainwindow.cpp](../../tests/app/test_ui_mainwindow.cpp) | The layout and its look, every menu action calling the session, the shortcuts, checkable actions kept in step with the model, Open Recent, files with the unsaved-changes question, closing (a render running, unsaved changes), the title, the status line, warnings, Export Audio, the clip view covering the arrangement, Rename, `PluginEditorKeys`, the window state kept |
| [test_ui_transport.cpp](../../tests/app/test_ui_transport.cpp) | Each control of the transport bar and what it shows, whoever changed it; Play and Record following the bridge; nothing taking the keyboard |
| [test_ui_dialogs.cpp](../../tests/app/test_ui_dialogs.cpp) | Preferences' pages on their controllers, Export Audio's choices, the render progress (modal, label, bar, Cancel) while exporting and freezing, the message boxes |
| [test_ui_controls.cpp](../../tests/app/test_ui_controls.cpp) | Knobs and value boxes dragged (one gesture key per drag), wheeled, double-clicked and typed into; the meter's fall and clip light; the oscilloscope's trigger and fade from a fake feed; buttons that never take the focus |
| [test_ui_sg.cpp](../../tests/app/test_ui_sg.cpp) | `SgCanvas` and `SgPainter` rendered for real and checked pixel by pixel; the big-recording split; a benchmark of an arrangement's worth of rects |
| [test_ui_theme.cpp](../../tests/app/test_ui_theme.cpp) | Every colour with its value, the button roles' looks, every icon (its colour, states, the disabled variant) and the image provider |
| [test_ui_gallery.cpp](../../tests/app/test_ui_gallery.cpp) | The look end to end: a gallery of the shared controls and a sample of what the views draw |
| [test_session_keyboard.cpp](../../tests/app/test_session_keyboard.cpp) | The computer MIDI keyboard: notes and octaves, shortcuts and text inputs keeping their keys, modifiers, releasing held notes |
| [test_session_renders.cpp](../../tests/app/test_session_renders.cpp), [test_session_files.cpp](../../tests/app/test_session_files.cpp) | The session's side: renders in the background and Cancel, closing while one runs, plug-ins loading after a project opens; files, recent projects, the preferences |

## The other UI pages

| Page | Covers |
|---|---|
| [arrangement.md](arrangement.md) | `ui/src/arrangement`, `ui/qml/arrangement`: the `Arrangement` state, the layout, the ruler, the lanes and their gestures, track, return and master headers, automation lanes and choosers, waveforms |
| [piano-roll.md](piano-roll.md) | `ui/src/pianoroll`, `ui/qml/pianoroll`, `ui/qml/clipview`: the piano roll's keys, ruler, note grid, velocity lane and note tools, and the clip view |
| [device-view.md](device-view.md) | `ui/qml/devices`, `ui/src/devices`, the device selection: the device view, racks, the built-in devices' editors |
| [../browser.md](../browser.md) | `ui/qml/browser`, the application layer's browser and its native backend |
