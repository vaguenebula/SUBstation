# Arrangement view

The arrangement view is the timeline: a ruler on top, the track lanes with their headers
on the right (as in Ableton), the return tracks and the master pinned at the bottom, and
the automation lanes. It lives in
[src/substation/ui/arrangement](../../src/substation/ui/arrangement). It is
custom-painted for speed: each paint touches only what meets the dirty rectangle, and a
moving playhead repaints two thin strips.

What the user does with it: [guide/arrangement.md](../guide/arrangement.md),
[guide/automation.md](../guide/automation.md), [guide/mixing.md](../guide/mixing.md),
[guide/recording.md](../guide/recording.md). This page is about the code.

## Files

| File | What it holds |
|---|---|
| [arrangement_view.py](../../src/substation/ui/arrangement/arrangement_view.py) | `ArrangementView`: composes everything in a grid layout, owns the `ViewState`, `TrackLayout` and `WaveformCache`, the scroll bars, the returns' rows, the playhead and follow; `GridInfo` (the corner showing the grid size); hosts the clip view overlay |
| [view_state.py](../../src/substation/ui/arrangement/view_state.py) | `ViewState` (zoom, scroll, grid, snap, follow), `Selection` (shared with the whole window), `TrackLayout`, `Row`, `LaneRow`, `automation_rows()`; layout constants |
| [grid.py](../../src/substation/ui/arrangement/grid.py) | `grid_lines()`, `label_step()`, `draw_grid()`, `draw_loop_region()`: shared by the ruler, the lanes, the bus lanes and the piano roll |
| [ruler.py](../../src/substation/ui/arrangement/ruler.py) | `TimelineRuler`: the loop brace strip and the scrub area |
| [lanes_canvas.py](../../src/substation/ui/arrangement/lanes_canvas.py) | `LanesCanvas`: painting clips, group summaries, live takes, the selection, markers; hit-testing; mouse, wheel, keys, context menus, drag and drop; the clipboard; Alt+wheel resizing (`wheel_action`) |
| [interactions.py](../../src/substation/ui/arrangement/interactions.py) | the clip gestures: `ClipGesture` (base), `MoveRangeGesture`, `TrimGesture` (both heard as they drag: `bridge.preview_clips`), `TimeSelectGesture`, `PanGesture` |
| [track_headers.py](../../src/substation/ui/arrangement/track_headers.py) | `TrackHeader`, `TrackHeaderColumn`; `duplicate_tracks()`, input menus |
| [mixer_controls.py](../../src/substation/ui/arrangement/mixer_controls.py) | What every strip's header has: `volume_box()`, `pan_knob()`, `SendControls`; `automation_state()`, `show_mixer_values()` |
| [bus_tracks.py](../../src/substation/ui/arrangement/bus_tracks.py) | The master and return tracks' rows: `MasterHeader`, `ReturnHeader`, `BusLane`, `MasterLane`; `return_rows()` |
| [automation_lanes.py](../../src/substation/ui/arrangement/automation_lanes.py) | envelopes drawn and edited: `EnvelopeArea`, `Hover`, hit-testing, `trace()`, `draw_area()`, the automation gestures and `press()`/`hover()` |
| [automation_header.py](../../src/substation/ui/arrangement/automation_header.py) | `AutomationControls`: the device and parameter choosers in a header |
| [waveform_cache.py](../../src/substation/ui/arrangement/waveform_cache.py) | `render_tile()` (numpy rasterising, scaled by a clip's gain) and `WaveformCache` (an LRU of `QImage` tiles) |

## Layout

```
 col 0 (stretches)                       col 1 (HEADER_WIDTH 252)   col 2
┌──────────────────────────────────────┬────────────────────────┬───┐
│ TimelineRuler                        │ GridInfo               (spans 1-2)
├──────────────────────────────────────┼────────────────────────┼───┤
│ LanesCanvas            (row stretch) │ TrackHeaderColumn      │ v │
├──────────────────────────────────────┼────────────────────────┤ b │
│ return_lanes: a BusLane per return   │ return_headers         │ a │
├──────────────────────────────────────┼────────────────────────┤ r │
│ MasterLane                           │ MasterHeader           │   │
├──────────────────────────────────────┼────────────────────────┴───┘
│ hbar                                 │
└──────────────────────────────────────┘
          ClipView: an overlay over all of it, hidden until opened
```

The lanes canvas and the header column scroll together: they share one `ViewState`
(`scroll_y`), and `TrackHeaderColumn.relayout()` places each `TrackHeader` at
`row.top - scroll_y`. The returns and the master don't scroll vertically; they are
stacks of fixed-height widgets whose height follows their automation lanes
(`return_rows()`, `automation_rows()`).

The scroll bars are views of `ViewState`, not the other way round: `_update_hbar()`
sets the range from the content's end (the last clip, the loop's end, or the visible
width, plus 16 bars) and `_update_vbar()` from `TrackLayout.total_height` plus
`DROP_ZONE` (120 px of empty space below the tracks for dropping files). Both block the
bars' signals while they set them.

## ViewState and coordinates

`ViewState` ([view_state.py](../../src/substation/ui/arrangement/view_state.py)) holds
`px_per_beat` (0.25 to 4000), `scroll_beats`, `scroll_y`, `max_scroll_y` (kept by the
view), `grid_level`, `snap` and `follow`. It emits `changed` (zoom or horizontal
scroll), `vscroll_changed` and `grid_changed`.

