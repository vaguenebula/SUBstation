"""Persistent audio preferences (QSettings)."""

from __future__ import annotations

from dataclasses import dataclass

from PySide6.QtCore import QSettings

DRIVERS = ("WASAPI", "ASIO")
BUFFER_SIZES = (64, 128, 256, 512, 1024, 2048)
SAMPLE_RATES = (0, 44100, 48000, 88200, 96000)  # 0 = device default


def _channels(value) -> tuple[int, ...]:
    """Channel numbers saved as "2,3"."""
    try:
        return tuple(int(part) for part in str(value or "").split(",") if part.strip())
    except ValueError:
        return ()


@dataclass(frozen=True)
class AudioSettings:
    driver: str = "WASAPI"
    device_name: str = ""  # empty = the system default (WASAPI), the first driver (ASIO)
    sample_rate: int = 0  # 0 = the rate the device runs at
    buffer_frames: int = 256  # 0 = the device's preferred size
    exclusive: bool = False  # WASAPI
    # Device channels (0-based). The master plays on the first two outputs; () = the device's first two.
    output_channels: tuple[int, ...] = ()
    # Inputs to open (ASIO); none until something records.
    input_channels: tuple[int, ...] = ()

    @classmethod
    def load(cls) -> AudioSettings:
        s = QSettings()
        driver = str(s.value("audio/driver", "WASAPI"))
        return cls(
            driver=driver if driver in DRIVERS else "WASAPI",
            device_name=str(s.value("audio/device", "")),
            sample_rate=int(s.value("audio/sample_rate", 0)),
            buffer_frames=int(s.value("audio/buffer_frames", 256)),
            exclusive=str(s.value("audio/exclusive", "false")).lower() == "true",
            output_channels=_channels(s.value("audio/output_channels", "")),
            input_channels=_channels(s.value("audio/input_channels", "")),
        )

    def save(self) -> None:
        s = QSettings()
        s.setValue("audio/driver", self.driver)
        s.setValue("audio/device", self.device_name)
        s.setValue("audio/sample_rate", self.sample_rate)
        s.setValue("audio/buffer_frames", self.buffer_frames)
        s.setValue("audio/exclusive", "true" if self.exclusive else "false")
        s.setValue("audio/output_channels", ",".join(map(str, self.output_channels)))
        s.setValue("audio/input_channels", ",".join(map(str, self.input_channels)))
