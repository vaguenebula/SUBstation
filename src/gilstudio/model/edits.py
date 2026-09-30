"""Pure clip-editing functions (no Qt, no undo): easy to test in isolation.

They edit audio and MIDI clips alike: each is a window onto its content (the
audio file, the notes), and editing moves the window's edges while the content
stays where it is on the timeline.
"""

from __future__ import annotations

import math
from dataclasses import replace

from .project import AnyClip, MidiClip, new_id

MIN_CLIP_SEC = 0.005
MIN_MIDI_CLIP_BEATS = 1 / 64
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


def _piece(clip: AnyClip, start: float, end: float, tempo: float, clip_id: str) -> AnyClip | None:
    """The part of `clip` between two beats, its content left in place on the
    timeline; None if that is too short to keep."""
    if isinstance(clip, MidiClip):
        if end - start < MIN_MIDI_CLIP_BEATS:
            return None
        return replace(clip, id=clip_id, start_beat=start, duration_beats=end - start,
                       offset_beats=clip.offset_beats + (start - clip.start_beat))
    duration = clip.beats_to_source(end - start, tempo)
    if duration < MIN_CLIP_SEC:
        return None
    return replace(clip, id=clip_id, start_beat=start, duration_sec=duration,
                   offset_sec=clip.offset_sec + clip.beats_to_source(start - clip.start_beat, tempo))


def resolve_overlaps(clips: list[AnyClip], winners: set[str], tempo: float) -> list[AnyClip]:
    """Ableton's overlap rule: clips in `winners` keep their place, and every other
    clip on the track is trimmed, split or removed where a winner covers it."""
    cuts = sorted((c.start_beat, c.end_beat(tempo)) for c in clips if c.id in winners)
    if not cuts:
        return sorted(clips, key=lambda c: c.start_beat)
    result: list[AnyClip] = []
    for clip in clips:
        if clip.id in winners:
            result.append(clip)
        else:
            result.extend(cut_clip(clip, cuts, tempo))
    return sorted(result, key=lambda c: c.start_beat)


def cut_clip(clip: AnyClip, cuts: list[tuple[float, float]], tempo: float) -> list[AnyClip]:
    """The parts of `clip` outside every cut (beat intervals): nothing, the clip
    itself, a trimmed clip, or pieces. The first piece keeps the clip's id; every
    piece still plays the same audio (or notes) at the same place on the timeline."""
    end = clip.end_beat(tempo)
    pieces = subtract_intervals(clip.start_beat, end, cuts)
    if pieces == [(clip.start_beat, end)]:
        return [clip]  # untouched: avoid float drift from recomputing it
    result: list[AnyClip] = []
    for a, b in pieces:
        piece = _piece(clip, a, b, tempo, new_id() if result else clip.id)
        if piece is not None:
            result.append(piece)
    return result


def remove_range(clips: list[AnyClip], start: float, end: float, tempo: float) -> list[AnyClip]:
    """Delete everything between two beats: whole clips inside go, clips across an
    edge are trimmed, and a clip spanning the range keeps its start and its end."""
    return sorted((piece for clip in clips for piece in cut_clip(clip, [(start, end)], tempo)),
                  key=lambda c: c.start_beat)


def fit_to_tempo(clips: list[AnyClip], tempo: float) -> list[AnyClip]:
    """Unwarped clips keep their length in seconds, so a faster tempo makes them
    longer in beats (and a warped clip grows when its segment BPM drops). Trim any
    clip that would run into the next one (the later clip keeps its place) so clips
    never overlap. Returns `clips` itself if nothing changes."""
    ordered = sorted(clips, key=lambda c: c.start_beat)
    result: list[AnyClip] = []
    changed = False
    for i, clip in enumerate(ordered):
        if i + 1 < len(ordered) and clip.end_beat(tempo) > ordered[i + 1].start_beat + EPS:
            changed = True
            clip = _piece(clip, clip.start_beat, ordered[i + 1].start_beat, tempo, clip.id)
            if clip is None:
                continue  # fully covered
        result.append(clip)
    return result if changed else clips


