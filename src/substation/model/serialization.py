"""Project files: JSON (.gilproj). Clip paths are stored both absolute and
relative to the project file, so a project folder can be moved. MIDI tracks
store their clips' notes inline, as [pitch, start, length, velocity]. Plug-in
devices store which plug-in they are and its state (a .vstpreset, base64).
Automation is stored per track (and for the master) by target key, each
envelope as [beat, value, curve] points, with what the arrangement shows of it.
The master is stored apart from the tracks: its mixer, devices and automation.
Files from before version 5 have no master devices; they load as they were.
Tracks store their audio input (device channels), monitoring and whether they
are armed (version 6; older files load with none, Auto, not armed), and MIDI
tracks their MIDI input (version 7; older ones load hearing every input).
Tracks store the group they are in ("parent") and whether they are folded
(version 8); a track that can't be in its group (the file was edited)
loads out of it. Return tracks are stored apart from the tracks ("returns"),
and every track (and return) its sends, by return id (version 9; older files
have none). Sends to a return that isn't there, or that would close a cycle,
are dropped. Audio tracks store the track whose output they take as their
input ("input_track", or MASTER: resampling; version 10); one that isn't there,
or that would close a cycle, is dropped. Devices store their sidechain (the
track and where it is tapped; version 11); one from a track that isn't there,
or that would close a cycle, is dropped. Racks store their chains (each with
its devices and mixer) and macro mappings (version 12; a mapping to a device
not in its rack is dropped).

Presets: a device (a rack with everything in it too) on its own, in a file of
its own (device_to_preset, preset_device): what the project file stores of it.
A preset loads as new devices (new ids), without sidechains (they name tracks
of the project it came from)."""

from __future__ import annotations

import json
import os
from pathlib import Path

from . import automation
from .automation import MASTER, AutomationPoint, AutomationView, Envelope
from .keys import key_from_name
from .notes import normalize
from .project import (
    DEFAULT_TRACK_HEIGHT,
    DEFAULT_WARP_MODE,
    GROUP_KIND,
    LEGACY_WARP_MODES,
    MACRO_COUNT,
    MAX_RACK_DEPTH,
    MONITOR_MODES,
    POST_FADER,
    RETURN_KIND,
    TRACK_KINDS,
    WARP_MODES,
    AnyClip,
    Chain,
    Clip,
    Device,
    MacroMapping,
    MidiClip,
    MidiInput,
    Note,
    PluginRef,
    Project,
    Send,
    Sidechain,
    Track,
    feeds,
    iter_devices,
    new_master,
    rack_height,
    refresh_ids,
    repair_tree,
    routing_graph,
    sidechain_would_cycle,
    would_cycle,
)
from .timebase import TimeSignature

FORMAT = "gilstudio-project"
VERSION = 12  # 2: MIDI tracks, 3: plug-ins, 4: automation and master pan, 5: master devices, 6: inputs,
# 7: MIDI inputs, 8: group tracks, 9: return tracks and sends, 10: inputs from tracks (resampling), 11: sidechains,
# 12: racks
PRESET_FORMAT = "gilstudio-preset"
PRESET_VERSION = 1
PRESET_EXTENSION = ".gilpreset"
EXTENSION = ".gilproj"


class ProjectFileError(Exception):
    pass


def _relative(path: str, base: Path | None) -> str | None:
    if base is None:
        return None
    try:
        return os.path.relpath(path, base)
    except ValueError:  # different drive
        return None


def _clip_to_dict(clip: AnyClip, base: Path | None) -> dict:
    if isinstance(clip, MidiClip):
        return {
            "id": clip.id,
            "name": clip.name,
            "start_beat": clip.start_beat,
            "duration_beats": clip.duration_beats,
            "offset_beats": clip.offset_beats,
            "notes": [[n.pitch, n.start, n.length, n.velocity] for n in clip.notes],
        }
    return {
        "id": clip.id,
        "name": clip.name,
        "path": clip.path,
        "relative_path": _relative(clip.path, base),
        "start_beat": clip.start_beat,
        "duration_sec": clip.duration_sec,
        "offset_sec": clip.offset_sec,
        "source_duration_sec": clip.source_duration_sec,
        "gain_db": clip.gain_db,
        "warp": clip.warp,
        "warp_mode": clip.warp_mode,
        "segment_bpm": clip.segment_bpm,
        "transpose": clip.transpose,
        "detune": clip.detune,
        "pan": clip.pan,
    }


