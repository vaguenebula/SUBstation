"""Operations on tracks: naming and colouring them, adding, grouping, moving,
duplicating and deleting them, their mixer and their sends."""

from __future__ import annotations

from typing import Annotated

from ...model.automation import MASTER, MAX_VOLUME_DB, MIN_VOLUME_DB, MIXER_PAN, MIXER_VOLUME
from ...model.devices import BUILTIN_INSTRUMENTS, DEFAULT_INSTRUMENT
from ...model.editor import AT_INDEX
from ...model.project import PluginRef, iter_devices
from .context import Color, OpContext, TrackId, TrackName
from .errors import invalid, not_found
from .params import is_automated, override
from .registry import Doc, Ge, Le, MinLen, Risk, operation

TrackIds = Annotated[list[TrackId], MinLen(1), Doc("Tracks' ids.")]
GroupId = Annotated[str | None, Doc("The group to put it in (its id); null: where `index` is (no group if last).")]
Index = Annotated[int | None, Ge(0), Doc("Where among the tracks (0: first); null: last.")]


def find_plugin(ctx: OpContext, uid: str) -> PluginRef | None:
    """A scanned plug-in by its uid, as a PluginRef (None: not installed)."""
    if ctx.ui is None:
        return None
    for plugin in ctx.ui.available_plugins():
        if plugin.uid.upper() == uid.upper():
            return PluginRef(format=plugin.format, uid=plugin.uid, name=plugin.name, vendor=plugin.vendor,
                             path=plugin.path, instrument=plugin.instrument)
    return None


def _parent(ctx: OpContext, group_id: str | None):
    if group_id is None:
        return AT_INDEX
    if not ctx.project.has_track(group_id) or not ctx.project.track(group_id).is_group:
        raise not_found(f"There is no group {group_id!r}", group_id=group_id)
    return group_id


@operation(risk=Risk.COSMETIC, summary="Rename a track, group or return.", idempotent=True)
def rename_track(ctx: OpContext, track_id: TrackId, name: TrackName) -> dict:
    track = ctx.owner(track_id)
    if track.is_master:
        raise invalid("The master can't be renamed")
    if not name.strip():
        raise invalid("A name can't be blank")
    ctx.editor.rename_track(track_id, name.strip())
    return {"track": ctx.ref(track_id)}


@operation(risk=Risk.COSMETIC, summary="Set a track's (group's, return's) colour.", label="Change Track Color",
           idempotent=True)
def set_track_color(ctx: OpContext, track_id: TrackId, color: Color) -> dict:
    track = ctx.owner(track_id)
    if track.is_master:
        raise invalid("The master's colour can't be changed")
    ctx.editor.set_track_color(track_id, color.lower())
    return {"track": ctx.ref(track_id)}


@operation(risk=Risk.EDIT, summary="Add an audio track.", label="Insert Audio Track")
def add_audio_track(ctx: OpContext, name: TrackName | None = None, index: Index = None,
                    group_id: GroupId = None) -> dict:
    track = ctx.editor.add_audio_track(index, name, parent=_parent(ctx, group_id))
    return {"track": ctx.ref(track.id)}


@operation(risk=Risk.EDIT, summary="Add a MIDI track with an instrument: a built-in one ('synth', 'sampler'), an "
                                   "instrument plug-in by its uid (list_available_devices), or none (null).",
           label="Insert MIDI Track")
def add_midi_track(ctx: OpContext, name: TrackName | None = None,
                   instrument: Annotated[str | None, Doc("A built-in instrument's id, an instrument plug-in's uid, "
                                                        "or null for none.")] = DEFAULT_INSTRUMENT,
                   index: Index = None, group_id: GroupId = None) -> dict:
    builtin, plugin = None, None
    if instrument in BUILTIN_INSTRUMENTS:
        builtin = instrument
    elif instrument is not None:
        plugin = find_plugin(ctx, instrument)
        if plugin is None:
            raise not_found(f"There is no instrument {instrument!r} (see list_available_devices)",
                            instrument=instrument)
        if not plugin.instrument:
            raise invalid(f"{plugin.name} isn't an instrument")
    track = ctx.editor.add_midi_track(index, name, instrument=builtin, plugin=plugin, parent=_parent(ctx, group_id))
    return {"track": ctx.ref(track.id), "devices": [d.id for d in iter_devices(track.devices)]}


@operation(risk=Risk.EDIT, summary="Add a return track (fed by sends).", label="Insert Return Track")
def add_return_track(ctx: OpContext, name: TrackName | None = None) -> dict:
    track = ctx.editor.add_return_track(None, name)
    return {"track": ctx.ref(track.id)}


@operation(risk=Risk.EDIT, summary="Put tracks (and what is in them) into a new group, where the first of them was.")
def group_tracks(ctx: OpContext, track_ids: TrackIds) -> dict:
    for track_id in track_ids:
        ctx.track(track_id)
    group = ctx.editor.group_tracks(track_ids)
    if group is None:
        raise invalid("Nothing to group")
    return {"group": ctx.ref(group.id)}


