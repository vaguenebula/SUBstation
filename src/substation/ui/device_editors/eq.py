"""The EQ's editor, after FabFilter's Pro-Q: one big curve to play with.

Hover the curve and a ghost band shows where a click adds one, and of what
type: a low cut at the far left, then a low shelf, bells across the middle, a
high shelf, and a high cut at the far right. Drag a band across for its
frequency and up and down for its gain (a cut, notch or band pass: its Q);
the wheel (or Ctrl-drag) sets its Q, Alt-wheel its slope (and so does the
plain wheel while a cut is being dragged), Shift makes any of them fine. Double-click a band to switch it off and on, Alt-click (or Delete)
to remove it, right-click for its type, slope and placement. Behind the curve
an analyzer shows the input and the output.

The panel beside it has the selected band's controls, and the output gain and
gain scale; the expand button opens the same editor, bigger, in a window.

The editor (EqEditor) works through a host: the device view's EqWidget, or
that window (EqWindow). Every change goes through ProjectEditor, so undo,
automation and saving work as for any control; a drag is one undo step."""

from __future__ import annotations

import math
from dataclasses import dataclass

import numpy as np
from PySide6.QtCore import QEvent, QObject, QPoint, QPointF, QRectF, QSize, Qt, QTimer
from PySide6.QtGui import (
    QColor,
    QFont,
    QKeyEvent,
    QLinearGradient,
    QMouseEvent,
    QPainter,
    QPainterPath,
    QPen,
    QWheelEvent,
)
from PySide6.QtWidgets import (
    QComboBox,
    QFrame,
    QHBoxLayout,
    QLabel,
    QMenu,
    QPushButton,
    QStackedWidget,
    QVBoxLayout,
    QWidget,
)

from ... import _engine as ge
from ... import theme
from ...model.automation import device_key
from ...model.params import format_value
from .. import icons
from ..arrangement.track_headers import automation_state
from ..device_panel import DeviceWidget
from ..widgets import Knob, ToggleButton
from . import device_editor

BANDS = 24
TYPES = ("Bell", "Low Shelf", "Low Cut", "High Shelf", "High Cut", "Notch", "Band Pass", "Tilt Shelf")
BELL, LOW_SHELF, LOW_CUT, HIGH_SHELF, HIGH_CUT, NOTCH, BAND_PASS, TILT_SHELF = range(len(TYPES))
GAIN_TYPES = frozenset({BELL, LOW_SHELF, HIGH_SHELF, TILT_SHELF})
SLOPE_TYPES = frozenset({LOW_SHELF, HIGH_SHELF, LOW_CUT, HIGH_CUT, TILT_SHELF})
SLOPES = (6, 12, 18, 24, 30, 36, 48, 72, 96)  # dB/octave, as the engine lists them
PLACES = ("Stereo", "Left", "Right", "Mid", "Side")
PLACE_LETTERS = ("", "L", "R", "M", "S")
FREQ_MIN, FREQ_MAX = 10.0, 22000.0
GAIN_MAX = 30.0
Q_MIN, Q_MAX = 0.025, 40.0
RANGES = (3.0, 6.0, 12.0, 30.0)  # the curve's scale: ± this many dB
ANALYZER_MODES = ("Analyzer Off", "Pre", "Post", "Pre + Post")

# Where a click on the curve adds which type: up to this fraction of the way across.
ZONES = ((0.09, LOW_CUT), (0.24, LOW_SHELF), (0.76, BELL), (0.91, HIGH_SHELF), (2.0, HIGH_CUT))
DEFAULT_Q = {BELL: 1.0, NOTCH: 4.0, BAND_PASS: 1.0}  # others: Butterworth's
DEFAULT_SLOPE = {LOW_CUT: 3, HIGH_CUT: 3}  # 24 dB/octave; shelves 12

GRAPH_WIDTH = 580
PANEL_WIDTH = 162
SPACING = 8
PANEL_INSET = 6  # px between the panel and the device's right edge (the graph has no margins)
DOT_RADIUS, DOT_HOVER_RADIUS = 6.5, 8.5
HIT_RADIUS = 11.0
CURVE_HIT = 12.0  # px from the curve where the ghost band shows
EQ_MARGIN = 10.0  # px between ± the range and the graph's top and bottom

FFT_SIZE = 8192
SPECTRUM_FLOOR, SPECTRUM_CEIL = -96.0, 6.0  # dBFS (after the tilt) at the bottom and top
SPECTRUM_TILT = 4.5  # dB/octave around 1 kHz, as Pro-Q's: music looks about level
SPECTRUM_RISE, SPECTRUM_FALL = 0.55, 0.09  # per display refresh, towards the latest
TILT_FADE = 18.0  # dB above the floor over which the tilt comes in (the floor itself stays flat)

# What every editor shows (not saved): the curve's range, the analyzer's mode, and whether the
# device view shows the selected band's controls beside the curve.
VIEW = {"range": 12.0, "analyzer": 3, "panel": False}

CURVE_COLOR = QColor("#ffd68a")
BACKGROUND_TOP, BACKGROUND_BOTTOM = QColor("#1d2027"), QColor("#101216")
GRID_MAJOR, GRID_MINOR = QColor(255, 255, 255, 30), QColor(255, 255, 255, 11)
LABEL_COLOR = QColor(255, 255, 255, 90)
POST_COLOR = QColor(110, 160, 235)
PRE_COLOR = QColor(200, 210, 230)


def param(band: int, name: str) -> str:
    """A band's parameter id (bands from 0)."""
    return f"b{band + 1}_{name}"


def band_color(band: int) -> QColor:
    """Each band its own colour, round the colour wheel."""
    return QColor.fromHsvF((0.02 + band * 0.137) % 1.0, 0.58, 1.0)


def type_at(fraction: float) -> int:
    """The type a click adds this fraction of the way across the graph."""
    return next(kind for limit, kind in ZONES if fraction < limit)


def band_response(kind: int, freq: float, gain: float, q: float, slope: int, sample_rate: float,
                  freqs: np.ndarray) -> np.ndarray:
    """A band's response in dB at `freqs`: the engine's own (see EqDesign.h)."""
    return np.asarray(ge.eq_response(int(kind), float(freq), float(gain), float(q), int(slope), float(sample_rate),
                                     np.ascontiguousarray(freqs, np.float64)))


def format_freq(freq: float) -> str:
    return f"{freq / 1000:.2f} kHz" if freq >= 1000 else f"{freq:.1f} Hz" if freq < 100 else f"{freq:.0f} Hz"


@dataclass(frozen=True)
class Band:
    index: int
    on: bool
    type: int
    freq: float
    gain: float  # as set (before the gain scale)
    q: float
    slope: int
    place: int

    @property
    def color(self) -> QColor:
        return band_color(self.index)


