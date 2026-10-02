"""Pure note-editing functions for the piano roll (no Qt, no undo)."""

from __future__ import annotations

import bisect
import random
from collections.abc import Iterable
from dataclasses import replace

from .project import Note

MIN_NOTE_BEATS = 1 / 64
EPS = 1e-9
# Quantize grids, as the piano roll offers them: (name, beats).
QUANTIZE_GRIDS = (("1/4", 1.0), ("1/8", 0.5), ("1/8T", 1 / 3), ("1/16", 0.25), ("1/16T", 1 / 6), ("1/32", 0.125))
# At 100 % humanize, how far a note may move (a 32nd note) and its velocity change.
HUMANIZE_BEATS = 0.125
HUMANIZE_VELOCITY = 24
NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
BLACK_KEYS = {1, 3, 6, 8, 10}


def note_name(pitch: int) -> str:
    """Ableton's octave numbering: note 60 (middle C) is C3."""
    return f"{NOTE_NAMES[pitch % 12]}{pitch // 12 - 2}"


def is_black_key(pitch: int) -> bool:
    return pitch % 12 in BLACK_KEYS


def by_time(note: Note) -> tuple[float, int]:
    """Sort key: by start, then pitch."""
    return note.start, note.pitch


def normalize(notes: Iterable[Note]) -> tuple[Note, ...]:
    """How a clip stores its notes: sorted by start, then pitch; no exact duplicates."""
    return tuple(sorted(set(notes), key=lambda n: (n.start, n.pitch, n.length, n.velocity)))


def span(notes: Iterable[Note]) -> tuple[float, float]:
    notes = list(notes)
    return min(n.start for n in notes), max(n.end for n in notes)


def resolve_overlaps(notes: list[Note], winners: set[Note]) -> list[Note]:
    """Notes in `winners` keep their place. Any other note on the same key that
    overlaps one is shortened to end where the winner starts, starts where the
    winner ends, or goes if it is covered (like clips on a track)."""
    by_pitch: dict[int, list[Note]] = {}
    for winner in sorted(winners, key=lambda n: n.start):
        by_pitch.setdefault(winner.pitch, []).append(winner)
    result = []
    for note in notes:
        if note in winners:
            result.append(note)
            continue
        for winner in by_pitch.get(note.pitch, ()):
            if winner.start >= note.end - EPS or winner.end <= note.start + EPS:
                continue
            if note.start < winner.start - EPS:
                note = replace(note, length=winner.start - note.start)
            elif note.end > winner.end + EPS:
                note = replace(note, start=winner.end, length=note.end - winner.end)
            else:
                note = None
                break
        if note is not None and note.length >= MIN_NOTE_BEATS - EPS:
            result.append(note)
    return result


def place(notes: Iterable[Note], removed: Iterable[Note], added: Iterable[Note]) -> tuple[Note, ...]:
    """A clip's notes with `removed` taken out and `added` put in; the added
    notes win where they overlap others."""
    removed, added = set(removed), list(added)
    kept = [n for n in notes if n not in removed]
    return normalize(resolve_overlaps(kept + added, set(added)))


def clamp_move(notes: Iterable[Note], delta_beats: float, delta_pitch: int) -> tuple[float, int]:
    """Limit a move of `notes` as a group: none before the content start or off the keyboard."""
    notes = list(notes)
    if not notes:
        return 0.0, 0
    delta_beats = max(delta_beats, -min(n.start for n in notes))
    delta_pitch = max(-min(n.pitch for n in notes), min(delta_pitch, 127 - max(n.pitch for n in notes)))
    return delta_beats, delta_pitch


def shifted(notes: Iterable[Note], delta_beats: float, delta_pitch: int) -> list[Note]:
    return [replace(n, start=n.start + delta_beats, pitch=n.pitch + delta_pitch) for n in notes]


