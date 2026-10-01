"""Piano roll: the clip view of a MIDI clip, laid out like Ableton's MIDI editor.
The ruler on top; keys on the left, the notes in the middle, velocities below.
Selecting a group of notes by dragging (or Ctrl+A) brings up the note tools
(legato, timing ×2 and ÷2, quantize, humanize) by them.

Times are beats of the clip's content (the ruler's 1 is its first beat). The
part the clip plays in the arrangement is lit; notes outside it are kept but
dimmed. Every edit goes through the editor, so it is undoable and heard at once.
"""

from __future__ import annotations

import math
import random

from PySide6.QtCore import QPointF, QRect, QRectF, Qt, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPainterPath, QWheelEvent
from PySide6.QtWidgets import QGridLayout, QLabel, QScrollBar, QWidget

from ... import theme
from ...audio.engine_bridge import EngineBridge
from ...model import notes
from ...model.editor import ProjectEditor
from ...model.notes import is_black_key, note_name
from ...model.project import MidiClip, Note
from ...model.timebase import format_bar_label
from .. import icons
from ..arrangement.grid import grid_lines, label_step
from ..arrangement.view_state import ViewState
from ..widgets import ToggleButton
from .note_grid import NoteGrid
from .note_tools import NoteTools
from .velocity_lane import VelocityLane

KEYS_WIDTH = 64
RULER_HEIGHT = 24
ROW_HEIGHT = 12
MIN_ROW_HEIGHT = 5
MAX_ROW_HEIGHT = 36
ROW_HEIGHT_STEP = 1.5  # pixels per wheel notch (Alt+wheel)
DEFAULT_PITCH = 60  # C3: centred for a clip without notes
PREVIEW_VELOCITY = 100


