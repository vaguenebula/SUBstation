"""The Delay's editor, laid out as Ableton's: each side's time (Sync on: a grid
of sixteenths and an offset; off: a time knob) and the link between them; the
filter on the echoes (a curve to drag: across for its frequency, up and down
for its width) over a spectrum of the input; the mode the time changes in and
ping pong; and on the right the feedback with its freeze, over the dry/wet mix.

Every control sets its parameter as the generic knobs do (undoably), shows
automation as it plays, and right-click gives its automation menu."""

from __future__ import annotations

import math
from collections.abc import Callable

import numpy as np
from PySide6.QtCore import QPointF, QRectF, QSize, Qt
from PySide6.QtGui import QColor, QMouseEvent, QPainter, QPainterPath, QPen, QWheelEvent
from PySide6.QtWidgets import (
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from ... import theme
from ...model.automation import device_key
from ...model.params import format_value
from .. import icons
from ..device_panel import KNOB_SIZE, DeviceWidget
from ..widgets import Knob, ToggleButton
from ..widgets.knob import DRAG_PIXELS, FINE_DRAG_PIXELS
from ..widgets.value_box import ValueBox
from . import device_editor

DIVISIONS = ("1", "2", "3", "4", "5", "6", "8", "16")  # the sixteenths, as the engine lists them
MODES = ("Repitch", "Fade", "Jump")
BUTTON_HEIGHT = 16
BOX_HEIGHT = 18
SIDE_WIDTH = 56
LINK_WIDTH = 18
GRAPH_WIDTH = 220
MODE_WIDTH = 62
RIGHT_WIDTH = 64
SMALL_KNOB = 30
SPACING = 10
GRAPH_LOW, GRAPH_HIGH = 20.0, 20000.0  # Hz across the graph
GRAPH_FLOOR_DB = -30.0
WIDTH_MIN, WIDTH_MAX = 0.5, 9.0
FFT_SIZE = 4096
SPECTRUM_FLOOR_DB = -90.0
SPECTRUM_FALL_DB = 1.0  # per display refresh (~60 a second)


def _alpha(color, opacity: int) -> QColor:
    result = QColor(color)
    result.setAlpha(opacity)
    return result


def _label(text: str) -> QLabel:
    label = QLabel(text)
    label.setAlignment(Qt.AlignmentFlag.AlignCenter)
    label.setStyleSheet(f"color: {theme.TEXT_DIM}; font-size: 8pt;")
    return label


def _button(text: str = "", tooltip: str = "", icon=None) -> ToggleButton:
    button = ToggleButton(text, role="small", icon=icon, tooltip=tooltip)
    button.setFixedHeight(BUTTON_HEIGHT)
    if icon is not None:
        button.setIconSize(QSize(12, 12))
    return button


def filter_response(freq: float, center: float, width: float) -> float:
    """The filter's gain in dB at `freq`: a 12 dB/octave high-pass and low-pass,
    `width` octaves apart around `center`, as the engine has them."""
    low, high = center * 2 ** (-width / 2), center * 2 ** (width / 2)
    hp = (freq / low) ** 2 / math.sqrt(1 + (freq / low) ** 4)
    lp = 1 / math.sqrt(1 + (freq / high) ** 4)
    return 20 * math.log10(max(hp * lp, 1e-9))


def _parse_frequency(text: str) -> float | None:
    cleaned = text.strip().lower().removesuffix("hz").strip()
    scale = 1.0
    if cleaned.endswith("k"):
        cleaned, scale = cleaned[:-1].strip(), 1000.0
    try:
        return float(cleaned) * scale
    except ValueError:
        return None


class LogValueBox(ValueBox):
    """A ValueBox that drags (and wheels) evenly in log(value): frequencies, rates."""

    def _moved(self, value: float, fraction: float) -> float:
        return value * (self._max / self._min) ** fraction

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._drag_origin is None:
            return
        pixels = FINE_DRAG_PIXELS if event.modifiers() & Qt.KeyboardModifier.ShiftModifier else DRAG_PIXELS
        self.relative = True
        self._set_from_user(self._drag_by(event, lambda value, up: self._moved(value, up / pixels)),
                            self._gesture)

    def wheelEvent(self, event: QWheelEvent) -> None:
        notches = event.angleDelta().y() / 120.0
        if notches:
            self.relative = True
            self._set_from_user(self._moved(self._value, notches / 50.0), object())
        event.accept()


class FilterGraph(QWidget):
    """The filter's response: drag across for its frequency, up and down for its width."""

    def __init__(self, widget: DelayWidget):
        super().__init__()
        self.widget = widget
        self._gesture: object | None = None
        self.sample_rate = 48000.0
        self._samples = np.zeros(FFT_SIZE, np.float32)  # the input's latest samples
        self._window = np.hanning(FFT_SIZE).astype(np.float32)
        self.spectrum = np.full(FFT_SIZE // 2 + 1, SPECTRUM_FLOOR_DB, np.float32)  # dB per bin, falling slowly
        self.setFixedWidth(GRAPH_WIDTH)
        self.setMinimumHeight(60)
        self.setCursor(Qt.CursorShape.SizeAllCursor)
        self.setToolTip("Drag across for the filter's frequency, up and down for its width")

    def _plot(self) -> QRectF:
        return QRectF(self.rect()).adjusted(1, 1, -1, -1)

    def add_samples(self, samples: np.ndarray, sample_rate: float) -> None:
        """The input's samples since the last call: the spectrum follows them (and falls back without any)."""
        self.sample_rate = sample_rate
        if len(samples):
            samples = np.nan_to_num(np.asarray(samples[-FFT_SIZE:], np.float32))
            self._samples = np.concatenate((self._samples[len(samples):], samples))
            magnitude = np.abs(np.fft.rfft(self._samples * self._window)) * (2.0 / self._window.sum())
            new = (20.0 * np.log10(np.maximum(magnitude, 1e-9))).astype(np.float32)
        else:
            new = np.full_like(self.spectrum, SPECTRUM_FLOOR_DB)
        falling = np.maximum(self.spectrum - SPECTRUM_FALL_DB, SPECTRUM_FLOOR_DB)
        spectrum = np.maximum(new, falling)
        if not np.array_equal(spectrum, self.spectrum):
            self.spectrum = spectrum
            self.update()

    def spectrum_columns(self, columns: int) -> np.ndarray:
        """The spectrum in dB under each of `columns` columns across the graph: the loudest bin
        under a column, or (low down, where a bin spans columns) interpolated between bins."""
        hz_per_bin = self.sample_rate / FFT_SIZE
        fractions = np.arange(columns + 1) / columns
        edges = GRAPH_LOW * (GRAPH_HIGH / GRAPH_LOW) ** fractions / hz_per_bin  # in bins
        centers = np.sqrt(edges[:-1] * edges[1:])
        values = np.interp(centers, np.arange(len(self.spectrum)), self.spectrum)
        first = np.clip(np.ceil(edges[:-1]).astype(np.int64), 0, len(self.spectrum) - 1)
        last = np.clip(np.floor(edges[1:]).astype(np.int64), 0, len(self.spectrum) - 1)
        for i in np.flatnonzero(last >= first):
            values[i] = max(values[i], self.spectrum[first[i]:last[i] + 1].max())
        return values

    def _x(self, freq: float) -> float:
        plot = self._plot()
        return plot.left() + math.log(freq / GRAPH_LOW) / math.log(GRAPH_HIGH / GRAPH_LOW) * plot.width()

    def _freq(self, x: float) -> float:
        plot = self._plot()
        fraction = min(1.0, max(0.0, (x - plot.left()) / plot.width()))
        return GRAPH_LOW * (GRAPH_HIGH / GRAPH_LOW) ** fraction

    def _dot_y(self, width: float) -> float:
        plot = self._plot()
        inner = plot.adjusted(0, 8, 0, -8)
        return inner.bottom() - (width - WIDTH_MIN) / (WIDTH_MAX - WIDTH_MIN) * inner.height()

    def _width(self, y: float) -> float:
        inner = self._plot().adjusted(0, 8, 0, -8)
        fraction = min(1.0, max(0.0, (inner.bottom() - y) / inner.height()))
        return WIDTH_MIN + fraction * (WIDTH_MAX - WIDTH_MIN)

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            super().mousePressEvent(event)
            return
        self._gesture = object()
        self.widget.touch("freq")
        self._drag_to(event.position())

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._gesture is not None:
            self._drag_to(event.position())

    def mouseReleaseEvent(self, _event: QMouseEvent) -> None:
        self._gesture = None

    def _drag_to(self, pos: QPointF) -> None:
        self.widget.set_params({"freq": min(18000.0, max(50.0, self._freq(pos.x()))),
                                "width": round(self._width(pos.y()), 2)}, self._gesture)

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        plot = self._plot()
        p.fillRect(self.rect(), theme.METER_BG)
        for decade in (10.0, 100.0, 1000.0, 10000.0):  # a line per decade, fainter ones between
            for multiple in range(1, 10):
                freq = decade * multiple
                if GRAPH_LOW < freq < GRAPH_HIGH:
                    p.setPen(QPen(_alpha(theme.GRID_BEAT, 200 if multiple == 1 else 90), 1))
                    x = self._x(freq)
                    p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()))

        # The input's spectrum, behind the curve.
        columns = max(2, int(plot.width()))
        levels = self.spectrum_columns(columns)
        if levels.max() > SPECTRUM_FLOOR_DB:
            heights = np.clip((levels - SPECTRUM_FLOOR_DB) / -SPECTRUM_FLOOR_DB, 0.0, 1.0) * plot.height()
            shape = QPainterPath(QPointF(plot.left(), plot.bottom()))
            for i, h in enumerate(heights):
                shape.lineTo(plot.left() + (i + 0.5) * plot.width() / columns, plot.bottom() - float(h))
            shape.lineTo(plot.right(), plot.bottom())
            shape.closeSubpath()
            p.fillPath(shape, _alpha(theme.TEXT, 34))
            p.strokePath(shape, QPen(_alpha(theme.TEXT, 70), 1))

        on = self.widget.value("filter") >= 0.5
        center, width = self.widget.value("freq"), self.widget.value("width")
        color = QColor(theme.SCOPE_LINE) if on else QColor(theme.TEXT_DISABLED)
        top = plot.top() + 10

        def y(db: float) -> float:
            return top + max(0.0, min(1.0, db / GRAPH_FLOOR_DB)) * (plot.bottom() - top)

        curve = QPainterPath()
        for i in range(columns + 1):
            x = plot.left() + i * plot.width() / columns
            point = QPointF(x, y(filter_response(self._freq(x), center, width) if on else 0.0))
            if i == 0:
                curve.moveTo(point)
            else:
                curve.lineTo(point)
        area = QPainterPath(curve)
        area.lineTo(plot.right(), plot.bottom())
        area.lineTo(plot.left(), plot.bottom())
        area.closeSubpath()
        p.fillPath(area, _alpha(color, 28))
        p.setPen(QPen(color, 1.5))
        p.drawPath(curve)

        dot = QPointF(self._x(center), self._dot_y(width))
        p.setPen(QPen(QColor(theme.ACCENT) if on else QColor(theme.TEXT_DIM), 2))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawEllipse(dot, 5, 5)
        p.end()


