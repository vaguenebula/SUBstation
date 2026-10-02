# SUBstation

A basic DAW for Windows with an Ableton-style arrangement view. The UI and
editing logic are Python (PySide6/Qt 6); the real-time audio engine is C++
(ASIO, or WASAPI through miniaudio), bound with nanobind.

**What works today**
- Arrangement timeline with any number of audio and MIDI tracks, waveforms and note previews, adaptive grid with snapping, zoom and scroll.
- Clip editing: move (also across tracks of the same kind), Ctrl-drag to copy, trim either edge, split, duplicate, delete. Selecting is always on the grid, which shows through the clips and runs all the way down: clicking a clip's title selects the area it covers, Shift-clicking another selects the area that fully contains both (on the tracks between too), and dragging anywhere in the lanes (or below the tracks) selects a time range; in a lane's title band it selects the clips it touches. Double-click a clip to open it in the clip view (the piano roll for MIDI). Dragging inside a time selection moves (or copies) just that stretch, splitting clips at its edges. Overlaps follow Ableton's rule: the clip you place wins.
- Clip view (double-click a clip): for audio clips, warping, transpose/detune, clip volume and pan, for one clip or many at once. For a MIDI clip, the piano roll (below).
  - **Warp** locks a clip to the beat grid. Its audio is taken to be at the *Seg. BPM* and is stretched in real time to follow the project tempo. Turning Warp on sets Seg. BPM to the current tempo, so nothing moves until the tempo changes.
  - Warp modes: *Transients* (short stretch blocks, tight attacks), *Standard* (all-round), *Smooth* (long blocks, for pads and textures), *Formants* (Standard, keeping formants when transposing), and *Re-Pitch* (no stretching: speed and pitch change together, like a turntable). Projects saved with the earlier Ableton-style names load into the equivalent mode.
  - **Transpose/Detune** shift the pitch without changing the speed, warped or not (except in Re-Pitch).
- **Tempo and key from file names.** Audio you drag in (or double-click in the browser) is set up from its name, as sample packs name loops (`Bass_Loop_128_Am.wav`, `Keys 92bpm F# minor`, `Vox_Ebmaj_140BPM`):
  - A tempo in the name (`128`, `128bpm`, `bpm128`) warps the clip with that Seg. BPM, so the loop plays in time at any project tempo. Files 6 seconds or longer are warped even without one (at the project tempo, as when you turn Warp on).
  - A key in the name (`Am`, `F#m`, `Eb`, `G minor`, `Cmaj`) transposes the clip to the **project key**, chosen next to the metronome. A minor key and its relative major count as the same (an A minor loop stays put in C major), and the clip takes the shortest shift (at most 6 semitones). With *No Key*, nothing is transposed. A bare letter (`_D_`) counts only next to a tempo, so names like "A Day" are left alone.
- MIDI tracks (Ctrl+Shift+T) come with the built-in **Synth**. To make a MIDI clip, press Ctrl+Shift+D (or use Create › Insert MIDI Clip, or right-click a MIDI track › Insert MIDI Clip): it fills the time selection, or is a bar long at the insert marker (or where you right-clicked), and opens in the piano roll.
  - A MIDI clip is a window onto its notes, like an audio clip onto its file: trimming or splitting it hides notes but never deletes them. Its length is in beats, so it doesn't change with the tempo.
  - As in Ableton, a clip plays the notes that start inside it and cuts them at its end.
