"""Editing the project's settings: tempo, time signature, key and loop."""

from __future__ import annotations

from .. import edits
from ..commands import SetTempoCommand, UpdateSettingsCommand
from ..keys import Key
from ..timebase import TimeSignature


class SettingsEdits:
    """The project's tempo, time signature, key and loop. Part of ProjectEditor (editor/__init__.py)."""

    def _set_settings(self, text: str, merge_key: object | None = None, **new) -> None:
        old = {name: getattr(self.project, name) for name in new}
        if old != new:
            self._push(UpdateSettingsCommand(self.project, old, new, text, merge_key))

    def set_tempo(self, bpm: float, merge_key: object | None = None) -> None:
        """Change the tempo, trimming clips that would otherwise overlap."""
        p = self.project
        tempo = max(20.0, min(999.0, round(bpm, 2)))
        if tempo == p.tempo:
            return
        current = {t.id: list(t.clips) for t in p.tracks}
        # Within one drag, fit from the clips as they were when the drag began, so
        # going up and back down doesn't leave clips trimmed.
        baseline = current
        index = self.undo_stack.index()
        last = self.undo_stack.command(index - 1) if index > 0 else None
        if (merge_key is not None and isinstance(last, SetTempoCommand) and last.merge_key == merge_key
                and last.old[1].keys() == current.keys()):
            baseline = last.old[1]
        fitted = {tid: edits.fit_to_tempo(clips, tempo) for tid, clips in baseline.items()}
        self._push(SetTempoCommand(p, (p.tempo, current), (tempo, fitted), merge_key))

    def set_time_signature(self, ts: TimeSignature) -> None:
        self._set_settings("Change Time Signature", time_signature=ts)

    def set_key(self, key: Key | None) -> None:
        """The project's key (None: none). Audio added afterwards is transposed to it."""
        self._set_settings("Change Key", key=key)

    def set_loop(self, enabled: bool, start: float, end: float, merge_key: object | None = None) -> None:
        start = max(0.0, start)
        end = max(start + 0.25, end)
        self._set_settings("Change Loop", merge_key, loop_enabled=enabled, loop_start=start, loop_end=end)

    def set_loop_enabled(self, enabled: bool) -> None:
        self._set_settings("Toggle Loop", loop_enabled=enabled)
