"""Automation lanes in the arrangement: envelopes drawn over the timeline and
edited with the mouse. Tracks show theirs in their own lanes (over the clips,
below the clips' title band) and in lanes below them (LanesCanvas); the master
in its lane (MasterLane). Both hosts hand their lanes to the functions here as
EnvelopeAreas.

In a lane (as in Ableton):
- A press on the envelope's line adds a breakpoint on it (on the grid when
  snapping is on; Alt-click where there is no segment to bend: off the grid),
  and dragging on places it. Where it would go shows while the mouse is over
  the line. A click off the line adds nothing.
- A click on a breakpoint deletes it; Shift- or Ctrl-click selects it instead
  (with the others selected), and Delete deletes the selected ones.
- Near the line between two breakpoints (not on it), the segment lights up:
  dragging it moves both breakpoints, in time and value. A step (two
  breakpoints at the same time) is a segment too, grabbed anywhere along it.
- Dragging a breakpoint moves it, and the others selected with it, in time and
  value. Alt: off the grid; Shift while dragging: finer values. One breakpoint
  can't pass its neighbours; several override the breakpoints they land on.
- Alt-dragging between two breakpoints bends the segment: up bulges it upward.
- Dragging from off the breakpoints selects a time range on the lanes it
  crosses; Delete clears their automation there, Ctrl+D duplicates it. Up into
  the clips' title band, or above the track, it selects clips instead, as a
  drag in the clips does. Dragging inside the selected range (off its
  breakpoints and line) moves the automation in it, on all its lanes, up, down,
  left and right, over what is where it lands: breakpoints at the range's edges
  keep the envelope outside it as it was.

Envelopes are drawn red while they play, grey when overridden (the target was
changed by hand: Re-Enable Automation brings them back); a target without an
envelope shows its own value as a faint line.
"""

from __future__ import annotations

import bisect
import math
from dataclasses import dataclass

from PySide6.QtCore import QLineF, QPointF, QRectF, Qt
from PySide6.QtGui import QColor, QCursor, QFontMetrics, QPainter, QPainterPath, QPen, QPixmap, QPolygonF
from PySide6.QtWidgets import QMenu

from ... import theme
from ...model import automation
from ...model.automation import MASTER
from .interactions import DRAG_THRESHOLD, ClipGesture, TimeSelectGesture

ENVELOPE = QColor("#ff4a3d")
OVERRIDDEN = QColor("#8c8c8c")
UNAUTOMATED = QColor(255, 74, 61, 110)
GHOST = QColor(255, 74, 61, 170)  # where a click on the line would add a breakpoint
LANE_BACKGROUND = QColor(0, 0, 0, 55)  # over the clips of a lane showing automation
POINT_RADIUS = 3.0
POINT_GRAB = 6.0  # pixels around a breakpoint that grab it
LINE_GRAB = 4.0  # pixels around the envelope's line that count as on it
SEGMENT_GRAB = 14.0  # pixels around it that grab the segment (beyond LINE_GRAB)
CURVE_PIXELS = 150.0  # an Alt-drag this far bends a segment from straight to its most
VALUE_PAD = 4.0  # between a lane's edges and its values' range
SAMPLE_PIXELS = 3.0  # between the points a curved segment is drawn through


@dataclass(frozen=True)
class EnvelopeArea:
    """A lane showing an envelope, in its host's widget coordinates."""

    owner: str  # track id or MASTER
    key: str
    lane: int  # -1: the owner's own lane; else its index in the lanes below it
    rect: QRectF  # where it takes the mouse

    @property
    def ident(self) -> tuple[str, str, int]:
        return self.owner, self.key, self.lane

    @property
    def values(self) -> QRectF:
        """Where values go: 1 at the top, 0 at the bottom."""
        return self.rect.adjusted(0, VALUE_PAD, 0, -VALUE_PAD)

    def y(self, value: float) -> float:
        values = self.values
        return values.bottom() - value * values.height()

    def value(self, y: float) -> float:
        values = self.values
        return min(1.0, max(0.0, (values.bottom() - y) / max(1.0, values.height())))