class Analyzer:
    """Spectra of the input and the output: a Hann-windowed FFT of the latest
    samples, rising fast and falling back slowly, as Pro-Q's does."""

    def __init__(self):
        self.sample_rate = 48000.0
        self._window = np.hanning(FFT_SIZE).astype(np.float32)
        self._scale = 2.0 / float(self._window.sum())
        self._samples = {name: np.zeros(FFT_SIZE, np.float32) for name in ("input", "output")}
        self.levels = {name: np.full(FFT_SIZE // 2 + 1, SPECTRUM_FLOOR, np.float32) for name in self._samples}
        self._columns: tuple | None = None  # (key, edges...) for columns()

    def feed(self, name: str, samples: np.ndarray, sample_rate: float) -> None:
        self.sample_rate = sample_rate
        levels = self.levels[name]
        if len(samples):
            samples = np.nan_to_num(np.asarray(samples[-FFT_SIZE:], np.float32))
            buffer = self._samples[name]
            self._samples[name] = buffer = np.concatenate((buffer[len(samples):], samples))
            magnitude = np.abs(np.fft.rfft(buffer * self._window)) * self._scale
            latest = 20.0 * np.log10(np.maximum(magnitude, 1e-7))
        else:
            latest = np.full_like(levels, SPECTRUM_FLOOR)
        rate = np.where(latest > levels, SPECTRUM_RISE, SPECTRUM_FALL)
        self.levels[name] = np.maximum(levels + (latest - levels) * rate, SPECTRUM_FLOOR).astype(np.float32)

    def live(self, name: str) -> bool:
        return bool(self.levels[name].max() > SPECTRUM_FLOOR + 1.0)

    def columns(self, name: str, freqs: np.ndarray) -> np.ndarray:
        """The spectrum in dB (tilted) at each of the graph's column frequencies: the loudest bin
        between a column and the next, or (low down, where a bin spans columns) between bins."""
        key = (len(freqs), float(freqs[0]), float(freqs[-1]), self.sample_rate)
        if self._columns is None or self._columns[0] != key:
            bins = freqs / (self.sample_rate / FFT_SIZE)
            edges = np.sqrt(bins[:-1] * bins[1:])
            edges = np.concatenate(([bins[0] ** 2 / edges[0]], edges, [bins[-1] ** 2 / edges[-1]]))
            last_bin = FFT_SIZE // 2
            first = np.clip(np.ceil(edges[:-1]).astype(np.int64), 0, last_bin)
            last = np.clip(np.floor(edges[1:]).astype(np.int64), 0, last_bin)
            tilt = SPECTRUM_TILT * np.log2(np.maximum(freqs, 1.0) / 1000.0)
            self._columns = (key, bins, first, last >= first, tilt)
        _key, bins, first, has_bins, tilt = self._columns
        levels = self.levels[name]
        values = np.interp(bins, np.arange(len(levels)), levels)
        peaks = np.maximum.reduceat(levels, first) if len(first) else values
        values = np.where(has_bins, np.maximum(values, peaks), values)
        values = np.convolve(np.pad(values, 1, mode="edge"), (0.25, 0.5, 0.25), mode="valid")
        # Tilted, but not near the floor: silence (or a spectrum falling back to it) stays flat
        # instead of the tilt lifting its high end into view.
        return values + tilt * np.clip((values - SPECTRUM_FLOOR) / TILT_FADE, 0.0, 1.0)


class EqGraph(QWidget):
    """The curve, its bands' dots and the analyzer behind them."""

    def __init__(self, view: EqEditor):
        super().__init__()
        self.view = view
        self.hover: int | None = None  # the band under the mouse
        self.ghost: tuple[QPointF, int] | None = None  # where a click adds a band, and its type
        self._drag: dict | None = None
        self._added_on_press = False
        self._wheel_gesture: object | None = None
        self._wheel_timer = QTimer(self)
        self._wheel_timer.setSingleShot(True)
        self._wheel_timer.setInterval(400)
        self._wheel_timer.timeout.connect(lambda: setattr(self, "_wheel_gesture", None))
        self._radius: dict[int, float] = {}  # each dot's radius, easing to its target
        self._ghost_alpha = 0.0
        self._animation = QTimer(self)
        self._animation.setInterval(16)
        self._animation.timeout.connect(self._animate)
        self._responses: dict[int, tuple[tuple, np.ndarray]] = {}  # band: (its key, its response per column)
        self._overlays: dict[str, QRectF] = {}  # the clickable texts' places
        self.right_inset = 0.0  # px kept free at the top right (the expand button)
        self.setMouseTracking(True)
        self.setMinimumSize(200, 60)
        self.setFocusPolicy(Qt.FocusPolicy.ClickFocus)

    # --- Geometry ----------------------------------------------------------------------

    def _plot(self) -> QRectF:
        return QRectF(self.rect()).adjusted(1, 1, -1, -1)

    def resizeEvent(self, event) -> None:
        super().resizeEvent(event)
        if hasattr(self.view, "corners"):
            self.view.place_overlays()

    def _x(self, freq: float) -> float:
        plot = self._plot()
        return plot.left() + math.log(freq / FREQ_MIN) / math.log(FREQ_MAX / FREQ_MIN) * plot.width()

    def _freq(self, x: float) -> float:
        plot = self._plot()
        fraction = min(1.0, max(0.0, (x - plot.left()) / plot.width()))
        return FREQ_MIN * (FREQ_MAX / FREQ_MIN) ** fraction

    def _half(self) -> float:
        """px from 0 dB to ± the range."""
        return max(10.0, self._plot().height() / 2 - EQ_MARGIN)

    def _y(self, db: float) -> float:
        return self._plot().center().y() - db / VIEW["range"] * self._half()

    def _db(self, y: float) -> float:
        return (self._plot().center().y() - y) / self._half() * VIEW["range"]

    def _spectrum_y(self, db: np.ndarray) -> np.ndarray:
        plot = self._plot()
        fraction = (db - SPECTRUM_FLOOR) / (SPECTRUM_CEIL - SPECTRUM_FLOOR)
        return plot.bottom() - np.clip(fraction, 0.0, 1.05) * plot.height()

    def column_freqs(self) -> np.ndarray:
        plot = self._plot()
        count = max(2, int(plot.width() / 2) + 1)
        return FREQ_MIN * (FREQ_MAX / FREQ_MIN) ** np.linspace(0.0, 1.0, count)

    # --- Responses -----------------------------------------------------------------------

    def response(self, band: Band, freqs: np.ndarray) -> np.ndarray:
        """A band's response at the columns (as it plays: its gain scaled)."""
        gain = band.gain * self.view.scale
        key = (band.type, band.freq, gain, band.q, band.slope, self.view.host.sample_rate, len(freqs), freqs[-1])
        cached = self._responses.get(band.index)
        if cached is None or cached[0] != key:
            cached = (key, band_response(band.type, band.freq, gain, band.q, band.slope,
                                         self.view.host.sample_rate, freqs))
            self._responses[band.index] = cached
        return cached[1]

    def total(self, freqs: np.ndarray) -> np.ndarray:
        """Every band that is on, together."""
        total = np.zeros(len(freqs))
        for band in self.view.bands:
            if band is not None and band.on:
                total += self.response(band, freqs)
        return total

    def dot(self, band: Band) -> QPointF:
        """Where a band's dot is: at its gain, or (no gain) on its own curve at its frequency."""
        if band.type in GAIN_TYPES:
            db = band.gain * self.view.scale
        else:
            db = float(band_response(band.type, band.freq, 0.0, band.q, band.slope, self.view.host.sample_rate,
                                     np.array([band.freq]))[0])
        limit = VIEW["range"] + EQ_MARGIN * 0.6 * VIEW["range"] / self._half()
        return QPointF(self._x(band.freq), self._y(max(-limit, min(limit, db))))

    def band_at(self, pos: QPointF) -> int | None:
        """The band whose dot is under `pos` (the selected one first, then the last drawn)."""
        found, nearest = None, HIT_RADIUS
        for band in self.view.bands:
            if band is None:
                continue
            distance = math.dist((pos.x(), pos.y()), (self.dot(band).x(), self.dot(band).y()))
            if distance < nearest or (band.index == self.view.selected and distance < HIT_RADIUS):
                found, nearest = band.index, distance
                if band.index == self.view.selected:
                    break
        return found

    def _curve_y(self, x: float) -> float:
        freq = self._freq(x)
        total = 0.0
        for band in self.view.bands:
            if band is not None and band.on:
                gain = band.gain * self.view.scale
                total += float(band_response(band.type, band.freq, gain, band.q, band.slope,
                                             self.view.host.sample_rate, np.array([freq]))[0])
        return self._y(max(-VIEW["range"] * 1.2, min(VIEW["range"] * 1.2, total)))

    # --- Mouse -----------------------------------------------------------------------------

    def _overlay_at(self, pos: QPointF) -> str | None:
        return next((name for name, rect in self._overlays.items() if rect.contains(pos)), None)

    def mousePressEvent(self, event: QMouseEvent) -> None:
        pos = event.position()
        self._added_on_press = False
        if event.button() == Qt.MouseButton.RightButton:
            band = self.band_at(pos)
            if band is not None:
                self.view.select(band)
                self._band_menu(band, event.globalPosition().toPoint())
            else:
                self._view_menu(event.globalPosition().toPoint())
            return
        if event.button() != Qt.MouseButton.LeftButton:
            return
        overlay = self._overlay_at(pos)
        if overlay == "range":
            VIEW["range"] = RANGES[(RANGES.index(VIEW["range"]) + 1) % len(RANGES)]
            self.view.refresh_views()
            return
        if overlay == "analyzer":
            VIEW["analyzer"] = (VIEW["analyzer"] + 1) % len(ANALYZER_MODES)
            self.view.refresh_views()
            return
        band = self.band_at(pos)
        modifiers = event.modifiers()
        if band is not None:
            if modifiers & Qt.KeyboardModifier.AltModifier:
                self.view.remove(band)
                return
            self.view.select(band)
            self._start_drag(band, pos)
            return
        if self.ghost is not None:
            self._add(pos, self.ghost[1])
            return
        self.view.select(None)

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton or self._added_on_press or self._overlay_at(event.position()):
            return
        band = self.band_at(event.position())
        if band is not None:
            current = self.view.band(band)
            self.view.set_params({param(band, "on"): 0.0 if current.on else 1.0}, text="Switch EQ Band")
        else:  # anywhere, not only on the curve
            plot = self._plot()
            self._add(event.position(), type_at((event.position().x() - plot.left()) / plot.width()))

    def _add(self, pos: QPointF, kind: int) -> None:
        index = self.view.free_band()
        if index is None:
            return
        gain = max(-GAIN_MAX, min(GAIN_MAX, self._db(pos.y()) / max(self.view.scale, 0.01)))
        values = {
            param(index, "used"): 1.0, param(index, "on"): 1.0, param(index, "type"): float(kind),
            param(index, "freq"): round(self._freq(pos.x()), 2),
            param(index, "gain"): round(gain, 2) if kind in GAIN_TYPES else 0.0,
            param(index, "q"): DEFAULT_Q.get(kind, 0.71),
            param(index, "slope"): float(DEFAULT_SLOPE.get(kind, 1)), param(index, "place"): 0.0,
        }
        gesture = object()
        self.view.set_params(values, gesture, "Add EQ Band")
        self.view.select(index)
        self._added_on_press = True
        self.ghost = None
        self._start_drag(index, pos, gesture, values)

    def _start_drag(self, index: int, pos: QPointF, gesture: object | None = None, base: dict | None = None) -> None:
        band = self.view.band(index)
        self.view.host.touch(param(index, "freq"))
        self._drag = {"band": index, "origin": pos, "freq": band.freq, "gain": band.gain, "q": band.q,
                      "gesture": gesture or object(), "base": base or {}}
        self.setCursor(Qt.CursorShape.ClosedHandCursor)
        self._animate_soon()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        pos = event.position()
        if self._drag is not None:
            self._drag_to(pos, event.modifiers())
            return
        hover = self.band_at(pos)
        ghost = None
        if hover is None and self._overlay_at(pos) is None and self.view.free_band() is not None:
            curve_y = self._curve_y(pos.x())
            if abs(curve_y - pos.y()) <= CURVE_HIT:
                plot = self._plot()
                ghost = (QPointF(pos.x(), curve_y), type_at((pos.x() - plot.left()) / plot.width()))
        if hover != self.hover or ghost != self.ghost:
            self.hover, self.ghost = hover, ghost
            if hover is not None:
                self.setCursor(Qt.CursorShape.OpenHandCursor)
            elif ghost is not None:
                self.setCursor(Qt.CursorShape.CrossCursor)
            elif self._overlay_at(pos) is not None:
                self.setCursor(Qt.CursorShape.PointingHandCursor)
            else:
                self.unsetCursor()
            self._animate_soon()
            self.update()

    def _drag_to(self, pos: QPointF, modifiers) -> None:
        drag = self._drag
        index = drag["band"]
        band = self.view.band(index)
        if band is None:
            return
        fine = 0.15 if modifiers & Qt.KeyboardModifier.ShiftModifier else 1.0
        dx = (pos.x() - drag["origin"].x()) * fine
        dy = (pos.y() - drag["origin"].y()) * fine
        plot = self._plot()
        values = dict(drag["base"])
        freq = drag["freq"] * (FREQ_MAX / FREQ_MIN) ** (dx / plot.width())
        values[param(index, "freq")] = round(max(FREQ_MIN, min(FREQ_MAX, freq)), 2)
        if modifiers & Qt.KeyboardModifier.ControlModifier or band.type not in GAIN_TYPES:
            q = drag["q"] * 2.0 ** (-dy / 45.0)
            values[param(index, "q")] = round(max(Q_MIN, min(Q_MAX, q)), 3)
        else:
            gain = drag["gain"] - dy / self._half() * VIEW["range"] / max(self.view.scale, 0.01)
            values[param(index, "gain")] = round(max(-GAIN_MAX, min(GAIN_MAX, gain)), 2)
        self.view.set_params(values, drag["gesture"], "Add EQ Band" if drag["base"] else "Move EQ Band")

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        if self._drag is not None:
            self._drag = None
            self.mouseMoveEvent(event)
            self.update()

    def leaveEvent(self, _event) -> None:
        if self._drag is None:
            self.hover, self.ghost = None, None
            self._animate_soon()
            self.update()

    def wheelEvent(self, event: QWheelEvent) -> None:
        index = self._drag["band"] if self._drag is not None else self.band_at(event.position())
        if index is None:
            index = self.view.selected
        band = self.view.band(index) if index is not None else None
        delta = event.angleDelta().y() or event.angleDelta().x()  # (Alt turns the wheel sideways)
        if band is None or not delta:
            event.ignore()
            return
        event.accept()
        if self._wheel_gesture is None:
            self._wheel_gesture = object()
        self._wheel_timer.start()
        notches = delta / 120.0
        modifiers = event.modifiers()
        dragging_cut = self._drag is not None and band.type in (LOW_CUT, HIGH_CUT)
        if dragging_cut or modifiers & Qt.KeyboardModifier.AltModifier:
            if band.type in SLOPE_TYPES:
                slope = float(max(0, min(len(SLOPES) - 1, band.slope + (1 if notches > 0 else -1))))
                self._wheel_set(index, {param(index, "slope"): slope}, None, "Change EQ Band Slope")
            return
        step = 1.03 if modifiers & Qt.KeyboardModifier.ShiftModifier else 1.15
        q = round(max(Q_MIN, min(Q_MAX, band.q * step ** notches)), 3)
        drag = self._drag
        if drag is not None and drag["band"] == index:
            # The drag goes on from the new Q: where the mouse is now gives it (see _drag_to).
            fine = 0.15 if modifiers & Qt.KeyboardModifier.ShiftModifier else 1.0
            dy = (event.position().y() - drag["origin"].y()) * fine
            sets_q = modifiers & Qt.KeyboardModifier.ControlModifier or band.type not in GAIN_TYPES
            drag["q"] = q * 2.0 ** (dy / 45.0) if sets_q else q
        self._wheel_set(index, {param(index, "q"): q}, self._wheel_gesture, "Change EQ Band Q")

    def _wheel_set(self, index: int, changes: dict[str, float], gesture: object | None, text: str) -> None:
        """Sets what the wheel changed. During the drag that added the band, every move sets all its
        parameters (one undo step), so they take the change too, or the next move would undo it."""
        drag = self._drag
        if drag is not None and drag["band"] == index and drag["base"]:
            band = self.view.band(index)
            drag["base"].update(changes)
            values = {**drag["base"], param(index, "freq"): band.freq, param(index, "gain"): band.gain, **changes}
            self.view.set_params(values, drag["gesture"], "Add EQ Band")
        else:
            self.view.set_params(changes, gesture, text)

    def event(self, event) -> bool:
        # Delete removes the selected band, not what the app's Delete would (the selected devices).
        if event.type() == QEvent.Type.ShortcutOverride and self.view.selected is not None \
                and event.key() in (Qt.Key.Key_Delete, Qt.Key.Key_Backspace):
            event.accept()
            return True
        return super().event(event)

    def keyPressEvent(self, event: QKeyEvent) -> None:
        if event.key() in (Qt.Key.Key_Delete, Qt.Key.Key_Backspace) and self.view.selected is not None:
            self.view.remove(self.view.selected)
            event.accept()
            return
        super().keyPressEvent(event)

    def _band_menu(self, index: int, at: QPoint) -> None:
        band = self.view.band(index)
        menu = QMenu(self)
        types = menu.addMenu("Type")
        for kind, name in enumerate(TYPES):
            action = types.addAction(name, lambda k=kind: self.view.set_type(index, k))
            action.setCheckable(True)
            action.setChecked(kind == band.type)
        if band.type in SLOPE_TYPES:
            slopes = menu.addMenu("Slope")
            for i, slope in enumerate(SLOPES):
                action = slopes.addAction(f"{slope} dB/oct", lambda i=i: self.view.set_params(
                    {param(index, "slope"): float(i)}, text="Change EQ Band Slope"))
                action.setCheckable(True)
                action.setChecked(i == band.slope)
        places = menu.addMenu("Placement")
        for i, name in enumerate(PLACES):
            action = places.addAction(name, lambda i=i: self.view.set_params(
                {param(index, "place"): float(i)}, text="Change EQ Band Placement"))
            action.setCheckable(True)
            action.setChecked(i == band.place)
        menu.addSeparator()
        enabled = menu.addAction("Enabled", lambda: self.view.set_params(
            {param(index, "on"): 0.0 if band.on else 1.0}, text="Switch EQ Band"))
        enabled.setCheckable(True)
        enabled.setChecked(band.on)
        menu.addAction("Delete Band", lambda: self.view.remove(index))
        menu.exec(at)

    def _view_menu(self, at: QPoint) -> None:
        menu = QMenu(self)
        for i, name in enumerate(ANALYZER_MODES):
            action = menu.addAction(name, lambda i=i: (VIEW.__setitem__("analyzer", i), self.view.refresh_views()))
            action.setCheckable(True)
            action.setChecked(i == VIEW["analyzer"])
        menu.addSeparator()
        for value in RANGES:
            action = menu.addAction(f"Range ± {value:g} dB", lambda v=value: (VIEW.__setitem__("range", v),
                                                                           self.view.refresh_views()))
            action.setCheckable(True)
            action.setChecked(value == VIEW["range"])
        if any(self.view.bands):
            menu.addSeparator()
            menu.addAction("Delete All Bands", self.view.remove_all)
        menu.exec(at)

    # --- Animation -------------------------------------------------------------------------

    def _target_radius(self, index: int) -> float:
        active = index == self.hover or (self._drag is not None and self._drag["band"] == index)
        return DOT_HOVER_RADIUS if active else DOT_RADIUS

    def _animate_soon(self) -> None:
        if not self._animation.isActive():
            self._animation.start()

    def _animate(self) -> None:
        settled = True
        for band in self.view.bands:
            if band is None:
                continue
            current = self._radius.get(band.index, 0.0)
            target = self._target_radius(band.index)
            current += (target - current) * 0.35
            if abs(target - current) < 0.05:
                current = target
            else:
                settled = False
            self._radius[band.index] = current
        target = 1.0 if self.ghost is not None else 0.0
        self._ghost_alpha += (target - self._ghost_alpha) * 0.3
        if abs(target - self._ghost_alpha) < 0.02:
            self._ghost_alpha = target
        else:
            settled = False
        self.update()
        if settled:
            self._animation.stop()

    # --- Painting ----------------------------------------------------------------------------

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        plot = self._plot()
        gradient = QLinearGradient(plot.topLeft(), plot.bottomLeft())
        gradient.setColorAt(0.0, BACKGROUND_TOP)
        gradient.setColorAt(1.0, BACKGROUND_BOTTOM)
        p.fillRect(self.rect(), gradient)
        p.setClipRect(plot)
        small = QFont(theme.ui_font(7))
        p.setFont(small)
        self._paint_grid(p, plot)
        freqs = self.column_freqs()
        xs = np.linspace(plot.left(), plot.right(), len(freqs))
        self._paint_analyzer(p, plot, freqs, xs)
        self._paint_bands(p, plot, freqs, xs)
        self._paint_ghost(p)
        self._paint_dots(p)
        self._paint_badge(p, plot)
        self._paint_overlays(p, plot)
        p.end()

    def _paint_grid(self, p: QPainter, plot: QRectF) -> None:
        for decade in (10.0, 100.0, 1000.0, 10000.0):
            for multiple in range(1, 10):
                freq = decade * multiple
                if not FREQ_MIN < freq < FREQ_MAX:
                    continue
                x = self._x(freq)
                major = multiple in (1, 2, 5)
                p.setPen(QPen(GRID_MAJOR if multiple == 1 else GRID_MINOR if not major else QColor(255, 255, 255, 18),
                              1))
                p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()))
                if major and freq >= 20:
                    p.setPen(LABEL_COLOR)
                    label = f"{freq / 1000:g}k" if freq >= 1000 else f"{freq:g}"
                    p.drawText(QRectF(x + 3, plot.bottom() - 13, 40, 12), Qt.AlignmentFlag.AlignLeft, label)
        step = VIEW["range"] / (2 if VIEW["range"] <= 3 else 4 if VIEW["range"] <= 12 else 3)
        db = -VIEW["range"]
        while db <= VIEW["range"] + 1e-6:
            y = self._y(db)
            p.setPen(QPen(QColor(255, 255, 255, 46) if abs(db) < 1e-6 else GRID_MINOR, 1))
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y))
            if abs(db) > 1e-6 and plot.top() + 8 < y < plot.bottom() - 16:
                p.setPen(LABEL_COLOR)
                p.drawText(QRectF(plot.right() - 34, y - 6, 31, 12), Qt.AlignmentFlag.AlignRight
                           | Qt.AlignmentFlag.AlignVCenter, f"{db:+g}")
            db += step

    def _paint_analyzer(self, p: QPainter, plot: QRectF, freqs: np.ndarray, xs: np.ndarray) -> None:
        mode = VIEW["analyzer"]
        analyzer = self.view.analyzer
        if mode in (2, 3) and analyzer.live("output"):  # post: filled
            ys = self._spectrum_y(analyzer.columns("output", freqs))
            shape = QPainterPath(QPointF(xs[0], plot.bottom()))
            for x, y in zip(xs, ys):
                shape.lineTo(QPointF(x, y))
            shape.lineTo(QPointF(xs[-1], plot.bottom()))
            shape.closeSubpath()
            fill = QLinearGradient(plot.topLeft(), plot.bottomLeft())
            fill.setColorAt(0.0, _alpha(POST_COLOR, 120))
            fill.setColorAt(1.0, _alpha(POST_COLOR, 18))
            p.fillPath(shape, fill)
            p.strokePath(shape, QPen(_alpha(POST_COLOR.lighter(130), 120), 1.0))
        if mode in (1, 3) and analyzer.live("input"):  # pre: a line
            ys = self._spectrum_y(analyzer.columns("input", freqs))
            line = QPainterPath(QPointF(xs[0], ys[0]))
            for x, y in zip(xs[1:], ys[1:]):
                line.lineTo(QPointF(x, y))
            p.setPen(QPen(_alpha(PRE_COLOR, 70 if mode == 3 else 110), 1.0))
            p.setBrush(Qt.BrushStyle.NoBrush)
            p.drawPath(line)

    def _curve_path(self, xs: np.ndarray, db: np.ndarray) -> QPainterPath:
        limit = VIEW["range"] * 3 + 20
        ys = [self._y(max(-limit, min(limit, float(v)))) for v in db]
        path = QPainterPath(QPointF(xs[0], ys[0]))
        for x, y in zip(xs[1:], ys[1:]):
            path.lineTo(QPointF(x, y))
        return path

    def _paint_bands(self, p: QPainter, plot: QRectF, freqs: np.ndarray, xs: np.ndarray) -> None:
        zero = self._y(0.0)
        focus = {self.view.selected, self.hover} - {None}
        for band in self.view.bands:
            if band is None or not band.on:
                continue
            curve = self._curve_path(xs, self.response(band, freqs))
            color = band.color
            if band.index in focus:
                area = QPainterPath(curve)
                area.lineTo(QPointF(xs[-1], zero))
                area.lineTo(QPointF(xs[0], zero))
                area.closeSubpath()
                fill = QLinearGradient(QPointF(0, self._y(VIEW["range"])), QPointF(0, self._y(-VIEW["range"])))
                strength = 1.0 if band.index == self.view.selected else 0.6
                fill.setColorAt(0.0, _alpha(color, int(95 * strength)))
                fill.setColorAt(0.5, _alpha(color, int(25 * strength)))
                fill.setColorAt(1.0, _alpha(color, int(95 * strength)))
                p.fillPath(area, fill)
                p.strokePath(curve, QPen(_alpha(color, int(120 * strength)), 1.0))
            else:
                p.strokePath(curve, QPen(_alpha(color, 55), 1.0))
        total = self._curve_path(xs, self.total(freqs))
        p.strokePath(total, QPen(_alpha(CURVE_COLOR, 38), 7.0, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap,
                                 Qt.PenJoinStyle.RoundJoin))
        p.strokePath(total, QPen(_alpha(CURVE_COLOR, 70), 3.5, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap,
                                 Qt.PenJoinStyle.RoundJoin))
        p.strokePath(total, QPen(CURVE_COLOR, 1.8, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap,
                                 Qt.PenJoinStyle.RoundJoin))
        if not any(self.view.bands):
            p.setPen(QColor(255, 255, 255, 70))
            p.setFont(theme.ui_font(8))
            p.drawText(plot.adjusted(0, 0, 0, -plot.height() * 0.45), Qt.AlignmentFlag.AlignCenter,
                       "Click the curve to add a band")

    def _paint_ghost(self, p: QPainter) -> None:
        if self._ghost_alpha <= 0.01:
            return
        if self.ghost is not None:
            self._last_ghost = self.ghost
        center, kind = getattr(self, "_last_ghost", (None, BELL))
        if center is None:
            return
        alpha = self._ghost_alpha
        color = band_color(self.view.free_band() or 0)
        p.setPen(QPen(_alpha(color, int(220 * alpha)), 1.5))
        p.setBrush(_alpha(color, int(45 * alpha)))
        p.drawEllipse(center, DOT_RADIUS + 1, DOT_RADIUS + 1)
        p.setPen(QPen(_alpha(QColor("white"), int(230 * alpha)), 1.4))
        p.drawLine(QPointF(center.x() - 3, center.y()), QPointF(center.x() + 3, center.y()))
        p.drawLine(QPointF(center.x(), center.y() - 3), QPointF(center.x(), center.y() + 3))
        label = TYPES[kind]
        p.setFont(theme.ui_font(7))
        width = p.fontMetrics().horizontalAdvance(label) + 10
        above = center.y() - 26 > self._plot().top()
        rect = QRectF(center.x() - width / 2, center.y() + (-24 if above else 12), width, 14)
        rect.moveLeft(max(self._plot().left() + 2, min(self._plot().right() - width - 2, rect.left())))
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(QColor(12, 13, 16, int(200 * alpha)))
        p.drawRoundedRect(rect, 7, 7)
        p.setPen(_alpha(color, int(255 * alpha)))
        p.drawText(rect, Qt.AlignmentFlag.AlignCenter, label)

    def _paint_dots(self, p: QPainter) -> None:
        order = [b for b in self.view.bands if b is not None]
        order.sort(key=lambda b: (b.index == self.view.selected, b.index == self.hover))  # the focus on top
        number_font = QFont(theme.ui_font(7, bold=True))
        for band in order:
            center = self.dot(band)
            radius = self._radius.get(band.index) or DOT_RADIUS
            color = band.color if band.on else QColor(110, 112, 118)
            if band.index == self.view.selected:
                p.setPen(QPen(QColor(255, 255, 255, 230), 1.6))
                p.setBrush(Qt.BrushStyle.NoBrush)
                p.drawEllipse(center, radius + 3.0, radius + 3.0)
            glow = QColor(color)
            glow.setAlpha(60 if band.index == self.hover else 35)
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(glow)
            p.drawEllipse(center, radius + 2.0, radius + 2.0)
            p.setBrush(color if band.on else QColor(40, 42, 48))
            p.setPen(QPen(QColor(10, 10, 12, 200) if band.on else color, 1.2))
            p.drawEllipse(center, radius, radius)
            p.setFont(number_font)
            p.setPen(QColor(15, 15, 18) if band.on else color)
            p.drawText(QRectF(center.x() - radius, center.y() - radius, 2 * radius, 2 * radius),
                       Qt.AlignmentFlag.AlignCenter, str(band.index + 1))
            letter = PLACE_LETTERS[band.place]
            if letter:
                p.setPen(_alpha(color, 230))
                p.drawText(QRectF(center.x() + radius + 1, center.y() - radius - 9, 12, 10),
                           Qt.AlignmentFlag.AlignLeft, letter)

    def _paint_badge(self, p: QPainter, plot: QRectF) -> None:
        index = self._drag["band"] if self._drag is not None else self.hover
        band = self.view.band(index) if index is not None else None
        if band is None:
            return
        parts = [TYPES[band.type], format_freq(band.freq)]
        if band.type in GAIN_TYPES:
            parts.append(f"{band.gain * self.view.scale:+.1f} dB")
        parts.append(f"Q {band.q:.2f}")
        if band.type in SLOPE_TYPES:
            parts.append(f"{SLOPES[band.slope]} dB/oct")
        text = "   ".join(parts)
        p.setFont(theme.ui_font(7))
        width = p.fontMetrics().horizontalAdvance(text) + 14
        center = self.dot(band)
        above = center.y() - 30 > plot.top()
        rect = QRectF(center.x() - width / 2, center.y() + (-30 if above else 16), width, 16)
        rect.moveLeft(max(plot.left() + 2, min(plot.right() - width - 2, rect.left())))
        p.setPen(QPen(_alpha(band.color, 170), 1))
        p.setBrush(QColor(12, 13, 16, 225))
        p.drawRoundedRect(rect, 8, 8)
        p.setPen(QColor(235, 235, 240))
        p.drawText(rect, Qt.AlignmentFlag.AlignCenter, text)

    def _paint_overlays(self, p: QPainter, plot: QRectF) -> None:
        p.setFont(theme.ui_font(7))
        self._overlays = {}
        for name, text, left in (("analyzer", ANALYZER_MODES[VIEW["analyzer"]], True),
                                 ("range", f"± {VIEW['range']:g} dB", False)):
            width = p.fontMetrics().horizontalAdvance(text) + 12
            right = plot.right() - width - 4 - self.right_inset
            rect = QRectF(plot.left() + 4 if left else right, plot.top() + 5, width, 14)
            self._overlays[name] = rect
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(QColor(255, 255, 255, 16))
            p.drawRoundedRect(rect, 7, 7)
            p.setPen(QColor(255, 255, 255, 120))
            p.drawText(rect, Qt.AlignmentFlag.AlignCenter, text)