def _device_to_dict(device: Device) -> dict:
    data = {"id": device.id, "kind": device.kind, "enabled": device.enabled, "params": device.params}
    if device.plugin is not None:
        p = device.plugin
        data["plugin"] = {"format": p.format, "uid": p.uid, "name": p.name, "vendor": p.vendor, "path": p.path,
                          "instrument": p.instrument}
        data["state"] = device.state
    elif device.state is not None:
        data["state"] = device.state
    if device.sidechain is not None:
        data["sidechain"] = {"track": device.sidechain.track_id, "tap": device.sidechain.tap}
    if device.is_rack:
        data["chains"] = [{"id": c.id, "name": c.name, "volume_db": c.volume_db, "pan": c.pan, "mute": c.mute,
                           "solo": c.solo, "devices": [_device_to_dict(d) for d in c.devices]} for c in device.chains]
        data["macros"] = [{"macro": m.macro, "device": m.device_id, "param": m.param_id, "low": m.low,
                           "high": m.high} for m in device.macros]
    return data


def _device(d: dict) -> Device:
    plugin = None
    if "plugin" in d:
        p = d["plugin"]
        plugin = PluginRef(format=str(p.get("format", "VST3")), uid=str(p["uid"]), name=str(p.get("name", "Plug-in")),
                           vendor=str(p.get("vendor", "")), path=str(p.get("path", "")),
                           instrument=bool(p.get("instrument", False)))
    state = d.get("state")
    device = Device(id=d["id"], kind=d["kind"], enabled=bool(d.get("enabled", True)),
                    params={k: float(v) for k, v in d.get("params", {}).items()}, plugin=plugin,
                    state=state if isinstance(state, str) else None, sidechain=_sidechain(d.get("sidechain")))
    if device.is_rack:
        device.chains = [_chain(c) for c in d.get("chains", [])]
        inside = {x.id for x in iter_devices([device])} - {device.id}
        device.macros = tuple(_macro(m) for m in d.get("macros", [])
                              if str(m.get("device")) in inside and 0 <= int(m.get("macro", -1)) < MACRO_COUNT)
    return device


def _chain(c: dict) -> Chain:
    return Chain(id=str(c["id"]), name=str(c.get("name", "Chain")),
                 devices=[_device(d) for d in c.get("devices", [])],
                 volume_db=max(automation.MIN_VOLUME_DB, min(automation.MAX_VOLUME_DB, float(c.get("volume_db", 0.0)))),
                 pan=max(-1.0, min(1.0, float(c.get("pan", 0.0)))), mute=bool(c.get("mute", False)),
                 solo=bool(c.get("solo", False)))


def _macro(m: dict) -> MacroMapping:
    return MacroMapping(int(m["macro"]), str(m["device"]), str(m["param"]), float(m.get("low", 0.0)),
                        float(m.get("high", 1.0)))


def _sidechain(data) -> Sidechain | None:
    if not isinstance(data, dict) or not isinstance(data.get("track"), str):
        return None
    tap = data.get("tap", POST_FADER)
    return Sidechain(data["track"], tap if isinstance(tap, str) and tap else POST_FADER)


def _automation_to_dict(envelopes: dict[str, Envelope]) -> dict:
    return {key: [[p.beat, p.value, p.curve] for p in points] for key, points in envelopes.items() if points}


def _view_to_dict(view: AutomationView) -> dict:
    return {"shown": view.shown, "key": view.key, "lanes": list(view.lanes)}


def _automation(data) -> dict[str, Envelope]:
    """Envelopes as saved; targets this version doesn't know are dropped."""
    envelopes = {}
    for key, points in (data or {}).items():
        if not automation.is_key(key):
            continue
        envelope = automation.normalize(AutomationPoint(float(p[0]), float(p[1]), float(p[2]) if len(p) > 2 else 0.0)
                                        for p in points)
        if envelope:
            envelopes[key] = envelope
    return envelopes


def _view(data) -> AutomationView:
    data = data or {}
    key = data.get("key")
    return AutomationView(shown=bool(data.get("shown", False)), key=key if key and automation.is_key(key) else None,
                          lanes=tuple(k for k in data.get("lanes", []) if automation.is_key(k)))


