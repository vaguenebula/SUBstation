"""Bottom 'detail view': the selected track's (or the master's) device chain. On
a MIDI track the instrument comes first; the master takes effects only.

Each device has a title bar, as in Ableton: its on/off switch and name, the
arrows to its other parameter pages, the button for a plug-in's own editor, a
save button (not wired up yet). It is lighter while the device is
selected.

Parameter metadata comes from the engine, so built-in devices and plug-ins
show alike: a knob per parameter (log-scaled where the engine says so), or a
list for parameters that choose between named values, four at a time in a 2×2
grid. A plug-in shows its own text for their values. Right-click a device for
more (move, presets).

Automated parameters are marked (red: automated, grey: overridden) and follow
their automation as it plays; right-click one to show its automation, delete it,
or re-enable it. Changing one shows its automation in the arrangement.

Click a device (its title or background) to select it, Shift-click to select a
range, Ctrl-click to add or remove one; Delete deletes the selection. Drag
effects to reorder them (the instrument stays first), or onto another track in
the arrangement to move them there (plug-ins keep their state). Ctrl+Alt-drag anywhere on
the chain scrolls it, as in the arrangement. The chain scrolls to show a device
when one is added, unless it was dropped on the chain (where it is in view).
"""

from __future__ import annotations

import base64
import os
from pathlib import Path

from PySide6.QtCore import (
    QEvent,
    QMimeData,
    QObject,
    QPoint,
    QSettings,
    QSize,
    Qt,
    QTimer,
    Signal,
)
from PySide6.QtGui import (
    QColor,
    QContextMenuEvent,
    QDrag,
    QDragEnterEvent,
    QDragMoveEvent,
    QDropEvent,
    QFontMetrics,
    QMouseEvent,
    QIcon,
    QPainter,
    QPalette,
)
from PySide6.QtWidgets import (
    QApplication,
    QComboBox,
    QFileDialog,
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QMenu,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QStyle,
    QVBoxLayout,
    QWidget,
)

from .. import theme
from ..audio.engine_bridge import EngineBridge
from ..model.automation import device_key
from ..model.editor import (
    BUILTIN_DEVICES,
    ProjectEditor,
    device_is_instrument,
    device_name,
)
from ..model.params import format_value
from ..model.project import PLUGIN_KIND, Device
from .arrangement.lanes_canvas import DEVICE_MOVE_MIME, is_pan_modifier, moved_devices
from .arrangement.track_headers import automation_state
from .arrangement.view_state import Selection
from .browser.browser_models import PLUGIN_MIME, device_kinds, plugin_refs
from . import icons
from .widgets import Knob, ToggleButton

PANEL_MARGIN = 8  # above and below the chain
PARAM_COLUMNS = 2
PARAMS_PER_PAGE = 4  # a 2×2 grid
PARAM_WIDTH = 84
KNOB_SIZE = 34
DEVICE_WIDTH = 216
EFFECTS_HINT = "Drop audio effects here from the browser (Built-in or Plug-ins › Audio Effects)"
INSTRUMENT_HINT = "Drop an instrument here from the browser (Built-in or Plug-ins › Instruments)"
INSTRUMENT_REFUSED = "Instruments go on MIDI tracks (Create › Insert MIDI Track)."
MESSAGE_LINES = 4  # a plug-in's error message is cut to this; its tooltip has it all
PRESET_FILTER = "VST3 Preset (*.vstpreset)"
AUTOSCROLL_EDGE = 40  # px from the chain's edge where a drag scrolls it
AUTOSCROLL_INTERVAL = 16  # ms


