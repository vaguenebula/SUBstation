"""Keeps the C++ engine in sync with the project model and feeds the UI with
engine state (playhead, meters, CPU) through Qt signals.

Everything here runs on the Qt main thread except source decoding, which runs
in a small thread pool; the engine releases the GIL while decoding.

Plug-ins: a device's engine processor lives as long as the device is in its
chain, so a plug-in keeps its state (and open editor) when the chain around it
changes. When a plug-in device goes away (deleted, or its track), its state is
kept here, so undo brings it back as it was. Edits made in a plug-in's own
editor come back from the engine as `plugin_param_edited`, for the undo stack.
"""

from __future__ import annotations

import base64
import math
import os
from collections.abc import Callable
from dataclasses import replace

from PySide6.QtCore import QObject, QRunnable, QThreadPool, QTimer, Signal

from .. import _engine as ge
from ..model.project import WARP_MODES, Clip, Device, PluginRef, Project, Track
from ..model.timebase import db_to_gain

AUDIO_EXTENSIONS = (".wav", ".wave", ".flac", ".mp3")
_WARP_MODES = {name: ge.WarpMode(index) for index, name in enumerate(WARP_MODES)}


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
    plugin_params_changed = Signal(str, str)  # values changed without edits (a preset, meters): show them
    plugin_params_rebuilt = Signal(str, str)  # the parameter list changed
    plugin_editor_changed = Signal(str, str)  # its editor opened or closed
    plugin_state_dirty = Signal()  # a plug-in changed in a way no edit shows: the project has changes
    devices_loaded = Signal(str)  # track id: its devices' processors were (re)created

    def __init__(self, engine: ge.Engine, project: Project, parent: QObject | None = None):
        super().__init__(parent)
        self.engine = engine
        self.project = project
        self._track_ids: dict[str, int] = {}  # model track id -> engine track id
        # track id -> [(model device id, engine id)]; the engine id is None when a plug-in didn't load
        self._devices: dict[str, list[tuple[str, int | None]]] = {}
        self._enabled: dict[int, bool] = {}  # engine id -> what the engine was told
        self._plugin_ids: dict[int, str] = {}  # engine id of each plug-in processor -> the path it came from
        self._plugin_states: dict[str, bytes] = {}  # device id -> its plug-in's state when it went away
        self._param_ids: dict[int, list[str]] = {}  # engine id -> parameter ids by index (cache)
        self._busy = 0  # > 0 while a plug-in call may run a message loop that calls us back
        self.plugin_errors: dict[str, str] = {}  # device id -> why its plug-in isn't loaded
        self.known_plugins: dict[str, str] = {}  # plug-in uid -> file, from the scan: finds moved plug-ins
        self.owner_window: Callable[[], int] = lambda: 0  # HWND owning plug-in editor windows
        self._editors_wanted: set[str] = set()  # device ids whose editor the user left open
        self._editors_track: str | None = None  # the track whose editors are shown (the selected one)
        self._sources: dict[str, ge.AudioSource] = {}
        self._loading: dict[str, list[Callable[[], None]]] = {}
        self._failed: dict[str, str] = {}
        self._file_info: dict[str, ge.AudioFileInfo] = {}
        self.meters: dict[str, tuple[float, float]] = {}  # track id or "master" -> (left, right)
        self._last_position = -1.0
        self._last_playing = False

        self._pool = QThreadPool(self)
        self._pool.setMaxThreadCount(2)
        self._load_signals = _LoadSignals(self)
        self._load_signals.loaded.connect(self._on_loaded)
        self._load_signals.failed.connect(self._on_failed)

        project.reset.connect(self._on_reset)
        project.track_inserted.connect(lambda tid, _i: self._add_engine_track(project.track(tid)))
        project.track_removed.connect(self._on_track_removed)
        project.track_changed.connect(self._push_mixer)
        project.clips_changed.connect(self._push_clips)
        project.devices_changed.connect(self._sync_devices)
        project.device_param_changed.connect(self._push_device_param)
        project.device_state_changed.connect(self._push_device_state)
        project.track_changed.connect(self._update_editor_titles)
        project.settings_changed.connect(self._push_settings)

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
        for engine_id in self._track_ids.values():
            self.engine.remove_track(engine_id)
        self._track_ids.clear()
        self._devices.clear()
        self._enabled.clear()
        self._plugin_ids.clear()
        self._plugin_states.clear()
        self._param_ids.clear()
        self.plugin_errors.clear()
        self.meters.clear()
        self._editors_wanted.clear()
        for track in self.project.tracks:
            self._add_engine_track(track)
        self._push_settings()
        # Forget decoded audio the new project doesn't use.
        used = {_key(c.path) for t in self.project.tracks if not t.is_midi for c in t.clips}
        self._sources = {k: s for k, s in self._sources.items() if k in used}
        self.engine.release_unused_sources()

    def _add_engine_track(self, track: Track) -> None:
        self._track_ids[track.id] = self.engine.add_track()
        self._push_mixer(track.id)
        self._push_clips(track.id)
        self._sync_devices(track.id)

    def _on_track_removed(self, track_id: str, _index: int) -> None:
        for device_id, processor_id in self._devices.pop(track_id, []):
            self._forget_processor(device_id, processor_id, remove=False)
        engine_id = self._track_ids.pop(track_id, None)
        if engine_id is not None:
            self.engine.remove_track(engine_id)  # also removes its devices
        self.meters.pop(track_id, None)

    def _push_mixer(self, track_id: str) -> None:
        engine_id = self._track_ids.get(track_id)
        if engine_id is None:
            return
        track = self.project.track(track_id)
        self.engine.set_track_gain(engine_id, db_to_gain(track.volume_db))
        self.engine.set_track_pan(engine_id, track.pan)
        self.engine.set_track_mute(engine_id, track.mute)
        self.engine.set_track_solo(engine_id, track.solo)

    def _push_clips(self, track_id: str) -> None:
        engine_id = self._track_ids.get(track_id)
        if engine_id is None:
            return
        track = self.project.track(track_id)
        if track.is_midi:
            self.engine.set_track_notes(engine_id, note_descs(track))
            return
        for clip in track.clips:
            self.request_source(clip.path)
        self.engine.set_track_clips(engine_id, [clip_desc(c) for c in track.clips])

    def _sync_devices(self, track_id: str) -> None:
        engine_track = self._track_ids.get(track_id)
        if engine_track is None:
            return
        track = self.project.track(track_id)
        current = dict(self._devices.get(track_id, []))
        if list(current) != [d.id for d in track.devices]:
            # The chain changed. Devices still in it keep their processors (a
            # plug-in keeps its state and editor); new ones get one; the rest go.
            wanted = {d.id for d in track.devices}
            for device_id, processor_id in current.items():
                if device_id not in wanted:
                    self._forget_processor(device_id, processor_id)
            chain = [(d.id, current[d.id] if d.id in current else self._create_processor(engine_track, d))
                     for d in track.devices]
            self._devices[track_id] = chain
            self.engine.set_track_processor_order(engine_track, [pid for _, pid in chain if pid is not None])
            self.devices_loaded.emit(track_id)
        self._push_enabled(track)

    def _push_enabled(self, track: Track) -> None:
        for device, (_, processor_id) in zip(track.devices, self._devices.get(track.id, []), strict=True):
            if processor_id is not None and self._enabled.get(processor_id) != device.enabled:
                self.engine.set_processor_enabled(processor_id, device.enabled)
                self._enabled[processor_id] = device.enabled

    def _create_processor(self, engine_track: int, device: Device) -> int | None:
        if device.is_plugin:
            return self._load_plugin(engine_track, device)
        processor_id = self.engine.add_builtin_processor(engine_track, device.kind)
        for param_id, value in device.params.items():
            self._set_param(processor_id, param_id, value)
        return processor_id

    def plugin_path(self, plugin: PluginRef) -> str | None:
        """Where a plug-in is now: where it was, or where the scan found it."""
        if plugin.path and os.path.exists(plugin.path):
            return plugin.path
        return self.known_plugins.get(plugin.uid)

    def _load_plugin(self, engine_track: int, device: Device) -> int | None:
        plugin = device.plugin
        path = self.plugin_path(plugin)
        if path is None:
            self._plugin_failed(device, f"{plugin.name} is not installed.")
            return None
        self._busy += 1
        try:
            processor_id = self.engine.add_plugin_processor(engine_track, plugin.format, path, plugin.uid)
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
        """A device's processor goes away (with its track if not `remove`). A
        plug-in's state is kept, in case the device comes back (undo)."""
        self.plugin_errors.pop(device_id, None)
        if processor_id is None:
            return
        if processor_id in self._plugin_ids:
            try:
                self._plugin_states[device_id] = self.engine.processor_state(processor_id)
            except RuntimeError:
                pass
            del self._plugin_ids[processor_id]
        self._enabled.pop(processor_id, None)
        self._param_ids.pop(processor_id, None)
        if remove:
            self.engine.remove_processor(processor_id)

    def engine_device_id(self, track_id: str, device_id: str) -> int | None:
        for model_id, engine_id in self._devices.get(track_id, []):
            if model_id == device_id:
                return engine_id
        return None

    def _push_device_param(self, track_id: str, device_id: str, param_id: str) -> None:
        engine_id = self.engine_device_id(track_id, device_id)
        if engine_id is None:
            return
        value = self.project.device(track_id, device_id).params[param_id]
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
        if engine_id is not None and device.state:
            self._set_plugin_state(engine_id, device.plugin.name if device.plugin else device.kind,
                                   base64.b64decode(device.state))

    # --- Plug-ins ------------------------------------------------------------------

    def set_known_plugins(self, plugins) -> None:
        """The scanned plug-ins (PluginInfo): lets projects find plug-ins that moved.
        Devices whose plug-in wasn't found get another try."""
        self.known_plugins = {p.uid: p.path for p in plugins}
        for track_id, chain in self._devices.items():
            track = self.project.track(track_id)
            if not any(pid is None for _, pid in chain):
                continue
            devices = {d.id: d for d in track.devices}
            reloaded = [(did, self._load_plugin(self._track_ids[track_id], devices[did])
                         if pid is None and devices[did].is_plugin and self.plugin_path(devices[did].plugin)
                         else pid) for did, pid in chain]
            if reloaded != chain:
                self._devices[track_id] = reloaded
                self.engine.set_track_processor_order(self._track_ids[track_id],
                                                      [pid for _, pid in reloaded if pid is not None])
                self._push_enabled(track)
                self.devices_loaded.emit(track_id)

    def plugin_state(self, track_id: str, device_id: str) -> bytes | None:
        """A plug-in device's current state (a .vstpreset), None if it isn't loaded."""
        engine_id = self.engine_device_id(track_id, device_id)
        if engine_id is None or engine_id not in self._plugin_ids:
            return None
        return self.engine.processor_state(engine_id)

    def store_plugin_states(self) -> None:
        """Copy every plug-in's state into the model, for saving the project."""
        for track in self.project.tracks:
            for device in track.devices:
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
        until that track is shown again."""
        if track_id == self._editors_track:
            return
        self._editors_track = track_id
        for chain_track, chain in self._devices.items():
            for device_id, processor_id in chain:
                if processor_id not in self._plugin_ids:
                    continue
                if chain_track != track_id:
                    if self.engine.is_editor_open(processor_id):
                        self.engine.set_editor_visible(processor_id, False)
                        self.plugin_editor_changed.emit(chain_track, device_id)
                elif device_id in self._editors_wanted and not self.engine.is_editor_open(processor_id):
                    if self.engine.set_editor_visible(processor_id, True):
                        self.plugin_editor_changed.emit(chain_track, device_id)
                    else:  # it was closed meanwhile (its plug-in reloaded): open it again
                        self.open_plugin_editor(chain_track, device_id, report=False)

    def is_plugin_editor_open(self, track_id: str, device_id: str) -> bool:
        engine_id = self.engine_device_id(track_id, device_id)
        return engine_id is not None and self.engine.is_editor_open(engine_id)

    def close_all_editors(self) -> None:
        for chain in self._devices.values():
            for _, processor_id in chain:
                if processor_id in self._plugin_ids:
                    self.engine.close_editor(processor_id)

    def shutdown(self) -> None:
        """Unload every plug-in now, while the application is still whole (not
        whenever the engine happens to be garbage-collected)."""
        self.close_all_editors()
        for engine_id in self._track_ids.values():
            self.engine.remove_track(engine_id)
        self._track_ids.clear()
        self._devices.clear()
        self._plugin_ids.clear()
        self.engine.idle()

    def _update_editor_titles(self, track_id: str) -> None:
        for device_id, processor_id in self._devices.get(track_id, []):
            if processor_id in self._plugin_ids and self.engine.is_editor_open(processor_id):
                self.engine.set_editor_title(processor_id, self._editor_title(track_id, device_id))

    def _dispatch_processor_events(self) -> None:
        events = self.engine.take_processor_events()
        if not events:
            return
        places = {pid: (tid, did) for tid, chain in self._devices.items() for did, pid in chain if pid is not None}
        changed: dict[tuple[str, str], None] = {}
        dirty = False
        kind = ge.ProcessorEventType
        for event in events:
            place = places.get(event.processor_id)
            if place is None:
                continue
            if event.type == kind.PARAM_EDITED:
                param_id = self.param_id(event.processor_id, event.param_index)
                if param_id is not None:
                    self.plugin_param_edited.emit(*place, param_id, event.value, event.old_value, event.gesture)
            elif event.type in (kind.PARAMS_CHANGED, kind.LATENCY_CHANGED):
                changed[place] = None
            elif event.type == kind.PARAM_INFO_CHANGED:
                self._param_ids.pop(event.processor_id, None)
                self.plugin_params_rebuilt.emit(*place)
            elif event.type == kind.EDITOR_CLOSED:
                self._editors_wanted.discard(place[1])
                self.plugin_editor_changed.emit(*place)
            elif event.type == kind.EDITOR_REQUESTED:
                self.open_plugin_editor(*place)
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
        self.engine.set_master_gain(db_to_gain(p.master_volume_db))

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
        self._poll_position()

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
        def start() -> None:
            try:
                self.engine.preview(path)
            except ValueError:
                pass  # the device changed rate while loading; ignore this click
        self.request_source(path, then=start)

    def stop_preview(self) -> None:
        self.engine.stop_preview()

    def preview_note(self, track_id: str, pitch: int, velocity: int) -> None:
        """Play a note on a MIDI track's instrument now; velocity 0 releases it."""
        engine_id = self._track_ids.get(track_id)
        if engine_id is not None:
            self.engine.preview_note(engine_id, pitch, velocity)

    # --- Device ------------------------------------------------------------------

    def open_device(self, name: str, sample_rate: int, buffer_frames: int, exclusive: bool) -> str | None:
        """Returns an error message, or None on success."""
        old_rate = self.engine.sample_rate
        try:
            self.engine.open_device(name, sample_rate, buffer_frames, exclusive)
            error = None
        except RuntimeError as exc:
            error = str(exc)
        if self.engine.sample_rate != old_rate:
            self.refresh_sources()
        self.device_changed.emit()
        return error

    def close_device(self) -> None:
        self.engine.close_device()
        self.device_changed.emit()

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
        by_engine_id = {engine_id: track_id for track_id, engine_id in self._track_ids.items()}
        for reading in self.engine.take_meters():
            key = "master" if reading.track_id == 0 else by_engine_id.get(reading.track_id)
            if key is not None:
                self.meters[key] = (reading.left, reading.right)
        self.meters_updated.emit()
        self.poll_plugins()
        event = self.engine.take_device_event()
        if event == "stopped":
            self.status_message.emit("The audio device stopped. Choose a device in Options > Preferences.")
            self.device_changed.emit()
        elif event == "rerouted":
            self.status_message.emit("Audio output was rerouted to another device.")
            self.device_changed.emit()
