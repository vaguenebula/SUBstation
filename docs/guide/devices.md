# Devices

The device view, along the bottom of the window (as in Ableton: under the browser and
the arrangement, beside the info view), shows the selected track's (or the master's)
device chain. This page covers working with devices, the built-in devices, racks with
their chains, macros and presets, folding, and cut, copy and paste. VST3 plug-ins have
their own page: [plugins.md](plugins.md).

## The device view

- It shows the built-in Synth and Sampler instruments, the Utility device, Over The Top,
  Compressor, Gate, Limiter, Multiband Dynamics, Spectral Compressor, Saturator, Amp,
  Erosion, Delay, Chorus-Ensemble, Phaser-Flanger, Reverb, Disperser, EQ and Sidechain,
  and plug-ins, all through the same interface: they show alike.
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
  others. The Sampler, the Compressor, the Gate, the Limiter, Multiband Dynamics, the
  Spectral Compressor, the Saturator, the Amp, Erosion, the Delay, the Chorus-Ensemble,
  the Phaser-Flanger, the Reverb, the Disperser, the EQ and the Sidechain have editors
  of their own (below).
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

An instrument that plays one audio file, as Ableton's Simpler does, in one of three
modes (the tabs at its left):

- **Classic**: played across the keyboard, pitched from its root key (up to 32 notes at
  once, *Voices*), with an ADSR envelope; it plays from Start to End, or with **Loop**
  on loops from Loop Start to End while a note holds and as it releases. The loop's
  **Fade** crossfades its end into what leads to its start, so a loop that doesn't
  meet itself cleanly doesn't click. With one voice and a **Glide** time, notes played
  legato glide from one to the next (letting go of the top note glides back to the one
  still held).
- **1-Shot**: one note at a time (a new one cuts the last), pitched; **Trigger** plays
  the whole of Start to End however short the note, **Gate** fades out when the note
  ends. **Fade In** and **Fade Out** shape each note (it also fades out before End).
- **Slice**: the sample cut into slices, a slice per key from C1 up (C1 the first,
  C#1 the next...), each at the sample's pitch: at its **transients** (more
  **Sensitivity** finds quieter ones), at **beats** (every 1/16 to 4 bars of the length
  Warp gives it), or into equal **regions**. **Mono**: a slice cuts the one before;
  **Poly**: they overlap; **Thru**: a slice plays on to End. Trigger, Gate and the fades
  as 1-Shot's.

Its display shows the sample, the part that plays lit (Start and End flagged at the
top), and by mode the loop (bracketed, its crossfade shaded), the fades, or the slices
(numbered, the one playing lit); the newest note's position as it plays; the time along
the bottom.

- Drop an audio file on the display (from the browser or the desktop) or double-click it
  to load one; the right-click menu has *Load Sample…*, *Clear Sample* and *Reverse*.
- Drag Start, End and (Classic, looping) Loop Start on the display.
- Under the display: **Gain**; the mode's own settings; **Snap** (Start, End, Loop
  Start and slices move to the nearest zero crossing, so notes don't click as they
  start); **Warp** *as* a length: the whole sample lasts that many beats at the song's
  tempo and follows it. Its warp modes are the clips': Transients, Standard, Smooth and
  Formants stretch it (the keys transpose it, not its length), Re-Pitch speeds it up or
  slows it down like a record (the pitch goes with it). **:2** and **\*2** halve and
  double the length.
- Under that: the **Filter** (low-pass, high-pass, band-pass or notch, 12 or 24 dB an
  octave, frequency, resonance), the **LFO** (sine, triangle, saw up and down, square or
  random; its rate in Hz or synced to the song, from 1/32 to 8 bars), the envelope
  (Classic's Attack, Decay, Sustain, Release; the others' Fade In and Fade Out),
  **Transp**, **Vol < Vel** (how much velocity sets the level) and **Volume**.
