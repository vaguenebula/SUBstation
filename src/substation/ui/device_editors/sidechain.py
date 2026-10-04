"""The Sidechain device's editor: its curve, big, to draw on; what the kick and
the input (a bass) clash at; and the device's controls.

The curve: drag a point to move it (the first and last only up and down),
drag between points to bend the curve there (as automation bends), click
anywhere else to add a point (and keep dragging it), double-click a point (or
Alt-click it, or select it and press Delete) to remove it, double-click a bend
to straighten it; the wheel bends too, Shift makes any of them fine.
Right-click for shapes to start from. Behind the curve: the kick's envelope
where it clashes (orange), and the curve the fit would draw (dashed); as hits
come, a playhead rides the curve; at the right, the key's level against the
threshold.

The clash view compares the kick's spectrum with the input's and marks where
they clash. Fit (or a click on the view) fits the curve to the kick there
(analysis.sidechain_fit): its points, length and crossover, in one undo step;
Auto fits again at every hit (all one step while nothing else changes), Tight,
Natural or Loose say how long the input keeps out of the way.

Every change goes through ProjectEditor (set_device_params), so undo,
automation and saving work as for any control; a drag is one undo step."""

from __future__ import annotations

import math
import time
from dataclasses import dataclass
from itertools import pairwise

