"""Preferences (audio device) and export dialogs."""

from __future__ import annotations

from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QLabel,
    QVBoxLayout,
    QWidget,
)

from .. import theme
from ..audio.engine_bridge import EngineBridge
from ..audio.settings import BUFFER_SIZES, SAMPLE_RATES, AudioSettings


class PreferencesDialog(QDialog):
    def __init__(self, bridge: EngineBridge, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("Preferences")
        self.setMinimumWidth(460)
        self.bridge = bridge
        self.settings = AudioSettings.load()
        engine = bridge.engine

        self.driver = QLabel(engine.device_status.backend or "WASAPI")
        self.device = QComboBox()
        self.device.addItem("System Default", "")
        for info in engine.list_output_devices():
            self.device.addItem(info.name + ("  (default)" if info.is_default else ""), info.name)
        self._select(self.device, self.settings.device_name)

        self.sample_rate = QComboBox()
        for rate in SAMPLE_RATES:
            self.sample_rate.addItem("Device Default" if rate == 0 else f"{rate} Hz", rate)
        self._select(self.sample_rate, self.settings.sample_rate)

        self.buffer = QComboBox()
        for frames in BUFFER_SIZES:
            self.buffer.addItem(f"{frames} samples", frames)
        self._select(self.buffer, self.settings.buffer_frames)

        self.exclusive = QCheckBox("Exclusive mode (lower latency; other apps are silenced)")
        self.exclusive.setChecked(self.settings.exclusive)

        self.status = QLabel()
        self.status.setWordWrap(True)
        self.status.setStyleSheet(f"color: {theme.TEXT_DIM};")

        form = QFormLayout()
        form.addRow("Driver Type", self.driver)
        form.addRow("Audio Output Device", self.device)
        form.addRow("Sample Rate", self.sample_rate)
        form.addRow("Buffer Size", self.buffer)
        form.addRow("", self.exclusive)
        note = QLabel("ASIO support is planned; WASAPI is used for now.")
        note.setStyleSheet(f"color: {theme.TEXT_DISABLED};")
        form.addRow("", note)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Apply
                                   | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self._ok)
        buttons.rejected.connect(self.reject)
        buttons.button(QDialogButtonBox.StandardButton.Apply).clicked.connect(self.apply)

        layout = QVBoxLayout(self)
        layout.addLayout(form)
        layout.addWidget(self.status)
        layout.addWidget(buttons)
        self._show_status()

    @staticmethod
    def _select(combo: QComboBox, value) -> None:
        index = combo.findData(value)
        combo.setCurrentIndex(max(0, index))

    def _show_status(self, error: str | None = None) -> None:
        status = self.bridge.engine.device_status
        if error:
            self.status.setText(f"<span style='color:#ff6b5e'>{error}</span>")
        elif status.open:
            self.status.setText(f"Running: {status.name}\n{status.sample_rate} Hz · {status.buffer_frames} frames"
                                f" · ~{status.latency_ms:.1f} ms output latency"
                                f"{' · exclusive' if status.exclusive else ''}")
        else:
            self.status.setText("No audio device is open.")

    def apply(self) -> bool:
        self.settings = AudioSettings(
            device_name=self.device.currentData(),
            sample_rate=int(self.sample_rate.currentData()),
            buffer_frames=int(self.buffer.currentData()),
            exclusive=self.exclusive.isChecked(),
        )
        error = self.bridge.open_device(self.settings.device_name, self.settings.sample_rate,
                                        self.settings.buffer_frames, self.settings.exclusive)
        if error is None:
            self.settings.save()
        self._show_status(error)
        return error is None

    def _ok(self) -> None:
        if self.apply():
            self.accept()


class ExportDialog(QDialog):
    def __init__(self, has_loop: bool, parent: QWidget | None = None):
        super().__init__(parent)
        self.setWindowTitle("Export Audio")
        self.range = QComboBox()
        self.range.addItem("Arrangement (start to end of last clip)", "arrangement")
        if has_loop:
            self.range.addItem("Loop region", "loop")
        self.bit_depth = QComboBox()
        for label, depth in (("16-bit", 16), ("24-bit", 24), ("32-bit float", 32)):
            self.bit_depth.addItem(label, depth)
        self.bit_depth.setCurrentIndex(1)
        form = QFormLayout()
        form.addRow("Rendered Range", self.range)
        form.addRow("Bit Depth", self.bit_depth)
        form.addRow("File Type", QLabel("WAV"))
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok | QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout = QVBoxLayout(self)
        layout.addLayout(form)
        layout.addWidget(buttons)
