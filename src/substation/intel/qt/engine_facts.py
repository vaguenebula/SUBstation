"""EngineFacts on the engine bridge: the one place the layer's reads of the
engine meet the bridge's (and so the engine's) names. When those change, only
this changes.

It also keeps a meter history: the bridge polls the meters about 30 times a
second (_poll_meters, then meters_updated); each poll's peaks go into a ring
per strip, so suggestions and agents can ask how loud a strip was over the
last seconds."""

from __future__ import annotations

import time
from collections import deque

from ...audio.engine_bridge import EngineBridge
from ...model.project import Freeze, find_device
from ...model.timebase import beats_to_seconds
from ..facts import MeterStats, SampleView, TransportState
from ..ops.errors import BUSY, OpError, not_found

HISTORY_SECONDS = 60.0
CLIP_LEVEL = 1.0  # 0 dBFS


class MeterHistory:
    """Each strip's meter peaks with when they were read, for the last `seconds`."""

    def __init__(self, seconds: float = HISTORY_SECONDS, rate: float = 30.0):
        self.seconds = seconds
        self._size = int(seconds * rate * 1.5)
        self._rings: dict[str, deque[tuple[float, float]]] = {}

    def record(self, meters: dict[str, tuple[float, float]], now: float) -> None:
        for strip, (left, right) in meters.items():
            ring = self._rings.get(strip)
            if ring is None:
                ring = self._rings[strip] = deque(maxlen=self._size)
            ring.append((now, max(left, right)))

    def stats(self, strip: str, seconds: float, now: float) -> MeterStats:
        since = now - min(seconds, self.seconds)
        peaks = [peak for t, peak in self._rings.get(strip, ()) if t >= since]
        return MeterStats(seconds=seconds, readings=len(peaks), peak=max(peaks, default=0.0),
                          clipped=sum(peak >= CLIP_LEVEL for peak in peaks))

    def forget(self, strip: str) -> None:
        self._rings.pop(strip, None)


class BridgeFacts:
    """EngineFacts (intel/facts.py) implemented on an EngineBridge."""

    def __init__(self, bridge: EngineBridge, clock=time.monotonic):
        self.bridge = bridge
        self.clock = clock
        self.meters = MeterHistory()
        bridge.meters_updated.connect(self._on_meters)
        bridge.project.track_removed.connect(lambda track_id, _index: self.meters.forget(track_id))
        bridge.project.return_removed.connect(lambda track_id, _index: self.meters.forget(track_id))

    def _on_meters(self) -> None:
        self.meters.record(self.bridge.meters, self.clock())

    # --- Parameters ----------------------------------------------------------------------------

    def param_specs(self, track_id: str, device_id: str):
        project = self.bridge.project
        if not project.has_owner(track_id):
            return None
        device = find_device(project.track(track_id).devices, device_id)
        if device is None or (not device.is_rack and self.bridge.engine_device_id(track_id, device_id) is None):
            return None
        return self.bridge.device_param_specs(track_id, device)

    def param_text(self, track_id: str, device_id: str, param_id: str, plain: float) -> str | None:
        specs = self.param_specs(track_id, device_id) or []
        spec = next((s for s in specs if s.key.endswith(f":{param_id}")), None)
        return None if spec is None else spec.format(plain)

    def own_value(self, owner: str, key: str) -> float | None:
        return self.bridge.own_value(owner, key)

    def is_automated(self, owner: str, key: str) -> bool:
        return self.bridge.is_automated(owner, key)

    def is_overridden(self, owner: str, key: str) -> bool:
        return self.bridge.is_overridden(owner, key)

    def override_automation(self, owner: str, key: str) -> None:
        self.bridge.override_automation(owner, key)

    def re_enable_automation(self, owner: str | None = None) -> None:
        self.bridge.re_enable_automation(owner)

    # --- Meters, audio ---------------------------------------------------------------------------

    def meter_history(self, strip_id: str, seconds: float) -> MeterStats:
        return self.meters.stats(strip_id, seconds, self.clock())

    def decoded(self, path: str) -> SampleView | None:
        source = self.bridge.source(path)
        if source is None:
            return None
        return SampleView(source.samples(0, source.frames), float(source.sample_rate))

    def file_duration(self, path: str) -> float | None:
        info = self.bridge.file_info(path)
        return None if info is None else float(info.duration)

    def render_track(self, track_id: str, start_beat: float, end_beat: float):
        """A track's signal before its fader over a beat range (E1). Refused while
        recording; while playing it silences the live output for the render's
        length (until engine request E3)."""
        if self.bridge.is_recording:
            raise OpError(BUSY, "Not while recording")
        engine_id = self.bridge.engine_track_id(track_id)
        if engine_id is None:
            raise not_found(f"There is no track {track_id!r} in the engine")
        self.bridge.wait_for_device_states()
        seconds = beats_to_seconds(max(0.0, end_beat - start_beat), self.bridge.project.tempo)
        frames = round(seconds * self.bridge.engine.sample_rate)
        return self.bridge.engine.render_track_offline(engine_id, start_beat, frames)

    def render_freeze(self, track_id: str) -> Freeze:
        return self.bridge.render_freeze(track_id)

    def discard_freeze(self, freeze: Freeze) -> None:
        self.bridge.discard_freeze(freeze)

    # --- Transport ---------------------------------------------------------------------------------

    def transport(self) -> TransportState:
        return TransportState(self.bridge.is_playing, self.bridge.is_recording, self.bridge.position)

    def play(self) -> None:
        self.bridge.play()

    def stop(self) -> None:
        self.bridge.stop()

    def locate(self, beat: float) -> None:
        self.bridge.locate(beat)

    # --- Plug-ins ------------------------------------------------------------------------------------

    def program_name(self, track_id: str, device_id: str) -> str | None:
        return None  # engine request E5

    def plugin_state(self, track_id: str, device_id: str) -> bytes | None:
        state = self.bridge.plugin_state(track_id, device_id)
        return None if state is None else bytes(state)

    def store_plugin_states(self, device_ids=None) -> None:
        self.bridge.store_plugin_states(device_ids)
