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

from .timebase import TimeSignature, beats_to_seconds, seconds_to_beats

# Ableton-like clip/track colours.
TRACK_COLORS = [
    "#ff94a6", "#ffa529", "#cc9927", "#f7f47c", "#bffb00", "#1aff2f", "#25ffa8",
    "#5cffe8", "#8bc5ff", "#5480e4", "#92a7ff", "#d86ce4", "#e553a0", "#ffb3a0",
]

# Warp modes, in the engine's order (gilstudio._engine.WarpMode).
WARP_MODES = ["Transients", "Standard", "Smooth", "Formants", "Re-Pitch"]
DEFAULT_WARP_MODE = "Standard"
# Names used by earlier versions, mapped to the mode that plays the same way.
LEGACY_WARP_MODES = {"Beats": "Transients", "Tones": "Standard", "Complex": "Standard",
                     "Texture": "Smooth", "Complex Pro": "Formants"}

# Audio tracks hold audio clips; MIDI tracks hold MIDI clips and an instrument.
TRACK_KINDS = ("audio", "midi")

DEFAULT_TRACK_HEIGHT = 68
MIN_TRACK_HEIGHT = 24
MAX_TRACK_HEIGHT = 400


def new_id() -> str:
    return uuid.uuid4().hex[:12]


@dataclass(frozen=True)
class Clip:
    """An audio clip. `offset_sec` and `duration_sec` measure the source audio it
    plays. Unwarped, that plays at its own speed, so the clip's length in beats
    follows the tempo. Warped, the audio is taken to be at `segment_bpm` and is
    stretched to the project tempo, so its length in beats is fixed (as in Ableton)."""

    id: str
    path: str
    name: str
    start_beat: float
    duration_sec: float
    offset_sec: float = 0.0
    source_duration_sec: float = 0.0
    gain_db: float = 0.0
    # Clip view settings.
    warp: bool = False
    warp_mode: str = DEFAULT_WARP_MODE  # one of WARP_MODES
    segment_bpm: float = 0.0  # tempo of the source audio; 0: not set, shown as the project tempo
    transpose: int = 0  # semitones
    detune: float = 0.0  # cents
    pan: float = 0.0

    @property
    def is_warped(self) -> bool:
        return self.warp and self.segment_bpm > 0

    def source_tempo(self, tempo: float) -> float:
        """The tempo at which this clip's audio maps onto beats: its segment BPM
        when warped, otherwise the project tempo (it plays at its own speed)."""
        return self.segment_bpm if self.is_warped else tempo

    def beats_to_source(self, beats: float, tempo: float) -> float:
        """Seconds of source audio covered by `beats` of this clip."""
        return beats_to_seconds(beats, self.source_tempo(tempo))

    def source_to_beats(self, seconds: float, tempo: float) -> float:
        return seconds_to_beats(seconds, self.source_tempo(tempo))

    def length_beats(self, tempo: float) -> float:
        return self.source_to_beats(self.duration_sec, tempo)

    def end_beat(self, tempo: float) -> float:
        return self.start_beat + self.length_beats(tempo)


@dataclass(frozen=True)
class Note:
    """A MIDI note. Times are in beats from the start of its clip's content."""

    pitch: int  # MIDI note number, 60 = C3
    start: float
    length: float
    velocity: int = 100  # 1..127

    @property
    def end(self) -> float:
        return self.start + self.length


@dataclass(frozen=True)
class MidiClip:
    """A MIDI clip: a window onto its notes, as an audio clip is onto its file.
    Content beat `offset_beats` plays at `start_beat`. Notes outside the window
    are kept but not played, so trimming or splitting a clip never loses notes.
    MIDI is measured in beats, so a clip's length doesn't follow the tempo.
    (`tempo` arguments are accepted so both kinds of clip can be edited alike.)"""

    id: str
    name: str
    start_beat: float
    duration_beats: float
    offset_beats: float = 0.0
    notes: tuple[Note, ...] = ()  # sorted by start, then pitch

    def length_beats(self, tempo: float = 0.0) -> float:
        return self.duration_beats

    def end_beat(self, tempo: float = 0.0) -> float:
        return self.start_beat + self.duration_beats

    @property
    def window_end(self) -> float:
        """The content beat at the clip's end."""
        return self.offset_beats + self.duration_beats

    def to_timeline(self, content_beat: float) -> float:
        return self.start_beat + content_beat - self.offset_beats

    def played_notes(self) -> list[tuple[float, float, Note]]:
        """(timeline start, timeline end, note) for each note the clip plays: those
        starting inside its window, cut at the clip's end (as in Ableton)."""
        end = self.window_end
        return [(self.to_timeline(n.start), self.to_timeline(min(n.end, end)), n)
                for n in self.notes if self.offset_beats <= n.start < end]


AnyClip = Clip | MidiClip


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
    clips: list[AnyClip] = field(default_factory=list)  # sorted by start_beat; MidiClips on MIDI tracks
    devices: list[Device] = field(default_factory=list)
    kind: str = "audio"  # one of TRACK_KINDS; fixed for the track's life

    @property
    def is_midi(self) -> bool:
        return self.kind == "midi"


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

    def clip(self, track_id: str, clip_id: str) -> AnyClip:
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
            if not hasattr(track, name) or name in ("id", "kind", "clips", "devices"):
                raise AttributeError(name)
            setattr(track, name, value)
        self.track_changed.emit(track_id)

    def set_clips(self, track_id: str, clips: list[AnyClip]) -> None:
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
