"""The browser on the left: categories and places, search, preview.

Plug-ins are listed as the background scan finds them (Plug-ins › Instruments /
Audio Effects); the footer shows the scan's progress, and hovering over
"Plug-ins" lists the files that could not be read."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QDir, QModelIndex, QSettings, Qt, QTimer, Signal
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QAbstractItemView,
    QFileDialog,
    QFileSystemModel,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QListView,
    QMenu,
    QSplitter,
    QStackedWidget,
    QTreeView,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from ... import theme
from ...audio.engine_bridge import AUDIO_EXTENSIONS, EngineBridge, is_audio_file
from ...model.editor import BUILTIN_CATEGORIES, BUILTIN_DEVICES
from .. import icons
from ..widgets import ToggleButton
from .browser_models import BrowserItem, ItemListModel
from .file_index import FileIndex, PluginIndex

ROLE_SCOPE = Qt.ItemDataRole.UserRole + 1
PLUGIN_CATEGORIES = ("Instruments", "Audio Effects")


def builtin_items(category: str | None = None) -> list[BrowserItem]:
    """Built-in devices, all or one category's."""
    return [BrowserItem(BUILTIN_DEVICES[kind][0], kind, "device", name)
            for name, kinds in BUILTIN_CATEGORIES.items() if category in (None, name) for kind in kinds]


def default_places() -> list[str]:
    music = Path.home() / "Music"
    return [str(music if music.is_dir() else Path.home())]


