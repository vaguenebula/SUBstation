"""State shared by the arrangement widgets: zoom/scroll, grid, selection, layout."""

from __future__ import annotations

import bisect
import math
from dataclasses import dataclass

from PySide6.QtCore import QObject, Signal

from ...model.project import Project

GRID_MIN_PIXELS = {-2: 6.0, -1: 11.0, 0: 20.0, 1: 40.0, 2: 80.0}  # narrowest .. widest


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

    def frames_per_pixel(self, sample_rate: float) -> float:
        return sample_rate * 60.0 / (self.project.tempo * self.px_per_beat)

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
    """Selected clips, the selected track, the insert (start) marker, and a time
    selection: a beat range spanning one or more adjacent tracks.

    A time selection made in the clips' band is a clip range: `clips` holds the
    clips it touches (for the clip view) and Delete cuts out just that range.
    Made lower in the lanes it is a lane range (for automation, later)."""

    changed = Signal()
    insert_changed = Signal()

    def __init__(self, parent: QObject | None = None):
        super().__init__(parent)
        self.clips: set[tuple[str, str]] = set()
        self.track_id: str | None = None
        self.insert_beat = 0.0
        self.time_range: tuple[float, float, tuple[str, ...]] | None = None  # start, end, track ids
        self._range_selects_clips = False
        self.focus = "clips"  # what Delete acts on: "clips" or "track"

    def set_clips(self, refs, track_id: str | None = None) -> None:
        self.clips = set(refs)
        self.time_range = None
        if track_id is not None:
            self.track_id = track_id
        self.focus = "clips"
        self.changed.emit()

    def toggle_clip(self, ref: tuple[str, str]) -> None:
        self.clips ^= {ref}
        self.time_range = None
        self.track_id = ref[0]
        self.focus = "clips"
        self.changed.emit()

    def select_track(self, track_id: str | None, focus_track: bool = False) -> None:
        self.track_id = track_id
        if focus_track:
            self.focus = "track"
            self.clips = set()
            self.time_range = None
        self.changed.emit()

    def set_time_range(self, start: float, end: float, track_ids, clips=None) -> None:
        """Select a beat range across tracks. With `clips` (the clips it touches,
        possibly none) it is a clip range, otherwise a lane range."""
        track_ids = tuple(track_ids)
        self.time_range = (start, end, track_ids) if end > start and track_ids else None
        self._range_selects_clips = clips is not None
        self.clips = set(clips or ()) if self.time_range else set()
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
        """Drop references to clips/tracks that no longer exist."""
        valid_tracks = {t.id for t in project.tracks}
        valid_clips = {(t.id, c.id) for t in project.tracks for c in t.clips}
        clips = self.clips & valid_clips
        track_id = self.track_id if self.track_id in valid_tracks else None
        time_range = self.time_range
        if time_range is not None:
            kept = tuple(t for t in time_range[2] if t in valid_tracks)
            time_range = (time_range[0], time_range[1], kept) if kept else None
        if clips != self.clips or track_id != self.track_id or time_range != self.time_range:
            self.clips = clips
            self.track_id = track_id
            self.time_range = time_range
            self.changed.emit()


@dataclass(frozen=True)
class Row:
    track_id: str
    top: int
    height: int

    @property
    def bottom(self) -> int:
        return self.top + self.height


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
            self.rows.append(Row(track.id, y, track.height))
            y += track.height
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
