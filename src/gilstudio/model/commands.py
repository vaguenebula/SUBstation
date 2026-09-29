"""Undo commands. Every project mutation goes through one of these.

Continuous gestures (dragging a fader, a tempo value, a loop brace) pass a
`merge_key`; consecutive commands with the same key collapse into one undo step.
"""

from __future__ import annotations

import copy
from typing import Any

from PySide6.QtGui import QUndoCommand

from .project import Clip, Device, Project, Track

_MERGE_ID = 0x6E1


class SetClipsCommand(QUndoCommand):
    """Replaces the clip lists of one or more tracks (moves, trims, splits...).
    With a `merge_key` (a knob drag in the clip view) consecutive edits merge."""

    def __init__(self, project: Project, text: str, before: dict[str, list[Clip]], after: dict[str, list[Clip]],
                 merge_key: object | None = None):
        super().__init__(text)
        self.project = project
        self.before = before
        self.after = after
        self.merge_key = merge_key

    def id(self) -> int:
        return _MERGE_ID if self.merge_key is not None else -1

    def mergeWith(self, other: QUndoCommand) -> bool:
        if (not isinstance(other, SetClipsCommand) or other.merge_key != self.merge_key
                or other.after.keys() != self.after.keys()):
            return False
        self.after = other.after
        return True

    def redo(self) -> None:
        for track_id, clips in self.after.items():
            self.project.set_clips(track_id, clips)

    def undo(self) -> None:
        for track_id, clips in self.before.items():
            self.project.set_clips(track_id, clips)


class InsertTrackCommand(QUndoCommand):
    def __init__(self, project: Project, track: Track, index: int, text: str = "Insert Track"):
        super().__init__(text)
        self.project = project
        self.track = track
        self.index = index

    def redo(self) -> None:
        self.project.insert_track(copy.deepcopy(self.track), self.index)

    def undo(self) -> None:
        self.project.remove_track(self.track.id)


class RemoveTrackCommand(QUndoCommand):
    def __init__(self, project: Project, track_id: str, text: str = "Delete Track"):
        super().__init__(text)
        self.project = project
        self.track_id = track_id
        self.saved: tuple[Track, int] | None = None

    def redo(self) -> None:
        self.saved = self.project.remove_track(self.track_id)

    def undo(self) -> None:
        track, index = self.saved
        self.project.insert_track(track, index)


class _MergeableCommand(QUndoCommand):
    """Base for single-value changes that merge while a gesture is in progress."""

    def __init__(self, text: str, key: tuple, old: Any, new: Any, merge_key: object | None):
        super().__init__(text)
        self.key = key
        self.old = old
        self.new = new
        self.merge_key = merge_key

    def id(self) -> int:
        return _MERGE_ID if self.merge_key is not None else -1

    def mergeWith(self, other: QUndoCommand) -> bool:
        if not isinstance(other, type(self)) or other.merge_key != self.merge_key or other.key != self.key:
            return False
        self.new = other.new
        return True


class UpdateTrackCommand(_MergeableCommand):
    def __init__(self, project: Project, track_id: str, attr: str, old: Any, new: Any,
                 text: str, merge_key: object | None = None):
        super().__init__(text, (track_id, attr), old, new, merge_key)
        self.project = project

    def redo(self) -> None:
        self.project.update_track(self.key[0], **{self.key[1]: self.new})

    def undo(self) -> None:
        self.project.update_track(self.key[0], **{self.key[1]: self.old})


class SetTempoCommand(_MergeableCommand):
    """Tempo change plus the clip trims that keep unwarped clips from overlapping.
    `old`/`new` are (tempo, {track id: clips}). While a tempo drag merges, `old`
    stays the state before the drag, so undo restores untrimmed clips."""

    def __init__(self, project: Project, old: tuple, new: tuple, merge_key: object | None = None):
        super().__init__("Change Tempo", ("tempo",), old, new, merge_key)
        self.project = project

    def _apply(self, state: tuple) -> None:
        tempo, clips = state
        self.project.update_settings(tempo=tempo)
        for track_id, track_clips in clips.items():
            if self.project.has_track(track_id) and self.project.track(track_id).clips != list(track_clips):
                self.project.set_clips(track_id, list(track_clips))

    def redo(self) -> None:
        self._apply(self.new)

    def undo(self) -> None:
        self._apply(self.old)


class UpdateSettingsCommand(_MergeableCommand):
    """Changes project settings; `old`/`new` are dicts of attribute values."""

    def __init__(self, project: Project, old: dict, new: dict, text: str, merge_key: object | None = None):
        super().__init__(text, tuple(sorted(new)), old, new, merge_key)
        self.project = project

    def redo(self) -> None:
        self.project.update_settings(**self.new)

    def undo(self) -> None:
        self.project.update_settings(**self.old)


class SetDevicesCommand(QUndoCommand):
    def __init__(self, project: Project, track_id: str, before: list[Device], after: list[Device], text: str):
        super().__init__(text)
        self.project = project
        self.track_id = track_id
        self.before = before
        self.after = after

    def redo(self) -> None:
        self.project.set_devices(self.track_id, copy.deepcopy(self.after))

    def undo(self) -> None:
        self.project.set_devices(self.track_id, copy.deepcopy(self.before))


class SetDeviceParamCommand(_MergeableCommand):
    def __init__(self, project: Project, track_id: str, device_id: str, param_id: str, old: float, new: float,
                 merge_key: object | None = None):
        super().__init__("Change Device Parameter", (track_id, device_id, param_id), old, new, merge_key)
        self.project = project

    def redo(self) -> None:
        self.project.set_device_param(*self.key, self.new)

    def undo(self) -> None:
        self.project.set_device_param(*self.key, self.old)


class SetDeviceEnabledCommand(QUndoCommand):
    def __init__(self, project: Project, track_id: str, device_id: str, enabled: bool):
        super().__init__("Activate Device" if enabled else "Deactivate Device")
        self.project = project
        self.track_id = track_id
        self.device_id = device_id
        self.enabled = enabled

    def redo(self) -> None:
        self.project.set_device_enabled(self.track_id, self.device_id, self.enabled)

    def undo(self) -> None:
        self.project.set_device_enabled(self.track_id, self.device_id, not self.enabled)
