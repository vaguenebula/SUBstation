# Piano roll

The piano roll is the clip view of a MIDI clip, laid out like Ableton's MIDI editor: a
ruler on top, keys on the left, the notes in the middle and velocities below. It lives
in [src/substation/ui/piano_roll](../../src/substation/ui/piano_roll) and is shown by
the clip view (see [device-view.md](device-view.md#clip-view)) when the clip opened
first is a MIDI clip.

What the user does with it: [guide/midi.md](../guide/midi.md). This page is about the
code.

## Files

| File | What it holds |
|---|---|
| [piano_roll.py](../../src/substation/ui/piano_roll/piano_roll.py) | `PianoRoll` (the container: the clip, its own `ViewState`, the selected notes, geometry, scroll bars, playhead, auditioning, the tools' actions), `PianoRuler`, `PianoKeys` |
| [note_grid.py](../../src/substation/ui/piano_roll/note_grid.py) | `NoteGrid` (painting notes, hit-testing, mouse, wheel, keys) and its gestures: `NoteGesture`, `MoveNotesGesture`, `ResizeNotesGesture`, `SelectNotesGesture`, `PanGesture` |
| [velocity_lane.py](../../src/substation/ui/piano_roll/velocity_lane.py) | `VelocityLane`: a stem per note, dragged to change velocities |
| [note_tools.py](../../src/substation/ui/piano_roll/note_tools.py) | `NoteTools`: the floating bar with Legato, ×2, ÷2, Quantize and Humanize |
| [\_\_init\_\_.py](../../src/substation/ui/piano_roll/__init__.py) | exports `PianoRoll` |

The note maths is in [model/notes.py](../../src/substation/model/notes.py) (pure
functions on `Note`s, tested without Qt): `place`, `shifted`, `resized`,
`clamp_move`, `with_velocity`, `legato`, `time_scaled`, `quantized`, `humanized`,
`span`, `by_time`, `MIN_NOTE_BEATS`, `QUANTIZE_GRIDS`, `HUMANIZE_BEATS`,
`HUMANIZE_VELOCITY`. See [python/model.md](../python/model.md).

## Layout

```
┌──────────┬──────────────────────────────────────────┬───┐
│ preview  │ PianoRuler (24 px)                       │   │
├──────────┼──────────────────────────────────────────┼───┤
│ PianoKeys│ NoteGrid              ┌───────────────┐  │ v │
│ (64 px)  │                       │ NoteTools     │  │ b │
│          │                       └───────────────┘  │ a │
│          │                                          │ r │
├──────────┼──────────────────────────────────────────┼───┤
│ Velocity │ VelocityLane (72 px)                     │   │
├──────────┼──────────────────────────────────────────┤   │
│          │ hbar                                     │   │
└──────────┴──────────────────────────────────────────┴───┘
```

`NoteTools` is a child of the note grid, floating over it. The roll's focus proxy is
the grid, so focusing the piano roll focuses the notes.

## Time and pitch

- **Time is content beats**: beats of the clip's notes, the ruler's 1 being the clip's
  first content beat, not the arrangement's. The roll has its own `ViewState` (from
  [arrangement/view_state.py](../../src/substation/ui/arrangement/view_state.py)), so
  `beat_to_x`, zoom, the adaptive grid, snapping and `grid_lines`/`draw_grid` work as
  in the arrangement. Its `grid_level` starts at 1, a little wider than the
  arrangement's (1/16 notes across a bar).
- **The part the clip plays** is from `clip.offset_beats` to `clip.window_end`: lit,
  with the rest dimmed (`theme.OUTSIDE_CLIP`), and its notes drawn at half strength
  (a note plays if it starts in the window). Notes outside it are kept: trimming a clip
  hides notes, never deletes them.
- Converting: the playhead is `beat - clip.start_beat + clip.offset_beats` while the
  arrangement's position is inside the clip; the ruler's click goes back with
  `clip.to_timeline(beat)`. `start_beat()` puts the arrangement's insert marker into
  content beats when it is inside the clip, for the start marker.
- **Pitch** runs 127 at the top to 0 at the bottom, a row of `row_height` pixels each
  (12 by default, 5 to 36): `pitch_top(pitch) = (127 - pitch) * row_height -
  scroll_y`, and `pitch_at(y)` the inverse, clamped to 0..127. `scroll_y` is in pixels.
- `zoom_rows(notches, anchor_y)` (Alt+wheel over the grid or keys) changes the row
  height by 1.5 px a notch, keeping the pitch under the mouse in place. It keeps an
  exact float height (`_row_height_exact`) so a trackpad's small steps add up, and
  rounds it to draw.

## The clip and its edits

`set_clip(track_id, clip_id)` shows a MIDI clip (or nothing). `clip()` looks it up each
time from the project, so the roll never holds a stale `MidiClip`. On a new clip the
selection and tools reset and a fit is pending: `_fit_if_ready()` zooms to the part
the clip plays (96 % of the grid's width) and centres its notes vertically (C3 for an
empty clip), but only once the grid has its size and is visible (the clip view may not
be laid out yet).

Every edit goes through one method:

```python
roll.commit(clip_notes, text, merge_key=None, selected=None)
```

It sets `selected`, then calls `editor.set_clip_notes((track_id, clip_id), clip_notes,
text, merge_key)`: the clip's whole new list of notes, one undo command, merged with
the previous one when they share `merge_key`. Gestures pass their own key, so a drag is
one undo step, while the model (and what plays) changes live as the mouse moves. Every
edit is heard at once, because the bridge sends the track's notes to the engine on
`clips_changed`.

`project.clips_changed` for the roll's track calls `refresh()`, which drops selected
notes that no longer exist (undo, redo) and repaints.

The selection is `roll.selected`, a set of `Note`s. `Note` is a frozen value: a moved
note is a new `Note`, which is why gestures pass the moved notes as the new selection.

## Note grid

[note_grid.py](../../src/substation/ui/piano_roll/note_grid.py).

### Painting

Rows (black keys darker, lines at B|C and E|F), the grid, the dimmed outside of the
clip's window, then the notes: in the track's colour, brighter for louder notes
(alpha from velocity) and when selected (white outline), with the note's name when it
is at least 30 by 10 px. Then the rubber band, the start marker and the playhead.

### Hit-testing

`note_rect(note)` (at least 3 px wide) and `note_at(pos)` → `(note, zone)` for the
topmost note: `end` or `start` within `EDGE_GRAB` (5 px, or a quarter of a short note)
of an end, else `body`.

### Mouse

| Press | Gesture |
|---|---|
| Ctrl+Alt | `PanGesture`: scrolls both ways, as in the arrangement |
| empty space | `SelectNotesGesture`: a rubber band; Ctrl or Shift adds to the selection |
| a note's body | `MoveNotesGesture` on the selected notes (the note is selected first if it wasn't) |
| a note's end | `ResizeNotesGesture` on the selected notes |

A press on a note auditions it. Clicking one of several selected notes selects just
it, and Ctrl-clicking a selected note deselects it, but only when the mouse comes up
without dragging (`_select_on_click`): dragging moves the whole selection instead.

Gestures start after `DRAG_THRESHOLD` (3 px):

- `MoveNotesGesture`: the time delta is snapped against the grabbed note's start (Alt:
  off the grid), the pitch delta from the rows moved; `notes.clamp_move` keeps every
  note in range. Ctrl copies (the originals stay). Each move commits
  `notes.place(base, moving, moved)` from the notes as they were at the press, and
  auditions the grabbed note at its new pitch.
