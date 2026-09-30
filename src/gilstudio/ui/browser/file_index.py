"""Background indexes for the browser: audio files under its places, and the
installed plug-ins (scanned in child processes; see plugins/scanner.py)."""

from __future__ import annotations

import os
from pathlib import Path

from PySide6.QtCore import QObject, QThread, Signal

from ...audio.engine_bridge import AUDIO_EXTENSIONS
from ...model.project import PluginRef
from ...plugins.scanner import PluginInfo, PluginScanner, ScanFailure
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


def plugin_ref(info: PluginInfo) -> PluginRef:
    return PluginRef(format=info.format, uid=info.uid, name=info.name, vendor=info.vendor, path=info.path,
                     instrument=info.instrument)


def plugin_item(info: PluginInfo) -> BrowserItem:
    kind = "Instrument" if info.instrument else "Audio Effect"
    tooltip = "\n".join(line for line in (f"{info.name} ({info.format} {kind})", info.vendor,
                                          info.category.replace("|", ", "), info.path) if line)
    return BrowserItem(info.name, info.path, "plugin", info.vendor, plugin_ref(info), tooltip)


class _IndexThread(QThread):
    done = Signal(list)

    def __init__(self, places: list[str], parent: QObject | None = None):
        super().__init__(parent)
        self.places = places

    def run(self) -> None:
        audio: dict[str, BrowserItem] = {}
        for place in self.places:
            for item in walk_audio(place):
                audio.setdefault(os.path.normcase(item.path), item)
        self.done.emit(sorted(audio.values(), key=lambda i: i.name.lower()))


class FileIndex(QObject):
    updated = Signal()

    def __init__(self, parent: QObject | None = None):
        super().__init__(parent)
        self.audio: list[BrowserItem] = []
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

    def _on_done(self, audio: list) -> None:
        self.audio = audio

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


class _PluginScanThread(QThread):
    progress = Signal(int, int, str)  # done, total, file being read
    done = Signal(list, list)  # PluginInfo, ScanFailure
    failed = Signal(str)

    def __init__(self, rescan: bool, parent: QObject | None = None):
        super().__init__(parent)
        self.rescan = rescan

    def run(self) -> None:
        try:
            result = PluginScanner().scan(rescan=self.rescan, progress=self.progress.emit,
                                          cancelled=self.isInterruptionRequested)
        except (OSError, RuntimeError) as exc:
            self.failed.emit(str(exc))
            return
        self.done.emit(result.plugins, result.failures)


class PluginIndex(QObject):
    """The installed plug-ins. Scanning reads only new or changed files, unless
    it is a rescan."""

    updated = Signal()  # plugins/failures changed, or scanning started or stopped
    progress = Signal(int, int, str)
    status_message = Signal(str)

    def __init__(self, parent: QObject | None = None):
        super().__init__(parent)
        self.plugins: list[PluginInfo] = []
        self.failures: list[ScanFailure] = []
        self.items: list[BrowserItem] = []
        self._thread: _PluginScanThread | None = None
        self._again: bool | None = None  # a scan asked for while one runs (rescan?)

    @property
    def scanning(self) -> bool:
        return self._thread is not None

    def scan(self, rescan: bool = False) -> None:
        if self._thread is not None:
            self._again = rescan or bool(self._again)
            return
        self._thread = _PluginScanThread(rescan, self)
        self._thread.progress.connect(self.progress)
        self._thread.done.connect(self._on_done)
        self._thread.failed.connect(self.status_message)
        self._thread.finished.connect(self._on_finished)
        self._thread.start()
        self.updated.emit()

    def _on_done(self, plugins: list, failures: list) -> None:
        self.plugins = plugins
        self.failures = failures
        self.items = [plugin_item(p) for p in plugins]

    def _on_finished(self) -> None:
        self._thread.deleteLater()
        self._thread = None
        if self._again is not None:
            rescan, self._again = self._again, None
            self.scan(rescan)
        else:
            self.updated.emit()

    def wait(self) -> None:
        if self._thread is not None:
            self._thread.requestInterruption()
            self._thread.wait()