- Piano roll: keys (click to hear a key and select its notes), a ruler in the clip's own time (click to play from there), notes, and a velocity lane.
  - Double-click to add a note (one grid step long) or delete one. Drag notes to move them (Ctrl copies, Alt ignores the grid), drag their ends to resize, drag in empty space to select.
  - Delete, Ctrl+A, Ctrl+D (duplicate), Up/Down (Shift: an octave), Left/Right (a grid step; Shift: a bar).
  - Drag a stem in the velocity lane to change velocities; several selected notes change together.
  - Selecting a group of notes by dragging a rubber band (or with Ctrl+A) brings up a small tool bar that glides in next to them; clicking or drawing single notes doesn't. It acts on the selected notes and hides while you drag them:
    - **Legato** makes each note last until the next one starts (a chord's notes together); the last ones reach the next note after them, or the clip's end if there is none. A note never runs into the next note on its own key.
    - **×2** and **÷2** double or halve the notes' timing: they spread out from (or draw in toward) the first one, and their lengths scale with them.
    - **Quantize** (Ctrl+U) moves note starts onto a grid (1/4 to 1/32, or triplets), by an amount from 0 to 100 %. Lengths stay. With nothing selected, Ctrl+U quantizes every note.
    - **Humanize** nudges starts and velocities at random. At 100 % a note moves by up to a 32nd note and its velocity by up to 24; the default is 25 %.
  - Notes you click, add or move are played on the track's instrument (the headphones button turns this off). The part of the clip that plays is lit; the rest is dimmed.
- Synth: a polyphonic subtractive synth (16 voices) with sine, triangle, saw and square oscillators (band-limited saw and square), an ADSR envelope, a resonant low-pass filter and volume. Velocity sets the level.
- Over The Top: a built-in multiband upward/downward compressor in the style of the one every drop uses, with a single big *Soundgoodize* knob (depth) and an output trim. Three bands (split at 88 Hz and 2.5 kHz), each squashed from above and dragged up from below. At 0 % it passes the audio through untouched.
- Compressor: a soft-knee compressor (threshold, ratio, attack, release, knee, makeup, dry/wet), keyed by its own input or, with a sidechain, by another track's signal (for ducking). Its editor shows every knob at once and, beside them, the gain reduction over the last second and meters of what keys it (the threshold marked) and of its output.
- Sampler: an instrument that plays one audio file across the keyboard (32 voices), pitched from its root key, with transpose and detune, the part of the sample that plays (start and end, looping between them while a note holds), an ADSR envelope, velocity sensitivity and volume. Drop an audio file on its waveform (from the browser or the desktop) or double-click the waveform to load one; drag the start and end markers on it. Loading a sample can be undone, and the sample is saved with the project. Files load in the background, and the engine swaps them in without stopping the audio.
- VST3 plug-ins, instruments and effects (see below).
- Track headers (on the right, like Ableton): activator (mute), solo, arm, volume, pan, input (audio, or a MIDI track's MIDI input) and monitoring, meters, rename, colour, resize.
  - Click a track's header to select it; **Ctrl-click** adds a track to the selection (or takes it out), **Shift-click** selects every track from the last one clicked. Delete deletes the selected tracks.
  - Soloing a track unsoloes the others, and unsoloing one unsoloes every track; **Ctrl-click** a solo button to solo (or unsolo) just that track, leaving the others as they are. Clicking the solo of a selected track solos (or unsoloes) all the selected tracks. **S** solos the selected tracks (unsoloing the rest), or, if they all are soloed already, unsoloes every track.
- **Group tracks**, as in Ableton: select tracks and press **Ctrl+G** (*Create › Group Tracks*, or a track header's right-click menu) to put them in a new group, where the first of them was. What is in a group goes into it, through its devices (audio effects) and mixer, and on to the master, or the group it is in (groups nest). A group has no clips; its lane shows the clips of the tracks in it, and its automation works as a track's.
  - The tracks in a group are indented under it, with a band in the group's colour.
  - **Folding**: every track header has a fold button next to its name (or *Fold Track* / *Fold Group* in its right-click menu). A track's is a triangle in a circle: folded, the track shrinks to its name row. A group's is three bars in a circle, filled while folded: folded, it shrinks to its name row too (a little taller than a track's) and hides its tracks. Folded tracks and groups don't show their automation (lanes or choosers); unfolded, it is back as it was. With several tracks or groups selected, folding one of them folds (or unfolds) them all. Folding is saved with the project, not an undo step.
  - Drag track headers to move tracks (the selected ones, if you drag one of them): between two tracks, or onto the middle of a group's header to put them in it (last). Dragged below a group's last track they leave it. *Move Out of Group* in the right-click menu does that too.
  - **Ctrl+Shift+G** (*Ungroup Tracks*) takes a group away; what was in it stays, where it was. Deleting a group deletes what is in it.
  - Soloing a group solos what is in it; soloing a track keeps the groups it is in heard (but not the other tracks in them). Muting a group silences what is in it.
  - Delay compensation lines up the tracks going into each group, and the groups going into the master: a latent plug-in anywhere (on a track deep in a group, or on a group) delays only what it has to.
- **Return tracks and sends**, as in Ableton: **Ctrl+Alt+T** (*Create › Insert Return Track*, or a header's right-click menu) adds a return track, named by letter (A, B, ...). Returns show apart, in compact rows above the master, each with its activator, solo, volume, pan, meter and lane (for its automation); click one to see its devices (audio effects: a reverb, a delay) in the device view.
  - While there are returns, every header (tracks', groups' and returns') has a **send knob** for each return, by its letter, below volume and pan: turn it up to send that much of the track to the return. A send taps the track after its fader and pan; right-click the knob for *Pre-Fader* (the track's fader doesn't change what it sends; its letter shows in orange), or to remove the send. A muted track sends nothing, before the fader or after.
  - A return can send on into another return, but not into one that feeds it (or itself): those knobs are greyed out.
  - Send levels are automated like volume (*Mixer › Send A* in a lane's choosers); turning a send knob by hand overrides its automation, as for volume.
  - Deleting a return deletes the sends into it and their automation, in one undo step.
  - Solo: soloing a track keeps everything it goes into heard, its returns too (you hear its reverb, not the others'); soloing a return keeps what sends to it sending (but not playing on its own).
  - Delay compensation works per send: a track sending to two returns of different latency is delayed differently on each, and everything lines up again at the master.
- **Sidechains**: a device with a sidechain input (a plug-in's aux input: a compressor, a gate, a vocoder) has a sidechain button (an arrow into a bar) in its title bar, lit while it has a sidechain. Click it to choose the track, group or return whose signal goes into it, and where that is taken, as in Ableton: *Pre FX* (the source's own audio, before its devices; a MIDI track's instrument's, before its effects), *After* one of its devices, *Post FX* (after all of them, before the fader) or *Post Mixer* (after its fader and pan, as it is heard). Sources that would close a cycle (the group the device's track is in, a return it sends to) are greyed out; the master's devices can take any track's.
  - The sidechain lines up with the device's own signal sample for sample: whichever comes first waits for the other, so latent plug-ins on the source (before the tap) or on the device's track (before the device) change nothing.
  - It isn't heard on its own: soloing the source doesn't bring in the device's track, and while the device's track is heard (soloed, or a track going into it is) the source keeps keying it (without being heard). Muting the source silences its sidechain only after the fader, so a muted "ghost" kick can still duck the bass from before its fader.
  - Deleting the source turns the sidechain off (in the same undo step), as does moving tracks into groups, or the device to another track, where it would close a cycle; otherwise a device moved to another track keeps its sidechain. Should the device a sidechain is taken after leave its track, it is taken Post FX until the device comes back.
- **Racks** (device groups), as Ableton's Audio Effect and Instrument Racks: select devices in the device view and press **Ctrl+G** (or *Group* in a device's right-click menu) to put them in a rack, in one chain, where the first of them was; **Ctrl+Shift+G** (*Ungroup*) takes a rack away, its chains' devices taking its place. A rack's chains each process its input, side by side, and it puts out their sum (an empty rack passes its input on).
  - A rack shows its chains, a row each with its activator, name, solo (only the soloed chains of a rack are heard), volume, pan and meter; **+ Chain** adds one, and a row's right-click menu renames, duplicates or deletes it. Click a chain to show its devices beside the rack, in a bracket: drop devices there (or onto a chain's row), drag them in and out, select and delete them as on the track's own chain. Racks nest, up to 8 deep.
  - A latent device in one chain doesn't smear the others: they are delayed to line up with the slowest, and the rack's latency is compensated like any device's. Every chain hears the track's notes, so a rack of instruments layers them (an *Instrument Rack*).
  - Devices in racks are automated like any of the track's devices (they keep their automation when grouped, ungrouped or moved), and so are the chains' volume and pan (*Chain Volume*, *Chain Pan* under the rack in a lane's device chooser), in time with the latency before them. A sidechain into a device in a rack lines up with the signal there.
  - **Macros**: a rack has eight; right-click a parameter of a device in the rack and choose *Map to Macro* to have one of them move it across its range (a macro can move several parameters; right-click a macro to see or unmap them). Turning a macro sets what it moves, as one undo step.
  - A rack's save button saves it as a **preset** (`.gilpreset`: its chains, the devices in them with plug-ins' states, its macros); right-click beside the devices to load one. A loaded preset is a new rack (loaded twice, two racks), without sidechains.
- Master track (volume, pan, and audio effects: click its header to show its chain in the device view), metronome, loop brace, follow mode, CPU meter.
- **Audio recording** (Arrangement recording, as in Ableton), from an ASIO driver's inputs.
  - An audio track's header has an **arm** button (●; arming one disarms the others unless Ctrl is held, and clicking a selected track's arms all the selected tracks), its **input** (one of the driver's inputs, or a pair of them for stereo) and its **monitoring**: *In* (it always hears its input, not its clips), *Auto* (it hears its input while armed, unless playing back without recording) or *Off*. A monitored track's input goes through its devices and mixer like any audio; it isn't delayed to line up with latent plug-ins on other tracks, so you hear yourself with only the latency of the track's own devices. Choosing an input the driver hasn't open opens it (and keeps it open next time).
  - **Record** (the button next to Stop, or **F9**) records every armed track that has an input: from the insert marker, after the **count-in** chosen next to it (none, 1, 2 or 4 bars: the metronome counts in even if it is off) if stopped, or from the playhead while playing (punch in). Record again to stop recording and keep playing; Stop or Space stop both. While recording the loop doesn't wrap.
  - A take grows on its track as it records, with its waveform. When recording stops, the takes become clips in one undo step, replacing what was under them (overdub); undo takes them away again. They are WAV files (32-bit float) in the project's `Recordings` folder (for a project not saved yet, `Music\SUBstation\Recordings`).
  - Takes land where they were played: the engine moves them back by the driver's input and output latency and the delay compensation's lag, so what you played along to lines up.
  - **Resampling**: an audio track's input menu also lists *Resampling* (the master's output) and every other track, group and return: the track then takes that one's output, after its fader (and pan), as its input. Recording records it (in stereo) and the take lands exactly where it was heard, so it lines up with what it was recorded from; latent plug-ins on the source (or the master) change nothing. Monitored, the track hears its source instead of its clips, as it would a microphone (not delayed to line up with other tracks); never the master's, which would feed back into it. A source the track feeds (its own group, a return it sends to) is greyed out; moving a track into the group it takes its input from, or deleting its source, leaves it with no input (in the same undo step). Resampling needs no audio inputs, so it records with WASAPI devices too.
  - Changing the audio device (or its sample rate, or a reset by the driver) ends a recording; what was recorded so far is kept. So does moving the playhead.
- **MIDI input** from controllers and keyboards (WinMM), played live and recorded.
  - Every MIDI input connected is used unless turned off in *Options › Preferences › MIDI* (*Refresh* there finds inputs plugged in since).
  - A MIDI track's header has an arm button, its **MIDI input** (*All Ins*, one input, or *No Input*; and every channel or one of the 16) and monitoring: *In* (it plays its input, not its clips), *Auto* (it plays its input while armed, its clips as well) or *Off*. Notes played reach the track's instrument one audio buffer after they arrive, at the same place in the buffer, so timing doesn't jitter. Keys held when the transport stops, or when a track stops hearing its input (disarmed, another input), are released.
  - Recording records armed MIDI tracks along with audio ones: the notes show on the take as they are played, and become a MIDI clip of what was recorded (replacing what was under it), in the same undo step as the audio takes. Notes land where they were heard against the timeline: moved back by the audio output latency, the delay compensation's lag and the buffer MIDI waits for. A key still held when recording stops ends with the take.
  - *Edit › Record Quantization* (none, 1/4 to 1/32, triplets) puts recorded notes' starts on that grid (lengths as played).
  - **Computer MIDI keyboard** (**M**, *Options › Computer MIDI Keyboard*, or ⌨ in the control bar): the letter keys play notes as one more MIDI input, *Computer Keyboard* (tracks on *All Ins* hear it too; it plays and records like any input). A S D F G H J K L ; ' are the white keys from C3, W E T Y U O P the black keys between them; Z and X move an octave down and up. While it is on those keys don't trigger the window's shortcuts (S, A, Z), but text fields still get them; held notes are released when it is turned off or the window loses the keyboard.
- An oscilloscope next to the transport controls shows the master output as it plays (as in FL Studio). It starts each picture at a rising zero crossing, so steady tones stand still.
- **Automation**, as in Ableton's arrangement, of every device parameter (built-in devices and plug-ins alike) and of each track's and the master's volume and pan.
  - **A** shows (or hides) the automation of every track and the master. A track's automation shows in its own lane, over its clips (the clips' title bar still moves and selects them), with the parameter chosen in its header: a device chooser (*Mixer* or one of its devices) and a parameter chooser; automated ones are marked with a red dot. **+** shows another parameter in a lane below the track (**−** removes it). Right-click a track header (or the master's) to show or hide its automation.
  - Clicking a parameter (a knob, list or name in the device view, or a control in a plug-in's own editor) or changing one by hand (also a track's volume or pan, or the master's) shows its track's automation with that parameter.
  - In a lane: **click on the envelope's line** to add a breakpoint on it (on the grid when snapping; Alt-click where there is no segment to bend: off the grid), or press there and drag to place it. Over the line the cursor shows a small plus, and a faint breakpoint shows where the click would put it; a click off the line adds nothing. **Click a breakpoint** to delete it. **Drag** breakpoints to move them in time and value (Shift-click or Ctrl-click selects several, which move together; Alt drags off the grid, Shift finer; one can't pass its neighbours, several override the breakpoints they land on; the value shows as you drag). Near the line between two breakpoints (but not on it) the segment lights up: **drag** it to move both breakpoints, in time and value (a click selects them). A step (two breakpoints at the same time) is a segment too: drag it sideways to move the step. **Alt-drag** between two breakpoints to bend that segment (up bulges it upward). Select breakpoints and press **Delete** to delete them. Drag across lanes (not on a breakpoint) to select a time range on them: **Delete** clears their automation there (the envelope outside stays as it was), **Ctrl+D** duplicates it after the range, and **dragging inside the range** (off its breakpoints and line, which work as anywhere else) moves the automation in it (on all its lanes) up, down, left or right, with breakpoints added at its edges so the envelope outside stays as it was (moved in time, it replaces what is where it lands). Right-click a lane for more (delete the envelope, remove the lane, hide the automation). All of it is undoable (one step per drag).
  - **Lock Envelopes** (the padlock next to Record, or *Options › Lock Envelopes*; saved with the project): unlocked, as by default, automation moves with clips. Moving a clip (or a time selection) moves the automation under it, replacing what was where it lands; copying clips (Ctrl-drag, Ctrl+D) copies it. Only envelopes with breakpoints under the clips move. Moved to another track, a clip takes its track's volume and pan automation along; a device's automation stays on its own track. Locked, automation stays where it is.
  - Before its first breakpoint an envelope holds the first one's value, after its last the last one's; two breakpoints at the same time make a step. Parameters that choose between values (lists, steps) move in steps, and their lanes show them so.
  - Automated controls follow their automation as it plays (and where the playhead is placed when stopped), in the device view, the track headers and plug-ins' own editors; they are marked with a red dot. Changing an automated parameter by hand **overrides** its automation, as in Ableton: its envelope turns grey and the parameter stays where you put it until **Re-Enable Automation** (the lit button next to Record, *Edit › Re-Enable Automation*, or a lane's or parameter's right-click menu). Right-click a parameter in the device view to show its automation, delete it or re-enable it.
  - The engine plays envelopes sample-accurately: volume and pan sample by sample; device parameters at each breakpoint and every 64 samples along a slope (plug-ins get them as sample-accurate VST3 parameter changes; built-in devices process their blocks in pieces where values change). Automation is delayed along with a track's audio by the plug-in latency before it (delay compensation), and it is in exports.
- Browser: categories (All, Samples, Built-in, Plug-ins), user "Places", instant search, click-to-preview, drag-and-drop or double-click to add clips. Lists sort by *Rank* (what you add most, and most recently, first; counts kept in `%LOCALAPPDATA%\SUBstation\library.json`) or *Name*.
  - Instruments (built-in or plug-in) go on a MIDI track, replacing its instrument. With no MIDI track selected, double-clicking one or dropping it below the tracks makes one.
  - The files under the places are indexed in the background and the index is kept (`%LOCALAPPDATA%\SUBstation\browser-index.bin`), so the next start shows it at once and only looks again at folders that changed. Files added, removed or renamed in a place show up while the program runs. Results show while a first scan is still going. Right-click › *Rescan* reads every folder again (for drives that don't report changes).
  - Searching 200 000 files takes about 10 ms and never holds up the window or the audio; see [benchmarks/README.md](benchmarks/README.md).
- Device view with the built-in Synth and Sampler instruments, Utility device (gain/pan/width), Over The Top and Compressor, and plug-ins, all through the same `Processor` interface. Parameters that choose between named values get a list; frequency and time knobs turn logarithmically. Drag effects (or right-click one) to move them along the chain, or drag them onto another track in the arrangement to move them there, with their automation; plug-ins move as they are, without loading again. Each device has a title bar, as in Ableton (lighter while the device is selected), with its on/off switch, name, the plug-in window button, the sidechain button (devices with a sidechain input), the parameter page arrows and a save button (a rack's saves it as a preset; the others' aren't wired up yet). Delete a device with the Delete key or its right-click menu.
- Undo/redo for all edits, `.gilproj` projects (JSON), WAV export (16/24/32-bit float).
- Audio devices: ASIO drivers (see below), or WASAPI shared or exclusive, with the sample rate and buffer size.

## ASIO

- **Choosing a driver.** *Options › Preferences*: set *Driver Type* to ASIO and pick the driver. Changes there apply at once, because what a driver offers is only known while it runs: the *Sample Rate* and *Buffer Size* lists show what it supports, and *Output Channels* which pair of its outputs the master plays on (1/2, 3/4, ...; an odd last one plays in mono). A driver that sets its buffer size itself only offers that one.
  - **Hardware Setup** opens the driver's own settings. When they change there (or in the driver's own app, or its clock follows another device), the driver asks for a reset and SUBstation opens it again with the new buffer size and sample rate, keeping the outputs.
  - The status line shows the input and output latency the driver reports.
- **Next time** the program opens the same driver. If it can't run as saved (another clock, fewer outputs), it opens with the driver's own settings; if the driver is gone, with the system's default output (WASAPI).
- Every sample format drivers use is converted (16, 24 and 32-bit integers in any alignment, 32 and 64-bit floats, either byte order); DSD drivers aren't supported. Integer formats are clipped at full scale.
- Only one ASIO driver can be open in a program (the ASIO callbacks have no way to tell drivers apart).
- **Inputs**: a track's input opens the driver's inputs it needs (`open_device(..., input_channels=[...])`); every buffer of them reaches the audio callback (with its sample position and time), where the engine meters them (`take_input_meters()`), monitors and records them.

## VST3 plug-ins

- **Finding them.** The browser lists the plug-ins in the standard VST3 folders (`C:\Program Files\Common Files\VST3` and `%LOCALAPPDATA%\Programs\Common\VST3`) under *Plug-ins › Instruments / Audio Effects*, with their vendor. A module with several plug-ins (an instrument and its FX version) shows each.
  - Plug-in files are read in a separate process, several per process, so a plug-in that crashes or hangs while loading is only marked as failed (hover over *Plug-ins* for the list and the reasons). The scan runs in the background at start-up and reads only new or changed files; the results are cached in `%LOCALAPPDATA%\SUBstation\vst3-cache.json`. *Options › Rescan Plug-ins* (or the button in *Preferences › Plug-ins*) reads everything again.
  - Besides the standard VST3 folders, plug-ins are looked for in folders of your own: add or remove them in *Options › Preferences › Plug-ins*. An added folder is scanned at once (only its new files), and a removed folder's plug-ins leave the browser.
- **Using them.** Drag a plug-in onto a track, into the device view, or below the tracks (an instrument makes a MIDI track); or double-click it to add it to the selected track. Instruments play the MIDI clips (and the piano roll's notes) sample-accurately.
- **The device view** shows a plug-in's parameters eight at a time (‹ › pages), with the plug-in's own text for their values (`-3.0 dB`, `Bell`); lists get a menu. Read-only and hidden parameters are left out, and the device's on/off switch stands in for the plug-in's own bypass.
  - **Edit** opens the plug-in's editor in a window of its own, which floats above the main window. It follows the plug-in's resize requests, lets you resize it within the plug-in's limits (if it can resize), and tells the plug-in when it moves to a screen with another scale.
  - Turning knobs in the plug-in's editor is recorded for undo like any other edit: one step per knob drag. A plug-in that changes in a way no parameter shows (a preset picked in its editor) marks the project as changed.
  - Right-click: *Load Preset…* / *Save Preset…* read and write standard `.vstpreset` files (loading one is undoable), and *Show Editor*.
- **Projects** save each plug-in's complete state (as a `.vstpreset`, base64) and which plug-in it is. A plug-in that moved is found again by its class id; a missing one keeps its place and settings in the project, shows what's wrong in the device view, and loads when it's back (after a rescan).
- **Latency** that plug-ins report (look-ahead limiters, linear-phase EQs) is compensated: the other tracks, and the metronome, are delayed to line up, and exports come out aligned. The device's tooltip shows the latency.
- **Transport**: plug-ins get the tempo, time signature, position in samples and beats, bar position, playing state and the loop, and blocks are split where the loop wraps, so tempo-synced plug-ins stay in time.
- Mono-only plug-ins get the track mixed to mono, and their output goes to both sides. A plug-in's first aux input is its sidechain (stereo if it takes it; see *Sidechains* above). Other extra buses get silent inputs, and their extra outputs are not used (no multi-output instruments yet). MIDI controllers reach the parameters the plug-in maps them to.

## Setup

Requirements: Windows 10/11, Python 3.12, and Visual Studio 2022 or newer with
the *Desktop development with C++* workload. CMake and Ninja are installed into
the venv from PyPI.

Always activate the venv before installing anything:

```powershell
Set-ExecutionPolicy -Scope Process Bypass    # only if activation scripts are blocked
.\venv\Scripts\Activate.ps1
python -m pip install scikit-build-core nanobind cmake ninja pytest ruff PySide6 numpy
python -m pip install --no-build-isolation -e .    # compiles substation._engine
```

Re-run the last command after changing any C++ code (the build is incremental,
in `build/`; it makes `substation._engine` and `substation._browser`). Python changes
need no reinstall. The app (and the tests) won't start with an engine built from
older code than the Python side: they say so, and to re-run it. When Python code
comes to need a change in `engine/src/bindings.cpp`, bump `API_VERSION` there and
`ENGINE_API` in `src/substation/__init__.py` together. If the newest Visual Studio
causes trouble, pick another one with `$env:CMAKE_GENERATOR="Visual Studio 17 2022"`.

The parts of the VST 3 SDK the engine uses are included (`engine/third_party/vst3sdk`,
MIT-licensed since SDK 3.8), so nothing else needs installing. The build also makes
the small VST3 plug-ins and the fake ASIO driver the tests use (`tests/vst3_plugins`,
`tests/asio_driver`), installed next to the engine; turn that off with
`-C cmake.define.SUBSTATION_TEST_PLUGINS=OFF`.

**ASIO** needs Steinberg's ASIO SDK, which isn't in the repository (its licence doesn't
allow passing it on). Download it from <https://www.steinberg.net/asiosdk> and unzip it
into the project folder as it comes (`SUBstation\asiosdk_2.3.3_2019-06-14\common\...`);
`.gitignore` keeps `asiosdk*` folders out of git. The build finds it in any folder of the
project folder (or one level deeper), or an `asio*` folder beside the project or in the
root of the drive; to keep it elsewhere, set `SUBSTATION_ASIO_SDK` to its folder (the one
containing `common`) before building. Only its headers are used. The build prints
`ASIO SDK: <folder>` when it found it; without it, it warns and builds the engine with
WASAPI only (the preferences then show ASIO greyed out). Re-run the install command after
adding the SDK.

## Run

```powershell
.\venv\Scripts\Activate.ps1
python -m substation                  # or: python -m substation path\to\song.gilproj
```

The first launch uses the system default output device; change it in
*Options → Preferences*. The browser starts with your Music folder as a Place.
Add more with *Add Folder…*.

## Tests

```powershell
.\venv\Scripts\Activate.ps1
python -m pytest
```

- ASIO tests use a fake ASIO driver built with the engine (`tests/asio_driver`): a real in-process COM object, loaded from its DLL rather than registered (`SUBSTATION_ASIO_DRIVERS`), so they never see the installed drivers and need no sound card. Its hooks (called through ctypes) let a test drive it a buffer at a time, feed its inputs, read back the bytes the engine wrote to its outputs, and send the engine what drivers send (reset requests, new latencies, a new sample rate). They check every sample format against numpy's decoding, output channels and mono, inputs, rates and buffer sizes, resets and the control panel, errors, playing on the driver's own thread, and the preferences and start-up in the application. They are skipped when the engine was built without the ASIO SDK.
- Recording tests use the same driver; it can also loop an output back into an input, as a cable would, delayed by the latencies it reports. A take of what the engine played then lines up with the timeline sample for sample, with and without latent plug-ins (on a track or the master). Others check input monitoring (its modes, and that a monitored track isn't delayed by another track's latency), stereo and mono takes and their live peaks, punching in, the count-in, what ends a recording (a locate, a device change), and in the application: the header controls, the record button, takes becoming clips in one undo step (and undo removing them), Space and device changes during recording.
- Resampling tests record a track's, a group's or the master's output and hold the take, sample for sample, to an offline render of that source, with latent plug-ins on the source, in the group and on the master, and a slower track the source is delayed to line up with; device and resampled takes recorded together are each placed by their own input. Others check that monitoring a track's output isn't delayed and the master's can't be heard, solo across a monitored input, cycles refused (through groups, sends and other inputs), and a source going (also while recording). In the model: undo, sources greyed out or refused, inputs dropped by regrouping and deleting (one undo step), saving, and the engine taking it all; in the window: the input menu and a take recorded from a track and from the master.
- MIDI input tests send messages (`send_midi_input()`) stamped against the audio clock the engine reports after a buffer, so where each plays is known to the sample: live notes reach a test instrument at their offsets in a block, tracks hear the inputs and channels they choose, held keys are released when the transport stops or a track stops hearing them, and recorded notes land exactly on the beats a player heard them on (with latent plug-ins on a track or the master). In the application: the MIDI header controls, a MIDI take with its live notes and record quantization, and the MIDI preferences.
- Engine tests render offline, so no audio device is needed. They check sample-exact clip placement, gain/pan/mute/solo, looping, tempo changes, the metronome, fades, the Utility device, the Compressor (its curve, attack on low notes, sidechain keying and its displays), the Sampler (pitch from key, root and tuning at any file rate, start, end and loop, velocity, its state's text and a missing file, and swapping samples while notes play, many times between blocks) and export; and automation: volume and pan (tracks and master) sample by sample, curves, device parameters (split exactly where they change, discrete ones in steps), and the normalized mapping of parameters.
- Sends and returns: sends at their levels before and after the fader, a return's effect on the sum of what is sent to it, returns into returns, mute and solo across sends, send automation in time, cycles refused across outputs and sends, and delay compensation per edge (latent returns, latent tracks sending, one track into two returns of different latency, a pre-fader tap after a latent device): clicks line up at the master. In the model: making and deleting returns (with their sends, one undo step), saving, and the engine hearing it all; in the window: Ctrl+Alt+T, the send knobs, pre-fader, greyed-out cycles and send automation.
- Sidechain tests key SUB Test Sidechain (a test plug-in whose output is its input plus its sidechain) with a track's click: it hears the source sample for sample, after its fader, before it, before its devices or after one of them, with latent plug-ins before and after the tap and on the device's track before and after the device (the sidechain or the track's own signal waits for the other), and taps before a device that waits for its own sidechain; into groups' and the master's devices too, and live with workers. A device after one that waits for its sidechain keeps its automation in time. Others check cycles refused (through outputs, sends, inputs and moving the device), the source going, mute and solo, and that a missing sidechain reaches the plug-in flagged as silence. In the model: undo, sources greyed out or refused, sidechains dropped by deleting, ungrouping, regrouping and moving devices (one undo step), saving, and the engine taking it all; in the window: the sidechain button and its menu.
- Rack tests: chains summing, their faders, mute and solo (also metered live), a rack switched off or empty passing its input on; a latent device in one chain (and racks in racks) not smearing the others, with tracks beside lined up; automation of a nested device and of a chain's fader in time behind latent devices; an instrument rack of two synths rendering as two tracks of one each; sidechains into devices in racks lining up sample for sample; devices and racks moving in and out of racks and between tracks (keeping their state), removals, cycles and nesting refused; and renders with racks bit-identical with and without workers. In the model: grouping and ungrouping, chains and their mixers, moves, nesting limits, instrument racks, macros (one undo step, mappings following their devices), sidechains in racks as routing edges, saving and presets (fresh ids, loading twice); through the bridge: the engine keeping each device's processor as racks are made, undone, moved and deleted, chain automation and overrides, macros moving plug-in parameters, and a saved rack rendering as the original; in the window: Ctrl+G and Ctrl+Shift+G, chains shown and dropped into, chain mixers and macros.
- Parallel tests render with and without the audio thread's workers: random routing graphs (nested groups, returns and sends before and after the fader, tracks taking their input from others, sidechains, latent plug-ins, solo, mute, automation, looping), many tracks over many runs, and stateful synths and plug-ins moving between threads all render the same bit for bit; the MIDI input, recording and resampling tests run again live with workers (held, preview and recorded notes). Tracks are timed, and the tracks on the longest paths start first (ranks checked on a hand-made graph). `python -m benchmarks.parallel_render_bench` compares render times on 1..N threads, and with `--heavy N --compare-ordering` the gain from starting heavy tracks first.
- Warp tests check that warped clips land on their beats at any tempo, keep their pitch, start sample-aligned (also after a locate), transpose to the right frequency, and that Re-Pitch filters rather than aliases.
- MIDI engine tests check that notes start on their sample and follow the tempo, that the Synth plays the right pitch and level, and that loop wraps and offline renders leave no hanging notes.
- Model tests cover overlap resolution, trims, splits (audio and MIDI), note editing, undo/redo and save/load; and automation: envelope maths (curves, exact splits, range delete and duplicate), the parameter mappings against the engine's, undoable edits, what happens to a deleted device's automation, the lanes shown, saving, and automation moving (or copied) with clips unless locked.
- Browser tests hold the native backend to the Python code it replaced (`tests/browser_reference.py`, kept as it was): the same files from a folder tree (hidden names, depth and file limits, junctions and symbolic links, overlapping and missing places) in the same order, and the same results for random queries, sorts, filters and use counts, item for item. Python's own text rules (`str.lower`, `casefold`, `split`, the regex word starts) are checked for every Unicode character. Others check the saved index (checked by folder times, rescan, damaged files), changes seen while running, that only the latest search's results are handed out, paging, and that waiting releases the GIL.
- The UI tests drive the real main window offscreen: mouse drags, drops, header controls, dialogs, the piano roll, and devices' own editors (the Compressor's graph; the Sampler's loading, undo, playhead, markers, drop and saving); and automation lanes: A, parameters showing their lanes, clicking on the line, dragging and bending breakpoints, deleting, time ranges, overriding and re-enabling, controls following automation, the master's lane, lanes below tracks, and saving.
- VST3 tests use three test plug-ins built with the engine: an instrument with a separate controller (it reports the transport it gets back as parameters), a single-component effect with adjustable latency and a Win32 editor, and a mono effect without a controller. They check scanning (including a plug-in that crashes or hangs while loading), sample-exact notes, parameters, the transport and loop splitting, latency compensation, mono buses, state and presets, editor windows (resizing, closing, edits reported for undo), and the device view, browser, undo and projects in the application. Editor tests briefly show real windows. The tests see only these plug-ins, never the installed ones.

## Keyboard shortcuts

| Action | Keys |
|---|---|
| Play / stop (returns to the start marker) | Space |
| Stop; press again to return to the start | Stop button |
| Go to start | Home |
| Insert audio track / MIDI track | Ctrl+T / Ctrl+Shift+T |
| Insert MIDI clip (on the selected MIDI track, or over a time selection) | Ctrl+Shift+D (or Ctrl+Shift+M) |
| Duplicate / split at insert marker / delete (clips, or automation in a lane's time selection) | Ctrl+D / Ctrl+E / Delete |
| Consolidate the selected MIDI clips on each track into one (also in the clip's right-click menu) | Ctrl+J |
| Show / hide automation (every track and the master) | A |
| Add an automation breakpoint / delete one | click on the envelope's line / click the breakpoint |
| Bend an automation segment | Alt-drag between two breakpoints |
| Select all clips | Ctrl+A |
| Undo / redo | Ctrl+Z / Ctrl+Y |
| Loop on/off | Ctrl+L |
| Zoom in / out / to arrangement | + / − / Z |
| Zoom around the mouse | Ctrl+wheel (or drag vertically in the ruler) |
| Scroll horizontally | Shift+wheel (or drag horizontally in the ruler) |
| Scroll in any direction | Ctrl+Alt drag |
| Resize the track under the mouse (piano roll: the keys' rows) | Alt+wheel |
| Narrow / widen grid, toggle snap | Ctrl+1 / Ctrl+2 / Ctrl+4 |
| Bypass snapping while dragging | hold Alt |
| Copy clips while dragging | hold Ctrl |
| Toggle browser / device view | Ctrl+Alt+B / Ctrl+Alt+L |
| Search everything in the browser ("All"); Enter selects the first result, Enter again adds it | Ctrl+F |
| Export audio | Ctrl+Shift+R |
| Show a plug-in's editor | double-click its device in the device view |
| Close the plug-in editor in front | Ctrl+W |

The Ctrl/Alt shortcuts also work while a plug-in's editor window has the focus, except Ctrl+A/C/V/X/Z/Y, which the plug-in keeps for its own text fields. Keys without Ctrl or Alt (Space, Delete, ...) go to the plug-in.

In the piano roll, Delete, Ctrl+A and Ctrl+D act on notes, Ctrl+U quantizes them, and arrow keys move them.

## Architecture

```
src/substation/                 Python: UI, model, undo, file I/O
  model/        project.py (Project/Track/Clip/MidiClip/Note + Qt signals), edits.py (pure clip maths),
                notes.py (pure note editing), automation.py (pure envelope maths, targets, mixer mappings),
                params.py (ParamSpec: any parameter described alike), editor.py (undoable operations),
                commands.py (QUndoCommands), serialization.py,
                device_state.py (built-in devices' state besides their parameters)
  audio/        engine_bridge.py: mirrors the model into the engine; async decoding;
                polls the playhead (60 Hz) and meters (30 Hz)
  ui/           main_window, transport_bar, device_panel, dialogs, clip_view,
                device_editors/ (built-in devices' own editors: one module each, registered by kind),
                arrangement/ (custom-painted ruler, lanes, headers; numpy waveform tiles;
                automation_lanes.py: drawing and editing envelopes; automation_header.py: the choosers),
                piano_roll/ (keys, ruler, note grid, velocity lane),
                browser/ (the panel, paged lists, use counts, preview; file_index.py drives the native backend)
  plugins/      scanner.py: finds VST3 plug-ins and reads them in child processes (scan_worker.py); cache
browser/src/                   C++: the browser's backend, module substation._browser (independent of the engine)
  Indexer       the saved index of the places' folders, kept up to date; publishes snapshots
  Search        filtering and ordering over snapshots, as the Python search did
  Text          Python's str.lower/casefold/split and regex classes, from UnicodeTables.inc
                (generated by browser/tools/gen_unicode_tables.py)
  Browser       the two threads, and the event that tells the UI there is something to take
  Platform      listing folders, folder times, watching for changes, background priority (Win32)
benchmarks/                    the browser's and the renderer's benchmarks and their results (not run by pytest)
engine/src/                    C++: everything on the audio thread, and plug-in hosting
  Engine        public API; edit model; builds and publishes render snapshots
  Renderer      mixing: strips (inserts -> fader/pan; racks: chains side by side, summed), edges (outputs, sends, inputs, sidechains; delay compensation), master strip; loop;
                metronome;
                count-in; preview; automation; input monitoring (audio and MIDI); hands input to the recorder
  Routing.h     the routing graph: edges, topological order, cycle check, delay compensation per edge (and at sidechained devices, and between a rack's chains)
  Rack.h        a rack's place in a chain (its chains live in the engine's chains and the snapshot)
  Scheduler     runs the routing graph's tracks on the audio thread and a pool of workers
  Recorder      recording: lock-free sample rings, live peaks, the disk-writer thread; MIDI takes
  MidiInput     MIDI input devices (WinMM), stamping against the audio clock
  Automation.h  envelopes: breakpoints, curves, evaluation, the mixer's lane mappings
  Warp          stretch voices (time stretch / pitch shift) and the Re-Pitch resampler
  AudioSource   decoding (WAV/FLAC/MP3) at the engine rate + peak mipmaps
  AudioDevice   devices of any driver type: DeviceConfig in, planar duplex AudioIO callbacks out
  backends/     WasapiBackend (miniaudio), AsioBackend (IASIO; only with the ASIO SDK),
                AsioSupport.h (ASIO sample formats and buffer sizes, no SDK needed)
  Processor.h   insert-device interface (built-ins and plug-ins); ParamInfo (normalized mapping); automation inbox
  builtin/      built-in devices: BuiltinProcessor (their parameters and automation), BuiltinRegistry
                (devices register themselves; created by id), devices/ (Synth, Sampler, Utility, Ott, Compressor: one .cpp each);
                devices may publish displays (meters, curves) for their own editors
  plugins/      PluginFormat.h (formats), Vst3Format (host context, modules, scanning),
                Vst3Processor (a VST3 plug-in as a Processor), EditorWindow (plug-in editors),
                Vst3Support.h (allocation-free event and parameter lists for the audio thread)
  bindings.cpp  nanobind module substation._engine
engine/third_party/            miniaudio, Signalsmith Stretch, the VST 3 SDK (subset); all MIT
tests/vst3_plugins/            the VST3 plug-ins the tests use
tests/asio_driver/             the fake ASIO driver the tests use
```

**Audio devices**
- Each driver type is an `AudioBackend` (engine/src/AudioDevice.h); `AudioDevice` opens a `DeviceConfig` (driver type, device, rate, buffer size, the device's input and output channels) with whichever one it names.
- Devices run duplex: each callback gets the open inputs and fills the open outputs, as separate float buffers of one length, with the device's sample position and the steady-clock time the callback began. Recording and input monitoring use them; MIDI input is timestamped against the same clock. WASAPI opens outputs only so far.

**Audio threads**
- A chunk renders in three parts (engine/src/Renderer.h). A serial prologue works out everything tracks share: the timeline's stretches, solo, the recording's input, each track's note events (clips, preview, MIDI input) and the stretch voices its clips play through, into the track's own `TrackBuffers`. Then the routing graph: the `Scheduler` (engine/src/Scheduler.h) runs each track once everything going into it is done, on the audio thread or one of its workers (dependency counters and a lock-free queue in the snapshot's `TaskGraph`; workers join MMCSS "Pro Audio", spin briefly between chunks, then sleep). A bus (a group, a return) sums its incoming edges in snapshot order, each at its level, and the master sums its incoming edges in order after the graph, so the result never depends on which thread finished first: renders are bit-identical on any number of threads. Each track's render is timed, and the tracks with the most work hanging off them (their own and that of everything they go into, along the heaviest path) start first, so a heavy track never starts last. Small chunks with little work render serially.
- Preferences › Audio › Audio Threads sets how many threads render (one per core but one by default; 1 = no workers). Offline renders and exports use the same threads.

**Recording**
- A track's input (`InputEdge` in the snapshot) is a mono channel or a stereo pair of the device's inputs, another track's output (resampling), or the master's. Another track's output comes in on an input edge (`EdgeRender::Kind::Input`), tapped after the source's fader and before any delay compensation: it orders the graph (the source renders first) and closes cycles like any edge, but isn't summed or aligned (`RouteEdge::sums`); solo goes across it only while the track monitors it. A monitored track's strip takes its input instead of its clips; its delay compensation is left out (`Renderer::compensationFor`). The master renders after every track, so it is no edge and can't be monitored.
- While recording, the renderer copies the recorded tracks' input into lock-free rings (`engine/src/Recorder.h`), with the timeline position where it was taken: the device's in the chunk's prologue, the tracks' outputs (from their buffers) and the master's (after its fader, before the metronome) in the epilogue, over the same stretches. A disk-writer thread empties them into WAV files; the audio thread never touches a file. If the writer falls behind and a ring fills, the input is dropped but its place is kept (the writer puts silence there) and the take reports how much was lost.
- Each take's start is moved back by its own placement: device input by the output lag (delay compensation and the master's devices) and the driver's output and input latency, as a sample comes back through the input that much after the timeline was heard; a track's output by how late it leaves the track (its edges' arrival: what feeds it and its devices); the master's by the output lag. The live waveform's peaks come through their own queue, not from the file.
- Anything that changes the device, and offline renders, end a recording first; the takes so far are kept.

**MIDI input**
- `MidiInputDevices` (engine/src/MidiInput.h) opens WinMM inputs on the main thread. Each message is stamped with the host clock as it arrives and turned, right there, into a sample on the audio device's clock: from the last callback's (host time, sample position) pair (`AudioClock`, a seqlock the audio thread writes), plus one device buffer. A message that arrives during one buffer plays at the same distance into the next, so the delay is constant. The producers (driver threads, `send_midi_input()`) share a mutex on their side of a single-consumer queue; the audio thread never waits.
- The renderer takes the messages due in each chunk (a note-off never before its note-on) and routes them to the tracks whose `MidiInputRoute` (an input or all, a channel or all) accepts them, beside the clips' notes and preview notes. It remembers the live notes it started, as it does the clips' notes, and releases them when the transport stops, the track stops hearing its input, or the track goes.
- A MIDI track being recorded gets a `MidiRecordingTake`: the audio thread sends its note-ons and note-offs, at their timeline position, through a queue; the edit side pairs them into notes. They are moved back by the output lag, the device's output latency and MIDI's buffer of delay.
- A driver's events (the device went away, a reset request, new latencies) are flags the audio side sets; the UI takes them from `Engine::takeDeviceEvent()` and answers a reset with `reopenDevice()`, which asks the driver for the buffer size and sample rate it now has before closing it.
- ASIO drivers are COM objects created on the UI thread, which has to be a single-threaded COM apartment for that: the engine makes its thread one before miniaudio would make it multithreaded (in the application Qt already has). A thread that is multithreaded all the same loads the driver from its DLL. The audio thread calls nothing on the driver but `outputReady()`.
- A driver's control panel may run a message loop; the device can't be closed or reset until it returns.

**Real-time safety**
- The audio callback never locks, allocates, frees or touches Python, so the GIL cannot cause dropouts. (That is the host's part; what a plug-in does in its `process()` is up to the plug-in.)
- Edits build an immutable `RenderSnapshot`, in which positions are already converted to samples. It is published with one atomic pointer swap.
- Retired snapshots are freed on the UI thread once the audio thread's epoch counter shows they are no longer in use.
- Continuous controls (volume, pan, mute, solo, device parameters) are atomics, smoothed on the audio thread.
- The UI never waits on the audio thread: the playhead, meters and CPU load are read from atomics.

**MIDI**
- The UI flattens a MIDI track's clips into the notes they play, in beats (`set_track_notes`). The snapshot converts them to samples at the current tempo.
- Each block, the renderer turns notes starting in it into note-on events for the track's devices, and remembers when each ends. Note-offs come from that record, not from the snapshot, so a note edited or deleted while it sounds still ends.
- Stopping, locating and wrapping around the loop release every sounding note. A tempo change moves the recorded note ends along with the playhead.
- Notes the piano roll plays go through a lock-free queue to the next audio block, straight to the track's instrument.
- Offline renders share devices with live playback, so the engine resets them before and after (from the rendering thread, via a flag). A device that is switched off resets when it comes back on, so it can't keep notes whose note-offs it missed.

**Warping**
- The snapshot turns each clip's beats into samples at the current tempo. A warped clip's two ends sit on their beats, and it plays at `tempo / segment BPM` speed.
- Clips that need it are time-stretched on the audio thread by [Signalsmith Stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) (MIT, header-only; vendored with its FFT library in `engine/third_party/`). Nothing else is needed to build it.
- Stretchers keep state from block to block, so they live in *voices*. These are allocated on the edit side, pooled per warp-mode block size, and handed to the audio thread in the snapshot. The renderer binds a voice to a playing clip.
- A voice re-seeks when playback jumps (start, locate, loop, clip start). It uses the stretcher's `outputSeek` to pre-compute its latency, so the first frame is exactly aligned. A tempo change keeps the source position, so the clip carries on without a re-seek.
- Offline renders and exports use fresh voices with a fixed random seed, so they are repeatable and don't disturb live playback.
- Clips at their own tempo with no transposition skip the stretcher and play bit-exact. Re-Pitch uses windowed-sinc resampling, with the cutoff lowered when speeding up.

**Automation**
- Every automatable thing is automated the same way, in normalized values (0..1). A device parameter is a `ParamInfo`, whatever the device: `toNormalized()`/`fromNormalized()` map plain values evenly, in log(value) or in whole steps (as VST3 maps stepped parameters, so plug-in values round-trip). The UI describes every parameter, the mixer's included, as a `ParamSpec` (model/params.py) with the same mapping, and the tests hold the two to each other.
- Envelopes belong to a track or the master and are keyed by target: `mixer:volume`, `mixer:pan`, or `device:<device id>:<parameter id>`. Device ids are unique in a project, so a key finds its device wherever it sits. Deleting a device deletes its automation (in the same undo step).
- The engine gets each track's envelopes (`set_track_automation`, track 0 is the master) and converts them to samples in the snapshot, as it does notes. Before each stretch a processor processes, the renderer hands it its parameters' values over the stretch (`Processor::automate`). How it applies them is the processor's own business: `BuiltinProcessor` renders its block in pieces between changes; `Vst3Processor` passes them to the plug-in as parameter changes at their sample offsets, and sends the last values to its controller in `idle()`, so its editor follows. Faders (tracks and master share one: `Renderer::applyFader`) follow volume and pan sample by sample; where automation stops, their smoothing carries on from its last value.
- A new built-in device subclasses `BuiltinProcessor`: it lists its `ParamInfo`s and renders; automation, the device view's knobs and saving work for it at once. A new plug-in format's `Processor` applies what `automate()` hands it in `process()`. The mixer's controls are `processorId` 0 with a name (`volume`, `pan`, `send:<track id>`); more (mute) would be more names.
- A send's level is a mixer target of the sending track: `processorId` 0, `send:<engine track id>` (`send:<return id>` in the model). It maps as volume does, and is applied where the return sums the send (after the edge's delay), so its envelope is as late as the return hears its inputs. Devices inside a rack get their automation directly (the renderer calls `automate()` on the nested processor, whatever holds it), and a rack's macros are its own parameters, automated like any other.
- The bridge overrides a target changed by hand (it stops sending its envelope) until re-enabled; the model's own value of a target counts again whenever its envelope doesn't play. What shows is view state (per track and the master: shown or not, the main lane's target, the lanes below), saved with the project but not undone, like track heights.

**Plug-in hosting**
- Plug-ins are created, configured, asked about and destroyed on the main thread, as VST3 requires; only `process()` runs on the audio thread. A removed plug-in waits until no snapshot uses it and is destroyed in `Engine::idle()`, on the main thread.
- The audio thread never waits for a plug-in's main-thread work. When the main thread must take a plug-in away for a moment (restarting it for a new latency or bus layout, loading its state) it takes it with a lock-free handshake, and the audio thread passes the track's audio by it meanwhile.
- Parameter values travel to the plug-in's processor through a lock-free queue; its output parameters (meters, its own changes) come back through another and reach its controller and the UI in `Engine::idle()`, with what its editor reported (edits, restarts, a closed window). Before its state is saved, queued changes are handed to it in a `process()` call without audio, so the state includes them even when no audio device runs.
- Plug-ins may run a message loop inside a call (a licence dialog) that calls back into the engine or the UI: the engine's lock is recursive and slow plug-in calls don't hold it, and the UI ignores plug-in reports until the call returns.
- A device's plug-in lives as long as the device is in its chain: reordering or changing the chain around it never reloads it. When a plug-in device goes away (deleted, or its track) its state is kept, so undo brings it back as it was.
- Delay compensation is per routing edge (engine/src/Routing.h): every place signals are summed (a group, a return, the master) hears its incoming edges as late as the latest of them, and each other edge is delayed (after its source's fader) to line up with it. An edge that needs its own signal (a pre-fader send, or one delayed differently from its source's other edges) gets its own buffer, which its source fills. The metronome is delayed as the master's input is, plus the master's devices. Offline renders render that much ahead and drop it. A sidechain is lined up where it ends, with the signal at its device: it is delayed, or that signal is, just before the device (so everything after the device on its track is that much later, its automation too).
- **Sidechains** are edges of the routing graph from their source to the strip of the device they go into (`EdgeRender::Kind::Sidechain`), kept with the device's processor id, so they move with it. The source writes its signal at the tap into the edge's buffer as it renders (after its fader, before it, or before or after one of its devices, as `processInserts` goes along the chain), and the renderer hands that to the processor before each `process()` call (`Processor::setSidechain`); the scheduler runs the source first. The VST3 adapter fills the plug-in's first aux input bus with it, a bus it arranges and activates along with the main buses on the main thread (flagged silent when there is nothing).
- **CLAP** would be a second `PluginFormat` (engine/src/plugins/PluginFormat.h): its plug-ins become `Processor`s, its main-thread callbacks go in `Engine::idle()`, and the scanner, device view and projects work as they do for VST3.

**Browser**
- The backend (`browser/src`, module `substation._browser`) shares nothing with the engine: no locks, no threads. It has two threads of its own: the indexer, at Windows' background priority for CPU, disk and memory (it yields to playback and to decoding), and one for searches.
- The indexer keeps a tree of the folders under the places, each with its audio files, and saves it. On start it compares every folder's last-write time and lists only those that changed; while running, `ReadDirectoryChangesW` on each place says which folders to list again (all folders are compared again if too much changed at once). What it lists follows the walk the Python code did (depth first, last folder first, 16 folders deep, 300 000 files a place, no names starting with `.` or `$`, junctions but not symbolic links), so the lists are the same.
- Searches never read that tree: the indexer publishes immutable snapshots of it (also every so often during a long scan), holding the files in the list's own order and in casefold order, so ordering a search is a pass over them rather than a sort. A new search makes the running one stop; only the latest one's results are handed out.
- Neither thread calls into Python. When there are results or the index changed, the backend sets a Win32 event; `QWinEventNotifier` wakes the UI thread, which takes them. Calls from Python release the GIL.
- Results cross into Python a page at a time (256 rows, more as the list scrolls): a view lays out every row it has, so a list of every file would cost the UI thread about 200 ms each time it changed.
- Matching and ordering are defined by Python's `str.lower()`, `str.casefold()`, `str.split()` and the regex classes `\s`/`\w` (see `search.py`); the backend uses tables generated from Python itself, so they agree for every character. Item keys use Windows' own lower case, as `os.path.normcase` does.

## Not yet implemented

- MIDI overdub (recording into existing clips), recording controllers (sustain, pitch bend: they are played, not recorded) and Windows MIDI Services; recording WASAPI devices' inputs (it opens outputs only; resampling works); punching in and out at the loop, and stacking takes while looping.
- Looping MIDI clips, MIDI effects, and editing several MIDI clips in the piano roll at once.
- Recording automation (writing it while playing), and tempo changes over time. Automation lanes below a track have a fixed height.
- CLAP plug-ins; multi-output instruments (plug-ins get their main buses and a sidechain only); MIDI effect plug-ins. The Sampler plays one sample (no zones or multisamples).
- Warp markers (warping within a clip) and automatic tempo detection: a warped clip has one segment BPM, and you set it.
- Streaming long files from disk: sources are decoded into memory.
