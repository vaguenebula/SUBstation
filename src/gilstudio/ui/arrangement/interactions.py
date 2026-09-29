"""Mouse gestures on the track lanes: move/copy clips, trim edges, rubber band."""

from __future__ import annotations

from dataclasses import replace

from PySide6.QtCore import QPointF, QRectF, Qt

from ...model import edits
from ...model.project import Clip

DRAG_THRESHOLD = 4


class ClipGesture:
    """Base: a gesture that may preview modified clips while dragging."""

    def hidden_ids(self) -> set[str]:
        return set()

    def ghosts(self) -> list[tuple[int, str, Clip]]:
        """(row index, track colour, clip) to draw as previews."""
        return []

    def rubber_band(self) -> QRectF | None:
        return None

    def move(self, pos: QPointF, modifiers) -> None: ...

    def finish(self) -> None: ...


class MoveClipsGesture(ClipGesture):
    def __init__(self, canvas, press: QPointF, primary: tuple[str, str], refs: list[tuple[str, str]]):
        self.canvas = canvas
        project = canvas.project
        self.press = press
        self.origin_beat = canvas.view.x_to_beat(press.x())
        self.origin_row = canvas.row_index_at(press.y(), clamp=True)
        self.primary = project.clip(*primary)
        self.items = [(project.track_index(tid), project.track(tid).color, project.clip(tid, cid)) for tid, cid in refs]
        self.refs = refs
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
        new_start = max(0.0, view.snap_beat(self.primary.start_beat + raw, bypass))
        self.delta = max(new_start - self.primary.start_beat, -min(c.start_beat for _, _, c in self.items))
        row = self.canvas.row_index_at(pos.y(), clamp=True)
        rows = [r for r, _, _ in self.items]
        count = len(self.canvas.project.tracks)
        self.track_delta = max(-min(rows), min(row - self.origin_row, count - 1 - max(rows)))
        self.copy = bool(modifiers & Qt.KeyboardModifier.ControlModifier)

    def hidden_ids(self) -> set[str]:
        return {c.id for _, _, c in self.items} if self.active and not self.copy else set()

    def ghosts(self) -> list[tuple[int, str, Clip]]:
        if not self.active:
            return []
        colors = [t.color for t in self.canvas.project.tracks]
        return [(r + self.track_delta, colors[r + self.track_delta], replace(c, start_beat=c.start_beat + self.delta))
                for r, _, c in self.items]

    def finish(self) -> None:
        if not self.active:
            return
        new_refs = self.canvas.editor.move_clips(self.refs, self.delta, self.track_delta, copy_clips=self.copy)
        if new_refs:
            self.canvas.selection.set_clips(new_refs, track_id=new_refs[0][0])


class TrimGesture(ClipGesture):
    def __init__(self, canvas, track_id: str, clip: Clip, edge: str):
        self.canvas = canvas
        self.track_id = track_id
        self.clip = clip
        self.edge = edge
        self.row = canvas.project.track_index(track_id)
        self.color = canvas.project.track(track_id).color
        self.result: Clip | None = None

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

    def ghosts(self) -> list[tuple[int, str, Clip]]:
        return [(self.row, self.color, self.result)] if self.result else []

    def finish(self) -> None:
        if self.result and self.result != self.clip:
            self.canvas.editor.replace_clip(self.track_id, self.result, "Trim Clip")


class RubberBandGesture(ClipGesture):
    def __init__(self, canvas, press: QPointF, additive: bool):
        self.canvas = canvas
        self.press = press
        self.base = set(canvas.selection.clips) if additive else set()
        self.rect: QRectF | None = None

    def move(self, pos: QPointF, modifiers) -> None:
        if self.rect is None and (pos - self.press).manhattanLength() < DRAG_THRESHOLD:
            return
        self.rect = QRectF(self.press, pos).normalized()
        view = self.canvas.view
        b0 = view.x_to_beat(self.rect.left())
        b1 = view.x_to_beat(self.rect.right())
        tempo = self.canvas.project.tempo
        top = self.rect.top() + view.scroll_y
        bottom = self.rect.bottom() + view.scroll_y
        hits = set()
        for _, row in self.canvas.layout_model.visible_rows(top, bottom):
            for clip in self.canvas.project.track(row.track_id).clips:
                if clip.start_beat < b1 and clip.end_beat(tempo) > b0:
                    hits.add((row.track_id, clip.id))
        self.canvas.selection.set_clips(self.base | hits)

    def rubber_band(self) -> QRectF | None:
        return self.rect
