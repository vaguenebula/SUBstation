"""Pure note-editing functions for the piano roll (no Qt, no undo)."""

from __future__ import annotations

from collections.abc import Iterable
from dataclasses import replace

from .project import Note

MIN_NOTE_BEATS = 1 / 64
EPS = 1e-9
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
