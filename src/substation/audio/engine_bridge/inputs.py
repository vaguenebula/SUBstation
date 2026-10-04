"""Tracks' inputs: what each track hears and records (audio channels, another
track's output, a MIDI input), its monitoring, the audio inputs the device
opens with, and the MIDI inputs (the computer keyboard's too)."""

from __future__ import annotations

from ... import _engine as ge
from ...model.automation import MASTER
from ...model.project import Track
from ..settings import AudioSettings, disabled_midi_inputs, set_midi_input_disabled

COMPUTER_KEYBOARD = "Computer Keyboard"  # the MIDI input the computer keyboard plays into


_MONITOR_MODES = {"off": ge.MonitorMode.OFF, "in": ge.MonitorMode.IN, "auto": ge.MonitorMode.AUTO}


class InputSync:
    """Tracks' audio and MIDI inputs, and the MIDI inputs open. Part of EngineBridge (engine_bridge/__init__.py)."""

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