def _master_to_dict(master: Track) -> dict:
    return {"volume_db": master.volume_db, "pan": master.pan,
            "devices": [_device_to_dict(d) for d in master.devices],
            "automation": _automation_to_dict(master.automation),
            "automation_view": _view_to_dict(master.automation_view)}


def _master(data: dict) -> Track:
    return new_master(
        volume_db=float(data.get("volume_db", 0.0)),
        pan=max(-1.0, min(1.0, float(data.get("pan", 0.0)))),
        devices=[_device(d) for d in data.get("devices", [])],  # none before version 5
        automation=_automation(data.get("automation")),
        automation_view=_view(data.get("automation_view")),
    )


def _sends_to_dict(sends: dict[str, Send]) -> dict:
    return {return_id: {"level_db": send.level_db, "pre_fader": send.pre_fader} for return_id, send in sends.items()}


def _sends(data) -> dict[str, Send]:
    sends = {}
    for return_id, send in (data or {}).items():
        sends[str(return_id)] = Send(level_db=max(automation.MIN_VOLUME_DB, min(automation.MAX_VOLUME_DB,
                                                                                float(send.get("level_db", 0.0)))),
                                     pre_fader=bool(send.get("pre_fader", False)))
    return sends


def _return_to_dict(track: Track) -> dict:
    return {"id": track.id, "kind": track.kind, "name": track.name, "color": track.color,
            "volume_db": track.volume_db, "pan": track.pan, "mute": track.mute, "solo": track.solo,
            "height": track.height, "devices": [_device_to_dict(d) for d in track.devices],
            "automation": _automation_to_dict(track.automation),
            "automation_view": _view_to_dict(track.automation_view),
            "sends": _sends_to_dict(track.sends)}


def _return(t: dict) -> Track:
    return Track(id=t["id"], name=t["name"], color=t["color"], kind=RETURN_KIND,
                 volume_db=float(t.get("volume_db", 0.0)), pan=float(t.get("pan", 0.0)),
                 mute=bool(t.get("mute", False)), solo=bool(t.get("solo", False)),
                 height=int(t.get("height", DEFAULT_TRACK_HEIGHT)),
                 devices=[_device(d) for d in t.get("devices", [])],
                 automation=_automation(t.get("automation")), automation_view=_view(t.get("automation_view")),
                 sends=_sends(t.get("sends")))


def returns_from_dict(data: dict) -> list[Track]:
    return [_return(t) for t in data.get("returns", [])]


def repair_routing(tracks: list[Track], returns: list[Track], master: Track | None = None) -> None:
    """Drops the sends, inputs and sidechains a project can't have (the file was
    edited): to a return (or from a track) that isn't there, and those closing a
    cycle (the later ones; sends first, then inputs)."""
    ids = {r.id for r in returns}
    for track in tracks:
        track.sends = {r: send for r, send in track.sends.items() if r in ids}
    saved = {ret.id: ret.sends for ret in returns}
    inputs = {t.id: t.input_track for t in tracks}
    for track in [*tracks, *returns]:  # made again in order: each checked against those before it
        track.input_track = None
    for ret in returns:
        ret.sends = {}
    for ret in returns:
        for return_id, send in saved[ret.id].items():
            if return_id in ids and not would_cycle(tracks, returns, ret.id, return_id):
                ret.sends = {**ret.sends, return_id: send}
    sources = ids | {t.id for t in tracks} | {MASTER}
    for track in tracks:
        source = inputs[track.id]
        if source in sources and track.is_audio and (
                source == MASTER or not feeds(routing_graph(tracks, returns), track.id, source)):
            track.input_track = source
            track.input = ()
    owners = [*tracks, *returns, *([master] if master is not None else [])]
    sidechained = [(owner, device, device.sidechain) for owner in owners for device in iter_devices(owner.devices)
                   if device.sidechain is not None]
    for _, device, _ in sidechained:
        device.sidechain = None
    for owner, device, sidechain in sidechained:
        owner_id = MASTER if owner is master else owner.id
        if sidechain.track_id in sources - {MASTER} and not sidechain_would_cycle(tracks, returns, owner_id,
                                                                               sidechain.track_id):
            device.sidechain = sidechain