@dataclass(frozen=True)
class Hover:
    """What is under the mouse in a lane: a breakpoint ("point", `index`), a
    place on the line where a click adds one ("add", `beat`, `value`), a segment
    to drag ("segment", `index`: its first breakpoint), the flat line out from
    the first or last breakpoint ("end", `index`, `beat`: under the mouse), or
    the selected range ("range")."""

    ident: tuple[str, str, int]
    kind: str
    index: int | None = None
    beat: float = 0.0
    value: float = 0.0


def area_at(areas: list[EnvelopeArea], pos: QPointF) -> EnvelopeArea | None:
    return next((a for a in areas if a.rect.contains(pos)), None)


def nearest_area(areas: list[EnvelopeArea], y: float) -> int | None:
    """The index of the area closest to `y` vertically."""
    if not areas:
        return None
    return min(range(len(areas)), key=lambda i: max(0.0, areas[i].rect.top() - y, y - areas[i].rect.bottom()))


def point_at(view, area: EnvelopeArea, points, pos: QPointF) -> int | None:
    """The breakpoint under `pos` (the one on top where several are)."""
    if not points:
        return None
    beats = [p.beat for p in points]
    first = bisect.bisect_left(beats, view.x_to_beat(pos.x() - POINT_GRAB))
    last = bisect.bisect_right(beats, view.x_to_beat(pos.x() + POINT_GRAB))
    best, best_distance = None, POINT_GRAB
    for i in range(first, last):
        distance = max(abs(view.beat_to_x(points[i].beat) - pos.x()), abs(area.y(points[i].value) - pos.y()))
        if distance <= best_distance:
            best, best_distance = i, distance
    return best


def _unautomated_value(host, area: EnvelopeArea) -> float | None:
    """Where a target without an envelope draws its line: its own value."""
    value = host.bridge.own_value(area.owner, area.key)
    spec = host.bridge.param_spec(area.owner, area.key)
    return None if value is None or spec is None else spec.to_normalized(value)


def _quantizer(host, area: EnvelopeArea):
    spec = host.bridge.param_spec(area.owner, area.key)
    return spec.quantize if spec is not None and spec.discrete else None


def _on_line(host, area: EnvelopeArea, points, pos: QPointF, grab: float = LINE_GRAB) -> bool:
    """Whether `pos` is within `grab` pixels of the envelope's line, as drawn."""
    if not points:
        value = _unautomated_value(host, area)
        return value is not None and abs(area.y(value) - pos.y()) <= grab
    line = trace(host.view, area, points, pos.x() - grab, pos.x() + grab, _quantizer(host, area))
    if line.size() == 1:
        return QLineF(line[0], pos).length() <= grab
    return any(_distance(pos, line[i], line[i + 1]) <= grab for i in range(line.size() - 1))


def _segment_line(host, area: EnvelopeArea, points, i: int, x0: float = -math.inf,
                  x1: float = math.inf) -> QPolygonF:
    """The segment from breakpoint i to the next as drawn (between x0 and x1)."""
    view = host.view
    a, b = points[i], points[i + 1]
    xa, xb = view.beat_to_x(a.beat), view.beat_to_x(b.beat)
    if xb - xa < 1.0:  # a step: straight up or down
        return QPolygonF([QPointF(xa, area.y(a.value)), QPointF(xb, area.y(b.value))])
    return trace(view, area, points[i:i + 2], max(xa, x0), min(xb, x1), _quantizer(host, area))


def _polyline_distance(pos: QPointF, line: QPolygonF) -> float:
    if line.size() == 1:
        return QLineF(line[0], pos).length()
    return min(_distance(pos, line[i], line[i + 1]) for i in range(line.size() - 1))


