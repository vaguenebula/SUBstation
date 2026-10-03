"""The piano roll's note area: a row per key, the grid, the notes, and editing them.

Mouse: double-click to add a note (one grid step long) or to delete one; drag a
note to move it (Ctrl copies, Alt bypasses the grid), drag either end to resize
it, drag in empty space to select. Selected notes move and resize together.
Ctrl+Alt drag scrolls the view. Wheel: scroll (Shift: sideways), Ctrl: zoom
time, Alt: make the keys' rows taller or shorter.
Keys: Delete, Ctrl+A, Ctrl+D (duplicate), Ctrl+U (quantize), Up/Down (Shift: an
octave), Left/Right (a grid step; Shift: a bar).
"""

from __future__ import annotations

import math
from typing import TYPE_CHECKING

from PySide6.QtCore import QEvent, QPointF, QRectF, Qt
from PySide6.QtGui import (
    QColor,
    QCursor,
    QKeyEvent,
    QMouseEvent,
    QPainter,
    QPen,
    QWheelEvent,
)
from PySide6.QtWidgets import QWidget

from ... import theme
from ...model import notes
from ...model.notes import by_time, is_black_key, note_name
from ...model.project import Note
from ..arrangement.grid import draw_grid
from ..arrangement.lanes_canvas import is_pan_modifier

if TYPE_CHECKING:
    from .piano_roll import PianoRoll

EDGE_GRAB = 5  # pixels inside each end of a note that resize it
DRAG_THRESHOLD = 3


class NoteGesture:
    """Base: a mouse drag in the note grid. Moves and resizes edit the clip live,
    as one undo step (the gesture's merge key)."""

    def __init__(self, grid: NoteGrid, press: QPointF):
        self.grid = grid
        self.roll = grid.roll
        self.press = press
        self.active = False
        self.key = object()

    def started(self, pos: QPointF) -> bool:
        if not self.active and (pos - self.press).manhattanLength() >= DRAG_THRESHOLD:
            self.active = True
        return self.active

    def move(self, pos: QPointF, modifiers) -> None: ...

    def rubber_band(self) -> QRectF | None:
        return None

    def finish(self) -> None: ...


class MoveNotesGesture(NoteGesture):
    def __init__(self, grid: NoteGrid, press: QPointF, grabbed: Note, moving: list[Note]):
        super().__init__(grid, press)
        self.grabbed = grabbed
        self.moving = moving
        self.base = self.roll.clip().notes
        self.origin_beat = self.roll.view.x_to_beat(press.x())
        self.origin_pitch = self.roll.pitch_at(press.y())

    def move(self, pos: QPointF, modifiers) -> None:
        if not self.started(pos):
            return
        view = self.roll.view
        bypass = bool(modifiers & Qt.KeyboardModifier.AltModifier)
        raw = view.x_to_beat(pos.x()) - self.origin_beat
        delta_beats = view.snap_beat(self.grabbed.start + raw, bypass) - self.grabbed.start
        delta_beats, delta_pitch = notes.clamp_move(self.moving, delta_beats,
                                                    self.roll.pitch_at(pos.y()) - self.origin_pitch)
        copy = bool(modifiers & Qt.KeyboardModifier.ControlModifier)
        moved = notes.shifted(self.moving, delta_beats, delta_pitch)
        self.roll.commit(notes.place(self.base, [] if copy else self.moving, moved),
                         "Copy Notes" if copy else "Move Notes", self.key, selected=moved)
        self.roll.audition(self.grabbed.pitch + delta_pitch, self.grabbed.velocity)


class ResizeNotesGesture(NoteGesture):
    def __init__(self, grid: NoteGrid, press: QPointF, grabbed: Note, moving: list[Note], edge: str):
        super().__init__(grid, press)
        self.grabbed = grabbed
        self.moving = moving
        self.edge = edge  # "start" or "end"
        self.base = self.roll.clip().notes
        self.origin_beat = self.roll.view.x_to_beat(press.x())

    def move(self, pos: QPointF, modifiers) -> None:
        if not self.started(pos):
            return
        view = self.roll.view
        bypass = bool(modifiers & Qt.KeyboardModifier.AltModifier)
        edge_beat = self.grabbed.end if self.edge == "end" else self.grabbed.start
        delta = view.snap_beat(edge_beat + view.x_to_beat(pos.x()) - self.origin_beat, bypass) - edge_beat
        min_length = view.grid_step() if view.snap and not bypass else notes.MIN_NOTE_BEATS
        resized = notes.resized(self.moving, self.edge, delta, min_length)
        self.roll.commit(notes.place(self.base, self.moving, resized), "Resize Notes", self.key, selected=resized)


