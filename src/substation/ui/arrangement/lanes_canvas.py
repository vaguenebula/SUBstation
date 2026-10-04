"""The track lanes: clips, grid, loop, playhead, and all clip mouse editing;
and the tracks' automation, over their clips and in lanes below them
(see automation_lanes.py). A group's lane shows a summary of the clips of the
tracks in it (folded or not); the tracks in a folded group have no lane.

Custom-painted for speed: each paint only touches what intersects the dirty
rectangle, and playhead motion repaints just two thin strips.
"""

from __future__ import annotations

import time
from pathlib import Path

import numpy as np
from PySide6.QtCore import QPointF, QRect, QRectF, Qt, Signal
from PySide6.QtGui import (
    QColor,
    QContextMenuEvent,
    QCursor,
    QDragEnterEvent,
    QDropEvent,
    QKeySequence,
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
from ...model.devices import BUILTIN_DEVICES, device_is_instrument, is_instrument
from ...model.editor import ClipboardContent, CopiedAutomation, ProjectEditor
from ...model.project import (
    DEFAULT_TRACK_HEIGHT,
    MAX_TRACK_HEIGHT,
    MIN_TRACK_HEIGHT,
    PLUGIN_KIND,
    AnyClip,
    MidiClip,
    PluginRef,
)
from ..browser.browser_models import (
    PLUGIN_MIME,
    device_kinds,
    plugin_refs,
    preset_paths,
    read_presets,
)
from . import automation_lanes
from .automation_lanes import EnvelopeArea, Hover
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
MIN_TITLE_ROW = 30  # clips in shorter rows (folded tracks) have a thin title bar instead
SHORT_TITLE_HEIGHT = 9  # that thin bar: grab it to move the clip; below it, select time as on any lane


def clip_title_height(clip_height: float) -> int:
    """The title bar of a clip this high: where it is grabbed (the rest selects time)."""
    return TITLE_HEIGHT if clip_height >= MIN_TITLE_ROW else SHORT_TITLE_HEIGHT
HEIGHT_STEP = 12  # pixels per wheel notch when Alt+wheel resizes a track
WHEEL_GESTURE = 0.4  # s: wheel events closer together than this resize (or fold) the same track
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


class _WheelResize:
    """Alt+wheel over a track (its header or its lane), as one gesture: down
    shrinks it, and once it is as small as it gets, folds it (a group: hides its
    tracks too); up unfolds a folded track, then makes it taller. A turn of the
    wheel acts on the track it started on: the tracks below move up under the
    mouse as it shrinks or folds."""

    def __init__(self):
        self._last: tuple[str, float] | None = None  # the track turned last, and when

    def __call__(self, editor: ProjectEditor, track_id: str, delta: int) -> None:
        project = editor.project
        now = time.monotonic()
        if self._last is not None and now - self._last[1] < WHEEL_GESTURE and project.has_track(self._last[0]):
            track_id = self._last[0]
        self._last = (track_id, now)
        if not delta or not project.has_track(track_id):
            return
        track = project.track(track_id)
        if track.folded:
            if delta > 0:
                editor.set_folded(track_id, False)  # (at the height it had)
        elif delta < 0 and track.height <= MIN_TRACK_HEIGHT:
            editor.set_folded(track_id, True)
        else:
            height = track.height + round(delta / 120.0 * HEIGHT_STEP)
            editor.set_track_height(track_id, max(MIN_TRACK_HEIGHT, min(MAX_TRACK_HEIGHT, height)))


resize_track_by_wheel = _WheelResize()


def wheel_action(editor: ProjectEditor, track_id: str, event: QWheelEvent) -> bool:
    """Alt+wheel resizes a track, folding and unfolding it at its smallest (Qt
    may report it as horizontal scrolling, so either axis counts). False for
    other wheel events."""
    mods = event.modifiers()
    if not mods & Qt.KeyboardModifier.AltModifier or mods & Qt.KeyboardModifier.ControlModifier:
        return False
    delta = event.angleDelta()
    resize_track_by_wheel(editor, track_id, delta.y() or delta.x())
    event.accept()
    return True


DEVICE_MOVE_MIME = "application/x-substation-device-move"  # track id, then device ids, a line each


def moved_devices(mime) -> tuple[str, list[str]] | None:
    """Devices dragged from a track's chain (the device panel): (track id, device ids)."""
    if not mime.hasFormat(DEVICE_MOVE_MIME):
        return None
    track_id, *device_ids = bytes(mime.data(DEVICE_MOVE_MIME)).decode().split("\n")
    return track_id, device_ids


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
        self._playhead: float | None = None
        self._gesture: ClipGesture | None = None
        self._clip_anchor: tuple[str, str] | None = None  # the last clip clicked without Shift
        self._drop_preview: tuple[int | None, float, list[tuple[str, float]]] | None = None
        self._hover_edge: tuple[str, str] | None = None  # (clip id, "left"/"right") under the mouse
        self._hover_point: Hover | None = None  # the breakpoint (or place on a line) under the mouse
        # Clip content, or automation, copied or cut (Ctrl+C / Ctrl+X): the last copied is what Ctrl+V pastes.
        self.clipboard: ClipboardContent | CopiedAutomation | None = None
        self.setAcceptDrops(True)
        self.setMouseTracking(True)
        self.setFocusPolicy(Qt.FocusPolicy.ClickFocus)
        self.setAttribute(Qt.WidgetAttribute.WA_OpaquePaintEvent)

        for signal in (view.changed, view.vscroll_changed, view.grid_changed, selection.changed,
                       selection.insert_changed, self.project.settings_changed, bridge.source_ready,
                       bridge.source_failed, self.project.clips_changed, self.project.track_changed,
                       self.project.automation_changed, self.project.automation_view_changed,
                       self.project.devices_changed, self.project.freeze_changed, bridge.automation_state_changed,
                       bridge.recording_updated):
            signal.connect(lambda *_args: self.update())
        # A parameter set by hand: lanes without an envelope draw its value.
        for signal in (self.project.device_param_changed, self.project.device_state_changed,
                       bridge.plugin_param_edited, bridge.plugin_params_changed, bridge.plugin_params_rebuilt,
                       bridge.devices_loaded):
            signal.connect(lambda track_id, *_args: self._update_if_shown(track_id))

    def _update_if_shown(self, track_id: str) -> None:
        if self.project.has_track(track_id) and self.project.track(track_id).automation_view.shown:
            self.update()

    # --- Geometry ------------------------------------------------------------------

    def row_index_at(self, y: float, clamp: bool = False) -> int | None:
        index = self.layout_model.row_index_at(y + self.view.scroll_y)
        if index is None and clamp and self.layout_model.rows:
            return 0 if y + self.view.scroll_y < 0 else self.layout_model.last_shown_index()
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
                if pos.y() >= rect.top() + clip_title_height(rect.height()):
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

    def set_playhead(self, beat: float | None) -> None:
        """None hides it (playback stopped)."""
        for b in (self._playhead, beat):
            if b is not None:
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
            color = theme.LANE_SELECTED if row.track_id in self.selection.track_ids else theme.LANE
            p.fillRect(QRectF(visible.left(), y, visible.width(), row.height), QColor(color))
        # The grid goes all the way down: below the tracks too, where selecting works on it as well.
        draw_grid(p, view, visible.left(), visible.right(), visible.top(), visible.bottom() + 1)
        draw_loop_region(p, view, visible.left(), visible.right(), 0.0, float(self.height()))

        gesture = self._gesture
        hidden = gesture.hidden_ids() if gesture else set()
        for _, row in rows:
            track = self.project.track(row.track_id)
            y = row.top - view.scroll_y
            if track.is_group:
                self._draw_group_summary(p, track.id, y, row.main_height, visible)
            for clip in track.clips:
                if clip.id in hidden:
                    continue
                rect = self._clip_rect(clip, y, row.main_height)
                if rect.left() > visible.right():
                    break
                if rect.right() < visible.left():
                    continue
                self._draw_clip(p, track.color, clip, rect, visible, False)  # the selected area is tinted
            if self.project.is_frozen(track.id):  # its clips play as frozen: tinted, as in Ableton
                p.fillRect(QRectF(visible.left(), y, visible.width(), row.main_height - 1), theme.FROZEN_TINT)
            live = self.bridge.live_takes.get(track.id)
            if live is not None and live.started:
                self._draw_live_take(p, track.color, live, y, row.main_height, visible)
            for lane in row.lanes:  # automation lanes below the track
                p.fillRect(QRectF(visible.left(), lane.top - view.scroll_y - 1, visible.width(), 1),
                           QColor(theme.GRID_BAR))
            p.fillRect(QRectF(visible.left(), y + row.height - 1, visible.width(), 1), QColor(theme.BORDER))

        if gesture:
            for row_index, color, clip in gesture.kept():
                row = self.layout_model.rows[row_index]
                if row.hidden:
                    continue
                rect = self._clip_rect(clip, row.top - view.scroll_y, row.main_height)
                self._draw_clip(p, color, clip, rect, visible, False)
            for row_index, color, clip in gesture.ghosts():
                row = self.layout_model.rows[row_index]
                if row.hidden:
                    continue
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
                if row is None or row.hidden:
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

        if self._playhead is not None:
            x = round(view.beat_to_x(self._playhead))
            if visible.left() - 2 <= x <= visible.right() + 2:
                p.fillRect(QRectF(x, 0, 1, self.height()), QColor(theme.PLAYHEAD))
        automation_lanes.draw_readout(p, self, gesture)


    def _draw_clip(self, p: QPainter, track_color: str, clip: AnyClip, rect: QRectF, visible: QRectF,
                   selected: bool, ghost: bool = False) -> None:
        base = QColor(track_color)
        title_h = clip_title_height(rect.height())
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
        if title_h >= TITLE_HEIGHT and rect.width() > 16:  # (no name in a thin bar)
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

    def _draw_group_summary(self, p: QPainter, group_id: str, row_top: float, row_height: int,
                            visible: QRectF) -> None:
        """What is in a group, at a glance: the clips of its tracks as bars in their
        colours, overlapping, as in Ableton's group lanes."""
        area = QRectF(visible.left(), row_top + 2, visible.width(), row_height - 5)
        if area.height() < 3:
            return
        tempo = self.project.tempo
        p.save()
        p.setClipRect(area)
        for track in self.project.descendants(group_id):
            color = QColor(track.color)
            color.setAlphaF(0.85)
            fill = QColor(color)
            fill.setAlphaF(0.15)  # light enough that the grid (and overlapping clips) show through
            for clip in track.clips:
                x0, x1 = self.view.beat_to_x(clip.start_beat), self.view.beat_to_x(clip.end_beat(tempo))
                if x1 < visible.left():
                    continue
                if x0 > visible.right():
                    break
                rect = QRectF(x0, area.top(), max(1.0, x1 - x0), area.height())
                p.fillRect(rect, fill)
                p.fillRect(QRectF(x0, area.top(), rect.width(), min(4.0, area.height())), color)
        p.restore()

    def _draw_live_take(self, p: QPainter, track_color: str, take, row_top: float, row_height: int,
                        visible: QRectF) -> None:
        """A take while it records: a clip that grows, its waveform drawn from the
        peaks the engine sends (the file isn't read until the take is done), or
        a MIDI take's notes so far."""
        rate = self.bridge.engine.sample_rate
        tempo = self.project.tempo
        start = take.start_sample / rate * tempo / 60.0
        end = (take.start_sample + take.frames) / rate * tempo / 60.0
        x0, x1 = self.view.beat_to_x(max(0.0, start)), self.view.beat_to_x(end)
        rect = QRectF(x0, row_top + 1, max(1.0, x1 - x0), row_height - 3)
        if rect.right() < visible.left() or rect.left() > visible.right():
            return
        p.save()
        p.setClipRect(rect.intersected(visible).adjusted(-1, -1, 1, 1))
        title_h = TITLE_HEIGHT if rect.height() >= MIN_TITLE_ROW else 0
        p.fillRect(rect, QColor(track_color).darker(160))
        if title_h:
            p.fillRect(QRectF(rect.left(), rect.top(), rect.width(), title_h), QColor(theme.RECORD_ON))
        body = rect.adjusted(0, title_h + 1, 0, -1)
        peaks = take.peaks
        if take.midi:
            self._draw_live_notes(p, take, end, body, visible)
        elif len(peaks) and body.height() > 2:
            # One column per pixel: the extremes of the peaks it covers.
            fpp = self.view.frames_per_pixel(rate)
            take_x = self.view.beat_to_x(start)
            columns = np.arange(int(max(rect.left(), visible.left())), int(min(rect.right(), visible.right())) + 1)
            index = ((columns - take_x) * fpp / take.PEAK_FRAMES).astype(np.int64)
            keep = (index >= 0) & (index < len(peaks))
            columns, index = columns[keep], index[keep]
            starts, at = np.unique(index, return_index=True)
            if len(starts):
                # The last column ends where the next pixel would start, not at the take's end.
                end = int((columns[-1] + 1 - take_x) * fpp / take.PEAK_FRAMES)
                shown = peaks[:min(len(peaks), max(end, int(starts[-1]) + 1))]
                lows = np.minimum.reduceat(shown[:, 0], starts)
                highs = np.maximum.reduceat(shown[:, 1], starts)
                mid, half = body.center().y(), body.height() / 2
                p.setPen(QColor(theme.WAVEFORM))
                for x, low, high in zip(columns[at], lows, highs, strict=True):
                    p.drawLine(QPointF(float(x), mid - high * half), QPointF(float(x), mid - low * half))
        p.setPen(QPen(QColor(theme.RECORD_ON), 1))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawRect(rect.adjusted(0.5, 0.5, -0.5, -0.5))
        p.restore()

    def _draw_live_notes(self, p: QPainter, take, take_end: float, area: QRectF, visible: QRectF) -> None:
        """A MIDI take's notes while it records, laid out as a clip's (held ones reach its end)."""
        notes = take.notes
        if not len(notes) or area.height() < 3:
            return
        beats_per_sample = self.project.tempo / 60.0 / self.bridge.engine.sample_rate
        low, high = int(notes[:, 2].min()), int(notes[:, 2].max())
        row = min(area.height() / (high - low + 1), max(2.0, area.height() / 12))
        top = area.top() + (area.height() - row * (high - low + 1)) / 2
        gap = 1.0 if row > 3 else 0.0
        for start, end, key, _velocity, _channel in notes.tolist():
            x0 = self.view.beat_to_x(start * beats_per_sample)
            x1 = self.view.beat_to_x(end * beats_per_sample if end >= 0 else take_end)
            if x1 >= visible.left() and x0 <= visible.right():
                p.fillRect(QRectF(x0, top + (high - key) * row, max(1.0, x1 - x0 - gap), max(1.0, row - gap)),
                           theme.WAVEFORM)

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
        if area is not None and not is_pan_modifier(event.modifiers()):
            self._gesture = automation_lanes.press(self, area, event.position(), event.modifiers())
            self.update()
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

    def copy_area(self) -> None:
        """Copy the clip content of the selected area (Ctrl+C)."""
        if self.selection.clip_range:
            content = self.editor.copy_range(*self.selection.time_range)
            if content is None:
                self.status_message.emit("There are no clips in the selection to copy.")
            else:
                self.clipboard = content

    def cut_area(self) -> None:
        """Copy the clip content of the selected area, and take it out (Ctrl+X); the
        (now empty) area stays selected."""
        if self.selection.clip_range:
            start, end, track_ids = self.selection.time_range
            content = self.editor.cut_range(start, end, track_ids)
            if content is None:
                self.status_message.emit("There are no clips in the selection to cut.")
                return
            self.clipboard = content
            self.selection.set_time_range(start, end, track_ids, clips=set())

    def copy_automation(self) -> None:
        """Copy the automation in the selected lane range (Ctrl+C)."""
        if self.selection.time_range is not None and self.selection.lanes:
            content = self.editor.copy_automation_range(*self.selection.time_range[:2], self.selection.lanes)
            if content is None:
                self.status_message.emit("There is no automation in the selection to copy.")
            else:
                self.clipboard = content

    def cut_automation(self) -> None:
        """Copy the automation in the selected lane range, and delete it (Ctrl+X);
        the range stays selected."""
        if self.selection.time_range is not None and self.selection.lanes:
            content = self.editor.cut_automation_range(*self.selection.time_range[:2], self.selection.lanes)
            if content is None:
                self.status_message.emit("There is no automation in the selection to cut.")
            else:
                self.clipboard = content

    def _paste_automation(self, content: CopiedAutomation, at_beat: float, lanes=None) -> None:
        """Copied automation at `at_beat`: onto `lanes` (default: the selected
        ones) as ProjectEditor.automation_paste_targets puts it, else the lanes it
        came from. The pasted range is selected, and the insert marker goes to its end."""
        selection = self.selection
        if lanes is None:
            lanes = selection.lanes if selection.time_range is not None else ()
            if not lanes and selection.points is not None:
                lanes = (selection.points[:2],)
        pasted = self.editor.paste_automation(content, at_beat, lanes)
        if not pasted:
            self.status_message.emit("The copied automation can't go there: its lanes are gone.")
            return
        at = max(0.0, at_beat)
        track_ids = list(dict.fromkeys(owner for owner, _key in pasted if self.project.has_track(owner)))
        selection.set_time_range(at, at + content.length, track_ids, lanes=tuple(pasted))
        selection.set_insert(at + content.length)

    def paste(self, at_beat: float | None = None, track_id: str | None = None) -> None:
        """Paste copied clip content (Ctrl+V) at `at_beat` (default: the insert
        marker), its top track onto `track_id` (default: the selected track), and
        select it; or copied automation (see _paste_automation). The insert marker
        goes to its end, so pasting again appends."""
        if self.clipboard is None:
            self.status_message.emit("Nothing to paste: copy (Ctrl+C) or cut (Ctrl+X) clips or automation first.")
            return
        if isinstance(self.clipboard, CopiedAutomation):
            self._paste_automation(self.clipboard, self.selection.insert_beat if at_beat is None else at_beat)
            return
        area = self.editor.paste(self.clipboard, self.selection.insert_beat if at_beat is None else at_beat,
                                 track_id or self.selection.track_id)
        if area is None:
            self.status_message.emit("The copied clips can't go there: paste them onto tracks of their kind.")
            return
        start, end, track_ids = area
        self.selection.set_time_range(start, end, track_ids, clips=self.editor.clips_in_range(start, end, track_ids))
        self.selection.set_insert(end)

    def consolidate(self) -> None:
        """Join the selected MIDI clips on each track into one (Ctrl+J), and select them."""
        joined = self.editor.consolidate_clips(sorted(self.selection.clips))
        if joined:
            self.selection.select_clips(self.editor, joined)

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
            # Alt+wheel resizes the track under the mouse, folding (or unfolding) it at its smallest.
            index = self.row_index_at(event.position().y())
            if index is not None:
                wheel_action(self.editor, self.layout_model.rows[index].track_id, event)
        elif mods & Qt.KeyboardModifier.ControlModifier:
            self.view.zoom_at(event.position().x(), 1.2 ** (delta.y() / 120.0))
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
            in_range = automation_lanes.in_range(self, area, pos)
            for text, slot, key in (("Cut", self.cut_automation, QKeySequence.StandardKey.Cut),
                                    ("Copy", self.copy_automation, QKeySequence.StandardKey.Copy)):
                action = menu.addAction(text, slot)
                self._show_shortcut(action, key)
                action.setEnabled(in_range)
            # At the insert marker: onto the selected lanes if this is one of them, else onto this one.
            selected = self.selection.time_range is not None and (area.owner, area.key) in self.selection.lanes
            lanes = self.selection.lanes if selected else ((area.owner, area.key),)
            content = self.clipboard
            paste = menu.addAction("Paste", lambda: self._paste_automation(content, self.selection.insert_beat, lanes))
            self._show_shortcut(paste, QKeySequence.StandardKey.Paste)
            paste.setEnabled(isinstance(content, CopiedAutomation))
            menu.addSeparator()
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
            self._add_clipboard_actions(menu)
            menu.addSeparator()
            menu.addAction("Split Here", lambda: self.editor.split_clips(refs, split_at))
            menu.addAction("Duplicate", self.duplicate_area)
            consolidate = menu.addAction("Consolidate", self.consolidate)
            self._show_shortcut(consolidate, "Ctrl+J")
            consolidate.setEnabled(bool(self.editor.consolidatable(refs)))
            menu.addSeparator()
            menu.addAction("Delete", self.delete_area)
        else:
            index = self.row_index_at(pos.y())
            track_id = None if index is None else self.layout_model.rows[index].track_id
            at, parent = self.editor.insertion_point(track_id)
            if track_id is not None:
                beat = max(0.0, self.view.snap_beat(self.view.x_to_beat(pos.x())))
                paste = menu.addAction("Paste", lambda: self.paste(beat, track_id))
                self._show_shortcut(paste, QKeySequence.StandardKey.Paste)
                paste.setEnabled(self.clipboard is not None)
                menu.addSeparator()
                if self.project.track(track_id).is_midi:
                    menu.addAction("Insert MIDI Clip", lambda: self.insert_midi_clip(track_id, pos.x()))
                    menu.addSeparator()
            menu.addAction("Insert Audio Track", lambda: self.editor.add_audio_track(at, parent=parent))
            menu.addAction("Insert MIDI Track", lambda: self.editor.add_midi_track(at, parent=parent))
            if track_id is not None:
                menu.addAction("Delete Track", lambda: self.editor.delete_tracks([track_id]))
        menu.exec(event.globalPos())

    @staticmethod
    def _show_shortcut(action, key) -> None:
        action.setShortcut(QKeySequence(key))  # (as a tip: the window's action handles the key)
        action.setShortcutVisibleInContextMenu(True)

    def _add_clipboard_actions(self, menu: QMenu) -> None:
        """Cut, Copy and Paste (at the insert marker), for the clicked clips."""
        for text, slot, key in (("Cut", self.cut_area, QKeySequence.StandardKey.Cut),
                                ("Copy", self.copy_area, QKeySequence.StandardKey.Copy),
                                ("Paste", lambda: self.paste(), QKeySequence.StandardKey.Paste)):
            self._show_shortcut(menu.addAction(text, slot), key)
        menu.actions()[-1].setEnabled(self.clipboard is not None)

    # --- Drag & drop from the browser -------------------------------------------

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:
        mime = event.mimeData()
        if (audio_paths(mime) or device_kinds(mime) or mime.hasFormat(PLUGIN_MIME) or preset_paths(mime)
                or moved_devices(mime)):
            event.acceptProposedAction()
        else:
            event.ignore()

    def dragMoveEvent(self, event) -> None:
        moved = moved_devices(event.mimeData())
        if moved:
            # Devices from a track's chain move to another track's, the one under the mouse.
            index = self.row_index_at(event.position().y())
            if index is not None and self.layout_model.rows[index].track_id != moved[0]:
                event.acceptProposedAction()
            else:
                event.ignore()
            return
        paths = audio_paths(event.mimeData())
        if not paths:
            # Devices (and presets) drop onto the track under the mouse; an
            # instrument below the tracks makes a new MIDI track.
            devices = dropped_devices(event.mimeData())
            presets = preset_paths(event.mimeData())
            on_track = (devices or presets) and self.row_index_at(event.position().y()) is not None
            if on_track or presets or any(is_instrument(*d) for d in devices):
                event.acceptProposedAction()
            else:
                event.ignore()
            return
        pos = event.position()
        index = self.row_index_at(pos.y())
        if index is not None and not self.project.track(self.layout_model.rows[index].track_id).is_audio:
            index = None  # audio goes only on an audio track: otherwise it gets a new track
        beat = max(0.0, self.view.snap_beat(self.view.x_to_beat(pos.x())))
        sources = []
        for path in paths:
            info = self.bridge.file_info(path)
            if info is not None:
                sources.append((path, info.duration))
        self._drop_preview = (index, beat, sources)
        event.acceptProposedAction()
        self.update()

    def _drop_presets(self, paths: list[str], track_id: str | None) -> bool:
        """Presets dropped onto a track (None: below the tracks, where an
        instrument preset makes a MIDI track, and the others go on it): new
        devices. Whether any went in."""
        loaded, errors = read_presets(paths)
        for error in errors:
            self.status_message.emit(error)
        if track_id is None:
            instrument = next(((name, d) for name, d in loaded if device_is_instrument(d)), None)
            if instrument is None:
                return False
            name, device = instrument
            track_id = self.editor.add_midi_track_with(device, text=f"Load Preset {name}").id
            loaded.remove(instrument)
        refused = [name for name, device in loaded
                   if not self.editor.insert_device(track_id, device, text=f"Load Preset {name}",
                                                    show_editors=not device.is_rack)]
        if refused:
            self.status_message.emit("Instruments go on MIDI tracks. Drop one below the tracks to make one.")
        self.selection.select_track(track_id)  # show its devices
        return True

    def dragLeaveEvent(self, _event) -> None:
        self._drop_preview = None
        self.update()

    def dropEvent(self, event: QDropEvent) -> None:
        preview, self._drop_preview = self._drop_preview, None
        self.update()
        devices = dropped_devices(event.mimeData())
        index = self.row_index_at(event.position().y())
        moved = moved_devices(event.mimeData())
        if moved:
            if index is not None:
                track_id = self.layout_model.rows[index].track_id
                if self.editor.move_devices_to_track(moved[0], moved[1], track_id):
                    self.selection.select_track(track_id)  # show them where they went
                    event.acceptProposedAction()
            return
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
        presets = preset_paths(event.mimeData())
        if presets:
            if self._drop_presets(presets, None if index is None else self.layout_model.rows[index].track_id):
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
