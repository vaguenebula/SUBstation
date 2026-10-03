"""The transport (play, stop, locate, the metronome), previews (a file, a
note), and polling the playhead and the meters."""

from __future__ import annotations

import math


class Transport:
    """Playing, previews, and the playhead and meters. Part of EngineBridge (engine_bridge/__init__.py)."""

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

    def locate(self, beat: float) -> None:
        self.engine.position_beats = max(0.0, beat)
        self._poll_position()

    @property
    def position(self) -> float:
        return self.engine.position_beats

    def set_metronome(self, enabled: bool) -> None:
        self.engine.metronome = enabled

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
