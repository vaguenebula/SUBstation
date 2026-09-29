"""Bottom 'detail view': the selected track's device chain. On a MIDI track the
instrument comes first.

Built-in devices only for now; this is also where VST3/CLAP devices will live.
Parameter metadata comes from the engine, so plugin parameters will show up
here without UI changes: a knob per parameter (log-scaled where the engine
says so), or a list for parameters that choose between named values.
"""

from __future__ import annotations

from PySide6.QtCore import Qt, Signal
from PySide6.QtGui import QColor, QDragEnterEvent, QDropEvent, QPainter
from PySide6.QtWidgets import (
    QComboBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QScrollArea,
    QVBoxLayout,
    QWidget,
)

from .. import theme
from ..audio.engine_bridge import EngineBridge
from ..model.editor import BUILTIN_DEVICES, ProjectEditor, is_instrument
from ..model.project import Device
from .arrangement.view_state import Selection
from .browser.browser_models import PLUGIN_MIME, device_kinds
from .widgets import Knob, ToggleButton

PANEL_HEIGHT = 150
EFFECTS_HINT = "Drop audio effects here from the browser (Built-in \u203a Audio Effects)"
INSTRUMENT_HINT = "Drop an instrument here from the browser (Built-in \u203a Instruments)"
INSTRUMENT_REFUSED = "Instruments go on MIDI tracks (Create \u203a Insert MIDI Track)."


def _format_value(value: float, unit: str) -> str:
    if unit == "dB":
        return f"{value:.1f} dB"
    if unit == "%":
        return f"{value:.0f} %"
    if unit == "":
        return f"{value:+.2f}" if value else "0.00"
    if unit == "Hz":
        return f"{value / 1000:.2f} kHz" if value >= 1000 else f"{value:.0f} Hz"
    if unit == "ms":
        if value >= 1000:
            return f"{value / 1000:.2f} s"
        return f"{value:.1f} ms" if value < 10 else f"{value:.0f} ms"
    return f"{value:.2f} {unit}"


class DeviceWidget(QFrame):
    def __init__(self, track_id: str, device: Device, editor: ProjectEditor, bridge: EngineBridge,
                 parent: QWidget | None = None):
        super().__init__(parent)
        self.track_id = track_id
        self.device_id = device.id
        self.editor = editor
        self.setObjectName("device")
        self.setStyleSheet(f"#device {{ background: {theme.PANEL_ALT}; border: 1px solid {theme.BORDER};"
                           f" border-radius: 4px; }}")

        self.enabled = ToggleButton(role="activator", tooltip="Device On/Off")
        self.enabled.setFixedSize(14, 14)
        self.enabled.setChecked(device.enabled)
        self.enabled.toggled.connect(lambda on: editor.set_device_enabled(track_id, self.device_id, on))
        title = QLabel(BUILTIN_DEVICES.get(device.kind, (device.kind,))[0])
        title.setFont(theme.ui_font(9, bold=True))
        remove = QPushButton("×")
        remove.setProperty("role", "flat")
        remove.setFont(theme.ui_font(11))
        remove.setFixedSize(18, 18)
        remove.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        remove.setToolTip("Delete device")
        remove.clicked.connect(lambda: editor.remove_device(track_id, self.device_id))
        header = QHBoxLayout()
        header.setSpacing(6)
        header.addWidget(self.enabled)
        header.addWidget(title)
        header.addStretch(1)
        header.addWidget(remove)

        self.knobs: dict[str, tuple[Knob, QLabel, str]] = {}
        self.choices: dict[str, QComboBox] = {}
        params = QHBoxLayout()
        params.setSpacing(10)
        engine_id = bridge.engine_device_id(track_id, device.id)
        infos = bridge.engine.processor_params(engine_id) if engine_id is not None else []
        for info in infos:
            value = device.params.get(info.id, info.default_value)
            name = QLabel(info.name)
            name.setAlignment(Qt.AlignmentFlag.AlignCenter)
            name.setStyleSheet(f"color: {theme.TEXT_DIM}; font-size: 8pt;")
            column = QVBoxLayout()
            column.setSpacing(1)
            column.addWidget(name)
            if info.value_labels:
                choice = QComboBox()
                choice.addItems(info.value_labels)
                choice.setCurrentIndex(round(value))
                choice.setFocusPolicy(Qt.FocusPolicy.NoFocus)
                choice.activated.connect(
                    lambda i, pid=info.id: editor.set_device_param(track_id, self.device_id, pid, float(i)))
                column.addWidget(choice)
                column.addStretch(1)
                self.choices[info.id] = choice
            else:
                knob = Knob(info.min_value, info.max_value, value, default=info.default_value,
                            bipolar=info.min_value < 0 < info.max_value and info.unit == "",
                            log_scale=info.log_scale, formatter=lambda v, u=info.unit: _format_value(v, u))
                knob.setFixedSize(38, 38)
                readout = QLabel(_format_value(value, info.unit))
                readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
                readout.setStyleSheet("font-size: 8pt;")
                knob.valueChanged.connect(
                    lambda v, key, pid=info.id: editor.set_device_param(track_id, self.device_id, pid, v, key))
                column.addWidget(knob, 0, Qt.AlignmentFlag.AlignHCenter)
                column.addWidget(readout)
                self.knobs[info.id] = (knob, readout, info.unit)
            params.addLayout(column)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 8)
        layout.addLayout(header)
        layout.addLayout(params)
        layout.addStretch(1)

    def refresh(self, device: Device) -> None:
        self.enabled.set_checked_silently(device.enabled)
        for param_id, (knob, readout, unit) in self.knobs.items():
            value = device.params.get(param_id)
            if value is not None:
                knob.setValue(value)
                readout.setText(_format_value(value, unit))
        for param_id, choice in self.choices.items():
            value = device.params.get(param_id)
            if value is not None:
                choice.setCurrentIndex(round(value))


