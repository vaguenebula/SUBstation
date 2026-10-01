"""Preferences (audio device, MIDI inputs, plug-in folders) and export dialogs."""

from __future__ import annotations

import html
import os
from collections.abc import Iterator
from contextlib import contextmanager
from dataclasses import replace
from pathlib import Path
from typing import TYPE_CHECKING

from PySide6.QtCore import Qt
from PySide6.QtGui import QColor
from PySide6.QtWidgets import (
    QCheckBox,
    QComboBox,
    QDialog,
    QDialogButtonBox,
    QFileDialog,
    QFormLayout,
    QHBoxLayout,
    QLabel,
    QListWidget,
    QListWidgetItem,
    QPushButton,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)

from .. import _engine as ge
from .. import theme
from ..audio.engine_bridge import EngineBridge
from ..audio.settings import (
    BUFFER_SIZES,
    DRIVERS,
    SAMPLE_RATES,
    AudioSettings,
    disabled_midi_inputs,
)
from ..plugins.scanner import standard_paths
from ..plugins.settings import custom_folders, set_custom_folders

if TYPE_CHECKING:
    from .browser.file_index import PluginIndex

NO_ASIO = ("This build has no ASIO support: unzip Steinberg's ASIO SDK into the project folder "
           "and build again (see the README).")


@contextmanager
def _quiet(*widgets: QWidget) -> Iterator[None]:
    """Fills widgets in without their signals applying anything."""
    previous = [widget.blockSignals(True) for widget in widgets]
    try:
        yield
    finally:
        for widget, blocked in zip(widgets, previous, strict=True):
            widget.blockSignals(blocked)


def _key(value):
    return tuple(value) if isinstance(value, (list, tuple)) else value


def output_choices(names: list[str]) -> list[tuple[str, tuple[int, ...]]]:
    """Where the master can play on a device with these outputs: each stereo
    pair, and the last output alone if their number is odd."""
    choices = []
    for first in range(0, len(names) - 1, 2):
        choices.append((f"{first + 1}/{first + 2} · {names[first]}, {names[first + 1]}", (first, first + 1)))
    if len(names) % 2:
        last = len(names) - 1
        choices.append((f"{last + 1} (mono) · {names[last]}", (last,)))
    return choices


