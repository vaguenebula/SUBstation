# Piano roll and clip view

The clip view is what a double-clicked clip opens: it covers the arrangement until Esc, × or Shift+Tab go back
([README.md](README.md#the-main-window)). Audio clips get their settings and large waveforms; a MIDI clip gets the
piano roll, laid out like Ableton's MIDI editor: a ruler on top, keys on the left, the notes in the middle and
velocities below. The clip view is [ui/qml/clipview](../../ui/qml/clipview) with its controller in
[ui/src/pianoroll](../../ui/src/pianoroll); the piano roll is
[PianoRollView.qml](../../ui/qml/pianoroll/PianoRollView.qml) and
[NoteTools.qml](../../ui/qml/pianoroll/NoteTools.qml) over the C++ items of
[ui/src/pianoroll](../../ui/src/pianoroll), which share one `PianoRoll`.

What the user does with them: [guide/midi.md](../guide/midi.md), [guide/audio-clips.md](../guide/audio-clips.md).
This page is about the code.

## Files

| File | What it holds |
|---|---|
| [ClipView.qml](../../ui/qml/clipview/ClipView.qml) | The clip view: its header (the track's colour, the name, a line about the clips, ×), the audio clips' controls and waveforms, or the piano roll |
| [ClipViewController](../../ui/src/pianoroll/ClipViewController.h) | What the clip view shows and does (the old `ClipView` without its widgets): which clips open, MIDI or audio, editing every open audio clip at once |
| [ClipWaveform](../../ui/src/pianoroll/ClipWaveform.h) | The audio clips' waveforms, each whole source file fitted to the width |
| [ClipViewKnob.qml](../../ui/qml/clipview/ClipViewKnob.qml), [ClipViewSection.qml](../../ui/qml/clipview/ClipViewSection.qml) | A captioned knob with its readout; a titled box of controls |
| [PianoRollView.qml](../../ui/qml/pianoroll/PianoRollView.qml) | The piano roll's layout: the headphones button, the ruler, the keys, the note grid with the note tools over it, the velocity lane, the scroll bars |
| [PianoRoll](../../ui/src/pianoroll/PianoRoll.h) | The piano roll's state (the old `PianoRoll` without its widgets): the clip, its own time axis and row height, the selected notes, the playhead, the key sounding, the note tools' settings and actions |
| [RollItem](../../ui/src/pianoroll/RollItem.h) | The base of the piano roll's items: `session` and `roll` properties |
| [NoteGrid](../../ui/src/pianoroll/NoteGrid.h) | The notes: painting, hit-testing, mouse, wheel, keys, and its gestures (`MoveNotesGesture`, `ResizeNotesGesture`, `SelectNotesGesture`, `PanGesture`) |
| [PianoKeys](../../ui/src/pianoroll/PianoKeys.h), [PianoRuler](../../ui/src/pianoroll/PianoRuler.h), [VelocityLane](../../ui/src/pianoroll/VelocityLane.h), [RollPlayhead](../../ui/src/pianoroll/RollPlayhead.h) | The keyboard, the ruler, the velocities, and the playhead over each |
| [NoteTools.qml](../../ui/qml/pianoroll/NoteTools.qml) | The floating bar with Legato, ×2, ÷2, Quantize and Humanize |
| [NoteSet.h](../../ui/src/pianoroll/NoteSet.h) | Sets of notes as sorted vectors (`noteSet`, `contains`, `united`, `without`, `byTime`) |

The note maths is in the application layer's [model/Notes.h](../../app/src/model/Notes.h) (pure functions on
`Note`s, tested without Qt): `place`, `shifted`, `resized`, `clampMove`, `withVelocity`, `legato`, `timeScaled`,
`quantized`, `humanized`, `span`, `byTime`, `kMinNoteBeats`, `kQuantizeGrids`, `kHumanizeBeats`,
`kHumanizeVelocity`. See [app/model.md](../app/model.md).

## The clip view

`ClipView` is given clips (the main window sets them from `Session.arrangement.clipViewRequested`):

```qml
ClipView {
    trackId: t                      // the track plain ids in clipIds are on
    clipIds: [c]                    // or [{trackId: t, clipId: c}, ...] across tracks
    leadClipId: c                   // optional: the clip double-clicked (a MIDI lead opens alone)
    onCloseRequested: ...           // Esc, ×, its clips all gone, the project reset
    onLocateRequested: beat => ...  // the piano roll's ruler was clicked: Session.locate(beat)
    onStatusMessage: message => ...
}
```

[ClipViewController](../../ui/src/pianoroll/ClipViewController.h) orders the clips top track first, then by time.
If the lead clip (the one double-clicked; by default the first) is a MIDI clip, it alone opens, in the piano roll
(`pianoRoll.setClip()`); otherwise every audio clip among them opens together. It shows them only while `active`
(bound to the view's `visible`): becoming active opens them afresh (the piano roll fitted to the clip again), going
inactive forgets them (a key sounding stops). Deleted clips or tracks are dropped; with none left, or on a project
reset, it emits `closeRequested`. When clips open (`opened`) the notes (MIDI) or the view (audio, for Esc) take the
keyboard.

The header shows the lead track's colour, `name` and `info` ("4.00 beats · 12 notes"; "2.50 s · 5.00 beats"; "on 2
tracks · changes apply to every selected clip"), and a × button.

### Audio clips

The left column (`kControlsWidth` 260 px, scrollable) has three `ClipViewSection`s:

- **Warp**: the Warp switch, the warp mode (`warpModes`, a tooltip each from `warpModeTips`; "Mixed" when the clips
  differ), *Seg. BPM* (`ValueBox`, 20 to 999), and :2 / ×2.
- **Pitch**: Transpose (±48 semitones, whole numbers) and Detune (±50 cents). Greyed out, with a tooltip saying
  why, when every clip is warped in Re-Pitch (`repitch`).
- **Mix**: clip gain (-70 to +24 dB; the waveforms are drawn scaled by it) and pan.

Every control edits all open clips through `editor.updateClips(refs, change, text, mergeKey)` with a function from
clip to new clip:

- Switches, the mode and the BPM set the same value on all (`setWarp`, `setWarpMode`, `setSegmentBpm`).
- Turning Warp on sets a clip's segment BPM to the project tempo if it was never set, so nothing moves until the
  tempo changes.
- :2 and ×2 scale each clip's own segment BPM (`scaleBpm(0.5)`, `scaleBpm(2.0)`).
- The knobs (`knobs`: a map per knob of caption, range, default, value, text, differs) sit on the first clip's value
  and their readout shows the range when the clips differ. Turning one (`nudge(knob, value, gestureKey)`) moves
  every clip by the lead's change, measured from where the gesture started (the baseline keeps each clip's value at
  the gesture's first event), so a clip held at a limit keeps its offset to the others when the knob comes back.

The right side, [ClipWaveform](../../ui/src/pianoroll/ClipWaveform.h), draws each clip's whole source file fitted to
the width, in its track's colour and scaled by its gain (rounded to 0.1 dB), the part the clip plays tinted, the rest
dimmed, and S and E flags at its start and end, as in Ableton's sample editor. Columns come from the source's peaks
(or its samples, zoomed in that far), blurred lightly when from peaks, as the arrangement's tiles are, but drawn
without a cache: the whole file is one width. One clip gets a time ruler (steps from a fixed list, at least 70 px
apart: `formatTime()`); several are stacked in bands of at least `kMinBandHeight` (40 px), labelled, with "+N more"
when they don't fit. What it draws (each clip's colour, its source, why it couldn't be loaded) is worked out on the
GUI thread.

Warping and its modes in the engine: [engine/warp.md](../engine/warp.md).

## The piano roll

### Layout

```
┌──────────┬────────────────────────────┬───┐
│ preview  │ PianoRuler (24 px)         │   │
├──────────┼────────────────────────────┼───┤
│ PianoKeys│ NoteGrid    ┌───────────┐  │ v │
│ (64 px)  │             │ NoteTools │  │ b │
│          │             └───────────┘  │ a │
├──────────┼────────────────────────────┼───┤
│ Velocity │ VelocityLane (72 px)       │   │
├──────────┼────────────────────────────┤   │
│          │ hbar                       │   │
└──────────┴────────────────────────────┴───┘
```

`PianoRollView` makes the `PianoRoll` (`session: Session`) and gives it to every item (`roll: pianoRoll`); a
`RollPlayhead` sits over the ruler (`ruler: true`), the note grid and the velocity lane. `NoteTools` is a child of
the note grid, floating over it. `focusNotes()` gives the note grid the keyboard; clicks on the keys, the ruler and
the velocities give it to the notes too (`PianoRoll::focusGrid()`). The scroll bars follow the roll except while
dragged (then the roll follows them: `scrollToX`, `scrollToY`). The ruler, the keys, the note grid and the velocity
lane have `clip: true` (and the clip view's `ClipWaveform`): a scene-graph item isn't clipped to itself, and notes or
the playhead scrolled out of the grid would show over the keys and the browser.

### Time and pitch

- **Time is content beats**: beats of the clip's notes, the ruler's 1 being the clip's first content beat, not the
  arrangement's. The roll has its own `timeline::Timeline` (see
  [arrangement.md](arrangement.md#arrangement-and-coordinates)), so `beatToX`, zoom, the adaptive grid, snapping and
  `gridLines`/`drawGrid` work as in the arrangement. Its grid level starts at 1, a little wider than the
  arrangement's (1/16 notes across a bar).
- **The part the clip plays** is from `clip.offsetBeats` to `clip.windowEnd()`: lit, with the rest dimmed
  (`kOutsideClip`), and notes outside it drawn at half strength (a note plays if it starts in the window). Notes
  outside it are kept: trimming a clip hides notes, never deletes them.
- **Converting**: the playhead is `beat - clip.startBeat + clip.offsetBeats` while the arrangement plays inside the
  clip; the ruler's click goes back with `clip.toTimeline(beat)` (`requestLocate()` → `locateRequested`).
  `startBeat()` puts the arrangement's insert marker into content beats when it is inside the clip, for the start
  marker.
- **Pitch** runs 127 at the top to 0 at the bottom, a row of `rowHeight` pixels each (`kRowHeight` 12 by default,
  `kMinRowHeight` 5 to `kMaxRowHeight` 36): `pitchTop(pitch) = (127 - pitch) * rowHeight - scrollY`, and
  `pitchAt(y)` the inverse, clamped to 0..127. `scrollY` is in pixels.
- `zoomRows(notches, anchorY)` (Alt+wheel over the grid or the keys) changes the row height by `kRowHeightStep`
  (1.5 px) a notch, keeping the pitch under the mouse in place. It keeps an exact height (`rowHeightExact_`) so a
  trackpad's small steps add up, and rounds it to draw.

### The clip and its edits

`setClip(trackId, clipId)` shows a MIDI clip (or nothing). `clip()` looks it up in the project each time, so the
roll never holds a stale clip (the project replaces it on every edit, and holds tracks by value). On a new clip the
selection and the tools reset and a fit is pending: `fitIfReady()` zooms to the part the clip plays (96 % of the
grid's width) and centres its notes vertically (C3, `kDefaultPitch`, for an empty clip), but only once the grid has
its size and shows (the clip view may not be laid out yet).

Every edit goes through one method:

```cpp
roll.commit(clipNotes, text, mergeKey, selected);
```

It sets `selected`, then calls `editor.setClipNotes({trackId, clipId}, clipNotes, text, mergeKey)`: the clip's whole
new list of notes, one undo command, merged with the previous one when they share the merge key. Gestures pass their
own key (a `QUuid` made at the press), so a drag is one undo step, while the model (and what plays) changes live as
the mouse moves. Every edit is heard at once, because the bridge sends the track's notes to the engine on
`clipsChanged`.

`clipsChanged` for the roll's track calls `refresh()`, which drops selected notes that no longer exist (undo, redo)
and repaints (`contentChanged`).

The selection is `roll.selected()`, a set of `Note`s (a sorted vector, [NoteSet.h](../../ui/src/pianoroll/NoteSet.h)).
A `Note` is a value: a moved note is a new note, which is why gestures pass the moved notes as the new selection.

### Note grid

[NoteGrid](../../ui/src/pianoroll/NoteGrid.h).

**Painting.** Rows (black keys `kBlackKeyRow`, lines at B|C and E|F), the grid, the dimmed outside of the clip's
window, then the notes: in the track's colour, more opaque for louder notes (alpha from velocity) and lighter when
selected (white outline), with the note's name when it is at least 30 by 10 px. Then the rubber band and the start
marker; the playhead is the `RollPlayhead` over it.

**Hit-testing.** `PianoRoll::noteRect(note)` (at least 3 px wide) and `noteAt(pos)` → `Hit{note, zone}` for the
topmost note: `End` or `Start` within `kEdgeGrab` (5 px, or a quarter of a short note) of an end, else `Body`.

**Mouse.**

| Press | Gesture |
|---|---|
| Ctrl+Alt | `PanGesture`: scrolls both ways, as in the arrangement |
| empty space | `SelectNotesGesture`: a rubber band; Ctrl or Shift adds to the selection |
| a note's body | `MoveNotesGesture` on the selected notes (the note is selected first if it wasn't) |
| a note's end | `ResizeNotesGesture` on the selected notes |

A press on a note auditions it. Clicking one of several selected notes selects just it, and Ctrl-clicking a selected
note deselects it, but only when the mouse comes up without dragging (`selectOnClick_`): dragging moves the whole
selection instead.

Gestures start after `kDragThreshold` (3 px):

- `MoveNotesGesture`: the time delta is snapped against the grabbed note's start (Alt: off the grid), the pitch delta
  from the rows moved; `notes::clampMove` keeps every note in range. Ctrl copies (the originals stay). Each move
  commits `notes::place(base, moving, moved)` from the notes as they were at the press, and auditions the grabbed
  note at its new pitch.
- `ResizeNotesGesture`: moves the grabbed edge, snapped; the shortest length is a grid step while snapping, else
  `kMinNoteBeats`; `notes::resized` applies it to all.
- `SelectNotesGesture`: selects the notes the band touches. On release, a band that was dragged selects them with
  the tools (`setSelection(notes, true)`).

Double-click: on a note, deletes it; elsewhere, adds a note one grid step long in the grid cell clicked (Alt: where
clicked), at the row's pitch, and auditions it. Adding or deleting a note this way never brings up the tools.

Wheel (`PianoRoll::wheel()`): Alt makes the rows taller or shorter (either axis, as Qt may report Alt+wheel as
horizontal); Ctrl zooms time around the mouse (1.2 a notch); Shift or a horizontal wheel scrolls sideways (80 px a
notch); otherwise it scrolls three rows a notch. The keys forward their wheel events to the same function.

### Keys

The grid takes `Delete`/`Backspace`, `Ctrl+A`, `Ctrl+D`, `Ctrl+U`, and the arrow keys (`handles()`). The main window
has shortcuts for Delete, Ctrl+A and Ctrl+D too (for clips); the grid accepts their `ShortcutOverride` event in
`event()`, so Qt delivers them to the grid as key presses instead of firing the window's actions, while it has the
focus. A hidden grid gives the focus up, so the window's shortcuts work again once the clip view goes.

| Key | Does |
|---|---|
| Ctrl+A | selects every note, with the tools |
| Ctrl+U | `quantize()`: the selected notes, or every note when none are |
| Delete, Backspace | deletes the selected notes |
| Ctrl+D | copies the selected notes right after them (`notes::span`), selected |
| Up / Down | a semitone (Shift: an octave) |
| Left / Right | a grid step (Shift: a bar) |

Moves go through `notes::clampMove` and do nothing at the edge.

### Keys and ruler

[PianoKeys](../../ui/src/pianoroll/PianoKeys.h) draws the keyboard (black keys 60 % wide, the key sounding lit, C's
labelled with `noteName`). Pressing a key selects every note on that pitch (Shift adds) and auditions it; dragging
over the keys plays each in turn; releasing stops it.

[PianoRuler](../../ui/src/pianoroll/PianoRuler.h) draws bar numbers in content time, a bar in the track's colour under
the part the clip plays, and the start marker. A click plays from the snapped beat (`requestLocate()`, converted to
the timeline); a drag pans or zooms as the arrangement's ruler does (decided on its first 3 px).

### Velocity lane

[VelocityLane](../../ui/src/pianoroll/VelocityLane.h) draws a stem per note at its start, as tall as its velocity (1 to
127 over the lane's height), white for selected notes. `stemAt(x)` finds the stem nearest the mouse within
`kStemGrab` (6 px), preferring selected notes. Dragging a stem selects its note (if it wasn't) and changes every
selected note's velocity by the same amount (`notes::withVelocity`), committed live with one key; the selected
notes' values show while dragging.

### Note tools

[NoteTools.qml](../../ui/qml/pianoroll/NoteTools.qml) is a small rounded bar: a note count, Legato, ×2, ÷2, Quantize
with its grid (`quantizeGrids`: 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32; 1/16 at first) and amount (100 %), Humanize with
its amount (25 %). Its buttons and boxes never take the focus, so the notes keep the keyboard; it keeps its clicks to
itself (the grid is underneath), while wheel turns outside its boxes still scroll the grid.

When it shows is the roll's: `toolsWanted` is set by `setSelection(notes, true)` (a rubber band that was dragged, or
Ctrl+A) and cleared by any other new selection (a click, a new clip, adding or deleting a note with a double-click,
an empty selection); edits through `commit()` keep it, so the bar stays while that group is edited. `placeTools()`
(on every repaint, scroll and release) sets `toolsShown`, `toolsArea` (the selected notes' bounding rectangle) and
`toolsCount` while it is wanted and no drag is under way (`NoteGrid::dragging()`), else hides it. Clicking or drawing
single notes doesn't bring it up.

The bar sits centred above the notes (below them when there is no room), kept inside the grid. It fades in rising
8 px (180 ms, ease out) and fades out sinking (120 ms); a reversed animation goes on from where the last one left
off, taking the rest of its time.

The actions are the roll's: `legato()`, `scaleTime(2.0)` / `scaleTime(0.5)`, `quantize()`, `humanize()`. Each takes
`toolTargets()` (the selected notes, or all when none are, by time), computes the changed notes with the matching
`notes::` function, and commits `notes::place(clip.notes, targets, changed)` as one undo step. `humanize` uses the
roll's own `QRandomGenerator` (`seedRandom()` for tests). The parameters are the roll's properties: `quantizeGrid`,
`quantizeAmount`, `humanizeAmount` (at 100 %, notes move by up to `humanizeBeats`, a 32nd note, and velocities change
by up to `humanizeVelocity`, 24).

### Hearing notes

`audition(pitch, velocity)` sounds a key on the track's instrument through `bridge.previewNote(trackId, pitch,
velocity)` until `releaseAudition()` (a `previewNote` with velocity 0); one key sounds at a time. The headphones
button (`preview`) turns it off. Hiding the grid or changing the clip releases it. Preview notes go through a
lock-free queue to the next audio block, straight to the track's instrument (see [engine/midi.md](../engine/midi.md)).

### Playhead

`onPosition` (from the bridge's `positionChanged` and `transportChanged`) works out the content-beat playhead while
playing inside the clip and emits `playheadChanged`; only the `RollPlayhead` items repaint, not the notes.

## Extending it

- **A new note tool**: write the pure function in [model/Notes.h](../../app/src/model/Notes.h) (notes in, changed
  notes out), add a `Q_INVOKABLE` on `PianoRoll` that calls `applyTool(targets, changed, text)`, and a button in
  NoteTools.qml.
- **A new gesture**: subclass `NoteGrid::Gesture` in [NoteGrid.cpp](../../ui/src/pianoroll/NoteGrid.cpp); use
  `started(pos)` for the drag threshold, commit with the gesture's `key`, and give `rubberBand()` if it draws one.
- **A new key**: add it to `NoteGrid::handles()` (so it overrides a window shortcut with the same key) and to
  `keyPressEvent`.
- **A new clip setting**: a method on `ClipViewController` that calls `update(change, text, mergeKey)` with a function
  from clip to clip (or a knob in its `knobs` table and `nudge()`), and its control in ClipView.qml.

## Gotchas

- Always go through `roll.clip()`; the clip is replaced on every edit.
- Gestures commit from the notes as they were at the press (`base_`), not from the current clip, or a drag would
  compound.
- Notes are compared by value: two identical notes on the same key and time are one in a set. `notes::place` decides
  how overlaps resolve.
- The piano roll shows one clip. Several MIDI clips at once aren't supported (see
  [guide/limitations.md](../guide/limitations.md)).

## Tests

| Test file | Covers here |
|---|---|
| [test_ui_pianoroll.cpp](../../tests/app/test_ui_pianoroll.cpp) | Through the clip view: a MIDI track with the Synth and a clip opened in the piano roll; notes drawn and heard, dragged to move, resize and copy; the notes' keys taking precedence over the window's shortcuts; the rubber band, the keys and the velocity lane; Alt+wheel and Ctrl+Alt drags; the note tools floating by notes selected by dragging |
| [test_ui_clipview.cpp](../../tests/app/test_ui_clipview.cpp) | Audio clips: one clip opened, its settings and waveform; several edited in unison; warping and transposing reaching the audio; the clip gain making the waveform taller; which clips open with a MIDI clip among them; going back (Esc, ×, the clips deleted, the project reset) |
| [test_midi_model.cpp](../../tests/app/test_midi_model.cpp) | The note maths |
