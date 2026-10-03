"""Grid lines shared by the ruler and the lanes."""

from __future__ import annotations

import math
from collections.abc import Iterator

from PySide6.QtCore import QRectF
from PySide6.QtGui import QColor, QPainter

from ... import theme
from ...model.timebase import is_multiple
from .view_state import ViewState

_COLORS = {"bar": QColor(theme.GRID_BAR), "beat": QColor(theme.GRID_BEAT), "sub": QColor(theme.GRID_SUB)}
# Over clips: faint dark lines, so the grid shows through whatever colour a clip has.
_OVER_CLIP_COLORS = {"bar": QColor(0, 0, 0, 70), "beat": QColor(0, 0, 0, 42), "sub": QColor(0, 0, 0, 24)}


def grid_lines(view: ViewState, x0: float, x1: float, step: float | None = None) -> Iterator[tuple[float, float, str]]:
    """Yields (x, beat, kind) with kind in bar/beat/sub for visible grid lines."""
    step = step or view.grid_step()
    ts = view.project.time_signature
    first = max(0, math.floor(view.x_to_beat(x0) / step))
    last = math.ceil(view.x_to_beat(x1) / step)
    for k in range(first, last + 1):
        beat = k * step
        if is_multiple(beat, ts.beats_per_bar):
            kind = "bar"
        elif is_multiple(beat, ts.beat_length):
            kind = "beat"
        else:
            kind = "sub"
        yield view.beat_to_x(beat), beat, kind


def label_step(view: ViewState, step: float) -> float:
    """Beats between ruler labels: the grid step or a coarser musical unit, so
    labels are at least 44 px apart."""
    ts = view.project.time_signature
    bar = ts.beats_per_bar
    candidates = [step, ts.beat_length, bar] + [bar * m for m in (2, 4, 8, 16, 32, 64, 128)]
    for candidate in sorted(c for c in candidates if c >= step):
        if candidate * view.px_per_beat >= 44:
            return candidate
    return candidates[-1]


def draw_grid(painter: QPainter, view: ViewState, x0: float, x1: float, top: float, bottom: float,
              over_clip: bool = False) -> None:
    """The grid lines between x0 and x1; `over_clip` draws them faint, to show through a clip."""
    if bottom <= top:
        return
    colors = _OVER_CLIP_COLORS if over_clip else _COLORS
    for x, _beat, kind in grid_lines(view, x0 - 1, x1 + 1):
        painter.fillRect(QRectF(round(x), top, 1, bottom - top), colors[kind])


def draw_loop_region(painter: QPainter, view: ViewState, x0: float, x1: float, top: float, bottom: float) -> None:
    project = view.project
    if not project.loop_enabled or bottom <= top:
        return
    left = max(x0, view.beat_to_x(project.loop_start))
    right = min(x1, view.beat_to_x(project.loop_end))
    if right > left:
        painter.fillRect(QRectF(left, top, right - left, bottom - top), theme.LOOP_REGION)
