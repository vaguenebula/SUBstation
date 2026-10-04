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

Freezing: render_freeze renders a track's signal before its fader (a group's
bus, a return's) from the timeline's start into a WAV file in the freeze
folder (the project's "Freeze" folder once it is saved), with its tail. A
frozen track is frozen in the engine too: it plays its frozen audio as its only
clip, and its devices' processors go (a plug-in's state is kept, as for a
device deleted, and comes back when it is unfrozen). The tracks in a frozen
group keep theirs, but the engine doesn't render them.
"""

from __future__ import annotations

from collections.abc import Callable

from PySide6.QtCore import QObject, QThreadPool, QTimer, Signal

from ... import _engine as ge
from ...model.automation import MASTER
from ...model.params import ParamSpec
from ...model.project import Project
from .audio_device import AudioDevice
from .devices import DeviceSync, _StateSignals
from .freezing import FREEZE_TAIL_SECONDS, FreezeSync, freeze_folder
from .inputs import COMPUTER_KEYBOARD, InputSync
from .parameters import ParameterSync
from .plugins import MAX_HIDDEN_EDITORS, PluginHost
from .recording import LiveTake, Recorder, recordings_folder, take_path
from .sources import AUDIO_EXTENSIONS, SourceLoader, _LoadSignals, is_audio_file
from .tracks import TrackSync, clip_desc, note_descs
from .transport import Transport

__all__ = [
    "AUDIO_EXTENSIONS",
    "COMPUTER_KEYBOARD",
    "FREEZE_TAIL_SECONDS",
    "MAX_HIDDEN_EDITORS",
    "EngineBridge",
    "LiveTake",
    "clip_desc",
    "freeze_folder",
    "is_audio_file",
    "note_descs",
    "recordings_folder",
    "take_path",
]


class EngineBridge(TrackSync, InputSync, DeviceSync, PluginHost, ParameterSync, SourceLoader, Transport, Recorder,
                   FreezeSync, AudioDevice, QObject):
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
        # (owner, key) -> a plug-in parameter's own value, kept while its envelope plays (the
        # engine's value follows the envelope then)
        self._plugin_own: dict[tuple[str, str], float] = {}
        self._mixer: dict[str, tuple[float, float]] = {}  # owner -> (volume dB, pan) the engine has
        self._inputs: dict[str, tuple] = {}  # track id -> (input, source, monitor, armed, MIDI input) the engine has
        self._outputs: dict[str, int] = {}  # track id -> the engine track its output goes into
        self._sends: dict[str, dict[int, tuple[float, bool]]] = {}  # track id -> {engine return: (gain, pre-fader)}
        self._frozen: set[str] = set()  # tracks the engine has frozen
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
        project.freeze_changed.connect(self._on_freeze_changed)
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