- `beat_to_x(beat) = (beat - scroll_beats) * px_per_beat`; `x_to_beat` is the inverse.
  Time is in beats everywhere in the UI; seconds only come in through the tempo.
- `frames_per_pixel(sample_rate, source_tempo)`: source frames per pixel for a
  waveform, at the clip's own tempo (a warped clip's segment BPM) or the project's.
- `zoom_at(x, factor)` keeps the beat under `x` in place; `zoom_to_fit(start, end,
  width)` is *Zoom to Arrangement*.
- `grid_step()` is the adaptive grid: the smallest step, among musical subdivisions of
  the bar (1/32 to 2 beats, only those that divide the bar) and multiples of the bar,
  that is at least `GRID_MIN_PIXELS[grid_level]` wide (6, 11, 20, 40 or 80 px for
  levels -2 to 2). Ctrl+1 and Ctrl+2 change the level; the step also changes with zoom.
- `snap_beat(beat, bypass)` rounds to the grid unless snapping is off or `bypass`
  (Alt held) is true.

The piano roll makes its own `ViewState` over content beats (see
[piano-roll.md](piano-roll.md)), so the grid code works for both.

Vertical positions are in *content coordinates* (0 at the top of the first track);
widget y is `content_y - scroll_y`.

## TrackLayout, rows and folding

`TrackLayout.rebuild()` walks `project.tracks` (groups and what is in them, flattened
in display order) and makes one `Row` per track:

