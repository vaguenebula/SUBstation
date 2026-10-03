"""The automation controls in a track's (or the master's) header, as in
Ableton: for its own lane, a device chooser ("Mixer" or one of its devices) and
a parameter chooser, and a button that shows another lane below; each lane
below has its own choosers and a button that removes it. Automated parameters
are marked in the menus."""

from __future__ import annotations

from PySide6.QtCore import QObject, QPoint, QRect, QRectF, QSize, Qt, Signal
from PySide6.QtGui import QColor, QIcon, QMouseEvent, QPainter, QPixmap
from PySide6.QtWidgets import QMenu, QPushButton, QWidget

from ... import theme
from ...audio.engine_bridge import EngineBridge
from ...model import automation
from ...model.editor import ProjectEditor
from .automation_lanes import ENVELOPE

CHOOSER_HEIGHT = 18
BUTTON_SIZE = 18


def _dot(color: QColor) -> QIcon:
    pixmap = QPixmap(10, 10)
    pixmap.fill(Qt.GlobalColor.transparent)
    p = QPainter(pixmap)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    p.setPen(Qt.PenStyle.NoPen)
    p.setBrush(color)
    p.drawEllipse(QRectF(2, 2, 6, 6))
    p.end()
    return QIcon(pixmap)


class Chooser(QWidget):
    """A compact drop-down: a click asks for its menu."""

    clicked = Signal()

    def __init__(self, tooltip: str, parent: QWidget | None = None):
        super().__init__(parent)
        self._text = ""
        self.setToolTip(tooltip)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)

    def sizeHint(self) -> QSize:
        return QSize(80, CHOOSER_HEIGHT)

    def text(self) -> str:
        return self._text

    def set_text(self, text: str) -> None:
        if text != self._text:
            self._text = text
            self.update()

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        p.setPen(QColor(theme.BORDER))
        p.setBrush(QColor(theme.SURFACE))
        p.drawRoundedRect(rect, 2, 2)
        p.setFont(theme.ui_font(7.5))
        p.setPen(QColor(theme.TEXT))
        text_rect = self.rect().adjusted(4, 0, -12, 0)
        p.drawText(text_rect, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft,
                   p.fontMetrics().elidedText(self._text, Qt.TextElideMode.ElideRight, text_rect.width()))
        p.setPen(QColor(theme.TEXT_DIM))
        p.drawText(self.rect().adjusted(0, 0, -3, 0), Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight,
                   "▾")

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self.clicked.emit()


def _button(text: str, tooltip: str, parent: QWidget) -> QPushButton:
    button = QPushButton(text, parent)
    button.setProperty("role", "small")
    button.setStyleSheet("padding: 0px; font-size: 10pt; font-weight: 600;")
    button.setFixedSize(BUTTON_SIZE, CHOOSER_HEIGHT)
    button.setFocusPolicy(Qt.FocusPolicy.NoFocus)
    button.setToolTip(tooltip)
    return button


class _LaneControls:
    """One lane's device and parameter choosers, and its button."""

    def __init__(self, controls: AutomationControls, lane: int, parent: QWidget):
        self.lane = lane
        self.device = Chooser("Device (or the mixer) to show automation of", parent)
        self.param = Chooser("Parameter to show automation of", parent)
        if lane < 0:
            self.button = _button("+", "Show automation in a new lane", parent)
            self.button.clicked.connect(lambda: controls.editor.add_automation_lane(controls.owner))
        else:
            self.button = _button("−", "Remove this lane", parent)
            self.button.clicked.connect(lambda: controls.editor.remove_automation_lane(controls.owner, self.lane))
        self.device.clicked.connect(lambda: controls.device_menu(self))
        self.param.clicked.connect(lambda: controls.param_menu(self))

    def widgets(self) -> tuple[QWidget, ...]:
        return self.device, self.param, self.button

    def place(self, rect: QRect | None) -> None:
        for widget in self.widgets():
            widget.setVisible(rect is not None)
        if rect is None:
            return
        chooser = (rect.width() - BUTTON_SIZE - 8) // 2
        y = rect.top() + (rect.height() - CHOOSER_HEIGHT) // 2
        self.device.setGeometry(rect.left(), y, chooser, CHOOSER_HEIGHT)
        self.param.setGeometry(rect.left() + chooser + 4, y, chooser, CHOOSER_HEIGHT)
        self.button.setGeometry(rect.right() - BUTTON_SIZE + 1, y, BUTTON_SIZE, CHOOSER_HEIGHT)

    def delete(self) -> None:
        for widget in self.widgets():
            widget.hide()
            widget.deleteLater()


