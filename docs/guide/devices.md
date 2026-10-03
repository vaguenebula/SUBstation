# Devices

The device view, below the arrangement, shows the selected track's (or the master's)
device chain. This page covers working with devices, the built-in devices, racks with
their chains, macros and presets, folding, and cut, copy and paste. VST3 plug-ins have
their own page: [plugins.md](plugins.md).

## The device view

- It shows the built-in Synth and Sampler instruments, the Utility device, Over The Top
  and Compressor, and plug-ins, all through the same interface: they show alike.
- On a MIDI track the instrument comes first; the master takes audio effects only.
- **Ctrl+Alt+L** (*View › Device View*) shows or hides it.
- Add a device by dragging it from the browser onto the chain or onto a track, or by
  double-clicking it in the browser (see [browser.md](browser.md)).

### Title bar

Each device has a title bar, as in Ableton (lighter while the device is selected), with:

- the **fold** triangle (see [Folding](#folding));
- its **on/off switch** and **name** (the name's tooltip shows the device's latency, if
  it has any);
- a plug-in's **editor window** button (see [plugins.md](plugins.md));
- the **sidechain** button, on devices with a sidechain input (see
  [mixing.md](mixing.md#sidechains));
- the **parameter page arrows** ‹ ›;
- a **save** button: saves the device as a preset (see [Presets](#presets)).

### Parameters

- Each parameter gets a knob, or a list for parameters that choose between named
  values. Frequency and time knobs turn logarithmically.
- Most devices show four parameters at a time, in a 2×2 grid; the page arrows show the
  others. The Compressor and the Sampler have editors of their own (below).
- Clicking a parameter shows its automation in the arrangement. Right-click one to
  *Show Automation*, *Delete Automation* or *Re-Enable Automation*, and, in a rack,
  *Map to Macro* (see [automation.md](automation.md) and [Macros](#macros)).
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

## Racks

Racks (device groups) work like Ableton's Audio Effect and Instrument Racks.

- Select devices in the device view and press **Ctrl+G** (or *Group* in a device's
  right-click menu) to put them in a rack, in one chain, where the first of them was.
- **Ctrl+Shift+G** (*Ungroup*) takes a rack away, its chains' devices taking its place.
  A rack with several instruments can't be ungrouped: a chain has one.
- A rack's chains each process its input, side by side, and it puts out their sum (an
  empty rack passes its input on).
- Racks nest, up to 8 deep.

### Chains

- A rack shows its chains, a row each with its activator, name, solo (only the soloed
  chains of a rack are heard), volume, pan and meter.
- **+ Chain** (or *Add Chain* in the rack's right-click menu) adds one. A row's
  right-click menu renames, duplicates or deletes it, and shows its volume or pan
  automation; double-click its name to rename it.
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

A rack has eight macro knobs. Right-click a parameter of a device in the rack and
choose *Map to Macro* to have one of them move it across its range (*Unmap from
Macro* undoes that). A macro can move several parameters; right-click a macro to see
or unmap them. Turning a macro sets what it moves, as one undo step.

## Presets

- Every device's **save** button (or *Save Preset…* in its right-click menu) saves it
  as a **preset**, under a name you give it: a built-in device's settings, a plug-in's
  whole state, or a rack with everything in it (its chains, the devices in them with
  plug-ins' states, its macros). Saving under a name that is taken asks before
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