import numpy as np
from PySide6.QtCore import QEvent, QPointF, QRectF, Qt
from PySide6.QtGui import (
    QColor,
    QKeyEvent,
    QLinearGradient,
    QMouseEvent,
    QPainter,
    QPainterPath,
    QPen,
    QWheelEvent,
)
from PySide6.QtWidgets import (
    QComboBox,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QMenu,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from ... import theme
from ...analysis import sidechain_fit
from ...model.automation import device_key
from ...model.params import format_value
from ..device_panel import DeviceWidget
from ..widgets import Knob, ToggleButton
from . import device_editor

POINTS = 16
TRIGGERS = ("Sidechain", "Every Bar", "Every 1/2", "Every 1/4", "Every 1/8", "Every 1/16")
RATES = ("1/32", "1/16", "1/8", "3/16", "1/4", "3/8", "1/2", "1 Bar")
RATE_BEATS = (0.125, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, -1.0)  # -1: a bar
KEEP_KICKS = 3  # the fit averages this many of the latest hits

# Shapes to start from: (x, y, curve) points.
SHAPES = (
    ("Pump", ((0.0, 0.0, 0.0), (0.1, 0.0, -0.45), (1.0, 1.0, 0.0))),
    ("Smooth", ((0.0, 0.0, -0.6), (1.0, 1.0, 0.0))),
    ("Snappy", ((0.0, 0.0, 0.0), (0.06, 0.0, 0.55), (0.45, 1.0, 0.0), (1.0, 1.0, 0.0))),
    ("Linear", ((0.0, 0.0, 0.0), (1.0, 1.0, 0.0))),
    ("Hold", ((0.0, 0.0, 0.0), (0.4, 0.0, 0.0), (0.42, 1.0, 0.0), (1.0, 1.0, 0.0))),
    ("Gentle", ((0.0, 0.45, 0.0), (0.12, 0.45, -0.4), (1.0, 1.0, 0.0))),
    ("Bounce", ((0.0, 0.0, 0.0), (0.12, 0.0, 0.5), (0.3, 0.85, 0.0), (0.36, 0.45, 0.5), (0.7, 1.0, 0.0),
                (1.0, 1.0, 0.0))),
    ("Stutter", ((0.0, 0.0, 0.0), (0.2, 0.0, 0.0), (0.21, 1.0, 0.0), (0.4, 1.0, 0.0), (0.41, 0.0, 0.0),
                 (0.6, 0.0, 0.0), (0.61, 1.0, 0.0), (1.0, 1.0, 0.0))),
)

GRAPH_MIN_WIDTH = 380
FIT_WIDTH = 150
CONTROLS_WIDTH = 168
SPACING = 8
INSET = 6  # px between the controls and the device's right edge
SMALL_KNOB = 26
POINT_RADIUS, POINT_HOVER_RADIUS = 4.5, 6.5
HIT_RADIUS = 9.0
CURVE_HIT = 7.0  # px from the curve where a drag bends it
METER_WIDTH = 6
METER_FLOOR = -60.0
TRAIL = 14  # the playhead's trail, in refreshes

BACKGROUND_TOP, BACKGROUND_BOTTOM = QColor("#1d2027"), QColor("#101216")
GRID_MAJOR, GRID_MINOR = QColor(255, 255, 255, 30), QColor(255, 255, 255, 11)
LABEL_COLOR = QColor(255, 255, 255, 90)
CURVE_COLOR = QColor("#8fe3ff")
DUCK_TOP, DUCK_BOTTOM = QColor(120, 140, 255, 25), QColor(110, 220, 255, 95)
KICK_COLOR = QColor("#ff9f5a")
BASS_COLOR = QColor("#6ea0eb")
CLASH_COLOR = QColor("#ff6fb5")


def point_param(index: int, name: str) -> str:
    """A curve point's parameter id (points from 0)."""
    return f"p{index + 1}_{name}"


def _round_pen(color: QColor, width: float) -> QPen:
    pen = QPen(color, width)
    pen.setCapStyle(Qt.PenCapStyle.RoundCap)
    pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
    return pen


def _alpha(color: QColor, alpha: int) -> QColor:
    result = QColor(color)
    result.setAlpha(alpha)
    return result


def shape(x: float, bend: float) -> float:
    a = -bend * sidechain_fit.CURVATURE
    if abs(a) < 1e-4:
        return x
    return math.expm1(a * x) / math.expm1(a)


@dataclass(frozen=True)
class Point:
    x: float
    y: float  # 1: untouched, 0: ducked by the depth
    curve: float  # how the segment after it bends, -1..1


def curve_value(points: list[Point], x: float) -> float:
    """The curve at x (0..1), as the device plays it."""
    if not points:
        return 1.0
    if x <= points[0].x:
        return points[0].y
    for a, b in pairwise(points):
        if x < b.x:
            span = b.x - a.x
            if span <= 0:
                return b.y
            bend = a.curve if b.y >= a.y else -a.curve
            return a.y + (b.y - a.y) * shape((x - a.x) / span, bend)
    return points[-1].y


def points_values(points: list[Point]) -> dict[str, float]:
    """The parameters for a curve of these points (every slot: the same parameters every time)."""
    values: dict[str, float] = {}
    for i in range(POINTS):
        p = points[i] if i < len(points) else None
        values[point_param(i, "used")] = 1.0 if p else 0.0
        values[point_param(i, "x")] = p.x if p else 1.0
        values[point_param(i, "y")] = p.y if p else 1.0
        values[point_param(i, "curve")] = p.curve if p else 0.0
    return values


def format_ms(ms: float) -> str:
    return f"{ms / 1000:.2f} s" if ms >= 1000 else f"{ms:.0f} ms"


class CurveGraph(QWidget):
    """The curve, its points, the kick behind it and the playhead."""

    def __init__(self, host: SidechainWidget):
        super().__init__()
        self.host = host
        self.hover: tuple[str, int] | None = None  # ("point" | "segment", index)
        self.selected: int | None = None
        self.drag: dict | None = None
        self.readout: tuple[QPointF, str] | None = None  # shown while dragging
        self._added = (0.0, -1)  # (when, which) a click last added a point: its double-click doesn't remove it
        self._wheel: tuple[float, object] = (0.0, object())  # (last wheel time, its gesture)
        self.trail: list[float] = []  # the playhead's latest positions (x 0..1), oldest first
        self.level = METER_FLOOR  # the key's level (dB), falling back slowly
        self.flash = 0.0  # a hit lights the meter up
        self._hint_rect = QRectF()
        self.setMinimumSize(GRAPH_MIN_WIDTH, 100)
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.ClickFocus)
        self.setAccessibleName("Sidechain curve")

    # --- Geometry ------------------------------------------------------------------------

    def plot(self) -> QRectF:
        return QRectF(self.rect()).adjusted(6, 16, -(METER_WIDTH + 12), -14)

    def to_screen(self, x: float, y: float) -> QPointF:
        plot = self.plot()
        return QPointF(plot.left() + x * plot.width(), plot.bottom() - y * plot.height())

    def from_screen(self, pos: QPointF) -> tuple[float, float]:
        plot = self.plot()
        return ((pos.x() - plot.left()) / max(plot.width(), 1.0), (plot.bottom() - pos.y()) / max(plot.height(), 1.0))

    def _curve_path(self, points: list[Point], plot: QRectF) -> QPainterPath:
        steps = max(2, int(plot.width()))
        path = QPainterPath()
        for i in range(steps + 1):
            x = i / steps
            at = self.to_screen(x, curve_value(points, x))
            if i == 0:
                path.moveTo(at)
            else:
                path.lineTo(at)
        return path

    def hit(self, pos: QPointF) -> tuple[str, int] | None:
        """The point under `pos`, else the segment whose curve is within CURVE_HIT."""
        points = self.host.points()
        best, best_distance = None, HIT_RADIUS
        for i, p in enumerate(points):
            d = math.dist((pos.x(), pos.y()), self._xy(self.to_screen(p.x, p.y)))
            if d < best_distance:
                best, best_distance = i, d
        if best is not None:
            return ("point", best)
        x, _y = self.from_screen(pos)
        if not 0.0 <= x <= 1.0 or len(points) < 2:
            return None
        if abs(self.to_screen(x, curve_value(points, x)).y() - pos.y()) > CURVE_HIT:
            return None
        segment = max(0, min(len(points) - 2, sum(1 for p in points if p.x <= x) - 1))
        return ("segment", segment)

    @staticmethod
    def _xy(point: QPointF) -> tuple[float, float]:
        return point.x(), point.y()

    # --- Painting ------------------------------------------------------------------------

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.rect())
        background = QLinearGradient(rect.topLeft(), rect.bottomLeft())
        background.setColorAt(0, BACKGROUND_TOP)
        background.setColorAt(1, BACKGROUND_BOTTOM)
        p.fillRect(rect, background)
        plot = self.plot()
        points = self.host.points()
        length = self.host.length_ms()
        self._grid(p, plot, length)
        fit = self.host.fit
        if fit is not None:
            self._kick(p, plot, fit, length)
        self._curve(p, plot, points)
        self._playhead(p, points)
        self._points(p, points)
        self._meter(p)
        self._labels(p, plot, fit, length)
        if self.readout is not None:
            self._badge(p, *self.readout)
        p.end()

    def _grid(self, p: QPainter, plot: QRectF, length: float) -> None:
        p.setFont(theme.ui_font(7))
        step = next((s for s in (1, 2, 5, 10, 20, 25, 50, 100, 200, 250, 500, 1000) if length / s <= 8), 1000)
        ms = 0.0
        while ms <= length + 1e-6:
            x = plot.left() + ms / length * plot.width()
            p.setPen(QPen(GRID_MAJOR if ms == 0 else GRID_MINOR, 1))
            p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()))
            if 0 < ms < length - step / 2:
                p.setPen(LABEL_COLOR)
                p.drawText(QRectF(x - 24, plot.bottom() + 1, 48, 12), Qt.AlignmentFlag.AlignCenter, format_ms(ms))
            ms += step
        depth = self.host.value("depth") / 100.0
        for y in (0.0, 0.25, 0.5, 0.75, 1.0):
            at = plot.bottom() - y * plot.height()
            p.setPen(QPen(GRID_MAJOR if y in (0.0, 1.0) else GRID_MINOR, 1))
            p.drawLine(QPointF(plot.left(), at), QPointF(plot.right(), at))
            if 0.0 < y < 1.0:  # what the depth makes of it
                gain = 1.0 - depth * (1.0 - y)
                text = f"{20 * math.log10(gain):.0f}" if gain > 1e-3 else "-∞"
                p.setPen(LABEL_COLOR)
                p.drawText(QRectF(plot.right() - 30, at - 12, 28, 11),
                           Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter, text)

    def _kick(self, p: QPainter, plot: QRectF, fit, length: float) -> None:
        """The kick's envelope where it clashes, and the curve the fit calls for (dashed)."""
        times = fit.times
        if len(times) < 2:
            return
        shown = times <= length
        xs = times[shown] / length
        if len(xs) < 2:
            return
        step = max(1, len(xs) // max(1, int(plot.width())))
        xs = xs[::step]
        envelope = fit.envelope[shown][::step]
        target = fit.target[shown][::step]
        area = QPainterPath(self.to_screen(float(xs[0]), 0.0))
        for x, e in zip(xs, envelope, strict=True):
            area.lineTo(self.to_screen(float(x), 0.92 * float(e)))
        area.lineTo(self.to_screen(float(xs[-1]), 0.0))
        area.closeSubpath()
        gradient = QLinearGradient(plot.topLeft(), plot.bottomLeft())
        gradient.setColorAt(0, _alpha(KICK_COLOR, 70))
        gradient.setColorAt(1, _alpha(KICK_COLOR, 12))
        p.fillPath(area, gradient)
        line = QPainterPath(self.to_screen(float(xs[0]), float(target[0])))
        for x, t in zip(xs[1:], target[1:], strict=True):
            line.lineTo(self.to_screen(float(x), float(t)))
        pen = QPen(_alpha(KICK_COLOR, 150), 1.2, Qt.PenStyle.DashLine)
        p.setPen(pen)
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawPath(line)

    def _curve(self, p: QPainter, plot: QRectF, points: list[Point]) -> None:
        path = self._curve_path(points, plot)
        duck = QPainterPath(path)  # what is taken away: from the curve up to untouched
        duck.lineTo(plot.right(), plot.top())
        duck.lineTo(plot.left(), plot.top())
        duck.closeSubpath()
        gradient = QLinearGradient(plot.topLeft(), plot.bottomLeft())
        gradient.setColorAt(0, DUCK_TOP)
        gradient.setColorAt(1, DUCK_BOTTOM)
        p.fillPath(duck, gradient)
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.setPen(_round_pen(_alpha(CURVE_COLOR, 45), 6))
        p.drawPath(path)
        p.setPen(_round_pen(CURVE_COLOR, 1.8))
        p.drawPath(path)
        bending = self.drag is not None and self.drag["kind"] == "bend"
        hovering = self.drag is None and self.hover is not None and self.hover[0] == "segment"
        if bending or hovering:
            index = self.drag["index"] if bending else self.hover[1]
            if index + 1 < len(points):
                a, b = points[index], points[index + 1]
                segment = QPainterPath()
                steps = max(2, int((b.x - a.x) * plot.width()))
                for i in range(steps + 1):
                    x = a.x + (b.x - a.x) * i / steps
                    at = self.to_screen(x, curve_value(points, min(x, b.x - 1e-9)) if i < steps else b.y)
                    if i == 0:
                        segment.moveTo(at)
                    else:
                        segment.lineTo(at)
                p.setPen(_round_pen(QColor(255, 255, 255, 200), 2.6))
                p.drawPath(segment)

    def _playhead(self, p: QPainter, points: list[Point]) -> None:
        for age, x in enumerate(reversed(self.trail)):
            if not 0.0 <= x <= 1.0:
                continue
            at = self.to_screen(x, curve_value(points, x))
            fade = 1.0 - age / TRAIL
            if age == 0:
                plot = self.plot()
                gradient = QLinearGradient(QPointF(at.x(), plot.top()), QPointF(at.x(), plot.bottom()))
                gradient.setColorAt(0, QColor(255, 255, 255, 0))
                gradient.setColorAt(1, QColor(255, 255, 255, 70))
                p.setPen(QPen(gradient, 1))
                p.drawLine(QPointF(at.x(), plot.top()), QPointF(at.x(), plot.bottom()))
                p.setPen(Qt.PenStyle.NoPen)
                p.setBrush(_alpha(CURVE_COLOR, 60))
                p.drawEllipse(at, 9, 9)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(_alpha(QColor(255, 255, 255), int(230 * fade)))
            p.drawEllipse(at, 3.5 * fade + 1, 3.5 * fade + 1)

    def _points(self, p: QPainter, points: list[Point]) -> None:
        for i, point in enumerate(points):
            at = self.to_screen(point.x, point.y)
            hovered = self.hover == ("point", i) or (self.drag is not None and self.drag.get("index") == i
                                                     and self.drag["kind"] == "point")
            radius = POINT_HOVER_RADIUS if hovered else POINT_RADIUS
            if hovered:
                p.setPen(Qt.PenStyle.NoPen)
                p.setBrush(_alpha(CURVE_COLOR, 70))
                p.drawEllipse(at, radius + 4, radius + 4)
            p.setPen(QPen(QColor(255, 255, 255, 230), 1.4))
            p.setBrush(CURVE_COLOR if hovered or i == self.selected else QColor("#15181e"))
            p.drawEllipse(at, radius, radius)

    def _meter(self, p: QPainter) -> None:
        """The key's level against the threshold, at the right; a hit lights it."""
        plot = self.plot()
        rect = QRectF(self.width() - METER_WIDTH - 5, plot.top(), METER_WIDTH, plot.height())

        def y(db: float) -> float:
            return rect.bottom() - (min(0.0, max(METER_FLOOR, db)) - METER_FLOOR) / -METER_FLOOR * rect.height()

        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(255, 255, 255, 14))
        p.drawRoundedRect(rect, 2, 2)
        threshold = self.host.value("threshold")
        top = y(self.level)
        if self.level > METER_FLOOR:
            p.setBrush(_alpha(KICK_COLOR, 210) if self.level >= threshold else QColor(255, 255, 255, 90))
            p.drawRoundedRect(QRectF(rect.left(), top, rect.width(), rect.bottom() - top), 2, 2)
        if self.flash > 0.02:
            p.setBrush(_alpha(KICK_COLOR, int(120 * self.flash)))
            p.drawRoundedRect(rect.adjusted(-2, -2, 2, 2), 3, 3)
        at = y(threshold)
        p.setPen(QPen(QColor(255, 255, 255, 220), 1.5))
        p.drawLine(QPointF(rect.left() - 3, at), QPointF(rect.right() + 1, at))

    def _labels(self, p: QPainter, plot: QRectF, fit, length: float) -> None:
        p.setFont(theme.ui_font(7))
        left = Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter
        if fit is not None:
            p.setPen(_alpha(KICK_COLOR, 200))
            text = f"Kick {fit.spectra.low:.0f}–{fit.spectra.high:.0f} Hz"
            p.drawText(QRectF(plot.left() + 2, 2, 160, 12), left, text)
        p.setPen(LABEL_COLOR)
        right = Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter
        p.drawText(QRectF(plot.right() - 160, 2, 158, 12), right, self.host.length_text())
        self._hint_rect = QRectF()
        hint = self.host.hint()
        if hint:
            p.setFont(theme.ui_font(8))
            width = p.fontMetrics().horizontalAdvance(hint) + 20
            self._hint_rect = QRectF(plot.center().x() - width / 2, plot.center().y() - 11, width, 22)
            p.setPen(QPen(QColor(255, 255, 255, 40), 1))
            p.setBrush(QColor(10, 11, 14, 210))
            p.drawRoundedRect(self._hint_rect, 11, 11)
            p.setPen(QColor(255, 255, 255, 200))
            p.drawText(self._hint_rect, Qt.AlignmentFlag.AlignCenter, hint)

    def _badge(self, p: QPainter, at: QPointF, text: str) -> None:
        p.setFont(theme.ui_font(7))
        width = p.fontMetrics().horizontalAdvance(text) + 10
        rect = QRectF(at.x() + 10, at.y() - 22, width, 15)
        plot = self.plot()
        if rect.right() > plot.right():
            rect.moveRight(at.x() - 10)
        if rect.top() < 0:
            rect.moveTop(at.y() + 8)
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(10, 11, 14, 220))
        p.drawRoundedRect(rect, 4, 4)
        p.setPen(QColor(255, 255, 255, 220))
        p.drawText(rect, Qt.AlignmentFlag.AlignCenter, text)

    # --- Live -------------------------------------------------------------------------------

    def tick(self, phase: float | None, key_peak: float, hit: bool) -> None:
        """A display refresh: the playhead (where on the curve, 0..1; None: not playing one), the
        key's peak since the last, and whether a hit came."""
        if phase is not None:
            self.trail.append(phase)
        elif self.trail:
            self.trail.append(-1.0)
        self.trail = self.trail[-TRAIL:]
        if self.trail and all(x < 0 for x in self.trail):
            self.trail = []
        level = 20 * math.log10(key_peak) if key_peak > 1e-6 else METER_FLOOR
        self.level = max(level, self.level - 1.5)
        self.flash = 1.0 if hit else self.flash * 0.82
        self.update()

    # --- Editing -------------------------------------------------------------------------

    def _readout_text(self, point: Point) -> str:
        depth = self.host.value("depth") / 100.0
        gain = 1.0 - depth * (1.0 - point.y)
        level = f"{20 * math.log10(gain):.1f} dB" if gain > 1e-4 else "-∞ dB"
        return f"{format_ms(point.x * self.host.length_ms())}  ·  {level}"

    def _set(self, points: list[Point], gesture: object | None, text: str = "Change Sidechain Curve") -> None:
        self.host.set_points(points, gesture, text)

    def mousePressEvent(self, event: QMouseEvent) -> None:
        pos = event.position()
        if event.button() != Qt.MouseButton.LeftButton:
            return
        if self._hint_rect.contains(pos):
            self.host.choose_sidechain()
            return
        points = self.host.points()
        hit = self.hit(pos)
        if hit is not None and hit[0] == "point" and event.modifiers() & Qt.KeyboardModifier.AltModifier:
            self.remove(hit[1])
            return
        if hit is not None:
            self._added = (0.0, -1)
        if hit is not None and hit[0] == "point":
            self.selected = hit[1]
            self.drag = {"kind": "point", "index": hit[1], "origin": pos, "points": points, "gesture": object()}
        elif hit is not None:
            self.drag = {"kind": "bend", "index": hit[1], "origin": pos, "points": points, "gesture": object()}
        elif len(points) < POINTS and self.plot().adjusted(-4, -4, 4, 4).contains(pos):
            x, y = self.from_screen(pos)
            x, y = min(max(x, 0.0), 1.0), min(max(y, 0.0), 1.0)
            if points and not 0.0 < x < 1.0:
                return
            index = sum(1 for p in points if p.x <= x)
            before = points[index - 1] if index > 0 else None
            new = Point(x, y, before.curve if before is not None else 0.0)
            points = [*points[:index], new, *points[index:]]
            gesture = object()
            self._set(points, gesture, "Add Sidechain Point")
            self._added = (time.monotonic(), index)
            self.selected = index
            self.drag = {"kind": "point", "index": index, "origin": pos, "points": points, "gesture": gesture,
                         "text": "Add Sidechain Point"}
        self.update()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        pos = event.position()
        if self.drag is None:
            hover = self.hit(pos)
            if hover != self.hover:
                self.hover = hover
                self.update()
            on_hint = self._hint_rect.contains(pos)
            cursor = (Qt.CursorShape.PointingHandCursor if on_hint
                      else Qt.CursorShape.SizeAllCursor if hover is not None and hover[0] == "point"
                      else Qt.CursorShape.SizeVerCursor if hover is not None
                      else Qt.CursorShape.CrossCursor)
            self.setCursor(cursor)
            return
        drag = self.drag
        fine = 0.2 if event.modifiers() & Qt.KeyboardModifier.ShiftModifier else 1.0
        points = list(drag["points"])
        index = drag["index"]
        plot = self.plot()
        dx = (pos.x() - drag["origin"].x()) * fine / max(plot.width(), 1.0)
        dy = (pos.y() - drag["origin"].y()) * fine
        text = drag.get("text", "Change Sidechain Curve")
        if drag["kind"] == "point":
            old = points[index]
            if index == 0 or index == len(points) - 1:
                x = old.x  # the ends stay at the ends
            else:
                x = min(max(old.x + dx, points[index - 1].x), points[index + 1].x)
            y = min(max(old.y - dy / max(plot.height(), 1.0), 0.0), 1.0)
            points[index] = Point(x, y, old.curve)
            self.readout = (self.to_screen(x, y), self._readout_text(points[index]))
        else:
            old = points[index]
            curve = min(max(old.curve - dy / 60.0, -1.0), 1.0)
            points[index] = Point(old.x, old.y, curve)
            self.readout = (pos, f"Bend {curve:+.2f}")
        self._set(points, drag["gesture"], text)
        self.update()

    def mouseReleaseEvent(self, _event: QMouseEvent) -> None:
        self.drag = None
        self.readout = None
        self.update()

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        hit = self.hit(event.position())
        if hit is None or (hit == ("point", self._added[1]) and time.monotonic() - self._added[0] < 0.5):
            return
        if hit[0] == "point":
            self.remove(hit[1])
        else:
            points = self.host.points()
            a = points[hit[1]]
            points[hit[1]] = Point(a.x, a.y, 0.0)
            self._set(points, None, "Straighten Sidechain Curve")

    def remove(self, index: int) -> None:
        points = self.host.points()
        if not 0 < index < len(points) - 1:
            return  # (the ends stay)
        del points[index]
        self.selected = None
        self.hover = None
        self._set(points, None, "Delete Sidechain Point")
        self.update()

    def wheelEvent(self, event: QWheelEvent) -> None:
        hit = self.hit(event.position())
        notches = event.angleDelta().y() / 120.0
        if hit is None or not notches:
            event.ignore()
            return
        points = self.host.points()
        index = hit[1] if hit[0] == "segment" else min(hit[1], len(points) - 2)
        if index < 0:
            return
        fine = 0.25 if event.modifiers() & Qt.KeyboardModifier.ShiftModifier else 1.0
        now = time.monotonic()
        last, gesture = self._wheel
        if now - last > 0.6:
            gesture = object()
        self._wheel = (now, gesture)
        a = points[index]
        points[index] = Point(a.x, a.y, min(max(a.curve + 0.08 * notches * fine, -1.0), 1.0))
        self._set(points, gesture, "Bend Sidechain Curve")
        event.accept()

    def event(self, event: QEvent) -> bool:
        # Delete removes the selected point, not whatever the app has Delete for.
        if event.type() == QEvent.Type.ShortcutOverride and self.selected is not None and isinstance(
                event, QKeyEvent) and event.key() in (Qt.Key.Key_Delete, Qt.Key.Key_Backspace):
            event.accept()
            return True
        return super().event(event)

    def keyPressEvent(self, event: QKeyEvent) -> None:
        if event.key() in (Qt.Key.Key_Delete, Qt.Key.Key_Backspace) and self.selected is not None:
            self.remove(self.selected)
            return
        super().keyPressEvent(event)

    def leaveEvent(self, _event) -> None:
        if self.drag is None:
            self.hover = None
            self.update()

    def contextMenuEvent(self, event) -> None:
        hit = self.hit(QPointF(event.pos()))
        menu = QMenu(self)
        points = self.host.points()
        if hit is not None and hit[0] == "point" and 0 < hit[1] < len(points) - 1:
            menu.addAction("Delete Point", lambda i=hit[1]: self.remove(i))
            menu.addSeparator()
        shapes = menu.addMenu("Shapes")
        for name, shape_points in SHAPES:
            shapes.addAction(name, lambda s=shape_points, n=name: self.host.set_points(
                [Point(*p) for p in s], None, f"Sidechain Shape: {n}"))
        fit = menu.addAction("Fit to Kick", self.host.fit_now)
        fit.setEnabled(self.host.fit is not None)
        menu.addAction("Flip Curve", self._flip)
        menu.addSeparator()
        menu.addAction("Reset Curve", lambda: self.host.set_points(
            [Point(*p) for p in SHAPES[0][1]], None, "Reset Sidechain Curve"))
        menu.exec(event.globalPos())

    def _flip(self) -> None:
        """Up for down: ducking becomes swelling (and the other way round)."""
        self.host.set_points([Point(p.x, 1.0 - p.y, p.curve) for p in self.host.points()], None,
                             "Flip Sidechain Curve")


