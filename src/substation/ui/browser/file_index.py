"""Background indexes for the browser: audio files under its places, and the
installed plug-ins (scanned in child processes; see plugins/scanner.py).

The files are indexed, and all of the browser's lists searched, by a native
backend (substation._browser, from browser/src), on threads of its own:

- One keeps an index of the folders under the places, at background CPU and I/O
  priority. It is saved (browser-index.bin next to the plug-in cache), so the
  next start shows it at once and only lists folders that changed; while running
  it watches the places and lists again what changed in them.
- Another runs searches over snapshots of it. A new search stops the one
  running, and only the latest one's results are handed out.

Neither calls into Python. When there are results, or the index changed, the
backend sets a Win32 event; `FileIndex` takes them on the UI thread."""

from __future__ import annotations

import math
import os
from pathlib import Path

import shiboken6
from PySide6.QtCore import QObject, QThread, QTimer, Signal

from ... import _browser
from ...audio.engine_bridge import AUDIO_EXTENSIONS
from ...model.project import PluginRef
from ...plugins.scanner import PluginInfo, PluginScanner, ScanFailure
from ...plugins.settings import plugin_folders
from .browser_models import BrowserItem
from .library import HALF_LIFE_DAYS, Library

try:
    from PySide6.QtCore import QWinEventNotifier
except ImportError:  # not Windows
    QWinEventNotifier = None

MAX_FILES = 300_000  # per place
MAX_DEPTH = 16
KINDS = ("audio", "plugin", "device", "preset")  # the backend's item kinds, by number


def index_path() -> Path:
    override = os.environ.get("SUBSTATION_BROWSER_INDEX")
    if override:
        return Path(override)
    base = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local"))
    return base / "SUBstation" / "browser-index.bin"


def place_spec(root: str) -> tuple[str, str, str]:
    """A place as the backend takes it: the root, its key (as in BrowserItem.key)
    and the detail its own files show."""
    return root, os.path.normcase(os.path.normpath(root)), os.path.basename(root)


def usage_records(library: Library) -> list[tuple[str, float, float]]:
    """The library's use counts as (key, score, last used or NaN)."""
    records = []
    for key, record in library.records.items():
        try:
            score = float(record["score"]) if record.get("score") else 0.0
            last = record.get("last_used")
            last = math.nan if last is None else float(last)
        except (TypeError, ValueError):
            continue
        records.append((key, score, last))
    return records


def plugin_ref(info: PluginInfo) -> PluginRef:
    return PluginRef(format=info.format, uid=info.uid, name=info.name, vendor=info.vendor, path=info.path,
                     instrument=info.instrument)


def plugin_item(info: PluginInfo) -> BrowserItem:
    kind = "Instrument" if info.instrument else "Audio Effect"
    tooltip = "\n".join(line for line in (f"{info.name} ({info.format} {kind})", info.vendor,
                                          info.category.replace("|", ", "), info.path) if line)
    return BrowserItem(info.name, info.path, "plugin", info.vendor, plugin_ref(info), tooltip)


class SearchResult:
    """A search's results, read a page at a time (see ItemListModel)."""

    def __init__(self, native, items: dict[str, BrowserItem]):
        self.native = native
        self._items = items  # the other items by key, as they were when the search ran

    @property
    def total(self) -> int:
        return self.native.total

    def items(self, start: int, count: int) -> list[BrowserItem]:
        out = []
        for kind, name, path, detail, key in self.native.rows(start, count):
            if not key:
                out.append(BrowserItem(name, path, "audio", detail))
            else:
                out.append(self._items.get(key) or BrowserItem(name, path, KINDS[kind], detail))
        return out

    def find(self, item: BrowserItem) -> int:
        """Row of the item, or -1."""
        if item.kind == "audio" and item.key not in self._items:
            return self.native.find(0, item.path)
        return self.native.find(KINDS.index(item.kind), item.key)