def segment_at(host, area: EnvelopeArea, points, pos: QPointF) -> int | None:
    """The segment (its first breakpoint) whose line `pos` is nearest to,
    within SEGMENT_GRAB pixels. Steps count too."""
    if len(points) < 2:
        return None
    view = host.view
    beats = [p.beat for p in points]
    first = max(0, bisect.bisect_left(beats, view.x_to_beat(pos.x() - SEGMENT_GRAB)) - 1)
    last = min(len(points) - 1, bisect.bisect_right(beats, view.x_to_beat(pos.x() + SEGMENT_GRAB)))
    best, best_distance = None, SEGMENT_GRAB
    for i in range(first, last):
        line = _segment_line(host, area, points, i, pos.x() - SEGMENT_GRAB, pos.x() + SEGMENT_GRAB)
        distance = _polyline_distance(pos, line)
        if distance <= best_distance:
            best, best_distance = i, distance
    return best


def end_at(host, area: EnvelopeArea, points, pos: QPointF) -> int | None:
    """The first or last breakpoint when `pos` is within SEGMENT_GRAB pixels of
    the flat line running out to the left or right of it."""
    if not points:
        return None
    view = host.view
    if pos.x() < view.beat_to_x(points[0].beat):
        index = 0
    elif pos.x() > view.beat_to_x(points[-1].beat):
        index = len(points) - 1
    else:
        return None
    quantize = _quantizer(host, area)
    value = quantize(points[index].value) if quantize is not None else points[index].value
    return index if abs(area.y(value) - pos.y()) <= SEGMENT_GRAB else None


def _is_step(points, segment: int) -> bool:
    return points[segment].beat == points[segment + 1].beat


def _grab(host, area: EnvelopeArea, points, pos: QPointF, mods) -> tuple[str, object] | None:
    """What a press off the breakpoints takes: ("add", (beat, value)) on the
    line, or ("segment", index) near it. On a step there is nothing to add:
    it takes the step."""
    segment = segment_at(host, area, points, pos)
    if segment is not None and _is_step(points, segment):
        return "segment", segment
    target = add_target(host, area, pos, mods)
    if target is not None:
        return "add", target
    return ("segment", segment) if segment is not None else None


def in_range(host, area: EnvelopeArea, pos: QPointF) -> bool:
    """Whether `pos` is inside the selected time range, on a lane it covers."""
    selection = host.selection
    if selection.time_range is None or (area.owner, area.key) not in selection.lanes:
        return False
    start, end = selection.time_range[:2]
    return start <= host.view.x_to_beat(pos.x()) <= end


def _distance(pos: QPointF, a: QPointF, b: QPointF) -> float:
    """From `pos` to the segment a-b."""
    d = b - a
    length = QPointF.dotProduct(d, d)
    t = 0.0 if length == 0 else min(1.0, max(0.0, QPointF.dotProduct(pos - a, d) / length))
    return QLineF(a + d * t, pos).length()


def add_target(host, area: EnvelopeArea, pos: QPointF, mods) -> tuple[float, float] | None:
    """Where a click at `pos` adds a breakpoint, (beat, value): on the line,
    at the grid line nearest the mouse. None off the line."""
    points = host.project.envelope(area.owner, area.key)
    if not _on_line(host, area, points, pos):
        return None
    view = host.view
    beat = max(0.0, view.snap_beat(view.x_to_beat(pos.x()), _alt(mods)))
    value = automation.value_at(points, beat) if points else _unautomated_value(host, area)
    if value is None:
        return None
    quantize = _quantizer(host, area)
    return beat, quantize(value) if quantize is not None else value


_ADD_CURSOR: list[QCursor] = []


def add_cursor() -> QCursor:
    """The arrow with a small plus beside it: a click adds a breakpoint."""
    if not _ADD_CURSOR:
        pixmap = QPixmap(24, 24)
        pixmap.fill(Qt.GlobalColor.transparent)
        p = QPainter(pixmap)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        arrow = QPainterPath(QPointF(1, 1))
        for x, y in ((1, 16), (4.5, 12.5), (7, 18), (9.5, 17), (7, 11.5), (12, 11.5)):
            arrow.lineTo(x, y)
        arrow.closeSubpath()
        p.setPen(QPen(QColor(0, 0, 0), 1.0, Qt.PenStyle.SolidLine, Qt.PenCapStyle.SquareCap,
                      Qt.PenJoinStyle.MiterJoin))
        p.setBrush(QColor(255, 255, 255))
        p.drawPath(arrow)
        plus = QPainterPath()
        plus.moveTo(14, 18.5)
        plus.lineTo(21, 18.5)
        plus.moveTo(17.5, 15)
        plus.lineTo(17.5, 22)
        for color, width in ((QColor(0, 0, 0), 3.5), (QColor(255, 255, 255), 1.5)):
            p.setPen(QPen(color, width, Qt.PenStyle.SolidLine, Qt.PenCapStyle.FlatCap))
            p.drawPath(plus)
        p.end()
        _ADD_CURSOR.append(QCursor(pixmap, 1, 1))
    return _ADD_CURSOR[0]


