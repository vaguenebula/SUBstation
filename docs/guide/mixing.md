# Mixing

How tracks are heard: solo and mute, group tracks in the mix, return tracks and sends,
sidechains, and delay compensation. The controls are in the track headers (see
[arrangement.md](arrangement.md)) and, for sidechains, in the device view (see
[devices.md](devices.md)).

## Solo and mute

- A track's **activator** (its number) mutes it when switched off.
- Soloing a track unsoloes the others, and unsoloing one unsoloes every track.
- **Ctrl-click** a solo button to solo (or unsolo) just that track, leaving the others
  as they are.
- Clicking the solo of a selected track solos (or unsoloes) all the selected tracks.
- **S** solos the selected tracks (unsoloing the rest), or, if they all are soloed
  already, unsoloes every track (*Edit › Solo Selected Tracks*).
- Soloing (a rack chain's too) is saved with the project but isn't an undo step, as in
  Ableton.

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

A device with a sidechain input (a plug-in's aux input: a compressor, a gate, a
vocoder; or the built-in Compressor) has a **sidechain button** (an arrow into a bar)
in its title bar, lit while it has a sidechain. Its tooltip names the source and where
it is taken.

Click it to choose the track, group or return whose signal goes into it (or *No
Sidechain*), and where that is taken, as in Ableton:

| Tap | What the device hears |
|---|---|
| *Pre FX* | The source's own audio, before its devices; a MIDI track's instrument's, before its effects |
| *After* one of its devices | The signal after that device |
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

---

For developers: [../engine/routing.md](../engine/routing.md),
[../engine/rendering.md](../engine/rendering.md),
[../ui/arrangement.md](../ui/arrangement.md).
