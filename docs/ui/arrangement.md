# Arrangement view

The arrangement view is the timeline: a ruler on top, the track lanes with their headers on the right (as in
Ableton), the return tracks and the master pinned at the bottom, and the automation lanes. Its layout is QML in
[ui/qml/arrangement](../../ui/qml/arrangement); what it draws and how it is edited are C++ scene-graph items in
[ui/src/arrangement](../../ui/src/arrangement), sharing one `Arrangement` (the view's state). What acts on the
selection without being about the view (the clipboard, deleting, duplicating, pasting, drops) is the application
layer's `ArrangementActions` (`Session.arrangement`, [app/session.md](../app/session.md)).

What the user does with it: [guide/arrangement.md](../guide/arrangement.md),
[guide/automation.md](../guide/automation.md), [guide/mixing.md](../guide/mixing.md),
[guide/recording.md](../guide/recording.md). This page is about the code.

## Files

| File | What it holds |
|---|---|
| [ArrangementView.qml](../../ui/qml/arrangement/ArrangementView.qml) | The layout: the ruler, the grid's corner, the lanes and the headers' column, the returns' rows, the master's, the scroll bars; its interface to the main window |
| [Arrangement](../../ui/src/arrangement/Arrangement.h) | The view's state (the old `ArrangementView` and `ViewState`): the time axis (a `timeline::Timeline`: zoom, scroll, the adaptive grid, snapping), following the playhead, the `TrackLayout` and the row models QML lays out, the scroll bars, the playhead, dragging headers to move tracks, Alt+wheel resizing, Ctrl+R |
| [TrackLayout](../../ui/src/arrangement/TrackLayout.h) | `TrackLayout`, `Row`, `LaneRow`, `AutomationRows`, `automationRows()`, `returnRows()`, `masterRows()`; the layout constants |
| [RowModels](../../ui/src/arrangement/RowModels.h) | `TrackRowModel` (a header per track, where it sits) and `ReturnRowModel` (a lane and a header per return) |
| [Timeline](../../ui/src/timeline/Timeline.h) (`ui/src/timeline`) | `timeline::Timeline`, `gridLines()`, `labelStep()`, `drawGrid()`, `drawLoopRegion()`: shared with the piano roll |
| [ArrangementItem](../../ui/src/arrangement/ArrangementItem.h) | The base of the drawn parts: `session` and `arrangement` properties, `repaint()` |
| [ArrangementRuler](../../ui/src/arrangement/ArrangementRuler.h) | The loop brace and the scrub area |
| [ArrangementLanes](../../ui/src/arrangement/ArrangementLanes.h) | The track lanes: painting clips, group summaries, the selection, markers; hit-testing; mouse, wheel, keys, context menus, drops |
| [ClipGestures](../../ui/src/arrangement/ClipGestures.h), [Gesture.h](../../ui/src/arrangement/Gesture.h) | The clip gestures: `MoveRangeGesture`, `TrimGesture` (both heard as they drag), `TimeSelectGesture`, `PanGesture`; the `Gesture` base |
| [Envelopes](../../ui/src/arrangement/Envelopes.h) | Automation lanes drawn and edited: `EnvelopeArea`, `Hover`, `EnvelopeLook`, `LanesHost`, and in `envelopes::` hit-testing, `trace()`, `press()`, `hover()`, the menu, drawing; the automation gestures |
| [BusLane](../../ui/src/arrangement/BusLane.h) | The lane of a strip without clips: a return's or the master's |
| [LiveTakes](../../ui/src/arrangement/LiveTakes.h), [ArrangementPlayhead](../../ui/src/arrangement/ArrangementPlayhead.h) | The takes being recorded and the playhead: items of their own over the lanes |
| [TrackHeaderItem](../../ui/src/arrangement/TrackHeaderItem.h) | A strip's header (track, group, return or master): what it paints, its mouse handling, what its controls show and do, its menus |
| [TrackHeader.qml](../../ui/qml/arrangement/TrackHeader.qml), [ReturnHeader.qml](../../ui/qml/arrangement/ReturnHeader.qml), [MasterHeader.qml](../../ui/qml/arrangement/MasterHeader.qml) | The headers' controls laid out over a `TrackHeaderItem` |
| [SendKnobs.qml](../../ui/qml/arrangement/SendKnobs.qml), [AutomationChoosers.qml](../../ui/qml/arrangement/AutomationChoosers.qml), [AutomationChooser.qml](../../ui/qml/arrangement/AutomationChooser.qml) | A header's send knobs and automation choosers |
| [GridInfo.qml](../../ui/qml/arrangement/GridInfo.qml) | The corner showing the grid's size; a click toggles snapping |
| [MenuEntries](../../ui/src/arrangement/MenuEntries.h), [ArrangementMenu.qml](../../ui/qml/arrangement/ArrangementMenu.qml), [ArrangementMenuItem.qml](../../ui/qml/arrangement/ArrangementMenuItem.qml) | Menus worked out in C++ and shown by QML |
| [WaveformCache](../../ui/src/arrangement/WaveformCache.h) | Waveform tiles: columns per pixel, an LRU of them |
| [Cursors](../../ui/src/arrangement/Cursors.h) | The bracket cursors for trimming and the add-a-breakpoint cursor |

## Layout

```
 col 0 (stretches)               col 1 (252 px)       col 2
┌──────────────────────────────┬────────────────────────────┐
│ ArrangementRuler (40)        │ GridInfo        (spans 1-2)│
├──────────────────────────────┼───────────────────┬────────┤
│ ArrangementLanes             │ track headers     │ vbar   │
│  (+ LiveTakes, playhead)     │                   │ (rows  │
├──────────────────────────────┼───────────────────┤  1-3)  │
│ a BusLane per return         │ ReturnHeaders     │        │
├──────────────────────────────┼───────────────────┤        │
│ BusLane (the master's)       │ MasterHeader      │        │
├──────────────────────────────┼───────────────────┴────────┘
│ hbar                         │
└──────────────────────────────┘
```

Every drawn part is given `session: Session` and `arrangement: arrangementState` (the view's one `Arrangement`).
The lanes and the header column scroll together: they share the `Arrangement`'s `scrollY`, and each header (a
`Repeater` over `arrangement.rows`) sits at `model.top - arrangement.scrollY`, hidden while its track is in a folded
group. The returns and the master don't scroll vertically: they are columns of fixed-height rows whose heights follow
their automation lanes (`arrangement.returns`, `masterHeight`). Below the headers, a click selects no track and the
wheel scrolls them with the lanes.

A scene-graph item draws where its geometry says, unclipped, so whatever scrolls past an item's edge would show over
its neighbours (the browser, left of the arrangement in the main window, among them): the ruler, the lanes and the
bus lanes have `clip: true` (a scissor each, for them and the playheads in them), and the header column clips its
headers.

The clip view is not part of this view: the main window shows it over the arrangement
([README.md](README.md#the-main-window)).

For the main window the view offers `zoom(factor)`, `zoomToArrangement()`, `narrowGrid()`, `widenGrid()`,
`openClipView()`, `renameTrack(trackId)`, `focusLanes()`, the properties `snap`, `follow`, `gridStep` and
`gridLevel`, and the signal `statusMessage(text)`. Clips double-clicked open through
`Session.arrangement.clipViewRequested`; a click on the ruler plays from there through `Session.locate(beat)`.

The scroll bars are views of the `Arrangement`, not the other way round: `updateHBar()` sets the horizontal range
from the content's end (the last clip, the loop's end, or the visible width, plus 16 bars) and `updateVBar()` the
vertical one from `TrackLayout::totalHeight()` plus `kDropZone` (120 px of empty space below the tracks for dropping
files). A bar's `position` is bound to the state except while the user drags it; then it calls `scrollToX()` /
`scrollToY()`.

## Arrangement and coordinates

[Arrangement](../../ui/src/arrangement/Arrangement.h) holds a [timeline::Timeline](../../ui/src/timeline/Timeline.h)
(`pxPerBeat` 0.25 to 4000, 24 at first; `scrollBeats`, `scrollY`, `maxScrollY`, `gridLevel`, `snap`) and `follow`.
It emits `viewChanged` (zoom or horizontal scroll), `vscrollChanged`, `gridChanged`, `layoutChanged`,
`playheadChanged`, `scrollBarsChanged`.

- `beatToX(beat) = (beat - scrollBeats) * pxPerBeat`; `xToBeat` is the inverse. Time is in beats everywhere in the
  UI; seconds only come in through the tempo.
- `framesPerPixel(sampleRate, sourceTempo)`: source frames per pixel for a waveform, at the clip's own tempo (a
  warped clip's segment BPM) or the project's.
- `zoomAt(x, factor)` keeps the beat under `x` in place. `zoom(factor)` (+ and -) zooms around the playhead if it
  shows, else around the middle. `zoomToArrangement()` (Z, and every project opened) fits the arrangement, at least
  8 bars, plus 5 %, to the lanes' width.
- `gridStep()` is the adaptive grid: the smallest step, among musical subdivisions of the bar (1/32 to 2 beats, only
  those that divide the bar) and multiples of the bar, that is at least `gridMinPixels(level)` wide (6, 11, 20, 40 or
  80 px for levels -2 to 2). Ctrl+1 and Ctrl+2 change the level; the step also changes with zoom. `gridLabel` is what
  the corner shows ("Grid 1/16", "Grid 2 Bars", " (off)" while not snapping).
- `snapBeat(beat, bypass)` rounds to the grid unless snapping is off or `bypass` (Alt held) is true.
- **Following**: while playing with `follow` on, `onPosition()` scrolls so the playhead stays between 4 % and 96 % of
  the lanes' width (`kFollowMargin`). Scrolling by hand (the scroll bar, the wheel, the ruler, a Ctrl+Alt drag:
  `scrollByHand()`) sets `followPaused` until playback stops or starts again.

The piano roll has a `Timeline` of its own over content beats (see [piano-roll.md](piano-roll.md)), so the grid code
works for both.

Vertical positions are in *content coordinates* (0 at the top of the first track); an item's y is
`contentY - scrollY`.

## TrackLayout, rows and folding

`TrackLayout::rebuild()` walks `Project::tracks()` (groups and what is in them, flattened in display order) and
makes one `Row` per track:

| Field | Meaning |
|---|---|
| `trackId`, `top` | the track, and where its row starts (content y) |
| `mainHeight` | its own lane: the track's height; at least `minAutomationRow(project)` (76 px, plus `kSendsRow` 24 while there are returns) while its automation shows, so the header has room for the choosers |
| `lanes` | `LaneRow`s: the automation lanes shown below it, `kAutomationLaneHeight` (44 px) each |
| `automation` | its automation shows |
| `hidden` | it is in a folded group: a row with no height |
| `folded` | it is folded itself: `kFoldedHeight` (22), or `kFoldedGroupHeight` (24) for a group, and no automation |
| `bars` | folded, and not a group: its clips are drawn and grabbed as bars, and its lane is no grid (see [hit-testing](#hit-testing)) |
| `depth` | how many groups it is in (for the header's indent) |

Rows stay one per track, in order, hidden or not, so a row's index is the track's index in `Project::tracks()`.
`rowIndexAt(contentY)` bisects the rows' tops; a hidden row has the same top as the row after it, so the bisect lands
on the shown one, never the hidden one. `visibleRows(top, bottom)` skips hidden rows; `lastShownIndex()` is where a
drag below the tracks clamps to. A return's rows are `kReturnHeight` (52) high (80 while its automation shows), the
master's `kMasterHeight` (40) (76 while its automation shows), each with its lanes below.

`TrackRowModel` (roles `trackId`, `top`, `mainHeight`, `rowHeight`, `hidden`, `folded`, `depth`, `number`) follows
the layout: a change that keeps the same tracks in the same order changes the rows' data; a track coming or going
inserts or removes its row, so the other headers stay as they are (a rename under way, a drag); anything else (tracks
moved) resets the model. `ReturnRowModel` (roles `trackId`, `mainHeight`, `rowHeight`) does the same for returns.

Folding is view state: `editor.setFolded()` (and the track's height) change the model without an undo step;
`trackChanged` makes `Arrangement::onTrackChanged()` rebuild the layout if the track's row changed. `setFolded` keeps
the height a track had, so unfolding brings it back.

`Arrangement::wheelResize(trackId, delta)` is Alt+wheel over a track's lane or header: wheel events less than
`kWheelGesture` (0.4 s) apart act on the track the gesture started on, because the tracks below move up under the
mouse as it shrinks. Down shrinks by `kHeightStep` (12 px) a notch and, at the smallest height, folds it; up
unfolds a folded track, then grows it. Qt may report Alt+wheel as horizontal scrolling, so either axis counts.

## Selection

The selection is the application layer's [Selection](../../app/src/session/Selection.h) (`Session.selection`),
shared by the arrangement, the device view and the piano roll; it emits `changed` and `insertChanged`. Its fields
(`trackId`, `trackIds`, `insertBeat`, `timeRange`, `clipRange`, `clips`, `lanes`, `points`, `focus`) are described in
[app/session.md](../app/session.md). What matters here:

- Selecting is always on the grid: selecting a clip selects the area it covers. `selectClips(editor, refs)` asks the
  editor for `clipsArea(refs)` (earliest start, latest end, the tracks from the first clip's to the last's) and sets
  a clip range with `clipsInRange()` over it. So a clip selection *is* a time selection; there is no separate
  per-clip selection to keep in step.
- A time range over tracks is everything in it: every one made on the grid is a clip range, and the editor's range
  operations act on the clips and the automation of every track in it ([app/model.md](../app/model.md)). A drag that
  crosses a group's row takes in what is in the group (`Project::withContents`), folded away or not. Only ranges
  made on automation lanes (`lanes`) are about automation alone.
- `selectTrack(trackId, focusTrack, mode, order)`: `Toggle` (Ctrl-click) adds or removes one; `Range` (Shift-click)
  takes the tracks between the last one clicked and it, in `order`.
- The session prunes the selection when tracks, clips or automation change, so the views needn't.

How the session dispatches Delete, Cut, Copy, Paste and Ctrl+D on these fields is in
[README.md](README.md#what-an-action-acts-on).

## The grid and the ruler

[Timeline.cpp](../../ui/src/timeline/Timeline.cpp): `gridLines(view, x0, x1, step)` gives `(x, beat, kind)` with kind
`Bar`, `Beat` or `Sub` against the time signature. `drawGrid(..., overClip = true)` draws faint dark lines instead,
so the grid shows through a clip body of any colour. `labelStep()` picks how often the ruler labels: the grid step
or a coarser musical unit, at least 44 px apart. `drawLoopRegion()` tints the loop while it is on.

[ArrangementRuler](../../ui/src/arrangement/ArrangementRuler.h), 40 px high:

- The top 14 px (`kLoopStrip`) is the loop brace. `loopZone(x)` says start edge, end edge (`kEdgeGrab` 6 px) or
  body. Dragging edits the loop through `editor.setLoop(enabled, start, end, key)` with one key per drag (one undo
  step); an edge can't come within a quarter beat of the other; the body moves it; dragging in empty space draws a
  new loop (and turns it on). Double-click on the brace toggles it.
- Below, the scrub area: a click (no movement past 3 px) calls `Session.locate()` with the snapped beat; a drag
  decides once, on its first 3 px, whether it pans (mostly horizontal: `scrollByHand`) or zooms (mostly vertical:
  `zoomAt` by 1.012 per pixel, around where it was pressed).
- It draws the insert marker (a triangle); the playhead is an `ArrangementPlayhead` over it (`ruler: true`: a
  triangle at its foot).

## The lanes

[ArrangementLanes](../../ui/src/arrangement/ArrangementLanes.h) is the largest item. It repaints (`repaint()`:
`polish()` then `update()`) on the view moving, the layout, the selection, the project's track, clip, device,
freeze, automation and settings signals, and the bridge's `sourceReady`, `sourceFailed` and
`automationStateChanged`; signals about one track's parameters (`deviceParamChanged`, `pluginParamEdited`,
`devicesLoaded`, ...) only repaint when that track's automation shows (`updateIfShown`), since that is when a lane
draws a parameter's own value. A project reset clears the waveform cache.

### Painting

`updatePolish()` (GUI thread) works out what `paint()` needs from the bridge: the envelopes showing
(`envelopeAreas()`) and how each looks (`envelopes::lookOf()`: the parameter's spec, whether it is overridden, the
target's own value). `paint()` then draws only the rows that show, in this order:

1. The empty area's colour, then each row's background (`kLaneSelected` if its track is selected). The time
   selection's tinted areas are worked out (`selectedAreas()`: each track's stretch, its automation lanes too unless
   automation is locked).
2. The grid, all the way down (below the tracks too, where selecting works as well), and the loop region.
3. For each row: a folded track's tint (under its clips' bars, which stay as they are); a group's summary
   (`drawGroupSummary()`: the clips of every track in the group as translucent bars with a solid top edge, as in
   Ableton's group lanes); its clips, but those the gesture hides; a frozen track's tint (`kFrozenTint`); the lines
   between its automation lanes; its bottom border.
4. The gesture's previews: `kept()` (what stays of moved clips) and `ghosts()` (where they go, translucent).
5. The envelopes (`envelopes::drawArea()`), shaded over the clips in a track's own lane.
6. A drop preview (files dragged in: dashed boxes with their names), or the empty arrangement's hint.
7. The time selection: over the automation lanes it covers (`envelopes::drawRange()`), or as a tint (`kSelection`)
   over each track's stretch; then the title bars and outlines of the clips there drawn again over it
   (`drawClipFrame()`, clipped to the tint), so selecting never lights up a clip's title bar.
8. The insert marker on the selected track (when nothing is selected), and a gesture's readout (a breakpoint's value
   while dragging it).

`drawClip()` draws a clip's body (the track colour at 60 % saturation and 78 % value), the faint grid over it, a MIDI
clip's `playedNotes()` fitted to the clip's height (`drawNotes()`) or an audio clip's waveform from the
`WaveformCache` (`drawContent()`), at the clip's own tempo (`sourceTempo`) and scaled by its gain (`dbToGain(gainDb)`:
louder is taller, cut off at the body's edges); then its frame (`drawClipFrame()`): its title bar (`kTitleHeight`
16 px; a `kShortTitleHeight` 9 px bar with no name in rows under `kMinTitleRow` 30 px) and its outline (white when
selected, else the track colour darker). A folded track's clip (`clipTitleHeight(h, true)`) is all title bar: a bar
with its name, as in Ableton's folded tracks. Channels are drawn apart when the body is at least 44 px. Without a
decoded source it says "Loading…", or "Missing file" over a red tint (`bridge.loadError`). Selection isn't drawn per
clip: the selected area's tint does it (under a folded track's bars, which show they are selected by a white
outline).

The playhead and the takes being recorded change every frame, so they are items of their own over the lanes:

- [ArrangementPlayhead](../../ui/src/arrangement/ArrangementPlayhead.h): a line while playing (stopped, only the
  insert marker shows), over the lanes, each bus lane and (as a triangle) the ruler.
- [LiveTakes](../../ui/src/arrangement/LiveTakes.h) draws each take while it records, from `bridge.liveTakes()`
  (`LiveTake`): a red-titled clip from `startSample` to the playhead (what has come in, `startSample + frames`, lags
  it by the input's latency and arrives a buffer at a time; it repaints on `playheadChanged` while there are takes, so
  the take grows smoothly), its waveform from the peaks the engine sends (`peaks`, a (min, max) pair per
  `kPeakFrames` (128) frames, reduced to a column per pixel), or a MIDI take's notes so far (a held note reaches the
  take's end). The file isn't read until the take is done. See [engine/recording.md](../engine/recording.md).

### Hit-testing

- `rowIndexAt(y, clamp)`: the row under an item y; with `clamp`, above the first track gives the first, below the
  last gives the last shown.
- `hitClip(pos)` → `Hit{trackId, clip, zone}`, the topmost clip first. Zones: `Left` and `Right` (trim handles:
  `kEdgeGrab` 6 px, or a third of a narrow clip, inside each end of the title bar), `Title` (select and move), `Body`
  (the rest: time selection or insert marker, as on an empty lane). A folded track's clips (`row.bars`) have no body:
  all of a bar is title. Only points inside a clip count: next to it, or on a neighbour's side of a shared edge, you
  aren't trimming this one. Automation lanes below a track never hit clips.
- `inClipBand(pos)`: the top band of a lane (the title bar's height), or all of a short lane. Whether a drag selects
  clips or a lane range depends on this.
- `envelopeAreas()` / `envelopeAreaAt(pos)`: the automation lanes showing, as `EnvelopeArea`s (see
  [automation lanes](#automation-lanes)).

### Mouse

`mousePressEvent` decides what a left press starts:

```
Ctrl+Alt held                        → PanGesture (hand scrolling)
on an automation lane                → envelopes::press(...)
on a clip's trim handle              → select that clip; TrimGesture
inside the selected clip range       → MoveRangeGesture (a click without dragging
  (in the clip band, no Shift)          selects as a click elsewhere would)
on a clip's title                    → select its area (Shift: the area holding it
                                       and the last clip clicked); insert marker at
                                       the range's start; MoveRangeGesture
anywhere else (clip body, empty lane,→ clear the selection on that track; insert
  below the tracks)                    marker at the snapped beat; TimeSelectGesture
                                       (not on a folded track's lane: no grid there,
                                       the click only sets the insert marker)
```

A press while a gesture is still there (its release never came) ends what that gesture previewed
(`bridge.endClipPreview()`) first. The press a double-click starts with is ignored (the double-click stands for it).

`mouseMoveEvent` forwards to the gesture, or updates the cursor and the hover (`updateHover()`: Ableton's `[`/`]`
bracket cursors from `trimCursor()`, a pointing hand over titles and inside the clip range, an I-beam elsewhere (an
arrow on a folded track's lane, which isn't a grid), the open hand while Ctrl+Alt is held, and the automation
cursors). Key presses and releases update the cursor too, so holding Ctrl+Alt shows the hand without moving the
mouse. `mouseReleaseEvent` calls the gesture's `finish()`; losing the mouse grab (a popup) calls its `cancel()`.

Double-click on a clip selects it (unless it was among the selected clips) and asks the `Arrangement` to open the
selected clips, it first (`openClips()` → `Session.arrangement.requestClipView()`). On an automation lane a
double-click starts another `envelopes::press()`: each click of it counts.

Wheel: Alt resizes (or folds) the track; Ctrl zooms around the mouse (1.2 a notch); Shift, or a horizontal wheel,
scrolls sideways (80 px a notch); otherwise it scrolls vertically (48 px a notch). The header column scrolls
vertically too.

### Gestures

[Gesture.h](../../ui/src/arrangement/Gesture.h), [ClipGestures](../../ui/src/arrangement/ClipGestures.h). A gesture
is an object the lanes keep in `gesture_` from press to release; it gets `move(pos, modifiers)`, `finish()` and
`cancel()`, and the lanes ask it what to draw: `hiddenIds()` (clips not drawn in place), `kept()`, `ghosts()`,
`timeRange()` (where the selection is drawn while it moves) and `readout()`. What it draws is worked out in `move()`
(`paint()` only reads it). Clip gestures preview with pure clip maths from the model
([model/Edits.h](../../app/src/model/Edits.h)) and only call the editor once, in `finish()`, so a drag is one undo
step and the model isn't touched while dragging.

| Gesture | Does |
|---|---|
| `MoveRangeGesture` | Drag a clip range (a selected clip is one): moves it, Ctrl copies, Alt off the grid. Up to `kDragThreshold` (4 px) it is still a click (calls `onClick`). At the press it works out, per track, the clips inside the range cut at its edges (the pieces that move) and what stays. While dragging: the time delta snapped against the range's start, and the track delta clamped by the editor (clips only move onto tracks of their kind); whenever they change, `editor.movedRange` (what the move would make of the clips) goes to `bridge.previewClips`, so the clips are heard where they would land. `finish()` calls `editor.moveRange(...)`, `bridge.endClipPreview()`, and selects where it landed. |
| `TrimGesture` | Drag a trim handle: the clip trimmed to the snapped beat, previewed in the engine as it changes; `finish()` calls `editor.replaceClip(trackId, result, "Trim Clip")`, ends the preview and reselects the clip so the selection follows its new edges. |
| `TimeSelectGesture` | Click: the insert marker (set at the press). Drag: a range over the rows from where it started to where it is, and what is in the groups among them: always a clip range (the clips it touches), wherever in the lanes it goes. |
| `PanGesture` | Ctrl+Alt drag: scrolls both ways. |

The automation gestures (`PointGesture`, `CurveGesture`, `RangeGesture`, `LaneGesture`, in
[Envelopes.cpp](../../ui/src/arrangement/Envelopes.cpp)) are `Gesture`s too, so the lanes run them the same way;
unlike the clip gestures they edit the model as they go, with a merge key per drag.

### Clipboard and area commands

The clipboard is `ArrangementActions`' ([ArrangementActions.h](../../app/src/session/ArrangementActions.h)): one for
clip content (`ClipboardContent`), automation (`CopiedAutomation`) and tracks (`CopiedTracks`), so Ctrl+V pastes
whichever was copied last. The lanes' menus and the session's Edit commands call it:

| Method | Editor call | Afterwards |
|---|---|---|
| `deleteArea()` | `deleteRange` (clips and automation) | the empty area stays selected |
| `duplicateArea()` | `duplicateRange` | the copy is selected, the insert marker at its start |
| `copyArea()` / `cutArea()` | `copyRange` / `cutRange` | (cut) the empty area stays selected |
| `copyAutomation()` / `cutAutomation()` | `copyAutomationRange` / `cutAutomationRange` | |
| `paste()`, `paste(atBeat, trackId)` | `paste` (clips), `pasteAutomation`, or `pasteTracks` | what is pasted is selected; the insert marker goes to its end, so pasting again appends |
| `consolidate()` | `consolidateClips` (Ctrl+J) | the joined clips are selected |
| `reverseSelection()` | a reversed copy of each audio file in the range (written at once, or, for more than `kReverseInPlaceSeconds` (30 s) of audio, in the background in the render dialog, whose Cancel changes nothing; a reversed clip whose file is there goes back to it), then `reverseRange` (R) | the range stays selected |
| `insertMidiClip(trackId, beat, gridStep)` | `addMidiClipsOver` (inside the time selection) or `midiClipSpan` + `addMidiClip` | selected, opened in the piano roll |

Automation paste goes onto the selected lanes if there is a lane range (or the lane of the selected breakpoints),
else onto the lanes it came from; an automation lane's menu pastes onto the selected lanes if it is one of them, else
onto itself (`pasteAutomationAt(owner, key)`).

### Menus

A right-click asks the item for its menu as [MenuEntries](../../ui/src/arrangement/MenuEntries.h): the entries
(text, enabled, checkable, checked, a shortcut hint, a tooltip, a colour swatch or an automation dot, submenus), each
with what it does as a C++ function. `menuRequested(entries, pos)` hands them to QML as a list
(`MenuEntries::toVariant()`, each with an id); [ArrangementMenu.qml](../../ui/qml/arrangement/ArrangementMenu.qml)
shows them and calls `triggerMenu(id)` on the item with the one chosen, which runs it in C++. The item keeps the
last menu until then. Shortcuts are hints only: the window's actions handle the keys.

The lanes' menu (`ArrangementLanes::contextMenu(pos)`): on an automation lane, Cut and Copy (enabled inside the
selected range), Paste (copied automation) and the lane's own entries ([below](#hit-testing-and-the-press)). On a clip
(it is selected first unless it already was), or anywhere in the selected time range: Cut, Copy, Paste, Split Here
(on a clip: at the snapped beat under the mouse), Duplicate, Consolidate (`canConsolidate()`), Reverse
(`canReverse()`: an audio clip in it), Delete. Elsewhere: Paste at the snapped beat on that track, Insert MIDI Clip
(MIDI tracks), Insert Audio/MIDI Track (after it, in its group: `insertTrackAfter()`), Delete Track.

### Drag and drop

The lanes accept:

- **Audio files** (local URLs with an audio extension, from the browser or the file manager). `dragOver()` keeps a
  `DropPreview` (row, snapped beat, and the sources' names and durations from `Session.arrangement.dropSources()`)
  and draws dashed boxes; audio over a track that isn't an audio track, or below the tracks, gets a new track.
  `drop()` calls `dropFiles(paths, trackId, beat)`, which selects the clips.
- **Devices and plug-ins from the browser** (`deviceKinds()`, `pluginRefs()` of
  [BrowserMime.h](../../app/src/browser/BrowserMime.h)): onto the track under the mouse (instruments only go on MIDI
  tracks), or, if one is an instrument, below the tracks onto a new MIDI track (`dropDevices()`).
- **Presets from the browser** (`presetPaths()`): onto the track under the mouse, or below the tracks (an
  instrument preset makes a MIDI track) (`dropPresets()`).
- **Devices moved from a chain** (`kDeviceMoveMime`, written by the device view: the source track's id, then the
  devices' ids, a line each; `movedDevices()` reads it): onto another track's row, `dropMovedDevices()` →
  `editor.moveDevicesToTrack`, with their automation; plug-ins move as they are, without loading again. The target
  track is selected.

## Track headers

[TrackHeaderItem](../../ui/src/arrangement/TrackHeaderItem.h) is a strip's header, a track's (a group's too), a
return's or the master's (`trackId: "master"`). It paints what isn't a control, handles the mouse on its background,
and offers its QML controls what they show (as properties) and do (as invokables).
[TrackHeader.qml](../../ui/qml/arrangement/TrackHeader.qml),
[ReturnHeader.qml](../../ui/qml/arrangement/ReturnHeader.qml) and
[MasterHeader.qml](../../ui/qml/arrangement/MasterHeader.qml) lay the controls out over it.

### What it paints

Its background (`kLaneSelected` while selected, else `kPanelAlt`), a panel for each lane below it, a colour band for
each group it is in (`kIndent` 6 px each, outermost first), its own colour, the fold button (a triangle in a circle
for a track, pointing right while folded; three bars in a circle, filled while folded, for a group), the snowflake
of a frozen track (dimmer in a frozen group, not frozen itself), and its name. A return paints a colour band and its
name; the master "Master".

### Layout

TrackHeader.qml (252 px wide): the `Meter` on the right; on the name row (22 px) the activator (mute, labelled with
the track's number), solo, and arm (not for a group: it records nothing); on the second row (only when the lane is
at least 48 px) volume (a `ValueBox`, -70 to +6 dB, no wheel), pan (a `Knob`, no wheel), the input and the monitoring
buttons; then the send knobs (`SendKnobs`, while there are returns); then the automation choosers
(`AutomationChoosers`) in the own lane and one set per lane below. ReturnHeader.qml: the letter (the activator),
name, solo, volume, pan, sends to other returns and meter, and the choosers while its automation shows.
MasterHeader.qml: volume, pan, meter and the choosers.

### Interactions

| Interaction | Code |
|---|---|
| Click | `Selection::selectTrack(..., focusTrack, mode)`: Ctrl toggles, Shift selects a range in track order. A plain click on one of several selected tracks keeps them (to drag them all) and selects just it on release if no drag followed. The master's and a return's select them: the device view shows their chains. |
| Drag (past the start-drag distance) | moves `draggedTracks()` (the selected tracks if it is one of them): `Arrangement::dragTracks()` while moving, `dropTracks()` on release |
| Bottom 4 px of the own lane (`kResizeGrab`) | resize (`editor.setTrackHeight`); not while folded |
| Fold button (each click of a double-click counts) | `toggleFold()`: every one of `draggedTracks()` takes the clicked one's new state |
| Ctrl+R, the menu's Rename | rename in place: `startRename()` shows a `TextField`; `finishRename(text)` → `editor.renameTrack` (blank: no change) |
| Alt+wheel (over the header or any control in it) | `Arrangement::wheelResize()`; the wheel alone scrolls the headers |
| Solo | `soloClicked()`: exclusive unless Ctrl is held; unsoloing exclusively unsoloes every track (`Project::senders()`); a selected track's button acts on all selected → `editor.soloTracks(tracks, on, exclusive)` |
| Arm | `armClicked()`: exclusive unless Ctrl is held, a selected track's acting on all selected → `editor.armTracks`; says so if the track has no input |
| Activator | `activatorToggled()` → the track's mute |
| Volume / pan | `setVolume()` / `setPan()`: on one of several selected tracks, every selected track follows, by the same amount when dragged or wheeled (the control's `relative`), to the same value when typed or reset → `editor.setTracksParam`. Pressing one (`touchVolume()`, `touchPan()`) calls `editor.touchParameter`: the session shows its automation, as in Ableton. |
| Input | `inputMenu()`: No Input, each device input then each pair, then *Resampling* (the master) and every other track, group and return, those that would close a cycle disabled; a MIDI track's: No Input, All Ins, each connected input, a disconnected one still chosen, and a Channel submenu → `editor.trySetTrackInput`, `trySetTrackInputTrack`, `trySetTrackMidiInput` |
| Monitoring | `monitorMenu()`: In, Auto, Off → `editor.trySetTrackMonitor` |
| Right-click | `contextMenu()`: Rename, Color, Insert Audio/MIDI/Return Track, Cut, Copy, Paste (the session's, on the selected tracks), Duplicate, Delete, Group, Ungroup, Fold/Unfold, Move Out of Group, Freeze/Unfreeze and Flatten (`Session.freezeActions()`), Show/Hide Automation, Show Automation in New Lane, Re-Enable Automation. A return's: Rename, Color, Insert Return Track, Delete, freezing, automation. The master's: automation. |

Dragging headers to move tracks: `Arrangement::dropTarget(trackIds, y)` gives `(index, parent group, the group shown
as taking them, line y)`:

- the middle half of a group's header: into the group, last (`Project::subtreeEnd(index)`);
- the upper half of a header: before it, in its group;
- the lower half: after it (after the whole subtree of a folded group; into an open group, first:
  `Project::parentAt(after)`);
- below the tracks: last, in no group.

`editor.canMoveTracks` vetoes impossible targets (a group into itself); the drop marker shows a line, or a frame
around the group (`dropMarkerVisible`, `dropMarkerInto`, `dropMarkerY`, `dropMarkerHeight`). `editor.moveTracks` does
the move.

Duplicating tracks (Ctrl+D with tracks focused, or the menu) is `ArrangementActions::duplicateTracks()`: it first
stores the plug-ins' states of everything copied, so the copies get the plug-ins as they are now, then calls
`editor.duplicateTracks` and selects the copies.

Volume and pan show as heard (`volume`, `pan`, `volumeAutomation`, `panAutomation`): the envelope's current value
while it plays (`"on"`: the red dot), the model's otherwise (`"off"`, grey, while overridden). While the mixer is
automated (`mixerAutomated()`), the header follows the playhead. It feeds its `Meter` (`setLevels()`) from the
bridge's meters on each `metersUpdated`.

### Sends

`sends` is one entry per return (`returnId`, `letter`, `value`, `automation`, `enabled`, `preFader`, `toolTip`), in
every header (tracks', groups' and returns') while there are returns.
[SendKnobs.qml](../../ui/qml/arrangement/SendKnobs.qml) draws a knob and the return's letter for each (in the accent
colour while it taps before the fader; those that don't fit don't show). A knob is 0..1 mapped through
`automation::volumeToNormalized` / `normalizedToVolume`, so it moves as a volume fader does; typing a dB value works
(`parseSendLevel`). Turning it calls `setSend()` → `editor.trySetSendLevel(owner, returnId, levelDb, key)`, which
makes the send if need be. Right-click (`sendMenu()`): *Pre-Fader*, *Remove Send*, *Show Automation*. A knob is
greyed out (no menu) where the return feeds this strip (`app::feeds(graph, returnId, owner)`); the `Arrangement`
builds the routing graph once per change (`routingGraph()`, invalidated by structure, track and device changes: a
sidechain is a routing edge too).

### Returns and the master

- Their lanes are [BusLane](../../ui/src/arrangement/BusLane.h)s (`owner`: a return's id, or "master"): grid, loop
  region and their automation (in their own lane and in lanes below), edited with the same `envelopes::` functions as
  track lanes. Each has an `ArrangementPlayhead` over it.
- Clicking a return's or the master's header selects it, so the device view shows its chain.

## Automation lanes

[Envelopes](../../ui/src/arrangement/Envelopes.h) draws and edits envelopes for both hosts: `ArrangementLanes` (a
track's own lane, below the clips' title band, and its lanes below) and `BusLane` (returns and the master). Hosts
describe what shows as `EnvelopeArea{owner, key, lane, rect}`: `lane` is -1 for the owner's own lane, else its index
in `AutomationView::lanes`. A host implements `LanesHost`: `hostSession()`, `hostArrangement()`, `envelopeAreas()`,
`hostWidth()`, `setHostCursor()`, `repaint()`, and for the track lanes `isTrackLanes()`, `inClipBand()` and `rowAt()`,
which `LaneGesture` uses to pass a drag up into the clips.

Values are normalized 0..1 (top 1, bottom 0, `kValuePad` 4 px padding): `area.y(value)` and `area.value(y)`. The
breakpoints come from `Project::envelope(owner, key)`; how a parameter maps to 0..1 and is shown comes from
`bridge.paramSpec(owner, key)` (a `ParamSpec`: `discrete()`, `quantize()`, `formatNormalized()`,
`toNormalized()`). See [app/model.md](../app/model.md) and [engine/automation.md](../engine/automation.md).

### Drawing

- `trace(view, area, points, x0, x1, quantize)` is the line as drawn: through the breakpoints, flat before the first
  and after the last, sampled every `kSamplePixels` (3 px) along curved segments (`automation::segmentValue`), and in
  steps for a discrete parameter. Hit-testing uses the same `trace`, so what you click is what you see.
- `drawArea()` shades a track's own lane (over its clips: `kLaneBackground`), draws the line red (`kEnvelope`) or grey
  while overridden (`kOverridden`), the breakpoints (white when selected, filled when hovered), the hovered segment or
  end line thicker, and the ghost breakpoint where a click would add one (`kGhost`). A target without an envelope
  draws its own value as a faint dashed line (`kUnautomated`).
- `drawRange()` tints the selected range on the lanes it covers; `drawReadout()` shows the value of a breakpoint
  being dragged (`formatNormalized`).

### Hit-testing and the press

`hover(host, area, pos, modifiers)` returns a `Hover` (what is under the mouse) and the cursor;
`press(host, area, pos, modifiers)` returns the gesture. Both test in the same order:

| Order | Under the mouse | Hover kind / cursor | Gesture |
|---|---|---|---|
| 1 | a breakpoint (`pointAt`: within `kPointGrab` 6 px, the topmost) | `Point`, pointing hand | `PointGesture`: a click deletes it (Shift/Ctrl-click toggles it in the selection); a drag moves it and the others selected |
| 2 | Alt, between two breakpoints | vertical-resize cursor | `CurveGesture`: bends the segment (`kCurvePixels` 150 px from straight to the most) |
| 3 | inside the selected range, off the line and its segments | `Range` | `RangeGesture`: a drag moves the automation in the range on all its lanes, in time and value; a click is a `LaneGesture` click |
| 4 | near the flat line out of the first or last breakpoint, not on the line | `End` | `PointGesture` on that breakpoint (added: a click deletes nothing) |
| 5 | on the line (within `kLineGrab` 4 px), not a step | `Add`, the arrow-plus `addCursor()` | `addAndDrag()`: a breakpoint at the snapped beat on the line at once (`editor.addAutomationPoint`), then a `PointGesture` that places it; adding and dragging share a merge key, so they are one undo step |
| 6 | near a segment (within `kSegmentGrab` 14 px), or anywhere along a step | `Segment` | `PointGesture` on the segment: drags both breakpoints; a click selects them |
| 7 | elsewhere | none | `LaneGesture`: a click sets the insert marker; a drag selects a time range on the lanes it crosses (`nearestArea`), or clips if it goes up into the clips' title band or above its track (it hands the drag to a `TimeSelectGesture`) |

`PointGesture` moves breakpoints with `editor.moveAutomationPoints(owner, key, original, indices, deltaBeats,
deltaValue, key)` from the envelope as it was at the press, so moving back and forth is exact. A purely vertical
drag (under 4 px sideways) leaves the time alone; Shift makes values ten times finer. The editor returns where each
moved point ended up (several points override what they land on, so indices shift), and the gesture reselects them.

`addMenuEntries()` is a lane's right-click menu: Delete Breakpoint (or Delete Selected Breakpoints), Delete
Envelope, Re-Enable Automation (if overridden), Remove Lane (lanes below), Show Automation in New Lane, Hide
Automation.

### Showing automation

What shows is view state on each owner's `automationView` (`shown`, `key`, `lanes`), saved but not undone. `A` is
`editor.toggleAllAutomation`. A parameter touched by hand (`editor.parameterTouched`, from a mixer control, a
device's knob, or a plug-in's editor) makes the session show its owner's automation with that parameter in the own
lane (`editor.showAutomation(owner, key)`), if `bridge.canAutomate` says it can be automated.

### Choosers

[AutomationChoosers.qml](../../ui/qml/arrangement/AutomationChoosers.qml) lays out, from the header's `choosers`
(`[{lane, device, param}]`), a device chooser, a parameter chooser and **+** (`addLane()`) for the own lane, and a
device chooser, a parameter chooser and **−** (`removeLane(lane)`) for each lane below, centred in it. A chooser
([AutomationChooser.qml](../../ui/qml/arrangement/AutomationChooser.qml)) shows its text and asks for its menu:
`deviceMenu(lane)` lists `bridge.paramGroups(owner)` (*Mixer*, then each device, rack chains included), each
choosing the group's first automated parameter (or its first); `paramMenu(lane)` lists the current group's
parameters. Automated ones get a red dot. Choosing calls `editor.setAutomationLane(owner, lane, key)`.

## Waveform tiles

[WaveformCache](../../ui/src/arrangement/WaveformCache.h). Waveforms are drawn as columns on the scene graph
(`SgPainter::fillColumns`): a column per pixel from the minimum to the maximum of what it covers.

`renderTile(source, framesPerPixel, index, height, splitChannels, gain)` works out 256 px (`kTile`) of a source's
waveform (an `app::Waveform`: [app/engine-bridge.md](../app/engine-bridge.md)):

1. Column edges in source frames, with one padding column each side so smoothing is seamless across tile borders.
2. The coarsest peak level of the source's mipmap (`Waveform::peaks(level)`, `samplesPerPeak`) with no more frames
   per peak than per pixel; when zoomed in past the finest level, the raw samples. See
   [engine/warp.md](../engine/warp.md) for the peaks.
3. Each column's minimum and maximum, times the clip's gain (linear; cut off at the lane's edges); a [1 2 1]/4 blur
   for peak data (envelopes read smoother), none for raw samples.
4. Per channel lane when split, each column's top and bottom in pixels.

`WaveformCache` keeps up to `kMaxTiles` (800) tiles in an LRU, keyed by (path, frames, sample rate, frames per pixel,
tile index, height, split, gain), the gain rounded to 0.1 dB (`quantizedGain()`: turning a gain knob renders a few
tiles, not one per value, and a quiet clip's waveform never goes flat). Tiles are anchored to the start of the source
file, not the clip, so moving or trimming a clip reuses them; only zooming, a tempo change (frames per pixel) or a
new lane height or gain makes new ones. `draw()` works out which tiles the visible part of a clip body needs from the
clip's x and `offsetSec`, and draws them in the colour asked for. The cache is the one thing the lanes' `paint()`
changes (tiles are made as they are first drawn); a mutex keeps `clear()` (the GUI thread, on a project reset) apart
from it. The clip view draws its waveforms without a cache ([piano-roll.md](piano-roll.md#the-clip-view)).

## Extending it

- **A new clip gesture**: subclass `arrangement::Gesture` in
  [ClipGestures](../../ui/src/arrangement/ClipGestures.h); preview with `hiddenIds`/`kept`/`ghosts`/`timeRange` using
  the model's pure clip functions, work out what it draws in `move()`, and make one editor call in `finish()` (and
  undo any preview in `cancel()`). Start it from `ArrangementLanes::mousePressEvent` and give it a cursor in
  `updateHover()`.
- **A new header control**: add what it shows as a property of `TrackHeaderItem` (with its NOTIFY signal, refreshed
  in `refresh()`) and what it does as an invokable, then place the control in TrackHeader.qml. If it changes the
  row's height needs, adjust [TrackLayout.h](../../ui/src/arrangement/TrackLayout.h)'s constants and
  `TrackLayout::rebuild`.
- **A new menu entry**: add it where the menu is built (`ArrangementLanes::contextMenu`,
  `TrackHeaderItem::contextMenu`, `envelopes::addMenuEntries`), with what it does as a lambda; QML needs nothing.
- **Something new that is automatable** shows in the lanes once the bridge describes it (`paramGroups`,
  `paramSpec`): the lanes and choosers have nothing per-parameter.

## Gotchas

- `TrackLayout` rows are one per track, hidden or not. Index rows by track index, and skip `row.hidden` when drawing
  or hit-testing.
- A row's `mainHeight` isn't the track's `height`: it is taller while automation shows and fixed while folded. Use
  the row.
- `paint()` reads only: the bridge's state (envelope looks) is fetched in `updatePolish()`, a gesture's previews in
  `move()`. Call `repaint()` (which polishes first), not just `update()`, when what the lanes fetch may have changed.
- `MoveRangeGesture` previews with track indices and a track delta over `Project::tracks()`, so ghost rows are
  indices in the same flattened order as the layout.
- The model holds tracks by value: a `const Track&` is stale after any change. Items look tracks up again by id
  (`findTrack`) in slots and in `paint()`.
- Double-clicks on lanes and fold buttons start a second press: code must expect each click of a double-click.

## Tests

| Test file | Covers here |
|---|---|
| [test_ui_arrangement.cpp](../../tests/app/test_ui_arrangement.cpp) | Dragging and Ctrl-dragging clips and undo, trimming, selecting below the tracks, the clip body setting the insert marker, Shift-click ranges, drags ending in the clip band, clips heard where a drag (or trim) takes them, the ruler's loop brace and scrub zoom, zoom, scroll and follow, Alt+wheel resizing and folding, Ctrl+Alt drags, files, devices and presets dropped, double-click opening the clip view, the lanes' menus, MIDI clips, reversing, the live take reaching the playhead |
| [test_ui_arrangement_automation.cpp](../../tests/app/test_ui_arrangement_automation.cpp) | A, parameters showing their lanes, clicking on the line, dragging and bending breakpoints, segments and steps, deleting, time ranges (clear, duplicate, move), the lanes' menus, overriding and re-enabling, controls following automation, the master's lane, lanes below tracks, saving, automation moving with a dragged clip, dragging up into the clips |
| [test_ui_arrangement_tracks.cpp](../../tests/app/test_ui_arrangement_tracks.cpp) | Groups (folding, the group's header, dragging headers into and out of groups, folded tracks' clips as bars, cut, copy and paste), returns and sends, following the playhead while scrolling by hand, the header's controls (volume, solo, the activator, arming, renaming in place), the input and monitoring menus with resampling and MIDI inputs, sidechains greying out sends, the track menu's freezing and flattening, resizing a track by its bottom edge |
| [test_session_edit.cpp](../../tests/app/test_session_edit.cpp) | The area commands and the clipboard as the session drives them: splitting, selecting all, duplicating and deleting the selected area, cutting, copying and pasting clips, automation and tracks, reversing (at once and in the background) |
| [test_selection.cpp](../../tests/app/test_selection.cpp) | The selection's rules |