class PreferencesDialog(QDialog):
    """Audio and plug-in preferences. Changes apply at once: what a device offers
    (its sample rates, buffer sizes and channels, its driver's own settings) is
    only known while it is open."""

    def __init__(self, bridge: EngineBridge, parent: QWidget | None = None,
                 plugins: PluginIndex | None = None):
        super().__init__(parent)
        self.setWindowTitle("Preferences")
        self.setMinimumWidth(500)
        self.bridge = bridge
        self.settings = self._current_settings()

        self.driver = QComboBox()
        available = ge.driver_types()
        for index, driver in enumerate(DRIVERS):
            self.driver.addItem(driver, driver)
            if driver not in available:
                item = self.driver.model().item(index)
                item.setEnabled(False)
                item.setToolTip(NO_ASIO)
        self.device = QComboBox()
        self.control_panel = QPushButton("Hardware Setup")
        self.control_panel.setToolTip("The driver's own settings (buffer size, clock, routing...)")
        self.outputs = QComboBox()
        self.outputs.setToolTip("The outputs the master plays on")
        self.sample_rate = QComboBox()
        self.buffer = QComboBox()
        self.exclusive = QCheckBox("Exclusive mode (lower latency; other apps are silenced)")

        self.status = QLabel()
        self.status.setWordWrap(True)
        self.status.setStyleSheet(f"color: {theme.TEXT_DIM};")

        device_row = QHBoxLayout()
        device_row.setContentsMargins(0, 0, 0, 0)
        device_row.addWidget(self.device, 1)
        device_row.addWidget(self.control_panel)
        self.form = QFormLayout()
        self.form.addRow("Driver Type", self.driver)
        self.form.addRow("Audio Device", device_row)
        self.form.addRow("Output Channels", self.outputs)
        self.form.addRow("Sample Rate", self.sample_rate)
        self.form.addRow("Buffer Size", self.buffer)
        self.form.addRow("", self.exclusive)

        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Close)
        buttons.rejected.connect(self.reject)

        audio = QWidget()
        audio_layout = QVBoxLayout(audio)
        audio_layout.addLayout(self.form)
        audio_layout.addWidget(self.status)
        audio_layout.addStretch(1)
        self.tabs = QTabWidget()
        self.tabs.addTab(audio, "Audio")
        self.midi = MidiPage(bridge)
        self.tabs.addTab(self.midi, "MIDI")
        self.plugins = PluginsPage(plugins) if plugins is not None else None
        if self.plugins is not None:
            self.tabs.addTab(self.plugins, "Plug-ins")

        layout = QVBoxLayout(self)
        layout.addWidget(self.tabs)
        layout.addWidget(buttons)

        self.driver.currentIndexChanged.connect(self._driver_chosen)
        self.device.currentIndexChanged.connect(self._device_chosen)
        for combo in (self.outputs, self.sample_rate, self.buffer):
            combo.currentIndexChanged.connect(self._setting_chosen)
        self.exclusive.toggled.connect(self._setting_chosen)
        self.control_panel.clicked.connect(self._show_control_panel)
        bridge.device_changed.connect(self._refresh)  # a driver that restarted, or a device that went away
        self._refresh()

    def _current_settings(self) -> AudioSettings:
        """The saved settings, unless another kind of device runs (the saved one didn't open)."""
        settings = AudioSettings.load()
        status = self.bridge.engine.device_status
        if status.open and status.backend != settings.driver:
            return AudioSettings(driver=status.backend, device_name=status.name if status.backend == "ASIO" else "")
        if settings.driver not in ge.driver_types():
            return AudioSettings()
        return settings

    def _devices(self, driver: str) -> list:
        try:
            return self.bridge.engine.list_devices(driver)
        except RuntimeError:
            return []

    @staticmethod
    def _select(combo: QComboBox, value, default_label: str | None = None) -> None:
        """Selects the item with this value; one is added for it if there is none and it has a label."""
        index = next((i for i in range(combo.count()) if _key(combo.itemData(i)) == _key(value)), -1)
        if index < 0 and default_label is not None:
            combo.insertItem(0, default_label, value)
            index = 0
        combo.setCurrentIndex(max(0, index))

    # --- Showing ---------------------------------------------------------------------

    def _refresh(self, error: str | None = None) -> None:
        """Shows the settings, with the choices the open device offers."""
        engine = self.bridge.engine
        s = self.settings
        asio = s.driver == "ASIO"
        status = engine.device_status
        running = status.open and status.backend == s.driver
        caps = engine.device_capabilities if running else None
        with _quiet(self.driver, self.device, self.outputs, self.sample_rate, self.buffer, self.exclusive):
            self._select(self.driver, s.driver)

            self.device.clear()
            if asio:
                for info in self._devices("ASIO"):
                    self.device.addItem(info.name, info.name)
                if self.device.count() == 0:
                    self.device.addItem("No ASIO driver is installed", None)
                self._select(self.device, status.name if running else s.device_name)
            else:
                self.device.addItem("System Default", "")
                for info in self._devices("WASAPI"):
                    self.device.addItem(info.name + ("  (default)" if info.is_default else ""), info.name)
                self._select(self.device, s.device_name)

            self.outputs.clear()
            for label, channels in output_choices(list(caps.output_names) if caps and asio else []):
                self.outputs.addItem(label, channels)
            self._select(self.outputs, tuple(status.output_channels) if running else s.output_channels)

            self.sample_rate.clear()
            if caps and caps.sample_rates:
                for rate in caps.sample_rates:
                    self.sample_rate.addItem(f"{rate} Hz", rate)
                self._select(self.sample_rate, status.sample_rate)
            else:
                for rate in SAMPLE_RATES:
                    self.sample_rate.addItem("Device Default" if rate == 0 else f"{rate} Hz", rate)
                self._select(self.sample_rate, s.sample_rate)

            self.buffer.clear()
            if caps and caps.buffer_sizes:
                for frames in caps.buffer_sizes:
                    self.buffer.addItem(f"{frames} samples", frames)
                self._select(self.buffer, status.buffer_frames)
            else:
                for frames in BUFFER_SIZES:
                    self.buffer.addItem(f"{frames} samples", frames)
                self._select(self.buffer, s.buffer_frames, "Device Default")

            self.exclusive.setChecked(s.exclusive)

        self.form.setRowVisible(self.outputs, asio)
        self.form.setRowVisible(self.exclusive, not asio)
        self.device.setEnabled(self.device.currentData() is not None)
        self.control_panel.setVisible(asio)
        self.control_panel.setEnabled(bool(running and asio and caps.has_control_panel))
        fixed = bool(running and asio and self.buffer.count() == 1)
        self.buffer.setEnabled(not fixed)
        self.buffer.setToolTip("The driver sets its buffer size: change it in Hardware Setup." if fixed else "")
        self._show_status(error)

    def _show_status(self, error: str | None = None) -> None:
        status = self.bridge.engine.device_status
        lines = []
        if error:
            lines.append(f"<span style='color:#ff6b5e'>{html.escape(error)}</span>")
        if status.open:
            if status.backend == "ASIO":
                latency = f"input latency {status.input_latency_ms:.1f} ms · output latency {status.latency_ms:.1f} ms"
            else:
                latency = f"~{status.latency_ms:.1f} ms output latency"
            lines.append(f"Running: {html.escape(status.name)} ({status.backend})<br>"
                         f"{status.sample_rate} Hz · {status.buffer_frames} samples · {latency}"
                         f"{' · exclusive' if status.exclusive else ''}")
        else:
            lines.append("No audio device is open.")
        self.status.setText("<br>".join(lines))

    # --- Changing ----------------------------------------------------------------------

    def _driver_chosen(self) -> None:
        driver = self.driver.currentData()
        names = [info.name for info in self._devices(driver)]
        if driver == "ASIO" and not names:
            self.settings = AudioSettings(driver=driver)
            self._refresh("No ASIO driver is installed.")
            return
        name = self.settings.device_name if self.settings.device_name in names else (names[0] if driver == "ASIO" else "")
        # A new driver type starts from its defaults, but keeps the sample rate if it can.
        defaults = AudioSettings(driver=driver, device_name=name, buffer_frames=0) if driver == "ASIO" else \
            AudioSettings(driver=driver, device_name=name)
        self._open(replace(defaults, sample_rate=self.settings.sample_rate), fall_back=True)

    def _device_chosen(self) -> None:
        name = self.device.currentData()
        if name is None:
            return
        s = self.settings
        # Another device: its own channels and buffer size (ASIO), but the same sample rate if it can.
        self._open(replace(s, device_name=name, buffer_frames=0 if s.driver == "ASIO" else s.buffer_frames,
                           output_channels=(), input_channels=()), fall_back=True)

    def _setting_chosen(self) -> None:
        s = self.settings
        asio = s.driver == "ASIO"
        outputs = self.outputs.currentData()
        self._open(replace(
            s,
            sample_rate=int(self.sample_rate.currentData() or 0),
            buffer_frames=int(self.buffer.currentData() or 0),
            exclusive=self.exclusive.isChecked() and not asio,
            output_channels=tuple(outputs) if asio and outputs is not None else s.output_channels,
        ))

    def _open(self, settings: AudioSettings, fall_back: bool = False) -> None:
        """Opens the device with these settings, and saves them if it opens. With
        `fall_back`, a device that doesn't take them opens with its own instead."""
        self.settings = settings
        error = self.bridge.open_device(settings)
        if error and fall_back:
            own = replace(settings, sample_rate=0, buffer_frames=0, output_channels=(), input_channels=())
            if own != settings and self.bridge.open_device(own) is None:
                self.settings, error = own, None
        if error is None:
            self.settings.save()
        self._refresh(error)

    def _show_control_panel(self) -> None:
        # The driver's dialog may run a message loop; this dialog waits for it.
        self.setEnabled(False)
        try:
            shown = self.bridge.show_device_control_panel()
        finally:
            self.setEnabled(True)
        if not shown:
            self._refresh("The driver has no settings dialog of its own.")