# --- Drawing -----------------------------------------------------------------------------


def trace(view, area: EnvelopeArea, points, x0: float, x1: float, quantize=None) -> QPolygonF:
    """The envelope's line from x0 to x1: through its breakpoints, and along
    curved segments every few pixels. A discrete target (`quantize`) steps."""
    b0, b1 = view.x_to_beat(x0), view.x_to_beat(x1)
    beats = [p.beat for p in points]
    first = max(0, bisect.bisect_right(beats, b0) - 1)
    last = min(len(points) - 1, bisect.bisect_left(beats, b1))
    line: list[tuple[float, float]] = []
    if b0 < points[0].beat:
        line.append((x0, points[0].value))
    for j in range(first, last + 1):
        a = points[j]
        xa = view.beat_to_x(a.beat)
        line.append((xa, a.value))
        if j + 1 < len(points):
            b = points[j + 1]
            xb = view.beat_to_x(b.beat)
            if xb > xa and (quantize is not None or (a.curve and a.value != b.value)):
                x, end = max(xa, x0) + SAMPLE_PIXELS, min(xb, x1)
                while x < end:
                    line.append((x, automation.segment_value(a, b, view.x_to_beat(x))))
                    x += SAMPLE_PIXELS
    if b1 > points[-1].beat:
        line.append((x1, points[-1].value))
    if quantize is not None:
        steps: list[tuple[float, float]] = []
        for x, value in line:
            value = quantize(value)
            if steps and steps[-1][1] != value:
                steps.append((x, steps[-1][1]))
            steps.append((x, value))
        line = steps
    return QPolygonF([QPointF(x, area.y(value)) for x, value in line])


def _draw_ghost(p: QPainter, host, area: EnvelopeArea, hover: Hover | None) -> None:
    """Where a click would add a breakpoint (not while a gesture is under way)."""
    if hover is None or hover.ident != area.ident or hover.kind != "add" or host._gesture is not None:
        return
    p.setPen(QPen(GHOST, 1.4))
    p.setBrush(QColor(GHOST.red(), GHOST.green(), GHOST.blue(), 70))
    radius = POINT_RADIUS + 1.0
    p.drawEllipse(QPointF(host.view.beat_to_x(hover.beat), area.y(hover.value)), radius, radius)


