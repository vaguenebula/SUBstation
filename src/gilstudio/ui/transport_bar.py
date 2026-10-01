"""Top control bar: tempo, time signature, metronome, key | transport (record and its
count-in), oscilloscope | loop, follow, CPU."""

from __future__ import annotations

from PySide6.QtCore import QSettings, Qt, Signal
from PySide6.QtGui import QColor, QMouseEvent, QPainter
from PySide6.QtWidgets import QComboBox, QFrame, QHBoxLayout, QLabel, QWidget

from .. import theme
from ..audio.engine_bridge import EngineBridge
from ..model.editor import ProjectEditor
from ..model.keys import ALL_KEYS
from ..model.timebase import TimeSignature, VALID_DENOMINATORS, split_position
from . import icons
from .arrangement.view_state import ViewState
from .widgets import Oscilloscope, ToggleButton, ValueBox


class _ClickableLabel(QLabel):
    clicked = Signal()

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self.clicked.emit()


def _separator() -> QFrame:
    line = QFrame()
    line.setFrameShape(QFrame.Shape.VLine)
    line.setStyleSheet(f"color: {theme.BORDER};")
    return line


COUNT_IN_BARS = (0, 1, 2, 4)
COUNT_IN_KEY = "transport/count_in_bars"


def count_in_bars() -> int:
    """The count-in before recording, in bars (saved with the preferences)."""
    try:
        bars = int(QSettings().value(COUNT_IN_KEY, 0))
    except (TypeError, ValueError):
        return 0
    return bars if bars in COUNT_IN_BARS else 0


