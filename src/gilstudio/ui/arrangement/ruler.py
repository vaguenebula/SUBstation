"""Beat-time ruler with the loop brace.

Top strip: drag the loop brace (body moves, edges resize, empty area draws a new
loop; double-click toggles it). Lower area, like Ableton's scrub area: click to
set the playhead, drag vertically to zoom and horizontally to scroll.
"""

from __future__ import annotations

from PySide6.QtCore import QPointF, QRect, QRectF, QSize, Qt, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPainterPath, QPolygonF
from PySide6.QtWidgets import QWidget

from ... import theme
from ...model.editor import ProjectEditor
from ...model.timebase import format_bar_label
from .grid import grid_lines, label_step
from .view_state import Selection, ViewState

LOOP_STRIP = 14
HEIGHT = 40
EDGE_GRAB = 6


class TimelineRuler(QWidget):
    locate_requested = Signal(float)

    def __init__(self, view: ViewState, editor: ProjectEditor, selection: Selection, parent: QWidget | None = None):
        super().__init__(parent)
        self.view = view
        self.editor = editor
        self.project = editor.project
        self.selection = selection
        self._playhead = 0.0
        self._drag: dict | None = None
        self.setFixedHeight(HEIGHT)
        self.setMouseTracking(True)
        view.changed.connect(self.update)
        view.grid_changed.connect(self.update)
        self.project.settings_changed.connect(self.update)
        selection.insert_changed.connect(self.update)

    def sizeHint(self) -> QSize:
        return QSize(400, HEIGHT)

    # --- Playhead ------------------------------------------------------------------

    def set_playhead(self, beat: float) -> None:
        for b in (self._playhead, beat):
            x = int(self.view.beat_to_x(b))
            self.update(QRect(x - 6, 0, 13, self.height()))
        self._playhead = beat

    # --- Painting ------------------------------------------------------------------

    def paintEvent(self, event) -> None:
        p = QPainter(self)
        rect = QRectF(event.rect())
        width = self.width()
        p.fillRect(rect, QColor(theme.PANEL))
        p.fillRect(QRectF(rect.left(), 0, rect.width(), LOOP_STRIP), QColor(theme.PANEL_ALT))

        # Loop brace
        project = self.project
        lx0 = self.view.beat_to_x(project.loop_start)
        lx1 = self.view.beat_to_x(project.loop_end)
        if lx1 > 0 and lx0 < width:
            brace = QRectF(lx0, 2, lx1 - lx0, LOOP_STRIP - 4)
            color = QColor(theme.LOOP_ON if project.loop_enabled else theme.LOOP_OFF)
            p.fillRect(brace, color)
            p.fillRect(QRectF(lx0, 2, 2, LOOP_STRIP - 4), color.darker(140))
            p.fillRect(QRectF(lx1 - 2, 2, 2, LOOP_STRIP - 4), color.darker(140))

        # Ticks and labels
        scale_top = LOOP_STRIP
        scale_h = self.height() - LOOP_STRIP
        step = self.view.grid_step()
        every = label_step(self.view, step)
        p.setFont(theme.ui_font(8))
        for x, beat, kind in grid_lines(self.view, rect.left() - 60, rect.right() + 1, step):
            tick = {"bar": scale_h, "beat": scale_h * 0.45, "sub": scale_h * 0.25}[kind]
            color = QColor(theme.TEXT_DIM if kind == "bar" else theme.GRID_BAR)
            p.fillRect(QRectF(round(x), self.height() - tick, 1, tick), color)
            if abs(beat / every - round(beat / every)) < 1e-6:
                p.setPen(QColor(theme.TEXT))
                p.drawText(QPointF(round(x) + 3, scale_top + 12), format_bar_label(beat, project.time_signature))
        p.fillRect(QRectF(rect.left(), self.height() - 1, rect.width(), 1), QColor(theme.BORDER))

        # Start marker (insert position) and playhead
        sx = self.view.beat_to_x(self.selection.insert_beat)
        marker = QPolygonF([QPointF(sx - 5, scale_top + 1), QPointF(sx + 5, scale_top + 1), QPointF(sx, scale_top + 8)])
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(theme.INSERT_MARKER))
        p.drawPolygon(marker)
        px = round(self.view.beat_to_x(self._playhead))
        path = QPainterPath(QPointF(px - 5, self.height() - 8))
        path.lineTo(px + 6, self.height() - 8)
        path.lineTo(px + 0.5, self.height() - 1)
        path.closeSubpath()
        p.fillPath(path, QColor(theme.PLAYHEAD))

    # --- Interaction ---------------------------------------------------------------

    def _loop_zone(self, x: float) -> str | None:
        x0 = self.view.beat_to_x(self.project.loop_start)
        x1 = self.view.beat_to_x(self.project.loop_end)
        if abs(x - x0) <= EDGE_GRAB:
            return "start"
        if abs(x - x1) <= EDGE_GRAB:
            return "end"
        if x0 < x < x1:
            return "body"
        return None

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        pos = event.position()
        beat = self.view.x_to_beat(pos.x())
        p = self.project
        if pos.y() < LOOP_STRIP:
            zone = self._loop_zone(pos.x())
            self._drag = {"mode": f"loop-{zone or 'new'}", "beat": beat, "start": p.loop_start, "end": p.loop_end,
                          "key": object()}
        else:
            self._drag = {"mode": "scrub", "x": pos.x(), "y": pos.y(), "last_y": pos.y(), "beat": beat,
                          "scroll": self.view.scroll_beats, "moved": False, "pan": False}

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        pos = event.position()
        if self._drag is None:
            if pos.y() < LOOP_STRIP:
                zone = self._loop_zone(pos.x())
                shape = {"start": Qt.CursorShape.SizeHorCursor, "end": Qt.CursorShape.SizeHorCursor,
                         "body": Qt.CursorShape.OpenHandCursor}.get(zone, Qt.CursorShape.ArrowCursor)
            else:
                shape = Qt.CursorShape.ArrowCursor
            self.setCursor(shape)
            return
        d = self._drag
        bypass = bool(event.modifiers() & Qt.KeyboardModifier.AltModifier)
        beat = self.view.x_to_beat(pos.x())

        def snap(b: float) -> float:
            return self.view.snap_beat(b, bypass)

        enabled = self.project.loop_enabled
        mode = d["mode"]
        if mode == "loop-start":
            self.editor.set_loop(enabled, min(snap(beat), d["end"] - 0.25), d["end"], d["key"])
        elif mode == "loop-end":
            self.editor.set_loop(enabled, d["start"], max(snap(beat), d["start"] + 0.25), d["key"])
        elif mode == "loop-body":
            start = max(0.0, snap(d["start"] + beat - d["beat"]))
            self.editor.set_loop(enabled, start, start + d["end"] - d["start"], d["key"])
        elif mode == "loop-new":
            a, b = sorted((snap(d["beat"]), snap(beat)))
            if b - a >= 0.25:
                self.editor.set_loop(True, a, b, d["key"])
        elif mode == "scrub":
            dx = pos.x() - d["x"]
            dy = pos.y() - d["y"]
            if not d["moved"] and max(abs(dx), abs(dy)) > 3:
                d["moved"] = True
                d["pan"] = abs(dx) > abs(dy)
            if d["moved"]:
                if d["pan"]:
                    self.view.set_scroll_beats(d["scroll"] - dx / self.view.px_per_beat)
                else:
                    self.view.zoom_at(d["x"], 1.012 ** (pos.y() - d["last_y"]))
                    d["last_y"] = pos.y()

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        d = self._drag
        self._drag = None
        if d and d["mode"] == "scrub" and not d["moved"]:
            bypass = bool(event.modifiers() & Qt.KeyboardModifier.AltModifier)
            self.locate_requested.emit(max(0.0, self.view.snap_beat(d["beat"], bypass)))

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        if event.position().y() < LOOP_STRIP and self._loop_zone(event.position().x()):
            self.editor.set_loop_enabled(not self.project.loop_enabled)
