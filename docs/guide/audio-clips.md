# Audio clips

Double-click an audio clip to open it in the clip view, which covers the arrangement:
clip controls on the left, large waveforms on the right. Here you warp clips, change
their pitch, gain and pan. Audio added from a file is set up from its name. MIDI clips
open in the piano roll instead (see [midi.md](midi.md)).

## The clip view

- It edits one clip or many at once: select several audio clips and open them, and
  each gets its own waveform.
- Knobs move all the open clips by the same amount (transposing up 2 semitones
  transposes each clip by 2, whatever it was at); switches and the warp mode set the
  same value on all of them. Where clips differ, the value's tooltip says so.
- **Shift+Tab** shows or hides the clip view; **Esc** (or the × button) goes back to
  the arrangement.

Its controls come in three sections:

| Section | Controls |
|---|---|
| Warp | **Warp** switch, warp mode, **Seg. BPM**, :2 and ×2 |
| Pitch | **Transpose** (±48 semitones), **Detune** (±50 cents) |
| Mix | Clip **Gain** (−70 to +24 dB), clip **Pan** |

A clip's gain changes how loud it plays, and its waveform with it (in the arrangement
and in the clip view): louder clips draw taller, cut off at the lane's edges.

## Warping

**Warp** locks a clip to the beat grid. Its audio is taken to be at the *Seg. BPM*
(segment BPM) and is stretched in real time to follow the project tempo: a warped clip
plays at project tempo ÷ segment BPM speed. Turning Warp on sets Seg. BPM to the
current tempo, so nothing moves until the tempo changes.

The **:2** and **×2** buttons halve or double each clip's segment BPM (so warped clips
play twice as fast, or half as fast). **Alt**-dragging a clip's edge in the arrangement
stretches it to any length, warping it if it isn't (see
[arrangement.md](arrangement.md#clips)).

## Fades

Hold **F** in the arrangement to drag an audio clip's fades by the squares at its top
corners, and bend them by the dots on their curves; see [arrangement.md](arrangement.md#clips).
A fade is measured in the clip's audio, so it stretches with the clip. Edges without a
fade of their own get a 4 ms fade against clicks only where they cut into the file, never
at its own start or end: a drum hit plays with its full attack.

### Warp modes

| Mode | For |
|---|---|
| *Transients* | Short stretch blocks, tight attacks: drums and other percussive material |
| *Standard* | All-round: melodies, bass lines and full mixes |
| *Smooth* | Long blocks, for pads, ambience and noisy sounds; softens attacks |
| *Formants* | Standard, keeping formants (vocal character) when transposing |
| *Re-Pitch* | No stretching: speed and pitch change together, like a turntable |

Projects saved with the earlier Ableton-style names load into the equivalent mode.

## Reversing

**R** (or *Reverse* in a clip's right-click menu) reverses the selected audio clips: each
plays its stretch of audio backwards, in the same place. If the selection covers just
part of a clip, that part is split off and reversed. As in Ableton, a reversed clip
plays a reversed copy of its file (`name R.wav`), made the first time and kept in the
project's `Reversed` folder (for a project not saved yet,
`Music\SUBstation\Recordings\Reversed`); reopening the project uses it again. Reversing
it again plays its own file again. Reversing long recordings (more than half a minute of
audio) shows its progress, with **Cancel**. Reversing is one undo step; MIDI clips aren't
reversed.

## Transpose and detune

**Transpose/Detune** shift the pitch without changing the speed, warped or not (except
in Re-Pitch, where the pitch follows the speed, so they have no effect).

## Tempo and key from file names

Audio you drag in (or double-click in the browser) is set up from its name, as sample
packs name loops (`Bass_Loop_128_Am.wav`, `Keys 92bpm F# minor`, `Vox_Ebmaj_140BPM`).

### Tempo

A tempo in the name (`128`, `128bpm`, `bpm128`) warps the clip with that Seg. BPM, so
the loop plays in time at any project tempo. Files 6 seconds or longer are warped even
without one (at the project tempo, as when you turn Warp on).

### Key

A key in the name (`Am`, `F#m`, `Eb`, `G minor`, `Cmaj`) transposes the clip to the
**project key**, chosen next to the metronome in the transport bar.

- A minor key and its relative major count as the same (an A minor loop stays put in C
  major), and the clip takes the shortest shift (at most 6 semitones).
- With *No Key*, nothing is transposed.
- A bare letter (`_D_`) counts only next to a tempo, so names like "A Day" are left
  alone.

## Audio files

The browser lists WAV, FLAC and MP3 files. Files are decoded into memory when added (see
[limitations.md](limitations.md)).

---

For developers: [../ui/piano-roll.md](../ui/piano-roll.md) (the clip view),
[../engine/warp.md](../engine/warp.md), [../app/model.md](../app/model.md).
