# Importing Ableton Live Sets

`io/LiveSet.h` reads a Live Set; `io/LiveImport.h` translates it into a project file's JSON;
`Session::importLiveSet` loads that as a new, untitled project. What the user sees:
[../guide/ableton-import.md](../guide/ableton-import.md).

```
Session::importLiveSet(path)                      (session/SessionFiles.cpp)
  ├─ live::readLiveSet(path)                      gzip (puff) → XML (QXmlStreamReader) → Element tree
  ├─ live::importLiveSet(root, path, {plug-ins})  → ImportResult {project JSON, notes, tracks, clips}
  ├─ loadInto(project, json)                      as a project file loads: repairs, defaults, reset
  └─ untitled, named after the set; projectOpened; statusMessage; information(notes)
```

## Files

| File | What |
|---|---|
| [io/LiveSet.h](../../app/src/io/LiveSet.h) / [.cpp](../../app/src/io/LiveSet.cpp) | `Element` (a tag, attributes, trimmed text, children; `at("A/B")`, `value()`, `number()`, `flag()`), `readLiveSet`, `parseLiveSet`, `gunzip`, `isGzip` |
| [io/LiveImport.h](../../app/src/io/LiveImport.h) / [.cpp](../../app/src/io/LiveImport.cpp) | `importLiveSet`, `ImportOptions` (the installed plug-ins), `ImportResult`; `vstPreset`, `vst2CompatibleState` |
| [engine/src/plugins/Vst3Ids.h](../../engine/src/plugins/Vst3Ids.h) | VST3 class ids: from the four words Live stores to `PluginDescription::uid` (`classIdFromWords`), to a .vstpreset's spelling (`presetClassId`), and back (`wordsFromClassId`) |
| [app/third_party/puff](../../app/third_party/puff) | Mark Adler's inflate (zlib licence), vendored unmodified |

## Reading

A set is gzip-compressed XML (plain XML is read too). `gunzip` reads gzip's header, inflates
with `puff` into a buffer as long as the trailer says, and checks the trailer's length and CRC-32:
anything else is "damaged". `parseLiveSet` builds the element tree with `QXmlStreamReader`:

- Element and attribute names are interned (a set has about a million elements of some 1500
  names), looked up by the reader's `QStringView` so no string is made for a name seen before.
- Subtrees the importer never reads are skipped as they are read (`skippedElements()`: the Session
  View's clip slots, frozen tracks' sequencers, MIDI controllers' targets, take lanes, views, the
  groove pool, presets' and browsers' records...): about a third of a set.
- Text is kept only where there is some: plug-in states, in hex.

A 55 MB set (a 3 MB file) reads in about 2 s: 0.35 s inflating, the rest parsing.

## Translating

`Importer` makes two passes over `LiveSet/Tracks`:

1. `assignIds()`: an id for every track and return, and for each Drum Rack pad the arrangement
   plays (`pads_`), so routing to a track further down (outputs, inputs, sidechains, a pad's
   output) resolves in the second pass.
2. `run()`: each track, return and the main track translated (`translateTrack`,
   `translateDrumTrack`, `translateReturn`, `translateMaster`), then their automation.

Every parameter translated that Live can automate is entered in `targets_`: its
`AutomationTarget Id` → (owner track, automation key, a function mapping Live's value to
automation's 0..1). `readAutomation` then puts each `AutomationEnvelope` (by its `PointeeId`) in its
owner's automation; one whose target isn't there is counted in the notes. The tempo's and time
signature's targets are read by `readSettings` instead.

Values Live writes, and what they become:

| Live | Here |
|---|---|
| Mixer and rack chain volume, send level: linear gain (1 is 0 dB, 1.995 is +6 dB) | dB, in the faders' range; automation through `volumeToNormalized` |
| Time signature: `numerator - 1 + 99 * log2(denominator)` | numerator, denominator |
| Colour: an index into Live's 70 colours | `kLiveColors` (its first 13 are `kTrackColors`) |
| Drum Rack pad `ReceivingNote`: `128 - note` | the MIDI note |
| Utility gain, Compressor threshold: linear amplitude | dB |
| Delay times: seconds; feedback, dry/wet: 0..1 | ms; percent |
| VST3 parameters: normalized | the same (their ids are the VST3 `ParamID`s) |

Clips:

- `clipSpans()` turns a clip's markers into what it plays: from its start marker (`LoopStart +
  StartRelative`) for its length, and round its loop (`LoopStart..LoopEnd`) when `LoopOn`. Warped
  and MIDI clips' markers are in beats, unwarped audio clips' in seconds.
- MIDI notes are written out span by span, cut at a span's end; a Drum Rack pad's clips keep its
  note only, at its `SendingNote` (a pad track's clip with none of them is left out).
- Warped audio: `WarpMap` maps content beats to file seconds through the warp markers (straight
  between them, the end stretches' tempo beyond). Each span is cut at the markers in it; each piece
  is a clip at `60 * beats / seconds` BPM, pieces of one tempo that follow each other are joined.
  Pieces are held to the file (from 0 to its length), tempos to `kMinSegmentBpm`..`kMaxSegmentBpm`.
- Fades go on the first and last piece, converted from beats (warped) or seconds.

Plug-ins:

- A VST3's class id is `Uid/Fields.0..3`, four 32-bit words (FUID's longs). `classIdFromWords`
  makes the engine's uid of them (the TUID's bytes: a COM GUID's layout on Windows, so the hex
  differs by platform), which finds the installed plug-in (its path, vendor, name). Its settings
  become a .vstpreset (`vstPreset`): Steinberg's header with `presetClassId` (the words as
  `%08X%08X%08X%08X`, the same on every platform), the `ProcessorState` as the `Comp` chunk, the
  `ControllerState` as `Cont`.
- A VST2 (`VstPluginInfo`) becomes an installed VST3: one whose class id is Steinberg's VST2
  replacement id for it (`"VST"`, the VST2's `UniqueId`, its name: `wordsFromClassId` says) takes
  its settings as `vst2CompatibleState` makes them (`VstW`, then an fxb or fxp `CcnK` block of its
  chunk: what Steinberg's VST2 wrapper and JUCE's VST3s read); else one of the same name
  (`plainPluginName`: lower case, letters and digits, no `x64`) at its defaults.

## The notes

`Notes` collects what didn't come across: plain lines once (`line`), and lists with how many of
each (`listed`: "Left out (SUBstation has no device like them): Saturator (15), Vocoder (7).").
Tracks are named in them as Live shows them ("31-Audio").

## Tests

[test_live_import.cpp](../../tests/app/test_live_import.cpp) writes sets as Live does (gzip or
XML) and checks each part: reading and damage, the song's settings, tracks, groups, returns,
mixers and routing, MIDI loops, audio clips and warping, plug-ins (.vstpreset chunks, VST2
replacement states), Live's own devices, Drum Racks, automation, and `Session::importLiveSet`.
`test_ui_mainwindow`'s `templatesAndImportingALiveSet` goes through the menu and its dialog.
