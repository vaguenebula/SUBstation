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
tracks their MIDI input (version 7; older ones load hearing every input)."""

from __future__ import annotations

import json
import os
from pathlib import Path

from . import automation
from .automation import AutomationPoint, AutomationView, Envelope
from .keys import key_from_name
from .notes import normalize
from .project import (
    DEFAULT_TRACK_HEIGHT,
    DEFAULT_WARP_MODE,
    LEGACY_WARP_MODES,
    MONITOR_MODES,
    TRACK_KINDS,
    WARP_MODES,
    AnyClip,
    Clip,
    Device,
    MidiClip,
    MidiInput,
    Note,
    PluginRef,
    Project,
    Track,
    new_master,
)
from .timebase import TimeSignature

FORMAT = "gilstudio-project"
VERSION = 7  # 2: MIDI tracks, 3: plug-ins, 4: automation and master pan, 5: master devices, 6: inputs,
# 7: MIDI inputs
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
    return data


def _device(d: dict) -> Device:
    plugin = None
    if "plugin" in d:
        p = d["plugin"]
        plugin = PluginRef(format=str(p.get("format", "VST3")), uid=str(p["uid"]), name=str(p.get("name", "Plug-in")),
                           vendor=str(p.get("vendor", "")), path=str(p.get("path", "")),
                           instrument=bool(p.get("instrument", False)))
    state = d.get("state")
    return Device(id=d["id"], kind=d["kind"], enabled=bool(d.get("enabled", True)),
                  params={k: float(v) for k, v in d.get("params", {}).items()}, plugin=plugin,
                  state=state if isinstance(state, str) else None)


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
                **({"midi_input": _midi_input_to_dict(t.midi_input)} if t.is_midi else {}),
                "monitor": t.monitor,
                "armed": t.armed,
            }
            for t in project.tracks
        ],
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
            ),
            kind=kind,
            automation=_automation(t.get("automation")),
            automation_view=_view(t.get("automation_view")),
            input=_input(t.get("input")),
            midi_input=_midi_input(t.get("midi_input", {})) if kind == "midi" else MidiInput(),
            monitor=t.get("monitor") if t.get("monitor") in MONITOR_MODES else "auto",
            armed=bool(t.get("armed", False)),
        ))
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
        raise ProjectFileError("Not a GIL Studio project")
    if int(data.get("version", 0)) > VERSION:
        raise ProjectFileError("This project was saved by a newer version of GIL Studio")
    num, den = data.get("time_signature", [4, 4])
    loop = data.get("loop", {})
    project.replace_contents(
        tempo=float(data.get("tempo", 120.0)),
        time_signature=TimeSignature(int(num), int(den)),
        loop_enabled=bool(loop.get("enabled", False)),
        loop_start=float(loop.get("start", 0.0)),
        loop_end=float(loop.get("end", 16.0)),
        master=_master(data.get("master", {})),
        automation_locked=bool(data.get("automation_locked", False)),
        key=key_from_name(data.get("key")),
        tracks=tracks_from_dict(data, project_file),
        path=project_file,
    )


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
