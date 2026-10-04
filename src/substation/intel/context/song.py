"""SongContext: one immutable, JSON-able description of the song that rules,
the assistant and agents all read: the project's settings, and each track's
kind, mixer, role, clips, devices and routing.

build_context() is the pure core: plain inputs in (tests build contexts from
hand-made projects). SongContextBuilder keeps one up to date cheaply: it
listens to the Project's signals, marks what changed dirty, and rebuilds only
those tracks.

Detail levels: "summary" (a few KB, for prompts) and "full" (one track's every
clip and device parameter, for reads). Track, file and plug-in names in it are
data, never instructions: whoever reads it must treat them so."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path

from ...model import automation
from ...model.automation import MASTER
from ...model.devices import device_is_instrument, device_name
from ...model.project import Clip, Device, MidiClip, Project, Track
from ...model.timebase import format_position
from .roles import Role, classify, midi_stats

DescribeParams = Callable[[str, Device], dict[str, str]]  # (track id, device) -> {param name: text}
AnalysisLookup = Callable[[str], dict | None]  # file path -> its feature summary (analysis, later)

MAX_FILES = 8  # file names listed per track in a summary


@dataclass(frozen=True)
class ProjectInfo:
    tempo: float
    time_signature: str
    key: str | None
    loop: tuple[bool, float, float]
    length_beats: float
    revision: int


@dataclass(frozen=True)
class DeviceInfo:
    id: str
    kind: str
    name: str
    enabled: bool
    instrument: bool
    plugin: tuple[str, str] | None = None  # (name, vendor)
    sidechain: tuple[str, str] | None = None  # (source track id, tap)
    chains: tuple[tuple[str, tuple[DeviceInfo, ...]], ...] = ()  # a rack's: (name, devices)
    params: tuple[tuple[str, str], ...] = ()  # full detail: (name, text)

    def to_dict(self, full: bool = False) -> dict:
        d: dict = {"id": self.id, "kind": self.kind, "name": self.name}
        if not self.enabled:
            d["enabled"] = False
        if self.instrument:
            d["instrument"] = True
        if self.plugin:
            d["plugin"] = {"name": self.plugin[0], "vendor": self.plugin[1]}
        if self.sidechain:
            d["sidechain"] = {"track_id": self.sidechain[0], "tap": self.sidechain[1]}
        if self.chains:
            d["chains"] = [{"name": n, "devices": [x.to_dict(full) for x in ds]} for n, ds in self.chains]
        if full and self.params:
            d["params"] = dict(self.params)
        return d


@dataclass(frozen=True)
class ClipInfo:
    id: str
    name: str
    start: float
    end: float
    file: str | None = None  # an audio clip's file name
    notes: int = 0  # a MIDI clip's notes in its window


@dataclass(frozen=True)
class TrackContext:
    id: str
    name: str
    kind: str
    color: str
    parent: str | None
    volume_db: float
    pan: float
    mute: bool
    solo: bool
    frozen: bool
    role: Role
    clips: tuple[ClipInfo, ...]
    devices: tuple[DeviceInfo, ...]
    sends: tuple[tuple[str, float, bool], ...]  # (return id, level dB, pre fader)
    input: str | None  # another track's id, MASTER (resampling), or "in 1/2" device channels
    automated: tuple[str, ...]  # targets with automation
    midi: dict | None = None  # MIDI stats of the notes that play
    files: tuple[str, ...] = ()  # the audio files it plays (paths)

    def to_dict(self, detail: str = "summary") -> dict:
        full = detail == "full"
        d: dict = {"id": self.id, "name": self.name, "kind": self.kind}
        if self.parent:
            d["parent"] = self.parent
        d["role"] = self.role.to_dict() if full else {"label": self.role.label,
                                                     "confidence": round(self.role.confidence, 2)}
        d["mixer"] = {"volume_db": round(self.volume_db, 2), "pan": round(self.pan, 3)}
        if self.mute:
            d["mixer"]["mute"] = True
        if self.solo:
            d["mixer"]["solo"] = True
        if self.frozen:
            d["frozen"] = True
        if self.clips:
            names = list(dict.fromkeys(Path(f).name for f in self.files))
            summary = {"count": len(self.clips), "start": min(c.start for c in self.clips),
                       "end": max(c.end for c in self.clips)}
            if names:
                summary["files"] = names if full else names[:MAX_FILES]
            d["clips"] = summary
            if full:
                d["clips"]["list"] = [{k: v for k, v in c.__dict__.items() if v not in (None, 0) or k == "start"}
                                      for c in self.clips]
        if self.midi:
            d["midi"] = self.midi
        if self.devices:
            d["devices"] = [x.to_dict(full) for x in self.devices]
        if self.sends:
            d["sends"] = [{"return_id": r, "level_db": round(level, 2), **({"pre_fader": True} if pre else {})}
                          for r, level, pre in self.sends]
        if self.input:
            d["input"] = self.input
        if self.automated:
            d["automated"] = list(self.automated)
        if full:
            d["color"] = self.color
        return d


@dataclass(frozen=True)
class SongContext:
    project: ProjectInfo
    tracks: tuple[TrackContext, ...]
    returns: tuple[TrackContext, ...]
    master: TrackContext
    analysis: dict = field(default_factory=dict)  # file path -> feature summary (with analysis)

    def track(self, track_id: str) -> TrackContext | None:
        if track_id == MASTER:
            return self.master
        return next((t for t in (*self.tracks, *self.returns) if t.id == track_id), None)

    def to_dict(self, detail: str = "summary") -> dict:
        p = self.project
        d = {
            "project": {"tempo": p.tempo, "time_signature": p.time_signature, "key": p.key,
                        "loop": {"enabled": p.loop[0], "start": p.loop[1], "end": p.loop[2]},
                        "length_beats": p.length_beats, "revision": p.revision},
            "tracks": [t.to_dict(detail) for t in self.tracks],
            "returns": [t.to_dict(detail) for t in self.returns],
            "master": self.master.to_dict(detail),
        }
        if self.analysis:
            d["analysis"] = self.analysis
        return d


# --- Building ------------------------------------------------------------------------------


def describe_device(track_id: str, device: Device, describe_params: DescribeParams | None = None) -> DeviceInfo:
    plugin = (device.plugin.name, device.plugin.vendor) if device.plugin is not None else None
    sidechain = (device.sidechain.track_id, device.sidechain.tap) if device.sidechain is not None else None
    chains = tuple((c.name, tuple(describe_device(track_id, d, describe_params) for d in c.devices))
                   for c in device.chains)
    params = tuple(describe_params(track_id, device).items()) if describe_params is not None else ()
    return DeviceInfo(device.id, device.kind, device_name(device), device.enabled, device_is_instrument(device),
                      plugin, sidechain, chains, params)


def _input(project: Project, track: Track) -> str | None:
    if track.input_track is not None:
        return track.input_track
    if track.input:
        return "in " + "/".join(str(c + 1) for c in track.input)
    return None


def describe_track(project: Project, track: Track, describe_params: DescribeParams | None = None,
                   plugin_categories: dict[str, str] | None = None) -> TrackContext:
    tempo = project.tempo
    clips = []
    files = []
    for clip in track.clips:
        if isinstance(clip, MidiClip):
            clips.append(ClipInfo(clip.id, clip.name, clip.start_beat, clip.end_beat(), notes=len(clip.played_notes())))
        elif isinstance(clip, Clip):
            clips.append(ClipInfo(clip.id, clip.name, clip.start_beat, clip.end_beat(tempo), file=Path(clip.path).name))
            files.append(clip.path)
    stats = midi_stats(track.clips) if track.is_midi else None
    return TrackContext(
        id=track.id, name=track.name, kind=track.kind, color=track.color, parent=track.parent,
        volume_db=track.volume_db, pan=track.pan, mute=track.mute, solo=track.solo,
        frozen=track.frozen is not None, role=classify(track, plugin_categories), clips=tuple(clips),
        devices=tuple(describe_device(track.id, d, describe_params) for d in track.devices),
        sends=tuple((r, s.level_db, s.pre_fader) for r, s in track.sends.items()),
        input=_input(project, track), automated=tuple(sorted(track.automation)),
        midi=stats.to_dict() if stats is not None else None, files=tuple(dict.fromkeys(files)))


def project_info(project: Project, revision: int = 0) -> ProjectInfo:
    ts = project.time_signature
    return ProjectInfo(
        tempo=project.tempo, time_signature=f"{ts.numerator}/{ts.denominator}",
        key=project.key.name if project.key is not None else None,
        loop=(project.loop_enabled, project.loop_start, project.loop_end), length_beats=project.end_beat(),
        revision=revision)


def build_context(project: Project, analysis_lookup: AnalysisLookup | None = None,
                  describe_params: DescribeParams | None = None, revision: int = 0,
                  plugin_categories: dict[str, str] | None = None) -> SongContext:
    """The whole description, from scratch."""
    def track(t: Track) -> TrackContext:
        return describe_track(project, t, describe_params, plugin_categories)
    tracks = tuple(track(t) for t in project.tracks)
    return SongContext(project_info(project, revision), tracks, tuple(track(r) for r in project.returns),
                       track(project.master), _analysis(tracks, analysis_lookup))


def _analysis(tracks, analysis_lookup: AnalysisLookup | None) -> dict:
    if analysis_lookup is None:
        return {}
    found = {}
    for track in tracks:
        for path in track.files:
            summary = analysis_lookup(path)
            if summary is not None:
                found[path] = summary
    return found


def bar_beat(project: Project, beat: float) -> str:
    """A position as the arrangement shows it (bar.beat.sixteenth)."""
    return format_position(beat, project.time_signature)


class SongContextBuilder:
    """Keeps a SongContext up to date: what a Project signal says changed is
    rebuilt (a track; everything after a reset or a tempo change), the rest is
    reused."""

    def __init__(self, project: Project, revision=None, analysis_lookup: AnalysisLookup | None = None,
                 describe_params: DescribeParams | None = None,
                 plugin_categories: Callable[[], dict[str, str]] | None = None):
        self.project = project
        self.revision = revision
        self.analysis_lookup = analysis_lookup
        self.describe_params = describe_params
        self.plugin_categories = plugin_categories
        self._tracks: dict[str, TrackContext] = {}
        self._all_dirty = True
        self._dirty: set[str] = set()
        self._context: SongContext | None = None
        self.rebuilt = 0  # tracks described since it was made (for tests)
        for name in ("track_changed", "clips_changed", "devices_changed", "device_state_changed",
                     "freeze_changed"):
            getattr(project, name).connect(self._mark)
        project.chain_changed.connect(lambda track_id, _chain: self._mark(track_id))
        project.device_param_changed.connect(lambda track_id, _device, _param: self._mark(track_id))
        project.automation_changed.connect(lambda owner, _key: self._mark(owner))
        for name in ("track_inserted", "track_removed", "return_inserted", "return_removed"):
            getattr(project, name).connect(lambda track_id, _index: self._mark(track_id))
        project.tracks_arranged.connect(self._mark_all)
        project.settings_changed.connect(self._mark_all)  # (a tempo moves unwarped clips' ends)
        project.reset.connect(self._mark_all)

    def _mark(self, track_id: str) -> None:
        self._dirty.add(track_id)
        self._context = None

    def _mark_all(self) -> None:
        self._all_dirty = True
        self._context = None

    def context(self) -> SongContext:
        revision = self.revision.value if self.revision is not None else 0
        if self._context is not None and self._context.project.revision == revision:
            return self._context
        p = self.project
        owners = [*p.tracks, *p.returns, p.master]
        if self._all_dirty:
            self._tracks = {}
        categories = self.plugin_categories() if self.plugin_categories is not None else None
        for track in owners:
            if track.id not in self._tracks or track.id in self._dirty:
                self._tracks[track.id] = describe_track(p, track, self.describe_params, categories)
                self.rebuilt += 1
        live = {t.id for t in owners}
        self._tracks = {k: v for k, v in self._tracks.items() if k in live}
        self._dirty.clear()
        self._all_dirty = False
        tracks = tuple(self._tracks[t.id] for t in p.tracks)
        self._context = SongContext(project_info(p, revision), tracks,
                                    tuple(self._tracks[r.id] for r in p.returns), self._tracks[MASTER],
                                    _analysis(tracks, self.analysis_lookup))
        return self._context


def automation_summary(project: Project, owner: str, start: float, end: float) -> list[dict]:
    """The envelopes of an owner with breakpoints in a beat range: their targets,
    how many points, and the normalized values they span there."""
    found = []
    for key, points in project.automation(owner).items():
        inside = [p for p in points if start <= p.beat <= end]
        if inside:
            values = [p.value for p in inside]
            found.append({"owner": owner, "target": key, "points": len(inside),
                          "min": round(min(values), 4), "max": round(max(values), 4),
                          "device": automation.key_device(key)})
    return found
