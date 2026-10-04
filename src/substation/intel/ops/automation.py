"""Operations on automation: what can be automated, reading and writing
envelopes (values in the parameter's own units or normalized), clearing them,
re-enabling overridden automation, and shapes (ramp, swell, dip, step, lfo)
in one call.

Targets are the model's keys: 'mixer:volume', 'mixer:pan', 'send:<return id>',
'device:<device id>:<param id>', and a rack chain's 'device:<rack
id>:chain:<chain id>:volume'. get_automation_targets lists them, so callers
never build them by hand. Points are [beat, value, curve]."""

from __future__ import annotations

import math
from typing import Annotated, Literal

from ...model import automation
from ...model.automation import AutomationPoint
from ...model.devices import device_name
from ...model.params import mixer_specs
from ...model.project import iter_devices
from .context import Beat, OpContext, OwnerId, PointRow
from .devices import Units
from .errors import invalid
from .params import describe, device_specs, spec_for, to_normalized, value_at
from .registry import Doc, Ge, Le, MaxLen, MinLen, Risk, operation

Target = Annotated[str, MinLen(1), MaxLen(200), Doc("An automation target's key (get_automation_targets).")]
Points = Annotated[list[PointRow], MaxLen(10_000), Doc("Breakpoints [beat, value, curve], beats from the "
                                                       "timeline's start.")]
Curve = Annotated[float, Ge(-1.0), Le(1.0), Doc("How the segment bends: 0 straight, positive bulges up.")]

LFO_POINTS_PER_CYCLE = 8


def _check_lane(ctx: OpContext, owner: str, key: str) -> None:
    if ctx.editor.lane_frozen(owner, key):
        ctx.check_unfrozen(owner, "automation")


def _plain_points(spec, points) -> list[list[float]]:
    return [[p.beat, round(spec.from_normalized(p.value), 6), p.curve] for p in points]


@operation(risk=Risk.READ, summary="What a track (or 'master') can automate: its mixer, sends and every device "
                                   "parameter, with their keys, units, ranges, and whether each is automated.")
def get_automation_targets(ctx: OpContext, owner: OwnerId,
                           query: Annotated[str | None, MaxLen(64), Doc("Words the name must contain.")] = None
                           ) -> dict:
    track = ctx.owner(owner)
    sends = tuple((r.id, ctx.project.return_letter(r.id)) for r in ctx.project.send_targets(owner)) \
        if not track.is_master else ()
    groups = [("mixer", "Mixer", mixer_specs(master=track.is_master, sends=sends))]
    for device in iter_devices(track.devices):
        specs = device_specs(ctx, owner, device)
        groups.append((device.id, device_name(device), specs or []))
    words = (query or "").lower().split()
    result = []
    for group_id, name, specs in groups:
        listed = [s for s in specs if all(w in f"{s.name} {name}".lower() for w in words)]
        if listed:
            result.append({"group": group_id, "name": name,
                           "targets": [{"key": s.key, "name": s.name, "unit": s.unit, "min": s.minimum,
                                        "max": s.maximum, **({"steps": s.steps} if s.steps else {}),
                                        **({"automated": True} if ctx.project.envelope(owner, s.key) else {})}
                                       for s in listed]})
    return {"owner": ctx.ref(owner), "groups": result}


@operation(risk=Risk.READ, summary="An envelope's breakpoints, in the target's plain units (or normalized).")
def get_automation(ctx: OpContext, owner: OwnerId, target: Target, units: Units = "plain") -> dict:
    spec = spec_for(ctx, owner, target)
    points = ctx.project.envelope(owner, target)
    rows = _plain_points(spec, points) if units == "plain" else [[p.beat, p.value, p.curve] for p in points]
    return {"owner": ctx.ref(owner), "target": describe(ctx, owner, spec), "points": rows,
            "overridden": bool(ctx.engine is not None and ctx.engine.is_overridden(owner, target))}


@operation(risk=Risk.EDIT, summary="Write an envelope: the whole of it, or (with start and end) only that stretch, "
                                   "the rest staying as it was. Values in the target's plain units (dB, Hz, %, a "
                                   "plug-in's 0..1) or normalized.", label="Write Automation")
def write_automation(ctx: OpContext, owner: OwnerId, target: Target, points: Points, units: Units = "plain",
                     start: Annotated[Beat | None, Doc("Replace only from here...")] = None,
                     end: Annotated[Beat | None, Doc("...to here (both or neither).")] = None) -> dict:
    spec = spec_for(ctx, owner, target)
    _check_lane(ctx, owner, target)
    new = []
    for i, row in enumerate(points):
        beat, value = row[0], row[1]
        curve = row[2] if len(row) > 2 else 0.0
        if beat < 0 or not -1.0 <= curve <= 1.0:
            raise invalid(f"points[{i}]: a beat is at least 0, a curve -1..1")
        new.append(AutomationPoint(beat, to_normalized(spec, value, units), curve))
    if (start is None) != (end is None):
        raise invalid("Give both start and end, or neither")
    if start is None:
        envelope = automation.normalize(new)
    else:
        if end <= start:
            raise invalid("start must be before end")
        if any(not start <= p.beat <= end for p in new):
            raise invalid("Every point must lie between start and end")
        content = tuple(AutomationPoint(p.beat - start, p.value, p.curve) for p in sorted(new, key=lambda p: p.beat))
        envelope = _paste(ctx, owner, target, spec, content, start, end, restore=True)
    ctx.editor.set_envelope(owner, target, envelope, "Write Automation")
    return _result(ctx, owner, target, spec)


