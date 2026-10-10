# MIDI

MIDI tracks, MIDI clips and the piano roll that edits them, notes' pitch bends and
vibrato (MIDI 2.0's per-note pitch bend), MIDI input from controllers and keyboards
(MIDI 2.0 too), and the computer MIDI keyboard. Recording MIDI is in
[recording.md](recording.md). A device on any track can play a MIDI track's notes
(*MIDI From*, see [devices.md](devices.md#midi-from-another-track)).

## MIDI tracks and clips

- MIDI tracks (**Ctrl+Shift+T**) come with the built-in **Synth** (see
  [devices.md](devices.md#synth)).
- To make a MIDI clip, press **Ctrl+Shift+D** (or Ctrl+Shift+M, or use *Create › Insert
  MIDI Clip*, or right-click a MIDI track › *Insert MIDI Clip*): it fills the time
  selection, or is a bar long at the insert marker (or where you right-clicked), and
  opens in the piano roll.
- A MIDI clip is a window onto its notes, like an audio clip onto its file: trimming or
  splitting it hides notes but never deletes them. Its length is in beats, so it
  doesn't change with the tempo.
- As in Ableton, a clip plays the notes that start inside it and cuts them at its end.
- **Ctrl+J** (*Consolidate*, also in a clip's right-click menu) joins the selected MIDI
  clips on each track into one.

## The piano roll

Double-click a MIDI clip to open it in the piano roll, laid out like Ableton's MIDI
editor: keys on the left, a ruler on top, notes in the middle and a velocity lane
below.

Select several MIDI clips (on one track or several) and double-click one of them (or
press Shift+Tab) to edit them all at once: every clip's notes show, each in its
track's colour, at their places in the song (the ruler shows the song's bars, with a
bar in each clip's colour under the part it plays). Notes you edit stay in their own
clips; a note you add goes into the clip playing where you draw it (the one you
double-clicked, if it plays there). Generate writes into the clip you double-clicked.

- **Keys**: click a key to hear it and select its notes (Shift adds them to the
  selection); drag along the keys to hear them and select every note on the keys you
  drag over.
- **Ruler**: in the clip's own time (its 1 is the clip's first beat). Click to play
  from there; drag sideways to scroll, up and down to zoom.
- The part of the clip that plays is lit; the rest is dimmed.
- Notes you click, add or move are played on the track's instrument (the headphones
  button turns this off).

### Editing notes with the mouse

- **Double-click** to add a note (one grid step long) or delete one.
- **Drag** notes to move them (**Ctrl** copies, **Alt** ignores the grid), drag their
  ends to resize them. Selected notes move and resize together.
- **Drag in empty space** to select: each note sounds for a moment as the rubber band
  reaches it (a chord sounds as a chord). Ctrl- or Shift-click adds notes to the
  selection; Ctrl-clicking a selected note takes it out.
- **Click in empty space** to place the paste marker there, a dashed blue line (with a
  tab in the ruler): Ctrl+V pastes there. It doesn't move the playhead or where
  playback starts.
- **Ctrl+Alt drag** scrolls. The wheel scrolls (Shift: sideways), **Ctrl+wheel** zooms
  in time and **Alt+wheel** makes the keys' rows taller or shorter.

### Keys

| Keys | Action |
|---|---|
| Delete | Delete the selected notes |
| Ctrl+A | Select all notes |
| Ctrl+D | Duplicate the selected notes: by the stretch you dragged over to select them (from where the drag started), else right after the last of them |
| Ctrl+C / Ctrl+X / Ctrl+V | Copy / cut the selected notes; paste them at the paste marker (click the grid to place it), selected; pasting again goes on after them |
| Ctrl+U | Quantize (every note, with nothing selected) |
| 0 | Deactivate the selected notes: they show grey and aren't heard (if they all are deactivated: activate them) |
| Up / Down | Move a semitone (Shift: an octave) |
| Left / Right | Move a grid step (Shift: a bar) |
| B | Bend mode: show the notes' pitch bends to edit (again: back to the notes) |
| V | The vibrato tool (into bend mode, if it is off; again: back to drawing points) |

### Velocity lane

Drag a stem in the velocity lane to change velocities; several selected notes change
together, by the same amount.

### Note tools

Selecting a group of notes by dragging a rubber band (or with Ctrl+A) brings up a small
tool bar that glides in next to them; clicking or drawing single notes doesn't. It acts
on the selected notes and hides while you drag them. Each tool is one undo step.

- **Legato** makes each note last until the next one starts (a chord's notes together);
  the last ones reach the next note after them, or the clip's end if there is none. A
  note never runs into the next note on its own key.
- **×2** and **÷2** double or halve the notes' timing: they spread out from (or draw in
  toward) the first one, and their lengths scale with them.
- **Quantize** (Ctrl+U) moves note starts onto a grid (1/4 to 1/32, or triplets; 1/16
  by default), by an amount from 0 to 100 %. Lengths stay. With nothing selected,
  Ctrl+U quantizes every note.
- **Humanize ▾** opens a menu of two, each with its own amount at its right (drag it, or
  double-click to type one); click a name to apply it:
  - **Velocity** gives the notes the velocities a model trained on pianists'
    performances hears in them: a melody louder than its accompaniment, a chord's top
    note, accents on strong beats, the shape of a phrase. It listens to the whole track
    (the notes around the selected ones too), and the notes keep their overall
    loudness: it shapes them, it doesn't make them louder or softer. The amount is how
    far velocities move toward the model's (100 % at first: all the way). The same notes
    always get the same velocities.
  - **Timing** nudges starts at random, as a player would. At 100 % a note moves by up
    to a 32nd note; the default is 25 %. Lengths and velocities stay.

### Pitch bends and vibrato

Every note can bend: its pitch moves along a curve you draw, and vibrato can swing
around it. This is MIDI 2.0's *per-note pitch bend*: each note bends on its own, so a
chord's notes can glide apart, and one note's scoop doesn't move the others.

Press **B** (or the bend button next to the headphones, above the keys) for **bend
mode**: the notes stand back and each shows its bend as a white line over the rows, a
semitone a row, starting from the middle of the note's own row (a note that doesn't
bend shows a flat line). The bend bar shows at the grid's top right. B again goes back
to editing notes.

Curves are edited as automation envelopes are:

- **Click on a note's line** to add a point there (press and drag to place it at once).
  Points snap to the grid and to whole semitones as you drag; **Alt** places them
  freely (off the grid, between semitones).
- **Drag a point** to move it in time and pitch; a point can't pass its neighbours, nor
  leave its note. The value shows by it as you drag ("+2.00 st").
- **Click a point** to delete it. **Shift-click** or **Ctrl-click** points to select
  several (they move together, even across notes), or **drag in empty space** to select
  those in a rubber band; **Ctrl+A** selects every point. **Delete** deletes the
  selected points (in bend mode Delete never deletes notes).
- **Alt-drag between two points** to bend that segment (up bulges it upward).
- **Double-click** to put a point at the pitch you clicked, on the note whose line is
  nearest (a whole semitone; Alt: exactly where you clicked).

How a curve plays: it starts at the note's own pitch at its start, goes through its
points (from the first one, a straight line or the bend you gave each segment), then
stays at the last point's pitch to the note's end and through its release. A point at
the note's very start makes it start bent (a scoop up into it from below). Bends reach
up to 48 semitones either way.

**The vibrato tool** (**V**, or the wavy button in the bend bar) draws vibrato onto
notes:

- **Drag across a note** to give it vibrato over the stretch you drag across (on the
  grid; Alt: anywhere). Dragging up as you go makes it deeper; the depth and rate show
  as you draw.
- **Click a note** for vibrato from there to its end. **Click a vibrato** to take it
  away. A vibrato shows as an orange bar along the bottom of its note.
- The bend bar sets what a new vibrato takes: its **rate** (5.5 Hz at first), its
  **depth** (how far it swings either way: 0.50 semitones), and its **swell** (how much
  of its length it takes to reach its depth: 30 %). A vibrato keeps the settings it was
  drawn with.
- Vibrato swings around the curve you draw by hand, and dies away at its end, so the
  two go together: draw a scoop up into a note and vibrato on what follows, or add points
  later and the vibrato follows them. In bend mode the curve it swings around shows as a
  fainter line.

**Clear** in the bend bar takes away the selected notes' bends and vibrato (every
note's, with none selected). Each edit is one undo step.

Bends move with their notes: moving, quantizing, copying, ×2 / ÷2 and legato carry
them along (×2 / ÷2 stretch them in time). Trimming a note's start keeps its bend where
it was in time; points or vibrato past a note's end are kept but not heard, as notes
outside a clip are. Out of bend mode, a bent note shows its curve faintly.

What hears bends: the built-in **Synth** and **Sampler**, and VST3 instruments that
take *note expression* (VST3's per-note pitch, "tuning"). A plug-in that doesn't
follows the notes without their bends. Notes recorded from a MIDI 2.0 controller keep
the per-note bends played on them, drawn as points ([recording.md](recording.md)).

### Chords and key

SUBstation works out the song's chords and key from its MIDI and shows them in the
piano roll. **C** (*View › Chords and Key*) hides or shows them; they show at first.

- Along the top of the notes, each chord the song plays over the clip is a see-through
  band of colour, named at its start ("Am", "G7", "C/E" when its bass is E).
  Related chords have related colours. Clicks go through the bands to the notes.
- The clip view's header shows the key, and notes out of it are tinted red. The key
  is the **project key** (chosen next to the metronome in the transport bar), or, if
  it has none, the key the song's MIDI is most likely in ("Key: A Minor (inferred)").
  A minor key's notes are its natural minor scale.
- The chords come from every MIDI track you hear: a muted track, or one in a muted
  group, doesn't count, nor do deactivated clips and notes, nor drum tracks (named "Drums", "Kick", "Snare", "Hats",
  "Perc", "Claps" and the like). The lowest notes are taken as the bass: a chord over
  another of its notes is an inversion ("C/E"). They follow your edits as you make
  them.
- Chords are heard where the song plays them clearly; a melody's passing notes don't
  make chords of their own, and a melody alone is heard as the key's plain chords
  (C, F, G in C major). Chord changes on bar lines are favoured over changes between
  beats. Audio clips aren't listened to yet.

### Generate

*Generate ▾* in the clip view's header writes a part into the clip, from the song's
chords over the part the clip plays:

- **Chords**: block chords between C2 and G4, each voiced to move as little as it can
  from the one before (C, G, Am, F come out as C E G, B D G, C E A, C F A), struck
  again at every bar.
- **Bass**: each chord's bass note (its root, or an inversion's bass) in the octave from
  C1, struck with each chord and again at every bar.

Where the song has no chords yet (an empty song), it writes from a progression in the
key (C major if there is none): I V vi IV, or i VI III VII in a minor key, a chord a
bar. The notes already in the clip stay as they are: a written note that would overlap
one on the same key is shortened or left out. The written notes are selected; each
Generate is one undo step. Write the chords into one clip and the bass into another, on tracks
of their own, to hear them on different instruments.

## MIDI input

MIDI input comes from controllers and keyboards (WinMM), played live and recorded. On
Linux SUBstation has no MIDI devices: the [computer MIDI keyboard](#computer-midi-keyboard)
is the only input.

- Every MIDI input connected is used unless turned off in *Options › Preferences ›
  MIDI* (*Refresh* there finds inputs plugged in since); see
  [audio-setup.md](audio-setup.md#midi).
- A MIDI track's header has an arm button, its **MIDI From** (*All Ins*, the *Computer
  Keyboard*, one input, or *No Input*; *Configure…* opens the MIDI preferences), under
  it the channel (*All Channels* or one of the 16) and monitoring: *In* (it plays its
  input, not its clips), *Auto* (it plays its input while armed, its clips as well) or
  *Off*.
- Notes played reach the track's instrument one audio buffer after they arrive, at the
  same place in the buffer, so timing doesn't jitter.
- Keys held when the transport stops, or when a track stops hearing its input
  (disarmed, another input), are released.
- An input chosen on a track but not connected shows as *(not connected)*.
- **MIDI 2.0.** The engine takes MIDI 2.0 messages (Universal MIDI Packets) as well as
  MIDI 1.0's: notes, controllers, pressure and pitch bend play as MIDI 1.0's do (at
  MIDI 1.0's resolution), and a **per-note pitch bend** bends the one note it names, as
  a drawn bend does (48 semitones either way; resetting a note's controllers takes its
  bend away). Recorded, a note keeps its per-note bends as points. MIDI 2.0 devices
  themselves need a MIDI 2.0 driver (Windows MIDI Services), which SUBstation doesn't
  have yet ([limitations.md](limitations.md)).

## Computer MIDI keyboard

**M** (*Options › Computer MIDI Keyboard*, or ⌨ in the control bar) turns it on: the
letter keys play notes as one more MIDI input, *Computer Keyboard* (tracks on *All Ins*
hear it too; it plays and records like any input).

```
  W E   T Y U   O P          black keys
 A S D F G H J K L ; '       white keys, from C3
 Z = octave down   X = octave up
```

- A S D F G H J K L ; ' are the white keys from C3, W E T Y U O P the black keys
  between them; Z and X move an octave down and up. The button's tooltip shows the
  current octave.
- While it is on those keys don't trigger the window's shortcuts (S, A, Z), but text
  fields still get them.
- Held notes are released when it is turned off or the window loses the keyboard.

---

For developers: [../ui/piano-roll.md](../ui/piano-roll.md),
[../engine/midi.md](../engine/midi.md), [../app/model.md](../app/model.md),
[../ui/README.md](../ui/README.md#the-computer-midi-keyboard) (the computer MIDI keyboard).
