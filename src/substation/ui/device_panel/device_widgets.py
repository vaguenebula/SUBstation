"""The kinds of devices in the device view: a built-in device (a knob per
parameter, or a list), a plug-in (its parameters, its own editor, its VST3
presets, why it didn't load), and a rack (its macros and chains: rack_view)."""

from __future__ import annotations

import base64
import os
from pathlib import Path

import numpy as np
from PySide6.QtCore import QSettings, QSize, Qt, Signal
from PySide6.QtGui import QFontMetrics
from PySide6.QtWidgets import QComboBox, QFileDialog, QLabel, QMenu, QWidget

from ... import theme
from ...audio.engine_bridge import EngineBridge
from ...model.automation import device_key
from ...model.devices import device_name
from ...model.editor import ProjectEditor
from ...model.params import format_value
from ...model.project import Chain, Device
from .. import icons
from ..rack_view import ChainList, MacroPanel
from ..widgets import Knob, ToggleButton
from .frame import HEADER_BUTTON, KNOB_SIZE, PARAM_WIDTH, _DeviceFrame, _elided

MESSAGE_LINES = 4  # a plug-in's error message is cut to this; its tooltip has it all
VST3_PRESET_FILTER = "VST3 Preset (*.vstpreset)"
RACK_WIDTH = 420


def _encode(state: bytes | None) -> str | None:
    return base64.b64encode(state).decode("ascii") if state else None


class DeviceWidget(_DeviceFrame):
    """A built-in device: its parameters are in the model."""

    def __init__(self, track_id: str, device: Device, editor: ProjectEditor, bridge: EngineBridge,
                 page: int = 0, parent: QWidget | None = None):
        super().__init__(track_id, device, editor, parent, bridge)
        self.knobs: dict[str, tuple[Knob, QLabel, str]] = {}
        self.choices: dict[str, QComboBox] = {}
        self.engine_id = bridge.engine_device_id(track_id, device.id)
        self.infos = bridge.engine.processor_params(self.engine_id) if self.engine_id is not None else []
        displays = bridge.engine.processor_displays(self.engine_id) if self.engine_id is not None else []
        self.displays = {d.id: index for index, d in enumerate(displays)}
        self._display_positions: dict[str, int] = {}
        self._set_param_count(len(self.infos), page)

    def read_display(self, display_id: str) -> np.ndarray:
        """The display's values the engine published since the last call (float32; empty without any)."""
        index = self.displays.get(display_id)
        if index is None:
            return np.zeros(0, np.float32)
        try:
            values, self._display_positions[display_id] = self.bridge.engine.read_processor_display(
                self.engine_id, index, self._display_positions.get(display_id, 0))
        except ValueError:  # its processor went before the widget did (it is about to be rebuilt)
            return np.zeros(0, np.float32)
        return values

    def refresh_state(self) -> None:
        """The device's state besides its parameters changed (Device.state): an
        editor that shows it (a sampler's sample) shows it anew."""

    def _clear_params(self) -> None:
        self.knobs.clear()
        self.choices.clear()

    def _param_widget(self, n: int) -> QWidget:
        info = self.infos[n]
        value = self.device().params.get(info.id, info.default_value)
        track_id, editor = self.track_id, self.editor
        cell, column = self._param_cell(info.name, info.id)
        if info.value_labels:
            choice = QComboBox()
            choice.addItems(info.value_labels)
            choice.setCurrentIndex(round(value))
            choice.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            choice.setFixedWidth(PARAM_WIDTH)
            choice.activated.connect(
                lambda i, pid=info.id: editor.set_device_param(track_id, self.device_id, pid, float(i)))
            column.addWidget(choice)
            column.addStretch(1)
            self.choices[info.id] = choice
            self._watch_touch(choice, info.id)
        else:
            knob = Knob(info.min_value, info.max_value, value, default=info.default_value,
                        bipolar=info.min_value < 0 < info.max_value and info.unit in ("", "st", "ct"),
                        log_scale=info.log_scale, formatter=lambda v, u=info.unit: format_value(v, u))
            knob.setFixedSize(KNOB_SIZE, KNOB_SIZE)
            readout = self._readout(format_value(value, info.unit))
            knob.valueChanged.connect(
                lambda v, key, pid=info.id: editor.set_device_param(track_id, self.device_id, pid, v, key))
            column.addWidget(knob, 0, Qt.AlignmentFlag.AlignHCenter)
            column.addWidget(readout)
            self.knobs[info.id] = (knob, readout, info.unit)
            self._watch_touch(knob, info.id)
            knob.set_automation(self.automation_state(info.id))
        return cell

    def _build_page(self) -> None:
        super()._build_page()
        self.refresh_automation()

    def refresh(self, device: Device) -> None:
        super().refresh(device)
        for param_id, (knob, readout, unit) in self.knobs.items():
            value = device.params.get(param_id)
            if value is not None:
                knob.setValue(value)
                readout.setText(_elided(format_value(value, unit), PARAM_WIDTH, readout))
        for param_id, choice in self.choices.items():
            value = device.params.get(param_id)
            if value is not None:
                choice.setCurrentIndex(round(value))
        self.refresh_automation()

    def follows_automation(self) -> bool:
        return any(self.automation_state(param_id) == "on" for param_id in [*self.knobs, *self.choices])

    def refresh_automation(self) -> None:
        """Automated parameters show their envelope's value (their own again when it stops)."""
        device = self.device()
        for param_id, (knob, readout, unit) in self.knobs.items():
            state = self.automation_state(param_id)
            knob.set_automation(state)
            value = (self.bridge.current_value(self.track_id, device_key(self.device_id, param_id)) if state == "on"
                     else device.params.get(param_id))
            if value is not None:
                knob.setValue(value)
                readout.setText(_elided(format_value(value, unit), PARAM_WIDTH, readout))
        for param_id, choice in self.choices.items():
            if self.automation_state(param_id) == "on":
                value = self.bridge.current_value(self.track_id, device_key(self.device_id, param_id))
            else:
                value = device.params.get(param_id)
            if value is not None:
                choice.setCurrentIndex(round(value))


