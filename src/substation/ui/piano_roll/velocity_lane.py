"""The velocity editor under the piano roll: a stem per note, as tall as its
velocity. Drag a stem up or down; with several notes selected, dragging one of
theirs changes them all by the same amount."""

from __future__ import annotations

from typing import TYPE_CHECKING

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QColor, QMouseEvent, QPainter
from PySide6.QtWidgets import QWidget

from ... import theme
from ...model import notes
from ...model.project import Note
from ..arrangement.grid import draw_grid

if TYPE_CHECKING:
    from .piano_roll import PianoRoll

VELOCITY_HEIGHT = 72
STEM_GRAB = 6  # pixels either side of a stem
MARGIN_TOP, MARGIN_BOTTOM = 8, 3


class VelocityLane(QWidget):
    def __init__(self, roll: PianoRoll):
        super().__init__(roll)
        self.roll = roll
        self._drag: dict | None = None
        self.setFixedHeight(VELOCITY_HEIGHT)
        self.setMouseTracking(True)

    def _span(self) -> float:
        return self.height() - MARGIN_TOP - MARGIN_BOTTOM

    def velocity_y(self, velocity: int) -> float:
        return self.height() - MARGIN_BOTTOM - self._span() * velocity / 127

    def stem_at(self, x: float) -> Note | None:
        """The note whose stem is nearest `x` (selected notes first), if within reach."""
        clip = self.roll.clip()
        if clip is None:
            return None
        view = self.roll.view
        candidates = [(abs(view.beat_to_x(n.start) - x), n not in self.roll.selected, n) for n in clip.notes]
        near = [c for c in candidates if c[0] <= STEM_GRAB]
        return min(near, key=lambda c: c[:2])[2] if near else None

    def paintEvent(self, event) -> None:
        p = QPainter(self)
        roll, view = self.roll, self.roll.view
        visible = QRectF(event.rect())
        p.fillRect(visible, QColor(theme.LANE))
        draw_grid(p, view, visible.left(), visible.right(), 1, self.height())
        clip = roll.clip()
        if clip is not None:
            x0, x1 = view.beat_to_x(clip.offset_beats), view.beat_to_x(clip.window_end)
            p.fillRect(QRectF(visible.left(), 0, max(0.0, x0 - visible.left()), self.height()), theme.OUTSIDE_CLIP)
            p.fillRect(QRectF(x1, 0, max(0.0, visible.right() - x1 + 1), self.height()), theme.OUTSIDE_CLIP)
            color = QColor(roll.track_color())
            bottom = self.height() - MARGIN_BOTTOM
            p.setFont(theme.ui_font(7))
            for note in clip.notes:
                x = round(view.beat_to_x(note.start))
                if x < visible.left() - 3 or x > visible.right() + 3:
                    continue
                selected = note in roll.selected
                stem = QColor(theme.SELECTION_OUTLINE) if selected else color
                y = self.velocity_y(note.velocity)
                p.fillRect(QRectF(x, y, 1, bottom - y), stem)
                p.fillRect(QRectF(x - 2, y - 2, 5, 5), stem)
                if selected and self._drag is not None:
                    p.setPen(QColor(theme.TEXT))
                    p.drawText(QRectF(x + 5, y - 7, 30, 12), Qt.AlignmentFlag.AlignLeft, str(note.velocity))
        p.fillRect(QRectF(visible.left(), 0, visible.width(), 1), QColor(theme.BORDER))
        roll.draw_start_marker(p, self.height())
        roll.draw_playhead(p, self.height())

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        roll = self.roll
        roll.grid.setFocus()
        clip = roll.clip()
        note = self.stem_at(event.position().x())
        if clip is None or note is None:
            return
        if note not in roll.selected:
            roll.set_selection({note})
        self._drag = {"y": event.position().y(), "base": clip.notes, "key": object(),
                      "targets": sorted(roll.selected, key=notes.by_time)}
        self.update()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        d = self._drag
        if d is None:
            near = self.stem_at(event.position().x()) is not None
            self.setCursor(Qt.CursorShape.SizeVerCursor if near else Qt.CursorShape.ArrowCursor)
            return
        delta = (d["y"] - event.position().y()) / self._span() * 127
        changed = notes.with_velocity(d["targets"], delta)
        self.roll.commit(notes.place(d["base"], d["targets"], changed), "Change Velocity", d["key"], selected=changed)

    def mouseReleaseEvent(self, _event: QMouseEvent) -> None:
        self._drag = None
        self.update()
