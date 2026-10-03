"""The presets in the user's library (model/presets.py), for the browser.

The library is small (a file per preset), so it is listed on the UI thread:
at start, when the app saves, renames or deletes a preset (`rescan`), and when
its folders change on disk (a watcher on the library and its folders)."""

from __future__ import annotations

import os
from pathlib import Path

from PySide6.QtCore import QFileSystemWatcher, QObject, QTimer, Signal

from ...model.presets import PresetFile, library_dir, list_presets
from .browser_models import BrowserItem

UNGROUPED = "Other"  # the browser's name for presets straight in the library folder


def preset_item(preset: PresetFile) -> BrowserItem:
    group = preset.group or UNGROUPED
    return BrowserItem(preset.name, str(preset.path), "preset", group, tooltip=f"{preset.name}\n{group} preset\n"
                                                                                f"{preset.path}")


class PresetIndex(QObject):
    updated = Signal()  # the presets changed

    def __init__(self, parent: QObject | None = None, root: Path | None = None):
        super().__init__(parent)
        self.root = library_dir() if root is None else Path(root)
        self.items: list[BrowserItem] = []
        self.groups: list[str] = []  # in the order the browser lists them
        self._watcher = QFileSystemWatcher(self)
        self._watcher.directoryChanged.connect(lambda _path: self._timer.start())
        self._timer = QTimer(self, singleShot=True, interval=200)  # a burst of changes: listed once
        self._timer.timeout.connect(self.rescan)
        self.rescan()

    def rescan(self) -> None:
        """List the library again (updated if anything changed)."""
        self._timer.stop()
        items = [preset_item(p) for p in list_presets(self.root)]
        self._watch()
        if items != self.items:
            self.items = items
            self.groups = list(dict.fromkeys(item.detail for item in items))
            self.updated.emit()

    def _watch(self) -> None:
        """Watch the library and its folders (those there now)."""
        folders = []
        if self.root.is_dir():
            folders.append(str(self.root))
            try:
                folders += [e.path for e in os.scandir(self.root) if e.is_dir() and not e.name.startswith(".")]
            except OSError:
                pass
        watched = set(self._watcher.directories())
        gone = [f for f in watched if f not in folders]
        if gone:
            self._watcher.removePaths(gone)
        new = [f for f in folders if f not in watched]
        if new:
            self._watcher.addPaths(new)
