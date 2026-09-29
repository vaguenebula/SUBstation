"""Keeps the C++ engine in sync with the project model and feeds the UI with
engine state (playhead, meters, CPU) through Qt signals.

Everything here runs on the Qt main thread except source decoding, which runs
in a small thread pool; the engine releases the GIL while decoding.
"""

from __future__ import annotations

import math
import os
from collections.abc import Callable

from PySide6.QtCore import QObject, QRunnable, QThreadPool, QTimer, Signal

from .. import _engine as ge
from ..model.project import WARP_MODES, Clip, Project, Track
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
        warp_mode=_WARP_MODES.get(clip.warp_mode, ge.WarpMode.BEATS),
        transpose=clip.transpose + clip.detune / 100.0,
        id=clip.id,
    )


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

    def __init__(self, engine: ge.Engine, project: Project, parent: QObject | None = None):
        super().__init__(parent)
        self.engine = engine
        self.project = project
        self._track_ids: dict[str, int] = {}  # model track id -> engine track id
        self._devices: dict[str, list[tuple[str, int]]] = {}  # track id -> [(model device id, engine id)]
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
        self.meters.clear()
        for track in self.project.tracks:
            self._add_engine_track(track)
        self._push_settings()
        # Forget decoded audio the new project doesn't use.
        used = {_key(c.path) for t in self.project.tracks for c in t.clips}
        self._sources = {k: s for k, s in self._sources.items() if k in used}
        self.engine.release_unused_sources()

    def _add_engine_track(self, track: Track) -> None:
        self._track_ids[track.id] = self.engine.add_track()
        self._push_mixer(track.id)
        self._push_clips(track.id)
        self._sync_devices(track.id)

    def _on_track_removed(self, track_id: str, _index: int) -> None:
        engine_id = self._track_ids.pop(track_id, None)
        if engine_id is not None:
            self.engine.remove_track(engine_id)  # also removes its devices
        self._devices.pop(track_id, None)
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
        clips = self.project.track(track_id).clips
        for clip in clips:
            self.request_source(clip.path)
        self.engine.set_track_clips(engine_id, [clip_desc(c) for c in clips])

    def _sync_devices(self, track_id: str) -> None:
        engine_track = self._track_ids.get(track_id)
        if engine_track is None:
            return
        devices = self.project.track(track_id).devices
        current = self._devices.get(track_id, [])
        if [d.id for d in devices] != [model_id for model_id, _ in current]:
            # Structure changed: rebuild the chain. (Plugin devices will need
            # state-preserving reordering here.)
            for _, engine_id in current:
                self.engine.remove_processor(engine_id)
            current = [(d.id, self.engine.add_builtin_processor(engine_track, d.kind)) for d in devices]
            self._devices[track_id] = current
            for device in devices:
                for param_id in device.params:
                    self._push_device_param(track_id, device.id, param_id)
        for device, (_, engine_id) in zip(devices, current, strict=True):
            self.engine.set_processor_enabled(engine_id, device.enabled)

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
        for index, info in enumerate(self.engine.processor_params(engine_id)):
            if info.id == param_id:
                self.engine.set_processor_param(engine_id, index, value)
                return

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

    def _poll_meters(self) -> None:
        by_engine_id = {engine_id: track_id for track_id, engine_id in self._track_ids.items()}
        for reading in self.engine.take_meters():
            key = "master" if reading.track_id == 0 else by_engine_id.get(reading.track_id)
            if key is not None:
                self.meters[key] = (reading.left, reading.right)
        self.meters_updated.emit()
        self.engine.idle()
        event = self.engine.take_device_event()
        if event == "stopped":
            self.status_message.emit("The audio device stopped. Choose a device in Options > Preferences.")
            self.device_changed.emit()
        elif event == "rerouted":
            self.status_message.emit("Audio output was rerouted to another device.")
            self.device_changed.emit()
