"""Mouse gestures on the track lanes: move/copy a time selection (a selected clip
is one too), trim edges, time selection, and hand-scrolling."""

from __future__ import annotations

from dataclasses import replace

from PySide6.QtCore import QPointF, Qt

from ...model import edits
from ...model.project import AnyClip

DRAG_THRESHOLD = 4


class ClipGesture:
    """Base: a gesture that may preview modified clips while dragging."""

    def hidden_ids(self) -> set[str]:
        return set()

    def ghosts(self) -> list[tuple[int, str, AnyClip]]:
        """(row index, track colour, clip) to draw as previews."""
        return []

    def kept(self) -> list[tuple[int, str, AnyClip]]:
        """(row index, track colour, clip) to draw, unselected, in place of hidden clips."""
        return []

    def time_range(self) -> tuple[float, float, list[str]] | None:
        """Where to draw the time selection while dragging it, if it is being moved."""
        return None

    def move(self, pos: QPointF, modifiers) -> None: ...

    def finish(self) -> None: ...


class MoveRangeGesture(ClipGesture):
    """Drag inside a clip range: move (Ctrl: copy) the selected stretch of clips,
    split at the range's edges. A click without dragging calls `on_click`."""

    def __init__(self, canvas, press: QPointF, on_click=None):
        self.canvas = canvas
        self.press = press
        self.on_click = on_click
        self.start, self.end, track_ids = canvas.selection.time_range
        self.track_ids = list(track_ids)
        project = canvas.project
        tempo = project.tempo
        self.origin_beat = canvas.view.x_to_beat(press.x())
        self.origin_row = canvas.row_index_at(press.y(), clamp=True)
        self.touched: set[str] = set()
        self.remnants: list[tuple[int, str, AnyClip]] = []
        self.pieces: list[tuple[int, AnyClip]] = []
        for tid in self.track_ids:
            track = project.track(tid)
            row = project.track_index(tid)
            inside = [c for c in track.clips if c.start_beat < self.end and c.end_beat(tempo) > self.start]
            self.touched |= {c.id for c in inside}
            self.remnants += [(row, track.color, c) for c in edits.remove_range(inside, self.start, self.end, tempo)]
            self.pieces += [(row, c) for c in edits.slice_range(inside, self.start, self.end, tempo)]
        self.delta = 0.0
        self.track_delta = 0
        self.copy = False
        self.active = False

    def move(self, pos: QPointF, modifiers) -> None:
        if not self.active:
            if (pos - self.press).manhattanLength() < DRAG_THRESHOLD:
                return
            self.active = True
        view = self.canvas.view
        bypass = bool(modifiers & Qt.KeyboardModifier.AltModifier)
        raw = view.x_to_beat(pos.x()) - self.origin_beat
        self.delta = max(0.0, view.snap_beat(self.start + raw, bypass)) - self.start
        row = self.canvas.row_index_at(pos.y(), clamp=True)
        self.track_delta = self.canvas.editor.clamp_track_delta([(t, "") for t in self.track_ids],
                                                                row - self.origin_row)
        self.copy = bool(modifiers & Qt.KeyboardModifier.ControlModifier)

    def hidden_ids(self) -> set[str]:
        return self.touched if self.active and not self.copy else set()

    def ghosts(self) -> list[tuple[int, str, AnyClip]]:
        if not self.active:
            return []
        colors = [t.color for t in self.canvas.project.tracks]
        return [(r + self.track_delta, colors[r + self.track_delta], replace(c, start_beat=c.start_beat + self.delta))
                for r, c in self.pieces]

    def kept(self) -> list[tuple[int, str, AnyClip]]:
        return self.remnants if self.active and not self.copy else []

    def time_range(self) -> tuple[float, float, list[str]] | None:
        if not self.active:
            return None
        tracks = self.canvas.project.tracks
        ids = [tracks[self.canvas.project.track_index(t) + self.track_delta].id for t in self.track_ids]
        return self.start + self.delta, self.end + self.delta, ids

    def finish(self) -> None:
        if not self.active:
            if self.on_click:
                self.on_click()
            return
        start, track_ids = self.canvas.editor.move_range(self.start, self.end, self.track_ids, self.delta,
                                                         self.track_delta, copy_clips=self.copy)
        end = start + self.end - self.start
        selection = self.canvas.selection
        selection.set_time_range(start, end, track_ids,
                                 clips=self.canvas.editor.clips_in_range(start, end, track_ids))
        selection.set_insert(start)


