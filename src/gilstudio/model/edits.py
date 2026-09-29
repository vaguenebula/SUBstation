"""Pure clip-editing functions (no Qt, no undo): easy to test in isolation."""

from __future__ import annotations

import math
from dataclasses import replace

from .project import Clip, new_id
from .timebase import beats_to_seconds, seconds_to_beats

MIN_CLIP_SEC = 0.005
EPS = 1e-9


def subtract_intervals(start: float, end: float, cuts: list[tuple[float, float]]) -> list[tuple[float, float]]:
    """The parts of [start, end) not covered by any cut interval."""
    pieces = [(start, end)]
    for cut_start, cut_end in cuts:
        next_pieces = []
        for a, b in pieces:
            if cut_end <= a + EPS or cut_start >= b - EPS:
                next_pieces.append((a, b))
                continue
            if cut_start > a + EPS:
                next_pieces.append((a, cut_start))
            if cut_end < b - EPS:
                next_pieces.append((cut_end, b))
        pieces = next_pieces
    return pieces


def resolve_overlaps(clips: list[Clip], winners: set[str], tempo: float) -> list[Clip]:
    """Ableton's overlap rule: clips in `winners` keep their place, and every other
    clip on the track is trimmed, split or removed where a winner covers it."""
    cuts = sorted((c.start_beat, c.end_beat(tempo)) for c in clips if c.id in winners)
    if not cuts:
        return sorted(clips, key=lambda c: c.start_beat)
    result: list[Clip] = []
    for clip in clips:
        if clip.id in winners:
            result.append(clip)
            continue
        kept_original_id = False
        for a, b in subtract_intervals(clip.start_beat, clip.end_beat(tempo), cuts):
            duration = beats_to_seconds(b - a, tempo)
            if duration < MIN_CLIP_SEC:
                continue
            result.append(replace(
                clip,
                id=new_id() if kept_original_id else clip.id,
                start_beat=a,
                offset_sec=clip.offset_sec + beats_to_seconds(a - clip.start_beat, tempo),
                duration_sec=duration,
            ))
            kept_original_id = True
    return sorted(result, key=lambda c: c.start_beat)


def split_clip(clip: Clip, at_beat: float, tempo: float) -> tuple[Clip, Clip] | None:
    """Split into two clips at `at_beat`; None if the point is not inside the clip."""
    left_sec = beats_to_seconds(at_beat - clip.start_beat, tempo)
    if left_sec < MIN_CLIP_SEC or clip.duration_sec - left_sec < MIN_CLIP_SEC:
        return None
    left = replace(clip, duration_sec=left_sec)
    right = replace(clip, id=new_id(), start_beat=at_beat, offset_sec=clip.offset_sec + left_sec,
                    duration_sec=clip.duration_sec - left_sec)
    return left, right


def trim_start(clip: Clip, new_start_beat: float, tempo: float) -> Clip:
    """Move the left edge. The audio stays in place on the timeline."""
    delta = beats_to_seconds(new_start_beat - clip.start_beat, tempo)
    delta = max(delta, -clip.offset_sec)  # cannot reveal audio before the file starts
    delta = max(delta, -beats_to_seconds(clip.start_beat, tempo))  # nor move before beat 0
    delta = min(delta, clip.duration_sec - MIN_CLIP_SEC)
    return replace(clip, start_beat=clip.start_beat + seconds_to_beats(delta, tempo),
                   offset_sec=clip.offset_sec + delta, duration_sec=clip.duration_sec - delta)


def trim_end(clip: Clip, new_end_beat: float, tempo: float) -> Clip:
    """Move the right edge, limited by the end of the source file."""
    duration = beats_to_seconds(new_end_beat - clip.start_beat, tempo)
    available = clip.source_duration_sec - clip.offset_sec if clip.source_duration_sec > 0 else math.inf
    return replace(clip, duration_sec=max(MIN_CLIP_SEC, min(duration, available)))


def selection_span(clips: list[Clip], tempo: float) -> tuple[float, float]:
    return min(c.start_beat for c in clips), max(c.end_beat(tempo) for c in clips)
