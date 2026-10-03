from __future__ import annotations

import json
import os
from dataclasses import asdict, dataclass
from functools import cached_property
from pathlib import Path

from PySide6.QtCore import QAbstractListModel, QMimeData, QModelIndex, Qt, QUrl

from ...model.project import Device, PluginRef
from ...model.serialization import ProjectFileError, load_preset
from .. import icons

PLUGIN_MIME = "application/x-substation-plugin"  # JSON list of PluginRef fields
DEVICE_MIME = "application/x-substation-device"  # JSON list of built-in device kinds
PRESET_MIME = "application/x-substation-preset"  # JSON list of preset file paths


def preset_paths(mime) -> list[str]:
    """Presets (their files) dragged from the browser, if any."""
    if not mime.hasFormat(PRESET_MIME):
        return []
    try:
        paths = json.loads(bytes(mime.data(PRESET_MIME)).decode())
    except ValueError:
        return []
    return [p for p in paths if isinstance(p, str)] if isinstance(paths, list) else []


def read_presets(paths: list[str]) -> tuple[list[tuple[str, Device]], list[str]]:
    """The devices of preset files, new (fresh ids each time), with the presets'
    names; and why those that couldn't be read couldn't."""
    devices, errors = [], []
    for path in paths:
        try:
            devices.append((Path(path).stem, load_preset(Path(path))))
        except ProjectFileError as exc:
            errors.append(str(exc))
    return devices, errors


def device_kinds(mime) -> list[str]:
    """Built-in device kinds dragged from the browser, if any."""
    if not mime.hasFormat(DEVICE_MIME):
        return []
    try:
        kinds = json.loads(bytes(mime.data(DEVICE_MIME)).decode())
    except ValueError:
        return []
    return [k for k in kinds if isinstance(k, str)]


def plugin_refs(mime) -> list[PluginRef]:
    """Plug-ins dragged from the browser, if any."""
    if not mime.hasFormat(PLUGIN_MIME):
        return []
    try:
        items = json.loads(bytes(mime.data(PLUGIN_MIME)).decode())
        return [PluginRef(format=str(i["format"]), uid=str(i["uid"]), name=str(i["name"]),
                          vendor=str(i.get("vendor", "")), path=str(i.get("path", "")),
                          instrument=bool(i.get("instrument", False)))
                for i in items]
    except (ValueError, KeyError, TypeError):
        return []


def audio_key(path: str) -> str:
    return "audio:" + os.path.normcase(os.path.normpath(path))


@dataclass(frozen=True)
class BrowserItem:
    name: str
    path: str
    kind: str  # "audio", "plugin", "device" (built-in; `path` is the device kind) or "preset" (`path`: its file)
    detail: str = ""  # parent folder, plug-in vendor, device category, or the device a preset is for
    plugin: PluginRef | None = None
    tooltip: str = ""

    @cached_property
    def key(self) -> str:
        """Who the item is, for what the browser remembers about it (library.py)."""
        if self.kind == "plugin" and self.plugin is not None:
            return f"plugin:{self.plugin.format}:{self.plugin.uid}"
        return audio_key(self.path) if self.kind == "audio" else f"{self.kind}:{self.path}"


class _ListSource:
    def __init__(self, items: list[BrowserItem]):
        self._list = items
        self.total = len(items)

    def items(self, start: int, count: int) -> list[BrowserItem]:
        return self._list[start:start + count]


class ItemListModel(QAbstractListModel):
    """Flat list of browser items (search results, samples, plug-ins); draggable.

    It shows a source (a search's results) a page at a time: the first page at
    once, more as the view scrolls near the end (Qt's fetchMore). Views lay out
    every row they have, so a list of 200 000 files would cost the UI thread
    that much each time it changed; this way it costs a page."""

    PAGE = 256

    def __init__(self, library=None, parent=None):
        super().__init__(parent)
        self.library = library  # a Library, for how often each item was used
        self._items: list[BrowserItem] = []
        self._source = _ListSource([])

    @property
    def total(self) -> int:
        """Rows in the whole list, shown or not yet."""
        return self._source.total

    def set_items(self, items: list[BrowserItem]) -> None:
        self.set_source(_ListSource(items))

    def set_source(self, source) -> None:
        """Show a source: anything with `total` and `items(start, count)`."""
        self.beginResetModel()
        self._source = source
        self._items = source.items(0, min(self.PAGE, source.total))
        self.endResetModel()

    def ensure_rows(self, rows: int) -> None:
        """Have at least `rows` rows (or all there are)."""
        rows = min(rows, self._source.total)
        if rows <= len(self._items):
            return
        more = self._source.items(len(self._items), rows - len(self._items))
        if more:
            self.beginInsertRows(QModelIndex(), len(self._items), len(self._items) + len(more) - 1)
            self._items.extend(more)
            self.endInsertRows()

    def canFetchMore(self, parent: QModelIndex = QModelIndex()) -> bool:
        return not parent.isValid() and len(self._items) < self._source.total

    def fetchMore(self, parent: QModelIndex = QModelIndex()) -> None:
        if not parent.isValid():
            self.ensure_rows(len(self._items) + self.PAGE)

    def item(self, index: QModelIndex) -> BrowserItem | None:
        return self._items[index.row()] if index.isValid() and index.row() < len(self._items) else None

    def rowCount(self, parent: QModelIndex = QModelIndex()) -> int:
        return 0 if parent.isValid() else len(self._items)

    def data(self, index: QModelIndex, role: int = Qt.ItemDataRole.DisplayRole):
        item = self.item(index)
        if item is None:
            return None
        if role == Qt.ItemDataRole.DisplayRole:
            return f"{item.name}   ({item.detail})" if item.kind in ("plugin", "preset") and item.detail else item.name
        if role == Qt.ItemDataRole.ToolTipRole:
            uses = self.library.uses(item.key) if self.library is not None else 0
            used = f"\nUsed {uses} time{'s' if uses != 1 else ''}" if uses else ""
            return (item.tooltip or item.path) + used
        if role == Qt.ItemDataRole.DecorationRole:
            if item.kind == "preset":
                return icons.preset()
            return icons.waveform() if item.kind == "audio" else icons.plugin()
        return None

    def flags(self, index: QModelIndex) -> Qt.ItemFlag:
        base = super().flags(index)
        return base | Qt.ItemFlag.ItemIsDragEnabled if index.isValid() else base

    def mimeTypes(self) -> list[str]:
        return ["text/uri-list", PLUGIN_MIME, DEVICE_MIME, PRESET_MIME]

    def mimeData(self, indexes) -> QMimeData:
        mime = QMimeData()
        items = [self.item(i) for i in indexes if self.item(i)]
        audio = [QUrl.fromLocalFile(i.path) for i in items if i.kind == "audio"]
        if audio:
            mime.setUrls(audio)
        plugins = [asdict(i.plugin) for i in items if i.kind == "plugin" and i.plugin is not None]
        if plugins:
            mime.setData(PLUGIN_MIME, json.dumps(plugins).encode())
        devices = [i.path for i in items if i.kind == "device"]
        if devices:
            mime.setData(DEVICE_MIME, json.dumps(devices).encode())
        presets = [i.path for i in items if i.kind == "preset"]
        if presets:
            mime.setData(PRESET_MIME, json.dumps(presets).encode())
        return mime