class PluginDeviceWidget(_DeviceFrame):
    """A plug-in: its parameters live in the plug-in (the engine), shown a page
    at a time; the Edit button shows its own editor."""

    def __init__(self, track_id: str, device: Device, editor: ProjectEditor, bridge: EngineBridge,
                 page: int = 0, parent: QWidget | None = None):
        super().__init__(track_id, device, editor, parent, bridge)
        self.engine = bridge.engine
        self.engine_id = bridge.engine_device_id(track_id, device.id)
        self.knobs: dict[int, tuple[Knob, QLabel]] = {}  # parameter index ->
        self.choices: dict[int, QComboBox] = {}
        self.infos = self.engine.processor_params(self.engine_id) if self.engine_id is not None else []
        # What a generic editor should offer: what can be automated and isn't the plug-in's own business.
        shown = [i for i, p in enumerate(self.infos) if p.automatable and not p.hidden and not p.read_only]
        self.shown = shown or [i for i, p in enumerate(self.infos) if not p.hidden and not p.read_only]

        plugin = device.plugin
        self.title.setText(plugin.name)
        self._update_tooltip()

        self.edit = ToggleButton(icon=icons.plugin_window(), role="device-header",
                                 tooltip="Show the plug-in's own editor")
        self.edit.setFixedSize(HEADER_BUTTON, HEADER_BUTTON)
        self.edit.setIconSize(self.edit.size() - QSize(5, 5))
        self.edit.toggled.connect(self._toggle_editor)
        self.header.insertWidget(self.header.indexOf(self.title) + 1, self.edit)
        self.edit.setEnabled(self.engine_id is not None)
        self.update_editor_button()

        if self.engine_id is None or not self.shown:
            text = "No parameters to show here: use the plug-in's editor."
            if self.engine_id is None and bridge.plugin_pending(device.id):
                text = f"{plugin.name} is loading…"  # (a project just opened: its plug-ins load one by one)
            elif self.engine_id is None:
                text = bridge.plugin_errors.get(device.id) or f"{plugin.name} is not loaded."
            message = QLabel(text)
            message.setWordWrap(True)
            message.setStyleSheet(f"color: {theme.TEXT_DIM};")
            # A long one would make the device taller than the view.
            message.setMaximumHeight(QFontMetrics(message.font()).lineSpacing() * MESSAGE_LINES)
            message.setToolTip(text)
            self.body.insertWidget(self.body.indexOf(self.params) + 1, message)
        self._set_param_count(len(self.shown) if self.engine_id is not None else 0, page)

    def _update_tooltip(self) -> None:
        plugin = self.device().plugin
        lines = [plugin.name, plugin.vendor, self.bridge.plugin_path(plugin) or plugin.path]
        if self.engine_id is not None:
            latency = self.engine.processor_info(self.engine_id).latency
            if latency:
                lines.append(f"Latency: {latency} samples (compensated)")
        self.title.setToolTip("\n".join(line for line in lines if line))

    # --- Parameters ----------------------------------------------------------------

    def _text(self, index: int, value: float) -> str:
        return self.bridge.plugin_param_text(self.engine_id, index, value)

    def _clear_params(self) -> None:
        self.knobs.clear()
        self.choices.clear()

    def _param_widget(self, n: int) -> QWidget:
        index = self.shown[n]
        info = self.infos[index]
        value = self.engine.processor_param(self.engine_id, index)
        cell, column = self._param_cell(info.name, info.id)
        if info.value_labels:
            choice = QComboBox()
            choice.addItems(info.value_labels)
            choice.setCurrentIndex(round(value))
            choice.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            choice.setFixedWidth(PARAM_WIDTH)
            choice.activated.connect(lambda i, ix=index: self._edit(ix, float(i), None))
            column.addWidget(choice)
            column.addStretch(1)
            self.choices[index] = choice
            self._watch_touch(choice, info.id)
        else:
            centred = info.steps == 0 and abs(info.default_value - 0.5 * (info.min_value + info.max_value)) < 1e-6
            knob = Knob(info.min_value, info.max_value, value, default=info.default_value, bipolar=centred,
                        step=1.0 if info.steps else 0.0, formatter=lambda v, ix=index: self._text(ix, v))
            knob.setFixedSize(KNOB_SIZE, KNOB_SIZE)
            readout = self._readout(self._text(index, value))
            knob.valueChanged.connect(lambda v, key, ix=index: self._edit(ix, v, key))
            column.addWidget(knob, 0, Qt.AlignmentFlag.AlignHCenter)
            column.addWidget(readout)
            self.knobs[index] = (knob, readout)
            self._watch_touch(knob, info.id)
            knob.set_automation(self.automation_state(info.id))
        return cell

    def refresh_automation(self) -> None:
        """Marks automated parameters; the plug-in reports their values as they play (refresh_values)."""
        for index, (knob, _readout) in self.knobs.items():
            knob.set_automation(self.automation_state(self.infos[index].id))

    def _edit(self, index: int, value: float, gesture: object | None) -> None:
        old = self.engine.processor_param(self.engine_id, index)
        self.editor.set_device_param(self.track_id, self.device_id, self.infos[index].id, value, gesture, old=old)

    def refresh_values(self) -> None:
        if self.engine_id is None:
            return
        for index, (knob, readout) in self.knobs.items():
            value = self.engine.processor_param(self.engine_id, index)
            knob.setValue(value)
            readout.setText(_elided(self._text(index, value), PARAM_WIDTH, readout))
        for index, choice in self.choices.items():
            choice.setCurrentIndex(round(self.engine.processor_param(self.engine_id, index)))
        self.refresh_automation()
        self._update_tooltip()

    def refresh(self, device: Device) -> None:
        super().refresh(device)
        self.refresh_values()

    # --- Editor and presets --------------------------------------------------------

    def update_editor_button(self) -> None:
        self.edit.set_checked_silently(self.bridge.is_plugin_editor_open(self.track_id, self.device_id))

    def _toggle_editor(self, show: bool) -> None:
        if show:
            self.bridge.open_plugin_editor(self.track_id, self.device_id)
        else:
            self.bridge.close_plugin_editor(self.track_id, self.device_id)
        self.update_editor_button()

    def open_editor(self) -> None:
        if self.engine_id is not None:
            self._toggle_editor(True)  # already open: brought to the front

    def add_menu_actions(self, menu: QMenu) -> None:
        loaded = self.engine_id is not None
        menu.addAction("Show Editor", lambda: self._toggle_editor(True)).setEnabled(loaded)
        menu.addAction("Load VST3 Preset…", self.load_vst3_preset).setEnabled(loaded)
        menu.addAction("Save VST3 Preset…", self.save_vst3_preset).setEnabled(loaded)
        menu.addSeparator()

    def _preset_folder(self) -> str:
        stored = QSettings().value("plugins/preset_dir")
        if stored and os.path.isdir(str(stored)):
            return str(stored)
        plugin = self.device().plugin
        # Where VST3 presets usually live.
        folder = Path.home() / "Documents" / "VST3 Presets" / (plugin.vendor or "Unknown") / plugin.name
        return str(folder if folder.is_dir() else Path.home() / "Documents")

    def load_vst3_preset(self) -> None:
        """A .vstpreset (the plug-in's own preset format) into the plug-in."""
        path, _ = QFileDialog.getOpenFileName(self, "Load VST3 Preset", self._preset_folder(), VST3_PRESET_FILTER)
        if not path:
            return
        QSettings().setValue("plugins/preset_dir", str(Path(path).parent))
        try:
            data = Path(path).read_bytes()
            old = self.bridge.plugin_state(self.track_id, self.device_id)
            self.engine.set_processor_state(self.engine_id, data)  # fails if it is another plug-in's
        except (OSError, RuntimeError, ValueError) as exc:
            self.bridge.status_message.emit(f"Could not load {Path(path).name}: {exc}")
            return
        self.editor.set_device_state(self.track_id, self.device_id, _encode(old), _encode(data),
                                     f"Load Preset {Path(path).stem}")

    def save_vst3_preset(self) -> None:
        """The plug-in's state as a .vstpreset, for other hosts (the save button saves a SUBstation preset)."""
        plugin = self.device().plugin
        suggested = str(Path(self._preset_folder()) / f"{plugin.name}.vstpreset")
        path, _ = QFileDialog.getSaveFileName(self, "Save VST3 Preset", suggested, VST3_PRESET_FILTER)
        if not path:
            return
        QSettings().setValue("plugins/preset_dir", str(Path(path).parent))
        try:
            state = self.bridge.plugin_state(self.track_id, self.device_id)
            if state is not None:
                Path(path).write_bytes(state)
        except (OSError, RuntimeError) as exc:
            self.bridge.status_message.emit(f"Could not save the preset: {exc}")


