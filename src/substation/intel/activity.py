"""The activity log: what actors other than the user did (an agent, the
assistant, automatic suggestions), each entry `{time, actor, op, args, result,
undo_index}`. Kept in memory for the activity view and appended to
%LOCALAPPDATA%\\SUBstation\\activity.jsonl (or SUBSTATION_ACTIVITY_LOG), which
is rotated once it grows past MAX_BYTES. `undo_index` is the undo stack's
index after the action: when the stack goes below it again, the action was
undone (how the layer will learn that an undo meant "no")."""

from __future__ import annotations

import json
import os
import time
from collections import deque
from collections.abc import Callable
from dataclasses import asdict, dataclass
from pathlib import Path

MAX_ENTRIES = 500  # kept in memory
MAX_BYTES = 2_000_000  # the file is rotated (to activity.1.jsonl) past this


def log_path() -> Path:
    override = os.environ.get("SUBSTATION_ACTIVITY_LOG")
    if override:
        return Path(override)
    base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    return base / "SUBstation" / "activity.jsonl"


@dataclass(frozen=True)
class ActivityEntry:
    time: float
    actor: str
    op: str
    args: dict
    result: dict
    undo_index: int | None

    @property
    def ok(self) -> bool:
        return "error" not in self.result


class ActivityLog:
    def __init__(self, path: Path | None = None, persist: bool = True):
        self.entries: deque[ActivityEntry] = deque(maxlen=MAX_ENTRIES)
        self.path = log_path() if path is None else Path(path)
        self.persist = persist
        self._listeners: list[Callable[[ActivityEntry], None]] = []

    def subscribe(self, listener: Callable[[ActivityEntry], None]) -> None:
        self._listeners.append(listener)

    def record(self, actor: str, op: str, args: dict, result: dict, undo_index: int | None) -> ActivityEntry:
        entry = ActivityEntry(time.time(), actor, op, dict(args), dict(result), undo_index)
        self.entries.append(entry)
        if self.persist:
            self._append(entry)
        for listener in list(self._listeners):
            listener(entry)
        return entry

    def _append(self, entry: ActivityEntry) -> None:
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            if self.path.exists() and self.path.stat().st_size > MAX_BYTES:
                self.path.replace(self.path.with_suffix(".1.jsonl"))
            with self.path.open("a", encoding="utf-8") as f:
                f.write(json.dumps(asdict(entry), default=str, ensure_ascii=False) + "\n")
        except OSError:
            pass  # the log is a convenience: never fail an edit over it