- `ResizeNotesGesture`: moves the grabbed edge, snapped; the shortest length is a grid
  step while snapping, else `MIN_NOTE_BEATS`; `notes.resized` applies it to all.
- `SelectNotesGesture`: selects the notes the band touches. On release, a band that
  was dragged sets `tools=True`, which brings up the note tools.

Double-click: on a note, deletes it; elsewhere, adds a note one grid step long in the
grid cell clicked (Alt: where clicked), at the row's pitch, and auditions it. Adding
or deleting a note this way never brings up the tools.

Wheel: Alt makes the rows taller or shorter (either axis, as Qt may report Alt+wheel
as horizontal); Ctrl zooms time around the mouse; Shift or a horizontal wheel scrolls
sideways; otherwise it scrolls three rows a notch. The keys forward their wheel events
to the grid.

### Keys

The grid takes `Delete`/`Backspace`, `Ctrl+A`, `Ctrl+D`, `Ctrl+U`, and the arrow keys.
The main window has shortcuts for Delete, Ctrl+A and Ctrl+D too (for clips); the grid
accepts their `ShortcutOverride` event in `event()` (`_handles`), so Qt delivers them
to the grid as key presses instead of firing the window's actions.

| Key | Does |
|---|---|
| Ctrl+A | selects every note, with the tools |
| Ctrl+U | `roll.quantize()`: the selected notes, or every note when none are |
| Delete, Backspace | deletes the selected notes |
| Ctrl+D | copies the selected notes right after them (`notes.span`), selected |
| Up / Down | a semitone (Shift: an octave) |
| Left / Right | a grid step (Shift: a bar) |

Moves go through `notes.clamp_move` and do nothing at the edge.

## Keys and ruler

`PianoKeys` draws the keyboard (black keys 60 % wide, the key sounding lit, C's
labelled with `note_name`). Pressing a key selects every note on that pitch (Shift
adds) and auditions it; dragging over the keys plays each in turn; releasing stops it.

