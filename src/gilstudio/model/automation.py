"""Automation envelopes: breakpoints that move a parameter over time.

Pure functions on immutable envelopes (tuples of AutomationPoint), shared by the
model, the editor, the UI and the engine bridge.

- Every target is automated the same way, in normalized values (0..1): a
  device's parameters (see params.py), and the mixer's volume and pan of a track
  or the master. The engine plays the same envelopes (engine/src/Automation.h);
  the shape of curved segments and the mixer's mappings here must match it.
- An envelope belongs to an owner, a track id or MASTER, and is keyed by its
  target: MIXER_VOLUME, MIXER_PAN, send_key(return id) (the level of the
  owner's send to a return track), or device_key(device id, parameter id).
  Device ids are unique in the project, so a key keeps pointing at its device
  wherever the device sits (in future, inside a rack). A send's level maps as
  a volume does.
- Before its first breakpoint an envelope holds the first one's value, after the
  last the last one's. Breakpoints may share a beat (a step): from that beat on,
  the later one's value counts.
- Each breakpoint's `curve` (-1..1) bends the segment that starts at it: 0 is a
  straight line, positive bulges upward, negative downward, whichever way the
  segment goes. Segments are exponential, so splitting one at any point gives two
  segments of the same kind that together follow it exactly.
"""

from __future__ import annotations

import bisect
import math
from collections.abc import Iterable, Sequence
from dataclasses import dataclass, replace

CURVATURE = 6.0  # engine: kAutomationCurvature
MASTER = "master"  # the master's automation owner id
MIXER_VOLUME = "mixer:volume"
MIXER_PAN = "mixer:pan"
MIXER_KEYS = (MIXER_VOLUME, MIXER_PAN)
SEND_PREFIX = "send:"

# The faders' range. On a volume lane the gain is the cube of the value, so the
# lane's 1 is +6 dB, 0 dB sits at about 0.79 and 0 is silence (engine: kMaxVolumeGain).
MAX_VOLUME_DB = 6.0
MIN_VOLUME_DB = -70.0  # the faders' floor: at or below it is silence
_VOLUME_FLOOR = 10.0 ** ((MIN_VOLUME_DB - MAX_VOLUME_DB) / 60.0)


@dataclass(frozen=True)
class AutomationPoint:
    beat: float
    value: float  # normalized, 0..1
    curve: float = 0.0  # how the segment to the next point bends, -1..1


Envelope = tuple[AutomationPoint, ...]


@dataclass(frozen=True)
class AutomationView:
    """How an owner's automation shows in the arrangement: whether it does, the
    target shown in its main lane (None: none chosen yet), and the targets of the
    lanes shown below it."""

    shown: bool = False
    key: str | None = None
    lanes: tuple[str, ...] = ()


# --- Keys ------------------------------------------------------------------------------


def device_key(device_id: str, param_id: str) -> str:
    return f"device:{device_id}:{param_id}"


def send_key(return_id: str) -> str:
    return f"{SEND_PREFIX}{return_id}"


def parse_key(key: str) -> tuple[str, ...]:
    """('mixer', 'volume' / 'pan'), ('send', return id) or ('device', device id, parameter id)."""
    kind, _, rest = key.partition(":")
    if kind == "mixer" and key in MIXER_KEYS:
        return kind, rest
    if kind == "send" and rest:
        return kind, rest
    if kind == "device":
        device_id, sep, param_id = rest.partition(":")
        if device_id and sep:
            return kind, device_id, param_id
    raise ValueError(f"not an automation target: {key!r}")


def is_key(key: str) -> bool:
    try:
        parse_key(key)
    except ValueError:
        return False
    return True


def key_device(key: str) -> str | None:
    """The device a key targets (None for the mixer and sends)."""
    parts = parse_key(key)
    return parts[1] if parts[0] == "device" else None


def key_send(key: str) -> str | None:
    """The return track whose send a key targets (None for anything else)."""
    return (key[len(SEND_PREFIX):] or None) if key.startswith(SEND_PREFIX) else None


def is_mixer_key(key: str) -> bool:
    """Whether a key targets the owner's mixer: its volume, pan or a send (not a device)."""
    return key in MIXER_KEYS or key_send(key) is not None


# --- Mixer mappings ----------------------------------------------------------------------


