from __future__ import annotations

import json
from dataclasses import asdict, dataclass

from PySide6.QtCore import QAbstractListModel, QMimeData, QModelIndex, Qt, QUrl

from ...model.project import PluginRef
from .. import icons

PLUGIN_MIME = "application/x-gilstudio-plugin"  # JSON list of PluginRef fields
DEVICE_MIME = "application/x-gilstudio-device"  # JSON list of built-in device kinds


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


@dataclass(frozen=True)
class BrowserItem:
    name: str
    path: str
    kind: str  # "audio", "plugin" or "device" (built-in; `path` is the device kind)
    detail: str = ""  # parent folder, plug-in vendor, or device category
    plugin: PluginRef | None = None
    tooltip: str = ""

    def matches(self, terms: list[str]) -> bool:
        haystack = f"{self.name} {self.detail}".lower()
        return all(term in haystack for term in terms)


class ItemListModel(QAbstractListModel):
    """Flat list of browser items (search results, samples, plug-ins); draggable."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._items: list[BrowserItem] = []

    def set_items(self, items: list[BrowserItem]) -> None:
        self.beginResetModel()
        self._items = items
        self.endResetModel()

    def item(self, index: QModelIndex) -> BrowserItem | None:
        return self._items[index.row()] if index.isValid() and index.row() < len(self._items) else None

    def rowCount(self, parent: QModelIndex = QModelIndex()) -> int:
        return 0 if parent.isValid() else len(self._items)

    def data(self, index: QModelIndex, role: int = Qt.ItemDataRole.DisplayRole):
        item = self.item(index)
        if item is None:
            return None
        if role == Qt.ItemDataRole.DisplayRole:
            return f"{item.name}   ({item.detail})" if item.kind == "plugin" and item.detail else item.name
        if role == Qt.ItemDataRole.ToolTipRole:
            return item.tooltip or item.path
        if role == Qt.ItemDataRole.DecorationRole:
            return icons.waveform() if item.kind == "audio" else icons.plugin()
        return None

    def flags(self, index: QModelIndex) -> Qt.ItemFlag:
        base = super().flags(index)
        return base | Qt.ItemFlag.ItemIsDragEnabled if index.isValid() else base

    def mimeTypes(self) -> list[str]:
        return ["text/uri-list", PLUGIN_MIME, DEVICE_MIME]

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
        return mime