def draw_area(p: QPainter, host, area: EnvelopeArea, visible: QRectF, hover: Hover | None = None,
              shade: bool = True) -> None:
    """A lane's envelope (and, with `shade`, a veil over what is under it)."""
    clip = area.rect.intersected(visible)
    if clip.isEmpty():
        return
    p.save()
    p.setClipRect(clip.adjusted(0, -POINT_RADIUS - 1, 0, POINT_RADIUS + 1))
    if shade:
        p.fillRect(clip, LANE_BACKGROUND)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    points = host.project.envelope(area.owner, area.key)
    spec = host.bridge.param_spec(area.owner, area.key)
    x0, x1 = clip.left() - 2, clip.right() + 2
    if not points:
        value = _unautomated_value(host, area)
        if value is not None:
            p.setPen(QPen(UNAUTOMATED, 1.5, Qt.PenStyle.DashLine))
            y = area.y(value)
            p.drawLine(QPointF(x0, y), QPointF(x1, y))
        _draw_ghost(p, host, area, hover)
        p.restore()
        return
    color = OVERRIDDEN if host.bridge.is_overridden(area.owner, area.key) else ENVELOPE
    quantize = spec.quantize if spec is not None and spec.discrete else None
    if (hover is not None and hover.ident == area.ident and hover.kind == "segment"
            and hover.index + 1 < len(points)):
        p.setPen(QPen(color, 3.2))
        p.drawPolyline(_segment_line(host, area, points, hover.index, x0, x1))
    if hover is not None and hover.ident == area.ident and hover.kind == "end" and hover.index < len(points):
        end = points[hover.index]
        y = area.y(quantize(end.value) if quantize is not None else end.value)
        p.setPen(QPen(color, 3.2))
        p.drawLine(QPointF(host.view.beat_to_x(end.beat), y), QPointF(x0 if hover.beat < end.beat else x1, y))
    p.setPen(QPen(color, 1.6))
    p.drawPolyline(trace(host.view, area, points, x0, x1, quantize))
    selected = host.selection.selected_points(area.owner, area.key)
    view = host.view
    for i, point in enumerate(points):
        x = view.beat_to_x(point.beat)
        if x < x0 - POINT_RADIUS or x > x1 + POINT_RADIUS:
            continue
        hovered = (hover is not None and hover.ident == area.ident and hover.index is not None
                   and (hover.index == i if hover.kind in ("point", "end") else hover.kind == "segment" and i - hover.index in (0, 1)))
        radius = POINT_RADIUS + (1.0 if hovered else 0.0)
        p.setPen(QPen(color, 1.4))
        p.setBrush(QColor(theme.SELECTION_OUTLINE) if i in selected else (color if hovered else QColor(theme.LANE)))
        p.drawEllipse(QPointF(x, area.y(point.value)), radius, radius)
    _draw_ghost(p, host, area, hover)
    p.restore()


def draw_range(p: QPainter, host, areas: list[EnvelopeArea], tint: QColor) -> None:
    """The selected time range, over the automation lanes it covers."""
    selection = host.selection
    if selection.time_range is None or not selection.lanes:
        return
    start, end = selection.time_range[:2]
    x0, x1 = host.view.beat_to_x(start), host.view.beat_to_x(end)
    lanes = set(selection.lanes)
    for area in areas:
        if (area.owner, area.key) in lanes:
            p.fillRect(QRectF(x0, area.rect.top(), x1 - x0, area.rect.height()), tint)


def draw_readout(p: QPainter, host, gesture) -> None:
    """What a breakpoint being dragged is set to, next to it."""
    readout = gesture.readout() if gesture is not None else None
    if readout is None:
        return
    at, text = readout
    p.save()
    p.setFont(theme.ui_font(8))
    metrics = QFontMetrics(p.font())
    box = QRectF(at.x() + 8, at.y() - metrics.height() - 6, metrics.horizontalAdvance(text) + 10, metrics.height() + 4)
    if box.top() < 0:
        box.moveTop(at.y() + 6)
    if box.right() > host.width():
        box.moveRight(at.x() - 8)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    p.setPen(QColor(theme.BORDER))
    p.setBrush(QColor(theme.PANEL_ALT))
    p.drawRoundedRect(box, 3, 3)
    p.setPen(QColor(theme.TEXT))
    p.drawText(box, Qt.AlignmentFlag.AlignCenter, text)
    p.restore()


# --- Mouse ---------------------------------------------------------------------------------


def _fine(mods) -> float:
    return 0.1 if mods & Qt.KeyboardModifier.ShiftModifier else 1.0


def _alt(mods) -> bool:
    return bool(mods & Qt.KeyboardModifier.AltModifier)