def volume_to_normalized(db: float) -> float:
    if db <= MIN_VOLUME_DB:
        return 0.0
    return min(1.0, 10.0 ** ((db - MAX_VOLUME_DB) / 60.0))


def normalized_to_volume(value: float) -> float:
    """dB; MIN_VOLUME_DB (silence) at the bottom of the lane."""
    if value <= _VOLUME_FLOOR:
        return MIN_VOLUME_DB
    return MAX_VOLUME_DB + 60.0 * math.log10(min(1.0, value))


def pan_to_normalized(pan: float) -> float:
    return min(1.0, max(0.0, (pan + 1.0) / 2.0))


def normalized_to_pan(value: float) -> float:
    return min(1.0, max(0.0, value)) * 2.0 - 1.0


# --- Evaluating ------------------------------------------------------------------------


def shape(x: float, bend: float) -> float:
    """How far a segment bent by `bend` has come at x (0..1): positive bends
    move early and level off."""
    a = -bend * CURVATURE
    if abs(a) < 1e-4:
        return x
    return math.expm1(a * x) / math.expm1(a)


def _bend(start: AutomationPoint, end: AutomationPoint) -> float:
    return start.curve if end.value >= start.value else -start.curve


def segment_value(start: AutomationPoint, end: AutomationPoint, beat: float) -> float:
    span = end.beat - start.beat
    if span <= 0:
        return end.value
    x = min(1.0, max(0.0, (beat - start.beat) / span))
    return start.value + (end.value - start.value) * shape(x, _bend(start, end))


def count_at_or_before(points: Sequence[AutomationPoint], beat: float) -> int:
    return bisect.bisect_right([p.beat for p in points], beat)


def value_at(points: Sequence[AutomationPoint], beat: float) -> float | None:
    """The envelope's value at `beat`; None if it has no points."""
    if not points:
        return None
    index = count_at_or_before(points, beat)
    if index == 0:
        return points[0].value
    if index >= len(points):
        return points[-1].value
    return segment_value(points[index - 1], points[index], beat)


def left_value(points: Sequence[AutomationPoint], beat: float) -> float | None:
    """The value just before `beat` (where a step at `beat` hasn't happened yet)."""
    if not points:
        return None
    index = bisect.bisect_left([p.beat for p in points], beat)
    if index == 0:
        return points[0].value
    if index >= len(points):
        return points[-1].value
    return segment_value(points[index - 1], points[index], beat)


def sample(points: Sequence[AutomationPoint], start: float, end: float, count: int) -> list[tuple[float, float]]:
    """(beat, value) at `count` evenly spaced beats from start to end (for drawing)."""
    if count < 2 or not points:
        return []
    step = (end - start) / (count - 1)
    return [(start + i * step, value_at(points, start + i * step)) for i in range(count)]


# --- Editing (each returns a new envelope) ------------------------------------------------


def _clamp(value: float, low: float = 0.0, high: float = 1.0) -> float:
    return min(high, max(low, value))


def normalize(points: Iterable[AutomationPoint]) -> Envelope:
    """Sorted by beat (points at the same beat keep their order), within range."""
    cleaned = [AutomationPoint(max(0.0, float(p.beat)), _clamp(float(p.value)), _clamp(float(p.curve), -1.0, 1.0))
               for p in points]
    return tuple(sorted(cleaned, key=lambda p: p.beat))


def split_at(points: Sequence[AutomationPoint], beat: float) -> tuple[Envelope, int]:
    """Add a point at `beat` without changing the envelope (a curved segment is
    split into two that follow it exactly). Returns the envelope and the new
    point's index (after any points already at that beat)."""
    points = list(points)
    index = count_at_or_before(points, beat)
    value = value_at(points, beat)
    if value is None:
        raise ValueError("an empty envelope has nothing to split")
    curve = 0.0
    if 0 < index < len(points):
        before, after = points[index - 1], points[index]
        share = (beat - before.beat) / (after.beat - before.beat)
        points[index - 1] = replace(before, curve=before.curve * share)
        curve = before.curve * (1.0 - share)
    points.insert(index, AutomationPoint(beat, value, curve))
    return tuple(points), index


def add_point(points: Sequence[AutomationPoint], beat: float, value: float) -> tuple[Envelope, int]:
    """A new point, at `value`. Returns the envelope and the point's index."""
    beat = max(0.0, beat)
    if not points:
        return (AutomationPoint(beat, _clamp(value)),), 0
    split, index = split_at(points, beat)
    return split[:index] + (replace(split[index], value=_clamp(value)),) + split[index + 1:], index