class MidiPage(QWidget):
    """The MIDI inputs connected: each on (tracks can hear and record it) or off.
    Inputs plugged in later show up with Refresh (and on the next start)."""

    def __init__(self, bridge: EngineBridge, parent: QWidget | None = None):
        super().__init__(parent)
        self.bridge = bridge
        self.inputs = QListWidget()
        self.inputs.setToolTip("MIDI tracks hear the inputs that are on (all of them, or the one they choose).")
        self.refresh_button = QPushButton("Refresh")
        self.refresh_button.setToolTip("Look for MIDI inputs plugged in or taken out since.")
        self.status = QLabel()
        self.status.setWordWrap(True)
        self.status.setStyleSheet(f"color: {theme.TEXT_DIM};")

        row = QHBoxLayout()
        row.setContentsMargins(0, 0, 0, 0)
        row.addWidget(self.refresh_button)
        row.addWidget(self.status, 1)
        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("MIDI Inputs"))
        layout.addWidget(self.inputs, 1)
        layout.addLayout(row)

        self.inputs.itemChanged.connect(self._toggled)
        self.refresh_button.clicked.connect(self.refresh)
        self._show()

    def refresh(self) -> None:
        self.bridge.open_midi_inputs()
        self._show()

    def _show(self) -> None:
        disabled = disabled_midi_inputs()
        with _quiet(self.inputs):
            self.inputs.clear()
            for name in self.bridge.midi_inputs():
                item = QListWidgetItem(name)
                item.setData(Qt.ItemDataRole.UserRole, name)
                item.setFlags(item.flags() | Qt.ItemFlag.ItemIsUserCheckable)
                item.setCheckState(Qt.CheckState.Unchecked if name in disabled else Qt.CheckState.Checked)
                error = self.bridge.midi_errors.get(name)
                if error:
                    item.setForeground(QColor("#ff6b5e"))
                    item.setToolTip(error)
                self.inputs.addItem(item)
        count = self.inputs.count()
        if count == 0:
            self.status.setText("No MIDI input is connected.")
        elif self.bridge.midi_errors:
            self.status.setText(f"{len(self.bridge.midi_errors)} could not be opened (see their tooltips).")
        else:
            self.status.setText(f"{count} MIDI input{'s' if count != 1 else ''}")

    def _toggled(self, item: QListWidgetItem) -> None:
        self.bridge.set_midi_input_enabled(item.data(Qt.ItemDataRole.UserRole),
                                           item.checkState() == Qt.CheckState.Checked)
        self._show()


