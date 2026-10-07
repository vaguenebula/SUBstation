# VST3 plug-ins

SUBstation hosts VST3 instruments and effects. This page covers finding them, using
them, their editors and presets, how projects keep them, and their latency. Plug-ins
sit in the device view beside the built-in devices (see [devices.md](devices.md)).

## Finding them

- The browser lists the plug-ins in the standard VST3 folders
  (`C:\Program Files\Common Files\VST3` and `%LOCALAPPDATA%\Programs\Common\VST3`; on
  Linux `~/.vst3`, `/usr/lib/vst3` and `/usr/local/lib/vst3`)
  under *Plug-ins › Instruments / Audio Effects*, with their vendor. A module with
  several plug-ins (an instrument and its FX version) shows each.
- Besides the standard VST3 folders, plug-ins are looked for in folders of your own:
  add or remove them in *Options › Preferences › Plug-ins* (*Add Folder…*, *Remove*).
  An added folder is scanned at once (only its new files), and a removed folder's
  plug-ins leave the browser. A folder that no longer exists shows in red.

### Scanning

- Plug-in files are read in a separate process, several per process, so a plug-in that
  crashes or hangs while loading is only marked as failed. Hover over *Plug-ins* in the
  browser for the list and the reasons (the *Plug-ins* page of the preferences shows
  them too).
- The scan runs in the background at start-up and reads only new or changed files; the
  results are cached in `%LOCALAPPDATA%\SUBstation\vst3-cache.json` (on Linux
  `~/.local/share/SUBstation/vst3-cache.json`). The browser's
  footer shows its progress.
- *Options › Rescan Plug-ins* (or the button in *Preferences › Plug-ins*, or right-click
  *Plug-ins* in the browser) reads everything again, also the files that could not be
  read before.

## Using them

- Drag a plug-in onto a track, into the device view, or below the tracks (an instrument
  makes a MIDI track); or double-click it to add it to the selected track.
- Instruments go on a MIDI track, replacing its instrument.
- Instruments play the MIDI clips (and the piano roll's notes) sample-accurately.
- MIDI controllers reach the parameters the plug-in maps them to.

### In the device view

- The device view shows a plug-in's parameters four at a time (‹ › pages), with the
  plug-in's own text for their values (`-3.0 dB`, `Bell`); lists get a menu.
- Read-only and hidden parameters are left out, and the device's on/off switch stands
  in for the plug-in's own bypass.
- A plug-in with no parameters to show says so: use its editor.
- The title's tooltip shows the plug-in's name, vendor, file and latency.

### The plug-in's editor

- **Edit** (the window button in the title bar), *Show Editor* in the right-click menu,
  or double-clicking the device opens the plug-in's editor in a window of its own,
  which floats above the main window.
- It follows the plug-in's resize requests, lets you resize it within the plug-in's
  limits (if it can resize), and tells the plug-in when it moves to a screen with
  another scale.
- **Ctrl+W** (*View › Close Plug-in Editor*) closes the plug-in editor in front.
- Turning knobs in the plug-in's editor is recorded for undo like any other edit: one
  step per knob drag. A plug-in that changes in a way no parameter shows (a preset
  picked in its editor) marks the project as changed.
- Controls in the editor can be automated like any parameter; see
  [automation.md](automation.md).
- Which keys reach the editor and which go to SUBstation is in
  [shortcuts.md](shortcuts.md#plug-in-editors).
- On Linux plug-ins' editor windows don't open: the status line says the plug-in has no
  editor, and you edit its parameters in the device view.

### Presets

A plug-in's save button saves it as a SUBstation preset, in your preset library (see
[devices.md](devices.md#presets)). Right-click a plug-in: *Load VST3 Preset…* / *Save
VST3 Preset…* read and write standard `.vstpreset` files, for sharing settings with
other hosts (loading one is undoable).

## Projects

Projects save each plug-in's complete state (as a `.vstpreset`, base64) and which
plug-in it is. A plug-in that moved is found again by its class id; a missing one keeps
its place and settings in the project, shows what's wrong in the device view, and loads
when it's back (after a rescan).

Opening a project shows it at once, and its plug-ins load after it, one at a time, while
you work (the title bar counts them, on its right: *Loading plug-ins: 3 of 12*).
Meanwhile:

- a plug-in still waiting says so in the device view (*… is loading*), and its track plays
  without it (an instrument's track is silent);
- the selected track's plug-ins load first, and opening a plug-in's editor loads it at once;
- you can edit, move or delete devices as usual, and save: a plug-in not loaded yet keeps
  its settings as they were;
- exporting and freezing wait for them (their progress dialog says how many are left).

They load on the window's own thread, as plug-ins require, so a slow one can still make
the window pause while it loads; the project around it doesn't wait.

## Latency

Latency that plug-ins report (look-ahead limiters, linear-phase EQs) is compensated:
the other tracks, and the metronome, are delayed to line up, and exports come out
aligned. The device's tooltip shows the latency. See
[mixing.md](mixing.md#delay-compensation).

## Transport

Plug-ins get the tempo, time signature, position in samples and beats, bar position,
playing state and the loop, and their audio is split where the loop wraps, so
tempo-synced plug-ins stay in time.

## Buses

- Mono-only plug-ins get the track mixed to mono, and their output goes to both sides.
- A plug-in's first aux input is its sidechain (stereo if it takes it; see
  [mixing.md](mixing.md#sidechains)).
- Other extra buses get silent inputs, and their extra outputs are not used (no
  multi-output instruments yet).

---

For developers: [../engine/plugins.md](../engine/plugins.md),
[../app/plugin-scanner.md](../app/plugin-scanner.md),
[../ui/device-view.md](../ui/device-view.md).
