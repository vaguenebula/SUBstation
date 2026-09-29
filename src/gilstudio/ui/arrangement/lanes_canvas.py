"""The track lanes: clips, grid, loop, playhead, and all clip mouse editing.

Custom-painted for speed: each paint only touches what intersects the dirty
rectangle, and playhead motion repaints just two thin strips.
"""

from __future__ import annotations

from pathlib import Path

from PySide6.QtCore import QPointF, QRect, QRectF, Qt, Signal
from PySide6.QtGui import QColor, QContextMenuEvent, QDragEnterEvent, QDropEvent, QMouseEvent, QPainter, QPen, QWheelEvent
from PySide6.QtWidgets import QMenu, QWidget

from ... import theme
from ...audio.engine_bridge import EngineBridge, is_audio_file
from ...model.editor import ProjectEditor
from ...model.project import Clip
from ..browser.browser_models import PLUGIN_MIME
from .grid import draw_grid, draw_loop_region
from .interactions import ClipGesture, MoveClipsGesture, RubberBandGesture, TrimGesture
from .view_state import Selection, TrackLayout, ViewState
from .waveform_cache import WaveformCache

EDGE_GRAB = 5
TITLE_HEIGHT = 13


def audio_paths(mime) -> list[str]:
    if not mime.hasUrls():
        return []
    return [u.toLocalFile() for u in mime.urls() if u.isLocalFile() and is_audio_file(u.toLocalFile())]


class LanesCanvas(QWidget):
    status_message = Signal(str)

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

    def hit_clip(self, pos: QPointF) -> tuple[str, Clip, str] | None:
        """(track id, clip, zone) under `pos`; zone is left/right/body."""
        index = self.row_index_at(pos.y())
        if index is None:
            return None
        row = self.layout_model.rows[index]
        top = row.top - self.view.scroll_y
        for clip in reversed(self.project.track(row.track_id).clips):
            rect = self._clip_rect(clip, top, row.height)
            if rect.left() - 1 <= pos.x() <= rect.right() + 1:
                grab = min(EDGE_GRAB, rect.width() / 3)
                if pos.x() <= rect.left() + grab:
                    zone = "left"
                elif pos.x() >= rect.right() - grab:
                    zone = "right"
                else:
                    zone = "body"
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
                self._draw_clip(p, track.color, clip, rect, visible, (track.id, clip.id) in self.selection.clips)
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

        # Insert marker on the selected track (Ableton's blinking cursor, minus the blink)
        row = self.layout_model.row_for(self.selection.track_id) if self.selection.track_id else None
        if row is not None and not self.selection.clips:
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
        title_h = TITLE_HEIGHT if rect.height() >= 30 else 0
        body = rect.adjusted(0, title_h, 0, 0)
        body_color = QColor(base)
        body_color.setHsvF(base.hsvHueF(), base.hsvSaturationF() * (0.45 if selected else 0.6),
                           min(1.0, base.valueF() * (0.98 if selected else 0.78)))
        if ghost:
            body_color.setAlphaF(0.75)
        p.save()
        p.setClipRect(rect.intersected(visible).adjusted(-1, -1, 1, 1))
        p.fillRect(body, body_color)
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
                                self.view.frames_per_pixel(source.sample_rate), theme.WAVEFORM,
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
        additive = bool(mods & (Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.ShiftModifier))
        hit = self.hit_clip(pos)
        if hit:
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
            elif ref not in self.selection.clips:
                self.selection.set_clips({ref} | (self.selection.clips if additive else set()), track_id=track_id)
            refs = sorted(self.selection.clips)
            self._gesture = MoveClipsGesture(self, pos, ref, refs)
            return

        index = self.row_index_at(pos.y())
        bypass = bool(mods & Qt.KeyboardModifier.AltModifier)
        if index is not None:
            if not additive:
                self.selection.set_clips(set(), track_id=self.layout_model.rows[index].track_id)
            self.selection.set_insert(self.view.snap_beat(self.view.x_to_beat(pos.x()), bypass))
        elif not additive:
            self.selection.set_clips(set())
        self._gesture = RubberBandGesture(self, pos, additive)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._gesture:
            self._gesture.move(event.position(), event.modifiers())
            self.update()
            return
        hit = self.hit_clip(event.position())
        if hit and hit[2] in ("left", "right"):
            self.setCursor(Qt.CursorShape.SizeHorCursor)
        else:
            self.setCursor(Qt.CursorShape.ArrowCursor)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        gesture, self._gesture = self._gesture, None
        if gesture:
            gesture.finish()
        self.update()

    def wheelEvent(self, event: QWheelEvent) -> None:
        delta = event.angleDelta()
        mods = event.modifiers()
        if mods & Qt.KeyboardModifier.ControlModifier:
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
        if audio_paths(event.mimeData()) or event.mimeData().hasFormat(PLUGIN_MIME):
            event.acceptProposedAction()
        else:
            event.ignore()

    def dragMoveEvent(self, event) -> None:
        paths = audio_paths(event.mimeData())
        if not paths:
            if event.mimeData().hasFormat(PLUGIN_MIME):
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