class AutomationControls(QObject):
    """An owner's automation choosers in its header widget. The header places
    them (`place`) where its own lane's and each lane's below are."""

    def __init__(self, owner: str, editor: ProjectEditor, bridge: EngineBridge, parent: QWidget):
        super().__init__(parent)
        self.owner = owner
        self.editor = editor
        self.project = editor.project
        self.bridge = bridge
        self.parent_widget = parent
        self.main = _LaneControls(self, -1, parent)
        self.lanes: list[_LaneControls] = []

    def key(self, lane: int) -> str | None:
        view = self.project.automation_view(self.owner)
        if lane < 0:
            return view.key
        return view.lanes[lane] if lane < len(view.lanes) else None

    def place(self, main: QRect | None, lanes: list[QRect]) -> None:
        """Where the choosers go: in the owner's own lane (None: hidden) and in each lane below."""
        while len(self.lanes) > len(lanes):
            self.lanes.pop().delete()
        while len(self.lanes) < len(lanes):
            controls = _LaneControls(self, len(self.lanes), self.parent_widget)
            self.lanes.append(controls)
        self.main.place(main)
        for controls, rect in zip(self.lanes, lanes, strict=True):
            controls.place(rect)
        self.refresh()

    def refresh(self) -> None:
        for controls in [self.main, *self.lanes]:
            key = self.key(controls.lane)
            spec = self.bridge.param_spec(self.owner, key) if key else None
            controls.device.set_text(spec.group if spec else "None")
            controls.param.set_text(spec.name if spec else "None")

    # --- Menus ----------------------------------------------------------------------

    def _group_of(self, key: str | None) -> str | None:
        if key is None:
            return None
        return "mixer" if automation.is_mixer_key(key) else automation.key_device(key)

    def _choose(self, controls: _LaneControls, key: str) -> None:
        self.editor.set_automation_lane(self.owner, controls.lane, key)

    def device_menu(self, controls: _LaneControls) -> None:
        menu = QMenu(self.parent_widget)
        current = self._group_of(self.key(controls.lane))
        automated = self.project.automation(self.owner)
        for group_id, name, specs in self.bridge.param_groups(self.owner):
            if not specs:
                menu.addAction(name).setEnabled(False)
                continue
            # A device's automated parameter first, as Ableton does.
            key = next((s.key for s in specs if s.key in automated), specs[0].key)
            action = menu.addAction(name, lambda k=key: self._choose(controls, k))
            action.setCheckable(True)
            action.setChecked(group_id == current)
            if any(s.key in automated for s in specs):
                action.setIcon(_dot(ENVELOPE))
        menu.exec(controls.device.mapToGlobal(QPoint(0, controls.device.height())))

    def param_menu(self, controls: _LaneControls) -> None:
        menu = QMenu(self.parent_widget)
        key = self.key(controls.lane)
        current = self._group_of(key)
        automated = self.project.automation(self.owner)
        for group_id, _name, specs in self.bridge.param_groups(self.owner):
            if group_id != current:
                continue
            for spec in specs:
                action = menu.addAction(spec.name, lambda k=spec.key: self._choose(controls, k))
                action.setCheckable(True)
                action.setChecked(spec.key == key)
                if spec.key in automated:
                    action.setIcon(_dot(ENVELOPE))
        if menu.isEmpty():
            menu.addAction("No parameters").setEnabled(False)
        menu.exec(controls.param.mapToGlobal(QPoint(0, controls.param.height())))