class SelectNotesGesture(NoteGesture):
    """Rubber band: selects the notes it touches (added to the selection with Ctrl/Shift)."""

    def __init__(self, grid: NoteGrid, press: QPointF, additive: bool):
        super().__init__(grid, press)
        self.base = set(self.roll.selected) if additive else set()
        self.rect: QRectF | None = None

    def move(self, pos: QPointF, modifiers) -> None:
        if not self.started(pos):
            return
        self.rect = QRectF(self.press, pos).normalized()
        clip = self.roll.clip()
        hits = {n for n in clip.notes if self.grid.note_rect(n).intersects(self.rect)} if clip else set()
        self.roll.set_selection(self.base | hits)

    def rubber_band(self) -> QRectF | None:
        return self.rect

    def finish(self) -> None:
        if self.active:  # a group chosen by dragging: bring up the note tools by it
            self.roll.set_selection(self.roll.selected, tools=True)


class PanGesture(NoteGesture):
    """Ctrl+Alt drag: scroll the view in both directions, as in the arrangement."""

    def __init__(self, grid: NoteGrid, press: QPointF):
        super().__init__(grid, press)
        view = self.roll.view
        self.scroll_beats = view.scroll_beats
        self.scroll_y = view.scroll_y

    def move(self, pos: QPointF, modifiers) -> None:
        view = self.roll.view
        delta = pos - self.press
        view.set_scroll_beats(self.scroll_beats - delta.x() / view.px_per_beat)
        view.set_scroll_y(self.scroll_y - delta.y())


