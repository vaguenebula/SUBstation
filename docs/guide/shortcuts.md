# Keyboard shortcuts

Every keyboard and mouse shortcut, and which keys reach a plug-in's editor while it has
the focus. Most are also in the menus, which show their keys.

## Transport

| Action | Keys |
|---|---|
| Play / stop (returns to the start marker) | Space |
| Stop; press again to return to the start | Stop button |
| Go to start | Home |
| Record (the armed tracks) | F9 |
| Loop on/off | Ctrl+L |

## Projects

| Action | Keys |
|---|---|
| New project / open / save | Ctrl+N / Ctrl+O / Ctrl+S |
| Save as | Ctrl+Shift+S |
| Export audio | Ctrl+Shift+R |
| Preferences | Ctrl+, |
| Quit | Ctrl+Q |
| Undo / redo | Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) |

## Tracks

| Action | Keys |
|---|---|
| Insert audio track / MIDI track | Ctrl+T / Ctrl+Shift+T |
| Insert return track | Ctrl+Alt+T |
| Duplicate the selected tracks (a track header clicked) | Ctrl+D |
| Group / ungroup the selected tracks | Ctrl+G / Ctrl+Shift+G |
| Solo the selected tracks (or unsolo every track) | S |
| Resize the track under the mouse; at its smallest, fold it (wheel down) or unfold it (wheel up) (piano roll: the keys' rows) | Alt+wheel |

## Clips

| Action | Keys |
|---|---|
| Insert MIDI clip (on the selected MIDI track, or over a time selection) | Ctrl+Shift+D (or Ctrl+Shift+M) |
| Duplicate / split at insert marker / delete (clips, or automation in a lane's time selection) | Ctrl+D / Ctrl+E / Delete (or Backspace) |
| Cut / copy / paste clips, or automation in a lane's time selection (the last copied is what pastes) | Ctrl+X / Ctrl+C / Ctrl+V |
| Consolidate the selected MIDI clips on each track into one (also in the clip's right-click menu) | Ctrl+J |
| Select all clips | Ctrl+A |
| Copy clips while dragging | hold Ctrl |
| Bypass snapping while dragging | hold Alt |
| Show / hide the clip view | Shift+Tab |
| Back to the arrangement from the clip view | Esc |

## Automation

| Action | Keys |
|---|---|
| Show / hide automation (every track and the master) | A |
| Add an automation breakpoint / delete one | click on the envelope's line / click the breakpoint |
| Bend an automation segment | Alt-drag between two breakpoints |

## View

| Action | Keys |
|---|---|
| Zoom in / out / to arrangement | + / − / Z |
| Zoom around the mouse | Ctrl+wheel (or drag vertically in the ruler) |
| Scroll horizontally | Shift+wheel (or drag horizontally in the ruler) |
| Scroll in any direction | Ctrl+Alt drag |
| Narrow / widen grid, toggle snap | Ctrl+1 / Ctrl+2 / Ctrl+4 |
| Toggle browser / device view | Ctrl+Alt+B / Ctrl+Alt+L |
| Search everything in the browser ("All"); Enter selects the first result, Enter again adds it | Ctrl+F |

## Devices

| Action | Keys |
|---|---|
| Fold / unfold a device | Ctrl+double-click it (or its ▾ button) |
| Cut / copy / paste / duplicate devices (the device view has the focus) | Ctrl+X / Ctrl+C / Ctrl+V / Ctrl+D |
| Group the selected devices into a rack / ungroup a rack | Ctrl+G / Ctrl+Shift+G |
| Show a plug-in's editor | double-click its device in the device view |
| Close the plug-in editor in front | Ctrl+W |

## MIDI

| Action | Keys |
|---|---|
| Computer MIDI keyboard on/off | M |
| Play notes (while it is on) | A S D F G H J K L ; ' (white keys), W E T Y U O P (black keys) |
| Octave down / up (while it is on) | Z / X |

In the piano roll, Delete, Ctrl+A and Ctrl+D act on notes, Ctrl+U quantizes them, and
arrow keys move them (Up/Down a semitone, Shift an octave; Left/Right a grid step, Shift
a bar). See [midi.md](midi.md#the-piano-roll).

## Plug-in editors

The Ctrl/Alt shortcuts also work while a plug-in's editor window has the focus, except
Ctrl+A/C/V/X/Z/Y, which the plug-in keeps for its own text fields.

So do **Space** (play / stop) and **S** (solo): the DAW comes first. They go to the
plug-in only:

- while it types into a standard Windows text field (most plug-ins draw their own,
  which can't be told apart, so Space and S never reach those);
- and S while the computer MIDI keyboard is on (it plays a note in the main window too).

Held down, Space acts once. Other keys without Ctrl or Alt (Delete, letters, ...) go to
the plug-in.

---

For developers: [../ui/README.md](../ui/README.md) (main_window.py, plugin_keys.py,
computer_keyboard.py).
