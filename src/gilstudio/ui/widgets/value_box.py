"""Ableton-style numeric field: drag vertically to change, double-click to type."""

from __future__ import annotations

import time
from collections.abc import Callable, Sequence

from PySide6.QtCore import QRectF, QSize, Qt, Signal
from PySide6.QtGui import QColor, QFontMetrics, QMouseEvent, QPainter, QWheelEvent
from PySide6.QtWidgets import QLineEdit, QWidget

from ... import theme


class ValueBox(QWidget):
    # (new value, gesture key). Values from one drag share a key so undo merges them.
    valueChanged = Signal(float, object)

    def __init__(self, value: float = 0.0, minimum: float = 0.0, maximum: float = 1.0, *,
                 step: float = 0.01, decimals: int = 2,
                 formatter: Callable[[float], str] | None = None,
                 parser: Callable[[str], float | None] | None = None,
                 choices: Sequence[float] | None = None, sample_text: str | None = None,
                 parent: QWidget | None = None):
        super().__init__(parent)
        self._value = value
        self._min = minimum
        self._max = maximum
        self._step = step
        self._decimals = decimals
        self._format = formatter or (lambda v: f"{v:.{decimals}f}")
        self._parse = parser or _parse_float
        self._choices = list(choices) if choices else None
        self._sample_text = sample_text
        self._drag_origin: tuple[float, float] | None = None
        self._gesture: object | None = None
        self._wheel_gesture: tuple[object, float] | None = None
        self._editor: QLineEdit | None = None
        self._hover = False
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setCursor(Qt.CursorShape.SizeVerCursor)
        self.setMinimumHeight(20)

    # --- Value -------------------------------------------------------------------

    def value(self) -> float:
        return self._value

    def setValue(self, value: float) -> None:
        value = self._constrain(value)
        if value != self._value:
            self._value = value
            self.update()

    def _constrain(self, value: float) -> float:
        if self._choices:
            return min(self._choices, key=lambda c: abs(c - value))
        return round(max(self._min, min(self._max, value)), self._decimals)

    def _set_from_user(self, value: float, gesture: object) -> None:
        value = self._constrain(value)
        if value != self._value:
            self._value = value
            self.update()
            self.valueChanged.emit(value, gesture)

    # --- Painting ----------------------------------------------------------------

    def sizeHint(self) -> QSize:
        metrics = QFontMetrics(self.font())
        text = self._sample_text or self._format(self._max)
        return QSize(metrics.horizontalAdvance(text) + 16, max(20, metrics.height() + 6))

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        dragging = self._drag_origin is not None
        background = QColor(theme.SURFACE_HOVER if (self._hover or dragging) else theme.SURFACE)
        p.setPen(QColor(theme.BORDER))
        p.setBrush(background)
        p.drawRoundedRect(rect, 3, 3)
        p.setPen(QColor(theme.ACCENT if dragging else theme.TEXT))
        p.drawText(rect, Qt.AlignmentFlag.AlignCenter, self._format(self._value))

    def enterEvent(self, _event) -> None:
        self._hover = True
        self.update()

    def leaveEvent(self, _event) -> None:
        self._hover = False
        self.update()

    # --- Interaction -------------------------------------------------------------

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self._drag_origin = (event.position().y(), self._value)
            self._gesture = object()
            self.update()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._drag_origin is None:
            return
        start_y, start_value = self._drag_origin
        dy = start_y - event.position().y()
        if self._choices:
            index = self._choices.index(self._constrain(start_value)) + int(dy / 10)
            self._set_from_user(self._choices[max(0, min(len(self._choices) - 1, index))], self._gesture)
            return
        fine = event.modifiers() & Qt.KeyboardModifier.ShiftModifier
        self._set_from_user(start_value + dy * self._step * (0.1 if fine else 1.0), self._gesture)

    def mouseReleaseEvent(self, _event: QMouseEvent) -> None:
        self._drag_origin = None
        self._gesture = None
        self.update()

    def mouseDoubleClickEvent(self, _event: QMouseEvent) -> None:
        self._open_editor()

    def wheelEvent(self, event: QWheelEvent) -> None:
        notches = event.angleDelta().y() / 120.0
        if not notches:
            return
        now = time.monotonic()
        if self._wheel_gesture is None or now - self._wheel_gesture[1] > 0.6:
            self._wheel_gesture = (object(), now)
        self._wheel_gesture = (self._wheel_gesture[0], now)
        if self._choices:
            index = self._choices.index(self._value) + (1 if notches > 0 else -1)
            self._set_from_user(self._choices[max(0, min(len(self._choices) - 1, index))], self._wheel_gesture[0])
        else:
            self._set_from_user(self._value + notches * self._step * 10, self._wheel_gesture[0])
        event.accept()

    def _open_editor(self) -> None:
        if self._editor:
            return
        editor = QLineEdit(self._format(self._value).split(" ")[0], self)
        editor.setGeometry(self.rect())
        editor.setAlignment(Qt.AlignmentFlag.AlignCenter)
        editor.selectAll()
        editor.setFocus()
        editor.show()
        editor.editingFinished.connect(lambda: self._close_editor(editor))
        self._editor = editor

    def _close_editor(self, editor: QLineEdit) -> None:
        if self._editor is not editor:
            return
        self._editor = None
        parsed = self._parse(editor.text())
        editor.deleteLater()
        if parsed is not None:
            self._set_from_user(parsed, object())


def _parse_float(text: str) -> float | None:
    cleaned = text.strip().lower()
    for suffix in ("db", "bpm", "%"):
        cleaned = cleaned.removesuffix(suffix).strip()
    if cleaned in ("-inf", "inf"):
        return -70.0
    try:
        return float(cleaned)
    except ValueError:
        return None