class TrimGesture(ClipGesture):
    def __init__(self, canvas, track_id: str, clip: AnyClip, edge: str):
        self.canvas = canvas
        self.track_id = track_id
        self.clip = clip
        self.edge = edge
        self.row = canvas.project.track_index(track_id)
        self.color = canvas.project.track(track_id).color
        self.result: AnyClip | None = None

    def move(self, pos: QPointF, modifiers) -> None:
        view = self.canvas.view
        tempo = self.canvas.project.tempo
        beat = view.snap_beat(view.x_to_beat(pos.x()), bool(modifiers & Qt.KeyboardModifier.AltModifier))
        if self.edge == "left":
            self.result = edits.trim_start(self.clip, beat, tempo)
        else:
            self.result = edits.trim_end(self.clip, beat, tempo)

    def hidden_ids(self) -> set[str]:
        return {self.clip.id} if self.result else set()

    def ghosts(self) -> list[tuple[int, str, AnyClip]]:
        return [(self.row, self.color, self.result)] if self.result else []

    def time_range(self) -> tuple[float, float, list[str]] | None:
        if not self.result:
            return None
        return self.result.start_beat, self.result.end_beat(self.canvas.project.tempo), [self.track_id]

    def finish(self) -> None:
        if self.result and self.result != self.clip:
            self.canvas.editor.replace_clip(self.track_id, self.result, "Trim Clip")
            # The selection follows the clip's new edges.
            self.canvas.selection.select_clips(self.canvas.editor, [(self.track_id, self.clip.id)])


class TimeSelectGesture(ClipGesture):
    """Click places the insert marker; drag selects across tracks. Where the drag
    ends decides what: in a lane's clip (title) band, or anywhere above the lane it
    started in, it selects the clips it touches; lower down it selects a time range
    (later: automation)."""

    def __init__(self, canvas, press: QPointF, bypass_snap: bool):
        self.canvas = canvas
        self.press = press
        self.anchor = max(0.0, canvas.view.snap_beat(canvas.view.x_to_beat(press.x()), bypass_snap))
        self.anchor_row = canvas.row_index_at(press.y(), clamp=True)
        self.active = False

    def move(self, pos: QPointF, modifiers) -> None:
        if not self.active:
            if (pos - self.press).manhattanLength() < DRAG_THRESHOLD:
                return
            self.active = True
        view = self.canvas.view
        bypass = bool(modifiers & Qt.KeyboardModifier.AltModifier)
        beat = max(0.0, view.snap_beat(view.x_to_beat(pos.x()), bypass))
        start, end = sorted((self.anchor, beat))
        row = self.canvas.row_index_at(pos.y(), clamp=True)
        first, last = sorted((self.anchor_row, row))
        rows = self.canvas.layout_model.rows[first:last + 1]
        track_ids = [r.track_id for r in rows]
        selection = self.canvas.selection
        # Up into the lanes above (or past the first track) selects clips too: only
        # a drag kept to the lower part of lanes at or below its start is a lane range.
        above = pos.y() + view.scroll_y < self.canvas.layout_model.rows[self.anchor_row].top
        if above or self.canvas.in_clip_band(pos):
            selection.set_time_range(start, end, track_ids,
                                     clips=self.canvas.editor.clips_in_range(start, end, track_ids))
            self.canvas.setCursor(Qt.CursorShape.PointingHandCursor)
        else:
            selection.set_time_range(start, end, track_ids)
            self.canvas.setCursor(Qt.CursorShape.IBeamCursor)
        selection.set_insert(start)


class PanGesture(ClipGesture):
    """Ctrl+Alt drag: scroll the arrangement in both directions (Ableton's hand)."""

    def __init__(self, canvas, press: QPointF):
        self.canvas = canvas
        self.press = press
        self.scroll_beats = canvas.view.scroll_beats
        self.scroll_y = canvas.view.scroll_y

    def move(self, pos: QPointF, modifiers) -> None:
        view = self.canvas.view
        delta = pos - self.press
        view.set_scroll_beats(self.scroll_beats - delta.x() / view.px_per_beat)
        view.set_scroll_y(self.scroll_y - delta.y())