class RackWidget(_DeviceFrame):
    """A rack: its macros and its chains (the device view shows the chain
    clicked beside it). Its save button saves it, with everything in it."""

    device_width = RACK_WIDTH
    chain_clicked = Signal(str, str)  # rack id, chain id

    def __init__(self, track_id: str, device: Device, editor: ProjectEditor, bridge: EngineBridge,
                 page: int = 0, parent: QWidget | None = None):
        super().__init__(track_id, device, editor, parent, bridge)
        self.macros = MacroPanel(track_id, device, editor, bridge)
        self.chains = ChainList(track_id, device, editor, bridge)
        self.chains.chain_clicked.connect(lambda chain_id: self.chain_clicked.emit(self.device_id, chain_id))
        self.content.addWidget(self.macros)
        self.content.addWidget(self.chains, 1)
        self._set_param_count(0, page)
        self._update_tooltip()

    def _param_widget(self, n: int) -> QWidget:
        raise IndexError(n)  # (it has none: its macros and chains instead)

    def _update_tooltip(self) -> None:
        latency = self.bridge.engine.processor_info(self.bridge.engine_device_id(self.track_id, self.device_id)).latency \
            if self.bridge.engine_device_id(self.track_id, self.device_id) is not None else 0
        self.title.setToolTip(device_name(self.device()) + (f"\nLatency: {latency} samples (compensated)" if latency
                                                            else ""))

    def refresh(self, device: Device) -> None:
        super().refresh(device)
        self.title.setText(device_name(device))
        self.macros.refresh(device)
        self.chains.refresh(device)
        self._update_tooltip()

    def refresh_chain(self, chain: Chain) -> None:
        self.chains.refresh_chain(chain)

    def show_chain(self, chain_id: str | None) -> None:
        self.chains.set_selected(chain_id)

    def follows_automation(self) -> bool:
        return self.chains.follows_automation()

    def refresh_automation(self) -> None:
        self.chains.refresh_automation()

    def refresh_displays(self) -> None:
        self.chains.refresh_meters()

    def add_menu_actions(self, menu: QMenu) -> None:
        menu.addAction("Add Chain", lambda: self.editor.add_rack_chain(self.track_id, self.device_id))
        menu.addSeparator()