@operation(risk=Risk.EDIT, summary="Take groups apart: what was in them takes their place.")
def ungroup_tracks(ctx: OpContext, group_ids: TrackIds) -> dict:
    for group_id in group_ids:
        if not ctx.track(group_id).is_group:
            raise invalid(f"{ctx.project.track(group_id).name} isn't a group")
    ctx.editor.ungroup(group_ids)
    return {}


@operation(risk=Risk.EDIT, summary="Move tracks (and what is in them) to before the track at `index` (counted as "
                                   "the tracks are now; past the end: last), into a group or none.")
def move_tracks(ctx: OpContext, track_ids: TrackIds, index: Annotated[int, Ge(0)],
                group_id: Annotated[str | None, Doc("The group they go into; null: none.")] = None) -> dict:
    for track_id in track_ids:
        ctx.track(track_id)
    if group_id is not None:
        _parent(ctx, group_id)
    if not ctx.editor.move_tracks(track_ids, index, group_id):
        raise invalid("They can't go there (a group into itself, amid another group's tracks, or nowhere new)")
    return {"tracks": [ctx.ref(t) for t in track_ids]}


@operation(risk=Risk.EDIT, summary="Duplicate tracks (a group with what is in it); the copies go after them.")
def duplicate_tracks(ctx: OpContext, track_ids: TrackIds) -> dict:
    for track_id in track_ids:
        ctx.track(track_id)
    if ctx.engine is not None:
        ctx.engine.store_plugin_states()  # (the copies take the plug-ins' states as they are now)
    copies = ctx.editor.duplicate_tracks(track_ids)
    return {"tracks": [ctx.ref(t.id) for t in copies]}


@operation(risk=Risk.DESTRUCTIVE, summary="Delete tracks and return tracks (a group with what is in it, a return "
                                          "with the sends into it).")
def delete_tracks(ctx: OpContext, track_ids: TrackIds) -> dict:
    for track_id in track_ids:
        if ctx.owner(track_id).is_master:
            raise invalid("The master can't be deleted")
    names = [ctx.project.track(t).name for t in track_ids]
    ctx.editor.delete_tracks(track_ids)
    return {"deleted": names}


@operation(risk=Risk.EDIT, summary="Set a track's mixer: volume (dB), pan (-1 left .. 1 right), mute, solo. Only "
                                   "what is given changes. Setting an automated volume or pan overrides its "
                                   "automation, as moving the fader does (re_enable_automation undoes that).",
           label="Change Mixer")
def set_track_mixer(ctx: OpContext, track_id: TrackId,
                    volume_db: Annotated[float | None, Ge(MIN_VOLUME_DB), Le(MAX_VOLUME_DB)] = None,
                    pan: Annotated[float | None, Ge(-1.0), Le(1.0)] = None,
                    mute: bool | None = None, solo: bool | None = None) -> dict:
    track = ctx.owner(track_id)
    if track.is_master and (mute is not None or solo is not None):
        raise invalid("The master has no mute or solo")
    overridden = []
    for attr, value, key in (("volume_db", volume_db, MIXER_VOLUME), ("pan", pan, MIXER_PAN),
                             ("mute", mute, None), ("solo", solo, None)):
        if value is None:
            continue
        automated = key is not None and is_automated(ctx, track_id, key)
        ctx.editor.set_track_param(track_id, attr, value)
        if automated:
            override(ctx, track_id, key)
            overridden.append(key)
    track = ctx.project.track(track_id)
    result = {"track": ctx.ref(track_id), "mixer": {"volume_db": track.volume_db, "pan": track.pan}}
    if not track.is_master:
        result["mixer"] |= {"mute": track.mute, "solo": track.solo}
    if overridden:
        result["overrides_automation"] = overridden
    return result


@operation(risk=Risk.EDIT, summary="Set a track's (group's, return's) send to a return: its level (dB, -70 is "
                                   "silent) and whether it taps before the fader. A new send starts silent, after "
                                   "the fader.", label="Change Send")
def set_send(ctx: OpContext, track_id: TrackId, return_id: TrackId,
             level_db: Annotated[float | None, Ge(MIN_VOLUME_DB), Le(MAX_VOLUME_DB)] = None,
             pre_fader: bool | None = None) -> dict:
    track = ctx.owner(track_id)
    if track_id == MASTER:
        raise invalid("The master has no sends")
    if not ctx.project.has_return(return_id):
        raise not_found(f"There is no return track {return_id!r}", return_id=return_id)
    ctx.editor.set_send(track_id, return_id, level_db, pre_fader)
    send = ctx.project.track(track_id).sends.get(return_id)
    return {"track": ctx.ref(track.id), "return": ctx.ref(return_id),
            "send": None if send is None else {"level_db": send.level_db, "pre_fader": send.pre_fader}}
