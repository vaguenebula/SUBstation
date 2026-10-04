"""High-level, undoable edit operations used by the UI: ProjectEditor.

Its operations are grouped by what they edit, one mixin per module:
tracks.py (tracks, returns and sends, groups, inputs, recordings),
settings.py (tempo, time signature, key, loop), clips.py (clips and time
selections), device_chains.py (devices in chains), racks.py (racks, their
chains and macros), device_settings.py (a device's parameters, state,
presets, switch and sidechain), automation_edits.py (envelopes and the
lanes shown) and freezing.py (freezing, flattening, and what frozen tracks
refuse). Each turns a user action into undo commands (commands.py), pushed
through _push, which refuses those changing what frozen audio holds (saying
why on `refused`). Kinds of devices and new devices are in devices.py."""

from __future__ import annotations

from PySide6.QtCore import QObject, Signal
from PySide6.QtGui import QUndoStack

from ..project import Project
from .automation_edits import AutomationEdits, CopiedAutomation, LaneRef
from .clips import ClipboardContent, ClipEdits, ClipRef, CopiedTrack
from .device_chains import DeviceChainEdits
from .device_settings import DeviceSettingsEdits
from .freezing import FreezeEdits
from .racks import RackEdits
from .settings import SettingsEdits
from .tracks import AT_INDEX, RecordedTake, TrackEdits

__all__ = [
    "AT_INDEX",
    "ClipRef",
    "ClipboardContent",
    "CopiedAutomation",
    "CopiedTrack",
    "LaneRef",
    "ProjectEditor",
    "RecordedTake",
]


class ProjectEditor(TrackEdits, SettingsEdits, ClipEdits, DeviceChainEdits, RackEdits, DeviceSettingsEdits,
                    AutomationEdits, FreezeEdits, QObject):
    # (track id, device id) when the user adds a plug-in (not on undo or redo).
    plugin_added = Signal(str, str)
    # (automation owner, target key) when the user changes a parameter that can be
    # automated (not on undo or redo): its automation lane shows it.
    parameter_touched = Signal(str, str)
    # Why an edit wasn't made: it would change what a frozen track's audio holds.
    refused = Signal(str)

    def __init__(self, project: Project, undo_stack: QUndoStack):
        super().__init__()
        self.project = project
        self.undo_stack = undo_stack

    def _push(self, command) -> None:
        problem = self._frozen_problem(command)
        if problem is not None:
            self.refused.emit(problem)
            return
        self.undo_stack.push(command)
