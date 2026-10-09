# Devices

The device view, along the bottom of the window (as in Ableton: under the browser and
the arrangement, beside the info view), shows the selected track's (or the master's)
device chain. This page covers working with devices, the built-in devices, racks with
their chains, macros and presets, folding, and cut, copy and paste. VST3 plug-ins have
their own page: [plugins.md](plugins.md).

## The device view

- It shows the built-in Synth and Sampler instruments, the Utility device, Over The Top,
  Compressor, Delay, Disperser, EQ and Sidechain, and plug-ins, all through the same interface:
  they show alike.
- On a MIDI track the instrument comes first; the master takes audio effects only.
- **Ctrl+Alt+L** (*View › Device View*) shows or hides it, with the info view.
- The **info view**, at the bottom left, says what the control under the mouse is: its
  tooltip shows there at once instead of popping up. *View › Info View* hides it; the
  tooltips then pop up again (as they always do in dialogs).
- Add a device by dragging it from the browser onto the chain or onto a track, or by
  double-clicking it in the browser (see [browser.md](browser.md)).

### Title bar

Each device has a title bar, as in Ableton (teal while the device is selected), with:

- the **fold** triangle (see [Folding](#folding));
- its **on/off switch** and **name** (the name's tooltip shows the device's latency, if
  it has any). The switch can be automated (*Device On*: right-click it; see
  [automation.md](automation.md#switching-off-and-on));
- a plug-in's **editor window** button (see [plugins.md](plugins.md));
- the **sidechain** button, on devices with a sidechain input (see
  [mixing.md](mixing.md#sidechains));
- the **parameter page arrows** ‹ ›;
- a **save** button: saves the device as a preset (see [Presets](#presets)).

### Parameters

- Each parameter gets a knob, or a list for parameters that choose between named
  values. Frequency and time knobs turn logarithmically.
- Most devices show four parameters at a time, in a 2×2 grid; the page arrows show the
  others. The Compressor, the Delay, the Disperser, the EQ, the Sidechain and the Sampler have
  editors of their own (below).
- Clicking a parameter shows its automation in the arrangement. Right-click one to
  *Show Automation*, *Delete Automation* or *Re-Enable Automation*, and, in a rack,
  *Map to Macro* (see [automation.md](automation.md) and [Macros](#macros)).
- A parameter mapped to a macro whose automation plays follows that automation (with
  the red dot), as if it were its own.
- Automated parameters are marked with a red dot (grey while overridden) and follow
  their automation as it plays.

### Selecting, moving and deleting

- Click a device (its title or background) to select it, **Shift-click** to select a
  range, **Ctrl-click** to add or remove one.
- Drag effects (or right-click one › *Move Left* / *Move Right*) to move them along the
  chain; the instrument stays first.
- Drag devices onto another track in the arrangement to move them there, with their
  automation; plug-ins move as they are, without loading again.
- Delete a device with the **Delete** key or its right-click menu.
- **Ctrl+Alt drag** anywhere on the chain scrolls it, as in the arrangement.

## Built-in devices

### Synth

A polyphonic subtractive synth (16 voices) with sine, triangle, saw and square
oscillators (band-limited saw and square), an ADSR envelope, a resonant low-pass
filter and volume. Velocity sets the level. New MIDI tracks come with it.

| Parameter | Range |
|---|---|
| Wave | Sine, Triangle, Saw, Square |
| Attack, Decay | 1 to 5000 ms |
| Sustain | 0 to 100 % |
| Release | 1 to 10 000 ms |
| Cutoff | 20 Hz to 20 kHz |
| Resonance | 0 to 100 % |
| Volume | −60 to +6 dB |

### Sampler

An instrument that plays one audio file across the keyboard (32 voices), pitched from
its root key, with transpose and detune, the part of the sample that plays (start and
end, looping between them while a note holds), an ADSR envelope, velocity sensitivity
and volume.

- Drop an audio file on its waveform (from the browser or the desktop) or double-click
  the waveform to load one; the right-click menu has *Load Sample…* and *Clear Sample*.
- Drag the start and end markers on the waveform. The part that plays is lit, the rest
  dimmed, the loop marked when it loops, and the newest note's position shows as it
  plays.
- Loading a sample can be undone, and the sample is saved with the project.
- Files load in the background, and the engine swaps them in without stopping the
  audio.

| Parameter | Range |
|---|---|
| Root Key | MIDI note 0 to 127 (C3 = 60 by default) |
| Transpose | −48 to +48 semitones |
| Detune | −100 to +100 cents |
| Start, End | 0 to 100 % of the sample |
| Loop | Off, On |
| Attack | 0.1 to 5000 ms |
| Decay | 1 to 10 000 ms |
| Sustain | 0 to 100 % |
| Release | 1 to 10 000 ms |
| Velocity | 0 to 100 % (how much velocity sets the level) |
| Volume | −60 to +6 dB |

### Utility

Gain (−60 to +24 dB), pan and stereo width (0 to 200 %).

### Over The Top

A multiband upward/downward compressor in the style of the one every drop uses, with a
single big **Soundgoodize** knob (depth) and an **Output** trim (±24 dB). Three bands
(split at 88 Hz and 2.5 kHz), each squashed from above and dragged up from below. At
0 % it passes the audio through untouched.

### Compressor

A soft-knee compressor, keyed by its own input or, with a sidechain, by another track's
signal (for ducking; see [mixing.md](mixing.md#sidechains)).

| Parameter | Range |
|---|---|
| Threshold | −60 to 0 dB |
| Ratio | 1:1 to 20:1 |
| Attack | 0.1 to 200 ms |
| Release | 5 to 2000 ms |
| Knee | 0 to 24 dB |
| Makeup | −12 to +24 dB |
| Dry/Wet | 0 to 100 % |

Its editor shows every knob at once and, beside them, the gain reduction over the last
second (growing downward, up to 24 dB) and meters of what keys it (the threshold
marked) and of its output, from −60 to 0 dBFS.

### Delay

A stereo delay after Ableton's, with an editor laid out like it.

- **Left / Right**: each side's time. With **Sync** on it is 1 to 16 sixteenths of the
  tempo (the grid), lengthened or shortened by up to 33 % (the field under it, for a
  swing feel); with Sync off, a time knob (1 ms to 5 s). The **link** button between
  them makes the right side follow the left (its controls grey out).
- **Filter**: a band-pass on the echoes (and so on what feeds back), from a 12 dB/octave
  high-pass to a 12 dB/octave low-pass, **Width** octaves apart around its frequency.
  Drag the curve's dot across for the frequency, up and down for the width. Behind the
  curve is a spectrum of the delay's input.
- **Mode**: how a change of time sounds. **Repitch** glides to it, pitching the echoes as
  a tape delay does; **Fade** crossfades to it; **Jump** switches at once.
- **Ping Pong**: the input, in mono, goes left first, and the echoes bounce from side to
  side.
- **Feedback** (0 to 95 %) and **Freeze** (∞): frozen, what is in the delay goes round
  for ever and new input is ignored.
- **Dry/Wet**: 0 to 100 %.

### Disperser

Phase dispersion, after Kilohearts' Disperser: a chain of all-pass filters that delays
what is around one frequency more than the rest, without making anything louder or
quieter. A kick's click turns into a zap, a snare into a laser, a pad's attack softens
into a chirp; sustained sounds change little.

- **Amount**: how many all-pass stages the sound goes through, 0 to 64. At 0 it passes
  through untouched. Each stage adds the same delay again.
- **Frequency** (20 Hz to 20 kHz): where the stages delay the sound most. (Near the top
  it is kept below half the sample rate: at 44.1 kHz, 20 kHz plays as 19.8 kHz.)
- **Pinch** (Q 0.1 to 10): how narrow the band they delay. Pinched, a narrow band comes
  out much later (a pitched, ringing zap); open, a wide band a little later.
- **Bypass**: the sound passes through untouched, fading over about 20 ms. The stages
  keep running, so switching it back is seamless (the device's on/off switch stops them).
- **The graph** shows how late each frequency comes out, in milliseconds (or seconds:
  64 narrow stages at 20 Hz hold 20 Hz back by 20 s). It is worked out from the very
  filters the engine plays, at its sample rate. Drag the dot across for the Frequency, up
  and down for the Pinch.
- Every control can be automated, and changes don't click: Amount crossfades to the new
  number of stages (the stages added fade in from silence), Bypass fades, and Frequency
  and Pinch glide. Swept quickly through many narrow stages, Frequency and Pinch are
  heard as the sweep itself (a zap), never louder than what went in.
- Its delay is the effect, not latency: other tracks aren't delayed to line up with it.
  A long dispersion rings on after the sound stops, as a reverb does.

### EQ

An equalizer after FabFilter's Pro-Q: up to 24 bands on one curve.

- **Adding a band**: hover the curve and a ghost band shows where a click adds one, and
  its type, which depends on where it is: a **low cut** at the far left, then a **low
  shelf**, **bells** across the middle, a **high shelf**, and a **high cut** at the far
  right. Keep the button down to drag it straight on. Double-clicking anywhere adds one
  too.
- **Moving a band**: drag its dot across for its frequency, up and down for its gain (a
  cut, notch or band pass: its Q). Shift drags finely; Ctrl-drag sets the Q.
- **The wheel** over a band (or the selected band) sets its Q; Alt-wheel sets its slope.
  While dragging a low or high cut, the wheel sets its slope.
- **Double-click** a band to switch it off and on; **Alt-click** it (or select it and
  press Delete) to remove it. Right-click it for its type, slope, placement and the rest.
- **Types**: bell, low and high shelf, low and high cut, notch, band pass and tilt shelf.
  Cuts and shelves have a **slope** of 6 to 96 dB/octave; a cut's Q above 0.71 makes it
  resonant at its frequency.
- **Placement**: a band works on both channels (Stereo), or only the Left, Right, Mid or
  Side (shown by a letter beside its dot).
- **The panel** beside the curve has the selected band's controls (on/off, delete, type,
  Freq, Gain, Q, slope, placement). It starts collapsed, so the device takes less room:
  the faders button (top right, beside the expand button) shows or collapses it, for
  every EQ. **Output** (±36 dB) and **Scale** (0 to 200 %: every band's gain at once)
  sit in the curve's bottom corners.
- **The analyzer** behind the curve shows the input (a line) and the output (filled),
  tilted 4.5 dB/octave so music looks about level (but not near its floor: silence stays
  flat). Click its label at the top left to
  switch between Pre, Post, both and off, and the label at the top right for the
  curve's range (±3, 6, 12 or 30 dB); right-click the background for both.
- **The expand button** (top right) opens the EQ, bigger, in a window of its own. It
  stays open as you change tracks, and closes when the device is deleted.
- The curve is the one the engine plays: its filters are matched to analog ones, so a
  bell high up keeps its shape instead of squeezing against the top of the spectrum.

### Sidechain

Ducks a track (a bass) out of the way of a kick along a curve that you draw, or that is
fitted to the kick.

- **Choosing the kick**: put the Sidechain on the bass's track and choose the kick's
  track with the sidechain button in the device's title bar (or click the hint over
  the curve). Each kick is a **hit**, found to the sample: the first sample at the
  **Threshold**, once the kick has dropped 3 dB below it again (and at least 20 ms
  after the last hit). The meter at the curve's right shows the kick against the
  threshold, and lights up at each hit.
- **Trigger**: *Sidechain* (the kick), or *Every Bar*, *1/2*, *1/4*, *1/8* or *1/16*:
  hits on the beat while the transport plays, with no kick needed, for pumping.
- **The curve** runs from each hit for its **Length** (in ms, or in notes with **Sync**),
  then holds its last value until the next hit. At the top the input is untouched,
  at the bottom it is down by the **Depth**. Drag a point to move it (the first and
  last only up and down). Drag the curve between points to bend it, or use the wheel
  there; double-click a bend to straighten it. Click anywhere else to add a point (keep
  the button down to drag it). Double-click a point, Alt-click it, or select it and
  press Delete to remove it. Shift drags finely. Right-click for **Shapes** (Pump,
  Smooth, Snappy, Linear, Hold, Gentle, Bounce, Stutter), *Fit to Kick*, *Flip Curve*
  (ducking becomes swelling) and *Reset Curve*. As hits come, a playhead rides the
  curve.
- **Fit**: as the kick plays (and the bass with it), the view beside the curve compares
  their spectra: the kick in orange, the bass in blue, and in pink where they **clash**,
  that is, where both are loud. **Fit** (or a click on that view) fits the curve to the
  kick there: the input is fully out of the way from the hit until the kick's energy
  in the clash peaks, then comes back as that energy dies away. It sets the curve's
  points, its length and the crossover. Behind the curve the kick's envelope there
  shows in orange, and the curve the fit calls for shows dashed. **Tight** brings the
  input back while the kick is still loud (12 dB down), **Natural** later (20 dB), and
  **Loose** only once the kick has all but gone (30 dB). **Auto** fits again at every
  hit (an undo takes back all of those fits at once).
- **Smooth** (0 to 30 ms) rounds off the curve's jumps so they don't click (0: they do,
  if you want that).
- **Lookahead** (0 to 20 ms) starts the curve that much before the kick. The rest of the
  mix is delayed to match.
- **Lows Only** ducks only what is below the **Crossover**, so the bass's upper
  harmonics carry on through the kick. The fit puts the crossover above the clash.

## Racks

Racks (device groups) work like Ableton's Audio Effect and Instrument Racks.

- Select devices in the device view and press **Ctrl+G** (or *Group* in a device's
  right-click menu) to put them in a rack, in one chain, where the first of them was.
- **Ctrl+Shift+G** (*Ungroup*) takes a rack away, its chains' devices taking its place.
  A rack with several instruments can't be ungrouped: a chain has one.
- A rack's chains each process its input, side by side, and it puts out their sum (an
  empty rack passes its input on).
- Racks nest, up to 8 deep.

### What a rack shows

A rack shows its macros. A strip of buttons at its left shows and hides the rest, as
Ableton's do, and adds and takes away macros:

- **Chain list** (the rows button): the rack's chains, beside its macros. It is hidden
  until you show it, and the rack is narrower without it.
- **Devices** (the bracket button): the devices of the chain the rack shows, beside
  it in a bracket. They show until you hide them; clicking a chain in the chain list
  shows them again.
- **+** and **−**: a macro more, or the last one taken away (see [Macros](#macros)).

The rack's right-click menu has *Show Chain List* / *Hide Chain List* and *Show
Devices* / *Hide Devices* too. What each rack shows is saved with the project, not an
undo step.

### Chains

- The chain list shows a rack's chains, a row each with its activator, name, solo
  (only the soloed chains of a rack are heard), volume, pan and meter.
- **+ Chain** (or *Add Chain* in the rack's right-click menu, which also shows the
  chain list) adds one. A row's right-click menu renames, duplicates or deletes it,
  and shows its volume or pan automation; double-click its name to rename it, or
  press **Ctrl+R** after clicking it (the chain list shows if it was hidden).
- Click a chain to show its devices beside the rack, in a bracket: drop devices there
  (or onto a chain's row), drag them in and out, select and delete them as on the
  track's own chain.
- A latent device in one chain doesn't smear the others: they are delayed to line up
  with the slowest, and the rack's latency is compensated like any device's.
- Every chain hears the track's notes, so a rack of instruments layers them (an
  *Instrument Rack*).

### Automation in racks

Devices in racks are automated like any of the track's devices (they keep their
automation when grouped, ungrouped or moved), and so are the chains' volume and pan
(*Chain Volume*, *Chain Pan* under the rack in a lane's device chooser), in time with
the latency before them. A sidechain into a device in a rack lines up with the signal
there.

### Macros

A new rack has four macro knobs, in two rows. The **+** button at the rack's left (or
*Add Macro* in a macro's right-click menu) adds one, up to 16; **−** (or *Remove Last
Macro*) takes the last one away, with what it is mapped to and its automation. Each is
one undo step.

- **Mapping**: right-click a parameter of a device in the rack and choose *Map to
  Macro*, then the macro (by its name), to have it move that parameter across its
  range (*Unmap from* the macro undoes that). A macro can move several parameters; a
  parameter follows one macro of a rack. Turning a macro sets what it moves, as one
  undo step.
- **Ranges**: right-click a macro › *Edit Mappings…* lists what it moves, each with a
  **Min** and a **Max**, in percent of the parameter's range (its value in its own
  units beside them). The parameter goes from Min to Max as the macro turns from 0 to
  100 %: *0 to 100 %* is its whole range, *50 to 100 %* its upper half, and *100 to
  0 %* turns it the other way round (**Invert** swaps Min and Max). Drag a box up or
  down, or click it and type a number (double-click puts it back to 0 or 100 %); a
  drag is one undo step. A range changed puts the parameter where the macro is in it
  at once. **Unmap** takes a parameter off the macro.
- **Names**: double-click a macro's name (or right-click it › *Rename*) to name it in
  place; Enter, Escape or clicking elsewhere keeps the name typed, and an empty name
  names it by its number again. The name shows in the parameters' *Map to Macro*
  menus and in the automation lanes. It is saved with the rack (and in its presets).
- **Automation**: a macro is automated like any parameter. Click it to show its
  automation in the arrangement (it is a lane of the rack: *Macro 1*… in a lane's
  device chooser), or right-click it › *Show Automation*, *Delete Automation*,
  *Re-Enable Automation*. While its automation plays, its knob follows it (with the
  red dot), and so does every parameter mapped to it, over its range and in time with
  the latency before it (instead of the parameter's own automation). Turning the
  macro by hand overrides its automation, as with any parameter, until it is
  re-enabled.
- The macro's tooltip lists what it moves; its name is lit while it moves something.

## Presets

- Every device's **save** button (or *Save Preset…* in its right-click menu) saves it
  as a **preset**, under a name you give it: a built-in device's settings, a plug-in's
  whole state, or a rack with everything in it (its chains, the devices in them with
  plug-ins' states, its macros with their names and ranges). Saving under a name that is taken asks before
  replacing that preset.
- Presets go into your preset library, `Documents\SUBstation\Presets`, in a folder per
  device (named as the device: a plug-in's name, *Utility*, *Audio Effect Rack*…). The
  browser lists them under *Presets*, by device (see [browser.md](browser.md#presets)).
- Drag a preset from the browser onto the chain, or onto a track in the arrangement, or
  double-click it, to add it as a new device. An instrument preset goes on a MIDI track;
  with none selected (or dropped below the tracks) it makes one.
- Drop a preset **onto a device of its kind** (the same plug-in, the same built-in
  device, or a rack of the same kind) to load it into that device: the device is
  outlined while the drag is over it. It keeps its place, its on/off switch and its
  sidechain; a rack's chains and macros are replaced (and the automation of the devices
  that leave with them). Dropped onto another kind of device, the preset goes in beside
  it as a new device.
- Loading a preset is one undo step. Loaded devices are new ones (a preset loaded twice
  makes two), without sidechains.
- A rack is titled with the name of the preset it was saved as or loaded from (loaded
  into another rack, that rack takes the name too). A rack that never was a preset is
  called *Audio Effect Rack* or *Instrument Rack*. The name is saved with the project.
- **Default presets**: right-click a device › *Save as Default Preset*, and every new
  device of that kind (that plug-in, that built-in device) starts as this one is now:
  added from the browser, dropped, or as a new MIDI track's instrument. *Clear Default
  Preset* makes new ones start as they come again. Racks have none. Defaults are kept in
  the library's `Defaults` folder (`<device>.gilpreset`, or `<plug-in> (<class
  id>).gilpreset`), which the browser doesn't list.
- A preset of a plug-in that isn't installed loads as a missing device, as in a
  project: it keeps its settings and loads once the plug-in is back. In a rack, the rest
  of the rack works.
- Right-click beside the devices › *Load Preset…* loads a preset file from anywhere.
- Plug-ins also read and write their own `.vstpreset` files (see
  [plugins.md](plugins.md#presets)).

## Folding

- The triangle at the start of a device's title bar (or *Fold* in its right-click menu)
  folds it to a narrow strip with its on/off switch and its name, reading upwards; a
  folded rack hides its chains too.
- Click the triangle again, or double-click the strip, to unfold it.
- **Ctrl+double-click** a device (its title or background) folds or unfolds it too.
- With several devices selected, folding one of them folds (or unfolds) them all.
- Folding is saved with the project, not an undo step.

## Cut, copy and paste

- While the device view has the focus (devices clicked, or the space beside them),
  **Ctrl+X**, **Ctrl+C** and **Ctrl+V** cut, copy and paste the selected devices, and
  **Ctrl+D** duplicates them.
- A device's right-click menu has them too, and right-clicking beside the devices
  pastes there.
- Pasted devices go after the selected ones (or at the end of the track's chain) and
  are selected. They are new devices with the same settings: plug-ins in their state
  when copied, racks with everything in them, folded if they were.
- Sidechains are kept, unless their source is gone or would close a cycle on the track
  pasted onto.
- An instrument pasted goes first on a MIDI track (replacing its instrument), and not
  onto an audio track.
- To paste onto another track, select it, click beside its devices, and press Ctrl+V.

---

For developers: [../ui/device-view.md](../ui/device-view.md),
[../engine/devices.md](../engine/devices.md),
[../engine/routing.md](../engine/routing.md) (racks).
