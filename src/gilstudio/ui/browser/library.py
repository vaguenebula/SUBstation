"""What the browser remembers about its items, beyond the files themselves.

One record per item, keyed by `BrowserItem.key`, in a JSON file next to the
plug-in cache. For now a record counts how often the item was used (added to the
project from the browser); search results are ranked by that. Records are plain
dicts, and fields this version does not know are kept, so later versions can add
their own (hidden from search, sound features...) without losing anything."""

from __future__ import annotations

import json
import os
import time
from collections.abc import Callable
from pathlib import Path

VERSION = 1
HALF_LIFE_DAYS = 30.0  # a use counts half as much after this long


def library_path() -> Path:
    override = os.environ.get("GILSTUDIO_LIBRARY")
    if override:
        return Path(override)
    base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    return base / "GIL Studio" / "library.json"


class Library:
    def __init__(self, path: Path | None = None, clock: Callable[[], float] = time.time):
        self.path = library_path() if path is None else Path(path)
        self.clock = clock
        self.records: dict[str, dict] = self._load()

    # --- Usage ---------------------------------------------------------------------

    def record_use(self, keys: list[str]) -> None:
        """The items were used (added to the project) just now."""
        now = self.clock()
        for key in keys:
            record = self.records.setdefault(key, {})
            record["uses"] = int(record.get("uses", 0)) + 1
            record["score"] = self.rank(key, now) + 1.0
            record["last_used"] = now
        if keys:
            self.save()

    def uses(self, key: str) -> int:
        record = self.records.get(key)
        return int(record.get("uses", 0)) if record else 0

    def rank(self, key: str, now: float | None = None) -> float:
        """How much, and how recently, the item was used: each use counts 1 when it
        happens, and half of that every HALF_LIFE_DAYS after. 0 if never used."""
        record = self.records.get(key)
        if not record or not record.get("score"):
            return 0.0
        now = self.clock() if now is None else now
        days = max(0.0, now - float(record.get("last_used", now))) / 86400.0
        return float(record["score"]) * 0.5 ** (days / HALF_LIFE_DAYS)

    # --- File ----------------------------------------------------------------------

    def _load(self) -> dict[str, dict]:
        try:
            data = json.loads(self.path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}
        items = data.get("items") if isinstance(data, dict) and data.get("version") == VERSION else None
        if not isinstance(items, dict):
            return {}
        return {k: v for k, v in items.items() if isinstance(k, str) and isinstance(v, dict)}

    def save(self) -> None:
        try:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            tmp = self.path.with_suffix(".tmp")
            tmp.write_text(json.dumps({"version": VERSION, "items": self.records}, indent=1), encoding="utf-8")
            os.replace(tmp, self.path)
        except OSError:
            pass  # losing a use count is not worth an error
