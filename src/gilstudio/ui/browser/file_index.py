"""Background index of audio files under the browser's places, plus plug-ins."""

from __future__ import annotations

import os
from pathlib import Path

from PySide6.QtCore import QObject, QThread, Signal

from ...audio.engine_bridge import AUDIO_EXTENSIONS
from ...plugins.scanner import scan_plugins
from .browser_models import BrowserItem

MAX_FILES = 300_000
MAX_DEPTH = 16


def walk_audio(root: str) -> list[BrowserItem]:
    items: list[BrowserItem] = []
    stack = [(root, 0)]
    while stack and len(items) < MAX_FILES:
        folder, depth = stack.pop()
        try:
            entries = list(os.scandir(folder))
        except OSError:
            continue
        for entry in entries:
            name = entry.name
            if name.startswith((".", "$")):
                continue
            try:
                if entry.is_dir(follow_symlinks=False):
                    if depth < MAX_DEPTH:
                        stack.append((entry.path, depth + 1))
                elif name.lower().endswith(AUDIO_EXTENSIONS):
                    items.append(BrowserItem(name, entry.path, "audio", os.path.basename(folder)))
            except OSError:
                continue
    return items


class _IndexThread(QThread):
    done = Signal(list, list)

    def __init__(self, places: list[str], parent: QObject | None = None):
        super().__init__(parent)
        self.places = places

    def run(self) -> None:
        audio: dict[str, BrowserItem] = {}
        for place in self.places:
            for item in walk_audio(place):
                audio.setdefault(os.path.normcase(item.path), item)
        plugins = [BrowserItem(p.name, p.path, "plugin", p.format) for p in scan_plugins()]
        self.done.emit(sorted(audio.values(), key=lambda i: i.name.lower()), plugins)


class FileIndex(QObject):
    updated = Signal()

    def __init__(self, parent: QObject | None = None):
        super().__init__(parent)
        self.audio: list[BrowserItem] = []
        self.plugins: list[BrowserItem] = []
        self._thread: _IndexThread | None = None
        self._pending: list[str] | None = None

    @property
    def indexing(self) -> bool:
        return self._thread is not None

    def rebuild(self, places: list[str]) -> None:
        if self._thread is not None:
            self._pending = list(places)  # restart once the current scan finishes
            return
        self._thread = _IndexThread([p for p in places if Path(p).is_dir()], self)
        self._thread.done.connect(self._on_done)
        self._thread.finished.connect(self._on_finished)
        self._thread.start()
        self.updated.emit()

    def _on_done(self, audio: list, plugins: list) -> None:
        self.audio = audio
        self.plugins = plugins

    def _on_finished(self) -> None:
        self._thread.deleteLater()
        self._thread = None
        if self._pending is not None:
            places, self._pending = self._pending, None
            self.rebuild(places)
        else:
            self.updated.emit()

    def wait(self) -> None:
        if self._thread is not None:
            self._thread.wait()
