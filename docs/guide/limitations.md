# Limitations

What SUBstation doesn't do yet, by area.

## Recording and MIDI input

- MIDI overdub (recording into existing clips).
- Recording controllers (sustain, pitch bend): they are played, not recorded.
- Windows MIDI Services (MIDI input is WinMM).
- Recording WASAPI devices' inputs: WASAPI opens outputs only (resampling works).
- Punching in and out at the loop, and stacking takes while looping.

## MIDI editing

- Looping MIDI clips.
- MIDI effects.
- Editing several MIDI clips in the piano roll at once.

## Automation and tempo

- Recording automation (writing it while playing).
- Tempo changes over time.
- Automation lanes below a track have a fixed height.

## Devices and plug-ins

- CLAP plug-ins.
- Multi-output instruments: plug-ins get their main buses and a sidechain only.
- MIDI effect plug-ins.
- The Sampler plays one sample (no zones or multisamples).

## Audio clips

- Warp markers (warping within a clip) and automatic tempo detection: a warped clip has
  one segment BPM, and you set it.
- Streaming long files from disk: sources are decoded into memory.

## Audio devices

- DSD ASIO drivers.
- More than one ASIO driver open at once.

---

For developers: [../../TODO.md](../../TODO.md), [../architecture.md](../architecture.md).
