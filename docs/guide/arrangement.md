# Arrangement

The arrangement is the main view: a timeline with any number of audio and MIDI tracks,
their headers on the right (as in Ableton), and the return tracks and the master pinned
at the bottom. This page covers the timeline, clips, selecting, tracks, group tracks,
folding, the master, the title bar and the transport bar, and projects.

## The timeline

- Audio clips show their waveforms, MIDI clips a preview of their notes. An audio
  clip's title bar has its name; a MIDI clip's is blank (its track's header names it).
- The grid adapts to the zoom. **Ctrl+1** narrows it, **Ctrl+2** widens it and **Ctrl+4**
  turns snapping on or off (also in the *View* menu, and by clicking the grid label in
  the arrangement). Hold **Alt** while dragging to bypass snapping.
- **Zoom**: **+** / **−** zoom in and out, **Z** zooms to the whole arrangement,
  **Ctrl+wheel** zooms around the mouse.
- **Scroll**: the wheel scrolls up and down, **Shift+wheel** sideways, and **Ctrl+Alt
  drag** scrolls in any direction.

### The ruler

- Its top strip holds the **loop brace**: drag its body to move it, its edges to resize
  it, or drag in an empty part of the strip to draw a new loop. Double-click it to turn
  the loop on or off (or **Ctrl+L**, or the loop button).
- Its lower part works like Ableton's scrub area: click to set the playhead, drag
  vertically to zoom and horizontally to scroll.

## Clips

- **Move** clips by dragging them, also onto other tracks of the same kind (audio onto
  audio, MIDI onto MIDI). **Ctrl-drag** copies them. While playing, you hear a clip
  where you drag it to straight away; the move itself is one undo step, when you let go.
- **Trim** either edge by dragging it (heard as you drag, too).
- **Stretch** a clip by holding **Alt** over either edge (the cursor shows a two-headed
  arrow, the edge lights up orange) and dragging it: the other edge stays, and the clip plays
  the same audio (or notes) faster or slower to fill the new length. An audio clip is
  warped to do it (its *Seg. BPM* set), keeping its pitch unless its warp mode is
  *Re-Pitch*; a MIDI clip's notes are stretched with it. It snaps to
  the grid; hold **Shift** too for off the grid. An audio clip stretches as far as
  Seg. BPMs go (20 to 999).
- **Slide a clip's content** by holding **Ctrl+Shift** and dragging its waveform or its
  notes (the body, below the title bar): the clip stays where it is, as long as it is, and
  plays what is earlier or later in its file (or notes). It moves by grid steps; hold
  **Alt** as well once you are dragging for off the grid. Audio stops at the ends of its
  file.
- **Fade** an audio clip in or out: hold **F** and its fade handles show, squares at the
  top corners of its waveform (at the ends of its fades, once it has some). Drag one in to
  lengthen the fade, back out to shorten it; its length shows as you drag. Each fade longer
  than a few pixels has a dot on its curve: drag it up to bend the fade up (the level rises
  sooner, or stays up longer before it falls), down to bend it down; **Shift** for finer
  steps. Double-click the dot to straighten the curve, the square to take the fade away.
  Fades show over the waveform, and are heard as you drag. A fade stays at its clip's end
  when the clip is trimmed or stretched; splitting keeps the fade in on the left and the
  fade out on the right. While the computer MIDI keyboard is on, F plays a note instead.
- **Clicks at clip edges**: where a clip without a fade of its own starts or ends inside its
  file, it fades in or out over 4 ms so the cut doesn't click; but not where it starts at
  the file's start or plays to its end, so one-shots (drums and the like) keep their attack
  as it is. A fade of your own replaces it.
- **Split** at the insert marker with **Ctrl+E**, or right-click a clip › *Split Here*.
- **Duplicate** with **Ctrl+D**, **delete** with **Delete** (or Backspace).
- **Cut / copy / paste** with **Ctrl+X / Ctrl+C / Ctrl+V**; what was copied last is
  what pastes. Paste goes to the insert marker, or, from a lane's right-click menu,
  where you right-clicked.
- **Consolidate** (**Ctrl+J**, or the clip's right-click menu) joins the selected MIDI
  clips on each track into one.
- **Reverse** (**R**, or the clip's right-click menu) plays the selected audio clips
  backwards (just the selected part, if the selection covers part of a clip); see
  [audio-clips.md](audio-clips.md#reversing).
- **Deactivate** (**0**, or *Deactivate* in the clip's right-click menu) silences the
  selected clips, audio and MIDI, as in Ableton: they turn grey and don't play (nor count
  towards the piano roll's chords), but stay where they are, and you can still edit,
  move, copy and save them. If the selection covers part of a clip, it is split there
  and just that part is deactivated. **0** again (or *Activate*) turns them back on
  when every clip in the selection is deactivated; otherwise it deactivates the rest.
  Frozen tracks' clips stay as they are (unfreeze a track to change them). In the
  piano roll, 0 deactivates the selected notes instead (see
  [midi.md](midi.md#keys)). Consolidating MIDI clips keeps a deactivated clip's notes,
  deactivated.
- **Double-click** a clip to open it in the clip view (the piano roll for a MIDI clip;
  double-clicking one of several selected MIDI clips opens them all, edited together);
  see [audio-clips.md](audio-clips.md) and [midi.md](midi.md). **Shift+Tab** shows or
  hides the clip view, and **Esc** goes back to the arrangement.
- Overlaps follow Ableton's rule: the clip you place wins.

A clip's right-click menu has *Cut*, *Copy*, *Paste*, *Split Here*, *Duplicate*,
*Consolidate*, *Reverse*, *Deactivate* (or *Activate*) and *Delete*; right-clicking anywhere in a time selection
offers the same (but *Split Here*), for what is selected. Right-clicking an empty part
of a lane offers *Paste* (at that point), *Insert MIDI Clip* (on a MIDI track), *Insert
Audio Track*, *Insert MIDI Track* and *Delete Track*.

## Selecting

Selecting is always on the grid, which shows through the clips and runs all the way
down. A selection is everything in its stretch of time on its tracks: the clips (just
the parts inside it), and the tracks' automation. Selected, the grid is tinted; the
clips' title bars stay as they are.

- Clicking a clip's title selects the area it covers.
- **Shift-clicking** another selects the area that fully contains both (on the tracks
  between too).
- Dragging anywhere in the lanes (or below the tracks) selects a time range on the
  tracks it crosses, up or down: folded and frozen tracks too. On a group's lane it
  takes in everything in the group, its tracks too (folded away or not), though it
  shows only over the rows you dragged across.
- Started on a track's lane (its clips), a selection selects clips on every track it
  crosses; started on an automation lane it selects automation on every automation
  lane it crosses, until it goes up into the clips: then it selects clips (see
  [automation.md](automation.md#time-ranges)).
- **Shift-clicking** an empty part of a lane (or a clip's body) extends the selection
  to there: over the time between and every track (or automation lane) between, as if
  the drag that made it had gone on; holding Shift you can drag it on. With nothing
  selected, it selects from the insert marker.
- **Ctrl+A** selects from the first clip to the last, on every track.
- **Delete**, **Ctrl+X / Ctrl+C** and **Ctrl+D** act on all of it, clips and automation
  alike (unless *Options › Lock Envelopes* is on: then the automation stays where it
  is); **Ctrl+V** pastes what was copied, clips and automation, at the insert marker.
- Dragging inside a time selection moves (or, with Ctrl, copies) just that stretch,
  splitting clips at its edges; the automation goes along.
- On a frozen track these work too, the frozen audio going with the clips: see
  [mixing.md](mixing.md#freezing-and-flattening).

## Tracks

**Ctrl+T** inserts an audio track, **Ctrl+Shift+T** a MIDI track (which comes with the
built-in Synth; see [midi.md](midi.md)). Both are in the *Create* menu and in a
header's right-click menu.

### Track headers

Each track's header (on the right) is laid out as Ableton's, in columns:

- the **name column**: its name bar in the track's colour, with the fold button (drag
  the bar to move the track); below it, on a shade of its colour, the **automation
  choosers** while its automation shows (see [automation.md](automation.md)). A group's
  colour fills its name column down to its choosers;
- the **In/Out column** (*View › In/Out*, **Ctrl+Alt+I**, shows or hides it): **Audio
  From** (a MIDI track's **MIDI From**) and its channel, **In / Auto / Off** monitoring
  (see [recording.md](recording.md)), and **Audio To** and where in that track it goes
  (see [mixing.md](mixing.md#where-a-track-goes-audio-to)). A group has Audio To only;
- the **mixer column**: the **activator** (its number; switched off, the track is
  muted), **solo** and **arm**; under them **volume** and **pan** (drag them up or
  down; select one and type a number, `25L` or `C` for pan; double-click to reset),
  each filled as far as its slider is; while there are return tracks, a **send knob**
  for each return (see [mixing.md](mixing.md));
- a **meter**.

A row shows when the track is tall enough for it: a folded track shows its name bar
and the first row of each column.

**Ctrl+R** (or *Rename* in its right-click menu) renames the selected track in place;
*Color* in the menu sets its colour. Drag the bottom edge of a track to resize it.

### Track names

As in Ableton, a **#** in a track's name stands for its number: its place from the top of the
arrangement, counting every track (groups, and the tracks in them, too). A new track is named
`# Audio`, `# MIDI` or `# Group` and shows `3 Audio`; insert, delete or move a track and the
tracks below it are renumbered. Renaming shows the name with its **#**, all of it selected: keep
the **#** and the track keeps its number (`# Lead` shows `4 Lead`); leave it out and it has none.

New audio and MIDI tracks are named after what they hold, and follow it:

- an **audio track** after the file most of its clips play (a tie: the one played first), so
  dropping `Kick.wav` makes `3 Kick`; with no clips it is `# Audio` again;
- a **MIDI track** after its instrument (`2 Synth`, a plug-in's name, an instrument rack's
  name), `# MIDI` without one.

Once you rename a track to anything else, its name stays what you typed. Projects saved by an
older SUBstation have their `3 Audio`, `2 MIDI` and `1 Group` tracks named and numbered this way
when opened (an older SUBstation can't open projects saved now).

Changing the volume or pan of one of several selected tracks changes them all: by the
same amount when dragged, to the same value when typed or reset.

### Selecting, deleting and duplicating tracks

- Click a track's header to select it; **Ctrl-click** adds a track to the selection (or
  takes it out), **Shift-click** selects every track from the last one clicked.
- **Delete** deletes the selected tracks (also *Create › Delete Selected Tracks*, or
  *Delete Track(s)* in the right-click menu).
- **Ctrl+D** (with a track header clicked), or *Duplicate Track* in a header's
  right-click menu, duplicates them: the copies go together after the last of them, and
  are selected. A copy has new clips and devices (plug-ins in their state as they are)
  and the same mixer, automation, sends, inputs and sidechains (those among the copied
  tracks taken from the copies); a group is copied with what is in it. Copies aren't
  armed.

### Moving tracks

Drag track headers to move tracks (the selected ones, if you drag one of them): between
two tracks, or onto the middle of a group's header to put them in it (last). Dragged
below a group's last track they leave it. *Move Out of Group* in the right-click menu
does that too.

## Group tracks

Group tracks work as in Ableton. Select tracks and press **Ctrl+G** (*Create › Group
Tracks*, or a track header's right-click menu) to put them in a new group, where the
first of them was.

- What is in a group goes into it, through its devices (audio effects) and mixer, and
  on to the master, or the group it is in (groups nest).
- A group has no clips. Its lane shows what is in it as Ableton's do: a thin row for
  each track in it (in groups in it too), with that track's clips, so the group's
  structure shows at a glance. Folded, the rows are in the tracks' colours; open (its
  tracks show below it), just a faint outline. Its automation works as a track's. It
  has no arm or input: it records nothing.
- A new audio or MIDI track made in a group takes the group's colour.
- A group's header is a little taller than a track's, with a bar in its colour across
  its top. The tracks in it are indented under it, and a band in the group's colour
  runs down the left of the headers from the group's to its last track's (past their
  automation lanes); a group in a group has a band of its own beside it.
- **Ctrl+Shift+G** (*Ungroup Tracks*) takes a group away; what was in it stays, where it
  was.
- Deleting a group deletes what is in it.

How groups solo, mute and line up is in [mixing.md](mixing.md).

## Folding

Every track header has a fold button next to its name (or *Fold Track* / *Fold Group*
in its right-click menu).

- A track's is a triangle in a circle: folded, the track shrinks to its name row and
  shows its clips as bars (audio clips' with their names), as in Ableton: the same title bars its clips
  have unfolded, as high. Click a bar to select its
  clip, drag it to move it (Ctrl copies), drag its ends to trim it. Beside its bars the
  row is a grid as any other: a click moves the insert marker, a drag selects time
  (on it, and on the tracks it goes on to), and what is selected there is cut, copied,
  duplicated and moved as on any track.
- A group's is three bars in a circle, filled while folded: folded, it shrinks to its
  name row too (a little taller than a track's) and hides its tracks.
- Folded tracks and groups don't show their automation (lanes or choosers); unfolded,
  it is back as it was.
- With several tracks or groups selected, folding one of them folds (or unfolds) them
  all.
- **Alt+wheel** over a track or group (its header or its lane) resizes it: wheel down
  shrinks it and, once it is as small as it gets, folds it; wheel up unfolds a folded
  one, then makes it taller. One turn of the wheel acts on the track it started on.
- Folding is saved with the project, not an undo step.

## Return tracks and the master

Return tracks show apart, in compact rows above the master; see
[mixing.md](mixing.md).

The **master track** has volume, pan and audio effects: click its header to show its
chain in the device view. Its automation shows in its own lane, like a track's.

## The title bar

The window's title bar holds the menus on the left, the project's name in the middle
(with * while it has unsaved changes), and on the right the status line, where
SUBstation says what it did or why it couldn't, and the window's buttons. Drag the
window by any empty part of it, the name or the status line; double-click there to
maximize it or bring it back, right-click for the window's menu. Hovering over the
maximize button shows Windows' snap layouts.

## The transport bar

The bar under the title bar holds, from left to right:

- **Tempo** (drag it, Shift for fine steps, double-click to type) and the **time
  signature**;
- the **metronome**, the **project key** (see [audio-clips.md](audio-clips.md)) and the
  **computer MIDI keyboard** button ⌨ (see [midi.md](midi.md));
- the position (bars, beats, sixteenths), **Play**, **Stop**, **Record** and the
  **count-in** (see [recording.md](recording.md)), **Re-Enable Automation** and **Lock
  Envelopes** (see [automation.md](automation.md));
- an **oscilloscope** of the master output as it plays (as in FL Studio). It starts each
  picture at a rising zero crossing, so steady tones stand still;
- **loop**, **follow** (the view follows the playhead), the **CPU meter**, and the audio
  device: click it to open the preferences.

**Space** plays and stops (stopping returns to the start marker). The **Stop** button
stops; press it again to return to the start. **Home** goes to the start.

## Projects, undo and export

- **Undo / redo** (**Ctrl+Z** / **Ctrl+Y** or **Ctrl+Shift+Z**) cover all edits.
- Projects are `.gilproj` files (JSON): *File › New Project*, *Open…*, *Open Recent*,
  *Save* and *Save As…* (**Ctrl+Shift+S**).
- *File › Export Audio…* (**Ctrl+Shift+R**) renders the arrangement (from the start to
  the end of the last clip) or the loop region to a WAV file, 16-bit, 24-bit or 32-bit
  float. It renders in the background, its progress in a dialog; the window goes on
  meanwhile (but takes no edits, and nothing plays). **Cancel** (or Esc) stops it and
  deletes the unfinished file.
- Opening a project shows it at once; its plug-ins load after it, one at a time (see
  [plugins.md](plugins.md#projects)).

---

For developers: [../ui/arrangement.md](../ui/arrangement.md),
[../ui/README.md](../ui/README.md), [../app/model.md](../app/model.md),
[../app/serialization.md](../app/serialization.md).
