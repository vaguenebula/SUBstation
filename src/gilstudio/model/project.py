"""The project model: the single source of truth for the UI, undo and saving.

The audio engine mirrors this model (see audio/engine_bridge.py). Mutating
methods here are called only by undo commands (model/commands.py), which keeps
every edit undoable and every change signalled. (View state, like track heights
and which automation shows, changes directly: it is saved but not undone.)

Automation belongs to an owner: a track (its id) or the master (MASTER); see
automation.py.

The master is a Track too (kind "master", id MASTER), with devices, a mixer and
automation, but no clips. It is `project.master`, not one of `project.tracks`
(the arrangement's); `project.track(MASTER)` finds it, so whatever works on a
track's devices, mixer or automation works on the master's.

Group tracks (kind "group") hold other tracks: what is in a group goes into it,
through its devices and mixer, and on to the master (or the group it is in).
A group has no clips. The hierarchy lives here, as each track's `parent` (the
group it is in, or None); `project.tracks` stays flat, with the invariant that
a group's descendants follow it, together (tree_problem). The engine sees only
where each track's output goes.

Any track can be folded (view state): a folded track shows as a thin row, its
automation hidden; a folded group keeps its row but hides its tracks (and its
automation).

Return tracks (kind "return") are fed by sends: any track, group or return can
send its signal to a return, at a level, after its fader or before it
(`Track.sends`, by return id). A return has no clips; it goes to the master,
through its devices and mixer, and may send on into another return, but never
back into one that feeds it (would_cycle). Returns are `project.returns`, apart
from the arrangement's tracks (and the master); `project.track()` finds them
too, as it does the master, so whatever works on a track's devices, mixer or
automation works on a return's. They are named by letter, in order (A, B...).

An audio track's input is some of the audio device's channels (`Track.input`),
or another track's output, after its fader (`Track.input_track`: a track, a
group or a return), or the master's (MASTER: resampling). Taking a track's
output is an edge of the routing graph, like a send: a track can't take the
output of one it feeds (routing_graph, feeds). The master is no part of that
graph: everything reaches it, and a track recording it never plays it back
into it (it can't monitor it).

A device with a sidechain (aux) input can hear a track's (a group's, a
return's) signal there (`Device.sidechain`): after its fader, before it, or
after one of its devices. That is an edge of the routing graph too, from the
source to the track the device is on (sidechain_would_cycle); the master's
devices can take any track's. Should the source go, the sidechain goes (in the
same undo step); should the device it taps after leave the source, it taps
before the fader until the device comes back.
"""

from __future__ import annotations

import uuid
from dataclasses import dataclass, field
from pathlib import Path

from PySide6.QtCore import QObject, Signal

from .automation import MASTER, MIN_VOLUME_DB, AutomationView, Envelope
from .keys import Key
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

# Audio tracks hold audio clips; MIDI tracks hold MIDI clips and an instrument;
# group tracks hold other tracks.
GROUP_KIND = "group"
TRACK_KINDS = ("audio", "midi", GROUP_KIND)
MASTER_KIND = "master"  # the master's kind: no clips, effects only
RETURN_KIND = "return"  # a return track's: fed by sends, no clips
# Input monitoring: when a track hears its input instead of its clips. "auto":
# while armed, unless it plays back without recording (as in Ableton).
MONITOR_MODES = ("off", "in", "auto")
MASTER_COLOR = "#a0a0a0"

DEFAULT_TRACK_HEIGHT = 80
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


@dataclass(frozen=True)
class PluginRef:
    """Which plug-in a device is: enough to load it, to find it again if it
    moved, and to name it if it is missing."""

    format: str  # "VST3"
    uid: str  # VST3 class id
    name: str
    vendor: str = ""
    path: str = ""  # where it was when last loaded
    instrument: bool = False


PLUGIN_KIND = "plugin"

# Where a sidechain takes its source's signal (Sidechain.tap), unless after one of its devices.
POST_FADER = "post"
PRE_FADER = "pre"