- The **Controls** page (its tab in the title bar) has the rest: the root key, detune,
  voices and glide; Start, End, Loop Start and Loop Fade as knobs, **Reverse** (the
  sample plays backwards, and is drawn so: the markers and slices are places in it as it
  plays); where the LFO goes (**Volume**, **Pitch**, **Filter**, **Pan**) and
  **Retrig** (each note starts the LFO from the start of its cycle; synced without it,
  the LFO follows the song's beats); **Pan** and Gain.
- Loading a sample can be undone, and the sample is saved with the project. Files load
  in the background, and the engine swaps them in without stopping the audio.
- Warped notes that stretch (not Re-Pitch) cost more: each one runs a stretcher (up to
  8 at once), and starting a note computes the stretcher's first block ahead. Warped to
  play more than 4 times as fast (a long sample in few beats), notes are resampled
  instead.

| Parameter | Range |
|---|---|
| Mode | Classic, 1-Shot, Slice |
| Root Key | MIDI note 0 to 127 (C3 = 60 by default) |
| Transpose | −48 to +48 semitones |
| Detune | −100 to +100 cents |
| Start, End | 0 to 100 % of the sample |
| Gain | −24 to +24 dB |
| Reverse, Snap | Off, On |
| Warp | Off, On |
| Warp Length | 1 beat to 64 bars (the whole sample) |
| Warp Mode | Transients, Standard, Smooth, Formants, Re-Pitch |
| Loop | Off, On (Classic) |
| Loop Start | 0 to 100 % of the sample (from Start at the earliest) |
| Loop Fade | 0 to 100 % of the loop |
| Attack | 0.1 to 5000 ms |
| Decay | 1 to 10 000 ms |
| Sustain | 0 to 100 % |
| Release | 1 to 10 000 ms |
| Voices | 1 to 32 (Classic; Slice's Poly) |
| Glide | 0 to 2000 ms (one voice) |
| Trigger Mode | Trigger, Gate (1-Shot, Slice) |
| Fade In, Fade Out | 0.1 to 2000 ms (1-Shot, Slice) |
| Slice By | Transient, Beat, Region |
| Sensitivity | 0 to 100 % |
| Slice Division | 1/16, 1/8, 1/4, 1/2, 1 Bar, 2 Bars, 4 Bars |
| Regions | 2 to 64 |
| Playback | Mono, Poly, Thru |
| Filter | Off, On |
| Filter Type | Low-pass, High-pass, Band-pass, Notch |
| Filter Slope | 12 dB, 24 dB |
| Filter Freq | 20 Hz to 22 kHz |
| Resonance | 0 to 100 % |
| LFO, LFO Sync, LFO Retrigger | Off, On |
| LFO Wave | Sine, Triangle, Saw Up, Saw Down, Square, Random |
| LFO Rate | 0.01 to 30 Hz |
| LFO Synced Rate | 1/32 to 8 Bars |
| LFO > Volume, Filter, Pan | 0 to 100 % (Filter: 4 octaves either way at 100 %) |
| LFO > Pitch | 0 to 1200 cents either way |
| Pan | Left to right |
| Vol < Vel | 0 to 100 % (how much velocity sets the level) |
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

### Gate

A gate after Ableton's: it lets through only what is louder than the **Threshold** and
turns everything else down to the **Floor**. Use it to take out the hiss or hum between
notes, a drum mic's spill between hits, or, with the threshold higher, to cut a reverb's
or a delay's tail short. It can be opened by another track instead (see
[mixing.md](mixing.md#sidechains)).

| Parameter | Range |
|---|---|
| Threshold | −70 to +6 dB |
| Return | 0 to 24 dB |
| Attack | 0.02 to 150 ms |
| Hold | 1 to 1500 ms |
| Release | 0.1 to 3000 ms |
| Floor | −inf to 0 dB |
| Lookahead | 0, 1 or 10 ms |

- **The display** shows the last two and a half seconds: what comes in as a light grey
  band, what comes out darker with a white outline over it, so what the gate takes away
  is the light part above the dark. Where the gate was open the whole height is tinted
  blue. The LED at the top left says whether it is open now ("Idle" while nothing comes
  through: the device switched off, or the audio stopped), and the two meters on the
  right show the input's level (**In**) and how far the gate turns it down (**Gate**),
  against the same figures as the display.
- **Threshold**: the gate opens when the level reaches it. It is the blue line across
  the display: drag it up and down (or drag anywhere in the display's graph), Shift for
  small steps, double-click it to put it back, right-click it for its menu (its
  automation, for instance). Set it so the sound you want crosses it and the noise
  doesn't: the dot on the display's right edge is the level now (it falls quickly rather
  than jumping), blue while the level is at or above the threshold and grey as soon as
  it is below.
- **Return**: how far the level has to fall below the threshold before the gate closes
  again: the orange line. Raise it when the gate chatters, opening and closing quickly
  on a sound that hovers around the threshold. Drag the orange line down for more (at
  0 the two lines are one: press on or just below it to drag Return, above it for the
  threshold).
- **Attack**: how long the gate takes to open. Very short can click; longer softens the
  start of each sound.
- **Hold**: how long it stays open once the level has fallen below the orange line, to
  bridge short gaps (the default 10 ms keeps it open through a low note's waves).
- **Release**: how long it then takes to close.
- **Floor**: how far a closed gate turns the sound down: at −inf, silence; at 0 dB, not
  at all. In between the gate only dips the sound between notes.
- **Flip**: the gate works the other way round: only what is quieter than the threshold
  passes (the loud parts are turned down to the floor).
- **Lookahead**: the gate sees what is coming this much ahead, so it is already open
  when a sharp attack arrives and doesn't cut its first moment off. It delays the track
  as much (1 ms by default, as in Ableton); the rest of the mix is delayed to match.

**The sidechain**: click the **Sidechain** strip at the device's left edge to unfold its
section (click it again to fold it; it lights up while what opens the gate isn't simply
the track itself).

- **Where the key comes from**: the button at the top (or the sidechain button in the
  title bar) chooses a track to open the gate with: a kick to gate a bass or a noise pad
  in time, for instance. That track is never heard through the gate. **Gain** sets how
  loud it is to the gate, and **Dry/Wet** blends it with the track's own sound (100 %:
  only the other track opens it; 0 %: only its own sound). Both are dimmed without a
  sidechain, but can be set beforehand.
- **EQ**: opens the gate from one band of the key only, so a snare mic opens for the
  snare and not for the hi-hat (a low-pass, or a band-pass around the snare's body; a
  high-pass keeps the kick out), or a kick mic for the kick's lows (a low-pass). Pick
  the filter with the six buttons (low shelf, bell, high shelf, low-pass, band-pass,
  high-pass), then set **Freq**, **Q** (how narrow: bell, low-, band- and high-pass) and
  **Gain** (shelves and bell; the one a filter doesn't use is greyed out). The curve
  under them shows the filter; drag its dot across for the frequency and up and down for
  the gain or the Q (the bell's Q with Ctrl held, or the mouse wheel over the dot),
  double-click it to reset them. It works on the track's own sound too, without a
  sidechain. While the EQ is off its controls and curve are dimmed, but they can still
  be set, ready for when it is switched on.
- **Listen** (the headphones): hear the key, what the gate listens to (the sidechain,
  through the EQ), instead of the gate's output: handy for tuning the EQ. Switch it off
  again to hear the result.

Every control except Lookahead and Listen can be automated, and none of them clicks when
it moves.

### Limiter

A brick-wall limiter after Ableton's (Live 12.1): nothing comes out louder than its
ceiling, however loud what goes in. It looks a few milliseconds ahead, so it turns the
gain down smoothly just before a peak instead of clipping it. Put it last, on the master
(or on a track or bus you want held under a level), and turn **Gain** up until the gain
reduction shows the loudness you want: a few dB is transparent; much more squashes the
sound.

- **Gain** (−24 to +24 dB): pushes the input into the ceiling (or pulls it back).
- **Ceiling** (−24 to 0 dB): the most that comes out. Set it in the box at the top left
  of the display, or drag the line across the display up and down (Shift for finer
  steps; double-click it for −0.3 dB).
- **Maximize**: loudness from one control. The ceiling becomes a **Threshold** (the
  line, and its box) and the Gain knob an **Output** knob: lower the Threshold and
  everything comes up by as much, while what reaches it comes out at the Output level.
  At their defaults (both −0.3 dB, as the Ceiling's) switching it on changes nothing but
  setting Gain aside; lower the Threshold from there.
- **Release** (0.1 ms to 3 s): how fast the gain comes back after a peak. Short is
  louder and punchier; long is smoother. **Auto** (on by default) sets it from the
  music: quick after short peaks (about 50 ms), slower the longer limiting goes on (up
  to 600 ms), so a held bass note isn't distorted by the gain following its every cycle,
  and the level comes back soon after a lone peak. The Release knob is dimmed while Auto
  is on; you can still set it, for when you switch Auto off.
- **Lookahead** (1.5, 3 or 6 ms): how far ahead it sees peaks. Shorter is punchier but
  can distort the lows; take 6 ms for bass-heavy material. It is the device's latency:
  the other tracks are delayed to line up with it.
- **Mode**: **Standard** keeps every sample under the ceiling. **Soft Clip** rounds
  peaks off as they near the ceiling (from 6 dB under it) instead of turning everything
  down: louder, with some crunch. It works at the project's rate (no added latency), so
  bright sounds (above about 8 kHz) driven into it get some inharmonic grit as well: on
  cymbals or a bright synth alone, Standard is cleaner. **True Peak** also keeps the
  peaks *between* samples under it (what a converter or a lossy encoder may make of
  them): use it for streaming and mastering deliveries, with the ceiling at −1 dB.
- **L/R** or **M/S**: limit left and right, or the middle and the sides. **Link** (0 to
  100 %) is how much of one channel's gain reduction the other shares: at 100 % both are
  turned down together (the stereo image stays put); lower, each more on its own. In M/S
  with Link down, a loud centre (the kick, the vocal) no longer pulls the sides down
  with it. On very wide material, where the sides are as loud as the centre, M/S limits
  more than L/R: left and right are still kept under the ceiling.
- **The display** shows the last second and a half: the input in grey (red where it goes
  over the line), the output in light grey inside it, and the gain reduction in orange,
  hanging from the top. Beside it are meters of the input (In), the gain reduction (GR)
  and the output (Out), with their peaks of the last second under them, and the gain
  reduction of the last half second at the bottom left. The line glows orange while it
  limits. In Soft Clip a band shows where it rounds off, and what it rounded off shows
  in lighter orange under the gain reduction (and is counted in both gain reduction
  figures). With Maximize the history shows the input as it comes in and the output as
  far under the Output as the line is under it, so the loudest output meets the line;
  the Out meter and its figure still read the level that comes out.
- Every control but Lookahead can be automated, and changes don't click: the levels
  glide, the modes crossfade, and a new Lookahead fades out and back in (for a few
  milliseconds) as the latency changes.

### Multiband Dynamics

A three-band compressor and expander after Ableton's Multiband Dynamics, made mainly for
mastering and for shaping a mix's parts apart: tame the highs (de-essing) without
dulling the rest, tighten the lows, lift the quiet detail of a band for density, or the
squashed, everything-loud sound of Over The Top. Each band has **two** thresholds, so
two kinds of processing work on it at once:

| Region | Ratio over 1:1 (1:2, 1:4...) | Ratio under 1:1 (1:0.500...) |
|---|---|---|
| **Above** the Above threshold | downward compression: loud parts come down | upward expansion: loud parts go further up |
| **Below** the Below threshold | upward compression: quiet parts come up | downward expansion: quiet parts go further down (a gate, at the extreme) |

Ratios read as Live writes them, "1:4": how many dB past the threshold come out as one.
At 1:4 above −20 dB, a level of −8 dB (12 dB over) comes out at −17 dB (3 dB over); at
1:4 below −40 dB, −60 dB comes up to −45 dB. Under 1:1 the distance grows: at 1:0.500 it
doubles. 1:1 does nothing, which is where a new device starts: it changes nothing until
you set a ratio.

**The editor** has a row per band, High on top:

- **High**, **Mid** and **Low**: the bands' buttons, their activators. A band switched
  off here is bypassed: its Input, Output and dynamics stop acting, but its frequencies
  stay its own (no other band shapes them). **S** solos a band (more than one can be
  soloed).
- The **crossovers** under High and Low (Low-Mid 30 Hz to 3 kHz, Mid-High 300 Hz to
  15 kHz; 120 Hz and 2.50 kHz to start) set where the bands split, 24 dB per octave.
  Left as they are, the bands add back up to the input, with no dip or bump at the
  splits. The switch beside each splits its band off: switched off, the High (or Low)
  band's frequencies belong to the Mid band, which shapes them with its own settings
  (with both off the device is a single-band compressor and expander, Mid's controls
  acting on everything). As in Live, these two switches can't be automated. The
  controls of a band switched off or bypassed dim but can still be set.
- **Input** and **Output** per band (±24 dB): a band's level before its dynamics (moving
  it against its thresholds) and after them.
- **The display**: each band's lane, from −80 dB at the left to +6 dB at the right. The
  thick bar is the band's level after its dynamics, the thin bar under it its level
  before them, and the stretch between them shows the change, in orange where the level
  is brought down and teal where it is brought up, its figure at the lane's top right. A
  small triangle over the bar marks where the level is heading while attack or release
  are still on their way. The Below region is the block at the left, the Above region
  the block at the right, tinted the same way by what they do, and the more densely
  striped the further their ratio is from 1:1. A side glows while it is acting on the
  band. Hover a lane to read what a level comes out at ("−30.0 → −34.5 dB").
  - Drag a block's edge left or right for its threshold (Above and Below never cross:
    one pushes the other).
  - Drag inside a block up or down for its ratio: the block's level follows the mouse,
    so in the Above block *down* compresses and *up* expands, and in the Below block
    *up* lifts the quiet parts and *down* pushes them further down.
  - **Ctrl** while dragging moves every band's threshold (or ratio) together; **Alt**
    moves both thresholds of a band together, keeping the gap between them; **Shift**
    drags finely. Double-click an edge for its threshold's default, a block for 1:1. The
    mouse wheel works on edges (half a dB a notch) and blocks too; a touchpad's small
    steps add up, and a turn of the wheel stays with the edge it started on as the edge
    moves away from the mouse. Over the device chain, Shift with the wheel and
    Ctrl+Alt-dragging scroll the chain, as everywhere there.
- **T**, **B** and **A**, over the bands' buttons, switch the two fields beside every band
  between its **T**ime (Attack and Release, each 0.1 ms to 5 s), the **B**elow threshold
  and ratio, and the **A**bove threshold and ratio (−80 to 0 dB; 1:0.250 to 1:100). Type a
  ratio as Live writes it ("1:4", Over The Top's "1:66.7"), as the number after "1:" ("4",
  "0.5"), or as a compressor's ("4:1"); type a time as "250 ms" or "1.5 s" (a bare number
  is milliseconds). Above, Attack is how fast compression or expansion comes when the
  level rises past the threshold and Release how fast it lets go when it falls back;
  Below, the same as the level drops under the threshold and comes back.
- **Amount** (0 to 100 %) scales every ratio's effect: at 0 % nothing is compressed or
  expanded, so it dials a heavy setting back. **Time** (10 to 1000 %) scales every
  attack and release together. **Output** (±24 dB) is the device's output.
- **Soft Knee** brings compression and expansion in gradually over 6 dB around the
  thresholds. **Peak** reacts to short peaks; **RMS** (the default) to the average
  level, letting short peaks through. On stereo, both follow the louder side.
- **Sidechain** (the button, or the title bar's): another track's signal keys the bands,
  each by its own band of it, so a kick ducks the lows and a vocal the mids. **S/C Gain**
  (−70 to +24 dB) sets the key's level, **S/C Mix** how much of the trigger is the key
  rather than the device's own input; both can be set before a sidechain is chosen.
  **Listen** (the headphones) lets you hear what the bands react to instead of the
  output: the key, as much of it as S/C Mix takes (without a sidechain, the input). See
  [mixing.md](mixing.md#sidechains).

Every control can be automated (but the solos, the split switches and Listen, as in
Live) and changes without clicks. The device
adds no latency. Upward compression fades out between −72 and −96 dB, so silence, and
whatever lies under −96 dB, isn't lifted; a noise floor above that is, as Over The Top
lifts it (hiss at −80 dB under a Below of −40 dB at 1:4 comes up 20 dB).

The Over The Top sound, by hand: on every band Above −30 dB at 1:66.7 and Below −40 dB
at 1:4, attack 10 to 50 ms, release 130 to 280 ms, Output +6 dB; then bring Amount down
until it is enough.

### Spectral Compressor

A compressor for every frequency at once. It splits the sound into about a thousand
narrow bands, and turns each one down where it is louder than a threshold line drawn
across the spectrum, and (if you want) up where it is quieter than a second line,
leaving the rest alone. Use it to tame harshness and ringing resonances where they
happen, to even out a spectrum, to bring up low-level detail and air, or, keyed by
another track, to duck one sound's frequencies only where they clash with another's (a
bass under a kick, a pad under a vocal).

**The display** shows the sound in the middle of the device, from 20 Hz at the left to
20 kHz at the right:

- grey (filled): the spectrum coming in; blue (a line): the spectrum going out;
- the **orange line**: the threshold. Where the orange glows between it and the
  spectrum, the sound is over it, and that is what is being turned down;
- orange **hanging from the top**: how far each frequency is turned down right now
  (24 dB reaches halfway down), with a thin line holding the deepest recent cut for a
  moment before it falls away;
- the **green line** (while Upward is past 1:1): Below. Green **rising from the
  bottom**: how far each frequency is brought up;
- the dimmed sides: outside the **Focus** band, where nothing is changed;
- the figures at the top right: the deepest cut now (and the biggest lift); the
  **Sidechain** badge at the top left lights up while another track keys it (click it to
  choose one, as with the sidechain button in the title bar); the meters at the right
  edge are the level in and out.

**Levels are measured against pink noise**: pink noise at −20 dBFS reads −20 dB at every
frequency, so a dense mix reads around its loudness across the spectrum (a mix at −14 dB
roughly −14 to −20 dB). A pure tone reads about 20 dB above its peak level (all its
energy is in one place), which is why resonances, and bass notes, stand out. As it
comes, with the threshold at −18 dB and a ratio of 2:1, it takes a few dB off a dense
mix, most where tones stand out (bass notes, ringing), and leaves pink noise at −20 dBFS
nearly alone (at −14 dBFS about 2.5 dB).

**Setting it up**:

- **Threshold** (−72 to +12 dB) and **Ratio** (1:1 to 20:1): drag the orange line (its
  dot at 1 kHz, or anywhere on it) up and down, or turn the knobs. Start with the line
  just under the loud parts and lower it until the orange glow covers what bothers you.
  (At 1:1 nothing is turned down, and the glow goes out.)
- **Tilt** (−6 to +6 dB per octave): drag either end of the orange line (the small
  diamonds at 100 Hz and 10 kHz; turned steeply enough to leave the display, a diamond
  waits where the line leaves it) to turn it, and the green line with it, about 1 kHz.
  At 0 the threshold follows pink noise; most mixes sit a little under pink at the top,
  so at 0 their highs are treated more gently than their lows. A slightly negative tilt
  lets the line follow such a mix, treating the highs as firmly as the lows; raise it to
  leave the highs alone.
- **Below** (−72 to +12 dB) and **Upward** (1:1 to 10:1): with Upward past 1:1, the
  green line appears at Below; frequencies under it are brought up towards it, for
  density and detail. Below is never above the threshold (dragged over it, it stays on
  the orange line). Silence and hiss far down (under −90 dB) are left alone. While
  Upward is at 1:1 the Below knob is dimmed: it does nothing then, but you can still
  set it ready.
- **Smoothing** (0 to 100 %): how wide a band each frequency's level is measured over.
  Low, each narrow band is judged alone: the most selective, for single resonances
  (whistles, ringing, a harsh "s"). High (up to two octaves), broad and gentle: for tone
  shaping.
- **Knee** (0 to 24 dB): how gradually it starts acting around each line. **Range**
  (0 to 48 dB): the most any frequency is turned down or up, a safety net.
- **Focus** (Low and High, 20 Hz to 20 kHz): drag the faint edges at the plot's bottom
  corners in from the sides, or set the two boxes beside the display (drag, scroll or
  type a value; right-click for automation and macros), to work on one region only, for
  example 2 to 8 kHz for harshness or 150 to 500 Hz for mud. Nothing outside is changed;
  the change fades out over a third of an octave beyond each edge.
- **Attack** (1 ms to 1 s) and **Release** (10 ms to 5 s): how fast each frequency's
  gain follows its level down and recovers, as on the Compressor (the same 150 ms
  releases as fast here as there). (The analysis itself smooths onsets over about 20 ms,
  so the shortest attacks act alike.)
- **Stereo Link** (0 to 100 %): at 100 % both channels get the same gains (the louder
  one's), keeping the stereo image; at 0 each channel is compressed on its own.
- **Dry/Wet** (0 to 100 %) and **Output** (±24 dB). The dry sound is delayed to stay in
  time with the processed one, so they blend cleanly.
- **Delta**: hear only what the device changes: what it takes away (what it adds comes
  out inverted). The blue line turns red and shows that spectrum (the orange from the
  top fades back, as the line now shows it). Use it while setting Threshold and
  Smoothing: you should hear only the harshness or the ringing, not the music.

Shift drags a tenth as fast, from where you press it on; double-click a line, a handle
or an edge to reset it. Every drag is one undo step, and every control can be automated.

**Keyed by a sidechain**, the other track's spectrum sets the gains: where it is loud,
this track is turned down at those frequencies only. The other track's levels show as a
dashed yellow line.

It adds **latency**: 53 ms at 48 kHz (58 ms at 44.1 kHz). Playback is compensated, but
you'll feel it while monitoring live input through it.

### Saturator

Saturation and distortion after Ableton's Saturator: the sound is driven into a curve
that rounds off, clips or folds its peaks, from a touch of warmth on a bus to a
fuzzed-out bass. Its editor is laid out like Live 12's with the expanded view open: the
front panel (Drive, the curve, Output and Dry/Wet), then Color, then the curve's own
controls.

- **Drive** (−36 to +36 dB): how hard the sound goes into the curve. The more, the
  further up the curve it reaches and the more it is squashed. Turn **Output** down by
  about as much to compare at the same loudness.
- **Type**, the list under Drive: the curve.
  - **Analog Clip** (the default) and **Digital Clip** clip the peaks: Analog Clip with
    a rounded corner (and untouched below about −2.5 dBFS), Digital Clip flat and hard.
  - **Soft Sine**, **Medium Curve** and **Hard Curve** bend the sound more gently, for
    warmth: Soft Sine is the roundest (and a little louder), Hard Curve keeps the most
    of the straight part before it bends.
  - **Bass Shaper**, for 808s and sub bass: straight below its **Threshold** (−50 to
    0 dB, the knob on the right) and saturating smoothly above it. A low threshold is a
    soft saturation, a high one close to a hard clip; low thresholds with plenty of
    Drive keep a bass's low end round while it gets louder and grittier.
  - **Sinoid Fold**: below full scale like Soft Sine, but driven past it the sound folds
    back over itself: a bright, strange distortion for effects.
  - **Waveshaper**: a curve of your own from the six knobs on the right: **Drive** how
    much of it is heard (0 % none), **Lin** the slope of its straight part, **Curve** a
    bend that adds mostly third harmonics, **Damp** a very fast gate that flattens quiet
    sound around silence, **Depth** and **Period** ripples of a sine over the curve (how
    big, and how many): metallic, folded sounds. Below Drive 100 % the Waveshaper can
    get very loud: use Post Clip. Its knobs are dimmed while another curve is chosen
    (still editable).
- **The curve** shows the shaping: the input across, the output up, the dashed diagonal
  the untouched sound. As the sound plays, two dots ride the curve at its level and the
  part of the curve it uses lights up, from amber to red the harder it is squashed;
  where loud hits have just been stays faintly lit for a moment. Red bars flash at the
  sides when the input itself goes over full scale. The strip under the curve is the
  input's level (it ends under the dots) and the strip at its right the output's; the
  ticks are their recent peaks. Drag the curve up and down for Drive, and across for the
  Bass Shaper's Threshold or the Waveshaper's Curve (Shift for finer steps);
  double-click it to set Drive back to 0 dB. The curve is worked out from the very same
  arithmetic the device plays.
- **Post Clip**, the list under the curve (**No Clip** to start with): **Soft Clip**
  (the Analog Clip curve again) or **Hard Clip** after everything else but Output, the
  dry sound Dry/Wet blends back in included, so the output never goes over full scale
  (times Output): a safety ceiling against Color's boosts, a loud Waveshaper or a hot
  input. Its ceiling shows as red dashes over the curve. With HQ on it clips at four
  times the rate too, so it adds no harsh, unrelated tones of its own; the price is that
  bright, hard-clipped sound can then peak a little over the ceiling.
- **Output** (−36 to 0 dB): the level out. **Dry/Wet** (0 to 100 %) blends the untouched
  sound back in: around 30–50 % for parallel saturation, which keeps the transients and
  adds the grit underneath.
- **Color** steers which frequencies saturate: an EQ before the curve and its exact
  opposite after it. A clean sound passes unchanged; once the curve bends, what the EQ
  boosts is driven harder (and comes out quieter, a dip), and what it cuts stays clean
  (and comes out louder, like a resonance). **Base** (±36 dB) does this to the lows
  below about 150 Hz: negative keeps the fundamental clean and full under a distorted
  top. **Freq**, **Width** and **Depth** do it to a band of your choice. The graph shows
  the EQ over the input's spectrum (filled) and the output's (the line), with the Color
  switch above it; drag its dots (Base up and down, the band's across for Freq and up
  and down for Depth), which also switches Color on; double-click a dot to set it back
  to 0 dB. While Color is off its knobs are dimmed.
- **DC** removes DC offset from the input (useful on recordings that sit off centre).
  **HQ** (Hi-Quality) shapes and clips at four times the sample rate, so loud, bright
  sounds driven hard don't fold back as harsh, unrelated tones; it costs more CPU and
  adds 36 samples of latency, which is compensated (the tracks stay in time).
- Everything but HQ can be automated, and nothing clicks as it changes: the knobs glide
  and the lists cross-fade.

### Amp

A guitar amplifier after Ableton's Amp: seven classic amps, each with an amp's dials.
Like a real amp's line output (and like Live's Amp), it has no speaker: alone it sounds
bright and fizzy. Put a cabinet after it, or an EQ's low-pass somewhere around 5 kHz, to
hear it as a miked amp. Its editor is laid out like Live's: the models across the top,
the six dials under them, Output and Dry/Wet at the right, and under them what the amp
is doing.

- **The models** (the row of buttons; a line in the model's colour slides under the one
  chosen):
  - **Clean**: the bright channel of a classic 1960s British amp: chiming, clean until
    pushed. Clean chords, funk.
  - **Boost**: the same amp's other channel with more gain: edgy rhythm parts and riffs,
    crunchy at noon.
  - **Blues**: a bright 1970s amp with plenty of clean headroom and a deep mid scoop:
    country, blues, rock. Turned up, its power amp breaks up and sags.
  - **Rock**: a classic 1960s 45-watt rock amp: crunchy, with strong mids. Riffs and
    classic rock.
  - **Lead**: the modern channel of a high-gain amp: tight, saturated and fizzy. Leads
    and metal.
  - **Heavy**: the same amp's vintage channel, looser and darker: crunchy at noon, heavy
    with Gain up; grunge, dirty rhythm parts. Turned up, its power amp breaks up and
    sags: Volume distorts it more.
  - **Bass**: a rare 1970s PA: a powerful low end and a fuzzy, lopsided edge. Bass, or
    fuzz on anything.

  Every model is level-matched at its defaults, so switching between them compares their
  sound, not their loudness; a switch glides from one amp to the next in about 50 ms,
  keeping the level on the way, so it can be automated.
- **Gain** (0 to 10): how hard the sound goes into the preamp: the main control of how
  much the amp distorts.
- **Bass**, **Middle**, **Treble** (0 to 10): the tone stack, a real amp's passive one
  with each model's own parts. As on a real amp they interact: turning one moves the
  others' part of the curve too, and they are gentle (a few dB each way at their ends;
  Middle at 0 scoops the mids most). Turning them up drives the next stage harder, so
  more of them can mean more distortion too.
- **Presence** (0 to 10): the power amp's edge: a shelf on the highs where they meet the
  power stage, 8 or 9 dB either way at the ends (5 is flat). Up, it adds crispness and
  bite; down, it smooths the top.
- **Volume** (0 to 10): the power amp's level, 12 dB either side of 5. On Blues, Heavy
  and Bass the power amp breaks up near the top and the supply sags under loud notes (a
  squash and bloom): turned up, Volume adds distortion of its own. On the others it is
  mostly the level.
- **Output**: **Mono** sums both channels through one amp (half the CPU: right for a
  guitar or a mono bass); **Dual** runs an amp for each channel, keeping a stereo
  source's width at twice the CPU. Its parameter is *Dual Mono*, as in Live.
- **Dry/Wet** (0 to 100 %): blends the amp with the untouched sound, for parallel
  distortion (a bass keeps its clean low end under the grit). The dry sound is delayed
  to line up with the amp's, so they never comb.
- **The tubes** at the left glow as hard as each stage is driven: the three preamp tubes
  (V1, V2, V3) and the power tube (P), flaring with each note and cooling after. The
  power tube turns blue as the supply sags.
- **Drive**, under the tubes, is the amp's transfer curve at these settings: for a tone
  going in at each level (across, full scale at the edges), how high its wave comes out
  at the right and how low at the left (up), worked out from the amp's own stages and
  filters. Gentle settings bend it into an S; high gain turns it into a step, the sound
  clipped flat. It is scaled to its own height, so it shows the shape: a curve whose top
  and bottom differ is clipping one side of the wave first (the even harmonics of a tube
  amp). As the sound plays, two dots ride it at the input's peaks, where the sound's
  peaks come out, with a short trail behind them, and the part the sound uses lights up;
  the input's peak is read out at the bottom right. As the supply sags, the curve's
  shoulder dips.
- **Tone** shows the tone stack and Presence as they sound, from 30 Hz to 16 kHz, worked
  out from the very filters the amp plays. Drag the **B**, **M**, **T** or **P** handle
  up and down, or turn the wheel over it, to set Bass, Middle, Treble or Presence (Shift
  for finer steps; one undo step per drag); double-click one to put it back to 5;
  right-click one for its automation menu, as on its knob, whose red dot it shows too.
- **The pilot lamp** at the bottom right glows with the output and dims as the supply
  sags, over the model's name. The meter at the far right is the output's level.
- Every control can be automated, and nothing clicks: the dials and the models glide,
  and Output crossfades. The amp delays the sound by 37 samples (the oversampling),
  which other tracks are delayed to line up with.

### Erosion

Digital grit, after Live 12.4's Erosion: the sound is read out of a very short delay
(2 ms) whose length a sine or filtered noise wobbles very fast. Every frequency in the
sound is smeared into sidebands around it, the more the higher it is: lows survive,
highs turn to fizz and hiss, and what is pushed past the top of the spectrum folds back
down as aliasing, the "digital" sound the device is for. A sine gives metallic,
ring-modulated tones; noise erodes; narrow noise at a low Frequency is a random,
wobbling vibrato.

- **Frequency** (20 Hz to 18 kHz): the sine's frequency, or the middle of the noise's
  band: the colour of the distortion.
- **Amount** (0 to 100 %): how far the modulation moves the delay, up to about ±1.4 ms.
  It grows with the square of the knob, so the lower half is the fine part. At 0 % the
  sound passes through untouched. There is no Dry/Wet (Live's has none either): to blend
  it in, put it in a rack chain beside a dry one.
- **Width** (Filter Width, 0.1 to 10 octaves): how wide the noise's band is. Narrow, the
  noise is nearly a wandering tone and erodes selectively; wide, it hisses over
  everything. It does nothing to the sine, and is dimmed while Noise Blend is 0 %.
- **Noise Blend** (0 to 100 %): from the sine alone to the noise alone, both in between
  (at equal power, so the strength stays the same). The sine and noise pictures beside
  it light up with each.
- **Stereo** (Stereo Width, 0 to 100 %): from the same modulation on both sides to
  independent noise on each side, and the sine's sides a quarter cycle apart. Each
  Erosion has noise of its own, so two set alike (on a pair of double-tracked parts,
  say) don't wobble together.
- Live's old Erosion (Erosion Legacy) had a Mode: **Sine** is Noise Blend 0 %, **Noise**
  is Noise Blend 100 %, and **Wide Noise** is Noise Blend 100 % with Stereo 100 %.
- **The display** shows the input's spectrum (filled, grey) and the output's (a line
  over it: what Erosion adds shows at the top), the noise's band (orange: as wide as
  Width, as high as Amount, worked out from the very filter that plays) and the sine (a
  blue spike). Drag the dot: across for the Frequency, up and down for the Amount (a
  click puts it where you click). Shift-drag moves it finely; Alt-drag up and down sets
  the Width, and so does the mouse wheel (with Ctrl, finely). Each drag is one undo
  step. While the sound is being eroded the band shimmers (with two edges as Stereo
  widens), the dot glows and the sine's spike trembles; in silence it all rests.
- **L/R Mod** shows the modulation, left against right: a line in mono, opening into an
  ellipse and then a round cloud (noise) or a circle (the sine) as Stereo widens.
- Every control can be automated, and changes glide (in about 50 ms), so they never
  click.
- It adds 2 ms of latency (the middle of its delay): the other tracks are delayed by as
  much, so everything stays in line.

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

### Chorus-Ensemble

Thickening, chorus and vibrato, after Ableton's Chorus-Ensemble: the sound is copied
into short delays whose length an LFO keeps moving, and the copies, slightly detuned and
spread across the stereo field, are added to it. Gently it thickens and widens a sound;
deeper it is the classic chorus shimmer; with feedback it heads towards flanging.

- **Mode**, the three tabs over the display:
  - **Chorus** (Live's Classic): one or two delays a side. **Taps** chooses: **2** (the
    classic dual chorus, the two moving opposite ways) or **1** (simpler and thicker, as
    a hardware pedal). **Time** chooses how long the delays are: **Auto** follows the
    Amount (a short, gentle chorus at low Amount, a deeper and longer one at high),
    while **7**, **10**, **20**, **35** and **50 ms** hold the delay where it is
    whatever the Amount: the longer ones avoid the phasing a short chorus puts on basses
    and guitars.
  - **Ensemble**: three delays a side, their movements a third of a cycle apart: a
    thicker, smoother, string-machine sound.
  - **Vibrato**: one delay a side, moved further: its pitch wobbles. Fully wet (Dry/Wet
    100 %) it is a pure vibrato. **Offset** (0 to 180°) sets how far apart the left and
    right move (180°: opposite, a wide, swirling vibrato), and **Shape** morphs the
    movement from a sine (0 %) to a triangle (100 %), whose pitch jumps between two
    notes.
- **Rate** (0.1 to 15 Hz): how fast the delays move. Slow, a gentle drift; fast, a
  warble.
- **Amount** (0 to 100 %): how far they move, and so how far the copies are detuned. At
  0 % the copies are steady (a short doubling, or with Warmth a gentle saturation of its
  own).
- **Feedback** (0 to 100 %, Chorus and Ensemble): how much of each side's output goes
  back into its delays: more extreme, brighter, metallic towards the top, and the delays
  ring on for a moment after the sound stops. **Ø** flips the feedback's polarity:
  hollow at high Feedback. Vibrato has no feedback: there both are dimmed, and what you
  set them to waits for Chorus or Ensemble.
- **Width** (0 to 200 %, Chorus and Ensemble): the chorused sound's stereo width: 0 %
  mono, 100 % as it is, wider above. Even a mono sound comes out in stereo: the two
  sides' delays move differently.
- **Warmth** (0 to 100 %): a subtle saturation and darkening of the chorused sound, as
  an analogue chorus's: mostly even harmonics, and no squashing (quiet sounds keep their
  level, a full-scale one loses about a dB). With Amount at 0 % it works as a gentle
  saturation of its own.
- **High-pass** (the switch under the display) and its frequency (20 Hz to 2 kHz): below
  it the sound isn't chorused; the lows pass through steady and whole at any Dry/Wet,
  keeping a bass or a kick solid. With it on, the dry sound goes through the same
  crossover, even fully dry: its level is unchanged, but its phase turns around the
  frequency (so it stays in step with the steady lows, and Dry/Wet blends them without
  cancelling).
- **Output** (−36 to +6 dB): the level of the chorused sound. **Dry/Wet** (0 to 100 %):
  the dry sound blended with it.
- **The display** shows each delay as it moves, newest at the right where a dot rides
  it, the left side's voices in orange and the right side's in blue, against the delay
  in ms (the range Amount 100 would cover; in Auto the dashed centre rises and falls
  with the Amount). The figure at the top right is how far the copies are detuned at
  most (in cents, or semitones). Drag up and down in it for the Rate, or across for the
  Amount (the drag's first move picks which, and it changes only that one; Shift:
  finely); each drag is one undo step. As sound passes the traces glow; in silence they
  rest, and with the device off or nothing playing they stop and dim.
- Every control can be automated, and changes don't click: Mode, Taps and Time crossfade
  to the new voices, and the rest glide. Switched on in the middle of a sound, its copies
  of the sound fade in too.
- Tips: for a surf guitar, Ensemble at 1 to 1.8 Hz with Amount at 100 %; for a vibrato,
  Vibrato with Dry/Wet at 100 % and the Rate at 5 to 7 Hz; for bursts of decaying
  oscillation, automate Ø with Feedback above 90 %.
- Its delay is the effect, not latency: other tracks aren't delayed to line up with it.

### Phaser-Flanger

Phasing, flanging and doubling in one device, after Ableton's Phaser-Flanger. The sound
is mixed with a moved copy of itself, which cuts notches into its spectrum; an LFO keeps
the copy moving, so the notches sweep: the classic swoosh of a phaser, the jet-plane
whoosh of a flanger, or (with a longer delay) a second, slightly wandering take.

- **Mode**, the tabs at the left:
  - **Phaser**: the copy goes through a chain of all-pass filters. **Notches** (1 to 42)
    is how many filters, one notch each; **Center** (70 Hz to 18.5 kHz) where the
    notches sit; **Spread** (0 to 100 %) how far apart they are (narrow filters, close
    notches at 0 %; far apart at 100 %); **Blend** (0 to 1) what the LFO moves: the
    Center at 0, the Spread at 1, both in between.
  - **Flanger**: the copy is delayed by **Time** (0.1 to 20 ms), which makes a comb of
    evenly spaced notches; a shorter time moves them up (the first notch, shown under
    the knob, is at 1 / (2 × Time)).
  - **Doubler**: a longer **Time** (20 to 150 ms): a gently moving second take, for
    width and thickness.
- **Amount** (0 to 100 %): how far the LFO (and the envelope, below) moves the sweep: up
  to three octaves of the Center either way, two octaves of the Flanger's Time, 15 % of
  the Doubler's.
- **The LFO**: **Freq** (0.01 to 40 Hz: from a slow sweep to a fast warble), or with the
  **♪** switch on, **Rate** in note values (Live's: 1/64 to 3/4 of a bar, triplets among
  them, then 1 to 8 bars; a value a wheel notch), the LFO then following the song; the
  **waveform** (Sine, Triangle, Triangle Analog: a rounded square that gets rounder and
  quieter the faster it runs, Triangle 8 and 16: stepped, Saw Up, Saw Down, Rectangle,
  Random: smooth, Random S&H: stepped); **Duty** (−100 to 100 %), which bends the shape
  (a rectangle's width, the others' skew); and **Phase** (0 to 360°), how far the right
  channel's LFO runs ahead of the left's (180°: opposite, a wide stereo sweep). With
  **Spin** on, the right LFO instead runs faster than the left by **Spin** (0 to 50 %:
  at most half as fast again), so the two drift in and out of step.
- **Feedback** (0 to 100 %): the output fed back in: sharper, ringing notches and a
  stronger, more metallic sweep. **Ø** flips its polarity, which moves the resonances
  between the notches (hollow, nasal).
- **Warmth** (0 to 100 %): a little saturation and darkening of the effect, also as it
  feeds back.
- **Output** (−36 to +6 dB) and **Dry/Wet** (0 to 100 %): at 50 % the notches are
  deepest; at 100 % only the moved copy is heard (a Flanger then becomes a vibrato, a
  Doubler a wandering delayed copy).
- **More** shows the rest:
  - **LFO 2** (0 to 100 %): crossfades the modulation from the first LFO to a second, a
    triangle at its own **Freq** (or with its ♪, **Rate**). Two rates against each other
    give a more irregular sweep.
  - **Env** follows the input's level and moves the sweep with it, by **Env Amount**
    (−100 to 100 %: negative moves it the other way): louder notes sweep further.
    **Attack** (0.1 to 30 ms) and **Release** (0.1 to 400 ms) are how fast it follows.
  - **Safe Bass** (Off at 5 Hz, up to 3 kHz): everything below it stays out of the
    effect, so a bass line or a kick keeps its weight while the rest sweeps.
- **The graph** shows the response as it plays: the notches (or the comb) moving with
  the LFO, each Phaser notch marked along the bottom, the right channel's curve in blue
  when it differs, the envelope's level as a bar at the left, and In and Out meters. A
  comb too fine to draw as a line (the Doubler's, high up) is shown as a band between
  its peaks and its notches. Under it the LFO's shape with a dot riding it (the right
  channel's in blue) and, at the right, a bar of the total modulation the sweep follows.
  With nothing playing it shows where the knobs put things. Drag in it: across for the
  Center (Flanger: the comb's first notch lands under the mouse; Doubler: the Time), up
  and down for the Spread (Flanger and Doubler: the Feedback); each drag is one undo
  step; double-click to reset them (one undo step too, which takes them back to where
  they were).
- Every control can be automated, and changes don't click: the controls glide, a change
  of mode or of Notches crossfades, and an LFO that jumps (a new waveform, Sync
  switched, the song jumping) fades to its new place. Switched on while something plays,
  the Flanger's and the Doubler's delayed copy comes in smoothly too.
- Tips: a slow Triangle (0.1 to 0.3 Hz) with 4 to 8 notches and some Feedback is the
  classic phaser; a Flanger at 1 to 3 ms with 70 % Feedback is the jet whoosh (Ø for a
  hollower one); a Doubler at about 30 ms with Phase 180° and little Amount widens a
  vocal or a guitar.

### Reverb

An algorithmic reverb after Ableton's: a room's early reflections, then a diffuse tail
that dies away, each band for its own time. Like Live's, it is mono in, stereo out: left
and right are summed before the reverb (a sound panned hard left reverberates in the
middle), and **Stereo** sets how wide the reverb comes out. On a return track, set
**Dry/Wet** to 100 %; on a track, blend it in (40 % to start with).

A good order to set it in: the band it hears, **Predelay** and **Size** first, then the
reflections (**Shape**, **Spin**), then how long it rings (**Decay** and the graph's
handles), then the levels. The editor goes left to right:

- **Input**: **Lo Cut** and **Hi Cut** take the lows and highs off what goes into the
  reverb (not off the dry sound): a reverb with less mud and hiss. The pad shows the
  band over a spectrum of the input: drag its dot across for the band's centre, up and
  down for its width (0.5 to 9 octaves; Shift for finer steps), or type them into the
  boxes under it. The band glows as sound comes in.
- **Early reflections**: the first echoes off the room's walls; the first comes
  **Predelay** after the sound (0.5 to 250 ms: 1 to 25 ms for natural rooms; longer
  keeps a vocal clear of its reverb). **Shape**: low, the reflections fade slowly and
  the tail starts early, blending with them; high, they fade fast and the tail starts
  later, apart from them. **Spin** makes them drift in time and swing around the stereo
  field, so the early sound is less fixed and coloured; the tail hears the room drift
  with them. A little (the default) keeps them alive; fast, it gives doppler pitch and
  swirling pans. (For a tail that rings less metallic, use **Chorus**, below.) Its pad
  shows the reflections as dots, placed across by where they sit in the stereo field and
  down by when they come (the first at the top), as big as they are loud; they light up
  as they sound and swing as Spin moves them. Drag across for Spin's rate and up and
  down for its amount. At the top right is when the tail starts after the sound comes
  in.
- **Global**: **Size** (0.22 to 500; 100 is a medium room) scales every delay of the
  reverb: small sizes ring metallic, huge ones grainy. **Smooth** is how a change of
  Size is heard: **None** at once (the tail's pitch sweeps fast), **Slow** or **Fast**
  gliding. **Stereo** (0 to 120°): mono at 0; at 120 each side hears a reverb
  independent of the other, as in a real room; the default 100 is a little narrower.
  **Density** trades richness for CPU: **High** (the default) is the smoothest;
  **Sparse** is grainy and light, a lo-fi sound of its own.
- **Diffusion network** (the tail): **Decay** (0.2 to 60 s) is how long it takes to fall
  60 dB. The graph shows how long each frequency rings, with the tail's spectrum dying
  away behind the curve and a meter of its level beside it. Drag the middle handle up
  and down for Decay. The **Lo** and **Hi** shelves make the lows and highs die away
  sooner (a darker, or less boomy, room): drag their handles across for where they start
  and up and down for how long their band rings, as a share of Decay (20 to 100 %; the
  dashed line out to the edge is where the curve settles), or use the boxes under the
  graph. Double-click a handle (or use the buttons above) to switch a shelf off. The
  high filter can be a **Low-pass** instead of a shelf: every pass round the room
  darker, like a real room's air; it has no share, and its handle moves across only.
  **Diffusion** is how quickly the echoes blur into a smooth tail; **Scale** how coarse
  that blur is (most noticeable in small rooms). **Chorus** makes the tail's echoes
  drift in pitch (**Amount**, **Rate**), against a metallic ring; the curve ripples with
  it while the tail sounds.
- **Freeze**: the tail holds for ever, for pads and drones (the curve rises to the top,
  in blue, and the handles dim: they are what it thaws to). With **Cut** (on by default)
  new sound no longer reaches it, so play a chord, freeze, and play over it; without
  Cut, what you play keeps adding to it. With **Flat** (on by default) every band holds;
  without it the shelves still take their bands away, and the frozen tail slowly
  darkens. Flat and Cut are dimmed while Freeze is off: they only act frozen. Changing
  **Density** while frozen keeps the tail in the lines the two share (High and Mid share
  them all; going down to Low or Sparse loses part of it).
- **Output**: **Reflect** and **Diffuse** (−30 to +6 dB) are the early reflections' and
  the tail's levels: more Reflect for a close, defined room, more Diffuse for a
  washed-out space. **Dry/Wet** blends the input with the reverb. Longer decays are
  louder, as in Live; at the same Decay a bigger room is a little quieter.
- Every control can be automated, and changes don't click: Size and Predelay glide (Size
  at Smooth's pace, and changing Smooth mid-glide eases the sweep), the switches and
  Density crossfade, the levels glide. Its sound is the reverb, not latency: nothing is
  delayed to line up with it. It costs about 1.4 % of a core at the defaults (Sparse
  under half of that), and almost nothing once its tail has died away.

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
- **Dry/Wet** (0 to 100 %): blends the untouched sound with the dispersed one. In between
  they add up as a phaser's do, cutting notches where the dispersion has turned a
  frequency half a circle (deepest at 50 %); fully wet, nothing is louder or quieter.
- **Bypass**: the sound passes through untouched, fading over about 20 ms. The stages
  keep running, so switching it back is seamless (the device's on/off switch stops them).
- **The graph** shows how late each frequency comes out, from 0.1 ms at the bottom to 30 s
  at the top, a decade per line (64 narrow stages at 20 Hz hold 20 Hz back by 20 s). The
  scale stays put: an octave lower, the same settings delay twice as long, and the curve
  slides up by the same distance, keeping its shape. It is worked out from the very
  filters the engine plays, at its sample rate. Drag the dot across for the Frequency, up
  and down for the Pinch. Bypassed or fully dry, it is greyed.
- Every control can be automated, and changes don't click: Amount crossfades to the new
  number of stages (the stages added fade in from silence), and Dry/Wet, Bypass,
  Frequency and Pinch glide. Swept quickly through many narrow stages, Frequency and Pinch are
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
