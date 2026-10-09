# Importing Ableton Live Sets

*File › Import Ableton Live Set…* opens a Live Set (`.als`, from Live 10, 11 or 12) as a
new project. SUBstation asks about unsaved changes first, as *Open…* does. The project is
untitled but named after the set: the title bar says its name, and *Save* asks where to
save it, starting with that name (`My Song.gilproj`). The set itself is only read.

Once it is open, a message lists what didn't come across as it was (devices SUBstation
has nothing like, plug-ins that aren't installed, automation of what was left out...),
with how many of each, and the status line says how many tracks and clips came in.
Then its plug-ins load, as they do for any project.

## What comes across

**The song.** Its tempo, time signature, loop and key. A tempo or time signature that
changes during the song takes its value at the start (SUBstation has one of each): the
message says so. The key is Live 12's scale if it is a major or minor one; Live's own C
major (before a scale is chosen) leaves the key unset, so the piano roll finds the key
the MIDI is in.

**Tracks.** Audio, MIDI and group tracks, nested and folded as in Live, return tracks and
the main track, each with:

- its name and colour. Live's numbered default names become SUBstation's: "4-Serum"
  is "# Serum", so it shows its place in the arrangement as tracks come and go.
- its volume, pan, activator and solo, and its sends (before or after the fader, as
  Live's *Sends Pre*).
- where its audio goes: into its group, past it to the main track, into another
  track's *Track In*, or nowhere (*Sends Only*). A track playing into Live's audio
  device outputs plays into the main track (noted).
- an audio track's input: the audio device's channels, another track (after its
  mixer, before it, or before its effects), and *Resampling* where the track is armed
  or monitors *In*. Its monitoring (*In*, *Auto*, *Off*) too.

**MIDI clips.** Their notes, deactivated notes and clips too, from the clip's start marker
to its end. A clip that loops is written out loop after loop (SUBstation's clips don't
loop), its notes cut at the loop's end as Live cuts them.

**Audio clips.** Their files, start marker, length, gain, transpose, detune, warp mode
and fades, and whether they are deactivated.

- Files are found where the set says, else beside the set (a project folder that was
  moved or copied). Those found in neither place keep the path the set has, and the
  [File Manager](file-manager.md) lists them, to find them again.
- A clip without warping plays at its own speed, as in Live.
- A warped clip plays at the tempo its warp markers give it. SUBstation's clips have
  one tempo each, so each stretch between two markers where the tempo changes becomes a
  clip of its own (a clip warped with Live's two default markers stays one clip).
- A looping clip is written out loop after loop, like a MIDI one.
- Warp modes: *Beats* and *REX* become *Transients*, *Texture* *Smooth*, *Re-Pitch*
  *Re-Pitch*, *Complex Pro* *Formants*, and *Tones* and *Complex* *Standard*.

**Plug-ins.**

- VST3 plug-ins come with their settings (what Live saved of them). One that isn't
  installed stays in its place and loads once it is (rescan the plug-ins), as in any
  project; the message lists them.
- SUBstation hosts VST3 plug-ins only. A VST2 plug-in becomes the VST3 of it that is
  installed: with its settings if that VST3 is made to replace the VST2 (most
  plug-ins whose VST2 and VST3 share a maker's ID are), else at its default settings
  (the message lists those: set them up again). A VST2 plug-in with no VST3 installed
  is left out. Their automation comes along only with a VST3's own.

**Live's own devices.** Those SUBstation has a device like:

| Live | SUBstation |
|---|---|
| Utility | Utility (gain, balance, width; Mono as width 0) |
| EQ Eight | EQ: its eight bands, their modes, frequency, gain and Q, its output and scale; in L/R or M/S mode sixteen bands, placed on each side |
| Compressor | Compressor (threshold, ratio, attack, release, knee, makeup, dry/wet) and its sidechain |
| Delay | Delay: sync, sixteenths and times, offsets, link, feedback, freeze, filter, mode, ping pong, dry/wet |
| Simpler, and a Sampler of one sample | Sampler: its sample, mode (Classic, One-Shot, Slicing), root key, transpose, detune, start and end, volume, envelope |
| Audio Effect Rack, Instrument Rack | a rack, its chains with their devices, volume, pan and activator (not its macros) |

A **Drum Rack** becomes a group track (with the Drum Rack track's mixer, and the devices
after the rack) holding a MIDI track for each pad the arrangement plays, named after the
pad: its devices (its Simpler a Sampler), its volume, pan and activator, and the clips'
notes for that pad, at the note the pad sends on (so the Sampler plays its sample at its
own pitch). A compressor sidechained from a pad listens to that pad's track.

Other Live devices, MIDI effects and Max for Live devices are left out; the message
lists them.

**Automation** of the mixers (volume, pan, activator, sends, rack chains' volume and pan),
of devices' on/off, of plug-ins' parameters and of the parameters of Live's devices that
came across, each where its target now is (a pad's on its track).

## What doesn't

- The Session View.
- Tempo and time signature changes over the song.
- Clip envelopes, grooves, follow actions, take lanes.
- Live devices SUBstation has nothing like, MIDI effects, Max for Live devices,
  racks' macros and their mappings, Drum Racks' return chains.
- Frozen tracks come unfrozen (their devices are all there).
- Curved automation segments come straight.

---

For developers: [../app/live-import.md](../app/live-import.md).