@dataclass(frozen=True)
class Sidechain:
    """What a device's sidechain (aux) input hears: a track's signal (a group's,
    a return's: `track_id`), after its fader and pan (POST_FADER), before it
    (PRE_FADER, after all its devices), or after one of its devices (`tap`: that
    device's id; before the fader while that device isn't on the track). It
    isn't heard on its own, so the source's mute and solo silence it only after
    the fader."""

    track_id: str
    tap: str = POST_FADER

    @property
    def tap_device(self) -> str | None:
        """The device it is taken after (None: after the fader, or before it)."""
        return None if self.tap in (POST_FADER, PRE_FADER) else self.tap


@dataclass
class Device:
    """An insert device on a track: a built-in one ('synth', 'utility'), or a
    plug-in (kind 'plugin', with `plugin` saying which).

    A built-in device's parameters are its whole state. A plug-in keeps its own
    state; `state` holds it (base64) as last saved, for loading the project.
    Its `params` only record values changed from the host, for undo. A device
    with a sidechain (aux) input may hear a track there (`sidechain`)."""

    id: str
    kind: str
    enabled: bool = True
    params: dict[str, float] = field(default_factory=dict)
    plugin: PluginRef | None = None
    state: str | None = None
    sidechain: Sidechain | None = None

    @property
    def is_plugin(self) -> bool:
        return self.kind == PLUGIN_KIND


@dataclass(frozen=True)
class MidiInput:
    """Which MIDI input a MIDI track hears and records: every input ("") or one
    by name, on every channel (0) or one (1-16)."""

    device: str = ""
    channel: int = 0

    @property
    def all_devices(self) -> bool:
        return not self.device


@dataclass(frozen=True)
class Send:
    """A track's send to a return track: its level (dB; MIN_VOLUME_DB or below
    is silent) and where it taps the track's signal: after its fader (and pan),
    or before it. A muted track sends nothing either way."""

    level_db: float = MIN_VOLUME_DB
    pre_fader: bool = False


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
    kind: str = "audio"  # one of TRACK_KINDS (or MASTER_KIND: the master); fixed for the track's life
    automation: dict[str, Envelope] = field(default_factory=dict)  # target key -> envelope (never empty)
    automation_view: AutomationView = field(default_factory=AutomationView)
    # Audio input: device channels (0-based): () none, (c,) mono, (l, r) a stereo pair.
    input: tuple[int, ...] = ()
    # An audio track's input from another track's output instead (its id; MASTER:
    # the master's, resampling); `input` is () then. None: the device's channels.
    input_track: str | None = None
    # MIDI input (MIDI tracks); None: none. New MIDI tracks hear every input, as in Ableton.
    midi_input: MidiInput | None = field(default_factory=MidiInput)
    monitor: str = "auto"  # one of MONITOR_MODES
    armed: bool = False  # records when recording starts (saved, not undone)
    parent: str | None = None  # the group it is in (None: none); see tree_problem
    folded: bool = False  # a thin row, automation hidden; a group: its tracks hidden (saved, not undone)
    sends: dict[str, Send] = field(default_factory=dict)  # return id -> its send (replaced whole, never changed)

    @property
    def is_midi(self) -> bool:
        return self.kind == "midi"

    @property
    def is_audio(self) -> bool:
        return self.kind == "audio"

    @property
    def is_group(self) -> bool:
        return self.kind == GROUP_KIND

    @property
    def is_master(self) -> bool:
        return self.kind == MASTER_KIND

    @property
    def is_return(self) -> bool:
        return self.kind == RETURN_KIND

    @property
    def has_clips(self) -> bool:
        """Whether it is a track of the arrangement that plays clips (audio or MIDI)."""
        return self.kind in ("audio", "midi")

    @property
    def has_input(self) -> bool:
        """Whether it has something to record: an audio input (or another track's
        output), or a MIDI track's MIDI input."""
        if not self.has_clips:
            return False
        return self.midi_input is not None if self.is_midi else bool(self.input) or self.input_track is not None


def new_master(**attrs) -> Track:
    return Track(id=MASTER, name="Master", color=MASTER_COLOR, kind=MASTER_KIND, **attrs)


