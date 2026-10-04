"""Keeps the C++ engine in sync with the project model and feeds the UI with
engine state (playhead, meters, CPU) through Qt signals.

Everything here runs on the Qt main thread except source decoding, which runs
in a small thread pool; the engine releases the GIL while decoding.

Plug-ins: a device's engine processor lives as long as the device is in its
chain, so a plug-in keeps its state (and open editor) when the chain around it
changes. A device moved to another chain (another track's, or a rack's) takes
its processor along (one engine move_processor, nothing loads again); a rack
moves with its chains and everything in them.

Racks: a rack is an engine rack, its chains engine chains of it (with their
faders), and the devices in them processors there, as on a track's own chain.
A rack's macros are the model's business (they set parameters there). When a plug-in device goes away
(deleted, or its track), its state is kept here, so undo brings it back as it was. Edits made in a plug-in's own
editor come back from the engine as `plugin_param_edited`, for the undo stack; only while
that editor shows (a plug-in may report its own changes as edits, as some do while their
state is restored: a copy pasted, a track duplicated, an undo).

Audio devices (WASAPI or ASIO) open as the preferences describe. An ASIO
driver whose settings change (in its control panel, or its clock) asks to be
reset; the bridge then opens it again, with its new settings.

The master is a track to the engine as to the model (engine track id MASTER):
its devices, mixer and automation go the same way as a track's.

Groups are tracks to the engine too. It knows only where each track's output
goes: into the engine track of the group it is in, or the master.

Return tracks are engine tracks as well, going to the master, and a track's
sends are engine sends into them (at the send's level, before or after the
fader). A send automated without having been set yet is made, silent, so that
its automation plays. Sends are pushed as each track changes, those going away
first, so that no step closes a cycle.

Sidechains: a device's sidechain is the engine processor's, from the source's
engine track, tapped as the model says (after a device that has left the
source: before the fader). They are pushed whenever devices or routes change,
those changing taken away first; one the engine refuses for now (a cycle with a
route another change hasn't undone yet) comes with that change. A processor
moving to another chain gives up its sidechain in the engine until it is there.

Automation: every envelope of a track (or the master) goes to the engine, which
plays it; its target follows it and the value the model holds for it (set by
hand) counts again when the envelope goes. Changing an automated target by hand
overrides its automation, as in Ableton: the engine stops playing that envelope
until automation is re-enabled. Parameters of every kind are described to the
UI as ParamSpecs (model/params.py).

Recording: armed audio tracks with an input record when recording starts; the
engine writes each take to a WAV file in the recordings folder (the project's
"Recordings" folder once it is saved). While it records, the bridge collects
each take's peaks (`live_takes`) for the arrangement's live waveform. When the
recording ends (stopped, or a device change or a locate ended it) the takes
come out as `takes_recorded`, for one undo step that adds them as clips. A
track given an input the device hasn't open gets it: the device opens again
with that input too (ASIO). A track taking its input from another track's
output (or the master's: resampling) records that, in stereo, placed where it
was heard.

MIDI input: every MIDI input connected is opened, but those turned off in the
preferences. MIDI tracks hear their MIDI input (every input, or one, on every
channel or one) while monitored, and record it when armed: their takes come
back with the notes played, for MIDI clips. Their live takes hold the notes so far.
The computer MIDI keyboard (ui/computer_keyboard.py) is one more MIDI input,
COMPUTER_KEYBOARD, always there.
"""

from __future__ import annotations

import base64
import math
import os
import re
from collections.abc import Callable
from dataclasses import dataclass, field, replace
from datetime import datetime
from pathlib import Path

import numpy as np
from PySide6.QtCore import (
    QObject,
    QRunnable,
    QStandardPaths,
    QThreadPool,
    QTimer,
    Signal,
)

from .. import _engine as ge
from ..model import automation
from ..model.automation import MASTER, MIXER_PAN, MIXER_VOLUME
from ..model.editor import RecordedTake, device_is_instrument, device_name
from ..model.params import ParamSpec, chain_specs, format_value, mixer_specs
from ..model.project import (
    POST_FADER,
    PRE_FADER,
    PRE_FX,
    WARP_MODES,
    Clip,
    Device,
    PluginRef,
    Project,
    Send,
    Track,
    find_device,
    iter_chains,
    iter_devices,
)
from ..model.timebase import db_to_gain
from .settings import (
    AudioSettings,
    audio_threads,
    disabled_midi_inputs,
    set_midi_input_disabled,
)

AUDIO_EXTENSIONS = (".wav", ".wave", ".flac", ".mp3")
# Plug-in editors of tracks not shown are hidden but keep running (animating,
# messaging their processors); this many at most, then the ones hidden longest close.
MAX_HIDDEN_EDITORS = 8
_WARP_MODES = {name: ge.WarpMode(index) for index, name in enumerate(WARP_MODES)}
COMPUTER_KEYBOARD = "Computer Keyboard"  # the MIDI input the computer keyboard plays into
_MONITOR_MODES = {"off": ge.MonitorMode.OFF, "in": ge.MonitorMode.IN, "auto": ge.MonitorMode.AUTO}


def recordings_folder(project: Project) -> Path:
    """Where takes go: the project's "Recordings" folder once it is saved,
    else SUBSTATION_RECORDINGS or the user's Music folder."""
    if project.path is not None:
        return Path(project.path).parent / "Recordings"
    if os.environ.get("SUBSTATION_RECORDINGS"):
        return Path(os.environ["SUBSTATION_RECORDINGS"])
    music = QStandardPaths.writableLocation(QStandardPaths.StandardLocation.MusicLocation) or str(Path.home())
    return Path(music) / "SUBstation" / "Recordings"


def take_path(folder: Path, track_name: str, when: datetime) -> Path:
    """A new file for a take: the track's name and the time, numbered if taken."""
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f]', "_", track_name).strip(" .") or "Audio"
    stem = f"{name} {when:%Y-%m-%d %H%M%S}"
    path, n = folder / f"{stem}.wav", 2
    while path.exists():
        path, n = folder / f"{stem} {n}.wav", n + 1
    return path


@dataclass
class LiveTake:
    """A take while it records, as the arrangement draws it: an audio take's
    peaks, or a MIDI take's notes."""

    track_id: str
    start_sample: int = 0  # timeline sample of its first frame
    started: bool = False
    frames: int = 0
    peaks: np.ndarray = field(default_factory=lambda: np.zeros((0, 2), np.float32))  # (min, max) per PEAK_FRAMES
    midi: bool = False
    # A MIDI take's notes so far: rows of (start, end, key, velocity, channel) in
    # timeline samples; a held note's end is -1.
    notes: np.ndarray = field(default_factory=lambda: np.zeros((0, 5), np.int64))
    PEAK_FRAMES = ge.RECORD_PEAK_FRAMES
    _buffer: np.ndarray = field(default_factory=lambda: np.zeros((1024, 2), np.float32), repr=False)

    def add_peaks(self, peaks: np.ndarray) -> None:
        """More peaks; `peaks` stays a view of the ones so far (the buffer grows by doubling)."""
        filled = len(self.peaks)
        if filled + len(peaks) > len(self._buffer):
            grown = np.zeros((max(2 * len(self._buffer), filled + len(peaks)), 2), np.float32)
            grown[:filled] = self._buffer[:filled]
            self._buffer = grown
        self._buffer[filled:filled + len(peaks)] = peaks
        self.peaks = self._buffer[:filled + len(peaks)]


def is_audio_file(path: str) -> bool:
    return path.lower().endswith(AUDIO_EXTENSIONS)


def _key(path: str) -> str:
    return os.path.normcase(os.path.abspath(path))


def clip_desc(clip: Clip) -> ge.ClipDesc:
    """The engine's view of a clip. Positions stay in beats and seconds; the
    engine converts them to samples at the current tempo and sample rate."""
    return ge.ClipDesc(
        clip.path, clip.start_beat, clip.duration_sec, clip.offset_sec, db_to_gain(clip.gain_db),
        pan=clip.pan,
        warp=clip.is_warped,
        segment_bpm=clip.segment_bpm,
        warp_mode=_WARP_MODES.get(clip.warp_mode, ge.WarpMode.STANDARD),
        transpose=clip.transpose + clip.detune / 100.0,
        id=clip.id,
    )


def note_descs(track: Track) -> list[ge.NoteDesc]:
    """The notes a MIDI track plays, from all of its clips, in timeline beats."""
    return [ge.NoteDesc(start, end - start, note.pitch, note.velocity)
            for clip in track.clips for start, end, note in clip.played_notes()]


class _LoadSignals(QObject):
    loaded = Signal(str, object)
    failed = Signal(str, str)


class _LoadTask(QRunnable):
    def __init__(self, engine: ge.Engine, path: str, signals: _LoadSignals):
        super().__init__()
        self.engine = engine
        self.path = path
        self.signals = signals

    def run(self) -> None:
        try:
            source = self.engine.load_source(self.path)
        except Exception as exc:  # noqa: BLE001 - reported to the UI
            self.signals.failed.emit(self.path, str(exc))
        else:
            self.signals.loaded.emit(self.path, source)


class _StateSignals(QObject):
    failed = Signal(str)  # a device's state couldn't be restored: why


class _StateTask(QRunnable):
    """Restores a built-in device's state off the UI thread: it may load files
    (a sampler's sample)."""

    def __init__(self, engine: ge.Engine, processor_id: int, name: str, state: bytes, signals: _StateSignals):
        super().__init__()
        self.engine = engine
        self.processor_id = processor_id
        self.name = name
        self.state = state
        self.signals = signals

    def run(self) -> None:
        try:
            self.engine.set_processor_state(self.processor_id, self.state)
        except ValueError:
            pass  # the device went meanwhile
        except RuntimeError as exc:
            self.signals.failed.emit(f"{self.name}: {exc}")