def _same_folder(a: str | Path, b: str | Path) -> bool:
    return os.path.normcase(os.path.normpath(str(a))) == os.path.normcase(os.path.normpath(str(b)))


class PluginsPage(QWidget):
    """Where VST3 plug-ins are looked for, and rescanning them. Folders added or
    removed apply at once: only the new files are read, and a removed folder's
    plug-ins leave the browser."""

    def __init__(self, index: PluginIndex, parent: QWidget | None = None):
        super().__init__(parent)
        self.index = index
        self.folders = QListWidget()
        self.folders.setToolTip("Plug-ins are looked for in these folders and the folders inside them.")
        self.add_button = QPushButton("Add Folder…")
        self.remove_button = QPushButton("Remove")
        self.rescan_button = QPushButton("Rescan Plug-ins")
        self.rescan_button.setToolTip("Read every plug-in file again, also those that could not be read before.")
        self.scan_status = QLabel()
        self.scan_status.setWordWrap(True)
        self.scan_status.setStyleSheet(f"color: {theme.TEXT_DIM};")

        folder_buttons = QHBoxLayout()
        folder_buttons.setContentsMargins(0, 0, 0, 0)
        folder_buttons.addWidget(self.add_button)
        folder_buttons.addWidget(self.remove_button)
        folder_buttons.addStretch(1)
        scan_row = QHBoxLayout()
        scan_row.setContentsMargins(0, 0, 0, 0)
        scan_row.addWidget(self.rescan_button)
        scan_row.addWidget(self.scan_status, 1)

        layout = QVBoxLayout(self)
        layout.addWidget(QLabel("VST3 Folders"))
        layout.addWidget(self.folders, 1)
        layout.addLayout(folder_buttons)
        layout.addSpacing(8)
        layout.addLayout(scan_row)

        self.add_button.clicked.connect(lambda: self.add_folder())
        self.remove_button.clicked.connect(self.remove_folder)
        self.rescan_button.clicked.connect(self.rescan)
        self.folders.currentItemChanged.connect(self._update_buttons)
        index.updated.connect(self._show_scan)
        index.progress.connect(self._show_progress)
        self._show_folders()
        self._show_scan()

    def _show_folders(self, select: str | None = None) -> None:
        self.folders.clear()
        for path in standard_paths():
            item = QListWidgetItem(f"{path}  (standard)")
            item.setForeground(QColor(theme.TEXT_DIM))
            item.setToolTip("A standard VST3 folder: always searched.")
            self.folders.addItem(item)
        for folder in custom_folders():
            item = QListWidgetItem(folder)
            item.setData(Qt.ItemDataRole.UserRole, folder)
            if Path(folder).is_dir():
                item.setToolTip(folder)
            else:
                item.setToolTip(f"{folder}\nThis folder doesn't exist (any more).")
                item.setForeground(QColor("#ff6b5e"))
            self.folders.addItem(item)
            if select is not None and _same_folder(folder, select):
                self.folders.setCurrentItem(item)
        self._update_buttons()

    def _update_buttons(self) -> None:
        item = self.folders.currentItem()
        self.remove_button.setEnabled(item is not None and item.data(Qt.ItemDataRole.UserRole) is not None)

    def add_folder(self, folder: str | None = None) -> None:
        """Adds a folder (asks which if none is given) and scans it."""
        if not folder:
            folder = QFileDialog.getExistingDirectory(self, "Add VST3 Folder", str(Path.home()))
        if not folder:
            return
        folder = str(Path(folder))
        folders = custom_folders()
        if not any(_same_folder(folder, f) for f in [*standard_paths(), *folders]):
            set_custom_folders([*folders, folder])
            self.index.scan()  # reads only the new folder's files
        self._show_folders(select=folder)

    def remove_folder(self) -> None:
        item = self.folders.currentItem()
        folder = item.data(Qt.ItemDataRole.UserRole) if item is not None else None
        if folder is None:
            return
        set_custom_folders([f for f in custom_folders() if not _same_folder(f, folder)])
        self._show_folders()
        self.index.scan()  # its plug-ins leave the browser

    def rescan(self) -> None:
        """Reads every plug-in file again (also those that failed before)."""
        self.index.scan(rescan=True)

    def _show_progress(self, done: int, total: int, path: str) -> None:
        self.scan_status.setText(f"Scanning {done + 1}/{total}: {Path(path).stem}")

    def _show_scan(self) -> None:
        index = self.index
        self.rescan_button.setEnabled(not index.scanning)
        if index.scanning:
            self.scan_status.setText("Scanning plug-ins…")
            self.scan_status.setToolTip("")
            return
        count = len(index.plugins)
        text = f"{count} plug-in{'s' if count != 1 else ''} found"
        failures = index.failures
        if failures:
            text += f" · {len(failures)} file{'s' if len(failures) != 1 else ''} could not be read"
        self.scan_status.setText(text)
        self.scan_status.setToolTip("\n".join(f"{Path(f.path).name}: {f.reason}" for f in failures[:30]))


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