class PointGesture(ClipGesture):
    """Drag breakpoints: the one pressed, and the others selected with it. A
    click without dragging deletes it (Shift/Ctrl: selects it instead). With
    `added`, the breakpoint was just added by this press: a click keeps it, and
    adding and dragging it are one undo step. With `segment`, it and the next
    one are dragged (a segment); a click selects them."""

    def __init__(self, host, area: EnvelopeArea, index: int, press: QPointF, mods, added: bool = False,
                 segment: bool = False):
        self.host = host
        self.area = area
        self.index = index
        self.press = press
        self.added = added or segment  # (a click deletes nothing)
        self.original = host.project.envelope(area.owner, area.key)
        selected = set(host.selection.selected_points(area.owner, area.key)) if not self.added else set()
        self.selecting = not self.added and bool(
            mods & (Qt.KeyboardModifier.ShiftModifier | Qt.KeyboardModifier.ControlModifier))
        if segment:
            selected = {index, index + 1}
        elif self.selecting:
            selected ^= {index}
        elif index not in selected:
            selected = {index}
        host.selection.select_points(area.owner, area.key, selected)
        self.indices = selected if index in selected else {index}
        self.current = index  # where the pressed point is now
        self.active = False
        self.merge_key: object = self

    def move(self, pos: QPointF, modifiers) -> None:
        if not self.active:
            if (pos - self.press).manhattanLength() < DRAG_THRESHOLD:
                return
            self.active = True
        view = self.host.view
        anchor = self.original[self.index]
        delta_beats = 0.0
        if abs(pos.x() - self.press.x()) >= DRAG_THRESHOLD:  # a vertical drag leaves the time alone
            beat = anchor.beat + view.x_to_beat(pos.x()) - view.x_to_beat(self.press.x())
            delta_beats = max(0.0, view.snap_beat(beat, _alt(modifiers))) - anchor.beat
        delta_value = (self.press.y() - pos.y()) / max(1.0, self.area.values.height()) * _fine(modifiers)
        where = self.host.editor.move_automation_points(self.area.owner, self.area.key, self.original, self.indices,
                                                        delta_beats, delta_value, merge_key=self.merge_key)
        # Several points override what they land on, so the points after them move up the list.
        self.current = where.get(self.index, self.current)
        self.host.selection.select_points(self.area.owner, self.area.key, set(where.values()))
        self.host.update()

    def readout(self) -> tuple[QPointF, str] | None:
        points = self.host.project.envelope(self.area.owner, self.area.key)
        spec = self.host.bridge.param_spec(self.area.owner, self.area.key)
        if not self.active or spec is None or self.current >= len(points):
            return None
        point = points[self.current]
        return (QPointF(self.host.view.beat_to_x(point.beat), self.area.y(point.value)),
                spec.format_normalized(spec.quantize(point.value)))

    def finish(self) -> None:
        if self.active or self.selecting or self.added:
            return
        owner, key = self.area.owner, self.area.key
        self.host.editor.delete_automation_points(owner, key, [self.current])
        self.host.selection.select_points(owner, key, ())


class CurveGesture(ClipGesture):
    """Alt-drag between two breakpoints: bend the segment."""

    def __init__(self, host, area: EnvelopeArea, index: int, press: QPointF):
        self.host = host
        self.area = area
        self.index = index
        self.press = press
        self.original = host.project.envelope(area.owner, area.key)
        host.selection.select_points(area.owner, area.key, ())

    def move(self, pos: QPointF, modifiers) -> None:
        bend = (self.press.y() - pos.y()) / CURVE_PIXELS * _fine(modifiers)
        self.host.editor.set_automation_curve(self.area.owner, self.area.key, self.original, self.index,
                                              self.original[self.index].curve + bend, merge_key=self)
        self.host.update()


