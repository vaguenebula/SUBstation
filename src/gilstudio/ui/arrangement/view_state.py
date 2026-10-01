"""State shared by the arrangement widgets: zoom/scroll, grid, selection, layout."""

from __future__ import annotations

import bisect
import math
from dataclasses import dataclass

from PySide6.QtCore import QObject, Signal

from ...model.automation import MASTER
from ...model.editor import ProjectEditor
from ...model.project import Project

GRID_MIN_PIXELS = {-2: 6.0, -1: 11.0, 0: 20.0, 1: 40.0, 2: 80.0}  # narrowest .. widest
AUTOMATION_LANE_HEIGHT = 44  # a lane shown below a track (or the master)
# A track's own lane while its automation shows: room in its header for the choosers.
MIN_AUTOMATION_ROW = 76


class ViewState(QObject):
    changed = Signal()  # zoom or horizontal scroll
    vscroll_changed = Signal()
    grid_changed = Signal()

    MIN_PX_PER_BEAT = 0.25
    MAX_PX_PER_BEAT = 4000.0

    def __init__(self, project: Project, parent: QObject | None = None):
        super().__init__(parent)
        self.project = project
        self.px_per_beat = 24.0
        self.scroll_beats = 0.0
        self.scroll_y = 0
        self.max_scroll_y = 0  # maintained by the arrangement view
        self.grid_level = 0
        self.snap = True
        self.follow = True

    # --- Coordinates -------------------------------------------------------------

    def beat_to_x(self, beat: float) -> float:
        return (beat - self.scroll_beats) * self.px_per_beat

    def x_to_beat(self, x: float) -> float:
        return self.scroll_beats + x / self.px_per_beat

    def frames_per_pixel(self, sample_rate: float, source_tempo: float | None = None) -> float:
        """Source frames per pixel. `source_tempo` is the tempo the audio maps onto
        beats at (a warped clip's segment BPM); by default the project tempo."""
        return sample_rate * 60.0 / ((source_tempo or self.project.tempo) * self.px_per_beat)

    # --- Scroll & zoom -----------------------------------------------------------

    def set_scroll_beats(self, beats: float) -> None:
        beats = max(0.0, beats)
        if beats != self.scroll_beats:
            self.scroll_beats = beats
            self.changed.emit()

    def set_scroll_y(self, y: float) -> None:
        y = max(0, min(self.max_scroll_y, int(y)))
        if y != self.scroll_y:
            self.scroll_y = y
            self.vscroll_changed.emit()

    def zoom_at(self, x: float, factor: float) -> None:
        """Zoom keeping the beat under `x` in place."""
        anchor = self.x_to_beat(x)
        ppb = max(self.MIN_PX_PER_BEAT, min(self.MAX_PX_PER_BEAT, self.px_per_beat * factor))
        if ppb == self.px_per_beat:
            return
        self.px_per_beat = ppb
        self.scroll_beats = max(0.0, anchor - x / ppb)
        self.changed.emit()

    def zoom_to_fit(self, start: float, end: float, width: float) -> None:
        if end <= start or width <= 0:
            return
        self.px_per_beat = max(self.MIN_PX_PER_BEAT, min(self.MAX_PX_PER_BEAT, width / (end - start)))
        self.scroll_beats = max(0.0, start)
        self.changed.emit()

    # --- Grid ----------------------------------------------------------------------

    def set_grid_level(self, level: int) -> None:
        level = max(min(GRID_MIN_PIXELS), min(max(GRID_MIN_PIXELS), level))
        if level != self.grid_level:
            self.grid_level = level
            self.grid_changed.emit()

    def set_snap(self, snap: bool) -> None:
        if snap != self.snap:
            self.snap = snap
            self.grid_changed.emit()

    def grid_step(self) -> float:
        """Adaptive grid in beats, like Ableton's zoom-dependent grid."""
        bar = self.project.time_signature.beats_per_bar
        sub_bar = [s for s in (1 / 32, 1 / 16, 1 / 8, 1 / 4, 1 / 2, 1.0, 2.0)
                   if s < bar and math.isclose(bar / s, round(bar / s))]
        steps = sub_bar + [bar * m for m in (1, 2, 4, 8, 16, 32, 64, 128, 256)]
        min_px = GRID_MIN_PIXELS[self.grid_level]
        for step in steps:
            if step * self.px_per_beat >= min_px:
                return step
        return steps[-1]

    def snap_beat(self, beat: float, bypass: bool = False) -> float:
        if not self.snap or bypass:
            return beat
        step = self.grid_step()
        return round(beat / step) * step


