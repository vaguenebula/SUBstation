from __future__ import annotations

import json
from dataclasses import dataclass

from PySide6.QtCore import QAbstractListModel, QMimeData, QModelIndex, Qt, QUrl

from .. import icons

PLUGIN_MIME = "application/x-gilstudio-plugin"


@dataclass(frozen=True)
class BrowserItem:
    name: str
    path: str
    kind: str  # "audio" or "plugin"
    detail: str = ""  # parent folder, or plugin format

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
            return icons.plugin() if item.kind == "plugin" else icons.waveform()
        return None

    def flags(self, index: QModelIndex) -> Qt.ItemFlag:
        base = super().flags(index)
        return base | Qt.ItemFlag.ItemIsDragEnabled if index.isValid() else base

    def mimeTypes(self) -> list[str]:
        return ["text/uri-list", PLUGIN_MIME]

    def mimeData(self, indexes) -> QMimeData:
        mime = QMimeData()
        items = [self.item(i) for i in indexes if self.item(i)]
        audio = [QUrl.fromLocalFile(i.path) for i in items if i.kind == "audio"]
        if audio:
            mime.setUrls(audio)
        plugins = [{"name": i.name, "format": i.detail, "path": i.path} for i in items if i.kind == "plugin"]
        if plugins:
            mime.setData(PLUGIN_MIME, json.dumps(plugins).encode())
        return mime