def project_to_dict(project: Project, project_file: Path | None = None) -> dict:
    base = project_file.parent if project_file else None
    return {
        "format": FORMAT,
        "version": VERSION,
        "tempo": project.tempo,
        "key": project.key.name if project.key else None,
        "time_signature": [project.time_signature.numerator, project.time_signature.denominator],
        "loop": {"enabled": project.loop_enabled, "start": project.loop_start, "end": project.loop_end},
        "automation_locked": project.automation_locked,
        "master": _master_to_dict(project.master),
        "tracks": [
            {
                "id": t.id,
                "kind": t.kind,
                "name": t.name,
                "color": t.color,
                "volume_db": t.volume_db,
                "pan": t.pan,
                "mute": t.mute,
                "solo": t.solo,
                "height": t.height,
                "devices": [_device_to_dict(d) for d in t.devices],
                "clips": [_clip_to_dict(c, base) for c in t.clips],
                "automation": _automation_to_dict(t.automation),
                "automation_view": _view_to_dict(t.automation_view),
                "input": list(t.input),
                **({"input_track": t.input_track} if t.input_track is not None else {}),
                **({"midi_input": _midi_input_to_dict(t.midi_input)} if t.is_midi else {}),
                "monitor": t.monitor,
                "armed": t.armed,
                "parent": t.parent,
                "folded": t.folded,
                "sends": _sends_to_dict(t.sends),
            }
            for t in project.tracks
        ],
        "returns": [_return_to_dict(t) for t in project.returns],
    }


def _resolve_clip_path(data: dict, base: Path | None) -> str:
    path = data["path"]
    if not os.path.exists(path) and base is not None and data.get("relative_path"):
        candidate = os.path.normpath(base / data["relative_path"])
        if os.path.exists(candidate):
            return candidate
    return path


def _audio_clip(c: dict, base: Path | None) -> Clip:
    return Clip(
        id=c["id"],
        path=_resolve_clip_path(c, base),
        name=c.get("name", Path(c["path"]).stem),
        start_beat=float(c["start_beat"]),
        duration_sec=float(c["duration_sec"]),
        offset_sec=float(c.get("offset_sec", 0.0)),
        source_duration_sec=float(c.get("source_duration_sec", 0.0)),
        gain_db=float(c.get("gain_db", 0.0)),
        warp=bool(c.get("warp", False)),
        warp_mode=_warp_mode(c.get("warp_mode")),
        segment_bpm=float(c.get("segment_bpm", 0.0)),
        transpose=int(c.get("transpose", 0)),
        detune=float(c.get("detune", 0.0)),
        pan=float(c.get("pan", 0.0)),
    )


def _midi_clip(c: dict) -> MidiClip:
    notes = []
    for pitch, start, length, velocity in c.get("notes", []):
        if float(length) > 0:
            notes.append(Note(pitch=max(0, min(127, int(pitch))), start=max(0.0, float(start)), length=float(length),
                              velocity=max(1, min(127, int(velocity)))))
    return MidiClip(
        id=c["id"],
        name=c.get("name", "MIDI"),
        start_beat=float(c["start_beat"]),
        duration_beats=float(c["duration_beats"]),
        offset_beats=float(c.get("offset_beats", 0.0)),
        notes=normalize(notes),
    )


def tracks_from_dict(data: dict, project_file: Path | None = None) -> list[Track]:
    base = project_file.parent if project_file else None
    tracks = []
    for t in data.get("tracks", []):
        kind = t.get("kind", "audio")
        if kind not in TRACK_KINDS:
            raise ValueError(f"unknown track kind {kind!r}")
        tracks.append(Track(
            id=t["id"],
            name=t["name"],
            color=t["color"],
            volume_db=float(t.get("volume_db", 0.0)),
            pan=float(t.get("pan", 0.0)),
            mute=bool(t.get("mute", False)),
            solo=bool(t.get("solo", False)),
            height=int(t.get("height", DEFAULT_TRACK_HEIGHT)),
            devices=[_device(d) for d in t.get("devices", [])],
            clips=sorted(
                (_midi_clip(c) if kind == "midi" else _audio_clip(c, base) for c in t.get("clips", [])),
                key=lambda c: c.start_beat,
            ) if kind != GROUP_KIND else [],
            kind=kind,
            automation=_automation(t.get("automation")),
            automation_view=_view(t.get("automation_view")),
            input=_input(t.get("input")),
            input_track=t.get("input_track") if isinstance(t.get("input_track"), str) else None,
            midi_input=_midi_input(t.get("midi_input", {})) if kind == "midi" else MidiInput(),
            monitor=t.get("monitor") if t.get("monitor") in MONITOR_MODES else "auto",
            armed=bool(t.get("armed", False)) and kind != GROUP_KIND,
            parent=t.get("parent") if isinstance(t.get("parent"), str) else None,
            folded=bool(t.get("folded", False)),
            sends=_sends(t.get("sends")),
        ))
    repair_tree(tracks)
    return tracks