class BrowserPanel(QWidget):
    file_activated = Signal(str)  # double-click: add the file to the arrangement
    device_activated = Signal(str)  # double-click a built-in device: add it to the selected track
    plugin_activated = Signal(object)  # double-click a plug-in (a PluginRef): add it to the selected track
    status_message = Signal(str)

    def __init__(self, bridge: EngineBridge, parent: QWidget | None = None):
        super().__init__(parent)
        self.bridge = bridge
        settings = QSettings()
        stored = settings.value("browser/places")
        self.places: list[str] = [str(p) for p in stored] if isinstance(stored, list) and stored else default_places()
        self.index = FileIndex(self)
        self.index.updated.connect(self._refresh)
        self.plugin_index = PluginIndex(self)
        self.plugin_index.updated.connect(self._plugins_updated)
        self.plugin_index.progress.connect(self._scan_progress)
        self.plugin_index.status_message.connect(self.status_message)
        self._scan_text = ""

        self.search = QLineEdit()
        self.search.setPlaceholderText("Search  (Ctrl+F)")
        self.search.setClearButtonEnabled(True)
        self._search_timer = QTimer(self, singleShot=True, interval=150)
        self._search_timer.timeout.connect(self._refresh)
        self.search.textChanged.connect(self._search_timer.start)

        self.sidebar = QTreeWidget()
        self.sidebar.setHeaderHidden(True)
        self.sidebar.setRootIsDecorated(False)
        self.sidebar.setIndentation(8)
        self.sidebar.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.sidebar.customContextMenuRequested.connect(self._sidebar_menu)
        self.sidebar.currentItemChanged.connect(lambda *_: self._refresh())
        self.sidebar.itemClicked.connect(self._sidebar_clicked)

        self.list_model = ItemListModel(self)
        self.list_view = QListView()
        self.list_view.setModel(self.list_model)
        self.list_view.setUniformItemSizes(True)
        self._make_draggable(self.list_view)
        self.list_view.doubleClicked.connect(self._activate_list)
        self.list_view.selectionModel().currentChanged.connect(self._list_current_changed)

        self.fs_model = QFileSystemModel(self)
        self.fs_model.setFilter(QDir.Filter.AllDirs | QDir.Filter.Files | QDir.Filter.NoDotAndDotDot)
        self.fs_model.setNameFilters([f"*{ext}" for ext in AUDIO_EXTENSIONS])
        self.fs_model.setNameFilterDisables(False)
        self.tree_view = QTreeView()
        self.tree_view.setModel(self.fs_model)
        self.tree_view.setHeaderHidden(True)
        for column in (1, 2, 3):
            self.tree_view.hideColumn(column)
        self._make_draggable(self.tree_view)
        self.tree_view.doubleClicked.connect(self._activate_tree)
        self.tree_view.selectionModel().currentChanged.connect(self._tree_current_changed)

        self.content = QStackedWidget()
        self.content.addWidget(self.list_view)
        self.content.addWidget(self.tree_view)

        splitter = QSplitter(Qt.Orientation.Horizontal)
        splitter.addWidget(self.sidebar)
        splitter.addWidget(self.content)
        splitter.setSizes([110, 220])
        splitter.setChildrenCollapsible(False)

        self.preview = ToggleButton(icon=icons.headphones(), role="tool",
                                    tooltip="Preview files when selected")
        self.preview.setChecked(True)
        self.preview.toggled.connect(self._preview_toggled)
        self.status = QLabel()
        self.status.setStyleSheet(f"color: {theme.TEXT_DIM};")
        footer = QHBoxLayout()
        footer.setContentsMargins(6, 2, 6, 4)
        footer.addWidget(self.preview)
        footer.addWidget(self.status, 1)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 6, 0, 0)
        layout.setSpacing(4)
        search_row = QHBoxLayout()
        search_row.setContentsMargins(6, 0, 6, 0)
        search_row.addWidget(self.search)
        layout.addLayout(search_row)
        layout.addWidget(splitter, 1)
        layout.addLayout(footer)

        self._build_sidebar()
        self.index.rebuild(self.places)
        self.plugin_index.scan()

    @staticmethod
    def _make_draggable(view: QAbstractItemView) -> None:
        view.setDragEnabled(True)
        view.setDragDropMode(QAbstractItemView.DragDropMode.DragOnly)
        view.setSelectionMode(QAbstractItemView.SelectionMode.ExtendedSelection)
        view.setFocusPolicy(Qt.FocusPolicy.ClickFocus)

    # --- Sidebar -----------------------------------------------------------------

    def _build_sidebar(self, select: tuple | None = None) -> None:
        self.sidebar.blockSignals(True)
        self.sidebar.clear()

        def section(title: str) -> None:
            item = QTreeWidgetItem([title])
            item.setFlags(Qt.ItemFlag.NoItemFlags)
            item.setForeground(0, QColor(theme.TEXT_DIM))
            font = theme.ui_font(7.5, bold=True)
            item.setFont(0, font)
            self.sidebar.addTopLevelItem(item)

        def entry(title: str, scope: tuple, icon=None, tooltip: str = "") -> QTreeWidgetItem:
            item = QTreeWidgetItem([title])
            item.setData(0, ROLE_SCOPE, scope)
            if icon is not None:
                item.setIcon(0, icon)
            if tooltip:
                item.setToolTip(0, tooltip)
            self.sidebar.addTopLevelItem(item)
            return item

        section("CATEGORIES")
        first = entry("Samples", ("samples",), icons.waveform())
        builtin = entry("Built-in", ("builtin",), icons.plugin())
        for name in BUILTIN_CATEGORIES:
            child = QTreeWidgetItem([name])
            child.setData(0, ROLE_SCOPE, ("builtin", name))
            child.setIcon(0, icons.plugin())
            builtin.addChild(child)
        builtin.setExpanded(True)
        plugins = entry("Plug-ins", ("plugins",), icons.plugin())
        for name in PLUGIN_CATEGORIES:
            child = QTreeWidgetItem([name])
            child.setData(0, ROLE_SCOPE, ("plugins", name))
            child.setIcon(0, icons.plugin())
            plugins.addChild(child)
        plugins.setExpanded(True)
        self._plugins_entry = plugins
        self._update_plugins_tooltip()
        section("PLACES")
        for place in self.places:
            entry(Path(place).name or place, ("place", place), icons.folder(), place)
        add = entry("Add Folder…", ("add",))
        add.setForeground(0, QColor(theme.TEXT_DIM))

        target = first
        if select is not None:
            items = [self.sidebar.topLevelItem(i) for i in range(self.sidebar.topLevelItemCount())]
            items += [item.child(j) for item in list(items) for j in range(item.childCount())]
            for item in items:
                if tuple(item.data(0, ROLE_SCOPE) or ()) == tuple(select):
                    target = item
        self.sidebar.setCurrentItem(target)
        self.sidebar.blockSignals(False)
        self._refresh()

    def _scope(self) -> tuple:
        item = self.sidebar.currentItem()
        scope = item.data(0, ROLE_SCOPE) if item else None
        return tuple(scope) if scope and scope[0] != "add" else ("samples",)

    def _sidebar_clicked(self, item: QTreeWidgetItem) -> None:
        scope = item.data(0, ROLE_SCOPE)
        if scope and scope[0] == "add":
            QTimer.singleShot(0, self.add_place)  # not from inside the click handler

    def _sidebar_menu(self, pos) -> None:
        item = self.sidebar.itemAt(pos)
        scope = item.data(0, ROLE_SCOPE) if item else None
        menu = QMenu(self)
        if scope and scope[0] == "place":
            menu.addAction("Remove from Places", lambda: self.remove_place(scope[1]))
        if scope and scope[0] == "plugins":
            menu.addAction("Rescan Plug-ins", self.rescan_plugins)
            menu.addSeparator()
        menu.addAction("Add Folder…", self.add_place)
        menu.addAction("Rescan", lambda: self.index.rebuild(self.places))
        menu.exec(self.sidebar.viewport().mapToGlobal(pos))

    def add_place(self) -> None:
        folder = QFileDialog.getExistingDirectory(self, "Add Folder to Places", str(Path.home()))
        if folder and folder not in self.places:
            self.places.append(str(Path(folder)))
            self._save_places()
            self._build_sidebar(select=("place", str(Path(folder))))
            self.index.rebuild(self.places)
        else:
            self._build_sidebar(select=self._scope())

    def remove_place(self, place: str) -> None:
        if place in self.places:
            self.places.remove(place)
            self._save_places()
            self._build_sidebar()
            self.index.rebuild(self.places)

    def _save_places(self) -> None:
        QSettings().setValue("browser/places", self.places)

    # --- Plug-ins ------------------------------------------------------------------

    def rescan_plugins(self) -> None:
        """Read every plug-in file again (also those that failed before)."""
        self.plugin_index.scan(rescan=True)

    def _scan_progress(self, done: int, total: int, path: str) -> None:
        self._scan_text = f"Scanning plug-ins {done + 1}/{total}: {Path(path).stem}"
        if self._scope()[0] == "plugins":
            self.status.setText(self._scan_text)

    def _plugins_updated(self) -> None:
        if not self.plugin_index.scanning:
            self._scan_text = ""
            failures = len(self.plugin_index.failures)
            if failures:
                self.status_message.emit(f"{failures} plug-in file{'s' if failures != 1 else ''} could not be read "
                                         "(hover over Plug-ins in the browser for details).")
        self._update_plugins_tooltip()
        self._refresh()

    def _update_plugins_tooltip(self) -> None:
        failures = self.plugin_index.failures
        lines = ["VST3 plug-ins"]
        if failures:
            lines.append("")
            lines.append("Could not be read:")
            lines += [f"{Path(f.path).name}: {f.reason}" for f in failures[:30]]
            if len(failures) > 30:
                lines.append(f"...and {len(failures) - 30} more")
        self._plugins_entry.setToolTip(0, "\n".join(lines))

    def _plugin_items(self, category: str | None) -> list[BrowserItem]:
        items = self.plugin_index.items
        if category == "Instruments":
            return [i for i in items if i.plugin.instrument]
        if category == "Audio Effects":
            return [i for i in items if not i.plugin.instrument]
        return items

    # --- Content -------------------------------------------------------------------

    def _refresh(self) -> None:
        scope = self._scope()
        terms = self.search.text().lower().split()
        if scope[0] == "place" and not terms:
            root = scope[1]
            self.fs_model.setRootPath(root)
            self.tree_view.setRootIndex(self.fs_model.index(root))
            self.content.setCurrentWidget(self.tree_view)
            self.status.setText(root)
            return

        if scope[0] == "plugins":
            items = self._plugin_items(scope[1] if len(scope) > 1 else None)
        elif scope[0] == "builtin":
            items = builtin_items(scope[1] if len(scope) > 1 else None)
        elif scope[0] == "place":
            prefix = str(Path(scope[1])).lower().rstrip("\\/") + "\\"
            items = [i for i in self.index.audio if i.path.lower().startswith(prefix)]
        else:
            items = self.index.audio
        if terms:
            items = [i for i in items if i.matches(terms)]
        self.list_model.set_items(items)
        self.content.setCurrentWidget(self.list_view)
        if scope[0] == "plugins":
            failures = len(self.plugin_index.failures)
            if self.plugin_index.scanning:
                self.status.setText(self._scan_text or "Scanning plug-ins…")
            elif not self.plugin_index.plugins:
                self.status.setText("No VST3 plug-ins found")
            else:
                note = f", {failures} could not be read" if failures and not terms else ""
                self.status.setText(f"{len(items)} plug-in{'s' if len(items) != 1 else ''}{note}")
        elif self.index.indexing:
            self.status.setText("Indexing…")
        else:
            self.status.setText(f"{len(items)} item{'s' if len(items) != 1 else ''}")

    def _list_current_changed(self, current: QModelIndex, _previous) -> None:
        item = self.list_model.item(current)
        if item and item.kind == "audio":
            self._maybe_preview(item.path)

    def _tree_current_changed(self, current: QModelIndex, _previous) -> None:
        path = self.fs_model.filePath(current)
        if current.isValid() and not self.fs_model.isDir(current):
            self._maybe_preview(path)

    def _preview_toggled(self, enabled: bool) -> None:
        if not enabled:
            self.bridge.stop_preview()

    def _maybe_preview(self, path: str) -> None:
        if self.preview.isChecked() and is_audio_file(path):
            self.bridge.preview_file(path)

    def _activate_list(self, index: QModelIndex) -> None:
        item: BrowserItem | None = self.list_model.item(index)
        if item is None:
            return
        if item.kind == "audio":
            self.file_activated.emit(item.path)
        elif item.kind == "device":
            self.device_activated.emit(item.path)
        elif item.kind == "plugin" and item.plugin is not None:
            self.plugin_activated.emit(item.plugin)

    def _activate_tree(self, index: QModelIndex) -> None:
        if not self.fs_model.isDir(index):
            self.file_activated.emit(self.fs_model.filePath(index))

    def focus_search(self) -> None:
        self.search.setFocus()
        self.search.selectAll()

    def shutdown(self) -> None:
        self.index.wait()
        self.plugin_index.wait()
