# Arrangement

The arrangement is the main view: a timeline with any number of audio and MIDI tracks,
their headers on the right (as in Ableton), and the return tracks and the master pinned
at the bottom. This page covers the timeline, clips, selecting, tracks, group tracks,
folding, the master, the transport bar, and projects.

## The timeline

- Audio clips show their waveforms, MIDI clips a preview of their notes.
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
- **Double-click** a clip to open it in the clip view (the piano roll for a MIDI clip);
  see [audio-clips.md](audio-clips.md) and [midi.md](midi.md). **Shift+Tab** shows or
  hides the clip view, and **Esc** goes back to the arrangement.
- Overlaps follow Ableton's rule: the clip you place wins.

A clip's right-click menu has *Cut*, *Copy*, *Paste*, *Split Here*, *Duplicate*,
*Consolidate*, *Reverse* and *Delete*; right-clicking anywhere in a time selection
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
  tracks it crosses. On a group's lane it takes in everything in the group, its tracks
  too (folded away or not).
- **Ctrl+A** selects from the first clip to the last, on every track.
- **Delete**, **Ctrl+X / Ctrl+C** and **Ctrl+D** act on all of it, clips and automation
  alike (unless *Options › Lock Envelopes* is on: then the automation stays where it
  is); **Ctrl+V** pastes what was copied, clips and automation, at the insert marker.
- Dragging inside a time selection moves (or, with Ctrl, copies) just that stretch,
  splitting clips at its edges; the automation goes along.
- A folded track's lane isn't a grid: see [Folding](#folding).

## Tracks

**Ctrl+T** inserts an audio track, **Ctrl+Shift+T** a MIDI track (which comes with the
built-in Synth; see [midi.md](midi.md)). Both are in the *Create* menu and in a
header's right-click menu.

### Track headers

Each track's header (on the right, like Ableton) has:

- the **activator** (its number; switched off, the track is muted), **solo** and **arm**;
- **volume** (drag it; select it and type a number; double-click to reset) and **pan**;
- the **input** (audio, or a MIDI track's MIDI input) and **monitoring**
  (see [recording.md](recording.md));
- a **meter**;
- while there are return tracks, a **send knob** for each return
  (see [mixing.md](mixing.md));
- while its automation shows, the **automation choosers**
  (see [automation.md](automation.md)).

**Ctrl+R** (or *Rename* in its right-click menu) renames the selected track in place;
*Color* in the menu sets its colour. Drag the bottom edge of a track to resize it.

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
- A group has no clips; its lane shows the clips of the tracks in it, and its
  automation works as a track's. It has no arm or input: it records nothing.
- The tracks in a group are indented under it, with a band in the group's colour.
- **Ctrl+Shift+G** (*Ungroup Tracks*) takes a group away; what was in it stays, where it
  was.
- Deleting a group deletes what is in it.

How groups solo, mute and line up is in [mixing.md](mixing.md).

## Folding

Every track header has a fold button next to its name (or *Fold Track* / *Fold Group*
in its right-click menu).

- A track's is a triangle in a circle: folded, the track shrinks to its name row and
  shows its clips as bars with their names, as in Ableton. Click a bar to select its
  clip, drag it to move it (Ctrl copies), drag its ends to trim it. The rest of the row
  isn't a grid to select time on: a click there only moves the insert marker (a
  selection made on other tracks still takes it in, if it spans it).
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

## The transport bar

The bar across the top holds, from left to right:

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
[../ui/README.md](../ui/README.md), [../python/model.md](../python/model.md),
[../python/serialization.md](../python/serialization.md).