class PianoRoll(QWidget):
    locate_requested = Signal(float)  # an arrangement beat

    def __init__(self, editor: ProjectEditor, bridge: EngineBridge, parent: QWidget | None = None):
        super().__init__(parent)
        self.editor = editor
        self.project = editor.project
        self.bridge = bridge
        self.view = ViewState(self.project, self)  # time in content beats; scroll_y in pixels
        self.view.grid_level = 1  # a little wider than the arrangement's: 1/16 notes across a bar
        self.row_height = ROW_HEIGHT
        self._row_height_exact = float(ROW_HEIGHT)  # keeps a trackpad's small steps adding up
        self.track_id: str | None = None
        self.clip_id: str | None = None
        self.selected: set[Note] = set()
        self.playhead: float | None = None  # content beat, while the arrangement is inside the clip
        self.auditioned: int | None = None  # the key sounding while the mouse holds it
        self._fit_pending = False
        self._rng = random.Random()  # for Humanize
        # The note tools show for a group chosen by a rubber band or Ctrl+A, not
        # for notes clicked or drawn; they stay while that group is edited.
        self.tools_wanted = False

        self.preview = ToggleButton(icon=icons.headphones(), role="tool",
                                    tooltip="Hear notes as you click, add and move them")
        self.preview.setChecked(True)
        self.preview.toggled.connect(self._preview_toggled)
        self.ruler = PianoRuler(self)
        self.keys = PianoKeys(self)
        self.keys.setFixedWidth(KEYS_WIDTH)
        self.grid = NoteGrid(self)
        self.velocity = VelocityLane(self)
        self.tools = NoteTools(self, self.grid)  # floats over the grid, by the selected notes
        velocity_label = QLabel("Velocity")
        velocity_label.setFixedWidth(KEYS_WIDTH)
        velocity_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        velocity_label.setStyleSheet(f"color: {theme.TEXT_DIM}; font-size: 8pt; background: {theme.PANEL};")
        self.hbar = QScrollBar(Qt.Orientation.Horizontal)
        self.vbar = QScrollBar(Qt.Orientation.Vertical)
        for bar in (self.hbar, self.vbar):
            bar.setFocusPolicy(Qt.FocusPolicy.NoFocus)

        grid = QGridLayout(self)
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setSpacing(0)
        grid.addWidget(self.preview, 0, 0, Qt.AlignmentFlag.AlignCenter)
        grid.addWidget(self.ruler, 0, 1)
        grid.addWidget(self.keys, 1, 0)
        grid.addWidget(self.grid, 1, 1)
        grid.addWidget(self.vbar, 1, 2)
        grid.addWidget(velocity_label, 2, 0)
        grid.addWidget(self.velocity, 2, 1)
        grid.addWidget(self.hbar, 3, 1)
        grid.setColumnStretch(1, 1)
        grid.setRowStretch(1, 1)
        self.setFocusProxy(self.grid)

        self.view.changed.connect(self._on_view_changed)
        self.view.grid_changed.connect(self.repaint_all)
        self.view.vscroll_changed.connect(self._on_vscroll)
        self.hbar.valueChanged.connect(lambda value: self.view.set_scroll_beats(value / self.view.px_per_beat))
        self.vbar.valueChanged.connect(self.view.set_scroll_y)
        self.project.clips_changed.connect(lambda track_id: self.refresh() if track_id == self.track_id else None)
        self.project.settings_changed.connect(self.repaint_all)
        self.project.track_changed.connect(lambda track_id: self.repaint_all() if track_id == self.track_id else None)
        bridge.position_changed.connect(self._on_position)

    # --- The clip ------------------------------------------------------------------

    def set_clip(self, track_id: str | None, clip_id: str | None) -> None:
        """Show a MIDI clip (fitted to the view the first time), or nothing."""
        self.release_audition()
        if (track_id, clip_id) != (self.track_id, self.clip_id):
            self.track_id, self.clip_id = track_id, clip_id
            self.selected = set()
            self.tools_wanted = False
            self._fit_pending = clip_id is not None
        self.refresh()

    def clip(self) -> MidiClip | None:
        if self.track_id is None or not self.project.has_track(self.track_id):
            return None
        return next((c for c in self.project.track(self.track_id).clips
                     if c.id == self.clip_id and isinstance(c, MidiClip)), None)

    def track_color(self) -> str:
        return self.project.track(self.track_id).color if self.clip() is not None else theme.ACCENT

    def refresh(self) -> None:
        """After the clip changed (edits, undo): forget selected notes that are gone, repaint."""
        clip = self.clip()
        self.selected &= set(clip.notes) if clip is not None else set()
        self._fit_if_ready()
        self._update_bars()
        self._on_position(self.bridge.position)
        self.repaint_all()

    def commit(self, clip_notes, text: str, merge_key: object | None = None, selected=None) -> None:
        """Make `clip_notes` the clip's notes (one undo step per `merge_key`) and select `selected`."""
        if self.clip() is None:
            return
        if selected is not None:
            self.selected = set(selected)
        self.editor.set_clip_notes((self.track_id, self.clip_id), clip_notes, text, merge_key)
        self.repaint_all()

    def set_selection(self, selected, tools: bool = False) -> None:
        """Select `selected`; `tools` brings up the note tools by them."""
        self.selected = set(selected)
        self.tools_wanted = tools and bool(self.selected)
        self.repaint_all()

    # --- Tools ---------------------------------------------------------------------

    def place_tools(self) -> None:
        """Show the note tools by the selected notes when they were chosen as a
        group, or hide them: with nothing selected, and while a drag is moving
        notes or drawing a rubber band."""
        shown = self.tools_wanted and not self.grid.dragging()
        rects = [self.grid.note_rect(n) for n in self.selected] if shown else []
        area = None
        for rect in rects:
            area = rect if area is None else area.united(rect)
        self.tools.show_near(area, len(rects))

    def tool_targets(self) -> list[Note]:
        """What the tools act on: the selected notes, or all of them if none are
        (Ctrl+U with nothing selected)."""
        clip = self.clip()
        if clip is None:
            return []
        return sorted(self.selected or clip.notes, key=notes.by_time)

    def _apply_tool(self, targets: list[Note], changed: list[Note], text: str) -> None:
        clip = self.clip()
        if clip is not None and targets:
            self.commit(notes.place(clip.notes, targets, changed), text,
                        selected=changed if self.selected else set())

    def legato(self) -> None:
        clip, targets = self.clip(), self.tool_targets()
        if clip is not None:
            self._apply_tool(targets, notes.legato(targets, clip.notes, clip.window_end), "Legato")

    def scale_time(self, factor: float) -> None:
        targets = self.tool_targets()
        self._apply_tool(targets, notes.time_scaled(targets, factor), "Timing ×2" if factor > 1 else "Timing ÷2")

    def quantize(self) -> None:
        targets = self.tool_targets()
        self._apply_tool(targets, notes.quantized(targets, self.tools.quantize_step, self.tools.quantize_amount),
                         "Quantize")

    def humanize(self) -> None:
        targets = self.tool_targets()
        self._apply_tool(targets, notes.humanized(targets, self._rng, self.tools.humanize_level), "Humanize")

    # --- Geometry ------------------------------------------------------------------

    def pitch_top(self, pitch: int) -> float:
        """Top of a key's row, in the note grid's (and the keys') coordinates."""
        return (127 - pitch) * self.row_height - self.view.scroll_y

    def pitch_at(self, y: float) -> int:
        return max(0, min(127, 127 - math.floor((y + self.view.scroll_y) / self.row_height)))

    def zoom_rows(self, notches: float, anchor_y: float) -> None:
        """Make the keys' rows taller (or shorter), keeping the pitch under
        `anchor_y` (a note grid y) in place."""
        exact = max(MIN_ROW_HEIGHT, min(MAX_ROW_HEIGHT, self._row_height_exact + notches * ROW_HEIGHT_STEP))
        self._row_height_exact = exact
        height = round(exact)
        if height == self.row_height:
            return
        rows = (anchor_y + self.view.scroll_y) / self.row_height
        self.row_height = height
        self._update_bars()
        self.view.set_scroll_y(rows * height - anchor_y)
        self._on_vscroll()

    def _fit_if_ready(self) -> None:
        """Zoom to the part the clip plays and centre its notes. Waits until the
        note grid has its size (the clip view may not be laid out yet)."""
        clip = self.clip()
        if not self._fit_pending or clip is None or self.grid.width() <= 1 or not self.isVisible():
            return
        self._fit_pending = False
        self.view.zoom_to_fit(clip.offset_beats, clip.window_end, self.grid.width() * 0.96)
        pitches = [n.pitch for n in clip.notes] or [DEFAULT_PITCH]
        self._update_bars()
        centre = (min(pitches) + max(pitches)) / 2
        self.view.set_scroll_y((127.5 - centre) * self.row_height - self.grid.height() / 2)

    def _update_bars(self) -> None:
        view = self.view
        width, height = max(1, self.grid.width()), max(1, self.grid.height())
        clip = self.clip()
        end = view.x_to_beat(width)
        if clip is not None:
            end = max(end, clip.window_end, max((n.end for n in clip.notes), default=0.0))
        content_end = end + 4 * self.project.time_signature.beats_per_bar
        self.hbar.blockSignals(True)
        self.hbar.setRange(0, max(0, int(content_end * view.px_per_beat - width)))
        self.hbar.setPageStep(width)
        self.hbar.setSingleStep(max(1, width // 20))
        self.hbar.setValue(int(view.scroll_beats * view.px_per_beat))
        self.hbar.blockSignals(False)
        view.max_scroll_y = max(0, 128 * self.row_height - height)
        self.vbar.blockSignals(True)
        self.vbar.setRange(0, view.max_scroll_y)
        self.vbar.setPageStep(height)
        self.vbar.setSingleStep(self.row_height)
        self.vbar.blockSignals(False)
        if view.scroll_y > view.max_scroll_y:
            view.set_scroll_y(view.max_scroll_y)
        self._on_vscroll()

    def _on_view_changed(self) -> None:
        self._update_bars()
        self.repaint_all()

    def _on_vscroll(self) -> None:
        self.vbar.blockSignals(True)
        self.vbar.setValue(self.view.scroll_y)
        self.vbar.blockSignals(False)
        self.keys.update()
        self.grid.update()
        self.place_tools()

    def repaint_all(self) -> None:
        for widget in (self.ruler, self.keys, self.grid, self.velocity):
            widget.update()
        self.place_tools()

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        self._fit_if_ready()
        self._update_bars()

    def showEvent(self, event) -> None:
        super().showEvent(event)
        self._fit_if_ready()
        self._update_bars()

    def hideEvent(self, event) -> None:
        self.release_audition()
        super().hideEvent(event)

    # --- Playhead ------------------------------------------------------------------

    def _on_position(self, beat: float) -> None:
        clip = self.clip()
        playhead = None
        if clip is not None and clip.start_beat <= beat < clip.end_beat():
            playhead = beat - clip.start_beat + clip.offset_beats
        if playhead == self.playhead:
            return
        for widget in (self.ruler, self.grid, self.velocity):
            for b in (self.playhead, playhead):
                if b is not None:
                    x = int(self.view.beat_to_x(b))
                    widget.update(QRect(x - 6, 0, 13, widget.height()))
        self.playhead = playhead

    def draw_playhead(self, p: QPainter, height: float) -> None:
        if self.playhead is not None:
            p.fillRect(QRectF(round(self.view.beat_to_x(self.playhead)), 0, 1, height), QColor(theme.PLAYHEAD))

    # --- Hearing notes ---------------------------------------------------------------

    def audition(self, pitch: int, velocity: int = PREVIEW_VELOCITY) -> None:
        """Sound a key on the track's instrument until release_audition()."""
        if pitch == self.auditioned:
            return
        self.release_audition()
        if not self.preview.isChecked() or self.clip() is None:
            return
        self.bridge.preview_note(self.track_id, pitch, velocity)
        self.auditioned = pitch
        self.keys.update()

    def _preview_toggled(self, enabled: bool) -> None:
        if not enabled:
            self.release_audition()

    def release_audition(self) -> None:
        if self.auditioned is not None and self.track_id is not None:
            self.bridge.preview_note(self.track_id, self.auditioned, 0)
        self.auditioned = None
        self.keys.update()


class PianoRuler(QWidget):
    """Bar numbers in the clip's own time and the part it plays. Click to play from
    there; drag vertically to zoom, horizontally to scroll (like the arrangement's)."""

    def __init__(self, roll: PianoRoll):
        super().__init__(roll)
        self.roll = roll
        self._drag: dict | None = None
        self.setFixedHeight(RULER_HEIGHT)

    def paintEvent(self, event) -> None:
        p = QPainter(self)
        roll, view = self.roll, self.roll.view
        rect = QRectF(event.rect())
        p.fillRect(rect, QColor(theme.PANEL))
        clip = roll.clip()
        if clip is not None:
            x0, x1 = view.beat_to_x(clip.offset_beats), view.beat_to_x(clip.window_end)
            p.fillRect(QRectF(x0, self.height() - 5, x1 - x0, 4), QColor(roll.track_color()))
        step = view.grid_step()
        every = label_step(view, step)
        p.setFont(theme.ui_font(8))
        for x, beat, kind in grid_lines(view, rect.left() - 60, rect.right() + 1, step):
            tick = {"bar": 10, "beat": 6, "sub": 3}[kind]
            p.fillRect(QRectF(round(x), self.height() - tick, 1, tick),
                       QColor(theme.TEXT_DIM if kind == "bar" else theme.GRID_BAR))
            if abs(beat / every - round(beat / every)) < 1e-6:
                p.setPen(QColor(theme.TEXT))
                p.drawText(QPointF(round(x) + 3, 12), format_bar_label(beat, roll.project.time_signature))
        p.fillRect(QRectF(rect.left(), self.height() - 1, rect.width(), 1), QColor(theme.BORDER))
        if roll.playhead is not None:
            x = round(view.beat_to_x(roll.playhead))
            path = QPainterPath(QPointF(x - 5, self.height() - 8))
            path.lineTo(x + 6, self.height() - 8)
            path.lineTo(x + 0.5, self.height() - 1)
            path.closeSubpath()
            p.fillPath(path, QColor(theme.PLAYHEAD))

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        self.roll.grid.setFocus()
        pos = event.position()
        self._drag = {"x": pos.x(), "y": pos.y(), "last_y": pos.y(), "scroll": self.roll.view.scroll_beats,
                      "beat": self.roll.view.x_to_beat(pos.x()), "moved": False, "pan": False}

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        d = self._drag
        if d is None:
            return
        view = self.roll.view
        pos = event.position()
        dx, dy = pos.x() - d["x"], pos.y() - d["y"]
        if not d["moved"] and max(abs(dx), abs(dy)) > 3:
            d["moved"] = True
            d["pan"] = abs(dx) > abs(dy)
        if d["pan"]:
            view.set_scroll_beats(d["scroll"] - dx / view.px_per_beat)
        elif d["moved"]:
            view.zoom_at(d["x"], 1.012 ** (pos.y() - d["last_y"]))
            d["last_y"] = pos.y()

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        d, self._drag = self._drag, None
        clip = self.roll.clip()
        if d and not d["moved"] and clip is not None:
            bypass = bool(event.modifiers() & Qt.KeyboardModifier.AltModifier)
            beat = self.roll.view.snap_beat(d["beat"], bypass)
            self.roll.locate_requested.emit(max(0.0, clip.to_timeline(beat)))


class PianoKeys(QWidget):
    """The keyboard: click a key to hear it and select its notes (Shift adds)."""

    def __init__(self, roll: PianoRoll):
        super().__init__(roll)
        self.roll = roll
        self._pressed = False

    def paintEvent(self, event) -> None:
        p = QPainter(self)
        roll = self.roll
        rect = QRectF(event.rect())
        height = roll.row_height
        p.fillRect(rect, QColor(theme.EMPTY_AREA))
        p.setFont(theme.ui_font(7))
        black_width = self.width() * 0.6
        for pitch in range(roll.pitch_at(rect.bottom()), roll.pitch_at(rect.top()) + 1):
            row = QRectF(0, roll.pitch_top(pitch), self.width() - 1, height)
            p.fillRect(row, QColor(theme.KEY_WHITE))
            if is_black_key(pitch):
                p.fillRect(QRectF(0, row.top(), black_width, height), QColor(theme.KEY_BLACK))
            if pitch == roll.auditioned:
                p.fillRect(QRectF(0, row.top(), black_width if is_black_key(pitch) else row.width(), height),
                           QColor(theme.ACCENT))
            if pitch % 12 in (0, 5):  # the gap between two white keys (B|C, E|F)
                p.fillRect(QRectF(0, row.bottom() - 1, row.width(), 1), QColor(theme.TEXT_DIM))
            if pitch % 12 == 0:
                p.setPen(QColor(theme.KEY_LABEL))
                p.drawText(row.adjusted(0, 0, -4, 0), Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter,
                           note_name(pitch))
        p.fillRect(QRectF(self.width() - 1, rect.top(), 1, rect.height()), QColor(theme.BORDER))

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        roll = self.roll
        roll.grid.setFocus()
        pitch = roll.pitch_at(event.position().y())
        clip = roll.clip()
        if clip is not None:
            keep = roll.selected if event.modifiers() & Qt.KeyboardModifier.ShiftModifier else set()
            roll.set_selection(keep | {n for n in clip.notes if n.pitch == pitch})
        self._pressed = True
        roll.audition(pitch)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._pressed:
            self.roll.audition(self.roll.pitch_at(event.position().y()))

    def mouseReleaseEvent(self, _event: QMouseEvent) -> None:
        self._pressed = False
        self.roll.release_audition()

    def wheelEvent(self, event: QWheelEvent) -> None:
        self.roll.grid.wheelEvent(event)