class ClashView(QWidget):
    """The kick's spectrum against the input's, and where they clash. A click fits the curve."""

    def __init__(self, host: SidechainWidget):
        super().__init__()
        self.host = host
        self.setMinimumHeight(44)
        self.setCursor(Qt.CursorShape.PointingHandCursor)
        self.setToolTip("Where the kick (orange) and the input (blue) clash (pink).\nClick to fit the curve to "
                        "the kick there.")

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        background = QLinearGradient(rect.topLeft(), rect.bottomLeft())
        background.setColorAt(0, BACKGROUND_TOP)
        background.setColorAt(1, BACKGROUND_BOTTOM)
        p.setPen(QPen(QColor(255, 255, 255, 22), 1))
        p.setBrush(background)
        p.drawRoundedRect(rect, 5, 5)
        plot = rect.adjusted(4, 13, -4, -3)
        fit = self.host.fit
        p.setFont(theme.ui_font(7))
        left = Qt.AlignmentFlag.AlignLeft | Qt.AlignmentFlag.AlignVCenter
        if fit is None:
            p.setPen(LABEL_COLOR)
            p.drawText(rect.adjusted(6, 0, -6, 0), Qt.AlignmentFlag.AlignCenter | Qt.TextFlag.TextWordWrap,
                       "Play the kick (and the bass) to fit to it")
            p.end()
            return
        found = fit.spectra
        low, high = math.log(sidechain_fit.CLASH_LOW), math.log(sidechain_fit.CLASH_HIGH)

        def x(freq: float) -> float:
            return plot.left() + (math.log(freq) - low) / (high - low) * plot.width()

        def y(db: float) -> float:
            return plot.bottom() - (max(db, -48.0) + 48.0) / 48.0 * plot.height()

        for freq in (50, 100, 200, 500, 1000):
            p.setPen(QPen(GRID_MINOR, 1))
            p.drawLine(QPointF(x(freq), plot.top()), QPointF(x(freq), plot.bottom()))
        band = QRectF(QPointF(x(found.low), plot.top()), QPointF(x(found.high), plot.bottom()))
        p.fillRect(band, _alpha(CLASH_COLOR, 30))
        xs = [x(float(f)) for f in found.freqs]
        if found.bass_heard:
            clash = QPainterPath(QPointF(xs[0], plot.bottom()))
            for at, db in zip(xs, found.clash, strict=True):
                clash.lineTo(at, y(float(db)))
            clash.lineTo(xs[-1], plot.bottom())
            clash.closeSubpath()
            p.fillPath(clash, _alpha(CLASH_COLOR, 90))
        for values, color in ((found.bass, BASS_COLOR), (found.kick, KICK_COLOR)):
            if values is found.bass and not found.bass_heard:
                continue
            line = QPainterPath(QPointF(xs[0], y(float(values[0]))))
            for at, db in zip(xs[1:], values[1:], strict=True):
                line.lineTo(at, y(float(db)))
            p.setPen(QPen(color, 1.3))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawPath(line)
        p.setPen(_alpha(CLASH_COLOR, 230))
        title = f"Clash {found.low:.0f}–{found.high:.0f} Hz" if found.bass_heard else (
            f"Kick {found.low:.0f}–{found.high:.0f} Hz (no bass)")
        p.drawText(QRectF(rect.left() + 6, rect.top() + 1, rect.width() - 12, 12), left, title)
        p.end()

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self.host.fit_now()


