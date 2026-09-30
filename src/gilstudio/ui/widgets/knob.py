"""Rotary knob: drag vertically, Shift for fine control, double-click resets.
With `log_scale` (for frequencies and times) it moves evenly in log(value);
with `step` it only takes multiples of it (from the minimum)."""

from __future__ import annotations

import math
from collections.abc import Callable

from PySide6.QtCore import QPointF, QRectF, QSize, Qt, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPen, QWheelEvent
from PySide6.QtWidgets import QWidget

from ... import theme

START_ANGLE = 225.0  # degrees, Qt convention (0 = 3 o'clock, counter-clockwise)
SPAN = 270.0


class Knob(QWidget):
    valueChanged = Signal(float, object)  # value, gesture key

    def __init__(self, minimum: float = 0.0, maximum: float = 1.0, value: float = 0.0, *,
                 default: float | None = None, bipolar: bool = False,
                 formatter: Callable[[float], str] | None = None, color: str = theme.ACCENT,
                 log_scale: bool = False, step: float = 0.0, parent: QWidget | None = None):
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
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
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

    def _fraction(self, value: float) -> float:
        if self._log:
            return math.log(value / self._min) / math.log(self._max / self._min)
        return (value - self._min) / (self._max - self._min)

    def _from_fraction(self, fraction: float) -> float:
        fraction = max(0.0, min(1.0, fraction))
        if self._log:
            return self._min * (self._max / self._min) ** fraction
        return self._min + fraction * (self._max - self._min)

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

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self._drag = (event.position().y(), self._value, object())

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._drag is None:
            return
        start_y, start_value, gesture = self._drag
        pixels_for_full_range = 1000.0 if event.modifiers() & Qt.KeyboardModifier.ShiftModifier else 150.0
        delta = (start_y - event.position().y()) / pixels_for_full_range
        self._set_from_user(self._from_fraction(self._fraction(start_value) + delta), gesture)

    def mouseReleaseEvent(self, _event: QMouseEvent) -> None:
        self._drag = None

    def mouseDoubleClickEvent(self, _event: QMouseEvent) -> None:
        self._set_from_user(self._default, object())

    def wheelEvent(self, event: QWheelEvent) -> None:
        notches = event.angleDelta().y() / 120.0
        self._set_from_user(self._from_fraction(self._fraction(self._value) + notches / 50.0), object())
        event.accept()