def return_letter(index: int) -> str:
    """A, B, ... Z, AA, AB...: what a return (by its place among the returns) is called."""
    letters = ""
    index += 1
    while index > 0:
        index, rest = divmod(index - 1, 26)
        letters = chr(ord("A") + rest) + letters
    return letters


def tree_problem(tracks: list[Track]) -> str | None:
    """Why these tracks, in this order, don't make a valid tree (None if they
    do): a track's parent must be a group listed before it, and everything
    between a group and its last descendant must be a descendant of it. Then a
    group's tracks follow it, together, and no group is in itself."""
    path: list[str] = []  # the groups the next track can be in (outermost first)
    seen = set()
    for track in tracks:
        if track.id in seen:
            return f"{track.name} is listed twice"
        seen.add(track.id)
        if track.parent is None:
            path = []
        elif track.parent in path:
            del path[path.index(track.parent) + 1:]
        else:
            return f"{track.name} is not with the other tracks of its group"
        if track.is_group:
            path.append(track.id)
    return None


def repair_tree(tracks: list[Track]) -> None:
    """Takes tracks out of groups they can't be in (see tree_problem), keeping their order."""
    path: list[str] = []
    for track in tracks:
        if track.parent is not None and track.parent not in path:
            track.parent = None
        if track.parent is None:
            path = []
        else:
            del path[path.index(track.parent) + 1:]
        if track.is_group:
            path.append(track.id)


RoutingGraph = dict[str, list[str]]  # track id -> the tracks its signal goes into


def routing_graph(tracks: list[Track], returns: list[Track]) -> RoutingGraph:
    """Where each track's (and return's) signal goes: into its group, into the
    returns it sends to, into the tracks taking their input from it, and into
    the tracks whose devices take it as their sidechain. (Not the master, which
    isn't in the graph.)"""
    graph: RoutingGraph = {t.id: [] for t in [*tracks, *returns]}
    for track in [*tracks, *returns]:
        if track.parent in graph:
            graph[track.id].append(track.parent)
        graph[track.id].extend(r for r in track.sends if r in graph)
        if track.input_track in graph:
            graph[track.input_track].append(track.id)
        for device in track.devices:
            if device.sidechain is not None and device.sidechain.track_id in graph:
                graph[device.sidechain.track_id].append(track.id)
    return graph


def feeds(graph: RoutingGraph, source: str, target: str) -> bool:
    """Whether `source`'s signal reaches `target` (or `source` is `target`)."""
    seen, stack = set(), [source]
    while stack:
        current = stack.pop()
        if current == target:
            return True
        if current not in seen:
            seen.add(current)
            stack.extend(graph.get(current, ()))
    return False


def would_cycle(tracks: list[Track], returns: list[Track], track_id: str, return_id: str) -> bool:
    """Whether a send from a track into a return would close a cycle: the return
    is the track, or feeds it (through outputs, sends and inputs)."""
    return feeds(routing_graph(tracks, returns), return_id, track_id)


def input_would_cycle(tracks: list[Track], returns: list[Track], track_id: str, source_id: str) -> bool:
    """Whether taking its input from `source_id`'s output would close a cycle: the
    source is the track, or the track feeds it. Never the master's."""
    return source_id != MASTER and feeds(routing_graph(tracks, returns), track_id, source_id)


def sidechain_would_cycle(tracks: list[Track], returns: list[Track], track_id: str, source_id: str) -> bool:
    """Whether a device on `track_id` taking `source_id`'s signal as its sidechain
    would close a cycle: the source is the device's track, or that track feeds
    it. Never on the master (everything goes into it); the master is never a source."""
    if source_id == MASTER:
        return True
    return track_id != MASTER and feeds(routing_graph(tracks, returns), track_id, source_id)


TrackTree = tuple[tuple[str, str | None], ...]  # every track's (id, parent), in order


