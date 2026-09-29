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


def draw_grid(painter: QPainter, view: ViewState, x0: float, x1: float, top: float, bottom: float) -> None:
    if bottom <= top:
        return
    for x, _beat, kind in grid_lines(view, x0 - 1, x1 + 1):
        painter.fillRect(QRectF(round(x), top, 1, bottom - top), _COLORS[kind])


def draw_loop_region(painter: QPainter, view: ViewState, x0: float, x1: float, top: float, bottom: float) -> None:
    project = view.project
    if not project.loop_enabled or bottom <= top:
        return
    left = max(x0, view.beat_to_x(project.loop_start))
    right = min(x1, view.beat_to_x(project.loop_end))
    if right > left:
        painter.fillRect(QRectF(left, top, right - left, bottom - top), theme.LOOP_REGION)
