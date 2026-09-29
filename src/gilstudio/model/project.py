"""The project model: the single source of truth for the UI, undo and saving.

The audio engine mirrors this model (see audio/engine_bridge.py). Mutating
methods here are called only by undo commands (model/commands.py), which keeps
every edit undoable and every change signalled.
"""

from __future__ import annotations

import uuid
from dataclasses import dataclass, field
from pathlib import Path

from PySide6.QtCore import QObject, Signal

from .timebase import TimeSignature, seconds_to_beats

# Ableton-like clip/track colours.
TRACK_COLORS = [
    "#ff94a6", "#ffa529", "#cc9927", "#f7f47c", "#bffb00", "#1aff2f", "#25ffa8",
    "#5cffe8", "#8bc5ff", "#5480e4", "#92a7ff", "#d86ce4", "#e553a0", "#ffb3a0",
]

DEFAULT_TRACK_HEIGHT = 68
MIN_TRACK_HEIGHT = 24
MAX_TRACK_HEIGHT = 400


def new_id() -> str:
    return uuid.uuid4().hex[:12]


@dataclass(frozen=True)
class Clip:
    """An audio clip. Unwarped: its length is fixed in seconds, so its length in
    beats follows the tempo (as in Ableton with warping off)."""

    id: str
    path: str
    name: str
    start_beat: float
    duration_sec: float
    offset_sec: float = 0.0
    source_duration_sec: float = 0.0
    gain_db: float = 0.0
    # Clip view settings. Stored and saved, but the engine doesn't apply these
    # yet (only gain_db reaches the audio).
    warp: bool = False
    warp_mode: str = "Beats"
    segment_bpm: float = 0.0  # 0: not set, shown as the project tempo
    transpose: int = 0  # semitones
    detune: float = 0.0  # cents
    pan: float = 0.0

    def length_beats(self, tempo: float) -> float:
        return seconds_to_beats(self.duration_sec, tempo)

    def end_beat(self, tempo: float) -> float:
        return self.start_beat + self.length_beats(tempo)


@dataclass
class Device:
    """An insert device on a track. `kind` is 'utility' today; plugin devices
    will use kinds like 'vst3' / 'clap' plus an identifier."""

    id: str
    kind: str
    enabled: bool = True
    params: dict[str, float] = field(default_factory=dict)


@dataclass
class Track:
    id: str
    name: str
    color: str
    volume_db: float = 0.0
    pan: float = 0.0
    mute: bool = False
    solo: bool = False
    height: int = DEFAULT_TRACK_HEIGHT
    clips: list[Clip] = field(default_factory=list)  # sorted by start_beat
    devices: list[Device] = field(default_factory=list)


class Project(QObject):
    track_inserted = Signal(str, int)  # track id, index
    track_removed = Signal(str, int)
    track_changed = Signal(str)  # name, colour, mixer settings or height
    clips_changed = Signal(str)  # track id
    devices_changed = Signal(str)  # track id: devices added/removed/toggled
    device_param_changed = Signal(str, str, str)  # track id, device id, param id
    settings_changed = Signal()  # tempo, time signature, loop, master volume
    reset = Signal()  # everything replaced (new/open)

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self.tempo = 120.0
        self.time_signature = TimeSignature()
        self.loop_enabled = False
        self.loop_start = 0.0
        self.loop_end = 16.0
        self.master_volume_db = 0.0
        self.tracks: list[Track] = []
        self.path: Path | None = None

    # --- Queries --------------------------------------------------------------

    def track(self, track_id: str) -> Track:
        for track in self.tracks:
            if track.id == track_id:
                return track
        raise KeyError(track_id)

    def track_index(self, track_id: str) -> int:
        for index, track in enumerate(self.tracks):
            if track.id == track_id:
                return index
        raise KeyError(track_id)

    def has_track(self, track_id: str) -> bool:
        return any(t.id == track_id for t in self.tracks)

    def clip(self, track_id: str, clip_id: str) -> Clip:
        for clip in self.track(track_id).clips:
            if clip.id == clip_id:
                return clip
        raise KeyError(clip_id)

    def end_beat(self) -> float:
        """End of the last clip (0 for an empty arrangement)."""
        return max((c.end_beat(self.tempo) for t in self.tracks for c in t.clips), default=0.0)

    def next_color(self) -> str:
        return TRACK_COLORS[len(self.tracks) % len(TRACK_COLORS)]

    def unique_track_name(self, base: str) -> str:
        names = {t.name for t in self.tracks}
        if base not in names:
            return base
        n = 2
        while f"{base} {n}" in names:
            n += 1
        return f"{base} {n}"

    # --- Mutations (call through undo commands) --------------------------------

    def insert_track(self, track: Track, index: int) -> None:
        index = max(0, min(index, len(self.tracks)))
        self.tracks.insert(index, track)
        self.track_inserted.emit(track.id, index)

    def remove_track(self, track_id: str) -> tuple[Track, int]:
        index = self.track_index(track_id)
        track = self.tracks.pop(index)
        self.track_removed.emit(track_id, index)
        return track, index

    def update_track(self, track_id: str, **attrs) -> None:
        track = self.track(track_id)
        for name, value in attrs.items():
            if not hasattr(track, name) or name in ("id", "clips", "devices"):
                raise AttributeError(name)
            setattr(track, name, value)
        self.track_changed.emit(track_id)

    def set_clips(self, track_id: str, clips: list[Clip]) -> None:
        self.track(track_id).clips = sorted(clips, key=lambda c: c.start_beat)
        self.clips_changed.emit(track_id)

    def set_devices(self, track_id: str, devices: list[Device]) -> None:
        self.track(track_id).devices = devices
        self.devices_changed.emit(track_id)

    def device(self, track_id: str, device_id: str) -> Device:
        for device in self.track(track_id).devices:
            if device.id == device_id:
                return device
        raise KeyError(device_id)

    def set_device_param(self, track_id: str, device_id: str, param_id: str, value: float) -> None:
        self.device(track_id, device_id).params[param_id] = value
        self.device_param_changed.emit(track_id, device_id, param_id)

    def set_device_enabled(self, track_id: str, device_id: str, enabled: bool) -> None:
        self.device(track_id, device_id).enabled = enabled
        self.devices_changed.emit(track_id)

    def update_settings(self, **attrs) -> None:
        for name, value in attrs.items():
            if name not in ("tempo", "time_signature", "loop_enabled", "loop_start", "loop_end", "master_volume_db"):
                raise AttributeError(name)
            setattr(self, name, value)
        self.settings_changed.emit()

    def replace_contents(self, *, tempo: float, time_signature: TimeSignature, loop_enabled: bool,
                         loop_start: float, loop_end: float, master_volume_db: float,
                         tracks: list[Track], path: Path | None) -> None:
        self.tempo = tempo
        self.time_signature = time_signature
        self.loop_enabled = loop_enabled
        self.loop_start = loop_start
        self.loop_end = loop_end
        self.master_volume_db = master_volume_db
        self.tracks = tracks
        self.path = path
        self.reset.emit()

    def clear(self) -> None:
        self.replace_contents(tempo=120.0, time_signature=TimeSignature(), loop_enabled=False, loop_start=0.0,
                              loop_end=16.0, master_volume_db=0.0, tracks=[], path=None)
