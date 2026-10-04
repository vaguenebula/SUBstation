"""The browser on the left: categories and places, search, preview.

"All" lists everything (built-in devices, plug-ins and samples) at once; Ctrl+F
searches there. Enter or Down in the search field selects the first result, and
Enter on a result adds it, as a double-click does. A preview stops when you click
anywhere outside the browser.

Lists are sorted by Rank (what you use most first) or Name; see search.py. An item
counts as used when it is added to the project from here, by double-click, Enter
or a drag that is dropped somewhere; library.py keeps the counts.

Plug-ins are listed as the background scan finds them (Plug-ins › Instruments /
Audio Effects); the footer shows the scan's progress, and hovering over
"Plug-ins" lists the files that could not be read.

Presets (saved with a device's save button) are listed by the device they are
for (Presets › the device's name); see preset_index.py. Dropped or
double-clicked, a preset adds a new device; dropped onto a device of its kind in
the device view, it loads into it. Right-click one to rename it, delete it (to
the recycle bin) or show it in its folder.

Every list is a search, run by the native backend on its own thread (see
file_index.py): the panel asks, and shows the results when they come, a page at
a time. A search asked for replaces the one running. When the index changes
(files found while scanning, or changed in a place), the list is searched again
and keeps its current item where it can."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

from PySide6.QtCore import (
    QDir,
    QEvent,
    QFile,
    QModelIndex,
    QObject,
    QPoint,
    QSettings,
    Qt,
    QTimer,
    QUrl,
    Signal,
)
from PySide6.QtGui import QColor, QDesktopServices, QDrag, QIcon
from PySide6.QtWidgets import (
    QAbstractItemView,
    QApplication,
    QComboBox,
    QFileDialog,
    QFileSystemModel,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QLineEdit,
    QListView,
    QMenu,
    QMessageBox,
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
from ...model.devices import BUILTIN_CATEGORIES, BUILTIN_DEVICES
from ...model.presets import rename_preset
from .. import icons
from ..widgets import ToggleButton
from .browser_models import BrowserItem, ItemListModel, audio_key
from .file_index import FileIndex, PluginIndex, SearchResult
from .library import Library
from .preset_index import PresetIndex
from .search import BUILTIN, PLUGINS, PRESETS, SORTS, plugin_tag, scope_query

ROLE_SCOPE = Qt.ItemDataRole.UserRole + 1
PLUGIN_CATEGORIES = ("Instruments", "Audio Effects")
KEEP_WITHIN = 5000  # rows: a list searched again keeps its current item if it is this near the top


def builtin_items(category: str | None = None) -> list[BrowserItem]:
    """Built-in devices, all or one category's."""
    return [BrowserItem(BUILTIN_DEVICES[kind][0], kind, "device", name)
            for name, kinds in BUILTIN_CATEGORIES.items() if category in (None, name) for kind in kinds]


def default_places() -> list[str]:
    music = Path.home() / "Music"
    return [str(music if music.is_dir() else Path.home())]


def _start_drag(view: QAbstractItemView, actions: Qt.DropAction) -> None:
    """QAbstractItemView.startDrag, but saying (`dropped`) which items a drop took."""
    indexes = [i for i in view.selectedIndexes()
               if i.column() == 0 and i.flags() & Qt.ItemFlag.ItemIsDragEnabled]
    mime = view.model().mimeData(indexes) if indexes else None
    if mime is None:
        return
    drag = QDrag(view)
    drag.setMimeData(mime)
    icon = indexes[0].data(Qt.ItemDataRole.DecorationRole)
    if isinstance(icon, QIcon):
        drag.setPixmap(icon.pixmap(24, 24))
    if drag.exec(actions, Qt.DropAction.CopyAction) != Qt.DropAction.IgnoreAction:
        view.dropped.emit(indexes)


class _ListView(QListView):
    dropped = Signal(list)  # QModelIndex: the dragged items, after a drop took them

    def startDrag(self, actions) -> None:
        _start_drag(self, actions)


class _TreeView(QTreeView):
    dropped = Signal(list)

    def startDrag(self, actions) -> None:
        _start_drag(self, actions)