def _paste(ctx: OpContext, owner: str, key: str, spec, content, start: float, end: float, restore: bool):
    """`content` (points from beat 0) over start..end of the envelope. On a target
    with no automation yet, the envelope is first seeded with its value now at
    both edges (only at the start, without `restore`), so that before and after
    the range it keeps that value rather than the content's ends."""
    current = ctx.project.envelope(owner, key)
    if not current:
        own = value_at(ctx, owner, key, spec, start)
        current = (AutomationPoint(start, own), AutomationPoint(end, own)) if restore else \
            (AutomationPoint(start, own),)
        if not restore and content:
            current = (AutomationPoint(start, own), AutomationPoint(end, content[-1].value))
    pasted = automation.paste_range(current, content, start, end - start)
    return automation.drop_redundant(pasted, (start, end))


def _result(ctx: OpContext, owner: str, key: str, spec) -> dict:
    points = ctx.project.envelope(owner, key)
    return {"owner": ctx.ref(owner), "target": key, "name": spec.name, "points": _plain_points(spec, points)}


@operation(risk=Risk.EDIT, summary="Delete an envelope, or only its automation between start and end (the "
                                   "envelope then runs straight across).", label="Delete Automation")
def clear_automation(ctx: OpContext, owner: OwnerId, target: Target, start: Beat | None = None,
                     end: Beat | None = None) -> dict:
    spec = spec_for(ctx, owner, target)
    _check_lane(ctx, owner, target)
    if (start is None) != (end is None):
        raise invalid("Give both start and end, or neither")
    if start is None:
        ctx.editor.clear_envelope(owner, target)
    else:
        if end <= start:
            raise invalid("start must be before end")
        ctx.editor.delete_automation_range(start, end, [(owner, target)])
    return _result(ctx, owner, target, spec)


@operation(risk=Risk.EDIT, summary="Let automation play again where setting a value by hand overrode it (one "
                                   "track's, or everywhere). Not an undo step.", undoable=False)
def re_enable_automation(ctx: OpContext, owner: Annotated[str | None, Doc("A track's id or 'master'; null: "
                                                                          "everywhere.")] = None) -> dict:
    if owner is not None:
        ctx.owner(owner)
    if ctx.engine is None:
        raise invalid("Automation plays in the engine, which isn't there")
    ctx.engine.re_enable_automation(owner)
    return {}


@operation(risk=Risk.EDIT, summary="Draw a shape over start..end of a target's automation, as one undo step: "
                                   "'ramp' (from -> to), 'swell' (rises from -> to, then snaps back at end), 'dip' "
                                   "(from -> to at the middle -> from), 'step' (to, over the range), 'lfo' "
                                   "(between from and to, `period` beats a cycle). Values in plain units (or "
                                   "normalized); `from` defaults to the value at start. With `restore` (the "
                                   "default) the target keeps its value outside the range, even if it had no "
                                   "automation yet.")
def automate_shape(ctx: OpContext, owner: OwnerId, target: Target, start: Beat, end: Beat,
                   shape: Literal["ramp", "swell", "dip", "step", "lfo"],
                   to: Annotated[float, Doc("The value the shape goes to.")],
                   from_value: Annotated[float | None, Doc("The value it starts from; null: the value at start.")]
                   = None,
                   curve: Curve = 0.0, units: Units = "plain", restore: bool = True,
                   period: Annotated[float, Ge(1 / 64), Le(1024.0), Doc("lfo: beats a cycle.")] = 1.0) -> dict:
    spec = spec_for(ctx, owner, target)
    _check_lane(ctx, owner, target)
    if end <= start:
        raise invalid("start must be before end")
    length = end - start
    high = to_normalized(spec, to, units)
    low = value_at(ctx, owner, target, spec, start) if from_value is None else to_normalized(spec, from_value, units)
    content = shape_points(shape, low, high, length, curve, period)
    envelope = _paste(ctx, owner, target, spec, content, start, end, restore)
    ctx.editor.set_envelope(owner, target, envelope, "Automate Shape")
    return _result(ctx, owner, target, spec)


def shape_points(shape: str, low: float, high: float, length: float, curve: float = 0.0,
                 period: float = 1.0) -> tuple[AutomationPoint, ...]:
    """A shape's points from beat 0 to `length` (normalized values)."""
    P = AutomationPoint
    if shape == "ramp":
        return (P(0.0, low, curve), P(length, high))
    if shape == "swell":
        return (P(0.0, low, curve), P(length, high), P(length, low))
    if shape == "dip":
        return (P(0.0, low, curve), P(length / 2, high, -curve), P(length, low))
    if shape == "step":
        return (P(0.0, high), P(length, high))
    if shape == "lfo":
        cycles = length / period
        count = max(2, math.ceil(cycles * LFO_POINTS_PER_CYCLE))
        middle, swing = (low + high) / 2, (high - low) / 2
        return tuple(P(length * i / count, middle - swing * math.cos(2 * math.pi * cycles * i / count))
                     for i in range(count + 1))
    raise invalid(f"no shape {shape!r}")