class RangeGesture(ClipGesture):
    """A press inside the selected time range on one of its lanes: a drag moves
    the automation in the range, on all its lanes, in time and value (the range
    goes along). (Its breakpoints and segments, and the line, are taken as
    outside it.) A click sets the insert marker, as outside the range."""

    def __init__(self, host, area: EnvelopeArea, press: QPointF, mods):
        self.host = host
        self.area = area
        self.press = press
        self.mods = mods
        self.start, self.end, self.track_ids = host.selection.time_range
        self.lanes = host.selection.lanes
        self.originals = {lane: host.project.envelope(*lane) for lane in self.lanes}
        self.active = False

    def move(self, pos: QPointF, modifiers) -> None:
        if not self.active:
            if (pos - self.press).manhattanLength() < DRAG_THRESHOLD:
                return
            self.active = True
        view = self.host.view
        delta_beats = 0.0
        if abs(pos.x() - self.press.x()) >= DRAG_THRESHOLD:  # a vertical drag leaves the time alone
            beat = self.start + view.x_to_beat(pos.x()) - view.x_to_beat(self.press.x())
            delta_beats = max(0.0, view.snap_beat(beat, _alt(modifiers))) - self.start
        delta_value = (self.press.y() - pos.y()) / max(1.0, self.area.values.height()) * _fine(modifiers)
        self.host.editor.move_automation_range(self.start, self.end, self.originals, delta_beats, delta_value,
                                               merge_key=self)
        start, end = self.start + delta_beats, self.end + delta_beats
        self.host.selection.set_time_range(start, end, self.track_ids, lanes=self.lanes)
        self.host.selection.set_insert(start)
        self.host.update()

    def finish(self) -> None:
        if not self.active:
            LaneGesture(self.host, self.area, self.press, self.mods).finish()


class _MergeKey:
    """Ties a new breakpoint's adding to the drag that follows."""


def _add_and_drag(host, area: EnvelopeArea, pos: QPointF, mods, target: tuple[float, float]) -> PointGesture:
    """A press on the line: a breakpoint there at once, and a drag places it."""
    key = _MergeKey()
    index = host.editor.add_automation_point(area.owner, area.key, *target, merge_key=key)
    gesture = PointGesture(host, area, index, pos, mods, added=True)
    gesture.merge_key = key
    host.selection.set_insert(target[0])
    return gesture


class LaneGesture(ClipGesture):
    """A press on a lane off its breakpoints and its line: a click sets the
    insert marker; a drag selects a time range on the lanes it crosses (on the
    clips when it goes up into their title band or above the track)."""

    def __init__(self, host, area: EnvelopeArea, press: QPointF, mods):
        self.host = host
        self.area = area
        self.press = press
        self.bypass = _alt(mods)
        view = host.view
        self.anchor = max(0.0, view.snap_beat(view.x_to_beat(press.x()), self.bypass))
        self.active = False
        # A track's lanes are in the lanes canvas, among the clips: a drag can reach them.
        self.clips: TimeSelectGesture | None = None
        if area.owner != MASTER and hasattr(host, "in_clip_band"):
            self.clips = TimeSelectGesture(host, press, self.bypass)
            self.clips.anchor_row = next(i for i, r in enumerate(host.layout_model.rows) if r.track_id == area.owner)
        host.selection.clear(track_id=None if area.owner == MASTER else area.owner)

    def move(self, pos: QPointF, modifiers) -> None:
        if not self.active:
            if (pos - self.press).manhattanLength() < DRAG_THRESHOLD:
                return
            self.active = True
        if self._over_clips(pos):
            self.clips.move(pos, modifiers)
            return
        view = self.host.view
        beat = max(0.0, view.snap_beat(view.x_to_beat(pos.x()), _alt(modifiers)))
        start, end = sorted((self.anchor, beat))
        areas = self.host.envelope_areas()
        first = next((i for i, a in enumerate(areas) if a.ident == self.area.ident), None)
        here = nearest_area(areas, pos.y())
        if first is None or here is None:
            covered = [self.area]
        else:
            covered = areas[min(first, here):max(first, here) + 1]
        lanes = list(dict.fromkeys((a.owner, a.key) for a in covered))
        track_ids = list(dict.fromkeys(a.owner for a in covered if a.owner != MASTER))
        selection = self.host.selection
        selection.set_time_range(start, end, track_ids, lanes=lanes)
        selection.set_insert(start)
        self.host.setCursor(Qt.CursorShape.IBeamCursor)

    def _over_clips(self, pos: QPointF) -> bool:
        """Whether the drag went up into the clips' title band or above its track."""
        if self.clips is None:
            return False
        host = self.host
        row = host.layout_model.rows[self.clips.anchor_row]
        return host.in_clip_band(pos) or pos.y() + host.view.scroll_y < row.top

    def finish(self) -> None:
        if not self.active:
            self.host.selection.set_insert(self.anchor)


