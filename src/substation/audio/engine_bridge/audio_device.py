"""The audio device: opening it (WASAPI or ASIO), resetting it when its driver
asks, its control panel, and what happens to it (stopped, rerouted)."""

from __future__ import annotations

from collections.abc import Callable

from ..settings import AudioSettings


class AudioDevice:
    """Opening the audio device and what happens to it. Part of EngineBridge (engine_bridge/__init__.py)."""

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