def _encode(state: bytes | None) -> str | None:
    return base64.b64encode(state).decode("ascii") if state else None


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
    there are (`_set_param_count`) and make each one's widget (`_param_widget`)."""

    pressed = Signal(str, object)  # device id, keyboard modifiers: select it
    released = Signal(str, object)  # device id, modifiers: a click (not a drag) ended
    drag_started = Signal(str)  # device id
    menu_requested = Signal(str)  # device id: select it before its menu shows
    page_changed = Signal(str, int)  # device id, page

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
        # What the menu's Delete does; the device view makes it delete all its selected devices.
        self.remove_selected = lambda: editor.remove_device(track_id, self.device_id)
        self._press: QPoint | None = None
        self.setObjectName("device")
        self.setFixedWidth(DEVICE_WIDTH)
        self._update_style()
        self.param_count = 0
        self.pages = 1
        self.page = 0

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
        self.save = _header_button("", "Save Preset", icons.save())  # not wired up yet

        # The title bar. Clicks on its background and name reach the frame (select, drag).
        self.header_bar = QFrame()
        self.header_bar.setObjectName("deviceHeader")
        self.header = QHBoxLayout(self.header_bar)
        self.header.setContentsMargins(5, 2, 3, 2)
        self.header.setSpacing(3)
        self.header.addWidget(self.enabled)
        self.header.addSpacing(2)
        self.header.addWidget(self.title, 1)
        for widget in (self.previous, self.page_label, self.next, self.save):
            self.header.addWidget(widget)
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
        self.body = QVBoxLayout()
        self.body.setContentsMargins(8, 6, 8, 6)
        self.body.setSpacing(4)
        self.body.addLayout(self.params)
        self.body.addStretch(1)
        outer.addLayout(self.body)

    # --- Parameter pages -------------------------------------------------------------

    def _set_param_count(self, count: int, page: int = 0) -> None:
        self.param_count = count
        self.pages = max(1, -(-count // PARAMS_PER_PAGE))
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
        first = self.page * PARAMS_PER_PAGE
        for slot, n in enumerate(range(first, min(first + PARAMS_PER_PAGE, self.param_count))):
            self.params.addWidget(self._param_widget(n), slot // PARAM_COLUMNS, slot % PARAM_COLUMNS,
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
        menu.exec(at)

    def automation_state(self, param_id: str) -> str | None:
        return automation_state(self.bridge, self.track_id, device_key(self.device_id, param_id)) \
            if self.bridge is not None else None

    def follows_automation(self) -> bool:
        """Whether a parameter shown moves with automation (so the view refreshes it as it plays)."""
        return False

    def refresh_automation(self) -> None:
        """Shows which parameters are automated, and their values as they play."""

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
            self.open_editor()
        event.accept()

    def open_editor(self) -> None:
        """What double-clicking the device does: plug-ins show their own editor."""

    def contextMenuEvent(self, event: QContextMenuEvent) -> None:
        self.menu_requested.emit(self.device_id)
        menu = QMenu(self)
        self.add_menu_actions(menu)
        chain = [d.id for d in self.editor.project.track(self.track_id).devices]
        index = chain.index(self.device_id)
        if not device_is_instrument(self.device()):
            left = menu.addAction("Move Left", lambda: self.editor.move_device(self.track_id, self.device_id, index - 1))
            first = 1 if device_is_instrument(self.editor.project.track(self.track_id).devices[0]) else 0
            left.setEnabled(index > first)
            right = menu.addAction("Move Right",
                                   lambda: self.editor.move_device(self.track_id, self.device_id, index + 1))
            right.setEnabled(index < len(chain) - 1)
            menu.addSeparator()
        menu.addAction("Delete", self.remove_selected)
        menu.exec(event.globalPos())

    def add_menu_actions(self, menu: QMenu) -> None:
        """Device-specific entries at the top of the right-click menu."""

    def refresh(self, device: Device) -> None:
        self.enabled.set_checked_silently(device.enabled)


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
    height = probe.minimumSizeHint().height()
    probe.deleteLater()
    return height


class DeviceWidget(_DeviceFrame):
    """A built-in device: its parameters are in the model."""

    def __init__(self, track_id: str, device: Device, editor: ProjectEditor, bridge: EngineBridge,
                 page: int = 0, parent: QWidget | None = None):
        super().__init__(track_id, device, editor, parent, bridge)
        self.knobs: dict[str, tuple[Knob, QLabel, str]] = {}
        self.choices: dict[str, QComboBox] = {}
        engine_id = bridge.engine_device_id(track_id, device.id)
        self.infos = bridge.engine.processor_params(engine_id) if engine_id is not None else []
        self._set_param_count(len(self.infos), page)

    def _clear_params(self) -> None:
        self.knobs.clear()
        self.choices.clear()

    def _param_widget(self, n: int) -> QWidget:
        info = self.infos[n]
        value = self.device().params.get(info.id, info.default_value)
        track_id, editor = self.track_id, self.editor
        cell, column = self._param_cell(info.name, info.id)
        if info.value_labels:
            choice = QComboBox()
            choice.addItems(info.value_labels)
            choice.setCurrentIndex(round(value))
            choice.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            choice.setFixedWidth(PARAM_WIDTH)
            choice.activated.connect(
                lambda i, pid=info.id: editor.set_device_param(track_id, self.device_id, pid, float(i)))
            column.addWidget(choice)
            column.addStretch(1)
            self.choices[info.id] = choice
            self._watch_touch(choice, info.id)
        else:
            knob = Knob(info.min_value, info.max_value, value, default=info.default_value,
                        bipolar=info.min_value < 0 < info.max_value and info.unit == "",
                        log_scale=info.log_scale, formatter=lambda v, u=info.unit: format_value(v, u))
            knob.setFixedSize(KNOB_SIZE, KNOB_SIZE)
            readout = self._readout(format_value(value, info.unit))
            knob.valueChanged.connect(
                lambda v, key, pid=info.id: editor.set_device_param(track_id, self.device_id, pid, v, key))
            column.addWidget(knob, 0, Qt.AlignmentFlag.AlignHCenter)
            column.addWidget(readout)
            self.knobs[info.id] = (knob, readout, info.unit)
            self._watch_touch(knob, info.id)
            knob.set_automation(self.automation_state(info.id))
        return cell

    def _build_page(self) -> None:
        super()._build_page()
        self.refresh_automation()

    def refresh(self, device: Device) -> None:
        super().refresh(device)
        for param_id, (knob, readout, unit) in self.knobs.items():
            value = device.params.get(param_id)
            if value is not None:
                knob.setValue(value)
                readout.setText(_elided(format_value(value, unit), PARAM_WIDTH, readout))
        for param_id, choice in self.choices.items():
            value = device.params.get(param_id)
            if value is not None:
                choice.setCurrentIndex(round(value))
        self.refresh_automation()

    def follows_automation(self) -> bool:
        return any(self.automation_state(param_id) == "on" for param_id in [*self.knobs, *self.choices])

    def refresh_automation(self) -> None:
        """Automated parameters show their envelope's value (their own again when it stops)."""
        device = self.device()
        for param_id, (knob, readout, unit) in self.knobs.items():
            state = self.automation_state(param_id)
            knob.set_automation(state)
            value = (self.bridge.current_value(self.track_id, device_key(self.device_id, param_id)) if state == "on"
                     else device.params.get(param_id))
            if value is not None:
                knob.setValue(value)
                readout.setText(_elided(format_value(value, unit), PARAM_WIDTH, readout))
        for param_id, choice in self.choices.items():
            if self.automation_state(param_id) == "on":
                value = self.bridge.current_value(self.track_id, device_key(self.device_id, param_id))
            else:
                value = device.params.get(param_id)
            if value is not None:
                choice.setCurrentIndex(round(value))


class PluginDeviceWidget(_DeviceFrame):
    """A plug-in: its parameters live in the plug-in (the engine), shown a page
    at a time; the Edit button shows its own editor."""

    def __init__(self, track_id: str, device: Device, editor: ProjectEditor, bridge: EngineBridge,
                 page: int = 0, parent: QWidget | None = None):
        super().__init__(track_id, device, editor, parent, bridge)
        self.engine = bridge.engine
        self.engine_id = bridge.engine_device_id(track_id, device.id)
        self.knobs: dict[int, tuple[Knob, QLabel]] = {}  # parameter index ->
        self.choices: dict[int, QComboBox] = {}
        self.infos = self.engine.processor_params(self.engine_id) if self.engine_id is not None else []
        # What a generic editor should offer: what can be automated and isn't the plug-in's own business.
        shown = [i for i, p in enumerate(self.infos) if p.automatable and not p.hidden and not p.read_only]
        self.shown = shown or [i for i, p in enumerate(self.infos) if not p.hidden and not p.read_only]

        plugin = device.plugin
        self.title.setText(plugin.name)
        self._update_tooltip()

        self.edit = ToggleButton(icon=icons.plugin_window(), role="device-header",
                                 tooltip="Show the plug-in's own editor")
        self.edit.setFixedSize(HEADER_BUTTON, HEADER_BUTTON)
        self.edit.setIconSize(self.edit.size() - QSize(5, 5))
        self.edit.toggled.connect(self._toggle_editor)
        self.header.insertWidget(self.header.indexOf(self.title) + 1, self.edit)
        self.edit.setEnabled(self.engine_id is not None)
        self.update_editor_button()

        if self.engine_id is None or not self.shown:
            text = "No parameters to show here: use the plug-in's editor."
            if self.engine_id is None:
                text = bridge.plugin_errors.get(device.id) or f"{plugin.name} is not loaded."
            message = QLabel(text)
            message.setWordWrap(True)
            message.setStyleSheet(f"color: {theme.TEXT_DIM};")
            # A long one would make the device taller than the view.
            message.setMaximumHeight(QFontMetrics(message.font()).lineSpacing() * MESSAGE_LINES)
            message.setToolTip(text)
            self.body.insertWidget(self.body.indexOf(self.params) + 1, message)
        self._set_param_count(len(self.shown) if self.engine_id is not None else 0, page)

    def _update_tooltip(self) -> None:
        plugin = self.device().plugin
        lines = [plugin.name, plugin.vendor, self.bridge.plugin_path(plugin) or plugin.path]
        if self.engine_id is not None:
            latency = self.engine.processor_info(self.engine_id).latency
            if latency:
                lines.append(f"Latency: {latency} samples (compensated)")
        self.title.setToolTip("\n".join(line for line in lines if line))

    # --- Parameters ----------------------------------------------------------------

    def _text(self, index: int, value: float) -> str:
        return self.bridge.plugin_param_text(self.engine_id, index, value)

    def _clear_params(self) -> None:
        self.knobs.clear()
        self.choices.clear()

    def _param_widget(self, n: int) -> QWidget:
        index = self.shown[n]
        info = self.infos[index]
        value = self.engine.processor_param(self.engine_id, index)
        cell, column = self._param_cell(info.name, info.id)
        if info.value_labels:
            choice = QComboBox()
            choice.addItems(info.value_labels)
            choice.setCurrentIndex(round(value))
            choice.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            choice.setFixedWidth(PARAM_WIDTH)
            choice.activated.connect(lambda i, ix=index: self._edit(ix, float(i), None))
            column.addWidget(choice)
            column.addStretch(1)
            self.choices[index] = choice
            self._watch_touch(choice, info.id)
        else:
            centred = info.steps == 0 and abs(info.default_value - 0.5 * (info.min_value + info.max_value)) < 1e-6
            knob = Knob(info.min_value, info.max_value, value, default=info.default_value, bipolar=centred,
                        step=1.0 if info.steps else 0.0, formatter=lambda v, ix=index: self._text(ix, v))
            knob.setFixedSize(KNOB_SIZE, KNOB_SIZE)
            readout = self._readout(self._text(index, value))
            knob.valueChanged.connect(lambda v, key, ix=index: self._edit(ix, v, key))
            column.addWidget(knob, 0, Qt.AlignmentFlag.AlignHCenter)
            column.addWidget(readout)
            self.knobs[index] = (knob, readout)
            self._watch_touch(knob, info.id)
            knob.set_automation(self.automation_state(info.id))
        return cell

    def refresh_automation(self) -> None:
        """Marks automated parameters; the plug-in reports their values as they play (refresh_values)."""
        for index, (knob, _readout) in self.knobs.items():
            knob.set_automation(self.automation_state(self.infos[index].id))

    def _edit(self, index: int, value: float, gesture: object | None) -> None:
        old = self.engine.processor_param(self.engine_id, index)
        self.editor.set_device_param(self.track_id, self.device_id, self.infos[index].id, value, gesture, old=old)

    def refresh_values(self) -> None:
        if self.engine_id is None:
            return
        for index, (knob, readout) in self.knobs.items():
            value = self.engine.processor_param(self.engine_id, index)
            knob.setValue(value)
            readout.setText(_elided(self._text(index, value), PARAM_WIDTH, readout))
        for index, choice in self.choices.items():
            choice.setCurrentIndex(round(self.engine.processor_param(self.engine_id, index)))
        self.refresh_automation()
        self._update_tooltip()

    def refresh(self, device: Device) -> None:
        super().refresh(device)
        self.refresh_values()

    # --- Editor and presets --------------------------------------------------------

    def update_editor_button(self) -> None:
        self.edit.set_checked_silently(self.bridge.is_plugin_editor_open(self.track_id, self.device_id))

    def _toggle_editor(self, show: bool) -> None:
        if show:
            self.bridge.open_plugin_editor(self.track_id, self.device_id)
        else:
            self.bridge.close_plugin_editor(self.track_id, self.device_id)
        self.update_editor_button()

    def open_editor(self) -> None:
        if self.engine_id is not None:
            self._toggle_editor(True)  # already open: brought to the front

    def add_menu_actions(self, menu: QMenu) -> None:
        loaded = self.engine_id is not None
        menu.addAction("Show Editor", lambda: self._toggle_editor(True)).setEnabled(loaded)
        menu.addAction("Load Preset…", self.load_preset).setEnabled(loaded)
        menu.addAction("Save Preset…", self.save_preset).setEnabled(loaded)
        menu.addSeparator()

    def _preset_folder(self) -> str:
        stored = QSettings().value("plugins/preset_dir")
        if stored and os.path.isdir(str(stored)):
            return str(stored)
        plugin = self.device().plugin
        # Where VST3 presets usually live.
        folder = Path.home() / "Documents" / "VST3 Presets" / (plugin.vendor or "Unknown") / plugin.name
        return str(folder if folder.is_dir() else Path.home() / "Documents")

    def load_preset(self) -> None:
        path, _ = QFileDialog.getOpenFileName(self, "Load Preset", self._preset_folder(), PRESET_FILTER)
        if not path:
            return
        QSettings().setValue("plugins/preset_dir", str(Path(path).parent))
        try:
            data = Path(path).read_bytes()
            old = self.bridge.plugin_state(self.track_id, self.device_id)
            self.engine.set_processor_state(self.engine_id, data)  # fails if it is another plug-in's
        except (OSError, RuntimeError, ValueError) as exc:
            self.bridge.status_message.emit(f"Could not load {Path(path).name}: {exc}")
            return
        self.editor.set_device_state(self.track_id, self.device_id, _encode(old), _encode(data),
                                     f"Load Preset {Path(path).stem}")

    def save_preset(self) -> None:
        plugin = self.device().plugin
        suggested = str(Path(self._preset_folder()) / f"{plugin.name}.vstpreset")
        path, _ = QFileDialog.getSaveFileName(self, "Save Preset", suggested, PRESET_FILTER)
        if not path:
            return
        QSettings().setValue("plugins/preset_dir", str(Path(path).parent))
        try:
            state = self.bridge.plugin_state(self.track_id, self.device_id)
            if state is not None:
                Path(path).write_bytes(state)
        except (OSError, RuntimeError) as exc:
            self.bridge.status_message.emit(f"Could not save the preset: {exc}")


class DevicePanel(QFrame):
    status_message = Signal(str)

    def __init__(self, editor: ProjectEditor, selection: Selection, bridge: EngineBridge,
                 parent: QWidget | None = None):
        super().__init__(parent)
        self.editor = editor
        self.project = editor.project
        self.selection = selection
        self.bridge = bridge
        self.track_id: str | None = None
        self.widgets: dict[str, _DeviceFrame] = {}
        self._pages: dict[str, int] = {}  # plug-in device id -> the parameter page it shows
        self.selected: list[str] = []  # selected device ids, in chain order
        self._anchor: str | None = None  # where a Shift-click range starts
        self.setAcceptDrops(True)

        self.chain = QWidget()
        self.chain_layout = QHBoxLayout(self.chain)
        self.chain_layout.setContentsMargins(0, 0, 0, 0)
        self.chain_layout.setSpacing(6)
        self.scroll = scroll = QScrollArea()
        scroll.setWidget(self.chain)
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        # Let the panel's own background show through (a plain `background:` rule
        # would cascade into every child widget).
        scroll.setStyleSheet("QScrollArea { background: transparent; }")
        scroll.viewport().setAutoFillBackground(False)
        self.chain.setAutoFillBackground(False)

        self.hint = QLabel(EFFECTS_HINT)
        self.hint.setStyleSheet(f"color: {theme.TEXT_DISABLED};")
        # Where dragged devices would go: a line between devices, outside the layout.
        self.drop_marker = QFrame(self.chain)
        self.drop_marker.setStyleSheet(f"background: {theme.ACCENT};")
        self.drop_marker.hide()
        # A drag held near the chain's left or right edge scrolls it.
        self._drag_pos = QPoint()
        self._scroll_step = 0
        self._autoscroll = QTimer(self)
        self._autoscroll.setInterval(AUTOSCROLL_INTERVAL)
        self._autoscroll.timeout.connect(self._auto_scroll)
        # Ctrl+Alt-drag scrolls the chain. The press usually lands on a device or a
        # knob, so the panel watches the chain's mouse events before they do.
        self._pan: tuple[float, int] | None = None  # (press x on screen, scroll value)
        self._dropping = False  # devices added by a drop on the chain are already in view
        QApplication.instance().installEventFilter(self)

        layout = QHBoxLayout(self)
        layout.setContentsMargins(10, PANEL_MARGIN, 10, PANEL_MARGIN)
        layout.addWidget(scroll, 1)
        # Room for the tallest device with the horizontal scroll bar showing: no vertical scrolling.
        bar = scroll.horizontalScrollBar()
        bar_height = scroll.style().pixelMetric(QStyle.PixelMetric.PM_ScrollBarExtent, None, bar)
        self.setFixedHeight(2 * PANEL_MARGIN + device_height(editor) + bar_height)

        selection.changed.connect(self._on_selection)
        self.project.devices_changed.connect(self._on_devices_changed)
        self.project.device_param_changed.connect(self._on_param_changed)
        self.project.device_state_changed.connect(self._on_state_changed)
        self.project.reset.connect(lambda: self.show_track(None))
        self.project.track_removed.connect(lambda tid, _i: self.show_track(None) if tid == self.track_id else None)
        bridge.plugin_params_changed.connect(self._on_plugin_values)
        bridge.plugin_params_rebuilt.connect(self._on_plugin_rebuilt)
        bridge.plugin_editor_changed.connect(self._on_plugin_editor)
        bridge.devices_loaded.connect(lambda track_id: self._on_devices_changed(track_id, rebuild=True))
        bridge.automation_state_changed.connect(self._on_automation_state)
        bridge.position_changed.connect(self._follow_automation)
        self.show_track(None)

    def _current_widgets(self) -> list[_DeviceFrame]:
        """The widgets of devices still in the chain (it may be changing: they are rebuilt after)."""
        return [self.widgets[i] for i in self._chain_ids() if i in self.widgets]

    def _on_automation_state(self, owner: str) -> None:
        if owner == self.track_id:
            for widget in self._current_widgets():
                widget.refresh_automation()

    def _follow_automation(self) -> None:
        for widget in self._current_widgets():
            if widget.follows_automation():
                widget.refresh_automation()

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(theme.PANEL))
        p.fillRect(0, 0, self.width(), 1, QColor(theme.BORDER))

    def _on_selection(self) -> None:
        if self.selection.track_id != self.track_id:
            self.show_track(self.selection.track_id)
        elif self.selection.focus != "devices" and self.selected:
            self._set_selected([])  # the user went on to select something else

    # --- Selecting devices -------------------------------------------------------------

    def _chain_ids(self) -> list[str]:
        return [d.id for d in self.project.track(self.track_id).devices] if self.track_id else []

    def _set_selected(self, device_ids) -> None:
        wanted = set(device_ids)
        self.selected = [i for i in self._chain_ids() if i in wanted]
        for device_id, widget in self.widgets.items():
            widget.set_selected(device_id in wanted)
        if self.selected:
            self.selection.focus_devices()

    def select_device(self, device_id: str, modifiers=Qt.KeyboardModifier.NoModifier) -> None:
        """A click on a device: Shift selects the range from the last one clicked,
        Ctrl adds or removes it, and a plain click selects just it."""
        chain = self._chain_ids()
        if modifiers & Qt.KeyboardModifier.ShiftModifier and self._anchor in chain:
            a, b = sorted((chain.index(self._anchor), chain.index(device_id)))
            self._set_selected(chain[a:b + 1])
            return
        if modifiers & Qt.KeyboardModifier.ControlModifier:
            self._set_selected(set(self.selected) ^ {device_id})
        else:
            self._set_selected([device_id])
        self._anchor = device_id

    def _on_device_pressed(self, device_id: str, modifiers) -> None:
        # A plain press on a device already selected keeps the others (to drag them
        # all); if no drag follows, the release selects just it.
        extend = modifiers & (Qt.KeyboardModifier.ShiftModifier | Qt.KeyboardModifier.ControlModifier)
        if extend or device_id not in self.selected:
            self.select_device(device_id, modifiers)
        else:
            self.selection.focus_devices()

    def _on_device_released(self, device_id: str, modifiers) -> None:
        extend = modifiers & (Qt.KeyboardModifier.ShiftModifier | Qt.KeyboardModifier.ControlModifier)
        if not extend and len(self.selected) > 1:
            self.select_device(device_id)

    def _on_device_menu(self, device_id: str) -> None:
        if device_id not in self.selected:
            self.select_device(device_id)

    def delete_selected(self) -> bool:
        """Delete the selected devices (one undo step). False if there were none."""
        if not self.track_id or not self.selected:
            return False
        device_ids, self.selected = self.selected, []
        self.editor.remove_devices(self.track_id, device_ids)
        return True

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self._set_selected([])  # a click beside the devices

    # --- Scrolling by hand -------------------------------------------------------------

    def eventFilter(self, obj: QObject, event: QEvent) -> bool:
        kind = event.type()
        if kind == QEvent.Type.MouseButtonPress:
            if (event.button() == Qt.MouseButton.LeftButton and is_pan_modifier(event.modifiers())
                    and isinstance(obj, QWidget) and self.scroll.isAncestorOf(obj)):
                self._pan = (event.globalPosition().x(), self.scroll.horizontalScrollBar().value())
                QApplication.setOverrideCursor(Qt.CursorShape.ClosedHandCursor)
                return True
        elif self._pan is not None and kind in (QEvent.Type.MouseMove, QEvent.Type.MouseButtonRelease):
            if kind == QEvent.Type.MouseMove and event.buttons() & Qt.MouseButton.LeftButton:
                x, value = self._pan
                self.scroll.horizontalScrollBar().setValue(value - round(event.globalPosition().x() - x))
            else:  # released (even if the release went elsewhere)
                self._pan = None
                QApplication.restoreOverrideCursor()
            return True
        return False

    # --- Reordering ------------------------------------------------------------------

    def _start_drag(self, device_id: str) -> None:
        moving = [i for i in (self.selected if device_id in self.selected else [device_id])
                  if not self.widgets[i].instrument]
        if not moving or self.track_id is None:
            return
        mime = QMimeData()
        mime.setData(DEVICE_MOVE_MIME, "\n".join([self.track_id, *moving]).encode())
        drag = QDrag(self)
        drag.setMimeData(mime)
        drag.setPixmap(self.widgets[device_id].grab().scaledToHeight(48, Qt.TransformationMode.SmoothTransformation))
        drag.exec(Qt.DropAction.MoveAction)
        self._drag_ended()

    def _moving(self, mime) -> list[str]:
        """The devices a drag moves, if it moves this track's."""
        moved = moved_devices(mime)
        return moved[1] if moved and moved[0] == self.track_id else []

    def _chain_widgets(self) -> list[_DeviceFrame]:
        return [self.widgets[i] for i in self._chain_ids() if i in self.widgets]

    def drop_index(self, pos: QPoint) -> int:
        """Where in the chain a drop at `pos` (panel coordinates) goes."""
        x = self.chain.mapFrom(self, pos).x()
        return sum(1 for w in self._chain_widgets() if w.geometry().center().x() < x)

    def _show_drop_marker(self, index: int) -> None:
        chain = self._chain_widgets()
        if not chain:
            self.drop_marker.hide()
            return
        gap = self.chain_layout.spacing()
        if index < len(chain):
            x = chain[index].geometry().left() - gap // 2 - 1
        else:
            x = chain[-1].geometry().right() + gap // 2
        top = min(w.geometry().top() for w in chain)
        bottom = max(w.geometry().bottom() for w in chain)
        self.drop_marker.setGeometry(x, top, 2, bottom - top + 1)
        self.drop_marker.raise_()
        self.drop_marker.show()

    def _drag_at(self, pos: QPoint) -> None:
        """A drag is over `pos` (panel coordinates): show where it would drop, and
        scroll while it is near an edge of the chain."""
        self._drag_pos = pos
        viewport = self.scroll.viewport()
        x = viewport.mapFrom(self, pos).x()
        if x < AUTOSCROLL_EDGE:
            self._scroll_step = -max(2, (AUTOSCROLL_EDGE - x) // 2)
        elif x > viewport.width() - AUTOSCROLL_EDGE:
            self._scroll_step = max(2, (x - viewport.width() + AUTOSCROLL_EDGE) // 2)
        else:
            self._scroll_step = 0
        if self._scroll_step:
            self._autoscroll.start()
        else:
            self._autoscroll.stop()
        self._show_drop_marker(self.drop_index(pos))

    def _auto_scroll(self) -> None:
        bar = self.scroll.horizontalScrollBar()
        bar.setValue(bar.value() + self._scroll_step)
        self._show_drop_marker(self.drop_index(self._drag_pos))

    def _drag_ended(self) -> None:
        self._autoscroll.stop()
        self.drop_marker.hide()

    def _on_devices_changed(self, track_id: str, rebuild: bool = False) -> None:
        if track_id != self.track_id:
            return
        devices = self.project.track(track_id).devices
        widgets = list(self.widgets.values())
        if not rebuild and len(widgets) == len(devices) and all(w.source is d for w, d in zip(widgets, devices)):
            # The same devices in the same order (one switched on or off): no need to rebuild.
            for widget, device in zip(widgets, devices):
                widget.refresh(device)
            return
        old = set(self.widgets)
        self.show_track(track_id)
        added = [i for i in self._chain_ids() if i not in old]
        if added and not self._dropping:
            # After the chain is laid out, so the new device has its place.
            QTimer.singleShot(0, lambda device_id=added[-1]: self._scroll_to(device_id))

    def _scroll_to(self, device_id: str) -> None:
        widget = self.widgets.get(device_id)
        if widget is not None:
            self.chain_layout.activate()  # so it has its place in the chain
            self.scroll.ensureWidgetVisible(widget, 0, 0)

    def _on_param_changed(self, track_id: str, device_id: str, _param_id: str) -> None:
        if track_id == self.track_id and device_id in self.widgets:
            self.widgets[device_id].refresh(self.project.device(track_id, device_id))

    def _on_state_changed(self, track_id: str, device_id: str) -> None:
        widget = self.widgets.get(device_id) if track_id == self.track_id else None
        if isinstance(widget, PluginDeviceWidget):
            widget.refresh_values()

    def _on_plugin_values(self, track_id: str, device_id: str) -> None:
        self._on_state_changed(track_id, device_id)

    def _on_plugin_rebuilt(self, track_id: str, _device_id: str) -> None:
        self._on_devices_changed(track_id, rebuild=True)

    def _on_plugin_editor(self, track_id: str, device_id: str) -> None:
        widget = self.widgets.get(device_id) if track_id == self.track_id else None
        if isinstance(widget, PluginDeviceWidget):
            widget.update_editor_button()

    def _remember_page(self, device_id: str, page: int) -> None:
        self._pages[device_id] = page

    def show_track(self, track_id: str | None) -> None:
        if track_id is not None and not self.project.has_owner(track_id):
            track_id = None
        if track_id != self.track_id:
            self.selected, self._anchor = [], None
        self.track_id = track_id
        self.drop_marker.hide()
        while self.chain_layout.count():
            item = self.chain_layout.takeAt(0)
            if item.widget() and item.widget() is not self.hint:
                item.widget().hide()  # now: until deleted it would still be painted where it was
                item.widget().deleteLater()
        self.widgets.clear()
        if track_id is None:
            self.hint.setText("No track selected")
            self.chain_layout.addWidget(self.hint)
            self.hint.show()
            return
        track = self.project.track(track_id)
        for device in track.devices:
            widget_type = PluginDeviceWidget if device.is_plugin else DeviceWidget
            widget = widget_type(track_id, device, self.editor, self.bridge, self._pages.get(device.id, 0))
            widget.page_changed.connect(self._remember_page)
            widget.pressed.connect(self._on_device_pressed)
            widget.released.connect(self._on_device_released)
            widget.drag_started.connect(self._start_drag)
            widget.menu_requested.connect(self._on_device_menu)
            widget.remove_selected = self.delete_selected
            self.widgets[device.id] = widget
            self.chain_layout.addWidget(widget)
            widget.show()  # now, not on Qt's next pass, so the chain can be laid out at once
        self.chain_layout.addWidget(self.hint)
        self.chain_layout.addStretch(1)
        needs_instrument = track.is_midi and not any(device_is_instrument(d) for d in track.devices)
        self.hint.setText(INSTRUMENT_HINT if needs_instrument else EFFECTS_HINT)
        self.hint.setVisible(needs_instrument or not track.devices)
        kept = [i for i in self.selected if i in self.widgets]
        self.selected = []
        self._set_selected(kept)

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:
        mime = event.mimeData()
        if self.track_id is not None and (device_kinds(mime) or mime.hasFormat(PLUGIN_MIME) or self._moving(mime)):
            event.acceptProposedAction()
            self._drag_at(event.position().toPoint())

    def dragMoveEvent(self, event: QDragMoveEvent) -> None:
        event.acceptProposedAction()
        self._drag_at(event.position().toPoint())

    def dragLeaveEvent(self, _event) -> None:
        self._drag_ended()

    def dropEvent(self, event: QDropEvent) -> None:
        self._drag_ended()
        mime = event.mimeData()
        if self.track_id is not None:
            index = self.drop_index(event.position().toPoint())
            moving = self._moving(mime)
            if moving:
                self.editor.move_devices(self.track_id, moving, index)
                event.acceptProposedAction()
                return
            # New effects go where they were dropped (an instrument always goes first).
            new = [(kind, None) for kind in device_kinds(mime) if kind in BUILTIN_DEVICES]
            new += [(PLUGIN_KIND, ref) for ref in plugin_refs(mime)]
            refused = False
            self._dropping = True
            try:
                for kind, ref in new:
                    count = len(self.project.track(self.track_id).devices)
                    device = self.editor.add_device(self.track_id, kind, index=index, plugin=ref)
                    refused |= device is None
                    chain = [d.id for d in self.project.track(self.track_id).devices]
                    if device is not None and not device_is_instrument(device):
                        index = chain.index(device.id) + 1  # the next one goes after it
                    else:  # a new instrument (not one replacing another) went in first, before the drop point
                        index += len(chain) - count
            finally:
                self._dropping = False
            if refused:
                self.status_message.emit(INSTRUMENT_REFUSED)
        event.acceptProposedAction()

