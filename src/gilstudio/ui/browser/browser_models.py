from __future__ import annotations

import json
from dataclasses import dataclass

from PySide6.QtCore import QAbstractListModel, QMimeData, QModelIndex, Qt, QUrl

from .. import icons

PLUGIN_MIME = "application/x-gilstudio-plugin"
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


@dataclass(frozen=True)
class BrowserItem:
    name: str
    path: str
    kind: str  # "audio", "plugin" or "device" (built-in; `path` is the device kind)
    detail: str = ""  # parent folder, plugin format, or device category

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
            return f"{item.name}   ({item.detail})" if item.kind == "plugin" else item.name
        if role == Qt.ItemDataRole.ToolTipRole:
            return item.path
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
        plugins = [{"name": i.name, "format": i.detail, "path": i.path} for i in items if i.kind == "plugin"]
        if plugins:
            mime.setData(PLUGIN_MIME, json.dumps(plugins).encode())
        devices = [i.path for i in items if i.kind == "device"]
        if devices:
            mime.setData(DEVICE_MIME, json.dumps(devices).encode())
        return mime
