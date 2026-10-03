"""Oscilloscope of the master output, as in FL Studio's tool bar."""

from __future__ import annotations

import numpy as np
from PySide6.QtCore import QPointF, QRectF, QTimer
from PySide6.QtGui import QColor, QPainter, QPen, QPolygonF
from PySide6.QtWidgets import QWidget

from ... import theme

# Samples shown (~21 ms at 48 kHz). Read twice as many to find a trigger in; the
# engine keeps 4096. Fixed, as the engine's sample rate is behind its edit lock.
WINDOW = 1024
UPDATE_MS = 16  # ~60 fps
FADE_PER_UPDATE = 0.8  # while no audio comes, the trace shrinks to a flat line (as fast as 0.64 per 33 ms)
SILENT = 1e-4  # below this the trace counts as flat and stops repainting


class Oscilloscope(QWidget):
    """Draws the latest master output, starting at a rising zero crossing so steady tones stand still.

    `source` is the engine (master_scope() and master_scope_written, which never block).
    """

    def __init__(self, source, parent: QWidget | None = None):
        super().__init__(parent)
        self._source = source
        self._samples = np.zeros(0, dtype=np.float32)
        self._trace = QPolygonF()  # built when the samples change, not on every paint
        self._written = -1
        self.setFixedSize(150, 30)
        self.setToolTip("Master output")
        self._timer = QTimer(self)
        self._timer.setInterval(UPDATE_MS)
        self._timer.timeout.connect(self._poll)
        self._timer.start()

    def _poll(self) -> None:
        if not self.isVisible():
            return
        written = self._source.master_scope_written
        if written == self._written:
            if not self._samples.size:
                return
            if np.abs(self._samples).max() > SILENT:
                self._samples = self._samples * FADE_PER_UPDATE
            else:
                self._samples = np.zeros(0, dtype=np.float32)  # flat: one last repaint, then idle
            self._rebuild()
            return
        self._written = written
        samples = self._source.master_scope(2 * WINDOW)
        self._samples = samples[_trigger(samples, WINDOW):][:WINDOW]
        self._rebuild()

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._rebuild()

    def _rebuild(self) -> None:
        rect = QRectF(self.rect())
        self._trace = _trace(self._samples, rect.left() + 1, int(rect.width()) - 2, rect.center().y(),
                             (rect.height() - 4) / 2)
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
        if self._trace.size() < 2:
            return
        p.setClipRect(rect.adjusted(1, 1, -1, -1))
        # The wide glow is soft anyway, so it skips antialiasing (the costly part); the line keeps it.
        p.setPen(QPen(theme.SCOPE_GLOW, 3.0))
        p.drawPolyline(self._trace)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(QPen(theme.SCOPE_LINE, 1.0))
        p.drawPolyline(self._trace)


def _trace(samples: np.ndarray, left: float, columns: int, mid: float, half: float) -> QPolygonF:
    """One column per pixel: a line through each column's highest and then lowest sample."""
    if samples.size < 2 or columns < 1:
        return QPolygonF()
    edges = np.linspace(0, samples.size, columns + 1).astype(int)[:-1]
    clipped = np.clip(samples, -1.0, 1.0)
    tops = mid - np.maximum.reduceat(clipped, edges) * half
    bottoms = mid - np.minimum.reduceat(clipped, edges) * half
    xs = left + np.arange(columns) + 0.5
    # Top then bottom in each column; the bottom only where the column spans a pixel or more.
    ys = np.column_stack((tops, bottoms)).ravel()
    keep = np.column_stack((np.ones(columns, bool), bottoms - tops >= 1.0)).ravel()
    return QPolygonF([QPointF(x, y) for x, y in zip(np.repeat(xs, 2)[keep].tolist(), ys[keep].tolist())])


def _trigger(samples: np.ndarray, window: int) -> int:
    """Where the shown window starts: the last rising zero crossing that leaves a full window after it."""
    latest = samples.size - window
    if latest <= 0:
        return 0
    crossings = np.flatnonzero((samples[:latest] < 0.0) & (samples[1:latest + 1] >= 0.0)) + 1
    return int(crossings[-1]) if crossings.size else latest
