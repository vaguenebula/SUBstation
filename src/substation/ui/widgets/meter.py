"""Stereo peak meter with a falling display."""

from __future__ import annotations

import math

from PySide6.QtCore import QRectF, QSize
from PySide6.QtGui import QLinearGradient, QPainter
from PySide6.QtWidgets import QWidget

from ... import theme

FLOOR_DB = -60.0
CEILING_DB = 6.0
FALL_PER_UPDATE = 0.035  # fraction of the scale per update (~30 Hz)


def _fraction(level: float) -> float:
    if level <= 0.0:
        return 0.0
    db = 20.0 * math.log10(level)
    return max(0.0, min(1.0, (db - FLOOR_DB) / (CEILING_DB - FLOOR_DB)))


class MeterWidget(QWidget):
    def __init__(self, parent: QWidget | None = None):
        super().__init__(parent)
        self._display = [0.0, 0.0]
        self._clipped = False
        self.setMinimumWidth(8)

    def sizeHint(self) -> QSize:
        return QSize(10, 40)

    def set_levels(self, left: float, right: float) -> None:
        changed = False
        for i, level in enumerate((left, right)):
            target = _fraction(level)
            value = max(target, self._display[i] - FALL_PER_UPDATE)
            if abs(value - self._display[i]) > 1e-4:
                self._display[i] = value
                changed = True
        if max(left, right) >= 1.0 and not self._clipped:
            self._clipped = True
            changed = True
        if changed:
            self.update()

    def reset(self) -> None:
        self._display = [0.0, 0.0]
        self._clipped = False
        self.update()

    def mousePressEvent(self, _event) -> None:
        self._clipped = False  # click to clear the clip indicator
        self.update()

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        rect = QRectF(self.rect())
        p.fillRect(rect, theme.METER_BG)
        clip_h = 3.0
        bars = rect.adjusted(1, clip_h + 1, -1, -1)
        gap = 1.0
        width = (bars.width() - gap) / 2
        zero_db = 1.0 - (0.0 - FLOOR_DB) / (CEILING_DB - FLOOR_DB)
        gradient = QLinearGradient(0, bars.bottom(), 0, bars.top())
        gradient.setColorAt(0.0, theme.METER_LOW)
        gradient.setColorAt(max(0.0, 1.0 - zero_db - 0.18), theme.METER_LOW)
        gradient.setColorAt(max(0.0, 1.0 - zero_db - 0.05), theme.METER_MID)
        gradient.setColorAt(1.0 - zero_db, theme.METER_HIGH)
        gradient.setColorAt(1.0, theme.METER_HIGH)
        for i, fraction in enumerate(self._display):
            if fraction <= 0.0:
                continue
            h = bars.height() * fraction
            bar = QRectF(bars.left() + i * (width + gap), bars.bottom() - h, width, h)
            p.fillRect(bar, gradient)
        if self._clipped:
            p.fillRect(QRectF(rect.left() + 1, rect.top() + 1, rect.width() - 2, clip_h - 1), theme.METER_HIGH)
