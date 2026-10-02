"""Compressor controls with downward gain-reduction history and In/Out meters."""

from __future__ import annotations

import numpy as np
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QLinearGradient, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import QWidget

from ... import theme
from ..device_panel import DEVICE_WIDTH, PARAM_WIDTH, DeviceWidget
from . import device_editor

GRAPH_WIDTH = 232
HISTORY = 240  # 256 samples each: about 1.3 s at 48 kHz
REDUCTION_RANGE_DB = 24.0
METER_FLOOR_DB = -60.0
METER_WIDTH = 8


def _alpha(color, opacity: int) -> QColor:
    result = QColor(color)
    result.setAlpha(opacity)
    return result


class ReductionGraph(QWidget):
    """Live reduction history; input includes the current threshold marker."""

    def __init__(self, threshold, parent: QWidget | None = None):
        super().__init__(parent)
        self.threshold = threshold
        self.history = np.zeros(HISTORY, np.float32)
        self.level_in = self.level_out = METER_FLOOR_DB
        self.setFixedWidth(GRAPH_WIDTH)
        self.setMinimumHeight(108)
        self.setAccessibleName("Compressor gain reduction and input/output levels")
        self.setToolTip(
            "Gain reduction grows downward (0–24 dB).\n"
            "In: detector level; the accent notch marks the threshold.\n"
            "Out: output level. Meters span −60 to 0 dBFS."
        )

    def add(self, reduction: np.ndarray, level_in: np.ndarray, level_out: np.ndarray) -> None:
        if len(reduction):
            values = np.asarray(reduction[-HISTORY:], dtype=np.float32)
            values = np.nan_to_num(values, nan=0.0, posinf=REDUCTION_RANGE_DB, neginf=0.0)
            self.history = np.concatenate((self.history[len(values):], np.maximum(values, 0.0)))
        if len(level_in):
            self.level_in = self._level(level_in)
        if len(level_out):
            self.level_out = self._level(level_out)
        self.update()

    @staticmethod
    def _level(values: np.ndarray) -> float:
        return float(np.nan_to_num(values, nan=METER_FLOOR_DB,
                                   posinf=0.0, neginf=METER_FLOOR_DB).max())

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        outer = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        p.setPen(QPen(_alpha(theme.GRID_BEAT, 150), 1))
        p.setBrush(QColor(theme.METER_BG))
        p.drawRoundedRect(outer, 5, 5)

        # A quiet header/footer keeps text clear of the moving waveform.
        plot = QRectF(9, 26, self.width() - 82, self.height() - 53)
        meter_x = self.width() - 48
        meter_in = QRectF(meter_x, plot.top(), METER_WIDTH, plot.height())
        meter_out = QRectF(meter_x + 26, plot.top(), METER_WIDTH, plot.height())
        left = Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter
        right = Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
        center = Qt.AlignmentFlag.AlignCenter

        p.setFont(theme.ui_font(8))
        p.setPen(QColor(theme.TEXT_DIM))
        p.drawText(QRectF(9, 4, plot.width(), 18), left, "Gain reduction")
        for rect, label in ((meter_in, "In"), (meter_out, "Out")):
            p.drawText(QRectF(rect.center().x() - 13, 4, 26, 18), center, label)

        # All grid lines share the same exact mapping as the trace.
        p.setFont(theme.ui_font(7))
        for db in (0, 6, 12, 18, 24):
            y = plot.top() + db / REDUCTION_RANGE_DB * plot.height()
            p.setPen(QPen(_alpha(theme.GRID_BEAT, 180 if db == 0 else 80), 1))
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y))
            p.setPen(QColor(theme.TEXT_DIM))
            p.drawText(QRectF(plot.right() + 2, y - 6, 18, 12), right, str(db))

        depth = np.clip(self.history / REDUCTION_RANGE_DB, 0.0, 1.0) * plot.height()
        step = plot.width() / (HISTORY - 1)
        trace = QPainterPath(QPointF(plot.left(), plot.top() + float(depth[0])))
        for i, d in enumerate(depth[1:], 1):
            trace.lineTo(plot.left() + i * step, plot.top() + float(d))
        area = QPainterPath(trace)
        area.lineTo(plot.right(), plot.top())
        area.lineTo(plot.left(), plot.top())
        area.closeSubpath()
        gradient = QLinearGradient(plot.topLeft(), plot.bottomLeft())
        gradient.setColorAt(0, _alpha(theme.ACCENT, 28))
        gradient.setColorAt(1, _alpha(theme.ACCENT, 112))
        p.save()
        p.setClipRect(plot.adjusted(-1, -1, 1, 1))
        p.fillPath(area, gradient)
        p.setPen(QPen(QColor(theme.ACCENT), 1.5))
        p.drawPath(trace)  # Only the signal edge is stroked, not the filled polygon.
        p.restore()

        threshold = float(self.threshold())
        if not np.isfinite(threshold):
            threshold = METER_FLOOR_DB
        self._meter(p, meter_in, self.level_in, threshold)
        self._meter(p, meter_out, self.level_out, None)

        # Current reduction is visually distinct from the threshold annotation.
        footer_y = plot.bottom() + 5
        p.setFont(theme.ui_font(9))
        p.setPen(QColor(theme.ACCENT))
        reduction = float(self.history[-1])
        text = f"−{reduction:.1f} dB" if reduction >= 0.05 else "0.0 dB"
        p.drawText(QRectF(9, footer_y, 70, 19), left, text)
        p.setFont(theme.ui_font(7))
        p.setPen(QColor(theme.TEXT_DIM))
        p.drawText(QRectF(80, footer_y, plot.right() - 80, 19), right,
                   f"T {threshold:.0f}")
        for rect, level in ((meter_in, self.level_in), (meter_out, self.level_out)):
            label = "−∞" if level <= METER_FLOOR_DB else f"{level:.0f}"
            p.drawText(QRectF(rect.center().x() - 13, footer_y, 26, 19), center, label)
        p.end()

    @staticmethod
    def _meter(p: QPainter, rect: QRectF, level: float, marker: float | None) -> None:
        def y(db: float) -> float:
            fraction = (min(0.0, max(METER_FLOOR_DB, db)) - METER_FLOOR_DB) / -METER_FLOOR_DB
            return rect.bottom() - fraction * rect.height()

        p.fillRect(rect, QColor(theme.PANEL))
        top = y(level)
        p.fillRect(QRectF(rect.left(), top, rect.width(), rect.bottom() - top),
                   QColor(theme.METER_LOW))
        # Only the hot section changes color, preserving the meter's level scale.
        if level > -6.0:
            p.fillRect(QRectF(rect.left(), top, rect.width(), y(-6.0) - top),
                       QColor(theme.METER_HIGH))
        p.setPen(QPen(_alpha(theme.METER_BG, 145), 1))
        for db in (-48, -36, -24, -12):
            p.drawLine(QPointF(rect.left(), y(db)), QPointF(rect.right(), y(db)))
        # A separate cap makes near-full-scale levels visible at a glance.
        cap = QRectF(rect.left(), rect.top() - 4, rect.width(), 2)
        p.fillRect(cap, QColor(theme.METER_HIGH) if level > -0.1
                   else _alpha(theme.GRID_BEAT, 100))
        if marker is not None:
            marker_y = y(marker)
            p.setPen(QPen(QColor(theme.METER_BG), 3.5))
            p.drawLine(QPointF(rect.left() - 2, marker_y), QPointF(rect.right() + 2, marker_y))
            p.setPen(QPen(QColor(theme.ACCENT), 1.5))
            p.drawLine(QPointF(rect.left() - 2, marker_y), QPointF(rect.right() + 2, marker_y))
            notch = QPainterPath(QPointF(rect.left() - 6, marker_y - 3))
            notch.lineTo(rect.left() - 3, marker_y)
            notch.lineTo(rect.left() - 6, marker_y + 3)
            notch.closeSubpath()
            p.fillPath(notch, QColor(theme.ACCENT))


@device_editor("compressor")
class CompressorWidget(DeviceWidget):
    params_per_page = 8
    param_columns = 4
    device_width = DEVICE_WIDTH + 2 * (PARAM_WIDTH + 16) + 12 + GRAPH_WIDTH

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.graph = ReductionGraph(self._threshold)
        self.content.addWidget(self.graph)

    def _threshold(self) -> float:
        return self.device().params.get("threshold", -18.0)

    def refresh_displays(self) -> None:
        self.graph.add(self.read_display("reduction"), self.read_display("input"),
                       self.read_display("output"))