class Project(QObject):
    track_inserted = Signal(str, int)  # track id, index
    track_removed = Signal(str, int)
    return_inserted = Signal(str, int)  # return id, index in project.returns
    return_removed = Signal(str, int)
    track_changed = Signal(str)  # name, colour, mixer settings, sends or height (MASTER: the master's mixer)
    tracks_arranged = Signal()  # the tracks' order or groups changed (not which tracks there are)
    clips_changed = Signal(str)  # track id
    devices_changed = Signal(str)  # track id: devices added/removed/toggled, or a sidechain changed
    device_param_changed = Signal(str, str, str)  # track id, device id, param id
    device_state_changed = Signal(str, str)  # track id, device id: a plug-in's whole state was set (a preset)
    settings_changed = Signal()  # tempo, time signature, key, loop, automation lock
    automation_changed = Signal(str, str)  # owner (track id or MASTER), target key
    automation_view_changed = Signal(str)  # owner: what its automation shows
    reset = Signal()  # everything replaced (new/open)

    def __init__(self, parent: QObject | None = None) -> None:
        super().__init__(parent)
        self.tempo = 120.0
        self.time_signature = TimeSignature()
        # The project's key: audio added with a key in its file name is transposed to it.
        self.key: Key | None = None
        self.loop_enabled = False
        self.loop_start = 0.0
        self.loop_end = 16.0
        # Locked: automation stays where it is when clips move. Unlocked, the
        # automation under clips moves (or is copied) with them, as in Ableton.
        self.automation_locked = False
        self.master = new_master()
        self.tracks: list[Track] = []
        self.returns: list[Track] = []  # return tracks, in order (A, B, ...)
        self.path: Path | None = None

    # --- Queries --------------------------------------------------------------

    def track(self, track_id: str) -> Track:
        """A track of the arrangement, a return track, or the master (MASTER)."""
        if track_id == MASTER:
            return self.master
        for track in self.tracks:
            if track.id == track_id:
                return track
        for track in self.returns:
            if track.id == track_id:
                return track
        raise KeyError(track_id)

    def track_index(self, track_id: str) -> int:
        for index, track in enumerate(self.tracks):
            if track.id == track_id:
                return index
        raise KeyError(track_id)

    def has_track(self, track_id: str) -> bool:
        """Whether this is a track of the arrangement (not the master: see has_owner)."""
        return any(t.id == track_id for t in self.tracks)

    def clip(self, track_id: str, clip_id: str) -> AnyClip:
        for clip in self.track(track_id).clips:
            if clip.id == clip_id:
                return clip
        raise KeyError(clip_id)

    def end_beat(self) -> float:
        """End of the last clip (0 for an empty arrangement)."""
        return max((c.end_beat(self.tempo) for t in self.tracks for c in t.clips), default=0.0)

    def has_owner(self, owner: str) -> bool:
        """Whether this is a track, a return or the master: something with devices, a mixer and automation."""
        return owner == MASTER or self.has_track(owner) or self.has_return(owner)

    def all_tracks(self) -> list[Track]:
        """The arrangement's tracks, the returns, then the master."""
        return [*self.tracks, *self.returns, self.master]

    def automation(self, owner: str) -> dict[str, Envelope]:
        """An owner's envelopes by target key (read only: change them through commands)."""
        return self.track(owner).automation

    def envelope(self, owner: str, key: str) -> Envelope:
        return self.automation(owner).get(key, ())

    def automation_view(self, owner: str) -> AutomationView:
        return self.track(owner).automation_view

    def owners(self) -> list[str]:
        """Everything that has automation: the tracks, the returns, then the master."""
        return [t.id for t in self.tracks] + [t.id for t in self.returns] + [MASTER]

    # --- Returns and sends ------------------------------------------------------

    def has_return(self, track_id: str) -> bool:
        return any(t.id == track_id for t in self.returns)

    def return_index(self, track_id: str) -> int:
        for index, track in enumerate(self.returns):
            if track.id == track_id:
                return index
        raise KeyError(track_id)

    def return_letter(self, track_id: str) -> str:
        return return_letter(self.return_index(track_id))

    def senders(self) -> list[Track]:
        """Everything that can send: the tracks (groups too), then the returns."""
        return [*self.tracks, *self.returns]

    def would_cycle(self, track_id: str, return_id: str) -> bool:
        """Whether a send from a track into a return would close a cycle (see would_cycle)."""
        return would_cycle(self.tracks, self.returns, track_id, return_id)

    # --- Inputs ----------------------------------------------------------------

    def input_would_cycle(self, track_id: str, source_id: str) -> bool:
        """Whether a track taking its input from another's output (or a return's)
        would close a cycle (see input_would_cycle)."""
        return input_would_cycle(self.tracks, self.returns, track_id, source_id)

    def sidechain_would_cycle(self, track_id: str, source_id: str) -> bool:
        """Whether a device on a track (or the master) taking another's signal as
        its sidechain would close a cycle (see sidechain_would_cycle)."""
        return sidechain_would_cycle(self.tracks, self.returns, track_id, source_id)

    def sidechain_sources(self, track_id: str) -> list[Track]:
        """The tracks (groups too) and returns a device on a track (or the
        master) could take as its sidechain, but its own, in order (some would
        close a cycle: sidechain_would_cycle)."""
        return [t for t in [*self.tracks, *self.returns] if t.id != track_id]

    def input_sources(self, track_id: str) -> list[Track]:
        """The tracks (groups too) and returns whose output a track could take as
        its input, but itself, in order (some would close a cycle: input_would_cycle).
        The master's (resampling) can be taken too."""
        return [t for t in [*self.tracks, *self.returns] if t.id != track_id]

    def input_name(self, source_id: str) -> str:
        """What an input from a track's output is called: the track's name, or "Resampling" (the master's)."""
        return "Resampling" if source_id == MASTER else self.track(source_id).name

    def send_targets(self, track_id: str) -> list[Track]:
        """The returns a track (or return) can send to: all but those that would close a cycle."""
        if track_id == MASTER:
            return []
        return [r for r in self.returns if not self.would_cycle(track_id, r.id)]

    # --- Groups ---------------------------------------------------------------

    def subtree_end(self, index: int) -> int:
        """The index just past the last descendant of the track at `index` (index + 1 if it has none)."""
        track_id = self.tracks[index].id
        end = index + 1
        while end < len(self.tracks) and self.is_descendant(self.tracks[end].id, track_id):
            end += 1
        return end

    def descendants(self, track_id: str) -> list[Track]:
        """What is in a group (the tracks in groups in it too), in order."""
        index = self.track_index(track_id)
        return self.tracks[index + 1:self.subtree_end(index)]

    def children(self, track_id: str) -> list[Track]:
        return [t for t in self.tracks if t.parent == track_id]

    def ancestors(self, track_id: str) -> list[str]:
        """The groups a track is in, the nearest first."""
        result = []
        parent = self.track(track_id).parent
        while parent is not None:
            result.append(parent)
            parent = self.track(parent).parent
        return result

    def is_descendant(self, track_id: str, group_id: str) -> bool:
        return group_id in self.ancestors(track_id)

    def depth(self, track_id: str) -> int:
        return len(self.ancestors(track_id))

    def is_hidden(self, track_id: str) -> bool:
        """Whether a group it is in is folded (the arrangement doesn't show it)."""
        return any(self.track(g).folded for g in self.ancestors(track_id))

    def parent_at(self, index: int) -> str | None:
        """The group a track inserted at `index` goes into: that of the track it goes
        before (amid a group's tracks it has to be in that group)."""
        return self.tracks[index].parent if 0 <= index < len(self.tracks) else None

    def tree(self) -> TrackTree:
        return tuple((t.id, t.parent) for t in self.tracks)

    def next_color(self) -> str:
        return TRACK_COLORS[len(self.tracks) % len(TRACK_COLORS)]

    def unique_track_name(self, base: str) -> str:
        names = {t.name for t in self.tracks} | {t.name for t in self.returns}
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

    def insert_return(self, track: Track, index: int) -> None:
        index = max(0, min(index, len(self.returns)))
        self.returns.insert(index, track)
        self.return_inserted.emit(track.id, index)

    def remove_return(self, track_id: str) -> tuple[Track, int]:
        """Takes a return away (the sends into it go first: see ProjectEditor.delete_tracks)."""
        index = self.return_index(track_id)
        track = self.returns.pop(index)
        self.return_removed.emit(track_id, index)
        return track, index

    def arrange_tracks(self, tree: TrackTree) -> None:
        """Put the tracks in this order and these groups (`tree`: every track's id and
        parent). Raises ValueError, changing nothing, if that isn't a valid tree."""
        by_id = {t.id: t for t in self.tracks}
        if sorted(by_id) != sorted(track_id for track_id, _ in tree):
            raise ValueError("an arrangement lists every track once")
        old_tracks, old_parents = self.tracks, {t.id: t.parent for t in self.tracks}
        self.tracks = [by_id[track_id] for track_id, _ in tree]
        for track_id, parent in tree:
            by_id[track_id].parent = parent
        problem = tree_problem(self.tracks)
        if problem is not None:
            self.tracks = old_tracks
            for track in old_tracks:
                track.parent = old_parents[track.id]
            raise ValueError(problem)
        self.tracks_arranged.emit()

    def update_track(self, track_id: str, **attrs) -> None:
        track = self.track(track_id)
        for name, value in attrs.items():
            if not hasattr(track, name) or name in ("id", "kind", "clips", "devices", "automation",
                                                    "automation_view", "parent"):
                raise AttributeError(name)
            setattr(track, name, value)
        self.track_changed.emit(track_id)

    def set_clips(self, track_id: str, clips: list[AnyClip]) -> None:
        self.track(track_id).clips = sorted(clips, key=lambda c: c.start_beat)
        self.clips_changed.emit(track_id)

    def set_devices(self, track_id: str, devices: list[Device]) -> None:
        self.track(track_id).devices = devices
        self.devices_changed.emit(track_id)

    def set_chains(self, chains: dict[str, list[Device]]) -> None:
        """Change several tracks' devices at once (a device moving between them):
        all of them change before anyone hears of it."""
        for track_id, devices in chains.items():
            self.track(track_id).devices = devices
        for track_id in chains:
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

    def set_device_sidechain(self, track_id: str, device_id: str, sidechain: Sidechain | None) -> None:
        self.device(track_id, device_id).sidechain = sidechain
        self.devices_changed.emit(track_id)

    def set_device_state(self, track_id: str, device_id: str, state: str | None) -> None:
        self.device(track_id, device_id).state = state
        self.device_state_changed.emit(track_id, device_id)

    def update_settings(self, **attrs) -> None:
        for name, value in attrs.items():
            if name not in ("tempo", "time_signature", "key", "loop_enabled", "loop_start", "loop_end", "automation_locked"):
                raise AttributeError(name)
            setattr(self, name, value)
        self.settings_changed.emit()

    def set_envelope(self, owner: str, key: str, points: Envelope) -> None:
        """Replace an envelope; an empty one removes the target's automation."""
        envelopes = self.automation(owner)
        if points:
            envelopes[key] = tuple(points)
        else:
            envelopes.pop(key, None)
        self.automation_changed.emit(owner, key)

    def set_automation_view(self, owner: str, view: AutomationView) -> None:
        self.track(owner).automation_view = view
        self.automation_view_changed.emit(owner)

    def replace_contents(self, *, tempo: float, time_signature: TimeSignature, loop_enabled: bool,
                         loop_start: float, loop_end: float, tracks: list[Track], path: Path | None,
                         master: Track | None = None, automation_locked: bool = False,
                         key: Key | None = None, returns: list[Track] | None = None) -> None:
        self.tempo = tempo
        self.time_signature = time_signature
        self.key = key
        self.loop_enabled = loop_enabled
        self.loop_start = loop_start
        self.loop_end = loop_end
        self.master = master or new_master()
        self.automation_locked = automation_locked
        self.tracks = tracks
        self.returns = returns or []
        self.path = path
        self.reset.emit()

    def clear(self) -> None:
        self.replace_contents(tempo=120.0, time_signature=TimeSignature(), loop_enabled=False, loop_start=0.0,
                              loop_end=16.0, tracks=[], path=None)