def move_points(points: Sequence[AutomationPoint], indices: Iterable[int], delta_beats: float,
                delta_value: float) -> Envelope:
    return move_points_mapped(points, indices, delta_beats, delta_value)[0]


def move_points_mapped(points: Sequence[AutomationPoint], indices: Iterable[int], delta_beats: float,
                       delta_value: float) -> tuple[Envelope, dict[int, int]]:
    """Move points together in time and value; returns the envelope and where
    each moved point is now ({old index: new index}). One point stays between
    its neighbours (and after 0). Several override what they land on: the
    points between the first and the last of them go."""
    chosen = sorted({i for i in indices if 0 <= i < len(points)})
    if not chosen:
        return tuple(points), {}
    moving = set(chosen)
    if len(chosen) == 1:
        i = chosen[0]
        before = next((points[j].beat for j in range(i - 1, -1, -1) if j not in moving), 0.0)
        after = next((points[j].beat for j in range(i + 1, len(points)) if j not in moving), math.inf)
        delta_beats = min(after - points[i].beat, max(before - points[i].beat, delta_beats))
        moved = tuple(replace(p, beat=p.beat + delta_beats, value=_clamp(p.value + delta_value)) if j == i else p
                      for j, p in enumerate(points))
        return moved, {i: i}
    delta_beats = max(delta_beats, -min(points[i].beat for i in chosen))
    low, high = points[chosen[0]].beat + delta_beats, points[chosen[-1]].beat + delta_beats
    # (sort key, old index, point): kept points at the edges stay on their side.
    entries = []
    for j, p in enumerate(points):
        if j in moving:
            entries.append(((p.beat + delta_beats, 1), j, replace(p, beat=p.beat + delta_beats,
                                                                  value=_clamp(p.value + delta_value))))
        elif not low < p.beat < high:
            side = 0 if p.beat < low or (p.beat == low and j < chosen[0]) else 2
            if p.beat == low == high:
                side = 0 if j < chosen[0] else 2
            entries.append(((p.beat, side), j, p))
    entries.sort(key=lambda e: e[0])  # stable: moved points keep their order
    return tuple(e[2] for e in entries), {j: n for n, (_, j, _) in enumerate(entries) if j in moving}


def delete_points(points: Sequence[AutomationPoint], indices: Iterable[int]) -> Envelope:
    gone = set(indices)
    return tuple(p for i, p in enumerate(points) if i not in gone)


def set_curve(points: Sequence[AutomationPoint], index: int, curve: float) -> Envelope:
    if not 0 <= index < len(points):
        return tuple(points)
    return tuple(replace(p, curve=_clamp(curve, -1.0, 1.0)) if i == index else p for i, p in enumerate(points))


def segment_index(points: Sequence[AutomationPoint], beat: float) -> int | None:
    """The point starting the segment that runs through `beat` (None before the
    first point or after the last: no segment to bend there)."""
    index = count_at_or_before(points, beat)
    if index == 0 or index >= len(points):
        return None
    return index - 1


def _simplify(points: Sequence[AutomationPoint]) -> Envelope:
    """Drops points that repeat the one before them (same beat and value)."""
    out: list[AutomationPoint] = []
    for p in points:
        if out and out[-1].beat == p.beat and out[-1].value == p.value:
            out[-1] = p
        else:
            out.append(p)
    return tuple(out)


def _first_at(points: Sequence[AutomationPoint], beat: float) -> AutomationPoint:
    """The first point at `beat`: where the envelope arrives there from before."""
    return next(p for p in points if p.beat == beat)


def _last_at(points: Sequence[AutomationPoint], beat: float) -> AutomationPoint:
    """The last point at `beat`: where the envelope leaves from there."""
    return next(p for p in reversed(points) if p.beat == beat)


def _edges(points: Sequence[AutomationPoint], start: float, end: float) -> Envelope:
    """The envelope with points at `start` and `end` (it doesn't change)."""
    points, _ = split_at(points, start)
    points, _ = split_at(points, end)
    return points