class DevicePanel(QFrame):
    status_message = Signal(str)

    def __init__(self, editor: ProjectEditor, selection: Selection, bridge: EngineBridge,
                 parent: QWidget | None = None):
        super().__init__(parent)
        self.editor = editor
        self.project = editor.project
        self.selection = selection
        self.bridge = bridge
        self.track_id: str | None = None
        self.widgets: dict[str, DeviceWidget] = {}
        self.setFixedHeight(PANEL_HEIGHT)
        self.setAcceptDrops(True)

        self.title = QLabel()
        self.title.setFixedWidth(130)
        self.title.setWordWrap(True)
        self.title.setAlignment(Qt.AlignmentFlag.AlignTop | Qt.AlignmentFlag.AlignLeft)
        self.title.setFont(theme.ui_font(9, bold=True))

        self.chain = QWidget()
        self.chain_layout = QHBoxLayout(self.chain)
        self.chain_layout.setContentsMargins(0, 0, 0, 0)
        self.chain_layout.setSpacing(6)
        scroll = QScrollArea()
        scroll.setWidget(self.chain)
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        scroll.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        # Let the panel's own background show through (a plain `background:` rule
        # would cascade into every child widget).
        scroll.setStyleSheet("QScrollArea { background: transparent; }")
        scroll.viewport().setAutoFillBackground(False)
        self.chain.setAutoFillBackground(False)

        self.hint = QLabel(EFFECTS_HINT)
        self.hint.setStyleSheet(f"color: {theme.TEXT_DISABLED};")

        layout = QHBoxLayout(self)
        layout.setContentsMargins(10, 8, 10, 8)
        layout.addWidget(self.title)
        layout.addWidget(scroll, 1)

        selection.changed.connect(self._on_selection)
        self.project.devices_changed.connect(self._on_devices_changed)
        self.project.device_param_changed.connect(self._on_param_changed)
        self.project.track_changed.connect(self._on_track_changed)
        self.project.reset.connect(lambda: self.show_track(None))
        self.project.track_removed.connect(lambda tid, _i: self.show_track(None) if tid == self.track_id else None)
        self.show_track(None)

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(theme.PANEL))
        p.fillRect(0, 0, self.width(), 1, QColor(theme.BORDER))

    def _on_selection(self) -> None:
        if self.selection.track_id != self.track_id:
            self.show_track(self.selection.track_id)

    def _on_devices_changed(self, track_id: str) -> None:
        if track_id == self.track_id:
            self.show_track(track_id)

    def _on_param_changed(self, track_id: str, device_id: str, _param_id: str) -> None:
        if track_id == self.track_id and device_id in self.widgets:
            self.widgets[device_id].refresh(self.project.device(track_id, device_id))

    def _on_track_changed(self, track_id: str) -> None:
        if track_id == self.track_id:
            self.title.setText(self.project.track(track_id).name)

    def show_track(self, track_id: str | None) -> None:
        if track_id is not None and not self.project.has_track(track_id):
            track_id = None
        self.track_id = track_id
        while self.chain_layout.count():
            item = self.chain_layout.takeAt(0)
            if item.widget() and item.widget() is not self.hint:
                item.widget().deleteLater()
        self.widgets.clear()
        if track_id is None:
            self.title.setText("No track selected")
            self.hint.hide()
            return
        track = self.project.track(track_id)
        self.title.setText(track.name)
        for device in track.devices:
            widget = DeviceWidget(track_id, device, self.editor, self.bridge)
            self.widgets[device.id] = widget
            self.chain_layout.addWidget(widget)
        self.chain_layout.addWidget(self.hint)
        self.chain_layout.addStretch(1)
        needs_instrument = track.is_midi and not any(is_instrument(d.kind) for d in track.devices)
        self.hint.setText(INSTRUMENT_HINT if needs_instrument else EFFECTS_HINT)
        self.hint.setVisible(needs_instrument or not track.devices)

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:
        mime = event.mimeData()
        if self.track_id is not None and (device_kinds(mime) or mime.hasFormat(PLUGIN_MIME)):
            event.acceptProposedAction()

    def dropEvent(self, event: QDropEvent) -> None:
        kinds = [k for k in device_kinds(event.mimeData()) if k in BUILTIN_DEVICES]
        if kinds and self.track_id is not None:
            refused = [kind for kind in kinds if self.editor.add_device(self.track_id, kind) is None]
            if refused:
                self.status_message.emit(INSTRUMENT_REFUSED)
        else:
            self.status_message.emit("VST3/CLAP plugin hosting is not available yet.")
        event.acceptProposedAction()