def resized(notes: Iterable[Note], edge: str, delta: float, min_length: float = MIN_NOTE_BEATS) -> list[Note]:
    """Move each note's `edge` ("start" or "end") by `delta` beats, keeping at
    least `min_length`; a moved start keeps the note's end and stays at or after 0."""
    result = []
    for n in notes:
        if edge == "end":
            result.append(replace(n, length=max(min(min_length, n.length), n.length + delta)))
        else:
            start = max(0.0, min(n.start + delta, n.end - min(min_length, n.length)))
            result.append(replace(n, start=start, length=n.end - start))
    return result


def with_velocity(notes: Iterable[Note], delta: float) -> list[Note]:
    return [replace(n, velocity=max(1, min(127, round(n.velocity + delta)))) for n in notes]


def untangle(notes: Iterable[Note]) -> list[Note]:
    """Where notes on the same key overlap, the earlier one ends where the later
    starts (and goes if nothing is left of it); of two starting together, the
    longer stays. For notes changed together, which can't win against each other."""
    result = []
    ordered = sorted(notes, key=lambda n: (n.pitch, n.start, n.length))
    for i, note in enumerate(ordered):
        following = ordered[i + 1] if i + 1 < len(ordered) else None
        if following is not None and following.pitch == note.pitch and following.start < note.end - EPS:
            note = replace(note, length=following.start - note.start)
        if note.length >= MIN_NOTE_BEATS - EPS:
            result.append(note)
    return result


def legato(targets: Iterable[Note], clip_notes: Iterable[Note], end: float) -> list[Note]:
    """Lengthen or shorten each target note to last until the next target starts
    (a chord's notes all reach the next chord); the last ones reach the next note
    in the clip after them, or `end`, the clip's end, if there is none. A note
    never runs into the next note on its own key."""
    targets = list(targets)
    clip_notes = list(clip_notes)
    starts = sorted({n.start for n in targets})
    last = starts[-1] if starts else 0.0
    after_last = [n.start for n in clip_notes if n.start > last + EPS]
    last_stop = min(after_last, default=end)
    same_key: dict[int, list[float]] = {}
    for n in clip_notes:
        same_key.setdefault(n.pitch, []).append(n.start)
    result = []
    for n in targets:
        i = bisect.bisect_right(starts, n.start + EPS)
        stop = min([starts[i] if i < len(starts) else last_stop]
                   + [s for s in same_key.get(n.pitch, ()) if s > n.start + EPS])
        result.append(replace(n, length=stop - n.start) if stop - n.start >= MIN_NOTE_BEATS else n)
    return result


def time_scaled(notes: Iterable[Note], factor: float) -> list[Note]:
    """Stretch (factor 2) or squeeze (factor 0.5) the notes' timing: starts move
    away from or toward the earliest one, and lengths scale with them."""
    notes = list(notes)
    if not notes:
        return []
    origin = min(n.start for n in notes)
    return untangle(replace(n, start=origin + (n.start - origin) * factor,
                            length=max(MIN_NOTE_BEATS, n.length * factor)) for n in notes)


def quantized(notes: Iterable[Note], step: float, amount: float = 1.0) -> list[Note]:
    """Move each note's start toward the nearest multiple of `step` beats, all the
    way at `amount` 1, keeping its length."""
    return untangle(replace(n, start=max(0.0, n.start + (round(n.start / step) * step - n.start) * amount))
                    for n in notes)


def humanized(notes: Iterable[Note], rng: random.Random, amount: float) -> list[Note]:
    """Nudge each note's start and velocity at random, as a player would: at
    `amount` 1 by up to HUMANIZE_BEATS and HUMANIZE_VELOCITY either way, more
    often a little than a lot. Lengths stay."""
    result = []
    for n in notes:
        start = max(0.0, n.start + rng.triangular(-1.0, 1.0, 0.0) * amount * HUMANIZE_BEATS)
        velocity = round(n.velocity + rng.triangular(-1.0, 1.0, 0.0) * amount * HUMANIZE_VELOCITY)
        result.append(replace(n, start=start, velocity=max(1, min(127, velocity))))
    return untangle(result)