class Selection(QObject):
    """The selected track (or tracks), the insert (start) marker, and a time selection: a beat
    range spanning one or more adjacent tracks. Selecting is always on the grid:
    selecting a clip selects the area it covers.

    A time selection made in the clips' band (or by clicking clips) is a clip
    range: `clips` holds the clips it touches (for the clip view) and Delete cuts
    out just that range. Made lower in the lanes it is a lane range; made in
    automation lanes, `lanes` holds them (owner, target key), and Delete clears
    their automation in the range.

    Automation breakpoints can be selected too (`points`: owner, key, indices);
    Delete deletes them.

    Several tracks can be selected (Ctrl-click toggles one, Shift-click selects
    the tracks from the last one clicked): `track_ids`. `track_id` is the one
    the device view shows, the last one clicked."""

    changed = Signal()
    insert_changed = Signal()

    def __init__(self, parent: QObject | None = None):
        super().__init__(parent)
        self.clips: set[tuple[str, str]] = set()
        self.track_id: str | None = None
        self._tracks: tuple[str, ...] = ()  # several selected tracks; only while track_id is one of them
        self._anchor: str | None = None  # where a Shift-click selects tracks from
        self.insert_beat = 0.0
        self.time_range: tuple[float, float, tuple[str, ...]] | None = None  # start, end, track ids
        self._range_selects_clips = False
        self.lanes: tuple[tuple[str, str], ...] = ()  # automation lanes a lane range covers
        self.points: tuple[str, str, frozenset[int]] | None = None  # selected automation breakpoints
        # What Delete acts on: "clips", "track", "devices" (the device view's) or "automation" (breakpoints).
        self.focus = "clips"

    def clear(self, track_id: str | None = None) -> None:
        """Select nothing (on `track_id`, if given: where the insert marker shows)."""
        self.clips = set()
        self.time_range = None
        self.lanes = ()
        self.points = None
        self._tracks = ()
        if track_id is not None:
            self.track_id = track_id
        self.focus = "clips"
        self.changed.emit()

    def select_points(self, owner: str, key: str, indices) -> None:
        """Select breakpoints of one envelope (none: select nothing)."""
        indices = frozenset(indices)
        self.clips = set()
        self.time_range = None
        self.lanes = ()
        self.points = (owner, key, indices) if indices else None
        self._tracks = ()
        if owner != MASTER:
            self.track_id = owner
        self.focus = "automation" if indices else "clips"
        self.changed.emit()

    def selected_points(self, owner: str, key: str) -> frozenset[int]:
        if self.points is not None and self.points[:2] == (owner, key):
            return self.points[2]
        return frozenset()

    def select_clips(self, editor: ProjectEditor, refs, track_id: str | None = None) -> None:
        """Select the grid area that fully contains these clips: from the earliest
        start to the latest end, on every track from the first clip's to the last's."""
        area = editor.clips_area(refs)
        if area is None:
            self.clear(track_id)
            return
        start, end, track_ids = area
        self.set_time_range(start, end, track_ids, clips=editor.clips_in_range(start, end, track_ids))
        if track_id is not None and track_id in track_ids:
            self.track_id = track_id

    @property
    def track_ids(self) -> tuple[str, ...]:
        """The selected tracks, in the order they were selected (track_id among them)."""
        if self.track_id in self._tracks:
            return self._tracks
        return (self.track_id,) if self.track_id is not None else ()

    def select_track(self, track_id: str | None, focus_track: bool = False, mode: str = "",
                     order: list[str] | None = None) -> None:
        """Select a track. `mode` "toggle" (Ctrl-click) adds it to the selected
        tracks or takes it out; "range" (Shift-click) selects the tracks from the
        last one clicked to it, in `order` (the tracks top to bottom)."""
        if mode == "toggle" and track_id is not None:
            tracks = self.track_ids
            if track_id in tracks:
                tracks = tuple(t for t in tracks if t != track_id)
                track_id = tracks[-1] if tracks else None
            else:
                tracks = tracks + (track_id,)
            self._tracks = tracks
            self._anchor = track_id
        elif mode == "range" and track_id is not None and order and self._anchor in order and track_id in order:
            a, b = order.index(self._anchor), order.index(track_id)
            self._tracks = tuple(order[min(a, b):max(a, b) + 1])
        else:
            self._tracks = ()
            self._anchor = track_id
        self.track_id = track_id
        if focus_track:
            self.focus = "track"
            self.clips = set()
            self.time_range = None
            self.lanes = ()
            self.points = None
        self.changed.emit()

    def focus_devices(self) -> None:
        """Devices were selected in the device view: Delete acts on them now."""
        if self.focus != "devices":
            self.focus = "devices"
            self.changed.emit()

    def set_time_range(self, start: float, end: float, track_ids, clips=None, lanes=()) -> None:
        """Select a beat range across tracks. With `clips` (the clips it touches,
        possibly none) it is a clip range, otherwise a lane range: of automation
        `lanes` (owner, key), if given (the master's have no track)."""
        track_ids = tuple(track_ids)
        lanes = tuple(lanes) if clips is None else ()
        self.time_range = (start, end, track_ids) if end > start and (track_ids or lanes) else None
        self._range_selects_clips = clips is not None
        self.clips = set(clips or ()) if self.time_range else set()
        self.lanes = lanes if self.time_range else ()
        self.points = None
        self._tracks = ()
        if track_ids:
            self.track_id = track_ids[0]
        self.focus = "clips"
        self.changed.emit()

    @property
    def clip_range(self) -> bool:
        """Whether the time selection selects clip content (not automation). Derived,
        so clearing the time selection can never leave it stale."""
        return self.time_range is not None and self._range_selects_clips

    def set_insert(self, beat: float) -> None:
        self.insert_beat = max(0.0, beat)
        self.insert_changed.emit()

    def prune(self, project: Project) -> None:
        """Drop references to clips/tracks/breakpoints that no longer exist."""
        valid_tracks = {t.id for t in project.tracks}
        valid_clips = {(t.id, c.id) for t in project.tracks for c in t.clips}
        clips = self.clips & valid_clips
        tracks = tuple(t for t in self._tracks if t in valid_tracks)
        track_id = self.track_id if self.track_id in valid_tracks else (tracks[-1] if tracks else None)
        lanes = tuple(lane for lane in self.lanes if project.has_owner(lane[0]))
        time_range = self.time_range
        if time_range is not None:
            kept = tuple(t for t in time_range[2] if t in valid_tracks)
            time_range = (time_range[0], time_range[1], kept) if kept or lanes else None
        points = self.points
        if points is not None:
            owner, key, indices = points
            count = len(project.envelope(owner, key)) if project.has_owner(owner) else 0
            indices = frozenset(i for i in indices if i < count)
            points = (owner, key, indices) if indices else None
        if (clips != self.clips or track_id != self.track_id or time_range != self.time_range
                or lanes != self.lanes or points != self.points or tracks != self._tracks):
            self.clips = clips
            self.track_id = track_id
            self._tracks = tracks
            self.time_range = time_range
            self.lanes = lanes if time_range else ()
            self.points = points
            if points is None and self.focus == "automation":
                self.focus = "clips"
            self.changed.emit()


