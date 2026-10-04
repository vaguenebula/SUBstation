"""Operations freezing, unfreezing and flattening tracks. Freezing renders each
track (through the engine) before one undo step freezes them all, as Ctrl+Shift+F does."""

from __future__ import annotations

from typing import Annotated

from .context import OpContext, TrackId
from .errors import BUSY, OpError, invalid
from .registry import Doc, MinLen, Risk, operation

TrackIds = Annotated[list[TrackId], MinLen(1), Doc("Tracks' (groups', returns') ids.")]


def _owners(ctx: OpContext, track_ids) -> list[str]:
    p = ctx.project
    for track_id in track_ids:
        if not (p.has_track(track_id) or p.has_return(track_id)):
            ctx.owner(track_id)  # (not_found)
            raise invalid("Only tracks, groups and returns freeze, not the master")
    return list(dict.fromkeys(track_ids))


@operation(risk=Risk.EDIT, summary="Freeze tracks (groups, returns): each is rendered and plays its rendering, its "
                                   "devices unloaded, until unfrozen. Its clips, devices and their parameters can't "
                                   "change while frozen; its mixer and sends can.", label="Freeze Tracks")
def freeze_tracks(ctx: OpContext, track_ids: TrackIds) -> dict:
    p = ctx.project
    tracks = _owners(ctx, track_ids)
    tracks = [t for t in tracks if not (p.has_track(t) and any(a in tracks for a in p.ancestors(t)))]
    for track_id in tracks:
        if (problem := p.freeze_problem(track_id)) is not None:
            raise invalid(problem, track_id=track_id)
    if ctx.engine is None:
        raise invalid("Freezing renders in the engine, which isn't there")
    if ctx.engine.transport().recording:
        raise OpError(BUSY, "Stop recording to freeze tracks")
    freezes = {}
    try:
        for track_id in tracks:
            try:
                freezes[track_id] = ctx.engine.render_freeze(track_id)
            except (ValueError, OSError, RuntimeError) as exc:
                raise invalid(f"{p.track(track_id).name} could not be frozen: {exc}", track_id=track_id) from exc
        frozen = ctx.editor.freeze_tracks(freezes)
    except BaseException:
        for freeze in freezes.values():  # renders no track will play
            ctx.engine.discard_freeze(freeze)
        raise
    for track_id, freeze in freezes.items():
        if track_id not in frozen:
            ctx.engine.discard_freeze(freeze)
    return {"frozen": [ctx.ref(t) for t in frozen]}


@operation(risk=Risk.EDIT, summary="Unfreeze tracks (or the frozen groups they are in): their devices load again.",
           label="Unfreeze Tracks")
def unfreeze_tracks(ctx: OpContext, track_ids: TrackIds) -> dict:
    p = ctx.project
    holders = [h for t in _owners(ctx, track_ids) if (h := p.frozen_by(t)) is not None]
    thawed = ctx.editor.unfreeze_tracks(holders)
    return {"unfrozen": [ctx.ref(t) for t in thawed]}


@operation(risk=Risk.DESTRUCTIVE, summary="Flatten frozen audio and MIDI tracks: each becomes an audio track playing "
                                          "its frozen audio as a clip, without its devices or their automation.",
           label="Flatten Tracks")
def flatten_tracks(ctx: OpContext, track_ids: TrackIds) -> dict:
    p = ctx.project
    tracks = [ctx.track(t).id for t in dict.fromkeys(track_ids)]
    for track_id in tracks:
        if (problem := p.flatten_problem(track_id)) is not None:
            raise invalid(problem, track_id=track_id)
    flat = ctx.editor.flatten_tracks(tracks)
    return {"flattened": [ctx.ref(t) for t in flat]}