def _caption(text: str) -> QLabel:
    label = QLabel(text)
    label.setAlignment(Qt.AlignmentFlag.AlignCenter)
    label.setStyleSheet(f"color: {theme.TEXT_DIM}; font-size: 7pt;")
    return label


@device_editor("sidechain")
class SidechainWidget(DeviceWidget):
    device_width = GRAPH_MIN_WIDTH + 2 * SPACING + FIT_WIDTH + CONTROLS_WIDTH + INSET + 2

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.defaults = {info.id: info.default_value for info in self.infos}
        self.content.removeItem(self.params)  # its own editor instead of pages of knobs
        self.body.setContentsMargins(0, 0, 0, 0)  # the graph from edge to edge
        self.fit: sidechain_fit.Fit | None = None
        self.kicks: list[np.ndarray] = []
        self.capture = sidechain_fit.Capture(self.sample_rate)
        self._positions: dict[str, int] = {}
        self._hits_seen = 0
        self._auto_gesture: object | None = None
        self.controls: dict[str, tuple[Knob, QLabel, object]] = {}

        self.graph = CurveGraph(self)
        self.clash = ClashView(self)
        fit_column = QVBoxLayout()
        fit_column.setContentsMargins(0, 5, 0, 4)
        fit_column.setSpacing(4)
        fit_column.addWidget(self.clash, 1)
        buttons = QHBoxLayout()
        buttons.setSpacing(4)
        self.fit_button = ToggleButton("Fit", role="small", checkable=False,
                                       tooltip="Fit the curve to the kick, where it clashes with the input")
        self.fit_button.clicked.connect(self.fit_now)
        self.auto = ToggleButton("Auto", role="small", tooltip="Fit again at every hit")
        self.auto.clicked.connect(self._toggle_auto)
        for button in (self.fit_button, self.auto):
            button.setFixedHeight(18)
            buttons.addWidget(button, 1)
        fit_column.addLayout(buttons)
        self.character = QComboBox()
        self.character.addItems(sidechain_fit.CHARACTERS)
        self.character.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.character.setToolTip("How long the fit keeps the input out of the kick's way:\nTight while it is "
                                  "loud, Loose as long as it lingers")
        self.character.activated.connect(self._set_character)
        fit_column.addWidget(self.character)
        fit_holder = QWidget()
        fit_holder.setFixedWidth(FIT_WIDTH)
        fit_holder.setLayout(fit_column)

        controls = QVBoxLayout()
        controls.setContentsMargins(0, 5, 0, 4)
        controls.setSpacing(2)
        self.trigger = QComboBox()
        self.trigger.addItems(TRIGGERS)
        self.trigger.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.trigger.setToolTip("What starts the curve: a hit in the sidechain, or the beat")
        self.trigger.activated.connect(lambda i: self.set_params({"trigger": float(i)}, None, "Change Trigger"))
        self._watch_touch(self.trigger, "trigger")
        self.sync_button = ToggleButton("Sync", role="small", tooltip="The length in notes, at the tempo")
        self.sync_button.clicked.connect(lambda on: self.set_params({"sync": float(on)}, None, "Change Sync"))
        self._watch_touch(self.sync_button, "sync")
        self.sync_button.setFixedSize(40, 18)
        top = QHBoxLayout()
        top.setSpacing(4)
        top.addWidget(self.trigger, 1)
        top.addWidget(self.sync_button)
        controls.addLayout(top)
        controls.addSpacing(2)
        grid = QGridLayout()
        grid.setSpacing(0)
        grid.setVerticalSpacing(1)
        grid.addWidget(self._knob("threshold", "Threshold", -60.0, 0.0, unit="dB"), 0, 0)
        grid.addWidget(self._knob("depth", "Depth", 0.0, 100.0, unit="%"), 0, 1)
        self.length_stack = QStackedWidget()
        self.length_stack.addWidget(self._knob("length", "Length", 10.0, 2000.0, unit="ms", log=True))
        self.length_stack.addWidget(self._knob("rate", "Length", 0.0, len(RATES) - 1.0, step=1.0,
                                               formatter=lambda v: RATES[round(v)]))
        grid.addWidget(self.length_stack, 0, 2)
        grid.addWidget(self._knob("smooth", "Smooth", 0.0, 30.0, unit="ms"), 1, 0)
        grid.addWidget(self._knob("lookahead", "Lookahead", 0.0, 20.0, unit="ms"), 1, 1)
        # Over the crossover, instead of its name: whether only the lows (below it) are ducked.
        self.lows = ToggleButton("Lows Only", role="small", tooltip="Duck only what is below the crossover")
        self.lows.clicked.connect(lambda on: self.set_params({"range": float(on)}, None, "Change Range"))
        self._watch_touch(self.lows, "range")
        self.lows.setFixedHeight(13)
        self.lows.setStyleSheet("font-size: 7pt; padding: 0px 2px;")
        grid.addWidget(self._knob("crossover", "Crossover", 30.0, 1000.0, unit="Hz", log=True, title_widget=self.lows),
                       1, 2)
        controls.addLayout(grid)
        controls.addStretch(1)
        controls_holder = QWidget()
        controls_holder.setFixedWidth(CONTROLS_WIDTH)
        controls_holder.setLayout(controls)

        row = QHBoxLayout()
        row.setContentsMargins(0, 0, INSET, 0)
        row.setSpacing(SPACING)
        row.addWidget(self.graph, 1)
        row.addWidget(fit_holder)
        row.addWidget(controls_holder)
        self.content.addLayout(row, 1)
        self.sync()

    def _set_param_count(self, count: int, page: int = 0) -> None:
        self.param_count, self.pages, self.page = count, 1, 0
        for widget in (self.previous, self.page_label, self.next):
            widget.setVisible(False)

    def _knob(self, param_id: str, title: str, low: float, high: float, *, unit: str = "", log: bool = False,
              step: float = 0.0, formatter=None, title_widget: QWidget | None = None) -> QWidget:
        """A small knob with its name over it and its value under it."""
        formatter = formatter or (lambda v, u=unit: format_value(v, u))
        default = self.defaults.get(param_id, low)
        knob = Knob(low, high, default, default=default, log_scale=log, step=step, formatter=formatter)
        knob.setFixedSize(SMALL_KNOB, SMALL_KNOB)
        knob.valueChanged.connect(lambda v, gesture: self.set_params(
            {param_id: float(round(v)) if step else v}, gesture, f"Change {title}"))
        knob.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        knob.customContextMenuRequested.connect(lambda pos: self._automation_menu(param_id, knob.mapToGlobal(pos)))
        self._watch_touch(knob, param_id)
        readout = _caption("")
        readout.setStyleSheet("font-size: 7pt;")
        cell = QWidget()
        column = QVBoxLayout(cell)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(0)
        column.addWidget(title_widget or _caption(title), 0, Qt.AlignmentFlag.AlignHCenter)
        column.addWidget(knob, 0, Qt.AlignmentFlag.AlignHCenter)
        column.addWidget(readout)
        self.controls[param_id] = (knob, readout, formatter)
        return cell

    # --- The model ------------------------------------------------------------------------

    @property
    def sample_rate(self) -> float:
        return float(self.bridge.engine.sample_rate)

    def value(self, param_id: str) -> float:
        """A parameter as it is now: its automation's value while that plays."""
        if self.automation_state(param_id) == "on":
            value = self.bridge.current_value(self.track_id, device_key(self.device_id, param_id))
            if value is not None:
                return value
        return self.device().params.get(param_id, self.defaults.get(param_id, 0.0))

    def set_params(self, values: dict[str, float], gesture: object | None = None,
                   text: str = "Change Sidechain") -> None:
        self.editor.set_device_params(self.track_id, self.device_id, values, gesture, text)

    def points(self) -> list[Point]:
        """The curve's points, in order of x."""
        points = [Point(self.value(point_param(i, "x")), self.value(point_param(i, "y")),
                        self.value(point_param(i, "curve")))
                  for i in range(POINTS) if self.value(point_param(i, "used")) >= 0.5]
        return sorted(points, key=lambda p: p.x)

    def set_points(self, points: list[Point], gesture: object | None, text: str = "Change Sidechain Curve",
                   extra: dict[str, float] | None = None) -> None:
        self.set_params({**points_values(points), **(extra or {})}, gesture, text)

    def tempo(self) -> float:
        return float(self.editor.project.tempo) or 120.0

    def length_ms(self) -> float:
        if self.value("sync") >= 0.5:
            beats = RATE_BEATS[round(self.value("rate"))]
            if beats < 0:
                beats = 4.0  # a bar (of 4/4)
            return beats * 60000.0 / self.tempo()
        return max(1.0, self.value("length"))

    def length_text(self) -> str:
        if self.value("sync") >= 0.5:
            return f"{RATES[round(self.value('rate'))]}  ·  {format_ms(self.length_ms())}"
        return format_ms(self.length_ms())

    def hint(self) -> str:
        """What the curve shows over it, if anything: how to get going."""
        if round(self.value("trigger")) == 0 and self.device().sidechain is None:
            return "Choose the kick to listen to (the sidechain)…"
        return ""

    def choose_sidechain(self) -> None:
        if self.sidechain is not None:
            self.sidechain_menu().exec(self.graph.mapToGlobal(self.graph.rect().center()))

    # --- Fitting -----------------------------------------------------------------------------

    def fit_values(self, fit: sidechain_fit.Fit) -> dict[str, float]:
        """The parameters a fit sets: the curve, its length (in ms) and the crossover above the clash."""
        crossover = min(1000.0, max(30.0, round(fit.spectra.high * 1.5)))
        return {**points_values([Point(*p) for p in fit.points]), "length": fit.length, "sync": 0.0,
                "crossover": crossover}

    def fit_now(self) -> None:
        if self.fit is None:
            self.bridge.status_message.emit("Sidechain: play the kick (and the bass) first, to fit to it")
            return
        self.set_params(self.fit_values(self.fit), None, "Fit Sidechain to Kick")

    def _toggle_auto(self, on: bool) -> None:
        self._auto_gesture = object() if on else None
        self.set_params({"autofit": float(on)}, None, "Auto Fit" if on else "Stop Auto Fit")
        if on and self.fit is not None:
            self._apply_auto()

    def _apply_auto(self) -> None:
        if self.fit is None or self.graph.drag is not None:
            return
        if self._auto_gesture is None:
            self._auto_gesture = object()
        self.set_params(self.fit_values(self.fit), self._auto_gesture, "Auto Fit Sidechain")

    def _set_character(self, index: int) -> None:
        self.set_params({"character": float(index)}, None, "Change Fit Character")
        self._analyze()
        if self.value("autofit") >= 0.5:
            self._apply_auto()

    def _analyze(self) -> None:
        if not self.kicks:
            return
        pre = int(sidechain_fit.PRE_SECONDS * self.capture.sample_rate)
        fit = sidechain_fit.analyze(self.kicks, self._bass, self.capture.sample_rate,
                                    round(self.value("character")), pre)
        if fit is not None:
            self.fit = fit
        self.graph.update()
        self.clash.update()

    # --- Displays ----------------------------------------------------------------------------

    def _read(self, display_id: str) -> tuple[int, np.ndarray]:
        """The display's values since the last read, and the absolute index of the first."""
        index = self.displays.get(display_id)
        if index is None:
            return 0, np.zeros(0, np.float32)
        try:
            values, position = self.bridge.engine.read_processor_display(
                self.engine_id, index, self._positions.get(display_id, 0))
        except ValueError:  # its processor went before the widget did
            return 0, np.zeros(0, np.float32)
        self._positions[display_id] = position
        values = np.asarray(values)
        return position - len(values), values

    def refresh_displays(self) -> None:
        if self.capture.sample_rate != self.sample_rate:
            self.capture = sidechain_fit.Capture(self.sample_rate)
        for name in ("key", "input", "phase"):
            self.capture.feed(name, *self._read(name))
        hit = self.capture.hit_count != self._hits_seen
        self._hits_seen = self.capture.hit_count
        phase = self.capture.last_phase
        length = self.length_ms() * self.capture.sample_rate / 1000.0
        self.graph.tick(phase / length if phase >= 0 else None, self.capture.take_key_peak(), hit)
        done = self.capture.hits()
        if done:
            self.kicks = [*self.kicks, *(kick for kick, _bass in done)][-KEEP_KICKS:]
            self._bass = done[-1][1]
            self._analyze()
            if self.value("autofit") >= 0.5:
                self._apply_auto()

    _bass = np.zeros(0, np.float32)

    # --- The device view ----------------------------------------------------------------

    def sync(self) -> None:
        """Shows the parameters as they are now."""
        for param_id, (knob, readout, formatter) in self.controls.items():
            value = self.value(param_id)
            knob.setValue(value)
            knob.set_automation(self.automation_state(param_id))
            readout.setText(formatter(value))
        self.trigger.setCurrentIndex(round(self.value("trigger")))
        self.character.setCurrentIndex(round(self.value("character")))
        synced = self.value("sync") >= 0.5
        self.sync_button.set_checked_silently(synced)
        self.length_stack.setCurrentIndex(1 if synced else 0)
        self.lows.set_checked_silently(self.value("range") >= 0.5)
        auto = self.value("autofit") >= 0.5
        self.auto.set_checked_silently(auto)
        if not auto:
            self._auto_gesture = None
        self.crossover_enabled(self.value("range") >= 0.5)
        self.graph.update()
        self.clash.update()

    def crossover_enabled(self, enabled: bool) -> None:
        knob, readout, _formatter = self.controls["crossover"]
        knob.setEnabled(enabled)
        readout.setEnabled(enabled)

    def refresh(self, device) -> None:
        super().refresh(device)
        self.sync()

    def follows_automation(self) -> bool:
        return any(self.automation_state(param_id) == "on" for param_id in (*self.controls, "trigger", "sync",
                                                                             "range"))

    def refresh_automation(self) -> None:
        super().refresh_automation()
        if hasattr(self, "graph"):
            self.sync()
