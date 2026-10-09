# Mixing

How tracks are heard: solo and mute, where each track's output goes, group tracks in the
mix, return tracks and sends, sidechains, delay compensation, and freezing tracks to save CPU. The controls are in the track headers (see
[arrangement.md](arrangement.md)) and, for sidechains, in the device view (see
[devices.md](devices.md)).

## Solo and mute

- A track's **activator** (its number) mutes it when switched off. It can be automated
  (*Track Activator*: see [automation.md](automation.md#switching-off-and-on)).
- Soloing a track unsoloes the others, and unsoloing one unsoloes every track.
- **Ctrl-click** a solo button to solo (or unsolo) just that track, leaving the others
  as they are.
- Clicking the solo of a selected track solos (or unsoloes) all the selected tracks.
- **S** solos the selected tracks (unsoloing the rest), or, if they all are soloed
  already, unsoloes every track (*Edit › Solo Selected Tracks*).
- Soloing (a rack chain's too) is saved with the project but isn't an undo step, as in
  Ableton.

## Where a track goes: Audio To

Each track's header has, in its In/Out column (as in Ableton; *View › In/Out*,
**Ctrl+Alt+I**, shows or hides the column), **Audio To**: where its output goes, and
under it where in that track. A group's and a return's Audio To is the first row of
its column; a track's is under its input and monitoring.

| Audio To | Under it | What happens |
|---|---|---|
| *Main* | (empty) | Into the master, past any group it is in |
| its group | (empty) | Into its group (a track in a group goes there by default, as in Ableton) |
| an audio track | *Track In* | Into that track's input: heard through it (its devices and mixer) while it monitors, *In*, or *Auto* and armed, instead of its clips, as if played into it; with monitoring *Off* it plays its clips and what comes in isn't heard |
| a track with a device taking a sidechain | *Sidechain-‹device›* | Into that device's sidechain input, after this track's fader: it keys the device (a ducker, a compressor) and isn't heard otherwise; several tracks can go into one device, along with the device's own sidechain, and are summed |
| *Sends Only* | | Nowhere: only its sends (and tracks taking it as their input) hear it |

*Ext. Out* (straight to the audio device's outputs) is listed but not there yet: tracks
go through the master. *Configure…* opens the audio preferences.

- The list has the tracks it can go into: audio tracks (their Track In) and any track,
  group or return with a device taking a sidechain. Those that would make a loop (they
  feed this track) are greyed out.
- Choosing an audio track sends it to its Track In; the menu under it chooses between
  its Track In and its devices' sidechains. If that track isn't monitoring *In*, the
  status line says so: as in Ableton, what goes into a track's input is heard only
  while it monitors.
- Putting tracks into a group (**Ctrl+G**, or dragging them in) sends them into it,
  whatever they went into before, as in Ableton; moving a track within its group or out
  of it keeps its Audio To.
- Deleting the track (or the device) a track goes into, or a move that would make a
  loop, sends it back into its group (in the same undo step).
- The master's Audio To is *Main Out*: the audio device's outputs it plays on (an ASIO
  device's pairs can be chosen there; other drivers play on the first two).
- What goes into a track's Track In isn't recorded with it (record a track's output with
  [Audio From](recording.md#resampling) instead).

## Groups in the mix

- Soloing a group solos what is in it; soloing a track keeps the groups it is in heard
  (but not the other tracks in them).
- Muting a group silences what is in it.
- Delay compensation lines up the tracks going into each group, and the groups going
  into the master: a latent plug-in anywhere (on a track deep in a group, or on a group)
  delays only what it has to.

Making, folding and ungrouping groups is in [arrangement.md](arrangement.md#group-tracks).

## Return tracks and sends

Return tracks and sends work as in Ableton. **Ctrl+Alt+T** (*Create › Insert Return
Track*, or a header's right-click menu) adds a return track, named by letter (A, B,
...). Returns show apart, in compact rows above the master, each with its activator,
solo, volume, pan, meter and lane (for its automation); click one to see its devices
(audio effects: a reverb, a delay) in the device view.

### Send knobs

- While there are returns, every header (tracks', groups' and returns') has a **send
  knob** for each return, by its letter, below volume and pan: turn it up to send that
  much of the track to the return. Its tooltip shows where it sends, how much, and
  whether before or after the fader.
- A send taps the track after its fader and pan. Right-click the knob for
  **Pre-Fader** (the track's fader doesn't change what it sends; its letter shows in
  orange), to remove the send (*Remove Send*), or to show its automation.
- A muted track sends nothing, before the fader or after.
- A return can send on into another return, but not into one that feeds it (or itself):
  those knobs are greyed out.

### Sends, automation and undo

- Send levels are automated like volume (*Mixer › Send A* in a lane's choosers);
  turning a send knob by hand overrides its automation, as for volume (see
  [automation.md](automation.md)).
- Deleting a return deletes the sends into it and their automation, in one undo step.

### Solo with returns

Soloing a track keeps everything it goes into heard, its returns too (you hear its
reverb, not the others'); soloing a return keeps what sends to it sending (but not
playing on its own).

### Delay compensation per send

Delay compensation works per send: a track sending to two returns of different latency
is delayed differently on each, and everything lines up again at the master.

## Sidechains

A device's sidechain can also be fed from the other side: a track's [Audio
To](#where-a-track-goes-audio-to) set to that device's track, *Sidechain-‹device›*.

A device with a sidechain input (a plug-in's aux input: a compressor, a gate, a
vocoder; or the built-in Compressor) has a **sidechain button** (an arrow into a bar)
in its title bar, lit while it has a sidechain. Its tooltip names the source and where
it is taken.

Click it to choose the track, group or return whose signal goes into it (or *No
Sidechain*), and where that is taken, as in Ableton:

| Tap | What the device hears |
|---|---|
| *Pre FX* | The source's own audio, before its devices; a MIDI track's instrument's, before its effects |
| *After* one of its devices | The signal after that device; one in a rack is listed by the rack (and its chain, if it has several): *After Glue › Compressor* is the signal there in that chain, before the chain's fader and the other chains |
| *Post FX* | After all its devices, before the fader |
| *Post Mixer* | After its fader and pan, as it is heard |

A newly chosen source is taken *Post Mixer*; the tap choices then appear below the
sources in the same menu. Sources that would close a cycle (the group the device's
track is in, a return it sends to) are greyed out; the master's devices can take any
track's.

### Timing

The sidechain lines up with the device's own signal sample for sample: whichever comes
first waits for the other, so latent plug-ins on the source (before the tap) or on the
device's track (before the device) change nothing.

### Solo and mute

A sidechain isn't heard on its own: soloing the source doesn't bring in the device's
track, and while the device's track is heard (soloed, or a track going into it is) the
source keeps keying it (without being heard). Muting the source silences its sidechain
only after the fader, so a muted "ghost" kick can still duck the bass from before its
fader.

### When things move

Deleting the source turns the sidechain off (in the same undo step), as does moving
tracks into groups, or the device to another track, where it would close a cycle;
otherwise a device moved to another track keeps its sidechain. Should the device a
sidechain is taken after leave its track, it is taken Post FX until the device comes
back.

A sidechain into a device in a rack lines up with the signal there (see
[devices.md](devices.md#racks)).

## Delay compensation

Latency that plug-ins report (look-ahead limiters, linear-phase EQs) is compensated:

- the other tracks, and the metronome, are delayed to line up, and exports come out
  aligned;
- groups, returns and the master each line up what goes into them (see above), and each
  send is delayed on its own;
- a latent device in one rack chain doesn't smear the others (see
  [devices.md](devices.md#racks));
- automation is delayed along with a track's audio (see
  [automation.md](automation.md)).

A device's tooltip shows its latency. A track you monitor live isn't delayed to line up
with others (see [recording.md](recording.md#monitoring)).

## Freezing and flattening

Freezing a track renders what its devices put out into an audio file, and the track
plays that instead, so its plug-ins cost no CPU.

- **Ctrl+Shift+F** (*Edit › Freeze / Unfreeze Track*, or *Freeze Track* in a track's
  right-click menu) freezes the selected tracks, groups and returns. If they are all
  frozen already, it unfreezes them.
- The render runs in the background, its progress in a dialog (*Freezing Bass (2 of
  3)…*); the window goes on meanwhile but takes no edits, and nothing plays. **Cancel**
  (or Esc) stops it: nothing is frozen, and no render is left behind.
- A frozen track's lane is tinted blue (its name stays as it is). The
  device view shows no devices: they are unloaded, and their settings (a plug-in's whole
  state too) come back when you unfreeze it.
- What is frozen: the track's clips or notes and its devices, with their automation.
  What stays live: its volume, pan, mute and solo (and their automation), its sends, and
  where its output goes. Sends and sidechains taking its signal after its fader or before
  it hear the frozen audio.
- While it is frozen you can't change its clips, devices or their automation; the
  status line says why when you try. Unfreeze it first. A frozen track isn't armed.
- **Except time selections**: on a frozen track you can still move (or Ctrl-drag copy),
  cut, copy, paste, duplicate and delete a selected stretch of time, and its frozen
  audio goes with the clips (and its devices' automation with them), so what you see is
  what plays. Each is one undo step. On a frozen track the stretch replaces everything
  where it lands, empty parts too, as a piece of its audio does. What you can't do:
  - drag clips off a frozen track onto another, or onto it from another;
  - paste into a frozen track what wasn't copied from it (since it was frozen): its
    frozen audio has to come along. Copied from a frozen track and pasted onto a track
    that isn't frozen, the clips go as they are.
  Unfreezing forgets the edited frozen audio and keeps the clips as you left them;
  freezing again renders them anew. Flattening plays what of the frozen audio you
  kept.
- Freezing a **group** renders its bus: what is in it, through the group's devices.
  The tracks in it then aren't played (unless they also send to a return, or key a
  sidechain, outside the group), and nothing in it can change, move in or out, or be
  deleted until you unfreeze the group. A new track inserted after one of them goes
  after the group. Time selections over all of it (select on the group's row) move,
  cut, copy, paste, duplicate and delete its frozen audio with the clips, as on a
  frozen track; a selection of only some of its tracks is refused.
- Freezing a **return** renders what the sends into it brought; changing those sends
  doesn't change it until you unfreeze it.
- The render goes from the start of the arrangement to the end of its last clip, then on
  for as long as the track still sounds (up to ten seconds: a reverb's tail). The files
  go into a *Freeze* folder next to the project (in the recordings folder while the
  project isn't saved).
- Changing the tempo plays the frozen audio warped (stretched to the new tempo); freeze
  the track again for a fresh render.
- A track can't be frozen while another track's sidechain takes its signal before its
  devices (Pre FX) or after one of them: the frozen audio doesn't have that signal.

**Flattening** (*Edit › Flatten Track*, or *Flatten Track* in the track's menu) turns a
frozen audio or MIDI track into an audio track that plays its frozen audio as a clip,
without its devices. A MIDI track becomes an audio track. One undo step brings the
track back as it was, frozen. Groups and returns can't be flattened.

---

For developers: [../engine/routing.md](../engine/routing.md),
[../engine/rendering.md](../engine/rendering.md),
[../ui/arrangement.md](../ui/arrangement.md).