def _alpha(color: QColor, alpha: int) -> QColor:
    result = QColor(color)
    result.setAlpha(max(0, min(255, alpha)))
    return result


def _label(text: str) -> QLabel:
    label = QLabel(text)
    label.setAlignment(Qt.AlignmentFlag.AlignCenter)
    label.setStyleSheet(f"color: {theme.TEXT_DIM}; font-size: 8pt;")
    return label


class TypeButton(QPushButton):
    """A filter type's button: its shape, drawn small."""

    def __init__(self, kind: int):
        super().__init__()
        self.kind = kind
        self.color = QColor(theme.ACCENT)
        self.setCheckable(True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setFixedSize(17, 15)
        self.setToolTip(TYPES[kind])

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        rect = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        hovered = self.underMouse() and self.isEnabled()
        p.setPen(Qt.PenStyle.NoPen)
        p.setBrush(_alpha(self.color, 60) if self.isChecked() else QColor(theme.SURFACE_HOVER if hovered
                                                                           else theme.SURFACE))
        p.drawRoundedRect(rect, 3, 3)
        inner = rect.adjusted(3, 3, -3, -3)
        path = _type_shape(self.kind, inner)
        color = self.color if self.isChecked() else QColor(theme.TEXT if self.isEnabled() else theme.TEXT_DISABLED)
        p.setPen(QPen(color, 1.3, Qt.PenStyle.SolidLine, Qt.PenCapStyle.RoundCap, Qt.PenJoinStyle.RoundJoin))
        p.setBrush(Qt.BrushStyle.NoBrush)
        p.drawPath(path)
        p.end()


def _type_shape(kind: int, r: QRectF) -> QPainterPath:
    """A filter type's outline in `r`, for its button."""
    left, right, top, bottom = r.left(), r.right(), r.top(), r.bottom()
    mid_x, mid_y = r.center().x(), r.center().y()
    path = QPainterPath()
    if kind == BELL:
        path.moveTo(left, mid_y + 1)
        path.cubicTo(mid_x - 2, mid_y + 1, mid_x - 2, top, mid_x, top)
        path.cubicTo(mid_x + 2, top, mid_x + 2, mid_y + 1, right, mid_y + 1)
    elif kind == LOW_SHELF:
        path.moveTo(left, top + 1)
        path.cubicTo(mid_x, top + 1, mid_x, bottom - 2, right, bottom - 2)
    elif kind == HIGH_SHELF:
        path.moveTo(left, bottom - 2)
        path.cubicTo(mid_x, bottom - 2, mid_x, top + 1, right, top + 1)
    elif kind == LOW_CUT:
        path.moveTo(left + 1, bottom)
        path.cubicTo(mid_x - 1, top + 1, mid_x, top + 1, right, top + 1)
    elif kind == HIGH_CUT:
        path.moveTo(left, top + 1)
        path.cubicTo(mid_x, top + 1, mid_x + 1, top + 1, right - 1, bottom)
    elif kind == NOTCH:
        path.moveTo(left, top + 1)
        path.cubicTo(mid_x - 1, top + 1, mid_x - 0.5, bottom, mid_x, bottom)
        path.cubicTo(mid_x + 0.5, bottom, mid_x + 1, top + 1, right, top + 1)
    elif kind == BAND_PASS:
        path.moveTo(left, bottom)
        path.cubicTo(mid_x - 2, bottom, mid_x - 1, top, mid_x, top)
        path.cubicTo(mid_x + 1, top, mid_x + 2, bottom, right, bottom)
    else:  # tilt
        path.moveTo(left, bottom - 1)
        path.cubicTo(mid_x - 2, bottom - 1, mid_x + 2, top + 1, right, top + 1)
    return path


class _Touch(QObject):
    """Pressing a control touches its parameter (the arrangement shows its automation)."""

    def __init__(self, widget: QWidget, touched):
        super().__init__(widget)
        self._touched = touched
        widget.installEventFilter(self)

    def eventFilter(self, _watched, event) -> bool:
        if event.type() == QEvent.Type.MouseButtonPress and event.button() == Qt.MouseButton.LeftButton:
            self._touched()
        return False


class BandPanel(QWidget):
    """The selected band's controls."""

    def __init__(self, view: EqEditor):
        super().__init__()
        self.view = view
        self.setFixedWidth(PANEL_WIDTH)
        self.pages = QStackedWidget()
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.addWidget(self.pages)

        hint = QLabel("Click the curve\nto add a band\n\nDouble-click a band\nto switch it off")
        hint.setAlignment(Qt.AlignmentFlag.AlignCenter)
        hint.setStyleSheet(f"color: {theme.TEXT_DISABLED}; font-size: 8pt;")
        self.pages.addWidget(hint)

        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(3)
        header = QHBoxLayout()
        header.setSpacing(4)
        self.title = QLabel()
        self.title.setFont(theme.ui_font(8, bold=True))
        header.addWidget(self.title, 1)
        self.on = ToggleButton(role="activator", tooltip="Band On/Off")
        self.on.setFixedSize(14, 14)
        self.on.clicked.connect(lambda checked: self._set("on", 1.0 if checked else 0.0, text="Switch EQ Band"))
        header.addWidget(self.on)
        remove = QPushButton("✕")
        remove.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        remove.setFixedSize(16, 14)
        remove.setToolTip("Delete the band")
        remove.setStyleSheet("font-size: 7pt; padding: 0;")
        remove.clicked.connect(lambda: self.view.remove(self.view.selected))
        header.addWidget(remove)
        layout.addLayout(header)

        types = QHBoxLayout()
        types.setSpacing(1)
        self.types: list[TypeButton] = []
        for kind in range(len(TYPES)):
            button = TypeButton(kind)
            button.clicked.connect(lambda _c, k=kind: self.view.set_type(self.view.selected, k))
            types.addWidget(button)
            self.types.append(button)
        layout.addLayout(types)

        knobs = QHBoxLayout()
        knobs.setSpacing(2)
        self.knobs: dict[str, tuple[Knob, QLabel]] = {}
        for name, title, low, high, log, bipolar in (("freq", "Freq", FREQ_MIN, FREQ_MAX, True, False),
                                                     ("gain", "Gain", -GAIN_MAX, GAIN_MAX, False, True),
                                                     ("q", "Q", Q_MIN, Q_MAX, True, False)):
            column = QVBoxLayout()
            column.setSpacing(0)
            column.addWidget(_label(title))
            knob = Knob(low, high, low, default={"freq": 1000.0, "gain": 0.0, "q": 1.0}[name], log_scale=log,
                        bipolar=bipolar, formatter=self._formatter(name))
            knob.setFixedSize(28, 28)
            knob.valueChanged.connect(lambda v, gesture, n=name: self._set(n, v, gesture))
            knob.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
            knob.customContextMenuRequested.connect(
                lambda pos, k=knob, n=name: self._automation_menu(n, k.mapToGlobal(pos)))
            _Touch(knob, lambda n=name: self._touch(n))
            readout = QLabel()
            readout.setAlignment(Qt.AlignmentFlag.AlignCenter)
            readout.setStyleSheet("font-size: 8pt;")
            column.addWidget(knob, 0, Qt.AlignmentFlag.AlignHCenter)
            column.addWidget(readout)
            knobs.addLayout(column)
            self.knobs[name] = (knob, readout)
        layout.addLayout(knobs)

        choices = QHBoxLayout()
        choices.setSpacing(3)
        self.slope = QComboBox()
        self.slope.addItems([f"{s} dB/oct" for s in SLOPES])
        self.slope.setToolTip("Slope")
        self.slope.activated.connect(lambda i: self._set("slope", float(i), text="Change EQ Band Slope"))
        self.place = QComboBox()
        self.place.addItems(PLACES)
        self.place.setToolTip("Placement: both channels, or only the left, right, mid or side")
        self.place.activated.connect(lambda i: self._set("place", float(i), text="Change EQ Band Placement"))
        for combo in (self.slope, self.place):
            combo.setFocusPolicy(Qt.FocusPolicy.NoFocus)
            combo.setFixedHeight(18)
            combo.setFont(theme.ui_font(7))
            choices.addWidget(combo, 1)
        layout.addLayout(choices)
        layout.addStretch(1)
        self.pages.addWidget(page)

    def _formatter(self, name: str):
        if name == "freq":
            return format_freq
        if name == "gain":
            return lambda v: f"{v:+.1f} dB"
        return lambda v: f"{v:.2f}"

    def _set(self, name: str, value: float, gesture: object | None = None, text: str = "Change EQ Band") -> None:
        if self.view.selected is not None:
            self.view.set_params({param(self.view.selected, name): float(value)}, gesture, text)

    def _touch(self, name: str) -> None:
        if self.view.selected is not None:
            self.view.host.touch(param(self.view.selected, name))

    def _automation_menu(self, name: str, at: QPoint) -> None:
        if self.view.selected is not None:
            self.view.host.automation_menu(param(self.view.selected, name), at)

    def sync(self) -> None:
        band = self.view.band(self.view.selected) if self.view.selected is not None else None
        self.pages.setCurrentIndex(0 if band is None else 1)
        if band is None:
            return
        color = band.color
        self.title.setText(f"Band {band.index + 1}")
        self.title.setStyleSheet(f"color: {color.name()};")
        self.on.set_checked_silently(band.on)
        for button in self.types:
            button.color = color
            button.setChecked(button.kind == band.type)
            button.update()
        for name, value in (("freq", band.freq), ("gain", band.gain), ("q", band.q)):
            knob, readout = self.knobs[name]
            knob.set_color(color)
            knob.setValue(value)
            knob.set_automation(self.view.host.automation_state(param(band.index, name)))
            readout.setText(self._formatter(name)(value))
            enabled = name != "gain" or band.type in GAIN_TYPES
            knob.setEnabled(enabled)
            readout.setEnabled(enabled)
        self.slope.setCurrentIndex(band.slope)
        self.slope.setEnabled(band.type in SLOPE_TYPES)
        self.place.setCurrentIndex(band.place)


class EqEditor(QWidget):
    """The graph, the selected band's panel, and the output gain and gain scale, for a host
    (EqWidget or EqWindow): host.value(id), set_params(values, gesture, text), touch(id),
    automation_state(id), automation_menu(id, at), sample_rate, read_display(id)."""

    def __init__(self, host, windowed: bool = False):
        super().__init__()
        self.host = host
        self.bands: list[Band | None] = [None] * BANDS
        self.scale = 1.0
        self.selected: int | None = None
        self.analyzer = Analyzer()
        self.graph = EqGraph(self)
        self.panel = BandPanel(self)
        self.globals: dict[str, tuple[Knob, QLabel, str]] = {}
        self.corners = {"scale": self._corner("scale", "Scale", "%", 0.0, 200.0, 100.0, left=True),
                        "output": self._corner("output", "Output", "dB", -36.0, 36.0, 0.0, left=False)}
        self.windowed = windowed
        self.expand: ToggleButton | None = None
        self.panel_button: ToggleButton | None = None
        if not windowed:
            self.expand = ToggleButton(icon=icons.expand(), checkable=False, tooltip="Open in a window",
                                       parent=self.graph)
            self.panel_button = ToggleButton(icon=icons.sliders(), tooltip="Show the band's controls",
                                             parent=self.graph)
            for button in (self.expand, self.panel_button):
                button.setFixedSize(18, 16)
                button.setIconSize(QSize(10, 10))
            self.expand.clicked.connect(lambda: self.host.open_window())
            self.panel_button.clicked.connect(self.show_panel)
            self.graph.right_inset = 2 * (self.expand.width() + 4)
        if windowed:  # the graph over a bar with the band's controls
            layout = QVBoxLayout(self)
            controls = QHBoxLayout()
            controls.addStretch(1)
            controls.addWidget(self.panel)
            controls.addStretch(1)
            layout.addWidget(self.graph, 1)
            layout.addLayout(controls)
            layout.setContentsMargins(0, 0, 0, 0)
        else:  # in the device view: the graph from edge to edge, the panel beside it
            layout = QHBoxLayout(self)
            layout.addWidget(self.graph, 1)
            layout.addWidget(self.panel)
            layout.setContentsMargins(0, 0, PANEL_INSET, 0)
            self.panel.layout().setContentsMargins(0, 5, 0, 4)
        layout.setSpacing(SPACING)
        self._layout = layout
        if not windowed:
            self._apply_panel()
        self.sync()

    def show_panel(self, shown: bool) -> None:
        """Shows (or collapses) the selected band's controls beside the curve, in every EQ in the
        device view."""
        VIEW["panel"] = bool(shown)
        for editor in list(_EDITORS) + ([self] if self not in _EDITORS else []):
            if not editor.windowed:
                editor._apply_panel()

    def _apply_panel(self) -> None:
        shown = bool(VIEW["panel"])
        self.panel.setVisible(shown)
        self._layout.setContentsMargins(0, 0, PANEL_INSET if shown else 0, 0)
        self.panel_button.set_checked_silently(shown)
        self.panel_button.setToolTip("Collapse the band's controls" if shown else "Show the band's controls")
        if hasattr(self.host, "panel_shown"):
            self.host.panel_shown(shown)

    def _corner(self, name: str, title: str, unit: str, low: float, high: float, default: float,
                left: bool) -> QFrame:
        """A knob in a corner of the graph, with its name and value beside it."""
        corner = QFrame(self.graph)
        corner.setObjectName("eqCorner")
        corner.setStyleSheet("#eqCorner { background: rgba(10, 11, 14, 185); border-radius: 6px; }")
        row = QHBoxLayout(corner)
        row.setContentsMargins(4, 2, 6 if left else 4, 2)
        row.setSpacing(4)
        knob = Knob(low, high, default, default=default, bipolar=name == "output",
                    formatter=lambda v: format_value(v, unit))
        knob.setFixedSize(24, 24)
        knob.valueChanged.connect(lambda v, gesture: self.set_params({name: v}, gesture))
        knob.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
        knob.customContextMenuRequested.connect(lambda pos: self.host.automation_menu(name, knob.mapToGlobal(pos)))
        _Touch(knob, lambda: self.host.touch(name))
        texts = QVBoxLayout()
        texts.setSpacing(0)
        caption = QLabel(title)
        caption.setStyleSheet(f"color: {theme.TEXT_DIM}; font-size: 7pt; background: transparent;")
        readout = QLabel()
        readout.setStyleSheet("font-size: 8pt; background: transparent;")
        alignment = Qt.AlignmentFlag.AlignLeft if left else Qt.AlignmentFlag.AlignRight
        for label in (caption, readout):
            label.setAlignment(alignment | Qt.AlignmentFlag.AlignVCenter)
            label.setMinimumWidth(label.fontMetrics().horizontalAdvance("-36.0 dB"))
            texts.addWidget(label)
        if left:
            row.addWidget(knob)
            row.addLayout(texts)
        else:
            row.addLayout(texts)
            row.addWidget(knob)
        self.globals[name] = (knob, readout, unit)
        return corner

    def place_overlays(self) -> None:
        """Puts the corners' knobs (and the expand button) where they go on the graph."""
        rect = self.graph.rect()
        for name, corner in self.corners.items():
            corner.adjustSize()
            x = 4 if name == "scale" else rect.width() - corner.width() - 4
            corner.move(x, rect.height() - corner.height() - 4)
        if self.expand is not None:
            self.expand.move(rect.width() - self.expand.width() - 4, 4)
            self.panel_button.move(self.expand.x() - self.panel_button.width() - 4, 4)

    # --- The model ------------------------------------------------------------------------

    def band(self, index: int | None) -> Band | None:
        return self.bands[index] if index is not None and 0 <= index < BANDS else None

    def free_band(self) -> int | None:
        return next((i for i, band in enumerate(self.bands) if band is None), None)

    def sync(self) -> None:
        """Reads every band (and the globals) from the host, and shows them."""
        value = self.host.value
        bands: list[Band | None] = []
        for i in range(BANDS):
            if value(param(i, "used")) < 0.5:
                bands.append(None)
                continue
            bands.append(Band(i, value(param(i, "on")) >= 0.5, round(value(param(i, "type"))),
                              value(param(i, "freq")), value(param(i, "gain")), value(param(i, "q")),
                              round(value(param(i, "slope"))), round(value(param(i, "place")))))
        self.bands = bands
        self.scale = value("scale") / 100.0
        if self.band(self.selected) is None:
            self.selected = None
        for name, (knob, readout, unit) in self.globals.items():
            knob.setValue(value(name))
            knob.set_automation(self.host.automation_state(name))
            readout.setText(format_value(value(name), unit))
        self.panel.sync()
        self.graph.update()

    def refresh_views(self) -> None:
        """The view settings changed: every open EQ editor shows them."""
        for editor in list(_EDITORS):
            editor.graph.update()

    def set_params(self, values: dict[str, float], gesture: object | None = None,
                   text: str = "Change EQ") -> None:
        self.host.set_params(values, gesture, text)
        self.sync()

    def select(self, index: int | None) -> None:
        if index != self.selected:
            self.selected = index
            self.panel.sync()
            self.graph.update()

    def set_type(self, index: int | None, kind: int) -> None:
        band = self.band(index)
        if band is None or band.type == kind:
            return
        values = {param(index, "type"): float(kind)}
        if kind in DEFAULT_SLOPE and band.type not in SLOPE_TYPES:
            values[param(index, "slope")] = float(DEFAULT_SLOPE[kind])
        self.set_params(values, text="Change EQ Band Type")

    def remove(self, index: int | None) -> None:
        if self.band(index) is None:
            return
        self.set_params({param(index, "used"): 0.0}, text="Delete EQ Band")
        if self.selected == index:
            self.select(None)

    def remove_all(self) -> None:
        values = {param(b.index, "used"): 0.0 for b in self.bands if b is not None}
        if values:
            self.set_params(values, text="Delete EQ Bands")

    def feed(self) -> None:
        """The analyzer takes what the engine played since, and the graph shows it."""
        if VIEW["analyzer"] == 0:
            return
        rate = self.host.sample_rate
        if VIEW["analyzer"] in (1, 3):
            self.analyzer.feed("input", self.host.read_display("input"), rate)
        if VIEW["analyzer"] in (2, 3):
            self.analyzer.feed("output", self.host.read_display("output"), rate)
        self.graph.update()

    def showEvent(self, event) -> None:
        _EDITORS.add(self)
        super().showEvent(event)

    def hideEvent(self, event) -> None:
        _EDITORS.discard(self)
        super().hideEvent(event)


_EDITORS: set[EqEditor] = set()  # those showing (the view settings repaint them all)
_WINDOWS: dict[tuple[str, str], EqWindow] = {}  # (track id, device id): its window


@device_editor("eq")
class EqWidget(DeviceWidget):
    device_width = GRAPH_WIDTH + SPACING + PANEL_WIDTH + PANEL_INSET + 2  # with the band's controls
    collapsed_width = GRAPH_WIDTH + 2  # without them (the default)

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.defaults = {info.id: info.default_value for info in self.infos}
        self.content.removeItem(self.params)  # its own editor instead of pages of knobs
        self.body.setContentsMargins(0, 0, 0, 0)  # the graph from edge to edge
        self.eq = EqEditor(self)
        self.content.addWidget(self.eq, 1)

    def _set_param_count(self, count: int, page: int = 0) -> None:
        self.param_count, self.pages, self.page = count, 1, 0
        for widget in (self.previous, self.page_label, self.next):
            widget.setVisible(False)

    # --- The host ---------------------------------------------------------------------

    @property
    def sample_rate(self) -> float:
        return float(self.bridge.engine.sample_rate)

    def value(self, param_id: str) -> float:
        """A parameter as it is now: its automation's value while that plays."""
        if self.automation_state(param_id) == "on":
            value = self.bridge.current_value(self.track_id, device_key(self.device_id, param_id))
            if value is not None:
                return value
        return self.device().params.get(param_id, self.defaults.get(param_id, 0.0))

    def set_params(self, values: dict[str, float], gesture: object | None = None, text: str = "Change EQ") -> None:
        self.editor.set_device_params(self.track_id, self.device_id, values, gesture, text)

    def touch(self, param_id: str) -> None:
        self.editor.touch_parameter(self.track_id, device_key(self.device_id, param_id))

    def automation_menu(self, param_id: str, at: QPoint) -> None:
        self._automation_menu(param_id, at)

    def open_window(self) -> None:
        open_window(self.editor, self.bridge, self.track_id, self.device_id)

    def panel_shown(self, shown: bool) -> None:
        """The band's controls were shown or collapsed: the device grows or shrinks to fit."""
        if not self.folded:
            self.setFixedWidth(self.device_width if shown else self.collapsed_width)

    # --- The device view ----------------------------------------------------------------

    def refresh(self, device) -> None:
        super().refresh(device)
        self.eq.sync()

    def follows_automation(self) -> bool:
        return any(self.automation_state(info.id) == "on" for info in self.infos)

    def refresh_automation(self) -> None:
        super().refresh_automation()
        if hasattr(self, "eq"):
            self.eq.sync()

    def refresh_displays(self) -> None:
        self.eq.feed()


def open_window(editor, bridge, track_id: str, device_id: str) -> EqWindow:
    """The EQ's window (made, or brought to the front)."""
    window = _WINDOWS.get((track_id, device_id))
    if window is None:
        window = EqWindow(editor, bridge, track_id, device_id)
        _WINDOWS[(track_id, device_id)] = window
    window.show()
    window.raise_()
    window.activateWindow()
    return window


class EqWindow(QWidget):
    """The EQ, bigger, in a window of its own; it stays open as the device view changes, and
    closes when the device goes."""

    def __init__(self, editor, bridge, track_id: str, device_id: str):
        super().__init__(None, Qt.WindowType.Window)
        self.editor, self.bridge = editor, bridge
        self.track_id, self.device_id = track_id, device_id
        self.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)
        self.setStyleSheet(f"EqWindow {{ background: {theme.PANEL}; }}")
        self._positions: dict[tuple[int, str], int] = {}
        info = next(d for d in ge.builtin_devices() if d.id == "eq")
        self.defaults = {p.id: p.default_value for p in info.params}
        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 10, 10, 10)
        self.eq = EqEditor(self, windowed=True)
        layout.addWidget(self.eq)
        self.resize(1100, 520)
        self._update_title()
        project = editor.project
        project.device_param_changed.connect(self._on_param)
        project.devices_changed.connect(self._check)
        project.track_removed.connect(lambda *_: self._check())
        project.reset.connect(self.close)
        bridge.automation_state_changed.connect(lambda owner: self.eq.sync() if owner == track_id else None)
        bridge.position_changed.connect(self._follow)
        self._timer = QTimer(self)
        self._timer.setInterval(16)
        self._timer.timeout.connect(lambda: self.eq.feed() if self.isVisible() else None)
        self._timer.start()

    def _update_title(self) -> None:
        project = self.editor.project
        name = project.track(self.track_id).name if project.has_owner(self.track_id) else ""
        self.setWindowTitle(f"EQ — {name}" if name else "EQ")

    def _alive(self) -> bool:
        return self.editor.project.has_device(self.track_id, self.device_id)

    def _check(self, *_args) -> None:
        if not self._alive():
            self.close()
        else:
            self._update_title()
            self.eq.sync()

    def _on_param(self, track_id: str, device_id: str, _param_id: str) -> None:
        if track_id == self.track_id and device_id == self.device_id and self._alive():
            self.eq.sync()

    def _follow(self) -> None:
        if self._alive() and any(self.automation_state(p) == "on" for p in self.defaults):
            self.eq.sync()

    def closeEvent(self, event) -> None:
        _WINDOWS.pop((self.track_id, self.device_id), None)
        self._timer.stop()
        for signal, slot in ((self.editor.project.device_param_changed, self._on_param),
                             (self.editor.project.devices_changed, self._check),
                             (self.bridge.position_changed, self._follow)):
            try:
                signal.disconnect(slot)
            except (RuntimeError, TypeError):
                pass
        super().closeEvent(event)

    # --- The host ---------------------------------------------------------------------

    @property
    def sample_rate(self) -> float:
        return float(self.bridge.engine.sample_rate)

    def automation_state(self, param_id: str) -> str | None:
        return automation_state(self.bridge, self.track_id, device_key(self.device_id, param_id))

    def value(self, param_id: str) -> float:
        if not self._alive():
            return self.defaults.get(param_id, 0.0)
        if self.automation_state(param_id) == "on":
            value = self.bridge.current_value(self.track_id, device_key(self.device_id, param_id))
            if value is not None:
                return value
        device = self.editor.project.device(self.track_id, self.device_id)
        return device.params.get(param_id, self.defaults.get(param_id, 0.0))

    def set_params(self, values: dict[str, float], gesture: object | None = None, text: str = "Change EQ") -> None:
        if self._alive():
            self.editor.set_device_params(self.track_id, self.device_id, values, gesture, text)

    def touch(self, param_id: str) -> None:
        self.editor.touch_parameter(self.track_id, device_key(self.device_id, param_id))

    def automation_menu(self, param_id: str, at: QPoint) -> None:
        editor, bridge, owner = self.editor, self.bridge, self.track_id
        key = device_key(self.device_id, param_id)
        menu = QMenu(self)
        menu.addAction("Show Automation", lambda: editor.show_automation(owner, key)).setEnabled(
            bridge.can_automate(owner, key))
        menu.addAction("Delete Automation", lambda: editor.clear_envelope(owner, key)).setEnabled(
            bool(editor.project.envelope(owner, key)))
        if bridge.is_overridden(owner, key):
            menu.addAction("Re-Enable Automation", lambda: bridge.re_enable_automation(owner))
        menu.exec(at)

    def read_display(self, display_id: str) -> np.ndarray:
        engine_id = self.bridge.engine_device_id(self.track_id, self.device_id) if self._alive() else None
        if engine_id is None:
            return np.zeros(0, np.float32)
        try:
            displays = [d.id for d in self.bridge.engine.processor_displays(engine_id)]
            index = displays.index(display_id)
            values, self._positions[(engine_id, display_id)] = self.bridge.engine.read_processor_display(
                engine_id, index, self._positions.get((engine_id, display_id), 0))
        except ValueError:
            return np.zeros(0, np.float32)
        return values
