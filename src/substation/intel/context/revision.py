"""The project's revision: a number that goes up with every change to it, so
that a caller who read revision 41 and acts on what it saw can be told the
project moved on (`if_revision`, error 'conflict') instead of clobbering an
edit made since."""

from __future__ import annotations

from collections.abc import Callable

from ...model.project import Project

# Every Project signal but those of view state alone (what the arrangement shows of
# automation, devices folded): those change nothing a reader of the song could act on.
_SIGNALS = (
    "track_inserted", "track_removed", "return_inserted", "return_removed", "track_changed", "tracks_arranged",
    "clips_changed", "devices_changed", "chain_changed", "device_param_changed", "device_state_changed",
    "freeze_changed", "settings_changed", "automation_changed", "reset",
)


class IntelRevision:
    def __init__(self, project: Project):
        self.value = 0
        self._listeners: list[Callable[[int], None]] = []
        for name in _SIGNALS:
            getattr(project, name).connect(self._bump)

    def _bump(self, *_args) -> None:
        self.value += 1
        for listener in list(self._listeners):
            listener(self.value)

    def subscribe(self, listener: Callable[[int], None]) -> None:
        """`listener(revision)` after every change."""
        self._listeners.append(listener)
