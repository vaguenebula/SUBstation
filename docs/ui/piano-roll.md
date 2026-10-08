# Piano roll and clip view

The clip view is what a double-clicked clip opens: it covers the arrangement until Esc, × or Shift+Tab go back
([README.md](README.md#the-main-window)). Audio clips get their settings and large waveforms; MIDI clips get the
piano roll (one clip, or several edited together), laid out like Ableton's MIDI editor: a ruler on top, keys on the left, the notes in the middle and
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
| [ClipView.qml](../../ui/qml/clipview/ClipView.qml) | The clip view: its header (the track's colour, the name, a line about the clips; for a MIDI clip the key and Generate; ×), the audio clips' controls and waveforms, or the piano roll |
| [ClipViewController](../../ui/src/pianoroll/ClipViewController.h) | What the clip view shows and does (the old `ClipView` without its widgets): which clips open, MIDI or audio, editing every open audio clip at once |
| [ClipWaveform](../../ui/src/pianoroll/ClipWaveform.h) | The audio clips' waveforms, each whole source file fitted to the width |
| [ClipViewKnob.qml](../../ui/qml/clipview/ClipViewKnob.qml), [ClipViewSection.qml](../../ui/qml/clipview/ClipViewSection.qml) | A captioned knob with its readout; a titled box of controls |
| [PianoRollView.qml](../../ui/qml/pianoroll/PianoRollView.qml) | The piano roll's layout: the headphones button, the ruler, the keys, the note grid with the chord lane and the note tools over it, the velocity lane, the scroll bars |
| [PianoRoll](../../ui/src/pianoroll/PianoRoll.h) | The piano roll's state (the old `PianoRoll` without its widgets): the clips it shows, its own time axis and row height, the selected notes (and the stretch a rubber band selected), the paste marker, the notes copied, the playhead, the keys sounding, the note tools' settings and actions, the song's chords and key over the clips, Generate |
| [RollItem](../../ui/src/pianoroll/RollItem.h) | The base of the piano roll's items: `session` and `roll` properties |
| [NoteGrid](../../ui/src/pianoroll/NoteGrid.h) | The notes: painting, hit-testing, mouse, wheel, keys, and its gestures (`MoveNotesGesture`, `ResizeNotesGesture`, `SelectNotesGesture`, `PanGesture`) |
| [PianoKeys](../../ui/src/pianoroll/PianoKeys.h), [PianoRuler](../../ui/src/pianoroll/PianoRuler.h), [VelocityLane](../../ui/src/pianoroll/VelocityLane.h), [RollPlayhead](../../ui/src/pianoroll/RollPlayhead.h) | The keyboard, the ruler, the velocities, and the playhead over each |
| [ChordLane](../../ui/src/pianoroll/ChordLane.h) | The song's chords along the top of the notes |
| [NoteTools.qml](../../ui/qml/pianoroll/NoteTools.qml) | The floating bar with Legato, ×2, ÷2, Quantize and Humanize (Velocity, Timing) |
| [NoteSet.h](../../ui/src/pianoroll/NoteSet.h) | `ClipNote` (a note of one of the clips shown); sets of notes and of ClipNotes as sorted vectors (`noteSet`, `clipNoteSet`, `contains`, `united`, `without`, `byTime`, `notesOf`, `tagged`, `clipsOf`) |

The note maths is in the application layer's [model/Notes.h](../../app/src/model/Notes.h) (pure functions on
`Note`s, tested without Qt): `place`, `shifted`, `resized`, `clampMove`, `withVelocity`, `legato`, `timeScaled`,
`quantized`, `humanizedTiming`, `span`, `byTime`, `kMinNoteBeats`, `kQuantizeGrids`, `kHumanizeBeats`. See
[app/model.md](../app/model.md). Humanize › Velocity's velocities are the session's humanizer's (`Session.humanizer`,
[intelligence.md](../intelligence.md#humanizing-velocities-by-machine-learning)).

## The clip view

`ClipView` is given clips (the main window sets them from `Session.arrangement.clipViewRequested`):

```qml
ClipView {
    trackId: t                      // the track plain ids in clipIds are on
    clipIds: [c]                    // or [{trackId: t, clipId: c}, ...] across tracks
    leadClipId: c                   // optional: the clip double-clicked (a MIDI lead opens the MIDI clips)
    onCloseRequested: ...           // Esc, ×, its clips all gone, the project reset
    onLocateRequested: beat => ...  // the piano roll's ruler was clicked: Session.locate(beat)
    onStatusMessage: message => ...
}
```

[ClipViewController](../../ui/src/pianoroll/ClipViewController.h) orders the clips top track first, then by time.
If the lead clip (the one double-clicked; by default the first) is a MIDI clip, every MIDI clip among them opens in
the piano roll, edited together, the lead first (`pianoRoll.setClips()`; `refs()` in that order); otherwise every
audio clip among them opens together. It shows them only while `active`
(bound to the view's `visible`): becoming active opens them afresh (the piano roll fitted to the clip again), going
inactive forgets them (a key sounding stops). Deleted clips or tracks are dropped; with none left, or on a project
reset, it emits `closeRequested` (the piano roll skips a clip that is gone, and is given the rest when its lead
goes). When clips open (`opened`) the notes (MIDI) or the view (audio, for Esc) take the
keyboard.

The header shows the lead track's colour, `name` (an audio clip's; for a MIDI clip, which has none, its track's) and
`info` ("4.00 beats · 12 notes"; several MIDI clips: "3 MIDI Clips", "on 2 tracks · 12 notes · edited together";
"2.50 s · 5.00 beats"; "on 2 tracks · changes apply to every selected clip"; with
"· deactivated (0 activates)" when every clip shown is), and a × button.

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
│ PianoKeys│ NoteGrid, ChordLane on top │ v │
│ (64 px)  │             ┌───────────┐  │ b │
│          │             │ NoteTools │  │ a │
│          │             └───────────┘  │ r │
├──────────┼────────────────────────────┼───┤
│ Velocity │ VelocityLane (72 px)       │   │
├──────────┼────────────────────────────┤   │
│          │ hbar                       │   │
└──────────┴────────────────────────────┴───┘
```

`PianoRollView` makes the `PianoRoll` (`session: Session`) and gives it to every item (`roll: pianoRoll`); a
`RollPlayhead` sits over the ruler (`ruler: true`), the note grid and the velocity lane. `ChordLane` (18 px, shown
with `Session.harmony.shown`) and `NoteTools` are children of the note grid, over it: the lane along its top, under
the playhead, taking no clicks. `focusNotes()` gives the note grid the keyboard; clicks on the keys, the ruler and
the velocities give it to the notes too (`PianoRoll::focusGrid()`). The scroll bars follow the roll except while
dragged (then the roll follows them: `scrollToX`, `scrollToY`). The ruler, the keys, the note grid and the velocity
lane have `clip: true` (and the clip view's `ClipWaveform`): a scene-graph item isn't clipped to itself, and notes or
the playhead scrolled out of the grid would show over the keys and the browser.

### Time and pitch

- **Time is roll beats**: one clip shows in its content beats (the beats of its notes, the ruler's 1 being its first
  content beat, not the arrangement's); several show in the arrangement's beats, each clip where it is in the song.
  `origin()` is the arrangement beat at roll beat 0 (one clip: `startBeat - offsetBeats`; several: 0), `shift(clip)`
  the roll beat of a clip's content beat 0, `rollStart(note)` where a note is. Notes stay in their clip's content
  beats in the model and in the roll (`ClipNote`). The roll has its own `timeline::Timeline` (see
  [arrangement.md](arrangement.md#arrangement-and-coordinates)), so `beatToX`, zoom, the adaptive grid, snapping and
  `gridLines`/`drawGrid` work as in the arrangement. Its grid level starts at 1, a little wider than the
  arrangement's (1/16 notes across a bar).
- **The part each clip plays** is from its `offsetBeats` to its `windowEnd()` (`windows()`, in roll beats): lit, with
  the rest dimmed (`kOutsideClip`), and notes outside their clip's drawn at half strength (a note plays if it starts
  in the window). Notes outside it are kept: trimming a clip hides notes, never deletes them.
- **Converting**: the playhead is `beat - origin()` while the arrangement plays inside a clip shown; the ruler's
  click goes back with `+ origin()` (`requestLocate()` → `locateRequested`). `startBeat()` (the start marker) is the
  arrangement's insert marker in roll beats when it is inside a clip shown. `pasteBeat()` (the paste marker) is the
  roll's own: where the grid was last clicked (`setPasteBeat()`), none for new clips; it never moves the start marker
  or the playhead.
- `origin()` and `shift()` are worked out in `relayout()` on every `refresh()` (any change of the clips shown), not
  looked up for each call: drawing and hit-testing ask for a note's place note by note, and `Project::findClip` is a
  linear search.
- **Pitch** runs 127 at the top to 0 at the bottom, a row of `rowHeight` pixels each (`kRowHeight` 12 by default,
  `kMinRowHeight` 5 to `kMaxRowHeight` 36): `pitchTop(pitch) = (127 - pitch) * rowHeight - scrollY`, and
  `pitchAt(y)` the inverse, clamped to 0..127. `scrollY` is in pixels.
- `zoomRows(notches, anchorY)` (Alt+wheel over the grid or the keys) changes the row height by `kRowHeightStep`
  (1.5 px) a notch, keeping the pitch under the mouse in place. It keeps an exact height (`rowHeightExact_`) so a
  trackpad's small steps add up, and rounds it to draw.

### The clips and their edits

`setClips(refs)` shows MIDI clips, the first leading (`setClip(trackId, clipId)`: one, or nothing). `clipAt(i)`
(`clip()`: the lead) looks a clip up in the project each time, so the roll never holds a stale clip (the project
replaces it on every edit, and holds tracks by value); a clip that is gone is skipped. On new clips the selection,
the marker and the tools reset and a fit is pending: `fitIfReady()` zooms to the parts the clips play (96 % of the
grid's width) and centres their notes vertically (C3, `kDefaultPitch`, without notes), but only once the grid has its
size and shows (the clip view may not be laid out yet).

A note is a `ClipNote`: its clip (an index into the roll's clips, 0 the lead) and the note in that clip's content
beats. `allNotes()` is every clip's, the lead's last (drawn on top). A note stays in its clip whatever is done to it;
a new one goes into the clip playing where it is drawn (`clipFor(beat)`: the lead if it plays there, else the first
that does, else the lead).

Every edit goes through one method:

```cpp
roll.commitNotes({{clip, notes}, ...}, text, mergeKey, selected);  // commit(notes, ...): the lead's
```

It sets `selected`, then calls `editor.setClipsNotes({{ref, notes}, ...}, text, mergeKey)`: each changed clip's whole
new list of notes, one undo command, merged with the previous one when they share the merge key. Gestures pass their
own key (a `QUuid` made at the press), so a drag is one undo step, while the model (and what plays) changes live as
the mouse moves. Every edit is heard at once, because the bridge sends the tracks' notes to the engine on
`clipsChanged`. Edits of notes in several clips work out each clip's notes on its own (`notes::place` per clip);
moves are limited so no note of any clip leaves its range.

`clipsChanged` for a track the roll shows calls `refresh()`, which drops selected notes that no longer exist (undo,
redo) and repaints (`contentChanged`).

The selection is `roll.selectedNotes()`, a set of `ClipNote`s (a sorted vector, [NoteSet.h](../../ui/src/pianoroll/NoteSet.h);
`selected()`: their notes, `setSelection(notes)`: the lead's). A `Note` is a value: a moved note is a new note, which
is why gestures pass the moved notes as the new selection. A rubber band also selects the stretch of time it covered
(`selectedSpan()`, snapped out to the grid lines around it), drawn tinted under the notes; any other new selection
forgets it.

### Note grid

[NoteGrid](../../ui/src/pianoroll/NoteGrid.h).

**Painting.** Rows (black keys `kBlackKeyRow`, lines at B|C and E|F), the grid, the dimmed outside of the clips'
windows (with several, a line in the clip's track colour at each window's ends), the selected stretch's tint, then
the notes: each in its clip's track colour (halfway to red, `Theme::kOutOfKey`, for a note out of the song's key:
`PianoRoll::outOfKey()`; grey, `Theme::kDeactivatedClip`, for a deactivated one, as its velocity stem is), more opaque for louder notes (alpha from velocity) and lighter when selected (white
outline), with the note's name when it is at least 30 by 10 px. Then the rubber band, the start marker
(`startBeat()`, a line in `kInsertMarker`) and the paste marker (`pasteBeat()`, dashed in `kPasteMarker`); the
playhead is the `RollPlayhead` over it.

**Hit-testing.** `PianoRoll::noteRect(note)` (at least 3 px wide) and `noteAt(pos)` → `Hit{note, zone}` for the
topmost note: `End` or `Start` within `kEdgeGrab` (5 px, or a quarter of a short note) of an end, else `Body`.

**Mouse.**

| Press | Gesture |
|---|---|
| Ctrl+Alt | `PanGesture`: scrolls both ways, as in the arrangement |
| empty space | `SelectNotesGesture`: a rubber band; Ctrl or Shift adds to the selection. A click (no drag) places the paste marker at the snapped beat (`setPasteBeat()`) |
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
- `SelectNotesGesture`: selects the notes the band touches (of every clip); each note sounds as the band catches it
  (`previewNotes()` with the notes it touches now and didn't before). On release, a band that was dragged selects
  them with the tools (`selectNotes(notes, true)`) and selects the stretch it covered (`setSelectedSpan()`, floored
  and ceiled to the grid).

Double-click: on a note, deletes it; elsewhere, adds a note one grid step long in the grid cell clicked (Alt: where
clicked), at the row's pitch, into the clip playing there (`clipFor()`), and auditions it. Adding or deleting a note this way never brings up the tools.

Wheel (`PianoRoll::wheel()`): Alt makes the rows taller or shorter (either axis, as Qt may report Alt+wheel as
horizontal); Ctrl zooms time around the mouse (1.2 a notch); Shift or a horizontal wheel scrolls sideways (80 px a
notch); otherwise it scrolls three rows a notch. The keys forward their wheel events to the same function.

### Keys

The grid takes `Delete`/`Backspace`, `Ctrl+A`, `Ctrl+D`, `Ctrl+C`, `Ctrl+X`, `Ctrl+V`, `Ctrl+U`, the arrow keys and
`0` (`handles()`). The main window has shortcuts for most of them too (for clips); the grid accepts their
`ShortcutOverride` event in
`event()`, so Qt delivers them to the grid as key presses instead of firing the window's actions, while it has the
focus. A hidden grid gives the focus up, so the window's shortcuts work again once the clip view goes.

| Key | Does |
|---|---|
| Ctrl+A | selects every note (of every clip), with the tools |
| Ctrl+U | `quantize()`: the selected notes, or every note when none are |
| Delete, Backspace | deletes the selected notes |
| Ctrl+D | `duplicateSelected()`: copies of the selected notes by the selected stretch's length (right after it; a rubber band's stretch reaches from where the drag started), else right after the last of them; selected, and the stretch after it selected (Ctrl+D again goes on) |
| Ctrl+C / Ctrl+X | `copySelected()` / `cutSelected()`: the selected notes copied (their track, their times from the stretch's start); cut takes them out |
| Ctrl+V | `paste()`: the copied notes at the paste marker (else right after where they were copied), each into the clip of its track that plays there (else the one there, else the lead); selected, the marker moving to their end |
| 0 | deactivates the selected notes (`notes::withActive`), or activates them if they all are; they stay selected |
| Up / Down | a semitone (Shift: an octave) |
| Left / Right | a grid step (Shift: a bar) |

Moves go through `notes::clampMove` and do nothing at the edge.

### Keys and ruler

[PianoKeys](../../ui/src/pianoroll/PianoKeys.h) draws the keyboard (black keys 60 % wide, the key sounding lit, C's
labelled with `noteName`). Pressing a key selects every note on that pitch (Shift adds) and auditions it; dragging
over the keys selects every note on the keys from the one pressed to the one under the mouse (`selectKeys()`) and
plays each key in turn; releasing stops it.

[PianoRuler](../../ui/src/pianoroll/PianoRuler.h) draws bar numbers in roll time, a bar in each clip's track colour
under the part it plays, the start marker (a triangle at the top) and the paste marker (a tab at the bottom, in
`Theme::kPasteMarker`, over the grid's dashed line). A click plays from the snapped beat (`requestLocate()`, converted to
the timeline); a drag pans or zooms as the arrangement's ruler does (decided on its first 3 px).

### Velocity lane

[VelocityLane](../../ui/src/pianoroll/VelocityLane.h) draws a stem per note at its start (every clip's, in its track's
colour), as tall as its velocity (1 to 127 over the lane's height), white for selected notes. `stemAt(x)` finds the stem nearest the mouse within
`kStemGrab` (6 px), preferring selected notes. Dragging a stem selects its note (if it wasn't) and changes every
selected note's velocity by the same amount (`notes::withVelocity`), committed live with one key; the selected
notes' values show while dragging.

### Note tools

[NoteTools.qml](../../ui/qml/pianoroll/NoteTools.qml) is a small rounded bar: a note count, Legato, ×2, ÷2, Quantize
with its grid (`quantizeGrids`: 1/4, 1/8, 1/8T, 1/16, 1/16T, 1/32; 1/16 at first) and amount (100 %), and Humanize ▾,
a menu (`humanizeMenu`) of Velocity (`humanizeVelocity`) and Timing (`humanizeTiming`), each with its own amount in a
number box at its right (`humanizeVelocityAmount`, 100 % at first; `humanizeTimingAmount`, 25 %): clicking an entry's
name humanizes, the box keeps its clicks. Velocity is greyed out ("Velocity (no model)") when the model isn't next to
the application (`velocityModelAvailable`). Its buttons and boxes never take the focus, so the notes keep the
keyboard (the menu has it while open, and gives it back to the grid when an entry is chosen); it keeps its clicks to
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

The actions are the roll's: `legato()`, `scaleTime(2.0)` / `scaleTime(0.5)`, `quantize()`, `humanizeVelocity()`,
`humanizeTiming()`. Each takes `toolTargets()` (the selected notes, or all when none are, by time on the roll),
computes the changed notes, and commits `notes::place(clip.notes, targets, changed)` for each clip as one undo step
(`applyTool()`: "Humanize Velocity", "Humanize Timing"). Legato works in each clip on its own (up to its own notes and
its end); Quantize, Humanize › Timing and ×2 / ÷2 work on the notes' times on the roll (`applyOnRoll()`: with several
clips, the arrangement's grid), each note then back in its clip. `humanizeTiming` uses the roll's own
`QRandomGenerator` (`seedRandom()` for tests); `humanizeVelocity` hands the targets (each with its track and clip) to
`Session.humanizer`, which judges each track's notes with the notes it plays around them and levels the model's
velocities to their own (nothing changes if the model can't be loaded: the status line says why). The parameters are
the roll's properties: `quantizeGrid`, `quantizeAmount`, `humanizeVelocityAmount` (how far velocities move toward the
model's), `humanizeTimingAmount` (at 100 %, notes move by up to `humanizeBeats`, a 32nd note).

### Chords, the key and Generate

The song's harmony is the session's (`Session.harmony`, [intelligence.md](../intelligence.md#harmony-the-application-side)):
its chords on the timeline and its key (the project's, else inferred from its MIDI). While it is shown (`shown`, View ›
Chords and Key, C), `refreshHarmony()` maps the chords over the part the clips play (the first one's `startBeat` to
the last one's `endBeat()` on the timeline) into roll beats, as `RollChord`s (`chords()`: start, end, name, root, minor), and keeps the key
(`scaleKey()`), on the GUI thread, for the items to draw: when the harmony says it changed (40 ms after an edit
anywhere in the song), when it is shown or hidden, when a clip opens and when the grid shows. Hidden, there are no
chords and no key, and nothing is inferred.

[ChordLane](../../ui/src/pianoroll/ChordLane.h) draws a see-through band for each chord along the grid's top, coloured
by its root around the circle of fifths from blue at C (`chordColor`; darker for minor and diminished chords), with a
line at its start and its name, kept at the lane's left while its start is scrolled away. It accepts no mouse
buttons: clicks reach the notes under it. `NoteGrid` tints notes out of the key (`outOfKey(pitch)`: not in the major,
or natural minor, scale).

The clip view's header shows the key (`keyLabel`: "Key: A Minor", "(inferred)" when the project has none) and
*Generate ▾*, a menu of *Chords* and *Bass*: `generateChords()` and `generateBass()`. Each writes into the lead clip:
it takes the song's chords over the part it plays (a progression in the key, C major without one, where there are none:
`starterProgression`), writes the part (`chordPart`, `bassPart`; bars on the timeline's) in content beats, and commits
the clip's notes with them as one undo step ("Generate Chords", "Generate Bass"), the written notes selected. The
clip's own notes win: a written note overlapping one on its key is shortened or left out (`notes::resolveOverlaps`
with the clip's notes as winners), and nothing is committed if nothing would change (Generate again).

### Hearing notes

`audition(pitch, velocity, clip)` sounds a key on the instrument of the clip's track through
`bridge.previewNote(trackId, pitch, velocity)` until `releaseAudition()` (a `previewNote` with velocity 0); one key
sounds at a time. `previewNotes(notes)` sounds notes for a moment (a rubber band's, as it catches them), each key
once on its track, as loud as its loudest note, deactivated notes left out, with those sounding already (a key
sounding isn't struck again); all of them stop `kChordPreviewMs` (400 ms) after the last were caught
(`previewing()`: the keys sounding). A key auditioned stops them. The headphones button (`preview`) turns both off. Hiding the grid or changing
the clips releases them. Preview notes go through a
lock-free queue to the next audio block, straight to the track's instrument (see [engine/midi.md](../engine/midi.md)).

### Playhead

`onPosition` (from the bridge's `positionChanged` and `transportChanged`) works out the roll-beat playhead while
playing inside a clip shown and emits `playheadChanged`; only the `RollPlayhead` items repaint, not the notes.

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

- Always go through `roll.clipAt()`; a clip is replaced on every edit.
- Notes are in their clip's content beats; draw and hit-test them with `noteRect(ClipNote)` / `rollStart()`, never
  with a bare `note.start`.
- Gestures commit from the notes as they were at the press (`base_`), not from the current clip, or a drag would
  compound.
- Notes are compared by value: two identical notes on the same key and time are one in a set. `notes::place` decides
  how overlaps resolve.

## Tests

| Test file | Covers here |
|---|---|
| [test_ui_pianoroll.cpp](../../tests/app/test_ui_pianoroll.cpp) | Through the clip view: a MIDI track with the Synth and a clip opened in the piano roll; notes drawn and heard, dragged to move, resize and copy; the notes' keys taking precedence over the window's shortcuts; the rubber band, the keys and the velocity lane; Alt+wheel and Ctrl+Alt drags; the note tools floating by notes selected by dragging (Humanize's menu: Velocity and Timing, each with its amount); the song's chords along the top (over the part the clip plays, hidden with the key, clicks going through them), notes out of the key in red, Generate › Chords and › Bass |
| [test_ui_clipview.cpp](../../tests/app/test_ui_clipview.cpp) | Audio clips: one clip opened, its settings and waveform; several edited in unison; warping and transposing reaching the audio; the clip gain making the waveform taller; which clips open with a MIDI clip among them; going back (Esc, ×, the clips deleted, the project reset) |
| [test_midi_model.cpp](../../tests/app/test_midi_model.cpp) | The note maths |