class NoteGrid(QWidget):
    def __init__(self, roll: PianoRoll):
        super().__init__(roll)
        self.roll = roll
        self._gesture: NoteGesture | None = None
        self._select_on_click: set[Note] | None = None  # when the mouse comes up without dragging
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent)
        self.setMinimumSize(120, 60)

    # --- Geometry ------------------------------------------------------------------

    def note_rect(self, note: Note) -> QRectF:
        view = self.roll.view
        x0, x1 = view.beat_to_x(note.start), view.beat_to_x(note.end)
        return QRectF(x0, self.roll.pitch_top(note.pitch), max(3.0, x1 - x0), self.roll.row_height)

    def note_at(self, pos: QPointF) -> tuple[Note, str] | None:
        """The note under `pos` (the one drawn on top) and where: "start" or "end"
        (resize handles) or "body"."""
        clip = self.roll.clip()
        if clip is None:
            return None
        for note in reversed(clip.notes):
            rect = self.note_rect(note)
            if rect.left() <= pos.x() <= rect.right() and rect.top() <= pos.y() < rect.bottom():
                grab = min(EDGE_GRAB, rect.width() / 4)
                if pos.x() >= rect.right() - grab:
                    return note, "end"
                if pos.x() <= rect.left() + grab:
                    return note, "start"
                return note, "body"
        return None

    # --- Painting ------------------------------------------------------------------

    def paintEvent(self, event) -> None:
        p = QPainter(self)
        roll, view = self.roll, self.roll.view
        visible = QRectF(event.rect())
        p.fillRect(visible, QColor(theme.LANE))
        height = roll.row_height
        for pitch in range(roll.pitch_at(visible.bottom()), roll.pitch_at(visible.top()) + 1):
            top = roll.pitch_top(pitch)
            if is_black_key(pitch):
                p.fillRect(QRectF(visible.left(), top, visible.width(), height), QColor(theme.BLACK_KEY_ROW))
            if pitch % 12 in (0, 5):  # octave (B|C) and E|F lines
                color = theme.GRID_BAR if pitch % 12 == 0 else theme.GRID_SUB
                p.fillRect(QRectF(visible.left(), top + height - 1, visible.width(), 1), QColor(color))
        bottom = min(visible.bottom(), roll.pitch_top(0) + height)
        draw_grid(p, view, visible.left(), visible.right(), visible.top(), bottom)
        if bottom < visible.bottom():
            p.fillRect(QRectF(visible.left(), bottom, visible.width(), visible.bottom() - bottom),
                       QColor(theme.EMPTY_AREA))

        clip = roll.clip()
        if clip is not None:
            x0, x1 = view.beat_to_x(clip.offset_beats), view.beat_to_x(clip.window_end)
            if x0 > visible.left():
                p.fillRect(QRectF(visible.left(), visible.top(), x0 - visible.left(), visible.height()),
                           theme.OUTSIDE_CLIP)
            if x1 < visible.right():
                p.fillRect(QRectF(x1, visible.top(), visible.right() - x1 + 1, visible.height()), theme.OUTSIDE_CLIP)
            color = QColor(roll.track_color())
            p.setFont(theme.ui_font(7))
            for note in clip.notes:
                rect = self.note_rect(note)
                if rect.intersects(visible):
                    playing = clip.offset_beats <= note.start < clip.window_end
                    self._draw_note(p, note, rect, color, note in roll.selected, playing)

        band = self._gesture.rubber_band() if self._gesture else None
        if band is not None:
            p.fillRect(band, theme.RUBBER_BAND)
            p.setPen(QPen(QColor(theme.ACCENT), 1))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawRect(band)
        roll.draw_start_marker(p, self.height())
        roll.draw_playhead(p, self.height())

    def _draw_note(self, p: QPainter, note: Note, rect: QRectF, color: QColor, selected: bool,
                   playing: bool) -> None:
        # Brighter for louder notes, as in Ableton; faint outside the part the clip plays.
        fill = QColor(color.lighter(135) if selected else color)
        fill.setAlphaF((0.35 + 0.65 * note.velocity / 127) * (1.0 if playing else 0.5))
        box = rect.adjusted(0.5, 0.5, -0.5, -0.5)
        p.fillRect(box, fill)
        p.setPen(QPen(QColor(theme.SELECTION_OUTLINE) if selected else color.darker(220), 1))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawRect(box)
        if rect.width() >= 30 and rect.height() >= 10:
            p.setPen(QColor(theme.ACCENT_TEXT))
            p.drawText(box.adjusted(3, 0, -2, 0), Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft,
                       note_name(note.pitch))

    # --- Mouse -----------------------------------------------------------------------

    def dragging(self) -> bool:
        """Whether a drag is under way (the note tools stay out of its way)."""
        return self._gesture is not None and self._gesture.active

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        self.setFocus()
        roll = self.roll
        pos, mods = event.position(), event.modifiers()
        if is_pan_modifier(mods):
            self._gesture = PanGesture(self, pos)
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
            return
        additive = bool(mods & (Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.ShiftModifier))
        hit = self.note_at(pos)
        if hit is None:
            if not additive:
                roll.set_selection(set())
            self._gesture = SelectNotesGesture(self, pos, additive)
            return
        note, zone = hit
        # Clicking one of several selected notes selects just it, and Ctrl-clicking
        # a selected note deselects it, but only once the mouse comes up without
        # dragging: dragging moves (Ctrl: copies) the whole selection instead.
        self._select_on_click = None
        if note in roll.selected:
            if mods & Qt.KeyboardModifier.ShiftModifier:
                pass
            elif mods & Qt.KeyboardModifier.ControlModifier:
                self._select_on_click = roll.selected - {note}
            elif len(roll.selected) > 1:
                self._select_on_click = {note}
        else:
            roll.set_selection((roll.selected if additive else set()) | {note})
        roll.audition(note.pitch, note.velocity)
        moving = sorted(roll.selected, key=by_time)
        if zone == "body":
            self._gesture = MoveNotesGesture(self, pos, note, moving)
        else:
            self._gesture = ResizeNotesGesture(self, pos, note, moving, zone)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._gesture:
            self._gesture.move(event.position(), event.modifiers())
            self.update()
            return
        self._update_cursor(event.position(), event.modifiers())

    def _update_cursor(self, pos: QPointF, mods) -> None:
        hit = self.note_at(pos)
        if is_pan_modifier(mods):
            self.setCursor(Qt.CursorShape.OpenHandCursor)
        elif hit is None:
            self.setCursor(Qt.CursorShape.ArrowCursor)
        elif hit[1] == "body":
            self.setCursor(Qt.CursorShape.PointingHandCursor)
        else:
            self.setCursor(Qt.CursorShape.SizeHorCursor)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        gesture, self._gesture = self._gesture, None
        if self._select_on_click is not None and gesture is not None and not gesture.active:
            self.roll.set_selection(self._select_on_click)
        self._select_on_click = None
        if gesture is not None:
            gesture.finish()
        self.roll.release_audition()
        self._update_cursor(event.position(), event.modifiers())
        self.roll.place_tools()
        self.update()

    def _on_modifiers(self, mods) -> None:
        """Show the hand as soon as Ctrl+Alt is held, without moving the mouse."""
        if self._gesture is None and self.underMouse():
            self._update_cursor(QPointF(self.mapFromGlobal(QCursor.pos())), mods)

    def keyReleaseEvent(self, event: QKeyEvent) -> None:
        self._on_modifiers(event.modifiers())
        super().keyReleaseEvent(event)

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        roll = self.roll
        clip = roll.clip()
        if event.button() != Qt.MouseButton.LeftButton or clip is None or is_pan_modifier(event.modifiers()):
            return
        pos = event.position()
        hit = self.note_at(pos)
        if hit is not None:
            note = hit[0]
            roll.tools_wanted = False
            roll.commit(notes.place(clip.notes, [note], []), "Delete Note", selected=roll.selected - {note})
            return
        view = roll.view
        step = view.grid_step()
        beat = max(0.0, view.x_to_beat(pos.x()))
        if view.snap and not event.modifiers() & Qt.KeyboardModifier.AltModifier:
            beat = math.floor(beat / step + 1e-9) * step  # the grid cell that was clicked
        note = Note(pitch=roll.pitch_at(pos.y()), start=beat, length=step)
        roll.tools_wanted = False
        roll.commit(notes.place(clip.notes, [], [note]), "Add Note", selected={note})
        roll.audition(note.pitch, note.velocity)

    def wheelEvent(self, event: QWheelEvent) -> None:
        view = self.roll.view
        delta, mods = event.angleDelta(), event.modifiers()
        if mods & Qt.KeyboardModifier.AltModifier and not mods & Qt.KeyboardModifier.ControlModifier:
            # Qt may report Alt+wheel as horizontal scrolling, so accept either axis.
            self.roll.zoom_rows((delta.y() or delta.x()) / 120.0, event.position().y())
        elif mods & Qt.KeyboardModifier.ControlModifier:
            view.zoom_at(event.position().x(), 1.2 ** (delta.y() / 120.0))
        elif mods & Qt.KeyboardModifier.ShiftModifier or delta.x():
            pixels = -(delta.x() or delta.y()) / 120.0 * 80.0
            view.set_scroll_beats(view.scroll_beats + pixels / view.px_per_beat)
        else:
            view.set_scroll_y(view.scroll_y - delta.y() / 120.0 * 3 * self.roll.row_height)
        event.accept()

    # --- Keys ------------------------------------------------------------------------

    @staticmethod
    def _handles(event: QKeyEvent) -> bool:
        key = event.key()
        if event.modifiers() & Qt.KeyboardModifier.ControlModifier and key in (Qt.Key.Key_A, Qt.Key.Key_D,
                                                                              Qt.Key.Key_U):
            return True
        return key in (Qt.Key.Key_Delete, Qt.Key.Key_Backspace, Qt.Key.Key_Up, Qt.Key.Key_Down, Qt.Key.Key_Left,
                       Qt.Key.Key_Right)

    def event(self, event: QEvent) -> bool:
        # Take Delete, Ctrl+A and Ctrl+D from the main window's (arrangement) shortcuts.
        if event.type() == QEvent.Type.ShortcutOverride and self._handles(event):
            event.accept()
            return True
        return super().event(event)

    def keyPressEvent(self, event: QKeyEvent) -> None:
        self._on_modifiers(event.modifiers())
        roll = self.roll
        clip = roll.clip()
        if clip is None or not self._handles(event):
            super().keyPressEvent(event)
            return
        key = event.key()
        shift = bool(event.modifiers() & Qt.KeyboardModifier.ShiftModifier)
        selected = sorted(roll.selected, key=by_time)
        if key == Qt.Key.Key_A:
            roll.set_selection(clip.notes, tools=True)
        elif key == Qt.Key.Key_U:
            roll.quantize()  # the selected notes, or all
        elif not selected:
            pass
        elif key in (Qt.Key.Key_Delete, Qt.Key.Key_Backspace):
            roll.commit(notes.place(clip.notes, selected, []), "Delete Notes", selected=set())
        elif key == Qt.Key.Key_D:
            start, end = notes.span(selected)
            copies = notes.shifted(selected, end - start, 0)
            roll.commit(notes.place(clip.notes, [], copies), "Duplicate Notes", selected=copies)
        else:
            if key in (Qt.Key.Key_Up, Qt.Key.Key_Down):
                delta_beats, delta_pitch = 0.0, (12 if shift else 1) * (1 if key == Qt.Key.Key_Up else -1)
            else:
                step = roll.project.time_signature.beats_per_bar if shift else roll.view.grid_step()
                delta_beats, delta_pitch = step * (1 if key == Qt.Key.Key_Right else -1), 0
            delta_beats, delta_pitch = notes.clamp_move(selected, delta_beats, delta_pitch)
            if delta_beats or delta_pitch:
                moved = notes.shifted(selected, delta_beats, delta_pitch)
                roll.commit(notes.place(clip.notes, selected, moved),
                            "Transpose Notes" if delta_pitch else "Move Notes", selected=moved)
        event.accept()