class FileIndex(QObject):
    """The native backend, on the UI thread's side."""

    updated = Signal()  # files were found or went, or indexing started or stopped
    results = Signal(object)  # SearchResult of the latest search()

    def __init__(self, parent: QObject | None = None):
        super().__init__(parent)
        self.native = _browser.Browser(str(index_path()), list(AUDIO_EXTENSIONS), MAX_FILES, MAX_DEPTH)
        self._items: dict[str, BrowserItem] = {}  # the other items, by key
        self._groups: dict[int, dict[str, BrowserItem]] = {}
        self._state = (True, 0)  # indexing, index version
        # The backend's event wakes the UI thread (without converting its handle
        # for Python: QWinEventNotifier's signal goes straight to a timer's slot).
        self._take_timer = QTimer(self, singleShot=True, interval=0)
        self._take_timer.timeout.connect(self._take)
        self._notifier = None
        if QWinEventNotifier is not None:
            self._notifier = QWinEventNotifier(shiboken6.VoidPtr(self.native.event_handle), self)
            self._notifier.activated.connect(self._take_timer.start)
        else:
            self._take_timer.setSingleShot(False)
            self._take_timer.setInterval(15)
            self._take_timer.start()

    # --- Index ------------------------------------------------------------------------

    @property
    def indexing(self) -> bool:
        return self.native.indexing

    @property
    def file_count(self) -> int:
        return self.native.file_count

    def set_places(self, places: list[str]) -> None:
        """Index these places: new ones are scanned, gone ones dropped, the rest kept."""
        self.native.set_places([place_spec(p) for p in places])

    def rebuild(self, places: list[str]) -> None:
        """Index these places, listing every folder again."""
        self.set_places(places)
        self.native.rescan()

    # --- What else is searched ------------------------------------------------------------

    def set_items(self, group: int, items: list[tuple[BrowserItem, str]]) -> None:
        """Other items to list (built-in devices, plug-ins), each with a tag to filter by."""
        self._groups[group] = {item.key: item for item, _ in items}
        self._items = {key: item for g in self._groups.values() for key, item in g.items()}
        self.native.set_external(group, [(KINDS.index(i.kind), i.name, i.path, i.detail, i.key, tag)
                                          for i, tag in items])

    def set_usage(self, library: Library) -> None:
        self.native.set_usage(usage_records(library), HALF_LIFE_DAYS)

    # --- Searching ------------------------------------------------------------------------

    def search(self, text: str, sort: str, now: float, groups: list[int], tag: str = "",
               place_prefix: str = "") -> int:
        """Start a search (stopping any that runs); its results come as `results`."""
        return self.native.search(text, sort, now, groups, tag, place_prefix)

    def _take(self) -> None:
        indexing, version, _files, result = self.native.take()
        if result is not None:
            self.results.emit(SearchResult(result, self._items))
        if (indexing, version) != self._state:
            self._state = (indexing, version)
            self.updated.emit()

    def wait_idle(self, timeout: float = 10.0) -> bool:
        """Block until the index settled and no search runs (tests, benchmarks)."""
        return self.native.wait_idle(timeout)

    def close(self) -> None:
        """Stop the backend's threads (saving the index)."""
        if self._notifier is not None:
            self._notifier.setEnabled(False)
        self._take_timer.stop()
        self.native.close()


class _PluginScanThread(QThread):
    progress = Signal(int, int, str)  # done, total, file being read
    done = Signal(list, list)  # PluginInfo, ScanFailure
    failed = Signal(str)

    def __init__(self, rescan: bool, folders: list[Path], parent: QObject | None = None):
        super().__init__(parent)
        self.rescan = rescan
        self.folders = folders

    def run(self) -> None:
        try:
            result = PluginScanner(folders=self.folders).scan(rescan=self.rescan, progress=self.progress.emit,
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
        self._thread = _PluginScanThread(rescan, plugin_folders(), self)
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
