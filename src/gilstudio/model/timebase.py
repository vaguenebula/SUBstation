"""Musical time helpers. Timeline positions are quarter-note beats."""

from __future__ import annotations

import math
from dataclasses import dataclass

VALID_DENOMINATORS = (1, 2, 4, 8, 16, 32)


@dataclass(frozen=True)
class TimeSignature:
    numerator: int = 4
    denominator: int = 4

    @property
    def beats_per_bar(self) -> float:
        """Bar length in quarter-note beats."""
        return self.numerator * 4.0 / self.denominator

    @property
    def beat_length(self) -> float:
        """Length of one signature beat (the denominator note) in quarter notes."""
        return 4.0 / self.denominator

    def __str__(self) -> str:
        return f"{self.numerator}/{self.denominator}"


def beats_to_seconds(beats: float, tempo: float) -> float:
    return beats * 60.0 / tempo


def seconds_to_beats(seconds: float, tempo: float) -> float:
    return seconds * tempo / 60.0


def split_position(beats: float, ts: TimeSignature) -> tuple[int, int, int]:
    """Zero-based (bar, beat, sixteenth) for a beat position."""
    beats = max(0.0, beats) + 1e-9
    bar = int(beats // ts.beats_per_bar)
    in_bar = beats - bar * ts.beats_per_bar
    beat = min(int(in_bar // ts.beat_length), ts.numerator - 1)
    in_beat = in_bar - beat * ts.beat_length
    sixteenth = int(in_beat // 0.25)
    return bar, beat, sixteenth


def format_position(beats: float, ts: TimeSignature) -> str:
    """Ableton-style 'bars.beats.sixteenths', one-based."""
    bar, beat, sixteenth = split_position(beats, ts)
    return f"{bar + 1}.{beat + 1}.{sixteenth + 1}"


def parse_position(text: str, ts: TimeSignature) -> float | None:
    """Inverse of format_position; missing parts default to 1. None if unparsable."""
    parts = [p for p in text.strip().replace(":", ".").split(".") if p]
    if not parts or len(parts) > 3:
        return None
    try:
        values = [int(p) for p in parts] + [1] * (3 - len(parts))
    except ValueError:
        return None
    bar, beat, sixteenth = values
    if bar < 1 or beat < 1 or sixteenth < 1:
        return None
    return (bar - 1) * ts.beats_per_bar + (beat - 1) * ts.beat_length + (sixteenth - 1) * 0.25


def is_multiple(value: float, step: float) -> bool:
    ratio = value / step
    return abs(ratio - round(ratio)) < 1e-6


def format_bar_label(beats: float, ts: TimeSignature) -> str:
    """Ruler label: '5' on a bar line, '5.3' on a beat, '5.3.2' below that."""
    bar, beat, sixteenth = split_position(beats, ts)
    if is_multiple(beats, ts.beats_per_bar):
        return str(bar + 1)
    if is_multiple(beats, ts.beat_length):
        return f"{bar + 1}.{beat + 1}"
    return f"{bar + 1}.{beat + 1}.{sixteenth + 1}"


def db_to_gain(db: float) -> float:
    return 0.0 if db <= -70.0 else 10.0 ** (db / 20.0)


def gain_to_db(gain: float) -> float:
    return -math.inf if gain <= 0.0 else 20.0 * math.log10(gain)


def format_db(db: float) -> str:
    return "-inf dB" if db <= -70.0 else f"{db:.1f} dB"


def format_pan(pan: float) -> str:
    value = round(pan * 50)
    if value == 0:
        return "C"
    return f"{abs(value)}{'L' if value < 0 else 'R'}"