def press(host, area: EnvelopeArea, pos: QPointF, mods) -> ClipGesture:
    """The gesture a press on an automation lane starts. (Hosts start one on a
    double-click too: each click of it counts.)"""
    points = host.project.envelope(area.owner, area.key)
    index = point_at(host.view, area, points, pos)
    if index is not None:
        return PointGesture(host, area, index, pos, mods)
    if _alt(mods):
        segment = automation.segment_index(points, host.view.x_to_beat(pos.x()))
        if segment is not None:
            return CurveGesture(host, area, segment, pos)
    grab = _grab(host, area, points, pos, mods)
    if grab is None and in_range(host, area, pos):
        return RangeGesture(host, area, pos, mods)
    end = end_at(host, area, points, pos)
    if end is not None and (grab is None or grab[0] != "add"):  # near the line, not on it
        return PointGesture(host, area, end, pos, mods, added=True)
    if grab is not None and grab[0] == "add":
        return _add_and_drag(host, area, pos, mods, grab[1])
    if grab is not None:
        return PointGesture(host, area, grab[1], pos, mods, segment=True)
    return LaneGesture(host, area, pos, mods)


def hover(host, area: EnvelopeArea | None, pos: QPointF, mods) -> tuple[Hover | None, QCursor | Qt.CursorShape]:
    """(what is under the mouse, the cursor) over a lane."""
    if area is None:
        return None, Qt.CursorShape.ArrowCursor
    points = host.project.envelope(area.owner, area.key)
    index = point_at(host.view, area, points, pos)
    if index is not None:
        return Hover(area.ident, "point", index), Qt.CursorShape.PointingHandCursor
    if _alt(mods) and automation.segment_index(points, host.view.x_to_beat(pos.x())) is not None:
        return None, Qt.CursorShape.SizeVerCursor
    grab = _grab(host, area, points, pos, mods)
    if grab is None and in_range(host, area, pos):
        return Hover(area.ident, "range"), Qt.CursorShape.ArrowCursor
    end = end_at(host, area, points, pos)
    if end is not None and (grab is None or grab[0] != "add"):
        return Hover(area.ident, "end", end, host.view.x_to_beat(pos.x())), Qt.CursorShape.ArrowCursor
    if grab is not None and grab[0] == "add":
        return Hover(area.ident, "add", None, *grab[1]), add_cursor()
    if grab is not None:
        return Hover(area.ident, "segment", grab[1]), Qt.CursorShape.ArrowCursor
    return None, Qt.CursorShape.ArrowCursor


def add_menu_actions(host, area: EnvelopeArea, pos: QPointF, menu: QMenu) -> None:
    """A lane's right-click menu."""
    editor, bridge = host.editor, host.bridge
    owner, key = area.owner, area.key
    points = host.project.envelope(owner, key)
    index = point_at(host.view, area, points, pos)
    if index is not None:
        menu.addAction("Delete Breakpoint", lambda: (editor.delete_automation_points(owner, key, [index]),
                                                     host.selection.select_points(owner, key, ())))
    selected = host.selection.selected_points(owner, key)
    if len(selected) > 1:
        menu.addAction("Delete Selected Breakpoints", lambda: (editor.delete_automation_points(owner, key, selected),
                                                               host.selection.select_points(owner, key, ())))
    delete = menu.addAction("Delete Envelope", lambda: editor.clear_envelope(owner, key))
    delete.setEnabled(bool(points))
    if bridge.is_overridden(owner, key):
        menu.addAction("Re-Enable Automation", lambda: bridge.re_enable_automation(owner))
    menu.addSeparator()
    if area.lane >= 0:
        menu.addAction("Remove Lane", lambda: editor.remove_automation_lane(owner, area.lane))
    menu.addAction("Show Automation in New Lane", lambda: editor.add_automation_lane(owner))
    menu.addAction("Hide Automation", lambda: editor.hide_automation(owner))
