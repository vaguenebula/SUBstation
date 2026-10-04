"""Operations on the project's settings (tempo, key, time signature, loop) and
the transport (play, stop, locate: not undo steps)."""

from __future__ import annotations

from typing import Annotated

from ...model.keys import key_from_name
from ...model.timebase import VALID_DENOMINATORS, TimeSignature
from ..context.song import bar_beat
from .context import Beat, OpContext
from .errors import invalid
from .registry import Doc, Ge, Le, MaxLen, Risk, operation


@operation(risk=Risk.EDIT, summary="Set the tempo (BPM, 20..999). Unwarped audio clips keep playing at their own "
                                   "speed; those that would then run into the next clip are trimmed.",
           label="Change Tempo", idempotent=True)
def set_tempo(ctx: OpContext, bpm: Annotated[float, Ge(20.0), Le(999.0)]) -> dict:
    ctx.editor.set_tempo(bpm)
    return {"tempo": ctx.project.tempo}


@operation(risk=Risk.EDIT, summary="Set the project's key ('F#m', 'Bb', ...; null: none). Audio added afterwards "
                                   "with a key in its file name is transposed to it.", label="Change Key",
           idempotent=True)
def set_key(ctx: OpContext, key: Annotated[str | None, MaxLen(8), Doc("A key name: 'C', 'F#m', 'Bb'...")]) -> dict:
    parsed = None
    if key is not None:
        parsed = key_from_name(key)
        if parsed is None:
            raise invalid(f"{key!r} isn't a key (write 'C', 'F#m', 'Bb'...)")
    ctx.editor.set_key(parsed)
    return {"key": parsed.name if parsed is not None else None}


@operation(risk=Risk.EDIT, summary="Set the time signature.", label="Change Time Signature", idempotent=True)
def set_time_signature(ctx: OpContext, numerator: Annotated[int, Ge(1), Le(32)],
                       denominator: Annotated[int, Doc("1, 2, 4, 8, 16 or 32.")]) -> dict:
    if denominator not in VALID_DENOMINATORS:
        raise invalid(f"A time signature's denominator is one of {', '.join(map(str, VALID_DENOMINATORS))}")
    ctx.editor.set_time_signature(TimeSignature(numerator, denominator))
    return {"time_signature": str(ctx.project.time_signature)}


@operation(risk=Risk.EDIT, summary="Set the loop: its start and end (beats) and whether it is on.", label="Change Loop",
           idempotent=True)
def set_loop(ctx: OpContext, start: Beat, end: Beat, enabled: bool = True) -> dict:
    if end <= start:
        raise invalid("The loop's end must be after its start")
    ctx.editor.set_loop(enabled, start, end)
    p = ctx.project
    return {"loop": {"enabled": p.loop_enabled, "start": p.loop_start, "end": p.loop_end}}


# --- Transport ------------------------------------------------------------------------------


def _engine(ctx: OpContext):
    if ctx.engine is None:
        raise invalid("There is no engine to play")
    return ctx.engine


def _transport(ctx: OpContext) -> dict:
    state = _engine(ctx).transport()
    p = ctx.project
    return {"playing": state.playing, "recording": state.recording, "position": state.position,
            "position_text": bar_beat(p, state.position), "tempo": p.tempo,
            "loop": {"enabled": p.loop_enabled, "start": p.loop_start, "end": p.loop_end}}


@operation(risk=Risk.READ, summary="The transport: playing, recording, where the playhead is, tempo and loop.")
def get_transport(ctx: OpContext) -> dict:
    return _transport(ctx)


@operation(risk=Risk.TRANSPORT, summary="Start playing from the playhead.", idempotent=True)
def play(ctx: OpContext) -> dict:
    _engine(ctx).play()
    return _transport(ctx)


@operation(risk=Risk.TRANSPORT, summary="Stop playing (and recording).", idempotent=True)
def stop(ctx: OpContext) -> dict:
    _engine(ctx).stop()
    return _transport(ctx)


@operation(risk=Risk.TRANSPORT, summary="Move the playhead to a beat.", idempotent=True)
def locate(ctx: OpContext, beat: Beat) -> dict:
    _engine(ctx).locate(beat)
    return _transport(ctx)
