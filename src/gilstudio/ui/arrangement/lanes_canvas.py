"""The track lanes: clips, grid, loop, playhead, and all clip mouse editing;
and the tracks' automation, over their clips and in lanes below them
(see automation_lanes.py).

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
from ...model.editor import BUILTIN_DEVICES, ProjectEditor, is_instrument
from ...model.project import (
    DEFAULT_TRACK_HEIGHT,
    MAX_TRACK_HEIGHT,
    MIN_TRACK_HEIGHT,
    PLUGIN_KIND,
    AnyClip,
    MidiClip,
    PluginRef,
)
from ..browser.browser_models import PLUGIN_MIME, device_kinds, plugin_refs
from . import automation_lanes
from .automation_lanes import EnvelopeArea
from .grid import draw_grid, draw_loop_region
from .interactions import (
    ClipGesture,
    MoveRangeGesture,
    PanGesture,
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


def dropped_devices(mime) -> list[tuple[str, PluginRef | None]]:
    """Devices dragged from the browser, as (kind, plug-in): built-in ones, then plug-ins."""
    devices: list[tuple[str, PluginRef | None]] = [(k, None) for k in device_kinds(mime) if k in BUILTIN_DEVICES]
    return devices + [(PLUGIN_KIND, ref) for ref in plugin_refs(mime)]


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
        self._clip_anchor: tuple[str, str] | None = None  # the last clip clicked without Shift
        self._drop_preview: tuple[int | None, float, list[tuple[str, float]]] | None = None
        self._hover_edge: tuple[str, str] | None = None  # (clip id, "left"/"right") under the mouse
        self._hover_point: tuple | None = None  # (lane, index) of the breakpoint under the mouse
        self.setAcceptDrops(True)
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.ClickFocus)
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent)

        for signal in (view.changed, view.vscroll_changed, view.grid_changed, selection.changed,
                       selection.insert_changed, self.project.settings_changed, bridge.source_ready,
                       bridge.source_failed, self.project.clips_changed, self.project.track_changed,
                       self.project.automation_changed, self.project.automation_view_changed,
                       self.project.devices_changed, bridge.automation_state_changed):
            signal.connect(lambda *_args: self.update())

    # --- Geometry ------------------------------------------------------------------

    def row_index_at(self, y: float, clamp: bool = False) -> int | None:
        index = self.layout_model.row_index_at(y + self.view.scroll_y)
        if index is None and clamp and self.layout_model.rows:
            return 0 if y + self.view.scroll_y < 0 else len(self.layout_model.rows) - 1
        return index

    def _clip_rect(self, clip: AnyClip, row_top: float, row_height: int) -> QRectF:
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
        y = pos.y() + self.view.scroll_y - row.top
        return y < row.main_height and (row.main_height < MIN_TITLE_ROW or 0 <= y < TITLE_HEIGHT + 1)

    def envelope_areas(self) -> list[EnvelopeArea]:
        """The automation lanes showing, top to bottom: in tracks' own lanes (below
        the clips' title band) and below them."""
        areas = []
        width, scroll = float(self.width()), self.view.scroll_y
        for row in self.layout_model.rows:
            if not row.automation or row.bottom - scroll < 0 or row.top - scroll > self.height():
                continue
            top = row.top - scroll
            key = self.project.track(row.track_id).automation_view.key
            if key:
                areas.append(EnvelopeArea(row.track_id, key, -1,
                                          QRectF(0, top + TITLE_HEIGHT + 1, width, row.main_height - TITLE_HEIGHT - 2)))
            for lane in row.lanes:
                areas.append(EnvelopeArea(row.track_id, lane.key, lane.index,
                                          QRectF(0, lane.top - scroll, width, lane.height - 1)))
        return areas

    def envelope_area_at(self, pos: QPointF) -> EnvelopeArea | None:
        return automation_lanes.area_at(self.envelope_areas(), pos)

    def hit_clip(self, pos: QPointF) -> tuple[str, AnyClip, str] | None:
        """(track id, clip, zone) under `pos`; zone is left/right (trim handles, at
        the ends of the title bar), title (select & move) or body (time selection
        / insert marker)."""
        index = self.row_index_at(pos.y())
        if index is None:
            return None
        row = self.layout_model.rows[index]
        top = row.top - self.view.scroll_y
        if pos.y() >= top + row.main_height:
            return None  # in an automation lane below the track
        for clip in reversed(self.project.track(row.track_id).clips):
            rect = self._clip_rect(clip, top, row.main_height)
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
        for _, row in rows:
            y = row.top - view.scroll_y
            color = theme.LANE_SELECTED if row.track_id == self.selection.track_id else theme.LANE
            p.fillRect(QRectF(visible.left(), y, visible.width(), row.height), QColor(color))
        # The grid goes all the way down: below the tracks too, where selecting works on it as well.
        draw_grid(p, view, visible.left(), visible.right(), visible.top(), visible.bottom() + 1)
        draw_loop_region(p, view, visible.left(), visible.right(), 0.0, float(self.height()))

        gesture = self._gesture
        hidden = gesture.hidden_ids() if gesture else set()
        for _, row in rows:
            track = self.project.track(row.track_id)
            y = row.top - view.scroll_y
            for clip in track.clips:
                if clip.id in hidden:
                    continue
                rect = self._clip_rect(clip, y, row.main_height)
                if rect.left() > visible.right():
                    break
                if rect.right() < visible.left():
                    continue
                self._draw_clip(p, track.color, clip, rect, visible, False)  # the selected area is tinted
            for lane in row.lanes:  # automation lanes below the track
                p.fillRect(QRectF(visible.left(), lane.top - view.scroll_y - 1, visible.width(), 1),
                           QColor(theme.GRID_BAR))
            p.fillRect(QRectF(visible.left(), y + row.height - 1, visible.width(), 1), QColor(theme.BORDER))

        if gesture:
            for row_index, color, clip in gesture.kept():
                row = self.layout_model.rows[row_index]
                rect = self._clip_rect(clip, row.top - view.scroll_y, row.main_height)
                self._draw_clip(p, color, clip, rect, visible, False)
            for row_index, color, clip in gesture.ghosts():
                row = self.layout_model.rows[row_index]
                rect = self._clip_rect(clip, row.top - view.scroll_y, row.main_height)
                self._draw_clip(p, color, clip, rect, visible, True, ghost=True)
        areas = self.envelope_areas()
        for area in areas:
            automation_lanes.draw_area(p, self, area, visible, self._hover_point, shade=area.lane < 0)
        self._draw_drop_preview(p)

        if not self.project.tracks and not self._drop_preview:
            p.setPen(QColor(theme.TEXT_DIM))
            p.setFont(theme.ui_font(10))
            p.drawText(QRectF(self.rect()), Qt.AlignmentFlag.AlignCenter,
                       "Drag audio files here from the browser\nor press Ctrl+T to create an audio track,"
                       " Ctrl+Shift+T for a MIDI track")

        time_range = (gesture.time_range() if gesture else None) or self.selection.time_range
        if time_range is not None and self.selection.lanes and not (gesture and gesture.time_range()):
            automation_lanes.draw_range(p, self, areas, SELECTION_TINT)  # on the automation lanes it covers
        elif time_range is not None:
            start, end, track_ids = time_range
            x0, x1 = view.beat_to_x(start), view.beat_to_x(end)
            for track_id in track_ids:
                row = self.layout_model.row_for(track_id)
                if row is None:
                    continue
                area = QRectF(x0, row.top - view.scroll_y, x1 - x0, row.main_height - 1)
                if not self.selection.clip_range and row.main_height >= MIN_TITLE_ROW:
                    # A lane range leaves the clips' title band alone.
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
        automation_lanes.draw_readout(p, self, gesture)


    def _draw_clip(self, p: QPainter, track_color: str, clip: AnyClip, rect: QRectF, visible: QRectF,
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
        # The grid shows through the body, faintly (under the notes and the waveform);
        # the title bar, where the clip is grabbed, stays solid.
        draw_grid(p, self.view, max(rect.left() + 1, visible.left()), min(rect.right() - 1, visible.right()),
                  body.top(), body.bottom(), over_clip=True)
        title = QRectF(rect.left(), rect.top(), rect.width(), title_h)
        if title_h:
            p.fillRect(title, base.lighter(115) if selected else base)
        if title_h and rect.width() > 16:
            p.setPen(QColor(theme.ACCENT_TEXT))
            p.setFont(theme.ui_font(7.5))
            text_rect = title.adjusted(4, 0, -3, 0)
            name = p.fontMetrics().elidedText(clip.name, Qt.TextElideMode.ElideRight, int(text_rect.width()))
            p.drawText(text_rect, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft, name)

        source = None if isinstance(clip, MidiClip) else self.bridge.source(clip.path)
        if isinstance(clip, MidiClip):
            self._draw_notes(p, clip, body.adjusted(0, 2, 0, -2), visible)
        elif source is not None:
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

    def _draw_notes(self, p: QPainter, clip: MidiClip, area: QRectF, visible: QRectF) -> None:
        """The notes a MIDI clip plays, fitted to the clip's height (as in Ableton)."""
        played = clip.played_notes()
        if not played or area.height() < 3:
            return
        low = min(note.pitch for *_, note in played)
        high = max(note.pitch for *_, note in played)
        row = min(area.height() / (high - low + 1), max(2.0, area.height() / 12))
        top = area.top() + (area.height() - row * (high - low + 1)) / 2
        gap = 1.0 if row > 3 else 0.0
        for start, end, note in played:
            x0, x1 = self.view.beat_to_x(start), self.view.beat_to_x(end)
            if x1 >= visible.left() and x0 <= visible.right():
                p.fillRect(QRectF(x0, top + (high - note.pitch) * row, max(1.0, x1 - x0 - gap), max(1.0, row - gap)),
                           theme.WAVEFORM)

    def _draw_drop_preview(self, p: QPainter) -> None:
        if not self._drop_preview:
            return
        row_index, beat, sources = self._drop_preview
        if row_index is None:
            top = self.layout_model.total_height - self.view.scroll_y
            height = DEFAULT_TRACK_HEIGHT
        else:
            row = self.layout_model.rows[row_index]
            top, height = row.top - self.view.scroll_y, row.main_height
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
        area = self.envelope_area_at(pos)
        if area is not None:
            self._gesture = automation_lanes.press(self, area, pos, mods)
            self.update()
            return
        hit = self.hit_clip(pos)
        if hit and hit[2] in ("left", "right"):
            track_id, clip, zone = hit
            self.selection.select_clips(self.editor, [(track_id, clip.id)])
            self._gesture = TrimGesture(self, track_id, clip, zone)
            return
        if self._in_clip_range(pos) and not mods & Qt.KeyboardModifier.ShiftModifier:
            # Dragging the selected stretch moves it (Ctrl: copies); a click selects as usual.
            self._gesture = MoveRangeGesture(self, pos, lambda: self._select_on_click(pos, mods, hit))
            return
        self._click(pos, mods, hit)

    def _select_on_click(self, pos: QPointF, mods, hit) -> None:
        """A click in a clip range without dragging: select as a click anywhere else would."""
        self._click(pos, mods, hit)
        self._gesture = None

    def _in_clip_range(self, pos: QPointF) -> bool:
        """Whether `pos` is in the clip band inside the selected clip range."""
        time_range = self.selection.time_range
        index = self.row_index_at(pos.y())
        if not self.selection.clip_range or index is None or not self.in_clip_band(pos):
            return False
        start, end, track_ids = time_range
        return self.layout_model.rows[index].track_id in track_ids and start <= self.view.x_to_beat(pos.x()) <= end

    def _click(self, pos: QPointF, mods, hit) -> None:
        """A press that is not a trim or a drag of the time selection."""
        if hit and hit[2] != "body":
            # Selecting a clip selects the area it covers on the grid; Shift-clicking
            # another selects the area that fully contains both (and the tracks between).
            track_id, clip, _ = hit
            ref = (track_id, clip.id)
            if mods & Qt.KeyboardModifier.ShiftModifier and self._clip_anchor is not None:
                self.selection.select_clips(self.editor, [self._clip_anchor, ref], track_id=track_id)
            else:
                self._clip_anchor = ref
                self.selection.select_clips(self.editor, [ref])
            # Playback will start from the selection, as with Ableton's start marker.
            self.selection.set_insert(self.selection.time_range[0])
            self._gesture = MoveRangeGesture(self, pos)  # dragging moves it (Ctrl: copies)
            return

        if not self.layout_model.rows:
            return
        # Clip body, empty lane or below the tracks: a click sets the insert marker,
        # a drag selects time on the grid (from below the tracks, starting at the last one).
        index = self.row_index_at(pos.y())
        gesture = TimeSelectGesture(self, pos, bool(mods & Qt.KeyboardModifier.AltModifier))
        self.selection.clear(track_id=None if index is None else self.layout_model.rows[index].track_id)
        self.selection.set_insert(gesture.anchor)
        self._gesture = gesture

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._gesture:
            self._gesture.move(event.position(), event.modifiers())
            self.update()
            return
        self._update_cursor(event.position(), event.modifiers())

    def _update_cursor(self, pos: QPointF, mods) -> None:
        area = None if is_pan_modifier(mods) else self.envelope_area_at(pos)
        point, shape = automation_lanes.hover(self, area, pos, mods)
        if point != self._hover_point:
            self._hover_point = point
            self.update()
        if area is not None:
            self._set_hover_edge(None)
            self.setCursor(shape)
            return
        if is_pan_modifier(mods):
            shape = Qt.CursorShape.OpenHandCursor
        elif not self.layout_model.rows:
            shape = Qt.CursorShape.ArrowCursor
        else:
            hit = self.hit_clip(pos)
            zone = hit[2] if hit else "body"
            if zone in ("left", "right"):
                self._set_hover_edge((hit[1].id, zone))
                self.setCursor(trim_cursor(zone))
                return
            grab = zone == "title" or self._in_clip_range(pos)
            shape = Qt.CursorShape.PointingHandCursor if grab else Qt.CursorShape.IBeamCursor
        self._set_hover_edge(None)
        self.setCursor(shape)

    def _set_hover_edge(self, edge: tuple[str, str] | None) -> None:
        if edge != self._hover_edge:
            self._hover_edge = edge
            self.update()

    def leaveEvent(self, _event) -> None:
        if self._gesture is None:
            self._set_hover_edge(None)
            if self._hover_point is not None:
                self._hover_point = None
                self.update()

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        gesture, self._gesture = self._gesture, None
        if gesture:
            gesture.finish()
        self._update_cursor(event.position(), event.modifiers())
        self.update()

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        area = self.envelope_area_at(event.position())
        if area is not None:
            automation_lanes.double_click(self, area, event.position())
            return
        hit = self.hit_clip(event.position())
        if hit is None:
            return
        track_id, clip, _ = hit
        # Double-clicking one of several selected clips opens them all.
        if (track_id, clip.id) not in self.selection.clips:
            self.selection.select_clips(self.editor, [(track_id, clip.id)])
        self.clip_view_requested.emit(track_id, clip.id)

    def insert_midi_clip(self, track_id: str, x: float) -> tuple[str, str] | None:
        """A new MIDI clip on a MIDI track, selected and opened in the piano roll: over
        the time selection (on each of its MIDI tracks) if `x` is inside it, else
        where `x` is (see ProjectEditor.midi_clip_span). The one on `track_id`."""
        if not self.project.track(track_id).is_midi:
            return None
        beat = self.view.x_to_beat(x)
        time_range = self.selection.time_range
        if time_range is not None and track_id in time_range[2] and time_range[0] <= beat <= time_range[1]:
            refs = self.editor.add_midi_clips_over(*time_range)
            start = time_range[0]
        else:
            step = self.view.grid_step() if self.view.snap else 0.0
            start, length = self.editor.midi_clip_span(track_id, beat, step)
            refs = [ref for ref in [self.editor.add_midi_clip(track_id, start, length)] if ref is not None]
        ref = next((r for r in refs if r[0] == track_id), None)
        if ref is None:
            return None
        self.selection.select_clips(self.editor, refs, track_id=track_id)
        self.selection.set_insert(start)
        self.clip_view_requested.emit(*ref)
        return ref

    def delete_area(self) -> None:
        """Cut the clips out of the selected area; the (now empty) area stays selected."""
        if self.selection.clip_range:
            start, end, track_ids = self.selection.time_range
            self.editor.delete_range(start, end, list(track_ids))
            self.selection.set_time_range(start, end, track_ids, clips=set())

    def duplicate_area(self) -> None:
        """Copy the selected area to right after it, and select the copy."""
        if self.selection.clip_range:
            start, end, track_ids = self.selection.time_range
            length = end - start
            self.editor.duplicate_range(start, end, list(track_ids))
            self.selection.set_time_range(end, end + length, track_ids,
                                          clips=self.editor.clips_in_range(end, end + length, track_ids))
            self.selection.set_insert(end)

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
        area = self.envelope_area_at(pos)
        if area is not None:
            automation_lanes.add_menu_actions(self, area, pos, menu)
            menu.exec(event.globalPos())
            return
        hit = self.hit_clip(pos)
        if hit:
            track_id, clip, _ = hit
            if (track_id, clip.id) not in self.selection.clips:
                self.selection.select_clips(self.editor, [(track_id, clip.id)])
            refs = sorted(self.selection.clips)
            split_at = self.view.snap_beat(self.view.x_to_beat(pos.x()))
            menu.addAction("Split Here", lambda: self.editor.split_clips(refs, split_at))
            menu.addAction("Duplicate", self.duplicate_area)
            menu.addSeparator()
            menu.addAction("Delete", self.delete_area)
        else:
            index = self.row_index_at(pos.y())
            at = None if index is None else index + 1
            if index is not None and self.project.track(self.layout_model.rows[index].track_id).is_midi:
                track_id = self.layout_model.rows[index].track_id
                menu.addAction("Insert MIDI Clip", lambda: self.insert_midi_clip(track_id, pos.x()))
                menu.addSeparator()
            menu.addAction("Insert Audio Track", lambda: self.editor.add_audio_track(at))
            menu.addAction("Insert MIDI Track", lambda: self.editor.add_midi_track(at))
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
            # Devices drop onto the track under the mouse; an instrument below the
            # tracks makes a new MIDI track.
            devices = dropped_devices(event.mimeData())
            on_track = devices and self.row_index_at(event.position().y()) is not None
            if on_track or any(is_instrument(*d) for d in devices):
                event.acceptProposedAction()
            else:
                event.ignore()
            return
        pos = event.position()
        index = self.row_index_at(pos.y())
        if index is not None and self.project.track(self.layout_model.rows[index].track_id).is_midi:
            index = None  # audio can't go on a MIDI track: it gets a new track
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
        devices = dropped_devices(event.mimeData())
        index = self.row_index_at(event.position().y())
        if devices:
            if index is not None:
                track_id = self.layout_model.rows[index].track_id
                refused = [d for d in devices if self.editor.add_device(track_id, d[0], plugin=d[1]) is None]
                if refused:
                    self.status_message.emit("Instruments go on MIDI tracks. Drop one below the tracks to make one.")
            else:
                instrument = next((d for d in devices if is_instrument(*d)), None)
                if instrument is None:
                    return
                kind, plugin = instrument
                track_id = self.editor.add_midi_track(instrument=kind if plugin is None else None, plugin=plugin).id
                for kind, plugin in devices:
                    if not is_instrument(kind, plugin):
                        self.editor.add_device(track_id, kind, plugin=plugin)
            self.selection.select_track(track_id)  # show its devices
            event.acceptProposedAction()
            return
        if not preview or not preview[2]:
            return
        index, beat, sources = preview
        track_id = None if index is None else self.layout_model.rows[index].track_id
        refs = self.editor.add_clips(track_id, beat, sources, track_index=len(self.project.tracks))
        if refs:
            self.selection.select_clips(self.editor, refs)
        event.acceptProposedAction()
        self.activateWindow()
        self.setFocus()
