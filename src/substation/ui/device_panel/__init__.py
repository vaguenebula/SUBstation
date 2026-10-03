"""Bottom 'detail view': the selected track's (or the master's) device chain. On
a MIDI track the instrument comes first; the master takes effects only.

Each device has a title bar, as in Ableton: its on/off switch and name, the
button for a plug-in's own editor, the sidechain button of a device with a
sidechain (aux) input, the arrows to its other parameter pages, and its save
button, which saves it as a preset in the user's library (model/presets.py),
under a name asked for (everything in a rack too: plug-ins' states, macros).
A rack takes the name of the preset it is saved as (or loaded from) as its
title. Right-click › Save as Default Preset makes it what new devices of its kind (that
plug-in) start as; Clear Default Preset undoes that. It is lighter while the
device is selected. The sidechain
button is lit while the device has a sidechain; clicking it picks the track it
comes from (those that would close a cycle greyed out) and where it is taken:
after the track's fader, before it, or after one of its devices.

Parameter metadata comes from the engine, so built-in devices and plug-ins
show alike: a knob per parameter (log-scaled where the engine says so), or a
list for parameters that choose between named values, four at a time in a 2×2
grid. A plug-in shows its own text for their values. Right-click a device for
more (move, presets; a plug-in's VST3 presets too). A built-in device may have an editor of its own instead
(see device_editors), which can also draw what the engine reports as it plays
(its displays: meters, curves).

Automated parameters are marked (red: automated, grey: overridden) and follow
their automation as it plays; right-click one to show its automation, delete it,
or re-enable it. Changing one shows its automation in the arrangement.

Click a device (its title or background) to select it, Shift-click to select a
range, Ctrl-click to add or remove one; Delete deletes the selection. Drag
effects to reorder them (the instrument stays first), or onto another track in
the arrangement to move them there (plug-ins keep their state). Ctrl+Alt-drag anywhere on
the chain scrolls it, as in the arrangement. The chain scrolls to show a device
when one is added, unless it was dropped on the chain (where it is in view).

Racks (device groups) show their macros and their chains (rack_view); the
chain clicked shows its devices right after the rack, in a bracket, where they
are selected, dragged and dropped onto as on the track's own chain (and racks
in it show theirs, further along). Ctrl+G groups the selected devices (of one
chain) into a rack; Ctrl+Shift+G ungroups a rack. Right-click a parameter of a
device in a rack to map one of the rack's macros to it.

Presets dragged from the browser go where they are dropped, as new devices, or
load into the device they are dropped onto if it is of their kind (the same
plug-in, built-in device, or kind of rack; it is outlined while the drag is
over it): one undo step. Right-click beside the devices to load a preset file
there.

The fold button (a triangle, first on the title bar) folds a device to a
narrow strip with its name; a folded rack hides its chains too. Click the
triangle again (or double-click the strip) to unfold it. Ctrl+double-click a
device (its title or background) to fold or unfold it too. With several devices selected,
folding one of them folds them all. Folding is saved with the project, not an
undo step.

Ctrl+C, Ctrl+X and Ctrl+V (and Ctrl+D) copy, cut and paste (and duplicate) the
selected devices while the device view has the focus (devices clicked, or the
space beside them): pasted devices go after the selected ones, or at the end of
the track's chain, as new devices with the same settings (plug-ins in their
state when copied, sidechains kept where they can be). Their right-click menus
have them too; right-click beside the devices to paste there.

Its parts: frame.py (what every device shares), device_widgets.py (built-in devices,
plug-ins, racks) and panel.py (the device view).
"""

from __future__ import annotations

from .device_widgets import DeviceWidget, PluginDeviceWidget, RackWidget
from .frame import DEVICE_WIDTH, FOLDED_WIDTH, PARAM_WIDTH, device_height
from .panel import DevicePanel, preset_folder

__all__ = [
    "DEVICE_WIDTH",
    "FOLDED_WIDTH",
    "PARAM_WIDTH",
    "DevicePanel",
    "DeviceWidget",
    "PluginDeviceWidget",
    "RackWidget",
    "device_height",
    "preset_folder",
]