def slice_range(clips: list[AnyClip], start: float, end: float, tempo: float,
                keep_ids: bool = False) -> list[AnyClip]:
    """New clips (fresh ids) holding just the parts of `clips` between two beats.
    A clip wholly inside is taken as it is (with its own id if `keep_ids`)."""
    result = []
    for clip in clips:
        clip_end = clip.end_beat(tempo)
        if clip.start_beat >= start - EPS and clip_end <= end + EPS:
            if clip.start_beat < end and clip_end > start:
                result.append(clip if keep_ids else replace(clip, id=new_id()))
            continue
        piece = _piece(clip, max(start, clip.start_beat), min(end, clip_end), tempo, new_id())
        if piece is not None:
            result.append(piece)
    return result


def split_clip(clip: AnyClip, at_beat: float, tempo: float) -> tuple[AnyClip, AnyClip] | None:
    """Split into two clips at `at_beat`; None if the point is not inside the clip."""
    if isinstance(clip, MidiClip):
        left = _piece(clip, clip.start_beat, at_beat, tempo, clip.id)
        right = _piece(clip, at_beat, clip.end_beat(), tempo, new_id())
        return (left, right) if left and right else None
    left_sec = clip.beats_to_source(at_beat - clip.start_beat, tempo)
    if left_sec < MIN_CLIP_SEC or clip.duration_sec - left_sec < MIN_CLIP_SEC:
        return None
    left = replace(clip, duration_sec=left_sec)
    right = replace(clip, id=new_id(), start_beat=at_beat, offset_sec=clip.offset_sec + left_sec,
                    duration_sec=clip.duration_sec - left_sec)
    return left, right


def trim_start(clip: AnyClip, new_start_beat: float, tempo: float) -> AnyClip:
    """Move the left edge. The audio (or notes) stay in place on the timeline."""
    if isinstance(clip, MidiClip):
        end = clip.end_beat()
        start = max(0.0, min(new_start_beat, end - MIN_MIDI_CLIP_BEATS))
        offset = clip.offset_beats + (start - clip.start_beat)
        notes = clip.notes
        if offset < 0:
            # Revealing time before the first content beat: the content grows at
            # its start, so every note moves along to stay put on the timeline.
            notes = tuple(replace(n, start=n.start - offset) for n in notes)
            offset = 0.0
        return replace(clip, start_beat=start, duration_beats=end - start, offset_beats=offset, notes=notes)
    delta = clip.beats_to_source(new_start_beat - clip.start_beat, tempo)
    delta = max(delta, -clip.offset_sec)  # cannot reveal audio before the file starts
    delta = max(delta, -clip.beats_to_source(clip.start_beat, tempo))  # nor move before beat 0
    delta = min(delta, clip.duration_sec - MIN_CLIP_SEC)
    return replace(clip, start_beat=clip.start_beat + clip.source_to_beats(delta, tempo),
                   offset_sec=clip.offset_sec + delta, duration_sec=clip.duration_sec - delta)


def trim_end(clip: AnyClip, new_end_beat: float, tempo: float) -> AnyClip:
    """Move the right edge, limited by the end of the source file (MIDI clips can grow freely)."""
    if isinstance(clip, MidiClip):
        return replace(clip, duration_beats=max(MIN_MIDI_CLIP_BEATS, new_end_beat - clip.start_beat))
    duration = clip.beats_to_source(new_end_beat - clip.start_beat, tempo)
    available = clip.source_duration_sec - clip.offset_sec if clip.source_duration_sec > 0 else math.inf
    return replace(clip, duration_sec=max(MIN_CLIP_SEC, min(duration, available)))


def selection_span(clips: list[AnyClip], tempo: float) -> tuple[float, float]:
    return min(c.start_beat for c in clips), max(c.end_beat(tempo) for c in clips)