@dataclass(frozen=True)
class LaneRow:
    """An automation lane shown below its owner's own lane."""

    index: int  # in the owner's AutomationView.lanes
    key: str
    top: int
    height: int


@dataclass(frozen=True)
class Row:
    """A track: its own lane (clips; its automation too while that shows), then
    the automation lanes shown below it."""

    track_id: str
    top: int
    main_height: int  # the track's own lane
    lanes: tuple[LaneRow, ...] = ()
    automation: bool = False  # its automation shows

    @property
    def height(self) -> int:
        return self.main_height + sum(lane.height for lane in self.lanes)

    @property
    def bottom(self) -> int:
        return self.top + self.height


def automation_rows(view, top: int, main_height: int) -> tuple[int, tuple[LaneRow, ...]]:
    """The height of an owner's own lane with its automation view, and the lanes below it."""
    if not view.shown:
        return main_height, ()
    main_height = max(main_height, MIN_AUTOMATION_ROW)
    lanes = tuple(LaneRow(i, key, top + main_height + i * AUTOMATION_LANE_HEIGHT, AUTOMATION_LANE_HEIGHT)
                  for i, key in enumerate(view.lanes))
    return main_height, lanes


class TrackLayout:
    """Vertical layout of track rows in content coordinates (before scrolling)."""

    def __init__(self, project: Project):
        self.project = project
        self.rows: list[Row] = []
        self._tops: list[int] = []
        self.total_height = 0
        self.rebuild()

    def rebuild(self) -> None:
        self.rows = []
        y = 0
        for track in self.project.tracks:
            view = track.automation_view
            main_height, lanes = automation_rows(view, y, track.height)
            row = Row(track.id, y, main_height, lanes, view.shown)
            self.rows.append(row)
            y += row.height
        self._tops = [r.top for r in self.rows]
        self.total_height = y

    def row_index_at(self, content_y: float) -> int | None:
        if content_y < 0 or content_y >= self.total_height:
            return None
        return bisect.bisect_right(self._tops, content_y) - 1

    def row_for(self, track_id: str) -> Row | None:
        for row in self.rows:
            if row.track_id == track_id:
                return row
        return None

    def visible_rows(self, top: float, bottom: float) -> list[tuple[int, Row]]:
        start = max(0, bisect.bisect_right(self._tops, top) - 1)
        result = []
        for index in range(start, len(self.rows)):
            row = self.rows[index]
            if row.top >= bottom:
                break
            result.append((index, row))
        return result
