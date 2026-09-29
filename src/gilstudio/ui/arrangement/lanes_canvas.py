"""The track lanes: clips, grid, loop, playhead, and all clip mouse editing.

Custom-painted for speed: each paint only touches what intersects the dirty
rectangle, and playhead motion repaints just two thin strips.
"""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QPointF, QRect, QRectF, Qt, Signal
from PySide6.QtGui import (
    QColor,
    QContextMenuEvent,
    QCursor,
    QDragEnterEvent,
    QDropEvent,
    QMouseEvent,
    QPainter,
    QPainterPath,
    QPen,
    QPixmap,
    QWheelEvent,
)
from PySide6.QtWidgets import QMenu, QWidget

from ... import theme
from ...audio.engine_bridge import EngineBridge, is_audio_file
from ...model.editor import BUILTIN_DEVICES, ProjectEditor
from ...model.project import MAX_TRACK_HEIGHT, MIN_TRACK_HEIGHT, Clip
from ..browser.browser_models import PLUGIN_MIME, device_kinds
from .grid import draw_grid, draw_loop_region
from .interactions import (
    ClipGesture,
    MoveClipsGesture,
    PanGesture,
    RubberBandGesture,
    TimeSelectGesture,
    TrimGesture,
)
from .view_state import Selection, TrackLayout, ViewState
from .waveform_cache import WaveformCache

EDGE_GRAB = 6  # trim handles: this many pixels inside each end of a clip's title bar
TITLE_HEIGHT = 16  # also the grab area for selecting/moving the clip
MIN_TITLE_ROW = 30  # clips in shorter rows have no title bar: the whole clip moves it
HEIGHT_STEP = 12  # pixels per wheel notch when Alt+wheel resizes a track
SELECTION_TINT = QColor(80, 150, 210, 150)  # selected clips and time selections, as in Ableton


def audio_paths(mime) -> list[str]:
    if not mime.hasUrls():
        return []
    return [u.toLocalFile() for u in mime.urls() if u.isLocalFile() and is_audio_file(u.toLocalFile())]


_TRIM_CURSORS: dict[str, QCursor] = {}


def trim_cursor(edge: str) -> QCursor:
    """Ableton-style bracket: '[' trims a clip's start, ']' its end. The bracket
    opens toward the clip being trimmed; the hotspot is on its upright."""
    if edge not in _TRIM_CURSORS:
        size = 24
        pixmap = QPixmap(size, size)
        pixmap.fill(Qt.GlobalColor.transparent)
        x = 9 if edge == "left" else 14  # the upright
        serif = 5 if edge == "left" else -5
        path = QPainterPath()
        path.moveTo(x + serif, 3)
        path.lineTo(x, 3)
        path.lineTo(x, 20)
        path.lineTo(x + serif, 20)
        arrow = -1 if edge == "left" else 1  # small arrow pointing outward
        tip = x + arrow * 7
        path.moveTo(x + arrow * 2, 11.5)
        path.lineTo(tip, 11.5)
        path.moveTo(tip - arrow * 3, 8.5)
        path.lineTo(tip, 11.5)
        path.lineTo(tip - arrow * 3, 14.5)
        p = QPainter(pixmap)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        for color, width in ((QColor(0, 0, 0), 4.0), (QColor(255, 255, 255), 2.0)):
            p.setPen(QPen(color, width, Qt.PenStyle.SolidLine, Qt.PenCapStyle.SquareCap,
                          Qt.PenJoinStyle.MiterJoin))
            p.drawPath(path)
        p.end()
        _TRIM_CURSORS[edge] = QCursor(pixmap, x, 11)
    return _TRIM_CURSORS[edge]


def is_pan_modifier(mods) -> bool:
    """Ctrl+Alt: drag to scroll the arrangement, as in Ableton."""
    return bool(mods & Qt.KeyboardModifier.ControlModifier and mods & Qt.KeyboardModifier.AltModifier)


def resize_track_by_wheel(editor: ProjectEditor, track_id: str, delta: int) -> None:
    height = editor.project.track(track_id).height + round(delta / 120.0 * HEIGHT_STEP)
    editor.set_track_height(track_id, max(MIN_TRACK_HEIGHT, min(MAX_TRACK_HEIGHT, height)))


