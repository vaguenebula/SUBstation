"""Persistent audio preferences (QSettings)."""

from __future__ import annotations

from dataclasses import dataclass

from PySide6.QtCore import QSettings

BUFFER_SIZES = (64, 128, 256, 512, 1024, 2048)
SAMPLE_RATES = (0, 44100, 48000, 88200, 96000)  # 0 = device default


@dataclass
class AudioSettings:
    device_name: str = ""  # empty = system default
    sample_rate: int = 0
    buffer_frames: int = 256
    exclusive: bool = False

    @classmethod
    def load(cls) -> AudioSettings:
        s = QSettings()
        return cls(
            device_name=str(s.value("audio/device", "")),
            sample_rate=int(s.value("audio/sample_rate", 0)),
            buffer_frames=int(s.value("audio/buffer_frames", 256)),
            exclusive=str(s.value("audio/exclusive", "false")).lower() == "true",
        )

    def save(self) -> None:
        s = QSettings()
        s.setValue("audio/device", self.device_name)
        s.setValue("audio/sample_rate", self.sample_rate)
        s.setValue("audio/buffer_frames", self.buffer_frames)
        s.setValue("audio/exclusive", "true" if self.exclusive else "false")