| Field | Meaning |
|---|---|
| `track_id`, `top` | the track, and where its row starts (content y) |
| `main_height` | its own lane: the track's height; at least `min_automation_row(project)` (76 px, plus `SENDS_ROW` 24 while there are returns) while its automation shows, so the header has room for the choosers |
| `lanes` | `LaneRow`s: the automation lanes shown below it, `AUTOMATION_LANE_HEIGHT` (44 px) each |
| `automation` | its automation shows |
| `hidden` | it is in a folded group: a row with no height |
| `folded` | it is folded itself: `FOLDED_HEIGHT` (22), or `FOLDED_GROUP_HEIGHT` (24) for a group, and no automation |
| `bars` | folded, and not a group: its clips are drawn and grabbed as bars, and its lane is no grid (see [hit-testing](#hit-testing)) |
| `depth` | how many groups it is in (for the header's indent) |

Rows stay one per track, in order, hidden or not, so a row's index is the track's
index in `project.tracks`. `row_index_at(content_y)` bisects the rows' tops; a hidden
row has the same top as the row after it, so the bisect lands on the shown one, never
the hidden one. `visible_rows(top, bottom)` skips hidden rows; `last_shown_index()` is
where a drag below the tracks clamps to.

Folding is view state: `editor.set_folded()` (and the track's height) change the model
without an undo step; `track_changed` makes `ArrangementView._on_track_changed()`
rebuild the layout, relayout the headers if anything moved, and repaint.
`set_folded` keeps the height a track had, so unfolding brings it back.

`_WheelResize` (`resize_track_by_wheel`, used by `wheel_action()`) is Alt+wheel over a
track's lane or header: wheel events less than `WHEEL_GESTURE` (0.4 s) apart act on the
track the gesture started on, because the tracks below move up under the mouse as it
shrinks. Down shrinks by `HEIGHT_STEP` (12 px) a notch and, at `MIN_TRACK_HEIGHT`, folds
it; up unfolds a folded track, then grows it. Qt may report Alt+wheel as horizontal
scrolling, so either axis counts.

## Selection

`Selection` ([view_state.py](../../src/substation/ui/arrangement/view_state.py)) is
made by the main window and shared by the arrangement, the device view and the piano
roll. It emits `changed` and `insert_changed`.

| Field | Meaning |
|---|---|
| `track_id` | the track the device view shows (the last one clicked), a return, `MASTER`, or None |
| `track_ids` | the selected tracks, in the order they were selected (a property; `_tracks` only counts while `track_id` is among them) |
| `insert_beat` | the insert (start) marker |
| `time_range` | `(start, end, track ids)`: a beat range over adjacent tracks, or None |
| `clip_range` | derived: the time range selects clip content (it was set with `clips=`) |
| `clips` | the `(track id, clip id)`s a clip range touches (what the clip view opens) |
| `lanes` | for a lane range on automation: the `(owner, key)`s it covers |
| `points` | `(owner, key, frozenset of indices)`: selected breakpoints of one envelope |
| `focus` | what Delete and the clipboard act on: `"clips"`, `"track"`, `"devices"`, `"automation"` |

Selecting is always on the grid: selecting a clip selects the area it covers.
`select_clips(editor, refs)` asks the editor for `clips_area(refs)` (earliest start,
latest end, the tracks from the first clip's to the last's) and sets a clip range with
`clips_in_range()` over it. So a clip selection *is* a time selection; there is no
separate per-clip selection to keep in step.

A time range over tracks is everything in it: every one made on the grid is a clip range
(`TimeSelectGesture` always passes `clips=`), and the editor's range operations act on
the clips and the automation of every track in it (see
[python/model.md](../python/model.md)). A drag that crosses a group's row takes in what is
in the group (`project.with_contents`), folded away or not. Only ranges made on automation
lanes (`lanes`) are about automation alone.

`select_track(track_id, focus_track, mode, order)`: `mode="toggle"` (Ctrl-click) adds
or removes one; `mode="range"` (Shift-click) takes the tracks between `_anchor` and it
in `order`. With `focus_track` the clip, range and point selection go and `focus`
becomes `"track"`.

`set_time_range(start, end, track_ids, clips=None, lanes=())` sets either kind of range;
`select_points` selects breakpoints and clears the rest; `focus_devices()` hands Delete
to the device view; `clear(track_id)` selects nothing. `prune(project)` drops what no
longer exists; the view calls it after structural changes, clip changes and automation
changes. How the main window dispatches Delete, Cut, Copy, Paste and Ctrl+D on these
fields is in [README.md](README.md#actions-and-the-focus).

## The grid and the ruler

[grid.py](../../src/substation/ui/arrangement/grid.py): `grid_lines(view, x0, x1, step)`
yields `(x, beat, kind)` with kind `bar`, `beat` or `sub` (`is_multiple` against the
time signature). `draw_grid(..., over_clip=True)` uses faint black lines instead, so the
grid shows through a clip body of any colour. `label_step()` picks how often the ruler
labels: the grid step or a coarser musical unit, at least 44 px apart.

[ruler.py](../../src/substation/ui/arrangement/ruler.py): `TimelineRuler`, 40 px high.

- The top 14 px (`LOOP_STRIP`) is the loop brace. `_loop_zone(x)` says start edge, end
  edge (6 px grab) or body. Dragging edits the loop through `editor.set_loop(enabled,
  start, end, key)` with one key per drag (one undo step); an edge can't come within a
  quarter beat of the other; the body moves it; dragging in empty space draws a new
  loop (and turns it on). Double-click on the brace toggles it.
- Below, the scrub area: a click (no movement past 3 px) emits `locate_requested` with
  the snapped beat; a drag decides once, on its first 3 px, whether it pans (mostly
  horizontal: scroll) or zooms (mostly vertical: `zoom_at` by 1.012 per pixel, around
  where it was pressed).
- It draws the insert marker (a triangle) and the playhead (only while playing).

## LanesCanvas

[lanes_canvas.py](../../src/substation/ui/arrangement/lanes_canvas.py) is the largest
widget. It is opaque (`WA_OpaquePaintEvent`) and repaints on any of a long list of
signals (view, selection, project, bridge `source_ready`, `recording_updated`, ...);
signals about one track's parameters only repaint when that track's automation shows
(`_update_if_shown`), since that is when a lane draws a parameter's own value.

### Painting

`paintEvent` works only on the rows that meet the dirty rectangle, in this order:

1. The empty area colour, then each row's background (lighter if its track is
   selected). The time selection's tinted areas are worked out (`_selected_areas`: each
   track's stretch, its automation lanes too unless automation is locked).
2. The grid, all the way down (below the tracks too, where selecting works as well),
   and the loop region.
3. For each row: a folded track's tint (under its clips' bars, which stay as they are);
   a group's summary (`_draw_group_summary`: the clips of every track in the group as
   translucent bars with a solid top edge, as in Ableton's group lanes); its clips,
   skipping those the current gesture hides; a live take while recording; the lines
   between its automation lanes; its bottom border.
4. The gesture's previews: `kept()` (what stays of moved clips) and `ghosts()` (where
   they go, translucent).
5. The envelope areas (`automation_lanes.draw_area`), shaded over the clips in a
   track's own lane.
6. A drop preview (files dragged in from the browser), or the empty-arrangement hint.
7. The time selection: over the automation lanes it covers (`draw_range`), or as a tint
   (`theme.SELECTION`) over each track's stretch; then the title bars and outlines of the
   clips drawn there again over it (`_draw_clip_frame`, clipped to the tint), so selecting
   never lights up a clip's title bar.
8. The insert marker on the selected track (when nothing is selected), the playhead,
   and a gesture's readout (a breakpoint's value while dragging it).

`_draw_clip` draws a clip's body (the track colour, desaturated), the faint grid over
it, a MIDI clip's `played_notes()` fitted to the clip's height (`_draw_notes`) or an audio
clip's waveform from the `WaveformCache` (`_draw_content`), with the clip's
`frames_per_pixel` at its own tempo (`source_tempo`) and scaled by its gain
(`db_to_gain(gain_db)`: louder is taller, cut off at the body's edges); then its frame
(`_draw_clip_frame`): its title bar (16 px; a 9 px bar with no name in rows under 30 px)
and its outline. A folded track's clip (`clip_title_height(h, folded=True)`) is all title
bar: a bar with its name, as in Ableton's folded tracks. Channels are drawn apart when the
body is at least 44 px. Without a decoded source it says "Loading…" or "Missing file"
(`bridge.load_error`). Selection isn't drawn per clip: the selected area's tint does it
(under a folded track's bars, which show they are selected by a white outline).

`_draw_live_take` draws a take while it records, from `bridge.live_takes`: a red-titled
clip from `start_sample` to the playhead (what has come in, `start_sample + frames`, lags
it by the input's latency and arrives a buffer at a time; `set_playhead` repaints the strip
between the old and new playhead while takes record, so it grows smoothly), its waveform
from the peaks the engine sends (`take.peaks`, `PEAK_FRAMES` frames each, reduced to one column per pixel
with `np.minimum.reduceat` / `np.maximum.reduceat`), or a MIDI take's notes so far
(`_draw_live_notes`; a held note reaches the take's end). The file isn't read until
the take is done. See [engine/recording.md](../engine/recording.md).

`set_playhead(beat)` repaints a 5 px strip where the playhead was and one where it is;
`None` hides it (stopped). `ArrangementView._on_position()` calls it on every lane
widget and the ruler, and with *Follow* on scrolls so the playhead stays between 4 %
and 96 % of the width.

### Hit-testing

- `row_index_at(y, clamp)`: the row under a widget y; with `clamp`, above the first
  track gives the first, below the last gives the last shown.
- `hit_clip(pos)` → `(track id, clip, zone)`, the topmost clip first. Zones: `left` and
  `right` (trim handles: `EDGE_GRAB` 6 px, or a third of a narrow clip, inside each end
  of the title bar), `title` (select and move), `body` (the rest: time selection or
  insert marker, as on an empty lane). A folded track's clips (`row.bars`) have no body:
  all of a bar is title. Only points inside a clip count: next to it, or
  on a neighbour's side of a shared edge, you aren't trimming this one. Automation
  lanes below a track never hit clips.
- `in_clip_band(pos)`: the top band of a lane (the title bar's height), or all of a
  short lane. Whether a drag selects clips or a lane range depends on this.
- `envelope_areas()` / `envelope_area_at(pos)`: the automation lanes showing, as
  `EnvelopeArea`s (see [automation lanes](#automation-lanes)).

### Mouse

`mousePressEvent` decides what a left press starts:

```
Ctrl+Alt held                        → PanGesture (hand scrolling)
on an automation lane                → automation_lanes.press(...)
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

A press while a gesture is still there (its release never came) ends what that gesture
previewed (`bridge.end_clip_preview()`) first.

`mouseMoveEvent` forwards to the gesture, or updates the cursor and hover state
(`_update_cursor`: Ableton's `[`/`]` bracket cursors from `trim_cursor()`, a pointing
hand over titles and inside the clip range, an I-beam elsewhere (an arrow on a folded
track's lane, which isn't a grid), the open hand while
Ctrl+Alt is held, and the automation cursors). Key presses and releases update the
cursor too, so holding Ctrl+Alt shows the hand without moving the mouse.
`mouseReleaseEvent` calls the gesture's `finish()`.

Double-click on a clip emits `clip_view_requested`; the view opens the selected clips
(or just that one if it wasn't selected) with it as the lead. On an automation lane a
double-click starts another `press()`: each click of it counts.

Wheel: Alt resizes (or folds) the track; Ctrl zooms around the mouse (1.2 a notch);
Shift, or a horizontal wheel, scrolls sideways (80 px a notch); otherwise it scrolls
vertically (48 px a notch). The header column scrolls vertically too.

### Gestures

[interactions.py](../../src/substation/ui/arrangement/interactions.py). A gesture is an
object the canvas keeps in `_gesture` from press to release; it gets `move(pos,
modifiers)` and `finish()`, and the canvas asks it what to draw: `hidden_ids()` (clips
not drawn in place), `kept()`, `ghosts()`, `time_range()` (where the selection is drawn
while it moves) and `readout()`. Clip gestures preview with pure clip maths from
[model/edits.py](../../src/substation/model/edits.py) and only call the editor once,
in `finish()`, so a drag is one undo step and the model isn't touched while dragging.

| Gesture | Does |
|---|---|
| `MoveRangeGesture` | Drag a clip range (a selected clip is one): moves it, Ctrl copies, Alt off the grid. Up to `DRAG_THRESHOLD` (4 px) it is still a click (calls `on_click`). At the press it works out, per track, the clips inside the range cut at its edges (`edits.slice_range`: the pieces that move) and what stays (`edits.remove_range`). While dragging: the time delta snapped against the range's start, and the track delta clamped by `editor.clamp_track_delta` (clips only move onto tracks of their kind); whenever they change, `editor.moved_range` (what the move would make of the clips) goes to `bridge.preview_clips`, so the clips are heard where they would land. `finish()` calls `editor.move_range(start, end, track_ids, delta, track_delta, copy_clips=...)`, `bridge.end_clip_preview()`, and selects where it landed. |
| `TrimGesture` | Drag a trim handle: `edits.trim_start` / `trim_end` to the snapped beat, previewed in the engine as it changes; `finish()` calls `editor.replace_clip(track_id, result, "Trim Clip")`, ends the preview and reselects the clip so the selection follows its new edges. |
| `TimeSelectGesture` | Click: the insert marker (set at the press). Drag: a range over the rows from where it started to where it is, and what is in the groups among them (`project.with_contents`): always a clip range (`clips=` the clips it touches), wherever in the lanes it goes. |
| `PanGesture` | Ctrl+Alt drag: scrolls both ways. |

The automation gestures (`PointGesture`, `CurveGesture`, `RangeGesture`, `LaneGesture`)
subclass `ClipGesture` too, so the canvas runs them the same way; unlike the clip
gestures they edit the model as they go, with a merge key per drag.

### Clipboard and area commands

`LanesCanvas.clipboard` holds the last thing copied: `ClipboardContent` (clips) or
`CopiedAutomation`. One clipboard means Ctrl+V pastes whichever was copied last.

| Method | Editor call | Afterwards |
|---|---|---|
| `delete_area()` | `delete_range` (clips and automation) | the empty area stays selected |
| `duplicate_area()` | `duplicate_range` | the copy is selected, the insert marker at its start |
| `copy_area()` / `cut_area()` | `copy_range` / `cut_range` | (cut) the empty area stays selected |
| `copy_automation()` / `cut_automation()` | `copy_automation_range` / `cut_automation_range` | |
| `paste(at_beat, track_id)` | `paste` (clips) or `paste_automation` | the pasted range is selected; the insert marker goes to its end, so pasting again appends |
| `consolidate()` | `consolidate_clips` (Ctrl+J) | the joined clips are selected |
| `reverse_selection()` | a reversed copy of each audio file in the range (`bridge.reversed_copy`, or written with `start_reversed`: more than `REVERSE_IN_PLACE_SECONDS` of them in the background, in the render dialog, whose Cancel changes nothing; a reversed clip whose file is there goes back to it), then `reverse_range` (R) | the range stays selected |
| `insert_midi_clip(track_id, x)` | `add_midi_clips_over` (inside the time selection) or `midi_clip_span` + `add_midi_clip` | selected, opened in the piano roll |

Automation paste goes onto the selected lanes if there is a lane range (or the lane of
the selected breakpoints), else onto the lanes it came from
(`editor.paste_automation(content, at, lanes)`).

### Context menus

`contextMenuEvent`: on an automation lane, Cut/Copy (enabled inside the selected
range), Paste (onto the selected lanes if this is one of them, else this lane) and the
lane's own entries (`automation_lanes.add_menu_actions`). On a clip (it is selected
first unless it already was), or anywhere in the selected time range: Cut, Copy, Paste,
Split Here (on a clip: at the snapped beat under the mouse), Duplicate, Consolidate
(enabled by `editor.consolidatable`), Reverse (enabled with an audio clip in it), Delete.
Elsewhere: Paste at the snapped beat on that track, Insert MIDI Clip (MIDI tracks),
Insert Audio/MIDI Track (after it, in its group: `editor.insertion_point`), Delete
Track.

### Drag and drop

The canvas accepts three kinds of drops:

- **Audio files** (`audio_paths(mime)`: local URLs with an audio extension, from the
  browser or the desktop). `dragMoveEvent` keeps `_drop_preview = (row index, snapped
  beat, [(path, duration)])` (from `bridge.file_info`) and draws dashed boxes; audio
  over a MIDI track or below the tracks gets a new track (index None). `dropEvent`
  calls `editor.add_clips(track_id, beat, sources, track_index=...)` and selects the
  clips.
- **Devices from the browser** (`dropped_devices(mime)`: built-in kinds, then plug-in
  refs, from [browser_models](../browser.md)): onto the track under the mouse
  (`editor.add_device`; instruments only go on MIDI tracks), or, if one is an
  instrument, below the tracks onto a new MIDI track.
- **Devices moved from a chain** (`DEVICE_MOVE_MIME`, written by the device view:
  the source track id, then device ids, a line each; `moved_devices(mime)` reads it):
  onto another track's row, `editor.move_devices_to_track`, with their automation;
  plug-ins move as they are, without loading again. The target track is selected.

## Track headers

[track_headers.py](../../src/substation/ui/arrangement/track_headers.py); their volume, pan and send controls are in
[mixer_controls.py](../../src/substation/ui/arrangement/mixer_controls.py), the master's and returns' rows in
[bus_tracks.py](../../src/substation/ui/arrangement/bus_tracks.py).

### TrackHeaderColumn

Holds one `TrackHeader` per row (`headers: dict[track id, TrackHeader]`). `sync()`
makes and deletes headers to match `TrackLayout.rows`; `relayout()` places them at
their row's widget y and height, hides those in folded groups, numbers them (the
activator shows the track's number) and hands each its `Row`. It forwards meters
(`bridge.meters_updated`) to every header, and on `position_changed` refreshes the
mixer of headers whose volume, pan or sends are automated (`mixer_automated`), so they
follow their automation as it plays.

Dragging headers to move tracks: the pressed `TrackHeader` calls
`column.drag_tracks(track_ids, y)` as the mouse moves past the start-drag distance and
`drop_tracks` on release. `drop_target(track_ids, y)` gives `(index, parent group, the
group shown as taking them, line y)`:

- the middle half of a group's header: into the group, last
  (`project.subtree_end(index)`);
- the upper half of a header: before it, in its group;
- the lower half: after it (after the whole subtree of a folded group; into an open
  group, first: `project.parent_at(after)`);
- below the tracks: last, in no group.

`editor.can_move_tracks` vetoes impossible targets (a group into itself); the
`_DropMarker` shows a line or a frame around the group. `editor.move_tracks` does the
move.

### TrackHeader

One per track (a group's too). Its widgets are placed by hand in `_layout()` from the
row: the meter on the right; the activator (mute, labelled with the track number),
solo and arm (hidden for groups: they record nothing) on the name row; on the second
row (only when the lane is at least 48 px) volume (`volume_box()`: a `ValueBox` -70 to
+6 dB, no wheel), pan (`pan_knob()`), the input button and the monitoring button; then
the send knobs (`SendControls.place`), then the automation choosers
(`AutomationControls.place`) in the own lane and one set per lane below.

It paints its background, a colour band for each group it is in (`INDENT` 6 px each,
outermost first), its own colour, the fold button (`_paint_fold`: a triangle in a
circle for a track; three bars in a circle, filled while folded, for a group) and the
name.

| Interaction | Code |
|---|---|
| Click | `selection.select_track(..., focus_track=True, mode=...)`: Ctrl toggles, Shift selects a range in track order. A plain click on one of several selected tracks keeps them (to drag them all) and selects just it on release if no drag followed. |
| Drag | moves `dragged_tracks()`: the selected tracks if it is one of them |
| Bottom 4 px of the own lane | resize (`editor.set_track_height`); not while folded |
| Fold button (or double-click it, each click counts) | `toggle_fold()`: every one of `dragged_tracks()` takes the clicked one's new state |
| Double-click the name | rename in place (`QLineEdit`; `editor.rename_track`) |
| Alt+wheel (over the header or any control in it) | `wheel_action()`: an event filter on its child widgets catches the wheel before they do |
| Solo | `_solo_clicked`: exclusive unless Ctrl is held (`QApplication.keyboardModifiers()`); unsoloing exclusively unsoloes every track (`project.senders()`); a selected track's button acts on all selected → `editor.solo_tracks(tracks, on, exclusive)` |
| Arm | `_arm_clicked`: the same rules → `editor.arm_tracks`; warns if the track has no input |
| Volume / pan | `_mixer_changed`: on one of several selected tracks, every selected track follows, by the same amount when dragged or wheeled (`widget.relative`), to the same value when typed or reset → `editor.set_tracks_param` |
| Input | `input_menu()`: no input, each device input then each pair (`input_choices`), then *Resampling* (the master) and every other track, group and return (`project.input_sources`), those that would close a cycle disabled (`project.input_would_cycle`); a MIDI track gets `midi_input_menu()` (No Input, All Ins, each connected input, a disconnected one still chosen, and a Channel submenu) |
| Monitoring | `monitor_menu()`: In, Auto, Off |
| Right-click | rename, colour, insert tracks, duplicate, delete, group, ungroup, fold/unfold, Move Out of Group, show/hide automation, Re-Enable Automation |

`duplicate_tracks(editor, bridge, selection, track_ids)` (Ctrl+D with a track focused,
or the menu) first stores the plug-ins' states of everything copied
(`bridge.store_plugin_states(devices)`), so the copies get the plug-ins as they are
now, then calls `editor.duplicate_tracks` and selects the copies.

Pressing a volume or pan control calls `editor.touch_parameter` through a `_TouchFilter`
event filter (`watch_mixer_touch`): clicking a control shows its automation, as in
Ableton. `show_mixer_values()` shows volume and pan as heard: the envelope's current
value while it plays (`automation_state()` is `"on"`), the model's otherwise, with the
red or grey dot.

### SendControls

One knob and letter per return, in every header (tracks', groups' and returns'), while
there are returns. A knob is 0..1 mapped through `automation.volume_to_normalized` /
`normalized_to_volume`, so it moves as a volume fader does; typing a dB value works.
Turning it calls `editor.set_send(owner, return_id, level_db, merge_key)`, which makes
the send if need be. Right-click: *Pre-Fader* (the letter turns the accent colour),
*Remove Send*, *Show Automation*. `refresh(graph)` disables knobs whose return feeds
this strip (`feeds(graph, return_id, owner)`); `ArrangementView._refresh_sends()`
builds the routing graph once and refreshes every header's sends with it, after any
change to structure, a track, or devices (a sidechain is a routing edge too).

### Returns and the master

- `ReturnHeader`: a return's compact row (`RETURN_HEIGHT` 52): name, activator (its
  letter), solo, volume, pan, sends to other returns, meter, and its automation
  choosers. Right-click: rename, colour, insert return, delete, automation.
- `MasterHeader`: volume, pan, meter (`MASTER_HEIGHT` 40) and automation choosers.
  Clicking it selects `MASTER`, so the device view shows the master's chain.
- `BusLane`: the lane of a strip without clips (a return, or the master as
  `MasterLane`): grid, loop region, playhead and its automation (in its own row and in
  lanes below), edited with the same `automation_lanes` functions as track lanes.
- `ArrangementView._sync_returns()` keeps a `(BusLane, ReturnHeader)` pair per return,
  in order; their heights follow `return_rows()`.

## Automation lanes

[automation_lanes.py](../../src/substation/ui/arrangement/automation_lanes.py) draws
and edits envelopes for both hosts: `LanesCanvas` (a track's own lane, below the clips'
title band, and its lanes below) and `BusLane` (returns and the master). Hosts describe
what shows as a list of `EnvelopeArea(owner, key, lane, rect)`: `lane` is -1 for the
owner's own lane, else its index in `AutomationView.lanes`. The host must provide
`view`, `project`, `editor`, `bridge`, `selection`, `_gesture`, `update()`, `width()`
and `setCursor()`; `LanesCanvas` also provides `envelope_areas()`, `in_clip_band()` and
`layout_model`, which `LaneGesture` uses to pass a drag up into the clips.

Values are normalized 0..1 (top 1, bottom 0, 4 px padding): `area.y(value)` and
`area.value(y)`. The breakpoints come from `project.envelope(owner, key)`; how a
parameter maps to 0..1 and is shown comes from `bridge.param_spec(owner, key)` (a
`ParamSpec`: `discrete`, `quantize`, `format_normalized`, `to_normalized`). See
[python/model.md](../python/model.md) and [engine/automation.md](../engine/automation.md).

### Drawing

- `trace(view, area, points, x0, x1, quantize)` is the line as drawn: through the
  breakpoints, flat before the first and after the last, sampled every 3 px along
  curved segments (`automation.segment_value`), and in steps for a discrete parameter.
  Hit-testing uses the same `trace`, so what you click is what you see.
- `draw_area()` shades a track's own lane (over its clips), draws the line red
  (`ENVELOPE`) or grey while overridden (`bridge.is_overridden`), the breakpoints
  (white when selected, filled when hovered), the hovered segment or end line thicker,
  and the ghost breakpoint where a click would add one. A target without an envelope
  draws its own value (`bridge.own_value`) as a faint dashed line.
- `draw_range()` tints the selected range on the lanes it covers; `draw_readout()`
  shows the value of a breakpoint being dragged (`spec.format_normalized`).

### Hit-testing and the press

`hover(host, area, pos, mods)` returns a `Hover` (what is under the mouse) and the
cursor; `press(host, area, pos, mods)` returns the gesture. Both test in the same
order:

| Order | Under the mouse | Hover kind / cursor | Gesture |
|---|---|---|---|
| 1 | a breakpoint (`point_at`: within `POINT_GRAB` 6 px, the topmost) | `point`, pointing hand | `PointGesture`: a click deletes it (Shift/Ctrl-click toggles it in the selection); a drag moves it and the others selected |
| 2 | Alt, between two breakpoints | vertical-resize cursor | `CurveGesture`: bends the segment (`CURVE_PIXELS` 150 px from straight to the most) |
| 3 | inside the selected range, off the line and its segments | `range` | `RangeGesture`: a drag moves the automation in the range on all its lanes, in time and value; a click is a `LaneGesture` click |
| 4 | near the flat line out of the first or last breakpoint (`end_at`), not on the line | `end` | `PointGesture` on that breakpoint (`added=True`: a click deletes nothing) |
| 5 | on the line (`_on_line`: within `LINE_GRAB` 4 px), not a step | `add`, the arrow-plus `add_cursor()` | `_add_and_drag`: a breakpoint at the snapped beat on the line at once (`editor.add_automation_point`), then a `PointGesture` that places it; adding and dragging share a `_MergeKey`, so they are one undo step |
| 6 | near a segment (`segment_at`: within `SEGMENT_GRAB` 14 px), or anywhere along a step | `segment` | `PointGesture(segment=True)`: drags both breakpoints; a click selects them |
| 7 | elsewhere | none | `LaneGesture`: a click sets the insert marker; a drag selects a time range on the lanes it crosses (`nearest_area`), or clips if it goes up into the clips' title band or above its track (it hands the drag to a `TimeSelectGesture`) |

`PointGesture.move` calls `editor.move_automation_points(owner, key, original, indices,
delta_beats, delta_value, merge_key)` from the envelope as it was at the press, so
moving back and forth is exact. A purely vertical drag (under 4 px sideways) leaves the
time alone; Shift makes values ten times finer. The editor returns where each moved
point ended up (several points override what they land on, so indices shift), and the
gesture reselects them.

`add_menu_actions()` is a lane's right-click menu: Delete Breakpoint(s), Delete
Envelope, Re-Enable Automation (if overridden), Remove Lane (lanes below), Show
Automation in New Lane, Hide Automation.

### Showing automation

What shows is view state on each owner's `automation_view` (`shown`, `key`, `lanes`),
saved but not undone. `A` is `editor.toggle_all_automation`. A parameter touched by
hand (`editor.parameter_touched`, from a mixer control, a device's knob, or a plug-in's
editor) makes `ArrangementView._on_parameter_touched()` show its owner's automation
with that parameter in the own lane (`editor.show_automation(owner, key)`), if
`bridge.can_automate` says it can be automated.

### Choosers

[automation_header.py](../../src/substation/ui/arrangement/automation_header.py):
`AutomationControls(owner, editor, bridge, parent)` keeps a `_LaneControls` for the own
lane (a device `Chooser`, a parameter `Chooser` and a **+** that adds a lane) and one
per lane below (with **−** to remove it). The header calls `place(main_rect,
lane_rects)` as its layout changes; controls are created and deleted to match. The
device menu lists `bridge.param_groups(owner)` (*Mixer*, then each device, rack chains
included), each choosing the group's first automated parameter (or its first); the
parameter menu lists the current group's parameters. Automated ones get a red dot.
Choosing calls `editor.set_automation_lane(owner, lane, key)`.

## Waveform tiles

[waveform_cache.py](../../src/substation/ui/arrangement/waveform_cache.py).

`render_tile(source, frames_per_px, index, height, split_channels, argb, gain)` rasterises
256 px (`TILE`) of a source's waveform into a premultiplied ARGB `QImage` with numpy:

1. Column edges in source frames, with one padding column each side so smoothing is
   seamless across tile borders.
2. The coarsest peak level of the engine's mipmap (`source.peaks(level)`,
   `AudioSource.samples_per_peak`) with no more frames per peak than per pixel; when
   zoomed in past level 0, the raw samples (`source.samples`). See
   [engine/warp.md](../engine/warp.md) for the peaks.
3. Each column's min and max (`np.minimum.reduceat` / `np.maximum.reduceat`), times
   the clip's gain (linear; cut off at the lane's edges); a [1 2 1]/4 blur for peak data
   (envelopes read smoother), none for raw samples.
4. Antialiased coverage: each pixel row gets the fraction of it between the column's
   top and bottom (at least a pixel thick), per channel lane when split.

`WaveformCache` keeps up to `MAX_TILES` (800) tiles in an `OrderedDict` used as an LRU,
keyed by `(path, frames, sample rate, frames per pixel, tile index, height, split,
colour, gain)` (the gain rounded to 1 %, so turning a gain knob renders a few tiles, not
one per value). Tiles are anchored to the start of the source file, not the clip, so moving
or trimming a clip reuses them; only zooming, a tempo change (frames per pixel) or a
new lane height or gain renders new ones. `draw()` works out which tiles the visible part of a
clip body needs from the clip's x and `offset_sec`. The cache is cleared on project
reset. The clip view keeps its own cache (200 tiles).

## Extending it

- **A new clip gesture**: subclass `ClipGesture` in
  [interactions.py](../../src/substation/ui/arrangement/interactions.py); preview with
  `hidden_ids`/`kept`/`ghosts`/`time_range` using pure functions from `model/edits.py`,
  and make one editor call in `finish()`. Start it from `LanesCanvas.mousePressEvent`
  and give it a cursor in `_update_cursor`.
- **A new header control**: create it in `TrackHeader.__init__`, place it in
  `_layout()`, refresh it in `refresh()`, and install the header as its event filter
  so Alt+wheel still resizes. If it changes the row's height needs, adjust
  `view_state`'s constants and `TrackLayout.rebuild`.
- **Something new that is automatable** shows in the lanes once the bridge describes it
  (`param_groups`, `param_spec`): the lanes and choosers have nothing per-parameter.

## Gotchas

- `TrackLayout` rows are one per track, hidden or not. Index rows by track index, and
  skip `row.hidden` when drawing or hit-testing.
- A row's `main_height` isn't the track's `height`: it is taller while automation shows
  and fixed while folded. Use the row.
- Signals that a return's lane or header listen to are connected to bound methods: the
  widgets can be deleted (the return deleted) while the bridge lives on.
- `MoveRangeGesture` previews with `project.track_index` and `track_delta` over
  `project.tracks`, so ghost rows are indices in the same flattened order as the layout.
- `LanesCanvas.paste` and `MainWindow.paste` share one clipboard for clips and
  automation; the last copied wins.
- Double-clicks on lanes and fold buttons start a second press: code must expect each
  click of a double-click.

## Tests

| Test file | Covers here |
|---|---|
| [test_ui_smoke.py](../../tests/test_ui_smoke.py) | dragging and Ctrl-dragging clips and undo, trimming, selecting below the tracks, the clip body setting the insert marker, title clicks setting the playback start, Shift-click ranges, drags ending in the clip band, moving clip ranges, cut/copy/paste, the ruler's loop brace and scrub zoom, Alt+wheel resizing and folding, Ctrl+Alt drag, zoom, scroll and follow, dropping files, double-click opening the clip view |
| [test_ui_automation.py](../../tests/test_ui_automation.py) | A, parameters showing their lanes, clicking on the line, dragging and bending breakpoints, segments and steps, deleting, time ranges (clear, duplicate, move), overriding and re-enabling, controls following automation, the master's lane, lanes below tracks, saving, automation moving with a dragged clip, dragging up into the clips |
| [test_ui_groups.py](../../tests/test_ui_groups.py) | Ctrl+G, folding (and its automation), the group's summary lane, dragging headers into and out of groups, a folded track's clips as bars to click and drag (its lane no grid) |
| [test_ui_clip_edits.py](../../tests/test_ui_clip_edits.py) | reversing audio clips (R, and part of a clip), clip gain (its knob's name, the waveform's height), clips heard where a drag (or trim) takes them before it ends, time selections over groups (delete, copy and paste, Ctrl+A), the live take reaching the playhead |
| [test_ui_sends.py](../../tests/test_ui_sends.py) | Ctrl+Alt+T, the returns' rows above the master, send knobs on every header, pre/post-fader, send automation lanes, deleting a return |
| [test_ui_recording.py](../../tests/test_ui_recording.py), [test_ui_resampling.py](../../tests/test_ui_resampling.py) | arm, input and monitoring controls, the input menu with resampling sources greyed out, the live waveform, takes becoming clips |
| [test_duplicate_tracks_copy_automation.py](../../tests/test_duplicate_tracks_copy_automation.py) | duplicating tracks (automation and routing following the copies), and cutting, copying and pasting automation ranges onto the selected lanes or those they came from |