@device_editor("delay")
class DelayWidget(DeviceWidget):
    device_width = 2 * SIDE_WIDTH + LINK_WIDTH + GRAPH_WIDTH + MODE_WIDTH + RIGHT_WIDTH + 4 * SPACING + 18

    def __init__(self, *args, **kwargs):
        self._views: dict[str, list[Callable[[float, str | None], None]]] = {}
        self._times: dict[str, QStackedWidget] = {}  # per side: synced (0) or free (1)
        self._right: list[QWidget] = []  # the right side's controls, off while linked
        super().__init__(*args, **kwargs)
        self.defaults = {info.id: info.default_value for info in self.infos}
        self.content.removeItem(self.params)  # its own layout instead of pages of knobs
        panel = QHBoxLayout()
        panel.setContentsMargins(0, 0, 0, 0)
        panel.setSpacing(SPACING)
        sides = QHBoxLayout()
        sides.setSpacing(0)
        sides.addWidget(self._side("l", "Left"))
        self.link = _button(tooltip="Link: the right side follows the left", icon=icons.link())
        self.link.setFixedSize(LINK_WIDTH, BUTTON_HEIGHT)
        self._toggle(self.link, "link")
        sides.addWidget(self.link, 0, Qt.AlignmentFlag.AlignVCenter)
        sides.addWidget(self._side("r", "Right"))
        panel.addLayout(sides)
        panel.addWidget(self._filter_section())
        panel.addWidget(self._mode_section())
        panel.addWidget(self._right_section())
        self.content.addLayout(panel)
        self._sync()

    # --- Parameters ------------------------------------------------------------------

    def _set_param_count(self, count: int, page: int = 0) -> None:
        self.param_count, self.pages, self.page = count, 1, 0
        for widget in (self.previous, self.page_label, self.next):
            widget.setVisible(False)

    def value(self, param_id: str) -> float:
        """A parameter as it is now: its automation's value while that plays."""
        if self.automation_state(param_id) == "on":
            value = self.bridge.current_value(self.track_id, device_key(self.device_id, param_id))
            if value is not None:
                return value
        return self.device().params.get(param_id, self.defaults.get(param_id, 0.0))

    def set_param(self, param_id: str, value: float, gesture: object | None = None) -> None:
        self.editor.set_device_param(self.track_id, self.device_id, param_id, float(value), gesture)
        self._sync()  # (a click that changes nothing still leaves the buttons right)

    def set_params(self, values: dict[str, float], gesture: object | None = None) -> None:
        """Several at once: one undo step (one per gesture)."""
        self.editor.set_device_params(self.track_id, self.device_id, values, gesture)
        self._sync()

    def touch(self, param_id: str) -> None:
        self.editor.touch_parameter(self.track_id, device_key(self.device_id, param_id))

    def _bind(self, widget: QWidget, param_id: str, update: Callable[[float, str | None], None]) -> None:
        """`update(value, automation state)` shows the parameter; right-click gives its automation."""
        self._views.setdefault(param_id, []).append(update)
        widget.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        widget.customContextMenuRequested.connect(
            lambda pos, w=widget: self._automation_menu(param_id, w.mapToGlobal(pos)))
        self._watch_touch(widget, param_id)

    def _sync(self) -> None:
        for param_id, updates in self._views.items():
            value, state = self.value(param_id), self.automation_state(param_id)
            for update in updates:
                update(value, state)
        linked = self.value("link") >= 0.5
        for widget in self._right:
            widget.setEnabled(not linked)
        for side in ("l", "r"):
            self._times[side].setCurrentIndex(0 if self.value(f"{side}_sync") >= 0.5 else 1)
        if hasattr(self, "graph"):
            self.graph.update()

    def refresh(self, device) -> None:
        super().refresh(device)
        self._sync()

    def follows_automation(self) -> bool:
        return any(self.automation_state(info.id) == "on" for info in self.infos)

    def refresh_automation(self) -> None:
        super().refresh_automation()
        if self._views:
            self._sync()

    # --- Controls --------------------------------------------------------------------

    def _toggle(self, button: ToggleButton, param_id: str) -> None:
        button.clicked.connect(lambda checked: self.set_param(param_id, 1.0 if checked else 0.0))
        self._bind(button, param_id, lambda v, _s: button.set_checked_silently(v >= 0.5))

    def _choice(self, button: ToggleButton, param_id: str, index: int) -> None:
        button.clicked.connect(lambda _checked: self.set_param(param_id, float(index)))
        self._bind(button, param_id, lambda v, _s: button.set_checked_silently(round(v) == index))

    def _box(self, box: ValueBox, param_id: str) -> ValueBox:
        box.setFixedHeight(BOX_HEIGHT)
        box.setFont(theme.ui_font(8))
        box.valueChanged.connect(lambda v, gesture: self.set_param(param_id, v, gesture))

        def update(value: float, state: str | None) -> None:
            box.setValue(value)
            box.set_automation(state)

        self._bind(box, param_id, update)
        return box

    def _knob(self, param_id: str, unit: str, size: int = KNOB_SIZE) -> tuple[Knob, QLabel]:
        info = next(i for i in self.infos if i.id == param_id)
        knob = Knob(info.min_value, info.max_value, self.value(param_id), default=info.default_value,
                    log_scale=info.log_scale, formatter=lambda v: format_value(v, unit))
        knob.setFixedSize(size, size)
        readout = _label("")
        readout.setStyleSheet("font-size: 8pt;")
        knob.valueChanged.connect(lambda v, gesture: self.set_param(param_id, v, gesture))

        def update(value: float, state: str | None) -> None:
            knob.setValue(value)
            knob.set_automation(state)
            readout.setText(format_value(value, unit))

        self._bind(knob, param_id, update)
        return knob, readout

    def _side(self, side: str, title: str) -> QWidget:
        """One side's time: Sync, then the sixteenths and offset (or, unsynced, the time)."""
        column = QWidget()
        column.setFixedWidth(SIDE_WIDTH)
        layout = QVBoxLayout(column)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(2)
        name = _label(title)
        layout.addWidget(name)
        sync = _button("Sync", "Sync the time to the tempo, in sixteenths")
        self._toggle(sync, f"{side}_sync")
        layout.addWidget(sync)

        times = QStackedWidget()
        synced = QWidget()
        grid = QGridLayout(synced)
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setSpacing(2)
        buttons = []
        for index, text in enumerate(DIVISIONS):
            button = _button(text, f"{text} sixteenth{'s' if text != '1' else ''}")
            self._choice(button, f"{side}_division", index)
            grid.addWidget(button, index // 2, index % 2)
            buttons.append(button)
        offset = self._box(ValueBox(0.0, -33.0, 33.0, step=0.2, decimals=1, default=0.0,
                                    formatter=lambda v: f"{v:.1f} %", sample_text="-33.0 %"), f"{side}_offset")
        offset.setToolTip("Offset: lengthens or shortens the synced time (swing)")
        grid.addWidget(offset, 4, 0, 1, 2)
        times.addWidget(synced)

        free = QWidget()
        free_layout = QVBoxLayout(free)
        free_layout.setContentsMargins(0, 4, 0, 0)
        free_layout.setSpacing(1)
        knob, readout = self._knob(f"{side}_time", "ms")
        free_layout.addWidget(knob, 0, Qt.AlignmentFlag.AlignHCenter)
        free_layout.addWidget(readout)
        free_layout.addStretch(1)
        times.addWidget(free)
        layout.addWidget(times, 1)

        self._times[side] = times
        if side == "r":
            self._right = [name, sync, *buttons, offset, knob, readout]
        return column

    def _filter_section(self) -> QWidget:
        section = QWidget()
        section.setFixedWidth(GRAPH_WIDTH)
        layout = QVBoxLayout(section)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(3)
        self.graph = FilterGraph(self)
        layout.addWidget(self.graph, 1)

        row = QHBoxLayout()
        row.setSpacing(3)
        filter_on = _button("Filter", "The band-pass filter on the echoes")
        filter_on.setFixedWidth(40)
        self._toggle(filter_on, "filter")
        row.addWidget(filter_on)
        freq = self._box(LogValueBox(1000.0, 50.0, 18000.0, decimals=0, default=1000.0,
                                     formatter=lambda v: format_value(v, "Hz"), parser=_parse_frequency,
                                     sample_text="18.00 kHz"), "freq")
        freq.setToolTip("Filter frequency")
        row.addWidget(freq, 1)
        row.addWidget(_label("Width"))
        width = self._box(ValueBox(8.0, WIDTH_MIN, WIDTH_MAX, step=0.05, decimals=2, default=8.0,
                                   sample_text="8.00"), "width")
        width.setToolTip("Filter width, in octaves")
        row.addWidget(width, 1)
        layout.addLayout(row)

        return section

    def _mode_section(self) -> QWidget:
        column = QWidget()
        column.setFixedWidth(MODE_WIDTH)
        layout = QVBoxLayout(column)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(2)
        layout.addWidget(_label("Mode"))
        tips = ("Repitch: time changes glide, pitching the echoes as a tape delay does",
                "Fade: time changes crossfade", "Jump: time changes switch at once")
        for index, (text, tip) in enumerate(zip(MODES, tips)):
            button = _button(text, tip)
            self._choice(button, "mode", index)
            layout.addWidget(button)
        layout.addSpacing(10)
        ping_pong = _button("Ping Pong", "The echoes bounce from left to right")
        self._toggle(ping_pong, "ping_pong")
        layout.addWidget(ping_pong)
        layout.addStretch(1)
        return column

    def _right_section(self) -> QWidget:
        """The feedback, with its freeze beside it, over the dry/wet mix."""
        column = QWidget()
        column.setFixedWidth(RIGHT_WIDTH)
        layout = QVBoxLayout(column)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(1)
        layout.addWidget(_label("Feedback"))
        knob, readout = self._knob("feedback", "%", SMALL_KNOB)
        freeze = _button(tooltip="Freeze: what is in the delay goes round for ever, new input is ignored",
                         icon=icons.infinity())
        freeze.setFixedWidth(22)
        self._toggle(freeze, "freeze")
        row = QHBoxLayout()
        row.setSpacing(2)
        row.addWidget(knob)
        row.addWidget(freeze, 0, Qt.AlignmentFlag.AlignVCenter)
        layout.addLayout(row)
        layout.addWidget(readout)
        layout.addStretch(1)
        layout.addWidget(_label("Dry/Wet"))
        knob, readout = self._knob("mix", "%", SMALL_KNOB)
        layout.addWidget(knob, 0, Qt.AlignmentFlag.AlignHCenter)
        layout.addWidget(readout)
        return column

    def refresh_displays(self) -> None:
        self.graph.add_samples(self.read_display("input"), float(self.bridge.engine.sample_rate))