def remove_range(points: Sequence[AutomationPoint], start: float, end: float) -> Envelope:
    """Delete the automation between two beats: the envelope outside stays as it
    was, and runs straight across the range. Deleting every point deletes the
    envelope."""
    if not points or end <= start or not any(start <= p.beat <= end for p in points):
        return tuple(points)
    if all(start <= p.beat <= end for p in points):
        return ()
    edged = _edges(points, start, end)
    before = [p for p in edged if p.beat < start] + [replace(_first_at(edged, start), curve=0.0)]
    after = [_last_at(edged, end)] + [p for p in edged if p.beat > end]
    return _simplify(before + after)


def copy_range(points: Sequence[AutomationPoint], start: float, end: float) -> Envelope:
    """The envelope from start to end, as points from beat 0."""
    if not points or end <= start:
        return ()
    edged = _edges(points, start, end)
    inside = [_last_at(edged, start)] + [p for p in edged if start < p.beat < end] + [_first_at(edged, end)]
    return tuple(replace(p, beat=p.beat - start) for p in inside)


def paste_range(points: Sequence[AutomationPoint], content: Sequence[AutomationPoint], start: float,
                length: float) -> Envelope:
    """Put `content` (points from beat 0, `length` long) at `start`, replacing
    what was there; the envelope outside stays as it was."""
    end = start + length
    shifted = [replace(p, beat=p.beat + start) for p in content if p.beat <= length]
    if not points:
        return normalize(shifted)
    edged = _edges(points, start, end)
    before = [p for p in edged if p.beat < start] + [_first_at(edged, start)]
    after = [_last_at(edged, end)] + [p for p in edged if p.beat > end]
    return _simplify(before + shifted + after)


def _redundant(points: Sequence[AutomationPoint], i: int) -> bool:
    """Whether the envelope stays the same without point i."""
    p = points[i]
    before = points[i - 1] if i > 0 else None
    after = points[i + 1] if i + 1 < len(points) else None
    if before is None or after is None:
        other = before or after
        return other is not None and other.value == p.value and other.beat != p.beat
    if before.beat == p.beat or p.beat == after.beat:
        return False  # part of a step
    if before.value == p.value == after.value:
        return True
    if before.curve or p.curve:
        return False
    on_line = before.value + (after.value - before.value) * (p.beat - before.beat) / (after.beat - before.beat)
    return abs(on_line - p.value) < 1e-9


def drop_redundant(points: Sequence[AutomationPoint], beats: Iterable[float]) -> Envelope:
    """Without the points at these beats that don't change the envelope (the
    edges range edits add where the envelope is flat or straight)."""
    beats = set(beats)
    out = list(points)
    i = 0
    while i < len(out):
        if out[i].beat in beats and _redundant(out, i):
            del out[i]
        else:
            i += 1
    return tuple(out)


def move_range(points: Sequence[AutomationPoint], start: float, end: float, delta_beats: float,
               delta_value: float) -> Envelope:
    """Move the envelope between two beats up or down (`delta_value`) and in
    time (`delta_beats`, over what is where it lands; where it was, the envelope
    runs straight across). Breakpoints at the range's edges keep the envelope
    outside it as it was: moved up or down, it steps there."""
    if not points or end <= start:
        return tuple(points)
    delta_beats = max(delta_beats, -start)
    content = tuple(replace(p, value=_clamp(p.value + delta_value)) for p in copy_range(points, start, end))
    base = tuple(points)
    if delta_beats:  # where it was, straight across (keeping the envelope outside, even if all of it moves)
        edged = _edges(points, start, end)
        base = _simplify([p for p in edged if p.beat < start] + [replace(_first_at(edged, start), curve=0.0)]
                         + [_last_at(edged, end)] + [p for p in edged if p.beat > end])
    moved = paste_range(base, content, start + delta_beats, end - start)
    return drop_redundant(moved, (start, end, start + delta_beats, end + delta_beats))


def has_points_in(points: Sequence[AutomationPoint], start: float, end: float) -> bool:
    return any(start <= p.beat <= end for p in points)


def merge_spans(spans: Iterable[tuple[float, float]]) -> list[tuple[float, float]]:
    """Beat ranges, sorted, with those that overlap or touch joined."""
    merged: list[tuple[float, float]] = []
    for start, end in sorted(spans):
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return merged


def shift(points: Sequence[AutomationPoint], delta_beats: float) -> Envelope:
    return normalize(replace(p, beat=p.beat + delta_beats) for p in points)