class EngineBridge(QObject):
    source_ready = Signal(str)  # a file finished decoding (waveform available)
    source_failed = Signal(str, str)
    position_changed = Signal(float)
    transport_changed = Signal(bool)  # playing
    meters_updated = Signal()
    device_changed = Signal()
    status_message = Signal(str)
    # Plug-ins (track id, device id first)
    plugin_param_edited = Signal(str, str, str, float, float, int)  # param id, value, value before, gesture
    plugin_param_touched = Signal(str, str, str)  # param id: taken hold of in the plug-in's editor
    plugin_params_changed = Signal(str, str)  # values changed without edits (a preset, meters): show them
    plugin_params_rebuilt = Signal(str, str)  # the parameter list changed
    plugin_editor_changed = Signal(str, str)  # its editor opened or closed
    plugin_state_dirty = Signal()  # a plug-in changed in a way no edit shows: the project has changes
    devices_loaded = Signal(str)  # track id: its devices' processors were (re)created
    # Automation owner (track id or MASTER): which of its envelopes play, or are overridden, changed.
    automation_state_changed = Signal(str)
    recording_changed = Signal(bool)  # recording started or ended
    recording_updated = Signal()  # live takes grew
    takes_recorded = Signal(list)  # [RecordedTake]: a recording ended with these

    def __init__(self, engine: ge.Engine, project: Project, parent: QObject | None = None):
        super().__init__(parent)
        self.engine = engine
        self.project = project
        self._track_ids: dict[str, int] = {MASTER: ge.MASTER}  # model track id -> engine track id
        # Chain key (a track's id for its own chain, a rack chain's id) -> its engine chain id
        self._chains: dict[str, int] = {MASTER: engine.track_chain(ge.MASTER)}
        self._chain_owner: dict[str, str] = {MASTER: MASTER}  # chain key -> the track it is on
        self._rack_of_chain: dict[str, str] = {}  # rack chain id -> its rack's device id
        self._rack_orders: dict[str, list[int]] = {}  # rack device id -> its engine chains, in the order the engine has
        # chain key -> [(model device id, engine id)]; the engine id is None when a plug-in didn't load
        self._devices: dict[str, list[tuple[str, int | None]]] = {}
        self._pids: dict[str, int | None] = {}  # device id -> its processor (None: not loaded)
        self._where: dict[str, str] = {}  # device id -> the chain key its processor is in
        self._chain_mixer: dict[str, tuple] = {}  # rack chain id -> (volume dB, pan, mute, solo) the engine has
        self._syncing: set[str] = set()  # tracks whose devices are being synced (not again from inside)
        self._enabled: dict[int, bool] = {}  # engine id -> what the engine was told
        self._plugin_ids: dict[int, str] = {}  # engine id of each plug-in processor -> the path it came from
        self._plugin_states: dict[str, bytes] = {}  # device id -> its plug-in's state when it went away
        self._param_ids: dict[int, list[str]] = {}  # engine id -> parameter ids by index (cache)
        self._param_infos: dict[int, list] = {}  # engine id -> its ParamInfos (cache)
        self._param_specs: dict[int, list[ParamSpec]] = {}  # engine id -> its automatable parameters (cache)
        self._automating: dict[str, set[str]] = {}  # owner -> the targets whose envelopes the engine plays
        self._overridden: set[tuple[str, str]] = set()  # (owner, key) changed by hand while automated
        self._mixer: dict[str, tuple[float, float]] = {}  # owner -> (volume dB, pan) the engine has
        self._inputs: dict[str, tuple] = {}  # track id -> (input, source, monitor, armed, MIDI input) the engine has
        self._outputs: dict[str, int] = {}  # track id -> the engine track its output goes into
        self._sends: dict[str, dict[int, tuple[float, bool]]] = {}  # track id -> {engine return: (gain, pre-fader)}
        self._send_levels: dict[str, dict[str, float]] = {}  # track id -> {return id: level dB} the engine has
        # Processor id -> (source engine track, tap, tap processor) of the sidechain the engine has
        self._sidechains: dict[int, tuple[int, ge.SidechainTap, int]] = {}
        self._busy = 0  # > 0 while a plug-in call may run a message loop that calls us back
        self.plugin_errors: dict[str, str] = {}  # device id -> why its plug-in isn't loaded
        self.known_plugins: dict[str, str] = {}  # plug-in uid -> file, from the scan: finds moved plug-ins
        self.owner_window: Callable[[], int] = lambda: 0  # HWND owning plug-in editor windows
        self._editors_wanted: set[str] = set()  # device ids whose editor the user left open
        self._editors_track: str | None = None  # the track whose editors are shown (the selected one)
        self._hidden_editors: list[int] = []  # processor ids of hidden editors, the longest hidden first
        self._sources: dict[str, ge.AudioSource] = {}
        self._loading: dict[str, list[Callable[[], None]]] = {}
        self._failed: dict[str, str] = {}
        self._file_info: dict[str, ge.AudioFileInfo] = {}
        self._preview_request = 0  # the latest preview asked for (or stopped): a file still loading then is not played
        self.meters: dict[str, tuple[float, float]] = {}  # track id or MASTER -> (left, right)
        self.chain_meters: dict[str, tuple[float, float]] = {}  # rack chain id -> (left, right)
        self._last_position = -1.0
        self._last_playing = False
        self._recording: dict[int, str] = {}  # engine track id -> track id, while recording
        self.live_takes: dict[str, LiveTake] = {}  # track id -> its take while recording
        self.midi_errors: dict[str, str] = {}  # MIDI input -> why it couldn't be opened

        self._pool = QThreadPool(self)
        self._pool.setMaxThreadCount(2)
        self._load_signals = _LoadSignals(self)
        self._load_signals.loaded.connect(self._on_loaded)
        self._load_signals.failed.connect(self._on_failed)
        # Built-in devices' states are restored one at a time, in the order they
        # were set, so the last one set wins.
        self._state_pool = QThreadPool(self)
        self._state_pool.setMaxThreadCount(1)
        self._state_signals = _StateSignals(self)
        self._state_signals.failed.connect(self.status_message)

        project.reset.connect(self._on_reset)
        project.track_inserted.connect(lambda tid, _i: self._add_engine_track(project.track(tid)))
        project.track_removed.connect(self._on_track_removed)
        project.return_inserted.connect(lambda tid, _i: self._add_engine_track(project.track(tid)))
        project.return_removed.connect(self._on_track_removed)
        project.track_changed.connect(self._on_track_changed)
        project.tracks_arranged.connect(self._push_outputs)
        project.clips_changed.connect(self._push_clips)
        project.devices_changed.connect(self._sync_devices)
        project.chain_changed.connect(self._on_chain_changed)
        project.device_param_changed.connect(self._on_device_param_changed)
        project.device_state_changed.connect(self._push_device_state)
        project.track_changed.connect(self._update_editor_titles)
        project.settings_changed.connect(self._push_settings)
        project.automation_changed.connect(self._on_automation_changed)

        self._position_timer = QTimer(self)
        self._position_timer.setInterval(16)
        self._position_timer.timeout.connect(self._poll_position)
        self._position_timer.start()
        self._meter_timer = QTimer(self)
        self._meter_timer.setInterval(33)
        self._meter_timer.timeout.connect(self._poll_meters)
        self._meter_timer.start()

        self._on_reset()

    # --- Model -> engine -----------------------------------------------------------

    def _on_reset(self) -> None:
        self._remove_engine_tracks()
        self._devices.clear()
        self._pids.clear()
        self._where.clear()
        self._chain_owner = {MASTER: MASTER}
        self._rack_of_chain.clear()
        self._rack_orders.clear()
        self._chain_mixer.clear()
        self.chain_meters.clear()
        self._enabled.clear()
        self._plugin_ids.clear()
        self._plugin_states.clear()
        self._param_ids.clear()
        self._param_infos.clear()
        self._param_specs.clear()
        self._automating.clear()
        self._overridden.clear()
        self._mixer.clear()
        self._inputs.clear()
        self._outputs.clear()
        self._sends.clear()
        self._send_levels.clear()
        self._sidechains.clear()
        self.plugin_errors.clear()
        self.meters.clear()
        self._editors_wanted.clear()
        self._hidden_editors.clear()
        for track in self.project.all_tracks():
            self._add_engine_track(track)
        self._push_outputs()
        self._push_all_sends()  # (into returns added after the tracks sending to them)
        self._push_all_inputs()  # (from tracks added after the tracks taking them)
        self._push_sidechains()
        self._push_settings()
        # Forget decoded audio the new project doesn't use.
        used = {_key(c.path) for t in self.project.tracks if not t.is_midi for c in t.clips}
        self._sources = {k: s for k, s in self._sources.items() if k in used}
        self.engine.release_unused_sources()

    def _remove_engine_tracks(self) -> None:
        """Every track goes from the engine, with its devices; the master stays, without its devices."""
        self._forget_chain_devices(MASTER, remove=True)
        for track_id, engine_id in list(self._track_ids.items()):
            if track_id != MASTER:
                self._forget_chain_devices(track_id, remove=False)
                self.engine.remove_track(engine_id)
                del self._track_ids[track_id]
                self._drop_chain(track_id)
        self._chain_owner.setdefault(MASTER, MASTER)

    def _add_engine_track(self, track: Track) -> None:
        if not track.is_master:  # the engine always has the master
            self._track_ids[track.id] = self.engine.add_track()
            self._chains[track.id] = self.engine.track_chain(self._track_ids[track.id])
            self._chain_owner[track.id] = track.id
        self._push_mixer(track.id)
        self._push_input(track.id)
        self._push_clips(track.id)
        self._sync_devices(track.id)
        self._push_automation(track.id)
        if not track.is_master:  # into its group, and what is in it (back) into it
            self._push_outputs()
        if track.is_return:  # its sends, and the sends into it
            self._push_all_sends()
        elif not track.is_master:
            self._push_sends(track.id)
        if not track.is_master:  # the inputs taken from it (back)
            self._push_all_inputs()
        self._push_sidechains()  # its devices', and those it is the source of

    def _on_track_removed(self, track_id: str, _index: int) -> None:
        self._forget_chain_devices(track_id, remove=False)
        engine_id = self._track_ids.pop(track_id, None)
        self._drop_chain(track_id)
        if engine_id is not None:
            self.engine.remove_track(engine_id)  # also removes its devices
        self.meters.pop(track_id, None)
        self._automating.pop(track_id, None)
        self._mixer.pop(track_id, None)
        self._inputs.pop(track_id, None)
        self._outputs.pop(track_id, None)
        self._sends.pop(track_id, None)
        self._send_levels.pop(track_id, None)
        # What went into it goes to the engine's master now, and the sends into it
        # and the inputs from it are gone (the model has its say next).
        self._outputs = {t: ge.MASTER if out == engine_id else out for t, out in self._outputs.items()}
        for sends in self._sends.values():
            sends.pop(engine_id, None)
        for track, state in list(self._inputs.items()):
            if state[1] == engine_id:
                self._inputs[track] = (state[0], None, *state[2:])
        self._sidechains = {p: state for p, state in self._sidechains.items() if state[0] != engine_id}
        self._overridden = {(o, k) for o, k in self._overridden if o != track_id}

    def _on_track_changed(self, track_id: str) -> None:
        if track_id not in self._track_ids:
            return
        track = self.project.track(track_id)
        self._override_changed_mixer(track_id, track.volume_db, track.pan)
        self._override_changed_sends(track)
        self._push_mixer(track_id)
        self._push_input(track_id)
        self._push_sends(track_id)
        self._push_sidechains()  # (a route that stood in a sidechain's way may have gone)

    def _override_changed_sends(self, track: Track) -> None:
        """A send level changed by hand while automated: its automation stops."""
        old = self._send_levels.get(track.id)
        new = {return_id: send.level_db for return_id, send in track.sends.items()}
        self._send_levels[track.id] = new
        if old is None:
            return
        for return_id, level in new.items():
            if old.get(return_id) != level:
                self.override_automation(track.id, automation.send_key(return_id))

    def _override_changed_mixer(self, owner: str, volume_db: float, pan: float) -> None:
        """A mixer control changed by hand while automated: its automation stops."""
        old = self._mixer.get(owner)
        if old is None:
            return
        if volume_db != old[0]:
            self.override_automation(owner, MIXER_VOLUME)
        if pan != old[1]:
            self.override_automation(owner, MIXER_PAN)

    def _push_mixer(self, track_id: str) -> None:
        engine_id = self._track_ids.get(track_id)
        if engine_id is None:
            return
        track = self.project.track(track_id)
        self.engine.set_track_gain(engine_id, db_to_gain(track.volume_db))
        self.engine.set_track_pan(engine_id, track.pan)
        if not track.is_master:
            self.engine.set_track_mute(engine_id, track.mute)
            self.engine.set_track_solo(engine_id, track.solo)
        self._mixer[track_id] = (track.volume_db, track.pan)

    def _push_outputs(self) -> None:
        """Every track's output into its group's engine track (or the master).
        Changed routes go to the master first, so that no step closes a cycle
        (a group moving into what was in it)."""
        wanted = {}
        for track in self.project.tracks:
            if track.id in self._track_ids:
                wanted[track.id] = self._track_ids.get(track.parent, ge.MASTER) if track.parent else ge.MASTER
        changed = {t: out for t, out in wanted.items() if self._outputs.get(t, ge.MASTER) != out}
        for track_id in changed:
            if self._outputs.get(track_id, ge.MASTER) != ge.MASTER:
                self.engine.set_track_output(self._track_ids[track_id], ge.MASTER)
                self._outputs[track_id] = ge.MASTER
        for track_id, out in changed.items():
            if out != ge.MASTER:
                self.engine.set_track_output(self._track_ids[track_id], out)
            self._outputs[track_id] = out
        if changed:
            self._push_sidechains()

    def _wanted_sends(self, track: Track) -> dict[int, tuple[float, bool]]:
        """The engine sends a track should have: its sends, and silent ones for
        those automated without having been set (so that the automation plays)."""
        sends = dict(track.sends)
        for key in track.automation:
            return_id = automation.key_send(key)
            if return_id is not None and return_id not in sends and self.project.has_return(return_id) \
                    and not self.project.would_cycle(track.id, return_id):
                sends[return_id] = Send()
        return {self._track_ids[r]: (db_to_gain(send.level_db), send.pre_fader) for r, send in sends.items()
                if r in self._track_ids and self.project.has_return(r)}

    def _push_sends(self, track_id: str) -> None:
        """A track's sends to the engine: those going away first (no step closes a cycle)."""
        engine_id = self._track_ids.get(track_id)
        if engine_id is None or track_id == MASTER:
            return
        wanted = self._wanted_sends(self.project.track(track_id))
        current = self._sends.setdefault(track_id, {})
        for return_engine_id in [r for r in current if r not in wanted]:
            self.engine.remove_track_send(engine_id, return_engine_id)
            del current[return_engine_id]
        for return_engine_id, (gain, pre_fader) in wanted.items():
            if current.get(return_engine_id) == (gain, pre_fader):
                continue
            try:
                self.engine.set_track_send(engine_id, return_engine_id, gain, pre_fader)
            except ValueError:
                continue  # a cycle with a send another track hasn't given up yet: it comes with that track's turn
            current[return_engine_id] = (gain, pre_fader)
        self._send_levels.setdefault(track_id, {r: s.level_db for r, s in self.project.track(track_id).sends.items()})

    def _push_all_sends(self) -> None:
        for track in self.project.senders():
            self._push_sends(track.id)

    def _input_source(self, track: Track) -> int | None:
        """The engine track whose output a track takes as its input (ge.MASTER: the
        master's); None: none, or one the engine hasn't (yet)."""
        if track.input_track is None or not track.is_audio:
            return None
        return ge.MASTER if track.input_track == MASTER else self._track_ids.get(track.input_track)

    def _push_input(self, track_id: str) -> None:
        engine_id = self._track_ids.get(track_id)
        if engine_id is None or track_id == MASTER:
            return
        track = self.project.track(track_id)
        channels = tuple(track.input) if not track.is_midi else ()
        midi_input = track.midi_input if track.is_midi else None
        state = (channels, self._input_source(track), track.monitor, track.armed, midi_input)
        old = self._inputs.get(track_id)
        if state == old:
            return  # (a mixer change)
        if old is None or old[:2] != state[:2]:
            if state[1] is None:
                self.engine.set_track_input(engine_id, list(channels))
            else:
                try:
                    self.engine.set_track_input_track(engine_id, state[1])
                except ValueError:
                    # A cycle with a route another change hasn't undone yet: it comes with that change.
                    self.engine.set_track_input(engine_id, [])
                    state = ((), None, *state[2:])
        self._inputs[track_id] = state
        if old is None or old[2] != state[2]:
            self.engine.set_track_monitor(engine_id, _MONITOR_MODES.get(track.monitor, ge.MonitorMode.AUTO))
        if old is None or old[3] != state[3]:
            self.engine.set_track_armed(engine_id, state[3])
        if old is None or old[4] != state[4]:
            if midi_input is None:
                self.engine.set_track_midi_input(engine_id, False)
            else:
                self.engine.set_track_midi_input(engine_id, True, midi_input.device, midi_input.channel)
        if channels and not self.is_recording:
            self._open_inputs(channels)

    def _push_all_inputs(self) -> None:
        for track in self.project.tracks:
            self._push_input(track.id)

    def _open_inputs(self, channels) -> None:
        """An ASIO device that hasn't these inputs open opens again with them too."""
        status = self.engine.device_status
        if not status.open or status.backend != "ASIO" or set(channels) <= set(status.input_channels):
            return
        names = self.engine.device_capabilities.input_names
        if any(c >= len(names) for c in channels):
            return  # not this device's: silent until a device that has them
        # As it runs now (not as saved: it may have opened with its own settings instead).
        settings = AudioSettings("ASIO", status.name, status.sample_rate, status.buffer_frames,
                                 output_channels=tuple(status.output_channels),
                                 input_channels=tuple(sorted(set(status.input_channels) | set(channels))))
        error = self.open_device(settings)
        if error is None:
            settings.save()
        else:
            self.status_message.emit(f"The input could not be opened: {error}")

    def input_names(self) -> list[str]:
        """The device's inputs, by channel (none while no device is open)."""
        if not self.engine.device_status.open:
            return []
        return list(self.engine.device_capabilities.input_names)

    # --- Audio threads ---------------------------------------------------------------

    def apply_audio_threads(self) -> None:
        """Renders on as many threads as the preferences say (the engine's default unless chosen)."""
        self.engine.audio_threads = audio_threads() or ge.Engine.default_audio_threads()

    # --- MIDI input ------------------------------------------------------------------

    def midi_inputs(self) -> list[str]:
        """The MIDI inputs connected, by name."""
        return list(self.engine.midi_input_devices())

    def midi_input_choices(self) -> list[str]:
        """What a track's MIDI input can be: the inputs connected, and the computer keyboard."""
        return [*self.midi_inputs(), COMPUTER_KEYBOARD]

    def send_midi(self, message: list[int], device: str = COMPUTER_KEYBOARD) -> None:
        """Plays a MIDI message now, as if `device` sent it (dropped while no audio device runs)."""
        self.engine.send_midi_input(device, message)

    def is_midi_input_open(self, name: str) -> bool:
        return name in self.engine.open_midi_inputs()

    def open_midi_inputs(self) -> None:
        """Opens every MIDI input connected but those turned off, and closes those
        turned off (or gone). Inputs that can't be opened are reported once."""
        disabled = disabled_midi_inputs()
        connected = self.midi_inputs()
        for name in self.engine.open_midi_inputs():
            if name in disabled or name not in connected:
                self.engine.close_midi_input(name)
        failed = {}
        for name in connected:
            if name in disabled or self.is_midi_input_open(name):
                continue
            try:
                self.engine.open_midi_input(name)
            except RuntimeError as exc:
                failed[name] = str(exc)
                if name not in self.midi_errors:
                    self.status_message.emit(str(exc))
        self.midi_errors = failed

    def set_midi_input_enabled(self, name: str, enabled: bool) -> None:
        set_midi_input_disabled(name, not enabled)
        self.open_midi_inputs()

    def _push_clips(self, track_id: str) -> None:
        engine_id = self._track_ids.get(track_id)
        if engine_id is None or track_id == MASTER:
            return
        track = self.project.track(track_id)
        if track.is_midi:
            self.engine.set_track_notes(engine_id, note_descs(track))
            return
        for clip in track.clips:
            self.request_source(clip.path)
        self.engine.set_track_clips(engine_id, [clip_desc(c) for c in track.clips])

    # --- Devices ----------------------------------------------------------------------
    # The engine's chains by key: a track's own chain by the track's id, a rack's
    # chain by the chain's (ids are unique in the project). Each device's
    # processor is in the chain the bridge last put it in (`_where`).

    def _model_chains(self) -> dict[str, tuple[str, list[Device]]]:
        """Every chain in the project, by key: (its track, its devices)."""
        chains = {}
        for track in self.project.all_tracks():
            chains[track.id] = (track.id, track.devices)
            for _rack, chain in iter_chains(track.devices):
                chains[chain.id] = (track.id, chain.devices)
        return chains

    def _owned_chains(self, track_id: str) -> list[str]:
        return [key for key, owner in self._chain_owner.items() if owner == track_id]

    def _sync_devices(self, track_id: str) -> None:
        """A track's devices (in racks too) to the engine. Devices still there keep
        their processors (a plug-in keeps its state and editor), and so do devices
        that moved here from another chain (of this track or another, into or out
        of a rack), or a rack with everything in it; new ones get one; the rest go."""
        if track_id not in self._chains or track_id in self._syncing:
            return
        self._syncing.add(track_id)
        changed: set[str] = set()  # chain keys whose devices changed
        try:
            track = self.project.track(track_id)
            before = {key: list(self._devices.get(key, [])) for key in self._owned_chains(track_id)}
            self._place(track_id, track_id, track.devices, changed)  # every chain, top down
            model = self._model_chains()
            for key, entries in before.items():  # what left its chain, and isn't in another of this track's
                wanted = {d.id for d in model[key][1]} if key in model else set()
                for device_id, processor_id in entries:
                    if device_id not in wanted and self._where.get(device_id) == key:
                        self._dispose(track_id, key, device_id, processor_id)
                        changed.add(key)
            for key in self._owned_chains(track_id):  # rack chains that went (their racks stay)
                if key not in model and key != track_id:
                    try:
                        self.engine.remove_rack_chain(self._chains[key])
                    except ValueError:
                        pass
                    self._drop_chain(key)
            for key in [track_id, *(c.id for _, c in iter_chains(track.devices))]:
                ids = [d for d, _ in self._devices.get(key, [])]
                if key in changed or ids != [d for d, _ in before.get(key, [])]:
                    self.engine.set_chain_order(self._chains[key], [p for _, p in self._devices[key] if p is not None])
                    changed.add(key)
            for rack in iter_devices(track.devices):
                processor_id = self._pids.get(rack.id)
                if rack.is_rack and processor_id is not None:
                    order = [self._chains[c.id] for c in rack.chains]
                    if self._rack_orders.get(rack.id) != order:
                        self.engine.set_rack_chain_order(processor_id, order)
                        self._rack_orders[rack.id] = order
        finally:
            self._syncing.discard(track_id)
        if changed:
            self._push_automation(track_id)  # its devices' envelopes go to the new processors
            self._update_editor_titles(track_id)
            self.devices_loaded.emit(track_id)
        self._push_enabled(track)
        self._push_chain_mixers(track_id)
        self._push_sidechains()  # (the new processors', or a sidechain changed)

    def _place(self, track_id: str, key: str, devices: list[Device], changed: set[str]) -> None:
        """A chain's devices into its engine chain (in no particular order yet),
        then the chains of the racks among them."""
        current = {d: p for d, p in self._devices.get(key, []) if self._where.get(d) == key}
        chain = []
        for device in devices:
            if device.id in current:
                processor_id = current[device.id]
            else:
                found, processor_id = self._take_over(track_id, key, device)
                if not found:
                    processor_id = self._create_processor(self._chains[key], device)
                    self._pids[device.id] = processor_id
                self._where[device.id] = key
                changed.add(key)
            chain.append((device.id, processor_id))
        self._devices[key] = chain
        for device, (_, processor_id) in zip(devices, chain, strict=True):
            if device.is_rack and processor_id is not None:
                self._place_rack(track_id, device, processor_id, changed)

    def _place_rack(self, track_id: str, rack: Device, processor_id: int, changed: set[str]) -> None:
        for chain in rack.chains:
            engine_chain = self._chains.get(chain.id)
            if engine_chain is not None and self._rack_of_chain.get(chain.id) != rack.id:
                # The chain is another rack's now: a new engine chain, with its devices.
                moved = self.engine.add_rack_chain(processor_id)
                for _, inner in self._devices.get(chain.id, []):
                    if inner is not None:
                        self.engine.move_processor(inner, moved)
                try:
                    self.engine.remove_rack_chain(engine_chain)
                except ValueError:
                    pass
                engine_chain = moved
            elif engine_chain is None:
                engine_chain = self.engine.add_rack_chain(processor_id)
                self._devices[chain.id] = []
                changed.add(chain.id)
            self._chains[chain.id] = engine_chain
            self._chain_owner[chain.id] = track_id
            self._rack_of_chain[chain.id] = rack.id
            self._place(track_id, chain.id, chain.devices, changed)

    def _take_over(self, track_id: str, key: str, device: Device) -> tuple[bool, int | None]:
        """A device new to a chain: whether its processor is in another chain (of
        any track: it moved here), and which then; it moves along in the engine
        (a rack with its chains and everything in them)."""
        old = self._where.get(device.id)
        if old is None or old == key or device.id not in self._pids:
            return False, None
        processor_id = self._pids[device.id]
        self._devices[old] = [(d, p) for d, p in self._devices.get(old, []) if d != device.id]
        if processor_id is not None:
            if self._chain_owner.get(old) != track_id:  # its sidechains come back once there (if they can)
                for inner in iter_devices([device]):
                    if self._pids.get(inner.id) is not None:
                        self._drop_sidechain(self._pids[inner.id])
            self.engine.move_processor(processor_id, self._chains[key])
        for _rack, chain in iter_chains([device]):
            if chain.id in self._chain_owner:
                self._chain_owner[chain.id] = track_id
        return True, processor_id

    def _dispose(self, track_id: str, key: str, device_id: str, processor_id: int | None) -> None:
        """A device left a chain of this track and isn't on it any more: if it went
        to another track, that track takes it over now; otherwise it goes (and a
        rack with what is in it, but what of that went elsewhere)."""
        owner = self.project.device_owner(device_id)
        if owner is not None and owner != track_id and owner in self._chains and owner not in self._syncing:
            self._sync_devices(owner)
            if self._where.get(device_id) != key:
                return
        for chain in self._chains_of(device_id):
            for inner, inner_id in list(self._devices.get(chain, [])):
                if self._where.get(inner) == chain:
                    self._dispose(track_id, chain, inner, inner_id)
        self._forget_processor(device_id, processor_id)

    def _chains_of(self, rack_id: str) -> list[str]:
        return [chain for chain, rack in self._rack_of_chain.items() if rack == rack_id]

    def _drop_chain(self, key: str) -> None:
        for mapping in (self._chains, self._devices, self._chain_owner, self._rack_of_chain, self._chain_mixer,
                        self.chain_meters):
            mapping.pop(key, None)

    def has_sidechain_input(self, track_id: str, device_id: str) -> bool:
        """Whether a device has a sidechain (aux) input (not while its plug-in isn't loaded)."""
        processor_id = self.engine_device_id(track_id, device_id)
        return processor_id is not None and self.engine.processor_info(processor_id).has_sidechain

    def _wanted_sidechain(self, device: Device, processor_id: int) -> tuple[int, ge.SidechainTap, int] | None:
        """The sidechain the engine should give a device's processor (None: none, or
        one from a track the engine hasn't yet)."""
        sidechain = device.sidechain
        if sidechain is None or sidechain.track_id == MASTER:
            return None
        source = self._track_ids.get(sidechain.track_id)
        if source is None or not self.engine.processor_info(processor_id).has_sidechain:
            return None
        if sidechain.tap == POST_FADER:
            return source, ge.SidechainTap.POST_FADER, 0
        if sidechain.tap == PRE_FX:
            # A MIDI track's own audio is its instrument's (or its instrument rack's): before its effects.
            devices = self.project.track(sidechain.track_id).devices
            if devices and device_is_instrument(devices[0]):
                instrument = self.engine_device_id(sidechain.track_id, devices[0].id)
                if instrument is not None:
                    return source, ge.SidechainTap.AFTER_DEVICE, instrument
            return source, ge.SidechainTap.PRE_FX, 0
        tapped = None if sidechain.tap == PRE_FADER else self.engine_device_id(sidechain.track_id, sidechain.tap)
        if tapped is None:  # before the fader (also while the device it is taken after isn't on the source)
            return source, ge.SidechainTap.PRE_FADER, 0
        return source, ge.SidechainTap.AFTER_DEVICE, tapped

    def _push_sidechains(self) -> None:
        """Every device's sidechain to the engine (devices in racks too): those
        changing go first, so that no step closes a cycle."""
        wanted = {}
        for track in self.project.all_tracks():
            for device in iter_devices(track.devices):
                processor_id = self.engine_device_id(track.id, device.id) if device.sidechain is not None else None
                if processor_id is not None:
                    state = self._wanted_sidechain(device, processor_id)
                    if state is not None:
                        wanted[processor_id] = state
        for processor_id in [p for p, state in self._sidechains.items() if wanted.get(p) != state]:
            self._drop_sidechain(processor_id)
        for processor_id, state in wanted.items():
            if processor_id in self._sidechains:
                continue
            try:
                self.engine.set_processor_sidechain(processor_id, *state)
            except ValueError:
                continue  # a cycle with a route another change hasn't undone yet: it comes with that change
            self._sidechains[processor_id] = state

    def _drop_sidechain(self, processor_id: int) -> None:
        if self._sidechains.pop(processor_id, None) is not None:
            self.engine.clear_processor_sidechain(processor_id)

    def _push_enabled(self, track: Track) -> None:
        for device in iter_devices(track.devices):
            processor_id = self.engine_device_id(track.id, device.id)
            if processor_id is not None and self._enabled.get(processor_id) != device.enabled:
                self.engine.set_processor_enabled(processor_id, device.enabled)
                self._enabled[processor_id] = device.enabled

    def _on_chain_changed(self, track_id: str, chain_id: str) -> None:
        chain = self.project.chain(track_id, chain_id)
        old = self._chain_mixer.get(chain_id)
        if old is not None:  # changed by hand while automated: its automation stops
            rack = self.project.chain_rack(track_id, chain_id)
            if chain.volume_db != old[0]:
                self.override_automation(track_id, automation.chain_key(rack.id, chain_id, automation.CHAIN_VOLUME))
            if chain.pan != old[1]:
                self.override_automation(track_id, automation.chain_key(rack.id, chain_id, automation.CHAIN_PAN))
        self._push_chain_mixers(track_id)

    def _push_chain_mixers(self, track_id: str) -> None:
        """A track's rack chains' faders to the engine (those that changed)."""
        if not self.project.has_owner(track_id):
            return
        for _rack, chain in iter_chains(self.project.track(track_id).devices):
            engine_chain = self._chains.get(chain.id)
            state = (chain.volume_db, chain.pan, chain.mute, chain.solo)
            old = self._chain_mixer.get(chain.id)
            if engine_chain is None or old == state:
                continue
            if old is None or old[0] != state[0]:
                self.engine.set_chain_gain(engine_chain, db_to_gain(chain.volume_db))
            if old is None or old[1] != state[1]:
                self.engine.set_chain_pan(engine_chain, chain.pan)
            if old is None or old[2] != state[2]:
                self.engine.set_chain_mute(engine_chain, chain.mute)
            if old is None or old[3] != state[3]:
                self.engine.set_chain_solo(engine_chain, chain.solo)
            self._chain_mixer[chain.id] = state

    def _create_processor(self, chain_id: int, device: Device) -> int | None:
        if device.is_rack:  # (its chains come next: _place_rack)
            try:
                return self.engine.add_rack(chain_id)
            except ValueError as exc:  # nested too deep (a file edited by hand)
                self.status_message.emit(str(exc))
                return None
        if device.is_plugin:
            return self._load_plugin(chain_id, device)
        try:
            processor_id = self.engine.add_builtin_processor(chain_id, device.kind)
        except (RuntimeError, ValueError):  # a device of a newer version (a preset, a project): missing, as a plug-in
            self._plugin_failed(device, f"{device_name(device)} is not a device this version of SUBstation has.")
            return None
        for param_id, value in device.params.items():
            self._set_param(processor_id, param_id, value)
        if device.state:
            self._set_builtin_state(processor_id, device)
        return processor_id

    def plugin_path(self, plugin: PluginRef) -> str | None:
        """Where a plug-in is now: where it was, or where the scan found it."""
        if plugin.path and os.path.exists(plugin.path):
            return plugin.path
        return self.known_plugins.get(plugin.uid)

    def _load_plugin(self, chain_id: int, device: Device) -> int | None:
        plugin = device.plugin
        path = self.plugin_path(plugin)
        if path is None:
            self._plugin_failed(device, f"{plugin.name} is not installed.")
            return None
        self._busy += 1
        try:
            processor_id = self.engine.add_plugin_processor(chain_id, plugin.format, path, plugin.uid)
        except (RuntimeError, ValueError) as exc:
            self._plugin_failed(device, f"{plugin.name} could not be loaded: {exc}")
            return None
        finally:
            self._busy -= 1
        self._plugin_ids[processor_id] = path
        self.plugin_errors.pop(device.id, None)
        # Its state as it was when the device went away (undo), else as saved.
        state = self._plugin_states.pop(device.id, None)
        if state is None and device.state:
            try:
                state = base64.b64decode(device.state)
            except ValueError:
                state = None
        if state:
            self._set_plugin_state(processor_id, plugin.name, state)
        return processor_id

    def _plugin_failed(self, device: Device, message: str) -> None:
        self.plugin_errors[device.id] = message
        self.status_message.emit(message)

    def _set_plugin_state(self, processor_id: int, name: str, state: bytes) -> None:
        self._busy += 1
        try:
            self.engine.set_processor_state(processor_id, state)
        except (RuntimeError, ValueError) as exc:
            self.status_message.emit(f"{name}: its settings could not be restored ({exc})")
        finally:
            self._busy -= 1

    def _forget_processor(self, device_id: str, processor_id: int | None, remove: bool = True) -> None:
        """A device's processor goes away (with its track if not `remove`; a rack
        with its chains and what is still in them). A plug-in's state is kept, in
        case the device comes back (undo), but not its editor: undo and redo
        don't open editors."""
        for chain in self._chains_of(device_id):  # (the engine removes them with the rack)
            for inner, inner_id in self._devices.get(chain, []):
                if self._where.get(inner) == chain:
                    self._forget_processor(inner, inner_id, remove=False)
            self._drop_chain(chain)
        self._rack_orders.pop(device_id, None)
        self._pids.pop(device_id, None)
        self._where.pop(device_id, None)
        self.plugin_errors.pop(device_id, None)
        self._editors_wanted.discard(device_id)
        if processor_id is None:
            return
        if processor_id in self._hidden_editors:
            self._hidden_editors.remove(processor_id)
        if processor_id in self._plugin_ids:
            try:
                self._plugin_states[device_id] = self.engine.processor_state(processor_id)
            except RuntimeError:
                pass
            del self._plugin_ids[processor_id]
        self._enabled.pop(processor_id, None)
        self._sidechains.pop(processor_id, None)
        self._param_ids.pop(processor_id, None)
        self._param_infos.pop(processor_id, None)
        self._param_specs.pop(processor_id, None)
        if remove:
            self.engine.remove_processor(processor_id)

    def _forget_chain_devices(self, key: str, remove: bool) -> None:
        """Every device of a chain goes (a track's own chain: with its track if not `remove`)."""
        for device_id, processor_id in list(self._devices.get(key, [])):
            if self._where.get(device_id) == key:
                self._forget_processor(device_id, processor_id, remove)
        self._devices.pop(key, None)

    def engine_device_id(self, track_id: str, device_id: str) -> int | None:
        """A device's processor, if it is on that track (in a rack too) and loaded."""
        key = self._where.get(device_id)
        if key is None or self._chain_owner.get(key) != track_id:
            return None
        return self._pids.get(device_id)

    def engine_chain_id(self, chain_id: str) -> int | None:
        """A rack chain's engine chain."""
        return self._chains.get(chain_id) if chain_id in self._rack_of_chain else None

    def device_param_info(self, track_id: str, device_id: str, param_id: str):
        """A device's parameter as the engine describes it (ParamInfo; None: not loaded, or no such one)."""
        processor_id = self.engine_device_id(track_id, device_id)
        if processor_id is None:
            return None
        return next((p for p in self.param_infos(processor_id) if p.id == param_id), None)

    def _on_device_param_changed(self, track_id: str, device_id: str, param_id: str) -> None:
        self.override_automation(track_id, automation.device_key(device_id, param_id))
        self._push_device_param(track_id, device_id, param_id)

    def _push_device_param(self, track_id: str, device_id: str, param_id: str) -> None:
        engine_id = self.engine_device_id(track_id, device_id)
        value = self.project.device(track_id, device_id).params.get(param_id)
        if engine_id is None or value is None:
            return
        if engine_id in self._plugin_ids:
            index = self.engine.processor_param_index(engine_id, param_id)
            if index < 0 or self.engine.processor_param(engine_id, index) == value:
                return  # an edit made in the plug-in's own editor: it has the value already
            self.engine.set_processor_param(engine_id, index, value)
            return
        self._set_param(engine_id, param_id, value)

    def _set_param(self, processor_id: int, param_id: str, value: float) -> None:
        index = self.engine.processor_param_index(processor_id, param_id)
        if index >= 0:
            self.engine.set_processor_param(processor_id, index, value)

    def _push_device_state(self, track_id: str, device_id: str) -> None:
        engine_id = self.engine_device_id(track_id, device_id)
        device = self.project.device(track_id, device_id)
        if engine_id is None:
            return
        if not device.is_plugin:
            self._set_builtin_state(engine_id, device)
        elif device.state:
            self._set_plugin_state(engine_id, device.plugin.name, base64.b64decode(device.state))

    def _set_builtin_state(self, processor_id: int, device: Device) -> None:
        """A built-in device's state is the model's (none: its defaults); it is
        restored in the background, as it may load files."""
        try:
            state = base64.b64decode(device.state) if device.state else b""
        except ValueError:
            state = b""
        self._state_pool.start(_StateTask(self.engine, processor_id, device_name(device), state, self._state_signals))

    def wait_for_device_states(self) -> None:
        """Until every built-in device's state is restored (before rendering offline)."""
        self._state_pool.waitForDone()

    # --- Plug-ins ------------------------------------------------------------------

    def set_known_plugins(self, plugins) -> None:
        """The scanned plug-ins (PluginInfo): lets projects find plug-ins that moved.
        Devices whose plug-in wasn't found get another try."""
        self.known_plugins = {p.uid: p.path for p in plugins}
        reloaded_tracks = set()
        for key, (track_id, devices) in self._model_chains().items():
            chain = self._devices.get(key)
            if chain is None or not any(pid is None for _, pid in chain):
                continue
            by_id = {d.id: d for d in devices}
            reloaded = []
            for device_id, processor_id in chain:
                device = by_id.get(device_id)
                if processor_id is None and device is not None and device.is_plugin and self.plugin_path(device.plugin):
                    processor_id = self._pids[device_id] = self._load_plugin(self._chains[key], device)
                reloaded.append((device_id, processor_id))
            if reloaded != chain:
                self._devices[key] = reloaded
                self.engine.set_chain_order(self._chains[key], [pid for _, pid in reloaded if pid is not None])
                reloaded_tracks.add(track_id)
        for track_id in reloaded_tracks:
            self._push_enabled(self.project.track(track_id))
            self._push_automation(track_id)
            self.devices_loaded.emit(track_id)

    def plugin_state(self, track_id: str, device_id: str) -> bytes | None:
        """A plug-in device's current state (a .vstpreset), None if it isn't loaded."""
        engine_id = self.engine_device_id(track_id, device_id)
        if engine_id is None or engine_id not in self._plugin_ids:
            return None
        return self.engine.processor_state(engine_id)

    def store_plugin_states(self, device_ids=None) -> None:
        """Copy every plug-in's state (or those of `device_ids`) into the model, for
        saving the project (or copying the devices)."""
        for track in self.project.all_tracks():
            for device in iter_devices(track.devices):
                if device_ids is not None and device.id not in device_ids:
                    continue
                engine_id = self.engine_device_id(track.id, device.id)
                if engine_id is None or engine_id not in self._plugin_ids:
                    continue
                try:
                    device.state = base64.b64encode(self.engine.processor_state(engine_id)).decode("ascii")
                except RuntimeError as exc:
                    self.status_message.emit(f"{device.plugin.name}: {exc}")
                path = self._plugin_ids[engine_id]
                if device.plugin.path != path:  # found somewhere else: remember where
                    device.plugin = replace(device.plugin, path=path)

    def param_id(self, processor_id: int, index: int) -> str | None:
        ids = self._param_ids.get(processor_id)
        if ids is None:
            ids = self._param_ids[processor_id] = [p.id for p in self.engine.processor_params(processor_id)]
        return ids[index] if 0 <= index < len(ids) else None

    def _editor_title(self, track_id: str, device_id: str) -> str:
        device = self.project.device(track_id, device_id)
        name = device.plugin.name if device.plugin else device.kind
        return f"{name} - {self.project.track(track_id).name}"

    def open_plugin_editor(self, track_id: str, device_id: str, report: bool = True) -> bool:
        engine_id = self.engine_device_id(track_id, device_id)
        if engine_id is None:
            return False
        self._busy += 1
        try:
            opened = self.engine.open_editor(engine_id, self.owner_window(), self._editor_title(track_id, device_id))
        finally:
            self._busy -= 1
        if opened:
            self._editors_wanted.add(device_id)
        else:
            self._editors_wanted.discard(device_id)
            if report:
                self.status_message.emit(f"{self.project.device(track_id, device_id).plugin.name} has no editor.")
        self.plugin_editor_changed.emit(track_id, device_id)
        return opened

    def close_plugin_editor(self, track_id: str, device_id: str) -> None:
        self._editors_wanted.discard(device_id)
        engine_id = self.engine_device_id(track_id, device_id)
        if engine_id is not None:
            if engine_id in self._hidden_editors:
                self._hidden_editors.remove(engine_id)
            self.engine.close_editor(engine_id)
            self.plugin_editor_changed.emit(track_id, device_id)

    def request_plugin_editor(self, track_id: str, device_id: str) -> None:
        """Open a plug-in's editor now if its track is the one shown, or when it is."""
        if track_id == self._editors_track:
            self.open_plugin_editor(track_id, device_id, report=False)  # having none is fine here
        else:
            self._editors_wanted.add(device_id)

    def show_plugin_editors(self, track_id: str | None) -> None:
        """Show the editors of one track (the selected one): the ones the user left
        open there come back where they were, and every other track's are hidden
        until that track is shown again. Hidden editors keep running (they come back
        as they were), but only MAX_HIDDEN_EDITORS of them: beyond that, the ones
        hidden longest are closed, and open again (where they were) when shown."""
        if track_id == self._editors_track:
            return
        self._editors_track = track_id
        # A copy: opening an editor may run a message loop that changes the chains.
        for chain_track, chain in [(self._chain_owner.get(k), list(c)) for k, c in self._devices.items()]:
            for device_id, processor_id in chain:
                if processor_id not in self._plugin_ids:
                    continue
                if chain_track != track_id:
                    if self.engine.is_editor_open(processor_id):
                        self.engine.set_editor_visible(processor_id, False)
                        self._hidden_editors.append(processor_id)
                        self.plugin_editor_changed.emit(chain_track, device_id)
                elif device_id in self._editors_wanted and not self.engine.is_editor_open(processor_id):
                    if processor_id in self._hidden_editors:
                        self._hidden_editors.remove(processor_id)
                    if self.engine.set_editor_visible(processor_id, True):
                        self.plugin_editor_changed.emit(chain_track, device_id)
                    else:  # it was closed meanwhile (too many hidden, or its plug-in reloaded)
                        self.open_plugin_editor(chain_track, device_id, report=False)
        while len(self._hidden_editors) > MAX_HIDDEN_EDITORS:
            self.engine.close_editor(self._hidden_editors.pop(0))  # still wanted: it reopens when shown

    def is_plugin_editor_open(self, track_id: str, device_id: str) -> bool:
        engine_id = self.engine_device_id(track_id, device_id)
        return engine_id is not None and self.engine.is_editor_open(engine_id)

    def close_all_editors(self) -> None:
        self._hidden_editors.clear()
        for chain in self._devices.values():
            for _, processor_id in chain:
                if processor_id in self._plugin_ids:
                    self.engine.close_editor(processor_id)

    def shutdown(self) -> None:
        """Unload every plug-in now, while the application is still whole (not
        whenever the engine happens to be garbage-collected)."""
        self.close_all_editors()
        self.wait_for_device_states()
        self._remove_engine_tracks()
        self._devices.clear()
        self._plugin_ids.clear()
        for name in self.engine.open_midi_inputs():
            self.engine.close_midi_input(name)
        self.engine.idle()

    def _update_editor_titles(self, track_id: str) -> None:
        for key in self._owned_chains(track_id):
            for device_id, processor_id in self._devices.get(key, []):
                if processor_id in self._plugin_ids:  # hidden editors too; no-op without one
                    self.engine.set_editor_title(processor_id, self._editor_title(track_id, device_id))

    def _dispatch_processor_events(self) -> None:
        events = self.engine.take_processor_events()
        if not events:
            return
        places = {pid: (self._chain_owner[key], did) for key, chain in self._devices.items() for did, pid in chain
                  if pid is not None and key in self._chain_owner}
        changed: dict[tuple[str, str], None] = {}
        dirty = False
        kind = ge.ProcessorEventType
        for event in events:
            place = places.get(event.processor_id)
            if place is None:
                continue
            if event.type in (kind.PARAM_EDITED, kind.PARAM_TOUCHED) and not self.engine.is_editor_open(
                    event.processor_id):
                # Not the user (who edits in the editor): the plug-in itself, as some do when
                # their state is restored. Not an edit to undo; its parameters show anew.
                changed[place] = None
                continue
            if event.type == kind.PARAM_EDITED:
                param_id = self.param_id(event.processor_id, event.param_index)
                if param_id is not None:
                    self.plugin_param_edited.emit(*place, param_id, event.value, event.old_value, event.gesture)
            elif event.type == kind.PARAM_TOUCHED:
                param_id = self.param_id(event.processor_id, event.param_index)
                if param_id is not None:
                    self.plugin_param_touched.emit(*place, param_id)
            elif event.type in (kind.PARAMS_CHANGED, kind.LATENCY_CHANGED):
                changed[place] = None
            elif event.type == kind.PARAM_INFO_CHANGED:
                self._param_ids.pop(event.processor_id, None)
                self._param_infos.pop(event.processor_id, None)
                self._param_specs.pop(event.processor_id, None)
                self._push_automation(place[0])  # its parameters may be elsewhere in the list now
                self.plugin_params_rebuilt.emit(*place)
            elif event.type == kind.EDITOR_CLOSED:
                self._editors_wanted.discard(place[1])
                self.plugin_editor_changed.emit(*place)
            elif event.type == kind.EDITOR_REQUESTED:  # like any editor, shown with its track
                self.request_plugin_editor(*place)
            elif event.type == kind.STATE_DIRTY:
                dirty = True
        for place in changed:
            self.plugin_params_changed.emit(*place)
        if dirty:
            self.plugin_state_dirty.emit()

    def _push_settings(self) -> None:
        p = self.project
        self.engine.tempo = p.tempo
        self.engine.set_time_signature(p.time_signature.numerator, p.time_signature.denominator)
        self.engine.set_loop(p.loop_enabled, p.loop_start, p.loop_end)

    # --- Automation ------------------------------------------------------------------

    def _on_automation_changed(self, owner: str, key: str) -> None:
        if automation.key_send(key) is not None:
            self._push_sends(owner)  # a send automated before it was set is made
        self._push_automation(owner)

    def _push_automation(self, owner: str) -> None:
        """The owner's envelopes to the engine, but those overridden. Targets whose
        envelope no longer plays go back to their own value."""
        engine_id = self._track_ids.get(owner)
        if engine_id is None:
            return
        lanes, playing = [], set()
        for key, points in self.project.automation(owner).items():
            if not points or (owner, key) in self._overridden:
                continue
            lane = self._engine_lane(owner, key, points)
            if lane is not None:
                lanes.append(lane)
                playing.add(key)
        self.engine.set_track_automation(engine_id, lanes)
        stopped = self._automating.get(owner, set()) - playing
        self._automating[owner] = playing
        for key in stopped:
            self._push_own_value(owner, key)
        self.automation_state_changed.emit(owner)

    def _engine_lane(self, owner: str, key: str, points) -> ge.AutomationLane | None:
        try:
            target = automation.parse_key(key)
        except ValueError:
            return None
        engine_points = [ge.AutomationPoint(p.beat, p.value, p.curve) for p in points]
        if target[0] == "mixer":
            return ge.AutomationLane(0, target[1], engine_points)
        if target[0] == "send":
            return_id = self._track_ids.get(target[1])
            return None if return_id is None else ge.AutomationLane(0, f"send:{return_id}", engine_points)
        processor_id = self.engine_device_id(owner, target[1])
        if (control := automation.key_chain_control(key)) is not None:  # a rack chain's fader
            chain = self.engine_chain_id(control[0])
            if processor_id is None or chain is None:
                return None
            return ge.AutomationLane(processor_id, f"chain:{chain}:{control[1]}", engine_points)
        return None if processor_id is None else ge.AutomationLane(processor_id, target[2], engine_points)

    def _push_own_value(self, owner: str, key: str) -> None:
        """A target no longer automated: back to the value it has in the model."""
        if key in automation.MIXER_KEYS:
            if self.project.has_owner(owner):
                self._push_mixer(owner)
            return
        if automation.key_send(key) is not None or automation.key_chain(key) is not None:
            return  # the engine kept the send's (or the chain's fader's) own level
        device_id = automation.key_device(key)
        if self.project.has_device(owner, device_id):
            self._push_device_param(owner, device_id, automation.parse_key(key)[2])

    def is_automated(self, owner: str, key: str) -> bool:
        """Whether the engine plays this target's envelope (it has one, not overridden)."""
        return key in self._automating.get(owner, ())

    def is_overridden(self, owner: str, key: str) -> bool:
        return (owner, key) in self._overridden

    @property
    def has_overrides(self) -> bool:
        return bool(self._overridden)

    def override_automation(self, owner: str, key: str) -> None:
        """The target was changed by hand: its automation stops until re-enabled."""
        if self.is_automated(owner, key):
            self._overridden.add((owner, key))
            self._push_automation(owner)

    def re_enable_automation(self, owner: str | None = None) -> None:
        """Automation plays again where it was overridden (everywhere, or for one owner)."""
        owners = {o for o, _ in self._overridden if owner is None or o == owner}
        self._overridden = {(o, k) for o, k in self._overridden if o not in owners}
        for o in owners:
            self._push_automation(o)

    def param_infos(self, processor_id: int) -> list:
        infos = self._param_infos.get(processor_id)
        if infos is None:
            infos = self._param_infos[processor_id] = list(self.engine.processor_params(processor_id))
        return infos

    def plugin_param_text(self, processor_id: int, index: int, value: float) -> str:
        """A plug-in's text for a value of its parameter, with its unit."""
        info = self.param_infos(processor_id)[index]
        text = self.engine.processor_param_text(processor_id, index, value)
        if not text:
            return f"{value:.2f}"
        return text if not info.unit or info.unit in text else f"{text} {info.unit}"

    def device_param_specs(self, track_id: str, device: Device) -> list[ParamSpec]:
        """The parameters of a device that can be automated (none if it isn't
        loaded); a rack's: its chains' faders."""
        if device.is_rack:
            return chain_specs(device.id, [(c.id, c.name) for c in device.chains], device_name(device))
        processor_id = self.engine_device_id(track_id, device.id)
        if processor_id is None:
            return []
        specs = self._param_specs.get(processor_id)
        if specs is None:
            name = device_name(device)
            is_plugin = processor_id in self._plugin_ids
            specs = []
            for index, info in enumerate(self.param_infos(processor_id)):
                if not info.automatable or info.hidden or info.read_only:
                    continue
                text = (lambda v, i=index: self.plugin_param_text(processor_id, i, v)) if is_plugin else None
                specs.append(ParamSpec.from_info(info, automation.device_key(device.id, info.id), name, text))
            self._param_specs[processor_id] = specs
        return specs

    def mixer_specs(self, owner: str) -> list[ParamSpec]:
        """An owner's mixer controls: volume, pan, and its sends (to the returns it can send to)."""
        sends = tuple((r.id, self.project.return_letter(r.id)) for r in self.project.send_targets(owner)) \
            if self.project.has_owner(owner) else ()
        return mixer_specs(master=owner == MASTER, sends=sends)

    def param_groups(self, owner: str) -> list[tuple[str, str, list[ParamSpec]]]:
        """What an owner has that can be automated, as (group id, name, specs): its
        mixer ("mixer", with its sends), then each device (by id)."""
        groups = [("mixer", "Mixer", self.mixer_specs(owner))]
        for device in iter_devices(self.project.track(owner).devices):
            groups.append((device.id, device_name(device), self.device_param_specs(owner, device)))
        return groups

    def can_automate(self, owner: str, key: str) -> bool:
        return any(spec.key == key for _, _, specs in self.param_groups(owner) for spec in specs)

    def param_spec(self, owner: str, key: str) -> ParamSpec | None:
        """A target's description; None if it doesn't exist (a device that is gone)."""
        if key in automation.MIXER_KEYS:  # every owner has these: no need to work out its sends
            return mixer_specs(master=owner == MASTER)[automation.MIXER_KEYS.index(key)]
        if automation.is_mixer_key(key):
            return next((s for s in self.mixer_specs(owner) if s.key == key), None)  # (a return that is gone)
        if not self.project.has_owner(owner):
            return None
        device_id = automation.key_device(key)
        device = find_device(self.project.track(owner).devices, device_id)
        if device is None:
            return None
        spec = next((s for s in self.device_param_specs(owner, device) if s.key == key), None)
        if spec is None:  # not loaded: its values are still worth showing
            param_id = automation.parse_key(key)[2]
            spec = ParamSpec(key, param_id, device_name(device), text=lambda v: format_value(v, ""))
        return spec

    def own_value(self, owner: str, key: str) -> float | None:
        """A target's value as set by hand (plain; None if not known)."""
        if not self.project.has_owner(owner):
            return None
        track = self.project.track(owner)
        if key == MIXER_VOLUME:
            return track.volume_db
        if key == MIXER_PAN:
            return track.pan
        if (return_id := automation.key_send(key)) is not None:
            return track.sends.get(return_id, Send()).level_db
        if (control := automation.key_chain_control(key)) is not None:
            chain = next((c for _, c in iter_chains(track.devices) if c.id == control[0]), None)
            return None if chain is None else chain.volume_db if control[1] == automation.CHAIN_VOLUME else chain.pan
        target = automation.parse_key(key)
        device = find_device(track.devices, target[1])
        if device is None:
            return None
        processor_id = self.engine_device_id(owner, device.id)
        if processor_id in self._plugin_ids:  # its values live in the plug-in
            index = self.engine.processor_param_index(processor_id, target[2])
            if index >= 0:
                return self.engine.processor_param(processor_id, index)
        value = device.params.get(target[2])
        if value is None and processor_id is not None:
            index = self.engine.processor_param_index(processor_id, target[2])
            if index >= 0:
                value = self.param_infos(processor_id)[index].default_value
        return value

    def current_value(self, owner: str, key: str, beat: float | None = None) -> float | None:
        """What a target is at `beat` (default: the playhead): its envelope's value
        while that plays, else its own (plain; None if not known)."""
        spec = self.param_spec(owner, key)
        if spec is not None and self.is_automated(owner, key):
            value = automation.value_at(self.project.envelope(owner, key), self.position if beat is None else beat)
            if value is not None:
                return spec.from_normalized(spec.quantize(value))
        return self.own_value(owner, key)

    # --- Sources -----------------------------------------------------------------

    def source(self, path: str) -> ge.AudioSource | None:
        return self._sources.get(_key(path))

    def is_loading(self, path: str) -> bool:
        return _key(path) in self._loading

    def load_error(self, path: str) -> str | None:
        return self._failed.get(_key(path))

    def request_source(self, path: str, then: Callable[[], None] | None = None) -> None:
        """Decode `path` in the background (once); `then` runs on the UI thread when ready."""
        key = _key(path)
        source = self._sources.get(key)
        if source is not None and source.sample_rate == int(self.engine.sample_rate):
            if then:
                then()
            return
        if key in self._loading:
            if then:
                self._loading[key].append(then)
            return
        self._failed.pop(key, None)
        self._loading[key] = [then] if then else []
        self._pool.start(_LoadTask(self.engine, path, self._load_signals))

    def _on_loaded(self, path: str, source: ge.AudioSource) -> None:
        key = _key(path)
        self._sources[key] = source
        callbacks = self._loading.pop(key, [])
        self.source_ready.emit(path)
        for callback in callbacks:
            callback()

    def _on_failed(self, path: str, message: str) -> None:
        key = _key(path)
        self._loading.pop(key, None)
        self._failed[key] = message
        self.source_failed.emit(path, message)
        self.status_message.emit(message)

    def file_info(self, path: str) -> ge.AudioFileInfo | None:
        """Length/format from the file header (cached). None if unreadable."""
        key = _key(path)
        if key not in self._file_info:
            try:
                self._file_info[key] = ge.probe_file(path)
            except RuntimeError as exc:
                self.status_message.emit(str(exc))
                return None
        return self._file_info[key]

    def refresh_sources(self) -> None:
        """After a sample-rate change: re-fetch every source used by the project."""
        stale = list(self._sources)
        self._sources.clear()
        for key in stale:
            self.request_source(key)

    # --- Transport -----------------------------------------------------------------

    @property
    def is_playing(self) -> bool:
        return self.engine.is_playing

    def play(self) -> None:
        self.engine.play()
        self._poll_position()

    def stop(self) -> None:
        self.engine.stop()
        if self._recording:
            self.stop_recording()
        self._poll_position()

    # --- Recording -----------------------------------------------------------------

    @property
    def is_recording(self) -> bool:
        return bool(self._recording)

    @property
    def is_counting_in(self) -> bool:
        return self.engine.is_counting_in

    def record_targets(self) -> list[Track]:
        """The tracks that record: armed tracks with an input (audio, or MIDI for MIDI tracks)."""
        return [t for t in self.project.tracks if t.armed and t.has_input]

    def start_recording(self, count_in_beats: float = 0.0) -> str | None:
        """Records the armed tracks (playing, after the count-in, if stopped).
        Returns why it couldn't, or None."""
        if self._recording:
            return None
        tracks = self.record_targets()
        if not tracks:
            return "Arm a MIDI track, or an audio track that has an input, to record."
        if not self.engine.device_status.open:
            return "No audio device is open. Choose one in Options > Preferences."
        audio = [track for track in tracks if not track.is_midi]
        for track in audio:
            self._push_input(track.id)  # (a source the engine couldn't take before)
            if track.input:
                self._open_inputs(track.input)
        folder = recordings_folder(self.project)
        if audio:
            try:
                folder.mkdir(parents=True, exist_ok=True)
            except OSError as exc:
                return f"Could not create the recordings folder {folder}: {exc}"
        now = datetime.now().astimezone()  # local time, in the file names
        targets, paths = [], set()
        for track in tracks:
            if track.is_midi:
                targets.append((self._track_ids[track.id], ""))  # its notes, no file
                continue
            path = take_path(folder, track.name, now)
            while path in paths:  # two tracks of the same name
                path = path.with_name(path.stem + "_.wav")
            paths.add(path)
            targets.append((self._track_ids[track.id], str(path)))
        try:
            self.engine.start_recording(targets, count_in_beats)
        except (RuntimeError, ValueError) as exc:
            return str(exc)
        self._recording = {engine_id: track.id for (engine_id, _), track in zip(targets, tracks, strict=True)}
        self.live_takes = {track.id: LiveTake(track.id, midi=track.is_midi) for track in tracks}
        self.recording_changed.emit(True)
        self._poll_position()
        return None

    def stop_recording(self) -> list[RecordedTake]:
        """Ends the recording (playing goes on); its takes go out as `takes_recorded`."""
        if not self._recording:
            return []
        recording, self._recording = self._recording, {}
        self.live_takes = {}
        takes = []
        for take in self.engine.stop_recording():
            if take.error:
                self.status_message.emit(take.error)
            if take.dropped_frames:
                self.status_message.emit(f"The disk fell behind while recording: {take.dropped_frames} samples "
                                         "were lost (silence in the take).")
            track_id = recording.get(take.track_id)
            if track_id is None or take.frames <= 0:
                continue
            rate = take.sample_rate
            notes = tuple((int(start) / rate, int(end) / rate, int(key), int(velocity))
                          for start, end, key, velocity, _channel in take.notes)
            takes.append(RecordedTake(track_id, take.path, take.start_sample / rate, take.frames / rate,
                                      notes=notes, midi=take.midi))
        self.recording_changed.emit(False)
        self.recording_updated.emit()
        if takes:
            self.takes_recorded.emit(takes)
        return takes

    def _poll_recording(self) -> None:
        if not self._recording:
            return
        for progress in self.engine.recording_progress():
            track_id = self._recording.get(progress.track_id)
            live = self.live_takes.get(track_id) if track_id else None
            if live is None:
                continue
            live.started = progress.started
            live.start_sample = progress.start_sample
            live.frames = progress.frames
            if progress.midi:
                live.notes = progress.notes
                continue
            peaks = progress.peaks
            if len(peaks):
                live.add_peaks(peaks)
        self.recording_updated.emit()
        if not self.engine.is_recording:  # a locate, or a device change, ended it
            if not self.engine.device_status.open or not self.engine.is_playing:
                self.status_message.emit("Recording stopped.")
            self.stop_recording()

    def locate(self, beat: float) -> None:
        self.engine.position_beats = max(0.0, beat)
        self._poll_position()

    @property
    def position(self) -> float:
        return self.engine.position_beats

    def set_metronome(self, enabled: bool) -> None:
        self.engine.metronome = enabled

    # --- Preview -----------------------------------------------------------------

    def preview_file(self, path: str) -> None:
        self._preview_request += 1
        request = self._preview_request

        def start() -> None:
            if request != self._preview_request:
                return  # stopped, or another file previewed, while this one loaded
            try:
                self.engine.preview(path)
            except ValueError:
                pass  # the device changed rate while loading; ignore this click
        self.request_source(path, then=start)

    def stop_preview(self) -> None:
        self._preview_request += 1
        self.engine.stop_preview()

    def preview_note(self, track_id: str, pitch: int, velocity: int) -> None:
        """Play a note on a MIDI track's instrument now; velocity 0 releases it."""
        engine_id = self._track_ids.get(track_id)
        if engine_id is not None:
            self.engine.preview_note(engine_id, pitch, velocity)

    # --- Device ------------------------------------------------------------------

    def open_device(self, settings: AudioSettings) -> str | None:
        """Opens the device the settings describe, closing the one open. Returns
        an error message, or None on success."""
        return self._change_device(lambda: self.engine.open_device(
            settings.device_name, settings.sample_rate, settings.buffer_frames, settings.exclusive,
            driver=settings.driver, input_channels=list(settings.input_channels),
            output_channels=list(settings.output_channels), window=self.owner_window()))

    def reset_device(self) -> str | None:
        """Opens the device again, as its driver asked: its settings changed (in
        its control panel, or its clock)."""
        error = self._change_device(self.engine.reopen_device)
        if error:
            self.status_message.emit(f"The audio device could not restart: {error}. "
                                     "Choose a device in Options > Preferences.")
        else:
            self.status_message.emit("The audio driver restarted with its new settings.")
        return error

    def show_device_control_panel(self) -> bool:
        """The ASIO driver's own settings. False if it has none."""
        self._busy += 1  # its dialog may run a message loop that calls us back
        try:
            return self.engine.show_device_control_panel()
        finally:
            self._busy -= 1

    def _change_device(self, change: Callable[[], None]) -> str | None:
        old_rate = self.engine.sample_rate
        self._busy += 1  # a driver may show a dialog while it opens
        try:
            change()
            error = None
        except RuntimeError as exc:
            error = str(exc)
        finally:
            self._busy -= 1
        if self.engine.sample_rate != old_rate:
            self.refresh_sources()
        self.device_changed.emit()
        return error

    def close_device(self) -> None:
        self.engine.close_device()
        self.device_changed.emit()
        if self._recording:
            self.stop_recording()

    # --- Polling -----------------------------------------------------------------

    def _poll_position(self) -> None:
        position = self.engine.position_beats
        if not math.isclose(position, self._last_position, abs_tol=1e-9):
            self._last_position = position
            self.position_changed.emit(position)
        playing = self.engine.is_playing
        if playing != self._last_playing:
            self._last_playing = playing
            self.transport_changed.emit(playing)

    def poll_plugins(self) -> None:
        """The engine's housekeeping, and what plug-ins reported since."""
        self.engine.idle()
        if not self._busy:  # not from a message loop inside a plug-in call
            self._dispatch_processor_events()

    def _poll_meters(self) -> None:
        self._poll_recording()
        by_engine_id = {engine_id: track_id for track_id, engine_id in self._track_ids.items()}
        by_chain = {self._chains[c]: c for c in self._rack_of_chain if c in self._chains}
        for reading in self.engine.take_meters():
            if reading.chain_id:
                chain = by_chain.get(reading.chain_id)
                if chain is not None:
                    self.chain_meters[chain] = (reading.left, reading.right)
                continue
            key = by_engine_id.get(reading.track_id)
            if key is not None:
                self.meters[key] = (reading.left, reading.right)
        self.meters_updated.emit()
        self.poll_plugins()
        if not self._busy:  # not from a message loop inside a plug-in's or driver's call
            self._poll_device()

    def _poll_device(self) -> None:
        """What happened to the device: one event per poll."""
        event = self.engine.take_device_event()
        if event == "stopped":
            self.status_message.emit("The audio device stopped. Choose a device in Options > Preferences.")
            self.device_changed.emit()
        elif event == "rerouted":
            self.status_message.emit("Audio output was rerouted to another device.")
            self.device_changed.emit()
        elif event == "reset":
            self.reset_device()
        elif event == "latency":
            self.device_changed.emit()