def _midi_input_to_dict(midi_input: MidiInput | None) -> dict | None:
    if midi_input is None:
        return None
    return {"device": midi_input.device, "channel": midi_input.channel}


def _midi_input(data) -> MidiInput | None:
    if data is None:
        return None
    channel = int(data.get("channel", 0))
    return MidiInput(device=str(data.get("device", "")), channel=channel if 0 <= channel <= 16 else 0)


def _input(data) -> tuple[int, ...]:
    channels = tuple(int(c) for c in (data or ()) if int(c) >= 0)
    return channels if len(channels) <= 2 else ()


def _warp_mode(name) -> str:
    name = LEGACY_WARP_MODES.get(name, name)
    return name if name in WARP_MODES else DEFAULT_WARP_MODE


def load_into(project: Project, data: dict, project_file: Path | None = None) -> None:
    if data.get("format") != FORMAT:
        raise ProjectFileError("Not a SUBstation project")
    if int(data.get("version", 0)) > VERSION:
        raise ProjectFileError("This project was saved by a newer version of SUBstation")
    num, den = data.get("time_signature", [4, 4])
    loop = data.get("loop", {})
    tracks = tracks_from_dict(data, project_file)
    returns = returns_from_dict(data)
    master = _master(data.get("master", {}))
    repair_routing(tracks, returns, master)
    project.replace_contents(
        tempo=float(data.get("tempo", 120.0)),
        time_signature=TimeSignature(int(num), int(den)),
        loop_enabled=bool(loop.get("enabled", False)),
        loop_start=float(loop.get("start", 0.0)),
        loop_end=float(loop.get("end", 16.0)),
        master=master,
        automation_locked=bool(data.get("automation_locked", False)),
        key=key_from_name(data.get("key")),
        tracks=tracks,
        returns=returns,
        path=project_file,
    )


def device_to_preset(device: Device) -> dict:
    """A device as a preset: what the project file stores of it (a rack with its
    chains, the devices in them and its macros; plug-ins' states as last stored
    in the model), without its sidechains."""
    data = _device_to_dict(device)

    def strip(d: dict) -> None:
        d.pop("sidechain", None)
        for chain in d.get("chains", []):
            for inner in chain["devices"]:
                strip(inner)

    strip(data)
    return {"format": PRESET_FORMAT, "version": PRESET_VERSION, "device": data}


def preset_device(data: dict) -> Device:
    """A preset's device, new: fresh ids for it and everything in it. Raises
    ProjectFileError for something that isn't a preset."""
    if not isinstance(data, dict) or data.get("format") != PRESET_FORMAT:
        raise ProjectFileError("Not a SUBstation preset")
    try:
        if int(data.get("version", 0)) > PRESET_VERSION:
            raise ProjectFileError("This preset was saved by a newer version of SUBstation")
        device = _device(data["device"])
        if rack_height(device) > MAX_RACK_DEPTH:
            raise ProjectFileError("The preset nests racks too deep")
    except (KeyError, TypeError, ValueError, AttributeError, RecursionError) as exc:
        raise ProjectFileError(f"The preset is damaged: {exc}") from exc
    for inner in iter_devices([device]):
        inner.sidechain = None
    refresh_ids(device)
    return device


def save_preset(device: Device, path: Path) -> None:
    path = Path(path)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(device_to_preset(device), indent=2), encoding="utf-8")
    os.replace(tmp, path)


def load_preset(path: Path) -> Device:
    path = Path(path)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ProjectFileError(f"Could not read {path.name}: {exc}") from exc
    return preset_device(data)


def save_project(project: Project, path: Path) -> None:
    path = Path(path)
    data = project_to_dict(project, path)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(data, indent=2), encoding="utf-8")
    os.replace(tmp, path)  # never leave a half-written project behind
    project.path = path


def load_project(project: Project, path: Path) -> None:
    path = Path(path)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ProjectFileError(f"Could not read {path.name}: {exc}") from exc
    try:
        load_into(project, data, path)
    except (KeyError, TypeError, ValueError) as exc:
        raise ProjectFileError(f"{path.name} is damaged: {exc}") from exc
