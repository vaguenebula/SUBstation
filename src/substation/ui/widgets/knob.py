"""Rotary knob: drag vertically, Shift for fine control, double-click resets.
With `log_scale` (for frequencies and times) it moves evenly in log(value);
with `step` it only takes multiples of it (from the minimum). A dot in its
corner marks it automated (red) or its automation overridden (grey)."""

from __future__ import annotations

import math
from collections.abc import Callable

from PySide6.QtCore import QPointF, QRectF, QSize, Qt, Signal
from PySide6.QtGui import QColor, QKeyEvent, QMouseEvent, QPainter, QPen, QWheelEvent
from PySide6.QtWidgets import QLineEdit, QWidget

from ... import theme

AUTOMATION_COLORS = {"on": "#ff4a3d", "off": "#8c8c8c"}  # automated / automation overridden
START_ANGLE = 225.0  # degrees, Qt convention (0 = 3 o'clock, counter-clockwise)
SPAN = 270.0


class Knob(QWidget):
    valueChanged = Signal(float, object)  # value, gesture key

    def __init__(self, minimum: float = 0.0, maximum: float = 1.0, value: float = 0.0, *,
                 default: float | None = None, bipolar: bool = False,
                 formatter: Callable[[float], str] | None = None, color: str = theme.ACCENT,
                 log_scale: bool = False, step: float = 0.0,
                 parser: Callable[[str], float | None] | None = None, wheel: bool = True, parent: QWidget | None = None):
        super().__init__(parent)
        self._min = minimum
        self._max = maximum
        self._log = log_scale and minimum > 0
        self._step = step
        self._value = value
        self._default = value if default is None else default
        self._bipolar = bipolar
        self._format = formatter or (lambda v: f"{v:.2f}")
        self._color = QColor(color)
        self._drag: tuple[float, float, object] | None = None
        self._automation: str | None = None
        self._wheel = wheel
        self._parse = parser  # if set: typing a number edits
        self._editor: QLineEdit | None = None
        self.relative = True  # whether the last user change was a drag/wheel (vs typed or reset)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus if parser is None else Qt.FocusPolicy.ClickFocus)
        self.setMinimumSize(22, 22)
        self._update_tooltip()

    def sizeHint(self) -> QSize:
        return QSize(28, 28)

    def value(self) -> float:
        return self._value

    def setValue(self, value: float) -> None:
        value = max(self._min, min(self._max, value))
        if value != self._value:
            self._value = value
            self._update_tooltip()
            self.update()

    def set_color(self, color) -> None:
        self._color = QColor(color)
        self.update()

    def _fraction(self, value: float) -> float:
        if self._log:
            return math.log(value / self._min) / math.log(self._max / self._min)
        return (value - self._min) / (self._max - self._min)

    def _from_fraction(self, fraction: float) -> float:
        fraction = max(0.0, min(1.0, fraction))
        if self._log:
            return self._min * (self._max / self._min) ** fraction
        return self._min + fraction * (self._max - self._min)

    def automation(self) -> str | None:
        return self._automation

    def set_automation(self, state: str | None) -> None:
        """None, "on" (automated) or "off" (automation overridden)."""
        if state != self._automation:
            self._automation = state
            self.update()

    def set_formatter(self, formatter: Callable[[float], str]) -> None:
        self._format = formatter
        self._update_tooltip()

    def _set_from_user(self, value: float, gesture: object) -> None:
        if self._step > 0:
            value = self._min + round((value - self._min) / self._step) * self._step
        value = max(self._min, min(self._max, value))
        if value != self._value:
            self._value = value
            self._update_tooltip()
            self.update()
            self.valueChanged.emit(value, gesture)

    def _update_tooltip(self) -> None:
        self.setToolTip(self._format(self._value))

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        side = min(self.width(), self.height()) - 4
        rect = QRectF((self.width() - side) / 2, (self.height() - side) / 2, side, side)
        track_pen = QPen(QColor(theme.SURFACE_HOVER), 3, Qt.PenStyle.SolidLine, Qt.PenCapStyle.FlatCap)
        p.setPen(track_pen)
        p.drawArc(rect, int((START_ANGLE - SPAN) * 16), int(SPAN * 16))

        origin = 0.5 if self._bipolar else 0.0
        frac = self._fraction(self._value)
        start = START_ANGLE - origin * SPAN
        sweep = -(frac - origin) * SPAN
        p.setPen(QPen(self._color, 3, Qt.PenStyle.SolidLine, Qt.PenCapStyle.FlatCap))
        if abs(sweep) > 0.5:
            p.drawArc(rect, int(start * 16), int(sweep * 16))

        angle = math.radians(START_ANGLE - frac * SPAN)
        center = rect.center()
        radius = side / 2 - 2
        tip = QPointF(center.x() + math.cos(angle) * radius, center.y() - math.sin(angle) * radius)
        p.setPen(QPen(QColor(theme.TEXT), 2, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap))
        p.drawLine(QPointF(center.x() + math.cos(angle) * radius * 0.3,
                           center.y() - math.sin(angle) * radius * 0.3), tip)
        draw_automation_dot(p, self._automation, QPointF(self.width() - 3.5, 3.5))

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self._drag = (event.position().y(), self._value, object())

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._drag is None:
            return
        start_y, start_value, gesture = self._drag
        self.relative = True
        pixels_for_full_range = 1000.0 if event.modifiers() & Qt.KeyboardModifier.ShiftModifier else 150.0
        delta = (start_y - event.position().y()) / pixels_for_full_range
        self._set_from_user(self._from_fraction(self._fraction(start_value) + delta), gesture)

    def mouseReleaseEvent(self, _event: QMouseEvent) -> None:
        self._drag = None

    def mouseDoubleClickEvent(self, _event: QMouseEvent) -> None:
        self.relative = False
        self._set_from_user(self._default, object())

    def keyPressEvent(self, event: QKeyEvent) -> None:
        if self._parse is not None and event.text() and event.text() in "0123456789-+.":
            self._open_editor(event.text())
        else:
            super().keyPressEvent(event)

    def _open_editor(self, initial: str) -> None:
        if self._editor:
            return
        editor = QLineEdit(initial, self.window())
        editor.setAlignment(Qt.AlignmentFlag.AlignCenter)
        width = max(48, editor.fontMetrics().horizontalAdvance("100%") + 16)
        origin = self.mapTo(self.window(), self.rect().center())
        editor.setGeometry(origin.x() - width // 2, origin.y() - 10, width, 20)
        editor.show()
        editor.raise_()
        editor.setFocus()
        editor.editingFinished.connect(lambda: self._close_editor(editor))
        self._editor = editor

    def _close_editor(self, editor: QLineEdit) -> None:
        if self._editor is not editor:
            return
        self._editor = None
        parsed = self._parse(editor.text()) if self._parse else None
        editor.deleteLater()
        if parsed is not None:
            self.relative = False
            self._set_from_user(parsed, object())

    def wheelEvent(self, event: QWheelEvent) -> None:
        if not self._wheel:
            event.ignore()
            return
        self.relative = True
        notches = event.angleDelta().y() / 120.0
        self._set_from_user(self._from_fraction(self._fraction(self._value) + notches / 50.0), object())
        event.accept()


def draw_automation_dot(p: QPainter, state: str | None, at: QPointF) -> None:
    if state is None:
        return
    p.save()
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    p.setPen(Qt.PenStyle.NoPen)
    p.setBrush(QColor(AUTOMATION_COLORS[state]))
    p.drawEllipse(at, 2.5, 2.5)
    p.restore()
