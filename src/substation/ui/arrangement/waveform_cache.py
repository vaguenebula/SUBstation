"""Waveform drawing: vectorised numpy rasterisation into cached QImage tiles.

Tiles are anchored to the start of the source file (not the clip), so trimming
or moving a clip reuses them; only zoom, lane-height or gain changes render new
ones. A clip's gain scales its waveform (louder is taller, cut off at the lane's
edges), as it scales its audio.
"""

from __future__ import annotations

import math
from collections import OrderedDict

import numpy as np
from PySide6.QtCore import QPointF, QRectF
from PySide6.QtGui import QColor, QImage, QPainter

from ... import _engine as ge

TILE = 256  # pixels
MAX_TILES = 800


def _column_minmax(lo: np.ndarray, hi: np.ndarray, starts: np.ndarray, ends: np.ndarray):
    """Per-column min/max over [starts[i], ends[i]) of (channels, n) arrays.
    `starts` is non-decreasing and every column is non-empty."""
    w0 = int(starts[0])
    w1 = int(ends[-1])
    idx = starts - w0
    col_lo = np.minimum.reduceat(lo[:, w0:w1], idx, axis=1)
    col_hi = np.maximum.reduceat(hi[:, w0:w1], idx, axis=1)
    return col_lo, col_hi


def _smooth(values: np.ndarray) -> np.ndarray:
    """[1 2 1]/4 along the last axis; the outer columns are padding and get dropped."""
    return (values[..., :-2] + 2.0 * values[..., 1:-1] + values[..., 2:]) * 0.25


def render_tile(source: ge.AudioSource, frames_per_px: float, index: int, height: int, split_channels: bool,
                argb: int, gain: float = 1.0) -> QImage | None:
    frames = source.frames
    # One padding column each side, so the smoothing is seamless across tile borders.
    edges = (index * TILE - 1 + np.arange(TILE + 3, dtype=np.float64)) * frames_per_px
    if edges[1] >= frames or height < 2:
        return None
    edges = np.maximum(edges, 0.0)

    level = -1
    for candidate in range(source.peak_levels - 1, -1, -1):
        if ge.AudioSource.samples_per_peak(candidate) <= frames_per_px:
            level = candidate
            break

    if level >= 0:
        peaks = source.peaks(level)  # (n, channels, 2)
        lo = peaks[:, :, 0].T
        hi = peaks[:, :, 1].T
        idx = np.floor(edges / ge.AudioSource.samples_per_peak(level)).astype(np.int64)
    else:
        first = int(edges[1])
        count = min(frames, int(math.ceil(edges[-1])) + 1) - first
        lo = hi = source.samples(first, count)  # (channels, count)
        idx = np.floor(edges).astype(np.int64) - first

    n = lo.shape[1]
    valid = idx[:-1] < n
    starts = np.clip(idx[:-1], 0, n - 1)
    ends = np.maximum(np.clip(idx[1:], 0, n), starts + 1)
    col_lo, col_hi = _column_minmax(lo, hi, starts, ends)
    col_lo = np.where(valid, col_lo, 0.0) * gain
    col_hi = np.where(valid, col_hi, 0.0) * gain
    if level >= 0:  # envelopes (not raw samples) read smoother with a light blur
        col_lo = _smooth(col_lo)
        col_hi = _smooth(col_hi)
    else:
        col_lo = col_lo[:, 1:-1]
        col_hi = col_hi[:, 1:-1]
    valid = valid[1:-1]

    if split_channels and col_lo.shape[0] == 2:
        half = height // 2
        lanes = [(col_lo[0], col_hi[0], 0, half), (col_lo[1], col_hi[1], half, height - half)]
    else:
        lanes = [(col_lo.min(axis=0), col_hi.max(axis=0), 0, height)]

    # Anti-aliased: each row gets the fraction of it the envelope covers.
    cover = np.zeros((height, TILE), dtype=np.float32)
    rows = np.arange(height, dtype=np.float32)[:, None]
    for c_lo, c_hi, top, lane_h in lanes:
        center = top + lane_h / 2.0
        half = max(1.0, lane_h / 2.0 - 1.0)
        y_top = np.clip(center - np.clip(c_hi, -1, 1) * half, top, top + lane_h).astype(np.float32)
        y_bot = np.clip(center - np.clip(c_lo, -1, 1) * half, top, top + lane_h).astype(np.float32)
        y_bot = np.minimum(np.maximum(y_bot, y_top + 1.0), top + lane_h)  # at least a pixel thick
        y_top = np.minimum(y_top, y_bot - 1.0)
        part = np.clip(np.minimum(rows + 1.0, y_bot[None, :]) - np.maximum(rows, y_top[None, :]), 0.0, 1.0)
        part *= valid[None, :]
        part[:top] = 0.0
        part[top + lane_h:] = 0.0
        cover = np.maximum(cover, part)

    base_a = (argb >> 24) & 0xFF
    channels = [((argb >> shift) & 0xFF) for shift in (16, 8, 0)]
    a = cover * (base_a / 255.0)
    image = ((a * 255.0 + 0.5).astype(np.uint32) << 24)
    for shift, value in zip((16, 8, 0), channels):
        image |= (a * value + 0.5).astype(np.uint32) << shift

    qimage = QImage(image.data, TILE, height, TILE * 4, QImage.Format.Format_ARGB32_Premultiplied)
    return qimage.copy()  # detach from the numpy buffer


class WaveformCache:
    def __init__(self, max_tiles: int = MAX_TILES):
        self._tiles: OrderedDict[tuple, QImage | None] = OrderedDict()
        self._max_tiles = max_tiles

    def clear(self) -> None:
        self._tiles.clear()

    def _tile(self, source: ge.AudioSource, fpp: float, index: int, height: int, split: bool,
              argb: int, gain: float) -> QImage | None:
        key = (source.path, source.frames, source.sample_rate, round(fpp, 9), index, height, split, argb, gain)
        if key in self._tiles:
            self._tiles.move_to_end(key)
            return self._tiles[key]
        tile = render_tile(source, fpp, index, height, split, argb, gain)
        self._tiles[key] = tile
        while len(self._tiles) > self._max_tiles:
            self._tiles.popitem(last=False)
        return tile

    def draw(self, painter: QPainter, source: ge.AudioSource, body: QRectF, clip_x: float, offset_sec: float,
             frames_per_px: float, color: QColor, split_channels: bool, visible: QRectF, gain: float = 1.0) -> None:
        """Draw the waveform for a clip body, scaled by `gain` (linear). `clip_x`
        is the screen x of the clip start; the painter should already be clipped
        to the body."""
        x0 = max(body.left(), visible.left())
        x1 = min(body.right(), visible.right())
        if x1 <= x0 or frames_per_px <= 0:
            return
        u_offset = offset_sec * source.sample_rate / frames_per_px  # source pixels before the clip start
        first = int(math.floor((x0 - clip_x + u_offset) / TILE))
        last = int(math.floor((x1 - clip_x + u_offset) / TILE))
        height = int(body.height())
        argb = color.rgba()
        gain = round(gain, 2)  # (1 % steps look the same, and turning a gain knob renders fewer tiles)
        for index in range(max(0, first), last + 1):
            tile = self._tile(source, frames_per_px, index, height, split_channels, argb, gain)
            if tile is None:
                break
            x = clip_x + index * TILE - u_offset
            painter.drawImage(QPointF(round(x), body.top()), tile)
