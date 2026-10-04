"""Operations on clips: reading one, adding audio from a file, moving, setting
up and deleting clips."""

from __future__ import annotations

from dataclasses import replace
from typing import Annotated, Literal

from ...model.project import WARP_MODES, AnyClip, Clip, MidiClip
from .context import Beat, ClipId, OpContext, TrackId
from .errors import invalid
from .registry import Doc, Ge, Le, MaxLen, MinLen, Risk, operation

ClipIds = Annotated[list[ClipId], MinLen(1), Doc("Clips' ids.")]
WarpMode = Literal["Transients", "Standard", "Smooth", "Formants", "Re-Pitch"]
assert list(WarpMode.__args__) == WARP_MODES


def clip_dict(ctx: OpContext, track_id: str, clip: AnyClip, notes: bool = False) -> dict:
    """A clip as results show it. A MIDI clip's notes (with `notes`) are
    [pitch, start, length, velocity], starts in beats from the clip's start."""
    tempo = ctx.project.tempo
    d = {"id": clip.id, "track_id": track_id, "name": clip.name, "start": clip.start_beat,
         "end": clip.end_beat(tempo)}
    if isinstance(clip, MidiClip):
        d["kind"] = "midi"
        if notes:
            d["notes"] = [[n.pitch, round(n.start - clip.offset_beats, 6), n.length, n.velocity] for n in clip.notes]
        else:
            d["note_count"] = len(clip.played_notes())
    else:
        d |= {"kind": "audio", "path": clip.path, "gain_db": clip.gain_db, "warp": clip.warp,
              "warp_mode": clip.warp_mode, "segment_bpm": clip.segment_bpm, "transpose": clip.transpose,
              "detune": clip.detune, "pan": clip.pan}
    return d


@operation(risk=Risk.READ, summary="A clip: where it is, its settings, and a MIDI clip's notes "
                                   "([pitch, start, length, velocity], starts in beats from the clip's start).")
def get_clip(ctx: OpContext, clip_id: ClipId) -> dict:
    track_id, clip = ctx.find_clip(clip_id)
    return {"clip": clip_dict(ctx, track_id, clip, notes=True)}


@operation(risk=Risk.FILES, summary="Put an audio file on a track at a beat, as dropping it does (a tempo or key in "
                                    "its name sets up warping and transposition). No track (or a MIDI one): a new "
                                    "audio track. The file must be in a browser place or the project's folder.",
           label="Add Clip")
def add_clip_from_file(ctx: OpContext, path: Annotated[str, MinLen(1), MaxLen(1024), Doc("The file's full path.")],
                       start_beat: Beat = 0.0, track_id: TrackId | None = None) -> dict:
    if track_id is not None:
        ctx.track(track_id)
        ctx.check_unfrozen(track_id, "clips")
    file = ctx.check_path(path)
    duration = ctx.engine.file_duration(str(file)) if ctx.engine is not None else None
    if duration is None or duration <= 0:
        raise invalid(f"{file.name} can't be read as audio")
    refs = ctx.editor.add_clips(track_id, start_beat, [(str(file), duration)],
                                track_index=len(ctx.project.tracks) if track_id is None else None)
    if not refs:
        raise invalid(f"{file.name} couldn't be added")
    (tid, cid), = refs
    return {"track": ctx.ref(tid), "clip": clip_dict(ctx, tid, ctx.project.clip(tid, cid))}


@operation(risk=Risk.EDIT, summary="Move (or copy) clips in time and across tracks (onto tracks of their kind "
                                   "only). What they land on is cut away, as when dragging them.")
def move_clips(ctx: OpContext, clip_ids: ClipIds,
               delta_beats: Annotated[float, Doc("How far in beats (negative: earlier).")],
               track_delta: Annotated[int, Doc("How many tracks down (negative: up).")] = 0,
               copy: bool = False) -> dict:
    refs = [(ctx.find_clip(c)[0], c) for c in clip_ids]
    for track_id in {t for t, _ in refs}:
        ctx.check_unfrozen(track_id, "clips")
    if track_delta and ctx.editor.clamp_track_delta(refs, track_delta) != track_delta:
        raise invalid("Clips can only move onto tracks of their kind (audio, MIDI), within the track list")
    moved = ctx.editor.move_clips(refs, delta_beats, track_delta, copy_clips=copy)
    return {"clips": [clip_dict(ctx, t, ctx.project.clip(t, c)) for t, c in moved if _exists(ctx, t, c)]}


def _exists(ctx: OpContext, track_id: str, clip_id: str) -> bool:
    return any(c.id == clip_id for c in ctx.project.track(track_id).clips)


@operation(risk=Risk.EDIT, summary="Change a clip's settings: its name; an audio clip's gain, warping, segment "
                                   "BPM, transposition, detune and pan. Only what is given changes.",
           label="Change Clip")
def set_clip_props(ctx: OpContext, clip_id: ClipId, name: Annotated[str | None, MinLen(1), MaxLen(64)] = None,
                   gain_db: Annotated[float | None, Ge(-70.0), Le(24.0)] = None, warp: bool | None = None,
                   warp_mode: WarpMode | None = None,
                   segment_bpm: Annotated[float | None, Ge(20.0), Le(999.0)] = None,
                   transpose: Annotated[int | None, Ge(-48), Le(48)] = None,
                   detune: Annotated[float | None, Ge(-50.0), Le(50.0)] = None,
                   pan: Annotated[float | None, Ge(-1.0), Le(1.0)] = None) -> dict:
    track_id, clip = ctx.find_clip(clip_id)
    ctx.check_unfrozen(track_id, "clips")
    changes = {k: v for k, v in (("name", name), ("gain_db", gain_db), ("warp", warp), ("warp_mode", warp_mode),
                                 ("segment_bpm", segment_bpm), ("transpose", transpose), ("detune", detune),
                                 ("pan", pan)) if v is not None}
    audio_only = set(changes) - {"name"}
    if audio_only and not isinstance(clip, Clip):
        raise invalid(f"A MIDI clip has no {', '.join(sorted(audio_only))}")
    if changes:
        ctx.editor.update_clips([(track_id, clip_id)], lambda c: replace(c, **changes), "Change Clip")
    return {"clip": clip_dict(ctx, track_id, ctx.project.clip(track_id, clip_id))}


@operation(risk=Risk.DESTRUCTIVE, summary="Delete clips.")
def delete_clips(ctx: OpContext, clip_ids: ClipIds) -> dict:
    refs = [(ctx.find_clip(c)[0], c) for c in clip_ids]
    for track_id in {t for t, _ in refs}:
        ctx.check_unfrozen(track_id, "clips")
    names = [ctx.project.clip(t, c).name for t, c in refs]
    ctx.editor.delete_clips(refs)
    return {"deleted": names}
