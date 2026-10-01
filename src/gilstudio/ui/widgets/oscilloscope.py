"""Oscilloscope of the master output, as in FL Studio's tool bar."""

from __future__ import annotations

import numpy as np
from PySide6.QtCore import QPointF, QRectF, QTimer
from PySide6.QtGui import QColor, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import QWidget

from ... import theme

# Samples shown (~21 ms at 48 kHz). Read twice as many to find a trigger in; the
# engine keeps 4096. Fixed, as the engine's sample rate is behind its edit lock.
WINDOW = 1024
FADE_PER_UPDATE = 0.8  # while no audio comes, the trace shrinks to a flat line


class Oscilloscope(QWidget):
    """Draws the latest master output, starting at a rising zero crossing so steady tones stand still.

    `source` is the engine (master_scope() and master_scope_written, which never block).
    """

    def __init__(self, source, parent: QWidget | None = None):
        super().__init__(parent)
        self._source = source
        self._samples = np.zeros(0, dtype=np.float32)
        self._written = -1
        self.setFixedSize(150, 30)
        self.setToolTip("Master output")
        self._timer = QTimer(self)
        self._timer.setInterval(16)
        self._timer.timeout.connect(self._poll)
        self._timer.start()

    def _poll(self) -> None:
        if not self.isVisible():
            return
        written = self._source.master_scope_written
        if written == self._written:
            if self._samples.size and np.abs(self._samples).max() > 1e-4:
                self._samples = self._samples * FADE_PER_UPDATE
                self.update()
            return
        self._written = written
        samples = self._source.master_scope(2 * WINDOW)
        self._samples = samples[_trigger(samples, WINDOW):][:WINDOW]
        self.update()

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        rect = QRectF(self.rect())
        p.fillRect(rect, theme.METER_BG)
        p.setPen(QPen(theme.SCOPE_AXIS, 1.0))
        mid = rect.center().y()
        p.drawLine(QPointF(rect.left(), mid), QPointF(rect.right(), mid))
        p.setPen(QPen(QColor(theme.BORDER), 1.0))
        p.drawRect(rect.adjusted(0, 0, -1, -1))
        if self._samples.size < 2:
            return

        # One column per pixel: a line through each column's lowest and highest sample.
        columns = int(rect.width()) - 2
        half = (rect.height() - 4) / 2
        edges = np.linspace(0, self._samples.size, columns + 1).astype(int)
        path = QPainterPath()
        for x in range(columns):
            chunk = self._samples[edges[x]:max(edges[x + 1], edges[x] + 1)]
            low = float(np.clip(chunk.min(), -1.0, 1.0))
            high = float(np.clip(chunk.max(), -1.0, 1.0))
            px = rect.left() + 1 + x + 0.5
            top, bottom = mid - high * half, mid - low * half
            if x == 0:
                path.moveTo(px, top)
            else:
                path.lineTo(px, top)
            if bottom - top >= 1.0:
                path.lineTo(px, bottom)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setClipRect(rect.adjusted(1, 1, -1, -1))
        p.setPen(QPen(theme.SCOPE_GLOW, 3.0))
        p.drawPath(path)
        p.setPen(QPen(theme.SCOPE_LINE, 1.0))
        p.drawPath(path)


def _trigger(samples: np.ndarray, window: int) -> int:
    """Where the shown window starts: the last rising zero crossing that leaves a full window after it."""
    latest = samples.size - window
    if latest <= 0:
        return 0
    crossings = np.flatnonzero((samples[:latest] < 0.0) & (samples[1:latest + 1] >= 0.0)) + 1
    return int(crossings[-1]) if crossings.size else latest
