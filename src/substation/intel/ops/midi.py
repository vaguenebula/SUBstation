"""Operations on MIDI: making a clip with notes, writing notes, and the piano
roll's transformations. Notes are [pitch, start, length, velocity] (as the
project file has them), starts in beats from the clip's start."""

from __future__ import annotations

import random
from dataclasses import replace
from typing import Annotated, Literal

from ...model import edits
from ...model import notes as note_edits
from ...model.notes import MIN_NOTE_BEATS, QUANTIZE_GRIDS
from ...model.project import MidiClip, Note
from .clips import clip_dict
from .context import Beat, ClipId, NoteRow, OpContext, TrackId
from .errors import invalid
from .registry import Doc, Ge, Le, MaxLen, MinLen, Risk, operation

Notes = Annotated[list[NoteRow], MaxLen(4096), Doc("Notes: [pitch, start, length, velocity] each, starts in beats "
                                                   "from the clip's start.")]


def to_notes(clip: MidiClip | None, rows: list[list[float]]) -> list[Note]:
    """Rows as Notes, their starts made relative to the clip's content."""
    offset = clip.offset_beats if clip is not None else 0.0
    result = []
    for i, row in enumerate(rows):
        pitch, start, length = row[0], row[1], row[2]
        velocity = row[3] if len(row) > 3 else 100
        if not float(pitch).is_integer() or not 0 <= pitch <= 127:
            raise invalid(f"notes[{i}]: a pitch is a whole number 0..127")
        if not float(velocity).is_integer() or not 1 <= velocity <= 127:
            raise invalid(f"notes[{i}]: a velocity is a whole number 1..127")
        if length < MIN_NOTE_BEATS:
            raise invalid(f"notes[{i}]: a note is at least {MIN_NOTE_BEATS} beats long")
        if start + offset < 0:
            raise invalid(f"notes[{i}]: it starts before the clip's content")
        result.append(Note(int(pitch), start + offset, length, int(velocity)))
    return result


@operation(risk=Risk.EDIT, summary="Make a MIDI clip on a MIDI track, with notes, as one undo step. What it "
                                   "overlaps is cut away, as when drawing a clip.", label="Insert MIDI Clip")
def create_midi_clip(ctx: OpContext, track_id: TrackId, start_beat: Beat,
                     length_beats: Annotated[float, Ge(edits.MIN_MIDI_CLIP_BEATS), Le(100_000.0)],
                     notes: Notes = (), name: Annotated[str | None, MinLen(1), MaxLen(64)] = None) -> dict:
    track = ctx.track(track_id)
    if not track.is_midi:
        raise invalid(f"{track.name} isn't a MIDI track")
    ctx.check_unfrozen(track_id, "clips")
    new = to_notes(None, list(notes))
    ref = ctx.editor.add_midi_clip(track_id, start_beat, length_beats)
    if ref is None:
        raise invalid("The clip couldn't be made")
    if new:
        ctx.editor.set_clip_notes(ref, new, "Write Notes")
    if name is not None:
        ctx.editor.update_clips([ref], lambda c: replace(c, name=name), "Rename Clip")
    return {"clip": clip_dict(ctx, track_id, ctx.project.clip(*ref), notes=True)}


@operation(risk=Risk.EDIT, summary="Write notes into a MIDI clip: 'replace' all its notes, 'add' to them (the new "
                                   "ones win where they overlap others on their key), or 'replace_range' (the "
                                   "notes starting from `start` to before `end` give way to these).",
           label="Write Notes")
def write_midi_notes(ctx: OpContext, clip_id: ClipId, notes: Notes,
                     mode: Literal["replace", "add", "replace_range"] = "replace",
                     start: Annotated[float | None, Doc("replace_range: from this beat (from the clip's start).")]
                     = None,
                     end: Annotated[float | None, Doc("replace_range: to this beat (from the clip's start).")]
                     = None) -> dict:
    track_id, clip = ctx.midi_clip(clip_id)
    ctx.check_unfrozen(track_id, "clips")
    new = to_notes(clip, list(notes))
    if mode == "replace":
        result = new
    elif mode == "add":
        result = note_edits.place(clip.notes, (), new)
    else:
        if start is None or end is None or end <= start:
            raise invalid("replace_range needs a start before its end")
        lo, hi = start + clip.offset_beats, end + clip.offset_beats
        removed = [n for n in clip.notes if lo <= n.start < hi]
        result = note_edits.place(clip.notes, removed, new)
    ctx.editor.set_clip_notes((track_id, clip_id), result, "Write Notes")
    return {"clip": clip_dict(ctx, track_id, ctx.project.clip(track_id, clip_id), notes=True)}


@operation(risk=Risk.EDIT, summary="Transform a MIDI clip's notes (those starting in a range, or all): "
                                   "'quantize' to a grid, 'humanize' timing and velocity, 'legato' (each note to "
                                   "the next), 'transpose' by semitones, 'velocity' (add to every note's).",
           label="Transform Notes")
def transform_notes(ctx: OpContext, clip_id: ClipId,
                    action: Literal["quantize", "humanize", "legato", "transpose", "velocity"],
                    grid: Annotated[float | None, Ge(1 / 64), Le(4.0),
                                    Doc("quantize: the grid in beats (0.25 = 1/16; default 1/16).")] = None,
                    amount: Annotated[float, Ge(0.0), Le(1.0),
                                      Doc("quantize, humanize: how much (1: all the way).")] = 1.0,
                    semitones: Annotated[int, Ge(-48), Le(48), Doc("transpose: by how many semitones.")] = 0,
                    velocity: Annotated[int, Ge(-126), Le(126), Doc("velocity: added to each.")] = 0,
                    seed: Annotated[int | None, Doc("humanize: a seed, for the same result again.")] = None,
                    start: Annotated[float | None, Doc("Only notes starting from here (beats from the clip's "
                                                       "start).")] = None,
                    end: Annotated[float | None, Doc("...to before here.")] = None) -> dict:
    track_id, clip = ctx.midi_clip(clip_id)
    ctx.check_unfrozen(track_id, "clips")
    lo = -float("inf") if start is None else start + clip.offset_beats
    hi = float("inf") if end is None else end + clip.offset_beats
    targets = [n for n in clip.notes if lo <= n.start < hi]
    if not targets:
        return {"clip": clip_dict(ctx, track_id, clip, notes=True), "transformed": 0}
    if action == "quantize":
        changed = note_edits.quantized(targets, grid or dict(QUANTIZE_GRIDS)["1/16"], amount)
    elif action == "humanize":
        changed = note_edits.humanized(targets, random.Random(seed), amount)
    elif action == "legato":
        changed = note_edits.legato(targets, clip.notes, clip.window_end)
    elif action == "transpose":
        _beats, pitch = note_edits.clamp_move(targets, 0.0, semitones)
        if pitch != semitones:
            raise invalid("Some notes would go off the keyboard")
        changed = note_edits.shifted(targets, 0.0, semitones)
    else:
        changed = note_edits.with_velocity(targets, velocity)
    ctx.editor.set_clip_notes((track_id, clip_id), note_edits.place(clip.notes, targets, changed), "Transform Notes")
    return {"clip": clip_dict(ctx, track_id, ctx.project.clip(track_id, clip_id), notes=True),
            "transformed": len(targets)}