class LanesCanvas(QWidget):
    status_message = Signal(str)
    clip_view_requested = Signal(str, str)  # track id, clip id

    def __init__(self, editor: ProjectEditor, view: ViewState, layout: TrackLayout, selection: Selection,
                 bridge: EngineBridge, waveforms: WaveformCache, parent: QWidget | None = None):
        super().__init__(parent)
        self.editor = editor
        self.project = editor.project
        self.view = view
        self.layout_model = layout
        self.selection = selection
        self.bridge = bridge
        self.waveforms = waveforms
        self._playhead = 0.0
        self._gesture: ClipGesture | None = None
        self._drop_preview: tuple[int | None, float, list[tuple[str, float]]] | None = None
        self._hover_edge: tuple[str, str] | None = None  # (clip id, "left"/"right") under the mouse
        self.setAcceptDrops(True)
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.ClickFocus)
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent)

        for signal in (view.changed, view.vscroll_changed, view.grid_changed, selection.changed,
                       selection.insert_changed, self.project.settings_changed, bridge.source_ready,
                       bridge.source_failed, self.project.clips_changed, self.project.track_changed):
            signal.connect(lambda *_args: self.update())

    # --- Geometry ------------------------------------------------------------------

    def row_index_at(self, y: float, clamp: bool = False) -> int | None:
        index = self.layout_model.row_index_at(y + self.view.scroll_y)
        if index is None and clamp and self.layout_model.rows:
            return 0 if y + self.view.scroll_y < 0 else len(self.layout_model.rows) - 1
        return index

    def _clip_rect(self, clip: Clip, row_top: float, row_height: int) -> QRectF:
        x0 = self.view.beat_to_x(clip.start_beat)
        x1 = self.view.beat_to_x(clip.end_beat(self.project.tempo))
        return QRectF(x0, row_top + 1, max(2.0, x1 - x0), row_height - 3)

    def in_clip_band(self, pos: QPointF) -> bool:
        """Whether `pos` is in the top band of a lane, where clips (not automation)
        are selected. Short lanes have no title bar, so all of them is the band."""
        index = self.row_index_at(pos.y(), clamp=True)
        if index is None:
            return False
        row = self.layout_model.rows[index]
        return row.height < MIN_TITLE_ROW or 0 <= pos.y() + self.view.scroll_y - row.top < TITLE_HEIGHT + 1

    def hit_clip(self, pos: QPointF) -> tuple[str, Clip, str] | None:
        """(track id, clip, zone) under `pos`; zone is left/right (trim handles, at
        the ends of the title bar), title (select & move) or body (time selection
        / insert marker)."""
        index = self.row_index_at(pos.y())
        if index is None:
            return None
        row = self.layout_model.rows[index]
        top = row.top - self.view.scroll_y
        for clip in reversed(self.project.track(row.track_id).clips):
            rect = self._clip_rect(clip, top, row.height)
            # Only inside the clip: next to it, or on a neighbour's side of a
            # shared boundary, you are not trimming this clip.
            if rect.left() <= pos.x() <= rect.right():
                if not (rect.height() < MIN_TITLE_ROW or pos.y() < rect.top() + TITLE_HEIGHT):
                    return row.track_id, clip, "body"
                grab = min(EDGE_GRAB, rect.width() / 3)
                if pos.x() <= rect.left() + grab:
                    zone = "left"
                elif pos.x() >= rect.right() - grab:
                    zone = "right"
                else:
                    zone = "title"
                return row.track_id, clip, zone
        return None

    # --- Playhead ------------------------------------------------------------------

    def set_playhead(self, beat: float) -> None:
        for b in (self._playhead, beat):
            x = int(self.view.beat_to_x(b))
            self.update(QRect(x - 2, 0, 5, self.height()))
        self._playhead = beat

    # --- Painting ------------------------------------------------------------------

    def paintEvent(self, event) -> None:
        p = QPainter(self)
        visible = QRectF(event.rect())
        view = self.view
        p.fillRect(visible, QColor(theme.EMPTY_AREA))

        rows = self.layout_model.visible_rows(view.scroll_y + visible.top(), view.scroll_y + visible.bottom() + 1)
        tracks_bottom = min(float(self.height()), self.layout_model.total_height - view.scroll_y)
        for _, row in rows:
            y = row.top - view.scroll_y
            color = theme.LANE_SELECTED if row.track_id == self.selection.track_id else theme.LANE
            p.fillRect(QRectF(visible.left(), y, visible.width(), row.height), QColor(color))
        draw_grid(p, view, visible.left(), visible.right(), max(0.0, visible.top()), tracks_bottom)
        draw_loop_region(p, view, visible.left(), visible.right(), 0.0, tracks_bottom)

        gesture = self._gesture
        hidden = gesture.hidden_ids() if gesture else set()
        # Clips touched by a clip range aren't drawn selected: only the range is.
        whole_clips = set() if self.selection.clip_range else self.selection.clips
        for _, row in rows:
            track = self.project.track(row.track_id)
            y = row.top - view.scroll_y
            for clip in track.clips:
                if clip.id in hidden:
                    continue
                rect = self._clip_rect(clip, y, row.height)
                if rect.left() > visible.right():
                    break
                if rect.right() < visible.left():
                    continue
                self._draw_clip(p, track.color, clip, rect, visible, (track.id, clip.id) in whole_clips)
            p.fillRect(QRectF(visible.left(), y + row.height - 1, visible.width(), 1), QColor(theme.BORDER))

        if gesture:
            for row_index, color, clip in gesture.ghosts():
                row = self.layout_model.rows[row_index]
                rect = self._clip_rect(clip, row.top - view.scroll_y, row.height)
                self._draw_clip(p, color, clip, rect, visible, True, ghost=True)
        self._draw_drop_preview(p)

        if not self.project.tracks and not self._drop_preview:
            p.setPen(QColor(theme.TEXT_DIM))
            p.setFont(theme.ui_font(10))
            p.drawText(QRectF(self.rect()), Qt.AlignmentFlag.AlignCenter,
                       "Drag audio files here from the browser\nor press Ctrl+T to create an audio track")

        time_range = self.selection.time_range
        if time_range is not None:
            start, end, track_ids = time_range
            x0, x1 = view.beat_to_x(start), view.beat_to_x(end)
            for track_id in track_ids:
                row = self.layout_model.row_for(track_id)
                if row is None:
                    continue
                area = QRectF(x0, row.top - view.scroll_y, x1 - x0, row.height - 1)
                if not self.selection.clip_range and row.height >= MIN_TITLE_ROW:
                    # A lane (automation) range leaves the clips' title band alone.
                    area.setTop(area.top() + TITLE_HEIGHT + 1)
                p.fillRect(area, SELECTION_TINT)

        # Insert marker on the selected track (Ableton's blinking cursor, minus the blink)
        row = self.layout_model.row_for(self.selection.track_id) if self.selection.track_id else None
        if row is not None and not self.selection.clips and time_range is None:
            x = round(view.beat_to_x(self.selection.insert_beat))
            p.fillRect(QRectF(x, row.top - view.scroll_y, 1, row.height - 1), QColor(theme.INSERT_MARKER))

        x = round(view.beat_to_x(self._playhead))
        if visible.left() - 2 <= x <= visible.right() + 2:
            p.fillRect(QRectF(x, 0, 1, self.height()), QColor(theme.PLAYHEAD))

        band = gesture.rubber_band() if gesture else None
        if band is not None:
            p.fillRect(band, theme.RUBBER_BAND)
            p.setPen(QPen(QColor(theme.ACCENT), 1))
            p.drawRect(band)

    def _draw_clip(self, p: QPainter, track_color: str, clip: Clip, rect: QRectF, visible: QRectF,
                   selected: bool, ghost: bool = False) -> None:
        base = QColor(track_color)
        title_h = TITLE_HEIGHT if rect.height() >= MIN_TITLE_ROW else 0
        body = rect.adjusted(0, title_h, 0, 0)
        body_color = QColor(base)
        body_color.setHsvF(base.hsvHueF(), base.hsvSaturationF() * 0.6, min(1.0, base.valueF() * 0.78))
        if ghost:
            body_color.setAlphaF(0.75)
        p.save()
        p.setClipRect(rect.intersected(visible).adjusted(-1, -1, 1, 1))
        p.fillRect(body, body_color)
        if selected:
            p.fillRect(body, SELECTION_TINT)
        if title_h:
            title = QRectF(rect.left(), rect.top(), rect.width(), title_h)
            p.fillRect(title, base.lighter(115) if selected else base)
            if rect.width() > 16:
                p.setPen(QColor(theme.ACCENT_TEXT))
                p.setFont(theme.ui_font(7.5))
                text_rect = title.adjusted(4, 0, -3, 0)
                name = p.fontMetrics().elidedText(clip.name, Qt.TextElideMode.ElideRight, int(text_rect.width()))
                p.drawText(text_rect, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft, name)

        source = self.bridge.source(clip.path)
        if source is not None:
            wave_area = body.adjusted(0, 1, 0, -1)
            p.setClipRect(wave_area.intersected(visible))
            self.waveforms.draw(p, source, wave_area, rect.left(), clip.offset_sec,
                                self.view.frames_per_pixel(source.sample_rate, clip.source_tempo(self.project.tempo)),
                                theme.WAVEFORM,
                                split_channels=wave_area.height() >= 44, visible=visible)
            p.setClipRect(rect.intersected(visible).adjusted(-1, -1, 1, 1))
        elif body.height() > 10 and rect.width() > 40:
            error = self.bridge.load_error(clip.path)
            p.setPen(QColor(theme.ACCENT_TEXT))
            p.setFont(theme.ui_font(7.5))
            if error:
                p.fillRect(body, QColor(200, 60, 60, 110))
            p.drawText(body.adjusted(4, 0, -2, 0), Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft,
                       "Missing file" if error else "Loading…")

        outline = QColor(theme.SELECTION_OUTLINE) if selected else base.darker(170)
        p.setPen(QPen(outline, 1))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawRect(rect.adjusted(0.5, 0.5, -0.5, -0.5))
        hover = self._hover_edge
        if hover is not None and hover[0] == clip.id and not ghost:
            x = rect.left() if hover[1] == "left" else rect.right() - 2
            p.fillRect(QRectF(x, rect.top(), 2, rect.height()), QColor(theme.SELECTION_OUTLINE))
        p.restore()

    def _draw_drop_preview(self, p: QPainter) -> None:
        if not self._drop_preview:
            return
        row_index, beat, sources = self._drop_preview
        if row_index is None:
            top = self.layout_model.total_height - self.view.scroll_y
            height = 68
        else:
            row = self.layout_model.rows[row_index]
            top, height = row.top - self.view.scroll_y, row.height
        x = self.view.beat_to_x(beat)
        for path, duration in sources:
            width = duration * self.project.tempo / 60.0 * self.view.px_per_beat
            rect = QRectF(x, top + 1, max(2.0, width), height - 3)
            p.fillRect(rect, QColor(255, 166, 43, 70))
            p.setPen(QPen(QColor(theme.ACCENT), 1, Qt.PenStyle.DashLine))
            p.drawRect(rect)
            p.setPen(QColor(theme.TEXT))
            p.drawText(rect.adjusted(4, 2, -2, 0), Qt.AlignmentFlag.AlignTop, Path(path).stem)
            x += width

    # --- Mouse -----------------------------------------------------------------------

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        pos = event.position()
        mods = event.modifiers()
        if is_pan_modifier(mods):
            self._gesture = PanGesture(self, pos)
            self.setCursor(Qt.CursorShape.ClosedHandCursor)
            return
        additive = bool(mods & (Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.ShiftModifier))
        hit = self.hit_clip(pos)
        if hit and hit[2] != "body":
            track_id, clip, zone = hit
            ref = (track_id, clip.id)
            if zone in ("left", "right"):
                self.selection.set_clips({ref}, track_id=track_id)
                self._gesture = TrimGesture(self, track_id, clip, zone)
                return
            if additive and not (mods & Qt.KeyboardModifier.ShiftModifier):
                self.selection.toggle_clip(ref)
                if ref not in self.selection.clips:
                    return
            elif ref not in self.selection.clips or self.selection.clip_range:
                # (Clicking a clip inside a clip range selects just that clip.)
                keep = self.selection.clips if additive and not self.selection.clip_range else set()
                self.selection.set_clips({ref} | keep, track_id=track_id)
            # Playback will start from the clicked clip, as with Ableton's start marker.
            self.selection.set_insert(clip.start_beat)
            refs = sorted(self.selection.clips)
            self._gesture = MoveClipsGesture(self, pos, ref, refs)
            return

        index = self.row_index_at(pos.y())
        if index is None:
            # Below the tracks: rubber-band select whole clips.
            if not additive:
                self.selection.set_clips(set())
            self._gesture = RubberBandGesture(self, pos, additive)
            return
        # Clip body or empty lane: a click sets the insert marker, a drag selects time.
        gesture = TimeSelectGesture(self, pos, bool(mods & Qt.KeyboardModifier.AltModifier))
        self.selection.set_clips(set(), track_id=self.layout_model.rows[index].track_id)
        self.selection.set_insert(gesture.anchor)
        self._gesture = gesture

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._gesture:
            self._gesture.move(event.position(), event.modifiers())
            self.update()
            return
        self._update_cursor(event.position(), event.modifiers())

    def _update_cursor(self, pos: QPointF, mods) -> None:
        if is_pan_modifier(mods):
            shape = Qt.CursorShape.OpenHandCursor
        elif self.row_index_at(pos.y()) is None:
            shape = Qt.CursorShape.ArrowCursor
        else:
            hit = self.hit_clip(pos)
            zone = hit[2] if hit else "body"
            if zone in ("left", "right"):
                self._set_hover_edge((hit[1].id, zone))
                self.setCursor(trim_cursor(zone))
                return
            shape = Qt.CursorShape.PointingHandCursor if zone == "title" else Qt.CursorShape.IBeamCursor
        self._set_hover_edge(None)
        self.setCursor(shape)

    def _set_hover_edge(self, edge: tuple[str, str] | None) -> None:
        if edge != self._hover_edge:
            self._hover_edge = edge
            self.update()

    def leaveEvent(self, _event) -> None:
        if self._gesture is None:
            self._set_hover_edge(None)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        gesture, self._gesture = self._gesture, None
        if gesture:
            gesture.finish()
        self._update_cursor(event.position(), event.modifiers())
        self.update()

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        hit = self.hit_clip(event.position())
        if hit is None:
            return
        track_id, clip, _ = hit
        # Double-clicking one of several selected clips opens them all.
        if (track_id, clip.id) not in self.selection.clips:
            self.selection.set_clips({(track_id, clip.id)}, track_id=track_id)
        self.clip_view_requested.emit(track_id, clip.id)

    def keyPressEvent(self, event) -> None:
        self._on_modifiers(event.modifiers())
        super().keyPressEvent(event)

    def keyReleaseEvent(self, event) -> None:
        self._on_modifiers(event.modifiers())
        super().keyReleaseEvent(event)

    def _on_modifiers(self, mods) -> None:
        """Show the hand cursor as soon as Ctrl+Alt is held, without moving the mouse."""
        if self._gesture is None and self.underMouse():
            self._update_cursor(QPointF(self.mapFromGlobal(QCursor.pos())), mods)

    def wheelEvent(self, event: QWheelEvent) -> None:
        delta = event.angleDelta()
        mods = event.modifiers()
        if mods & Qt.KeyboardModifier.AltModifier and not mods & Qt.KeyboardModifier.ControlModifier:
            # Alt+wheel resizes the track under the mouse. Qt may report Alt+wheel
            # as horizontal scrolling, so accept either axis.
            index = self.row_index_at(event.position().y())
            if index is not None:
                resize_track_by_wheel(self.editor, self.layout_model.rows[index].track_id, delta.y() or delta.x())
        elif mods & Qt.KeyboardModifier.ControlModifier:
            self.view.zoom_at(event.position().x(), 1.15 ** (delta.y() / 120.0))
        elif mods & Qt.KeyboardModifier.ShiftModifier or delta.x():
            pixels = -(delta.x() or delta.y()) / 120.0 * 80.0
            self.view.set_scroll_beats(self.view.scroll_beats + pixels / self.view.px_per_beat)
        else:
            self.view.set_scroll_y(self.view.scroll_y - delta.y() / 120.0 * 48)
        event.accept()

    def contextMenuEvent(self, event: QContextMenuEvent) -> None:
        pos = QPointF(event.pos())
        menu = QMenu(self)
        hit = self.hit_clip(pos)
        if hit:
            track_id, clip, _ = hit
            if (track_id, clip.id) not in self.selection.clips:
                self.selection.set_clips({(track_id, clip.id)}, track_id=track_id)
            refs = sorted(self.selection.clips)
            split_at = self.view.snap_beat(self.view.x_to_beat(pos.x()))
            menu.addAction("Split Here", lambda: self.editor.split_clips(refs, split_at))
            menu.addAction("Duplicate", lambda: self.selection.set_clips(self.editor.duplicate_clips(refs)))
            menu.addSeparator()
            menu.addAction("Delete", lambda: self.editor.delete_clips(refs))
        else:
            index = self.row_index_at(pos.y())
            menu.addAction("Insert Audio Track", lambda: self.editor.add_audio_track(
                None if index is None else index + 1))
            if index is not None:
                track_id = self.layout_model.rows[index].track_id
                menu.addAction("Delete Track", lambda: self.editor.delete_tracks([track_id]))
        menu.exec(event.globalPos())

    # --- Drag & drop from the browser -------------------------------------------

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:
        mime = event.mimeData()
        if audio_paths(mime) or device_kinds(mime) or mime.hasFormat(PLUGIN_MIME):
            event.acceptProposedAction()
        else:
            event.ignore()

    def dragMoveEvent(self, event) -> None:
        paths = audio_paths(event.mimeData())
        if not paths:
            # Devices drop onto the track under the mouse.
            mime = event.mimeData()
            on_track = device_kinds(mime) and self.row_index_at(event.position().y()) is not None
            if on_track or mime.hasFormat(PLUGIN_MIME):
                event.acceptProposedAction()
            else:
                event.ignore()
            return
        pos = event.position()
        index = self.row_index_at(pos.y())
        beat = max(0.0, self.view.snap_beat(self.view.x_to_beat(pos.x())))
        sources = []
        for path in paths:
            info = self.bridge.file_info(path)
            if info is not None:
                sources.append((path, info.duration))
        self._drop_preview = (index, beat, sources)
        event.acceptProposedAction()
        self.update()

    def dragLeaveEvent(self, _event) -> None:
        self._drop_preview = None
        self.update()

    def dropEvent(self, event: QDropEvent) -> None:
        preview, self._drop_preview = self._drop_preview, None
        self.update()
        kinds = [k for k in device_kinds(event.mimeData()) if k in BUILTIN_DEVICES]
        index = self.row_index_at(event.position().y())
        if kinds and index is not None:
            track_id = self.layout_model.rows[index].track_id
            for kind in kinds:
                self.editor.add_device(track_id, kind)
            self.selection.select_track(track_id)  # show its devices
            event.acceptProposedAction()
            return
        if event.mimeData().hasFormat(PLUGIN_MIME):
            self.status_message.emit("VST3/CLAP plugin hosting is not available yet.")
            event.acceptProposedAction()
            return
        if not preview or not preview[2]:
            return
        index, beat, sources = preview
        track_id = None if index is None else self.layout_model.rows[index].track_id
        refs = self.editor.add_clips(track_id, beat, sources, track_index=len(self.project.tracks))
        if refs:
            self.selection.set_clips(refs, track_id=refs[0][0])
        event.acceptProposedAction()
        self.activateWindow()
        self.setFocus()
