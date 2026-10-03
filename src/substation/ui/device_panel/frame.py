"""What every device in the device view shares (_DeviceFrame): the frame, the
title bar (fold, on/off, name, parameter pages, editor, sidechain, save), the
parameters' pages with their automation marks and menus, selecting and
dragging it, its right-click menu, saving it as a preset (or the default
preset), and its sidechain menu."""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QEvent, QObject, QPoint, QSize, Qt, Signal
from PySide6.QtGui import (
    QContextMenuEvent,
    QFontMetrics,
    QIcon,
    QMouseEvent,
    QPainter,
    QPalette,
)
from PySide6.QtWidgets import (
    QApplication,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QInputDialog,
    QLabel,
    QMenu,
    QMessageBox,
    QPushButton,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

from ... import theme
from ...audio.engine_bridge import EngineBridge
from ...model.automation import device_key
from ...model.devices import device_ids_of, device_is_instrument, device_name
from ...model.editor import ProjectEditor
from ...model.presets import (
    clear_default,
    has_default,
    preset_path,
    save_default,
    save_to_library,
)
from ...model.project import (
    MACRO_COUNT,
    POST_FADER,
    PRE_FADER,
    PRE_FX,
    Device,
    Sidechain,
    chain_devices,
    container_of,
)
from .. import icons
from ..arrangement.mixer_controls import automation_state
from ..widgets import Knob, ToggleButton

PARAM_COLUMNS = 2
PARAMS_PER_PAGE = 4  # a 2×2 grid
PARAM_WIDTH = 84
KNOB_SIZE = 34
DEVICE_WIDTH = 216
FOLDED_WIDTH = 26  # a folded device: a strip with its name


def _elided(text: str, width: int, label: QLabel) -> str:
    return QFontMetrics(label.font()).elidedText(text, Qt.TextElideMode.ElideRight, width)


HEADER_BUTTON = 16


def _header_button(text: str, tooltip: str, icon: QIcon | None = None) -> QPushButton:
    """A small button on a device's title bar."""
    button = QPushButton(text)
    button.setProperty("role", "device-header")
    button.setFont(theme.ui_font(11))
    button.setFixedSize(HEADER_BUTTON, HEADER_BUTTON)
    button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
    button.setToolTip(tooltip)
    if icon is not None:
        button.setIcon(icon)
        button.setIconSize(button.size() - QSize(5, 5))
    return button


class _TitleLabel(QLabel):
    """A device's name: elided to the room the title bar leaves it."""

    def __init__(self, text: str):
        super().__init__(text)
        self.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        self.setMinimumWidth(16)

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setFont(self.font())
        p.setPen(self.palette().color(QPalette.ColorRole.WindowText))
        text = p.fontMetrics().elidedText(self.text(), Qt.TextElideMode.ElideRight, self.width())
        p.drawText(self.rect(), Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft, text)


class _VerticalTitle(QWidget):
    """A folded device's name, reading upwards from near the top, as in Ableton (it takes the
    device's title as it changes)."""

    def __init__(self, title: QLabel):
        super().__init__()
        self.source = title
        self.setFont(title.font())
        self.setSizePolicy(QSizePolicy.Policy.Preferred, QSizePolicy.Policy.Ignored)
        self.setMinimumHeight(16)

    def sizeHint(self) -> QSize:
        return QSize(self.fontMetrics().height(), 16)

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setFont(self.font())
        p.setPen(self.palette().color(QPalette.ColorRole.WindowText))
        p.translate(0, self.height())
        p.rotate(-90)
        text = p.fontMetrics().elidedText(self.source.text(), Qt.TextElideMode.ElideRight, self.height())
        p.drawText(0, 0, self.height(), self.width(), Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight,
                   text)


class _TouchFilter(QObject):
    """Calls `touched` when a parameter's control (or its name) is pressed."""

    def __init__(self, touched, parent: QObject):
        super().__init__(parent)
        self.touched = touched

    def eventFilter(self, _obj: QObject, event: QEvent) -> bool:
        if event.type() in (QEvent.Type.MouseButtonPress, QEvent.Type.MouseButtonDblClick) \
                and event.button() == Qt.MouseButton.LeftButton:
            self.touched()
        return False


class _DeviceFrame(QFrame):
    """What built-in and plug-in devices share: the frame, the title bar (on/off,
    name, parameter pages, save), the parameters' pages, selecting and
    dragging it, and the right-click menu. Subclasses say how many parameters
    there are (`_set_param_count`) and make each one's widget (`_param_widget`).
    Device editors may lay out more parameters at a time, and add widgets beside
    them (`content`)."""

    device_width = DEVICE_WIDTH
    params_per_page = PARAMS_PER_PAGE
    param_columns = PARAM_COLUMNS

    pressed = Signal(str, object)  # device id, keyboard modifiers: select it
    released = Signal(str, object)  # device id, modifiers: a click (not a drag) ended
    drag_started = Signal(str)  # device id
    menu_requested = Signal(str)  # device id: select it before its menu shows
    page_changed = Signal(str, int)  # device id, page
    preset_saved = Signal(str)  # the preset file: the device was saved to the library

    def __init__(self, track_id: str, device: Device, editor: ProjectEditor, parent: QWidget | None = None,
                 bridge: EngineBridge | None = None):
        super().__init__(parent)
        self.track_id = track_id
        self.device_id = device.id
        self.source = device  # the model's device object it was built for
        self.editor = editor
        self.bridge = bridge
        self.instrument = device_is_instrument(device)
        self.selected = False
        # What the menu's Delete (and Group, and folding) does; the device view makes it act on all its
        # selected devices, and adds its clipboard's entries (Cut, Copy, Paste) to the menu.
        self.remove_selected = lambda: editor.remove_device(track_id, self.device_id)
        self.group_selected = lambda: editor.group_devices(track_id, [self.device_id])
        self.toggle_fold = lambda: editor.set_devices_folded(track_id, [self.device_id], not self.folded)
        self.clipboard_menu = None  # (menu) -> None
        self.folded = editor.project.is_device_folded(device.id)
        self._press: QPoint | None = None
        self.setObjectName("device")
        self.setFixedWidth(self.device_width)
        self._update_style()
        self.param_count = 0
        self.pages = 1
        self.page = 0

        self.fold_button = _header_button("", "Unfold" if self.folded else "Fold", icons.fold(self.folded))
        self.fold_button.clicked.connect(lambda: self.toggle_fold())
        self.enabled = ToggleButton(role="activator", tooltip="Device On/Off")
        self.enabled.setFixedSize(14, 14)
        self.enabled.setChecked(device.enabled)
        self.enabled.toggled.connect(lambda on: editor.set_device_enabled(track_id, self.device_id, on))
        self.title = _TitleLabel(device_name(device))
        self.title.setFont(theme.ui_font(9, bold=True))
        self.previous = _header_button("‹", "Previous parameters")
        self.previous.clicked.connect(lambda: self.set_page(self.page - 1))
        self.page_label = QLabel()
        self.page_label.setObjectName("devicePage")
        self.next = _header_button("›", "Next parameters")
        self.next.clicked.connect(lambda: self.set_page(self.page + 1))
        self.save = _header_button("", "Save Preset", icons.save())
        self.save.clicked.connect(lambda: self.save_to_library())
        self.sidechain = None  # its sidechain's button, if it has a sidechain input
        if bridge is not None and bridge.has_sidechain_input(track_id, device.id):
            self.sidechain = ToggleButton(icon=icons.sidechain(), role="device-header")
            self.sidechain.setFixedSize(HEADER_BUTTON, HEADER_BUTTON)
            self.sidechain.setIconSize(self.sidechain.size() - QSize(5, 5))
            self.sidechain.clicked.connect(self._show_sidechain_menu)

        # The title bar. Clicks on its background and name reach the frame (select, drag).
        self.header_bar = QFrame()
        self.header_bar.setObjectName("deviceHeader")
        self.header = QHBoxLayout(self.header_bar)
        self.header.setContentsMargins(3, 2, 3, 2)
        self.header.setSpacing(3)
        if not self.folded:
            self.header.addWidget(self.fold_button)
            self.header.addWidget(self.enabled)
        self.header.addSpacing(2)
        self.header.addWidget(self.title, 1)
        for widget in (self.sidechain, self.previous, self.page_label, self.next, self.save):
            if widget is not None:
                self.header.addWidget(widget)
        self.update_sidechain()
        self.params = QGridLayout()
        self.params.setContentsMargins(0, 0, 0, 0)
        self.params.setHorizontalSpacing(16)
        self.params.setVerticalSpacing(6)
        # Widgets go into the frame's layout before they are shown: one shown
        # before it has a parent becomes a window of its own, for a moment.
        outer = QVBoxLayout(self)
        outer.setContentsMargins(1, 1, 1, 1)  # inside the border
        outer.setSpacing(0)
        outer.addWidget(self.header_bar)
        # Folded, a strip instead: the fold button, the on/off switch and the name, upwards.
        self.folded_bar = QFrame()
        self.folded_bar.setObjectName("deviceHeader")
        strip = QVBoxLayout(self.folded_bar)
        strip.setContentsMargins(2, 4, 2, 4)
        strip.setSpacing(4)
        if self.folded:
            strip.addWidget(self.fold_button, 0, Qt.AlignmentFlag.AlignHCenter)
            strip.addWidget(self.enabled, 0, Qt.AlignmentFlag.AlignHCenter)
        strip.addWidget(_VerticalTitle(self.title), 1)  # (no alignment: it takes the height left)
        outer.addWidget(self.folded_bar, 1)
        self.body_widget = QWidget()  # what folding hides
        self.body = QVBoxLayout(self.body_widget)
        self.body.setContentsMargins(8, 6, 8, 6)
        self.body.setSpacing(4)
        self.content = QHBoxLayout()  # the parameters, and whatever an editor shows beside them
        self.content.setContentsMargins(0, 0, 0, 0)
        self.content.setSpacing(12)
        self.content.addLayout(self.params)
        self.body.addLayout(self.content)
        self.body.addStretch(1)
        outer.addWidget(self.body_widget, 1)
        self.header_bar.setVisible(not self.folded)
        self.body_widget.setVisible(not self.folded)
        self.folded_bar.setVisible(self.folded)
        if self.folded:
            self.setFixedWidth(FOLDED_WIDTH)

    # --- Parameter pages -------------------------------------------------------------

    def _set_param_count(self, count: int, page: int = 0) -> None:
        self.param_count = count
        self.pages = max(1, -(-count // self.params_per_page))
        self.page = max(0, min(page, self.pages - 1))
        for widget in (self.previous, self.page_label, self.next):
            widget.setVisible(self.pages > 1)
        self._build_page()

    def set_page(self, page: int) -> None:
        page = max(0, min(page, self.pages - 1))
        if page != self.page:
            self.page = page
            self._build_page()
            self.page_changed.emit(self.device_id, page)

    def _build_page(self) -> None:
        while self.params.count():
            item = self.params.takeAt(0)
            if item.widget():
                item.widget().hide()  # now: until deleted it would still be painted where it was
                item.widget().deleteLater()
        self._clear_params()
        self.page_label.setText(f"{self.page + 1}/{self.pages}")
        self.previous.setEnabled(self.page > 0)
        self.next.setEnabled(self.page < self.pages - 1)
        first = self.page * self.params_per_page
        for slot, n in enumerate(range(first, min(first + self.params_per_page, self.param_count))):
            self.params.addWidget(self._param_widget(n), slot // self.param_columns, slot % self.param_columns,
                                  Qt.AlignmentFlag.AlignTop)

    def _clear_params(self) -> None:
        """Forget the widgets of the page that goes."""

    def _param_widget(self, n: int) -> QWidget:
        """The n-th parameter's name and knob (or list)."""
        raise NotImplementedError

    def _param_cell(self, name_text: str, param_id: str | None = None) -> tuple[QWidget, QVBoxLayout]:
        """A parameter's column: its name on top; the caller adds the control.
        Right-clicking it offers its automation."""
        cell = QWidget()
        cell.setFixedWidth(PARAM_WIDTH)
        if param_id is not None and self.bridge is not None:
            cell.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
            cell.customContextMenuRequested.connect(
                lambda pos, c=cell: self._automation_menu(param_id, c.mapToGlobal(pos)))
        column = QVBoxLayout(cell)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(1)
        name = QLabel()
        name.setAlignment(Qt.AlignmentFlag.AlignCenter)
        name.setStyleSheet(f"color: {theme.TEXT_DIM}; font-size: 8pt;")
        name.setText(_elided(name_text, PARAM_WIDTH, name))
        name.setToolTip(name_text)
        column.addWidget(name)
        if param_id is not None and self.bridge is not None:
            self._watch_touch(cell, param_id)
        return cell, column

    def _watch_touch(self, widget: QWidget, param_id: str) -> None:
        """Pressing `widget` shows the parameter's automation, as clicking a control does in Ableton."""
        widget.installEventFilter(_TouchFilter(
            lambda: self.editor.touch_parameter(self.track_id, device_key(self.device_id, param_id)), widget))

    def _automation_menu(self, param_id: str, at: QPoint) -> None:
        editor, bridge, owner = self.editor, self.bridge, self.track_id
        key = device_key(self.device_id, param_id)
        menu = QMenu(self)
        menu.addAction("Show Automation", lambda: editor.show_automation(owner, key)).setEnabled(
            bridge.can_automate(owner, key))
        menu.addAction("Delete Automation", lambda: editor.clear_envelope(owner, key)).setEnabled(
            bool(editor.project.envelope(owner, key)))
        if bridge.is_overridden(owner, key):
            menu.addAction("Re-Enable Automation", lambda: bridge.re_enable_automation(owner))
        rack_id = self.enclosing_rack()
        if rack_id is not None:  # its rack's macros can move it
            menu.addSeparator()
            mapped = editor.macro_of(owner, self.device_id, param_id)
            macros = menu.addMenu("Map to Macro")
            for index in range(MACRO_COUNT):
                action = macros.addAction(f"Macro {index + 1}", lambda i=index: editor.map_macro(
                    owner, rack_id, i, self.device_id, param_id))
                action.setCheckable(True)
                action.setChecked(mapped == (rack_id, index))
            if mapped is not None:
                menu.addAction(f"Unmap from Macro {mapped[1] + 1}",
                               lambda: editor.unmap_macro(owner, mapped[0], self.device_id, param_id))
        menu.exec(at)

    def enclosing_rack(self) -> str | None:
        """The rack the device is in (None: on the track's own chain)."""
        project = self.editor.project
        chain = container_of(project.track(self.track_id).devices, self.device_id)
        return None if chain is None else project.chain_rack(self.track_id, chain).id

    def automation_state(self, param_id: str) -> str | None:
        return automation_state(self.bridge, self.track_id, device_key(self.device_id, param_id)) \
            if self.bridge is not None else None

    def follows_automation(self) -> bool:
        """Whether a parameter shown moves with automation (so the view refreshes it as it plays)."""
        return False

    def refresh_automation(self) -> None:
        """Shows which parameters are automated, and their values as they play."""

    def refresh_displays(self) -> None:
        """Draws what the engine reported since (meters, curves); called as the meters are."""

    @staticmethod
    def _readout(text: str) -> QLabel:
        readout = QLabel()
        readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
        readout.setStyleSheet("font-size: 8pt;")
        readout.setText(_elided(text, PARAM_WIDTH, readout))
        return readout

    def device(self) -> Device:
        return self.editor.project.device(self.track_id, self.device_id)

    def set_selected(self, selected: bool) -> None:
        if selected != self.selected:
            self.selected = selected
            self._update_style()

    def _update_style(self) -> None:
        border = theme.ACCENT if self.selected else theme.BORDER
        header = theme.DEVICE_HEADER_SELECTED if self.selected else theme.DEVICE_HEADER
        self.setStyleSheet(f"#device {{ background: {theme.PANEL_ALT}; border: 1px solid {border};"
                           f" border-radius: 4px; }}"
                           f"#deviceHeader {{ background: {header}; border: none;"
                           f" border-top-left-radius: 3px; border-top-right-radius: 3px; }}"
                           f"#devicePage {{ color: {theme.TEXT_DIM}; font-size: 8pt; }}")

    # Clicks that reach the frame are on its background or labels: the controls take their own.
    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self._press = event.position().toPoint()
            self.pressed.emit(self.device_id, event.modifiers())
        event.accept()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if (self._press is not None and event.buttons() & Qt.MouseButton.LeftButton and not self.instrument
                and (event.position().toPoint() - self._press).manhattanLength()
                >= QApplication.startDragDistance()):
            self._press = None
            self.drag_started.emit(self.device_id)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        if self._press is not None and event.button() == Qt.MouseButton.LeftButton:
            self.released.emit(self.device_id, event.modifiers())
        self._press = None

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            if self.folded or event.modifiers() & Qt.KeyboardModifier.ControlModifier:
                self.toggle_fold()  # (Ctrl+double-click folds it, as the triangle does)
            else:
                self.open_editor()
        event.accept()

    def open_editor(self) -> None:
        """What double-clicking the device does: plug-ins show their own editor."""

    def contextMenuEvent(self, event: QContextMenuEvent) -> None:
        self.menu_requested.emit(self.device_id)
        menu = QMenu(self)
        self.add_menu_actions(menu)
        menu.addAction("Unfold" if self.folded else "Fold", self.toggle_fold)
        menu.addSeparator()
        if self.clipboard_menu is not None:
            self.clipboard_menu(menu)
            menu.addSeparator()
        devices = self.editor.project.track(self.track_id).devices
        siblings = chain_devices(devices, container_of(devices, self.device_id))
        chain = [d.id for d in siblings]
        index = chain.index(self.device_id)
        if not device_is_instrument(self.device()):
            left = menu.addAction("Move Left", lambda: self.editor.move_device(self.track_id, self.device_id, index - 1))
            first = 1 if device_is_instrument(siblings[0]) else 0
            left.setEnabled(index > first)
            right = menu.addAction("Move Right",
                                   lambda: self.editor.move_device(self.track_id, self.device_id, index + 1))
            right.setEnabled(index < len(chain) - 1)
            menu.addSeparator()
        menu.addAction("Save Preset…", lambda: self.save_to_library())
        device = self.device()
        if not device.is_rack:
            menu.addAction("Save as Default Preset", self.save_as_default)
            menu.addAction("Clear Default Preset", self.clear_default).setEnabled(
                has_default(device.kind, device.plugin))
        menu.addSeparator()
        group = menu.addAction("Group", self.group_selected)
        group.setShortcut("Ctrl+G")  # (as a tip: the window's action handles the key)
        group.setShortcutVisibleInContextMenu(True)
        if self.device().is_rack:
            ungroup = menu.addAction("Ungroup", self._ungroup)
            ungroup.setShortcut("Ctrl+Shift+G")
            ungroup.setShortcutVisibleInContextMenu(True)
        menu.addSeparator()
        menu.addAction("Delete", self.remove_selected)
        menu.exec(event.globalPos())

    def _ungroup(self) -> None:
        if not self.editor.ungroup_rack(self.track_id, self.device_id) and self.bridge is not None:
            self.bridge.status_message.emit("A rack of several instruments can't be ungrouped: a chain has one.")

    def add_menu_actions(self, menu: QMenu) -> None:
        """Device-specific entries at the top of the right-click menu."""

    def save_to_library(self, name: str | None = None) -> Path | None:
        """The save button: save the device (a rack with everything in it) as a
        preset in the library, under a name asked for (or `name`; asked before
        replacing one of that name then). The preset's file (None: not saved)."""
        device = self.device()
        if self.bridge is not None:
            self.bridge.store_plugin_states(device_ids_of(device))  # (as they are now)
        asked = name is None
        if asked:
            name, ok = QInputDialog.getText(self, "Save Preset", "Preset name:", text=device_name(device))
            if not ok:
                return None
        name = name.strip()
        try:
            path = preset_path(device, name)
        except ValueError as exc:
            self._report(str(exc))
            return None
        if asked and path.exists() and QMessageBox.question(
                self, "Save Preset", f"There is a {path.parent.name} preset called \u201c{path.stem}\u201d already. "
                "Replace it?", QMessageBox.StandardButton.Yes | QMessageBox.StandardButton.No
        ) != QMessageBox.StandardButton.Yes:
            return None
        try:
            save_to_library(device, name)
        except OSError as exc:
            self._report(f"Could not save the preset: {exc}")
            return None
        self._report(f"Saved the preset {path.stem} ({path.parent.name}).")
        if device.is_rack:  # (a rack is named as its preset)
            self.editor.rename_rack(self.track_id, self.device_id, path.stem, f"Save Preset {path.stem}")
        self.preset_saved.emit(str(path))
        return path

    def save_as_default(self) -> Path | None:
        """The device as the default preset of its kind: new devices of that kind
        (that plug-in) start as it is now. Its file (None: not saved)."""
        device = self.device()
        if self.bridge is not None:
            self.bridge.store_plugin_states({device.id})
        try:
            path = save_default(device)
        except (OSError, ValueError) as exc:
            self._report(f"Could not save the default preset: {exc}")
            return None
        self._report(f"New {device_name(device)} devices will start like this one.")
        return path

    def clear_default(self) -> None:
        """New devices of this kind start as they come again."""
        device = self.device()
        try:
            if clear_default(device.kind, device.plugin):
                self._report(f"New {device_name(device)} devices will start as they come.")
        except OSError as exc:
            self._report(f"Could not clear the default preset: {exc}")

    def _report(self, message: str) -> None:
        if self.bridge is not None:
            self.bridge.status_message.emit(message)

    def refresh(self, device: Device) -> None:
        self.enabled.set_checked_silently(device.enabled)
        self.update_sidechain()

    # --- Sidechain -------------------------------------------------------------------

    def _tap_choices(self, source_id: str) -> list[tuple[str, str]]:
        """Where a sidechain from a track can be taken, along its signal, as in
        Ableton: before its devices, after each, after them all, after its fader
        (label, tap). On a MIDI track, Pre FX is after the instrument."""
        devices = self.editor.project.track(source_id).devices
        names = [device_name(d) for d in devices]
        choices = [("Pre FX", PRE_FX)]
        for device, name in zip(devices, names, strict=True):
            if names.count(name) > 1:
                name = f"{name} ({names[:devices.index(device) + 1].count(name)})"
            if not device_is_instrument(device):
                choices.append((f"After {name}", device.id))
        return [*choices, ("Post FX", PRE_FADER), ("Post Mixer", POST_FADER)]

    def _tap_of(self, sidechain: Sidechain) -> str:
        """Where it is taken now: after a device that has left its source, before
        the fader; after its instrument, before its effects."""
        devices = self.editor.project.track(sidechain.track_id).devices
        if sidechain.tap_device is not None:
            device = next((d for d in devices if d.id == sidechain.tap), None)
            if device is None:
                return PRE_FADER
            if device_is_instrument(device):
                return PRE_FX
        return sidechain.tap

    def update_sidechain(self) -> None:
        """The sidechain button: lit while the device has one, which its tooltip names."""
        if self.sidechain is None:
            return
        sidechain = self.device().sidechain
        if sidechain is not None and not self.editor.project.has_owner(sidechain.track_id):
            sidechain = None  # (its source is going: so is the sidechain)
        self.sidechain.set_checked_silently(sidechain is not None)
        if sidechain is None:
            self.sidechain.setToolTip("Sidechain: none (click to choose a track)")
            return
        tap = self._tap_of(sidechain)
        where = next(label for label, t in self._tap_choices(sidechain.track_id) if t == tap)
        name = self.editor.project.track(sidechain.track_id).name
        self.sidechain.setToolTip(f"Sidechain: {name}, {where}")

    def sidechain_menu(self) -> QMenu:
        """No sidechain, or the tracks, groups and returns it can come from
        (those that would close a cycle greyed out); then where it is taken."""
        project = self.editor.project
        current = self.device().sidechain
        if current is not None and not project.has_owner(current.track_id):
            current = None
        menu = QMenu(self)
        none = menu.addAction("No Sidechain", lambda: self._set_sidechain(None))
        none.setCheckable(True)
        none.setChecked(current is None)
        menu.addSeparator()
        for source in project.sidechain_sources(self.track_id):
            usable = not project.sidechain_would_cycle(self.track_id, source.id)
            # A new source: taken where the old one was (after its fader if after a device of it).
            tap = POST_FADER if current is None or current.tap_device is not None else current.tap
            action = menu.addAction(source.name if usable else f"{source.name} (this track feeds it)",
                                    lambda s=source.id, t=tap: self._set_sidechain(Sidechain(s, t)))
            action.setCheckable(True)
            action.setChecked(current is not None and current.track_id == source.id)
            action.setEnabled(usable)
        if current is not None:
            menu.addSeparator()
            tap = self._tap_of(current)
            for label, choice in self._tap_choices(current.track_id):
                action = menu.addAction(label, lambda c=choice: self._set_sidechain(Sidechain(current.track_id, c)))
                action.setCheckable(True)
                action.setChecked(choice == tap)
        return menu

    def _show_sidechain_menu(self) -> None:
        self.update_sidechain()  # (the click toggled it)
        self.sidechain_menu().exec(self.sidechain.mapToGlobal(QPoint(0, self.sidechain.height())))

    def _set_sidechain(self, sidechain: Sidechain | None) -> None:
        try:
            self.editor.set_device_sidechain(self.track_id, self.device_id, sidechain)
        except ValueError as exc:  # (its source went meanwhile)
            if self.bridge is not None:
                self.bridge.status_message.emit(str(exc))
        self.update_sidechain()


class _TallestDevice(_DeviceFrame):
    """A device as tall as any gets: a knob in every slot of the page, and page
    arrows. The device view is made to fit it, so the chain never scrolls
    vertically, whatever the fonts and the screen's scale."""

    def __init__(self, editor: ProjectEditor):
        super().__init__("", Device(id="", kind="utility"), editor)
        self._set_param_count(PARAMS_PER_PAGE + 1)

    def _param_widget(self, n: int) -> QWidget:
        cell, column = self._param_cell("Name")  # a knob and its readout: taller than a list
        knob = Knob()
        knob.setFixedSize(KNOB_SIZE, KNOB_SIZE)
        column.addWidget(knob, 0, Qt.AlignmentFlag.AlignHCenter)
        column.addWidget(self._readout("0.00"))
        return cell


def device_height(editor: ProjectEditor) -> int:
    """The height the tallest device needs."""
    probe = _TallestDevice(editor)
    probe.body.activate()  # (now, not when Qt gets to it: the frame's layout sees the body's size)
    height = probe.minimumSizeHint().height()
    probe.deleteLater()
    return height