`PianoRuler` draws bar numbers in content time, a bar in the track's colour under the
part the clip plays, the start marker and the playhead. A click emits
`locate_requested` with the snapped beat converted to the timeline (playback starts
there); a drag pans or zooms as the arrangement's ruler does (decided on its first
3 px).

## Velocity lane

`VelocityLane` draws a stem per note at its start, as tall as its velocity (1 to 127
over the lane's height), white for selected notes. `stem_at(x)` finds the stem nearest
the mouse within `STEM_GRAB` (6 px), preferring selected notes. Dragging a stem
selects its note (if it wasn't) and changes every selected note's velocity by the same
amount (`notes.with_velocity`), committed live with one key; the selected notes'
values show while dragging.

## Note tools

`NoteTools` ([note_tools.py](../../src/substation/ui/piano_roll/note_tools.py)) is a
small rounded bar: a note count, Legato, ×2, ÷2, Quantize with a grid combo
(`QUANTIZE_GRIDS`, 1/16 by default) and an amount `ValueBox` (100 %), Humanize with its
amount (25 %). Its buttons never take the focus, so the notes keep the keyboard.

When it shows: `roll.tools_wanted` is set by `set_selection(..., tools=True)` (a
rubber band that was dragged, or Ctrl+A) and cleared by a new clip, by adding or
deleting a note with a double-click, and by an empty selection. `place_tools()` (on
every repaint, scroll and release) shows it next to the selected notes' bounding
rectangle while it is wanted and no drag is under way (`grid.dragging()`), else hides
it. Clicking or drawing single notes doesn't bring it up.

`show_near(rect, count)` puts it centred above the notes (below them when there is no
room), kept inside the grid. It fades in rising 8 px (`QVariantAnimation` driving a
`QGraphicsOpacityEffect`, 180 ms, ease out) and fades out sinking (120 ms); a reversed
animation starts from where the last one left off. The opacity effect is turned off
once fully shown, so the bar is drawn crisp. It accepts its own mouse presses, so
clicks on it never reach the grid underneath.

The actions are on the roll: `legato()`, `scale_time(2.0 / 0.5)`, `quantize()`,
`humanize()`. Each takes `tool_targets()` (the selected notes, or all when none are),
computes the changed notes with the matching `model.notes` function, and commits
`notes.place(clip.notes, targets, changed)` as one undo step. `humanize` uses the
roll's own `random.Random`. The parameters come from the bar: `quantize_step`,
`quantize_amount`, `humanize_level`.

## Hearing notes

`audition(pitch, velocity)` sounds a key on the track's instrument through
`bridge.preview_note(track_id, pitch, velocity)` until `release_audition()` (a
`preview_note` with velocity 0); one key sounds at a time. The headphones button turns
it off. Hiding the roll or changing the clip releases it. Preview notes go through a
lock-free queue to the next audio block, straight to the track's instrument (see
[engine/midi.md](../engine/midi.md)).

## Playhead

`_on_position` (from `bridge.position_changed` and `transport_changed`) works out the
content-beat playhead while playing inside the clip, and repaints only 13 px strips of
the ruler, grid and velocity lane where it was and where it is.

## Extending it

- **A new note tool**: write the pure function in `model/notes.py` (notes in, changed
  notes out), add a method on `PianoRoll` that calls `_apply_tool(targets, changed,
  text)`, and a button in `NoteTools`.
- **A new gesture**: subclass `NoteGesture`; use `started(pos)` for the drag
  threshold, commit with the gesture's `key`, and give `rubber_band()` if it draws
  one.
- **A new key**: add it to `NoteGrid._handles` (so it overrides a window shortcut with
  the same key) and to `keyPressEvent`.

## Gotchas

- Always go through `roll.clip()`; the clip object is replaced on every edit.
- Gestures commit from the notes as they were at the press (`self.base`), not from the
  current clip, or a drag would compound.
- `Note` is hashable and compared by value: two identical notes on the same key and
  time are one in a set. `notes.place` decides how overlaps resolve.
- The piano roll shows one clip. Several MIDI clips at once aren't supported (see
  [guide/limitations.md](../guide/limitations.md)).

## Tests

[test_ui_midi.py](../../tests/test_ui_midi.py) drives the real window offscreen: a MIDI
track with the Synth and a clip opened in the piano roll, notes drawn being heard,
dragging to move, resize and copy notes, piano roll keys taking precedence over the
arrangement's shortcuts, the rubber band, keys and velocity lane, Alt+wheel on the
keys and Ctrl+Alt drag, the note tools floating by notes selected by dragging (it
waits `SHOW_MS`), MIDI clips in the arrangement, instruments from the browser, and
inserting MIDI clips. The note maths is covered by
[test_midi_model.py](../../tests/test_midi_model.py).