class BrowserPanel(QWidget):
    file_activated = Signal(str)  # double-click: add the file to the arrangement
    device_activated = Signal(str)  # double-click a built-in device: add it to the selected track
    plugin_activated = Signal(object)  # double-click a plug-in (a PluginRef): add it to the selected track
    preset_activated = Signal(str)  # double-click a preset (its file): add its device to the selected track
    status_message = Signal(str)

    def __init__(self, bridge: EngineBridge, parent: QWidget | None = None):
        super().__init__(parent)
        self.bridge = bridge
        settings = QSettings()
        stored = settings.value("browser/places")
        self.places: list[str] = [str(p) for p in stored] if isinstance(stored, list) and stored else default_places()
        self.library = Library()
        self.index = FileIndex(self)
        self.index.updated.connect(self._index_updated)
        self.index.results.connect(self._show_results)
        self.index.set_items(BUILTIN, [(item, item.detail) for item in builtin_items()])
        self.index.set_usage(self.library)
        self.plugin_index = PluginIndex(self)
        self.plugin_index.updated.connect(self._plugins_updated)
        self.plugin_index.progress.connect(self._scan_progress)
        self.plugin_index.status_message.connect(self.status_message)
        self.preset_index = PresetIndex(self)
        self.preset_index.updated.connect(self._presets_updated)
        self.index.set_items(PRESETS, [(item, item.detail) for item in self.preset_index.items])
        self._scan_text = ""
        self._searching = False  # results asked for and not shown yet
        self._select_first = False  # when they come (Enter was pressed before)
        self._keep: tuple | None = None  # where the list was, to go back to when they come
        self._restoring = False
        self._previewing = False  # a preview was started and not stopped since

        self.search = QLineEdit()
        self.search.setPlaceholderText("Search  (Ctrl+F)")
        self.search.setClearButtonEnabled(True)
        # Searching doesn't hold up the UI, so there is no need to wait for typing
        # to pause; this only merges changes that come together.
        self._search_timer = QTimer(self, singleShot=True, interval=0)
        self._search_timer.timeout.connect(self._refresh)
        self.search.textChanged.connect(self._search_timer.start)
        self.search.returnPressed.connect(self._select_first_result)
        self.sort = QComboBox()
        for sort, label in SORTS.items():
            self.sort.addItem(label, sort)
        self.sort.setToolTip("Sort the list: Rank puts what you use most first")
        stored_sort = settings.value("browser/sort")
        self.sort.setCurrentIndex(max(0, self.sort.findData(stored_sort)))
        self.sort.currentIndexChanged.connect(self._sort_changed)

        self.sidebar = QTreeWidget()
        self.sidebar.setHeaderHidden(True)
        self.sidebar.setRootIsDecorated(False)
        self.sidebar.setIndentation(8)
        self.sidebar.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.sidebar.customContextMenuRequested.connect(self._sidebar_menu)
        self.sidebar.currentItemChanged.connect(lambda *_: self._refresh())
        self.sidebar.itemClicked.connect(self._sidebar_clicked)

        self.list_model = ItemListModel(self.library, self)
        self.list_view = _ListView()
        self.list_view.setModel(self.list_model)
        self.list_view.setUniformItemSizes(True)
        self._make_draggable(self.list_view)
        self.list_view.doubleClicked.connect(self._activate_list)
        self.list_view.dropped.connect(self._used_list)
        self.list_view.selectionModel().currentChanged.connect(self._list_current_changed)
        self.list_view.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        self.list_view.customContextMenuRequested.connect(self._list_menu)

        self.fs_model = QFileSystemModel(self)
        self.fs_model.setFilter(QDir.Filter.AllDirs | QDir.Filter.Files | QDir.Filter.NoDotAndDotDot)
        self.fs_model.setNameFilters([f"*{ext}" for ext in AUDIO_EXTENSIONS])
        self.fs_model.setNameFilterDisables(False)
        self.tree_view = _TreeView()
        self.tree_view.setModel(self.fs_model)
        self.tree_view.setHeaderHidden(True)
        for column in (1, 2, 3):
            self.tree_view.hideColumn(column)
        self._make_draggable(self.tree_view)
        self.tree_view.doubleClicked.connect(self._activate_tree)
        self.tree_view.dropped.connect(self._used_tree)
        self.tree_view.selectionModel().currentChanged.connect(self._tree_current_changed)

        for view in (self.list_view, self.tree_view, self.search):
            view.installEventFilter(self)
        QApplication.instance().installEventFilter(self)  # clicks elsewhere stop the preview

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
        search_row.addWidget(self.search, 1)
        search_row.addWidget(self.sort)
        layout.addLayout(search_row)
        layout.addWidget(splitter, 1)
        layout.addLayout(footer)

        self.index.set_places(self.places)
        self._build_sidebar()
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
        entry("All", ("all",), icons.search())
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
        presets = entry("Presets", ("presets",), icons.preset(),
                        f"Saved with a device's save button\n{self.preset_index.root}")
        for name in self.preset_index.groups:
            child = QTreeWidgetItem([name])
            child.setData(0, ROLE_SCOPE, ("presets", name))
            child.setIcon(0, icons.preset())
            presets.addChild(child)
        presets.setExpanded(True)
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
        if scope and scope[0] == "presets":
            menu.addAction("Show in Folder", lambda: self.show_in_folder(self._preset_folder(scope)))
            menu.addSeparator()
        menu.addAction("Add Folder…", self.add_place)
        menu.addAction("Rescan", lambda: self.index.rebuild(self.places))
        menu.exec(self.sidebar.viewport().mapToGlobal(pos))

    def add_place(self) -> None:
        folder = QFileDialog.getExistingDirectory(self, "Add Folder to Places", str(Path.home()))
        if folder and folder not in self.places:
            self.places.append(str(Path(folder)))
            self._save_places()
            self.index.set_places(self.places)  # scans the new one only
            self._build_sidebar(select=("place", str(Path(folder))))
        else:
            self._build_sidebar(select=self._scope())

    def remove_place(self, place: str) -> None:
        if place in self.places:
            self.places.remove(place)
            self._save_places()
            self.index.set_places(self.places)
            self._build_sidebar()

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
        self.index.set_items(PLUGINS, [(item, plugin_tag(item)) for item in self.plugin_index.items])
        self._update_plugins_tooltip()
        self._refresh(keep=True)

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

    # --- Presets -------------------------------------------------------------------

    def presets_changed(self) -> None:
        """The library's presets changed (one was saved): list them again."""
        self.preset_index.rescan()

    def _presets_updated(self) -> None:
        self.index.set_items(PRESETS, [(item, item.detail) for item in self.preset_index.items])
        presets = self._sidebar_entry(("presets",))
        shown = [presets.child(i).text(0) for i in range(presets.childCount())] if presets is not None else []
        if shown != self.preset_index.groups:  # the sidebar lists the groups: made again
            scope = self._scope()
            if scope[0] == "presets" and len(scope) > 1 and scope[1] not in self.preset_index.groups:
                scope = ("presets",)  # (its group went)
            self._build_sidebar(select=scope)  # (it searches again)
        else:
            self._refresh(keep=True)

    def _sidebar_entry(self, scope: tuple) -> QTreeWidgetItem | None:
        items = [self.sidebar.topLevelItem(i) for i in range(self.sidebar.topLevelItemCount())]
        items += [item.child(j) for item in list(items) for j in range(item.childCount())]
        return next((item for item in items if tuple(item.data(0, ROLE_SCOPE) or ()) == tuple(scope)), None)

    def _preset_folder(self, scope: tuple) -> str:
        """The library folder a Presets entry lists (the library itself for the section)."""
        if len(scope) > 1:
            item = next((i for i in self.preset_index.items if i.detail == scope[1]), None)
            if item is not None:
                return str(Path(item.path).parent)
        return str(self.preset_index.root)

    def _list_menu(self, pos) -> None:
        item = self.list_model.item(self.list_view.indexAt(pos))
        if item is None or item.kind != "preset":
            return
        menu = QMenu(self)
        menu.addAction("Rename…", lambda: self.rename_preset(item.path))
        menu.addAction("Delete", lambda: self.delete_preset(item.path))
        menu.addSeparator()
        menu.addAction("Show in Folder", lambda: self.show_in_folder(item.path))
        menu.exec(self.list_view.viewport().mapToGlobal(pos))

    def rename_preset(self, path: str, name: str | None = None) -> str | None:
        """Give a preset another name (asked for, if not given); its new path (None: not renamed)."""
        if name is None:
            name, ok = QInputDialog.getText(self, "Rename Preset", "Name:", text=Path(path).stem)
            if not ok:
                return None
        name = name.strip()
        if not name or name == Path(path).stem:
            return None
        try:
            new = rename_preset(Path(path), name)
        except (OSError, ValueError) as exc:
            self.status_message.emit(f"Could not rename the preset: {exc}")
            return None
        self.preset_index.rescan()
        return str(new)

    def delete_preset(self, path: str, confirm: bool = True) -> bool:
        """Move a preset to the recycle bin (after asking, if `confirm`)."""
        if confirm and QMessageBox.question(
                self, "Delete Preset", f"Move the preset \u201c{Path(path).stem}\u201d to the Recycle Bin?",
                QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No) != QMessageBox.StandardButton.Yes:
            return False
        if not QFile.moveToTrash(path):
            self.status_message.emit(f"Could not delete the preset {Path(path).stem}.")
            return False
        self.preset_index.rescan()
        return True

    @staticmethod
    def show_in_folder(path: str) -> None:
        """Explorer at a file (selected) or a folder."""
        if sys.platform == "win32" and os.path.isfile(path):
            subprocess.Popen(["explorer", f"/select,{os.path.normpath(path)}"])
        else:
            folder = path if os.path.isdir(path) else os.path.dirname(path)
            QDesktopServices.openUrl(QUrl.fromLocalFile(folder))

    # --- Content -------------------------------------------------------------------

    @property
    def searching(self) -> bool:
        """Results are on their way."""
        return self._searching or self._search_timer.isActive()

    def _showing_tree(self) -> bool:
        scope = self._scope()
        return scope[0] == "place" and not self.search.text().split()

    def _refresh(self, keep: bool = False) -> None:
        """Search for what the list should show. `keep`: the list is searched
        again for a change elsewhere (the index, the plug-ins), so it keeps its
        current item and scroll position."""
        self._search_timer.stop()
        scope = self._scope()
        if self._showing_tree():
            self._searching = False
            self._select_first = False
            root = scope[1]
            self.fs_model.setRootPath(root)
            self.tree_view.setRootIndex(self.fs_model.index(root))
            self.content.setCurrentWidget(self.tree_view)
            self.status.setText(root)
            return
        groups, tag, prefix = scope_query(scope)
        if keep and self.content.currentWidget() is self.list_view:
            if self._keep is None:  # (a search replacing one that was to keep it keeps that)
                self._keep = self._list_position()
        else:
            self._keep = None
        self._searching = True
        self.index.search(self.search.text(), self.sort.currentData(), self.library.clock(), groups, tag, prefix)

    def _index_updated(self) -> None:
        if self._showing_tree():
            return  # the tree shows the folder itself
        self._refresh(keep=True)

    def _show_results(self, result: SearchResult) -> None:
        if not self._searching:
            return  # the tree was shown meanwhile
        self._searching = False
        keep, self._keep = self._keep, None
        self._restoring = True
        try:
            self.list_model.set_source(result)
            self.content.setCurrentWidget(self.list_view)
            if keep is not None:
                self._restore_position(keep, result)
        finally:
            self._restoring = False
        self._update_status()
        if self._select_first:
            self._select_first = False
            self._select_first_row()

    def _list_position(self) -> tuple:
        """(current item, its row on screen, first row on screen)."""
        view = self.list_view
        top = view.indexAt(QPoint(1, 1)).row()
        current = view.currentIndex()
        item = self.list_model.item(current)
        return item, (current.row() - top if item is not None and top >= 0 else 0), max(top, 0)

    def _restore_position(self, keep: tuple, result: SearchResult) -> None:
        item, offset, top = keep
        row = result.find(item) if item is not None else -1
        if 0 <= row < KEEP_WITHIN:
            top = max(0, row - offset)
            self.list_model.ensure_rows(row + 1)
            self.list_view.setCurrentIndex(self.list_model.index(row))
        if top > 0:
            self.list_model.ensure_rows(top + 1)
            self.list_view.scrollTo(self.list_model.index(min(top, self.list_model.rowCount() - 1)),
                                    QAbstractItemView.ScrollHint.PositionAtTop)

    def _update_status(self) -> None:
        scope = self._scope()
        count = self.list_model.total
        items = f"{count} item{'s' if count != 1 else ''}"
        if scope[0] == "presets" and not self.preset_index.items:
            self.status.setText("No presets yet: save one with a device's save button")
        elif scope[0] == "presets":
            self.status.setText(f"{count} preset{'s' if count != 1 else ''}")
        elif scope[0] == "plugins":
            failures = len(self.plugin_index.failures)
            if self.plugin_index.scanning:
                self.status.setText(self._scan_text or "Scanning plug-ins…")
            elif not self.plugin_index.plugins:
                self.status.setText("No VST3 plug-ins found")
            else:
                note = f", {failures} could not be read" if failures and not self.search.text().split() else ""
                self.status.setText(f"{count} plug-in{'s' if count != 1 else ''}{note}")
        elif scope[0] == "all" and (self.index.indexing or self.plugin_index.scanning):
            busy = "Indexing" if self.index.indexing else "Scanning plug-ins"
            self.status.setText(f"{items} ({busy}…)")
        elif scope[0] != "builtin" and self.index.indexing:
            self.status.setText(f"{items} (Indexing…)")
        else:
            self.status.setText(items)

    def _list_current_changed(self, current: QModelIndex, _previous) -> None:
        item = self.list_model.item(current)
        if item and item.kind == "audio" and not self._restoring:
            self._maybe_preview(item.path)

    def _tree_current_changed(self, current: QModelIndex, _previous) -> None:
        path = self.fs_model.filePath(current)
        if current.isValid() and not self.fs_model.isDir(current):
            self._maybe_preview(path)

    def _preview_toggled(self, enabled: bool) -> None:
        if not enabled:
            self.stop_preview()

    def _maybe_preview(self, path: str) -> None:
        if self.preview.isChecked() and is_audio_file(path):
            self._previewing = True
            self.bridge.preview_file(path)

    def stop_preview(self) -> None:
        self._previewing = False
        self.bridge.stop_preview()

    def _sort_changed(self) -> None:
        QSettings().setValue("browser/sort", self.sort.currentData())
        self._refresh()

    # Uses are counted but the list is not re-sorted then: the selection stays put.

    def _used_list(self, indexes: list[QModelIndex]) -> None:
        self._record_use([item.key for item in map(self.list_model.item, indexes) if item])

    def _used_tree(self, indexes: list[QModelIndex]) -> None:
        self._record_use([audio_key(self.fs_model.filePath(i)) for i in indexes if not self.fs_model.isDir(i)])

    def _record_use(self, keys: list[str]) -> None:
        if keys:
            self.library.record_use(keys)
            self.index.set_usage(self.library)

    def _activate_list(self, index: QModelIndex) -> None:
        item: BrowserItem | None = self.list_model.item(index)
        if item is None:
            return
        self._used_list([index])
        if item.kind == "audio":
            self.file_activated.emit(item.path)
        elif item.kind == "device":
            self.device_activated.emit(item.path)
        elif item.kind == "plugin" and item.plugin is not None:
            self.plugin_activated.emit(item.plugin)
        elif item.kind == "preset":
            self.preset_activated.emit(item.path)

    def _activate_tree(self, index: QModelIndex) -> None:
        if not self.fs_model.isDir(index):
            self._used_tree([index])
            self.file_activated.emit(self.fs_model.filePath(index))

    def _select_first_result(self) -> None:
        if self._search_timer.isActive():
            self._refresh()
        if self._searching:  # Enter typed before the results came: select when they do
            self._select_first = True
            return
        self._select_first_row()

    def _select_first_row(self) -> None:
        view = self.content.currentWidget()
        first = view.model().index(0, 0, view.rootIndex())
        if first.isValid():
            view.setCurrentIndex(first)
            view.setFocus()

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:
        kind = event.type()
        if (kind == QEvent.Type.MouseButtonPress and self._previewing and watched.isWidgetType()
                and not self._contains(watched)):
            self.stop_preview()  # a click outside the browser
            return False
        if kind == QEvent.Type.KeyPress and watched is self.search and event.key() == Qt.Key.Key_Down:
            self._select_first_result()
            return True
        if (kind == QEvent.Type.KeyPress and watched in (self.list_view, self.tree_view)
                and event.key() in (Qt.Key.Key_Return, Qt.Key.Key_Enter)):
            index = watched.currentIndex()
            if index.isValid():
                (self._activate_list if watched is self.list_view else self._activate_tree)(index)
            return True
        return super().eventFilter(watched, event)

    def _contains(self, widget: QWidget) -> bool:
        """Whether `widget` is part of the browser, its popups (menus, the sort list) included."""
        while widget is not None:
            if widget is self:
                return True
            widget = widget.parentWidget()
        return False

    def rename_current(self) -> bool:
        """Ctrl+R while the list has focus: rename the preset there (False: none is)."""
        item = self.list_model.item(self.list_view.currentIndex()) if self.list_view.hasFocus() else None
        if item is None or item.kind != "preset":
            return False
        self.rename_preset(item.path)
        return True

    def focus_search(self) -> None:
        """Search everything: switch to "All" and focus the search field."""
        if self._scope() != ("all",):
            self._build_sidebar(select=("all",))
        self.search.setFocus()
        self.search.selectAll()

    def shutdown(self) -> None:
        QApplication.instance().removeEventFilter(self)
        self.index.close()
        self.plugin_index.wait()
