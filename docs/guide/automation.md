# Automation

Automation works as in Ableton's arrangement, for every device parameter (built-in
devices and plug-ins alike), for each track's and the master's volume and pan, for send
levels, for rack chains' volume and pan, for racks' macros (which move what is
mapped to them along their automation: see [devices.md](devices.md#macros)), and for
switching tracks, groups, returns, devices and racks off and on (see
[Switching off and on](#switching-off-and-on)). This page covers showing lanes, editing
envelopes, Lock Envelopes, and overriding automation by hand.

## Showing automation

- **A** (*View › Automation*) shows (or hides) the automation of every track and the
  master.
- A track's automation shows in its own lane, over its clips (the clips' title bar
  still moves and selects them), with the parameter chosen in its header: a **device
  chooser** (*Mixer* or one of its devices) and a **parameter chooser**; automated ones
  are marked with a red dot.
- **+** shows another parameter in a lane below the track (**−** removes it). Lanes
  below a track have a fixed height.
- Right-click a track header (or the master's, or a return's) to *Show Automation*,
  *Hide Automation* or *Show Automation in New Lane*.
- Clicking a parameter (a knob, list or name in the device view, or a control in a
  plug-in's own editor) or changing one by hand (also a track's volume or pan, or the
  master's, and switching a track's activator or a device's on/off switch) shows its
  track's automation with that parameter.
- Right-click a send knob, a rack chain or a macro to show its automation; right-click
  a track's activator or a device's on/off switch for its (*Show Automation*, *Delete
  Automation*, *Re-Enable Automation*).
- Folded tracks don't show their automation (see
  [arrangement.md](arrangement.md#folding)).

What shows (per track and the master: shown or not, the main lane's parameter, the
lanes below) is saved with the project but isn't an undo step, like track heights.

## Editing envelopes

All of it is undoable (one step per drag).

### Breakpoints

- **Click on the envelope's line** to add a breakpoint on it (on the grid when
  snapping; Alt-click where there is no segment to bend: off the grid), or press there
  and drag to place it. Over the line the cursor shows a small plus, and a faint
  breakpoint shows where the click would put it; a click off the line adds nothing.
- **Click a breakpoint** to delete it.
- **Drag** breakpoints to move them in time and value. Shift-click or Ctrl-click
  selects several, which move together; Alt drags off the grid, Shift finer. One can't
  pass its neighbours; several override the breakpoints they land on. The value shows
  as you drag.
- Select breakpoints and press **Delete** to delete them.

### Segments

- Near the line between two breakpoints (but not on it) the segment lights up: **drag**
  it to move both breakpoints, in time and value (a click selects them).
- A step (two breakpoints at the same time) is a segment too: drag it sideways to move
  the step.
- **Alt-drag** between two breakpoints to bend that segment (up bulges it upward).

### Time ranges

Drag across lanes (not on a breakpoint) to select a time range on them: on every
automation lane you cross, down over the tracks below too. Up into the clips' title
band (or above the track) the drag selects clips instead, as a drag in the clips does
(see [arrangement.md](arrangement.md#selecting)).
**Shift-click** another lane (or anywhere in the lanes) to extend it there. Then:

- **Delete** clears their automation there (the envelope outside stays as it was);
- **Ctrl+D** duplicates it after the range;
- **Ctrl+X** / **Ctrl+C** cut or copy it, and **Ctrl+V** pastes it at the insert marker:
  onto the lanes of a time selection made first, one to one, or one lane copied onto
  each; otherwise onto the lanes it came from. The pasted range is selected.
- **Dragging inside the range** (off its breakpoints and line, which work as anywhere
  else) moves the automation in it (on all its lanes) up, down, left or right, with
  breakpoints added at its edges so the envelope outside stays as it was (moved in
  time, it replaces what is where it lands).

### The lane's menu

Right-click a lane for more: *Cut*, *Copy* and *Paste*, *Delete Breakpoint* (or *Delete
Selected Breakpoints*), *Delete Envelope*, *Re-Enable Automation* (while overridden),
*Remove Lane*, *Show Automation in New Lane* and *Hide Automation*.

## How envelopes play

- Before its first breakpoint an envelope holds the first one's value, after its last
  the last one's; two breakpoints at the same time make a step.
- Parameters that choose between values (lists, steps) move in steps, and their lanes
  show them so.
- Automated controls follow their automation as it plays (and where the playhead is
  placed when stopped), in the device view, the track headers and plug-ins' own
  editors; they are marked with a red dot.
- The engine plays envelopes sample-accurately: volume and pan sample by sample; device
  parameters at each breakpoint and every 64 samples along a slope (plug-ins get them as
  sample-accurate VST3 parameter changes; built-in devices process their blocks in
  pieces where values change).
- Automation is delayed along with a track's audio by the plug-in latency before it
  (delay compensation), and it is in exports.
- Send levels are automated like volume (*Mixer › Send A*), and rack chains' volume and
  pan as *Chain Volume* and *Chain Pan* under the rack in the device chooser (see
  [devices.md](devices.md#automation-in-racks)).

## Switching off and on

Turning a track off (muting it) or a device off can be automated, as Ableton's *Track
Activator* and *Device On*:

- **Track Activator**, under *Mixer* in a lane's chooser, switches a track, a group or
  a return off and on (the master has none). Off, it is silent, as if muted: its sends
  too, before its fader as well. While its automation plays, the activator follows it
  and the track's own mute doesn't count.
- **Device On**, the first parameter of every device in a lane's chooser (built-in
  devices, plug-ins and racks alike), switches the device off and on. Off, it is
  passed by: what goes into it comes out as it is, as late as the device would have
  made it, so what comes after it stays in time. A rack switched off passes its input on
  without its chains. Coming back on, a device starts again from silence (no old
  reverb tail or held note).
- Their lanes move in steps: up is on, down is off. The switch fades over about 5 ms so
  it doesn't click.
- While its automation plays the button follows it (a red dot in its corner); clicking
  it overrides the automation (a grey dot), as changing any automated parameter does.

## Overriding and re-enabling

Changing an automated parameter by hand (or switching an automated track activator or
device on/off switch) **overrides** its automation, as in Ableton: its envelope turns
grey and the parameter stays where you put it until **Re-Enable Automation**:

- the lit button next to Record in the transport bar;
- *Edit › Re-Enable Automation*;
- or a lane's, a track header's or a parameter's right-click menu.

Right-click a parameter in the device view to show its automation, delete it or
re-enable it.

## Lock Envelopes

**Lock Envelopes** (the padlock next to Record, or *Options › Lock Envelopes*; saved
with the project):

- Unlocked, as by default, automation moves with clips. Moving a clip (or a time
  selection) moves the automation under it, replacing what was where it lands; copying
  clips (Ctrl-drag, Ctrl+D) copies it. Only envelopes with breakpoints under the clips
  move.
- Moved to another track, a clip takes its track's volume, pan and activator automation
  along; a device's automation stays on its own track.
- Locked, automation stays where it is.

## Deleting devices

Deleting a device deletes its automation, in the same undo step. Devices moved to
another track (or into and out of racks) keep their automation.

---

For developers: [../ui/arrangement.md](../ui/arrangement.md#automation-lanes) (the
automation lanes and choosers), [../engine/automation.md](../engine/automation.md),
[../app/model.md](../app/model.md).