class TransportBar(QWidget):
    record_requested = Signal()
    play_requested = Signal()
    stop_requested = Signal()
    preferences_requested = Signal()
    re_enable_requested = Signal()

    def __init__(self, editor: ProjectEditor, bridge: EngineBridge, view: ViewState, parent: QWidget | None = None):
        super().__init__(parent)
        self.editor = editor
        self.project = editor.project
        self.bridge = bridge
        self.view = view
        self.setFixedHeight(40)

        self.tempo = ValueBox(120.0, 20.0, 999.0, step=0.25, decimals=2, sample_text="999.00")
        self.tempo.setToolTip("Tempo (drag, Shift for fine steps, double-click to type)")
        self.ts_num = ValueBox(4, 1, 32, step=0.1, decimals=0, formatter=lambda v: str(int(v)), sample_text="32")
        self.ts_den = ValueBox(4, 1, 32, decimals=0, choices=VALID_DENOMINATORS, formatter=lambda v: str(int(v)),
                               sample_text="32")
        self.metronome = ToggleButton(icon=icons.metronome(), role="tool", tooltip="Metronome")
        # The project's key: audio added with a key in its file name is transposed to it.
        self.key = QComboBox()
        self.key.addItem("No Key")
        for key in ALL_KEYS:
            self.key.addItem(key.label)
        self.key.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.key.setMaxVisibleItems(25)
        self.key.setToolTip("Project key. Audio files with a key in their name (\"Loop_128_Am\")\n"
                            "are transposed to it when added; a tempo in the name warps them to it.")

        self.play = ToggleButton(icon=icons.play(), role="play", tooltip="Play / Stop (Space)")
        self.stop = ToggleButton(icon=icons.stop(), role="tool", checkable=False,
                                 tooltip="Stop (press again to return to the start)")
        self.record = ToggleButton(icon=icons.record(), role="record",
                                   tooltip="Arrangement Record (F9): records the armed tracks")
        self.count_in = QComboBox()
        for bars in COUNT_IN_BARS:
            self.count_in.addItem("No Count-In" if bars == 0 else f"Count-In {bars} Bar{'s' if bars > 1 else ''}", bars)
        self.count_in.setCurrentIndex(COUNT_IN_BARS.index(count_in_bars()))
        self.count_in.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.count_in.setToolTip("Count-in: the metronome counts in this long before recording starts")
        # Lit while automation is overridden (a target changed by hand): a click brings it back.
        self.re_enable = ToggleButton(icon=icons.re_enable_automation(), role="re-enable",
                                      tooltip="Re-Enable Automation")
        self.re_enable.setEnabled(False)
        self.lock_envelopes = ToggleButton(icon=icons.lock_envelopes(), role="tool",
                                           tooltip="Lock Envelopes: automation stays in place when clips move "
                                                   "(off: it moves with them)")
        self.position = QLabel()
        self.position.setFont(theme.ui_font(11, bold=True))
        self.position.setMinimumWidth(96)
        self.position.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self.position.setStyleSheet(
            f"background: {theme.SURFACE}; border: 1px solid {theme.BORDER}; border-radius: 3px; padding: 1px 6px;")

        self.scope = Oscilloscope(bridge.engine)

        self.loop = ToggleButton(icon=icons.loop(), role="tool", tooltip="Loop (Ctrl+L)")
        self.follow = ToggleButton(icon=icons.follow(), role="tool", tooltip="Follow the playhead")
        self.follow.setChecked(view.follow)
        self.cpu = QLabel("CPU 0%")
        self.cpu.setStyleSheet(f"color: {theme.TEXT_DIM};")
        self.cpu.setMinimumWidth(60)
        self.device = _ClickableLabel("No audio device")
        self.device.setStyleSheet(f"color: {theme.TEXT_DIM};")
        self.device.setCursor(Qt.CursorShape.PointingHandCursor)
        self.device.setToolTip("Audio device. Click to open Preferences.")

        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 5, 8, 5)
        layout.setSpacing(5)
        tempo_label = QLabel("BPM")
        tempo_label.setStyleSheet(f"color: {theme.TEXT_DIM};")
        slash = QLabel("/")
        for widget in (self.tempo, tempo_label, _separator(), self.ts_num, slash, self.ts_den, _separator(),
                       self.metronome, self.key):
            layout.addWidget(widget)
        layout.addStretch(1)
        for widget in (self.position, self.play, self.stop, self.record, self.count_in, self.re_enable,
                       self.lock_envelopes):
            layout.addWidget(widget)
        layout.addSpacing(6)
        layout.addWidget(self.scope)
        layout.addStretch(1)
        for widget in (self.loop, self.follow, _separator(), self.cpu, self.device):
            layout.addWidget(widget)

        self.tempo.valueChanged.connect(lambda v, key: self.editor.set_tempo(v, key))
        self.ts_num.valueChanged.connect(lambda v, _k: self._set_signature(numerator=int(v)))
        self.ts_den.valueChanged.connect(lambda v, _k: self._set_signature(denominator=int(v)))
        self.metronome.toggled.connect(bridge.set_metronome)
        self.key.activated.connect(lambda i: self.editor.set_key(ALL_KEYS[i - 1] if i > 0 else None))
        self.play.clicked.connect(self._play_clicked)
        self.stop.clicked.connect(self.stop_requested)
        self.record.clicked.connect(self._record_clicked)
        self.count_in.activated.connect(lambda i: QSettings().setValue(COUNT_IN_KEY, COUNT_IN_BARS[i]))
        self.loop.toggled.connect(self.editor.set_loop_enabled)
        self.follow.toggled.connect(self._set_follow)
        self.device.clicked.connect(self.preferences_requested)
        self.re_enable.clicked.connect(self.re_enable_requested)
        self.lock_envelopes.toggled.connect(self.editor.set_automation_locked)

        self.project.settings_changed.connect(self.refresh)
        self.project.reset.connect(self.refresh)
        bridge.position_changed.connect(self._show_position)
        bridge.transport_changed.connect(self.play.set_checked_silently)
        bridge.recording_changed.connect(self.record.set_checked_silently)
        bridge.device_changed.connect(self.refresh_device)
        bridge.meters_updated.connect(self._show_cpu)
        bridge.automation_state_changed.connect(self._show_overrides)
        self._cpu_ticks = 0
        self.refresh()
        self._show_position(0.0)

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(theme.PANEL))
        p.fillRect(0, self.height() - 1, self.width(), 1, QColor(theme.BORDER))

    def refresh(self) -> None:
        p = self.project
        self.tempo.setValue(p.tempo)
        self.ts_num.setValue(p.time_signature.numerator)
        self.ts_den.setValue(p.time_signature.denominator)
        self.key.setCurrentIndex(ALL_KEYS.index(p.key) + 1 if p.key else 0)
        self.loop.set_checked_silently(p.loop_enabled)
        self.lock_envelopes.set_checked_silently(p.automation_locked)
        self._show_position(self.bridge.position)

    def refresh_device(self) -> None:
        status = self.bridge.engine.device_status
        if status.open:
            name = status.name if len(status.name) <= 28 else status.name[:27] + "…"
            self.device.setText(f"{name} · {status.sample_rate / 1000:g} kHz")
            self.device.setToolTip(f"{status.backend}: {status.name}\n{status.sample_rate} Hz, "
                                   f"{status.buffer_frames} frames, ~{status.latency_ms:.1f} ms output latency"
                                   f"{' (exclusive)' if status.exclusive else ''}\nClick to open Preferences.")
        else:
            self.device.setText("No audio device")
            self.device.setToolTip("No audio output is open. Click to open Preferences.")

    def _set_signature(self, numerator: int | None = None, denominator: int | None = None) -> None:
        ts = self.project.time_signature
        self.editor.set_time_signature(TimeSignature(numerator or ts.numerator, denominator or ts.denominator))

    def _set_follow(self, enabled: bool) -> None:
        self.view.follow = enabled

    def count_in_beats(self) -> float:
        """The chosen count-in, in beats of the project's time signature."""
        ts = self.project.time_signature
        return COUNT_IN_BARS[self.count_in.currentIndex()] * ts.numerator * 4.0 / ts.denominator

    def _record_clicked(self) -> None:
        self.record.set_checked_silently(self.bridge.is_recording)  # state follows the bridge
        self.record_requested.emit()

    def _play_clicked(self) -> None:
        self.play.set_checked_silently(self.bridge.is_playing)  # state follows the engine
        self.play_requested.emit()

    def _show_position(self, beat: float) -> None:
        bar, beat_index, sixteenth = split_position(beat, self.project.time_signature)
        text = f"{bar + 1:>3}. {beat_index + 1}. {sixteenth + 1}"
        if self.position.text() != text:
            self.position.setText(text)

    def _show_overrides(self, _owner: str = "") -> None:
        overridden = self.bridge.has_overrides
        self.re_enable.setEnabled(overridden)
        self.re_enable.set_checked_silently(overridden)

    def _show_cpu(self) -> None:
        self._cpu_ticks = (self._cpu_ticks + 1) % 15
        if self._cpu_ticks == 0:
            self.cpu.setText(f"CPU {self.bridge.engine.cpu_load * 100:.0f}%")
