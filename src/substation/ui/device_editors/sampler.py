"""The Sampler's editor: its sample's waveform beside its knobs (a page for the
sample, one for its amplitude), with what it plays (Start to End, the rest
dimmed; the loop marked when it loops) and where the newest note is.

Drop an audio file on the waveform (from the browser or the desktop), or
double-click it, to load one; drag the Start and End markers to choose what
plays. Loading a sample is undoable, as editing a parameter is."""

from __future__ import annotations

import os

import numpy as np
from PySide6.QtCore import QPointF, QRectF, Qt
from PySide6.QtGui import (
    QColor,
    QDragEnterEvent,
    QDropEvent,
    QMouseEvent,
    QPainter,
    QPen,
)
from PySide6.QtWidgets import QFileDialog, QMenu, QWidget

from ... import _engine as ge
from ... import theme
from ...audio.engine_bridge import is_audio_file
from ...model import device_state
from ...model.automation import device_key
from ..device_panel import DEVICE_WIDTH, PARAM_WIDTH, DeviceWidget
from . import device_editor

VIEW_WIDTH = 240
MARKER_GRAB = 5  # px either side of a marker that grab it
FILE_FILTER = "Audio Files (*.wav *.wave *.flac *.mp3)"


def waveform_columns(source: ge.AudioSource, width: int) -> tuple[np.ndarray, np.ndarray]:
    """The waveform's lowest and highest value under each of `width` columns
    (all channels together), from the source's peaks."""
    level = 0
    per_column = source.frames / max(1, width)
    while level + 1 < source.peak_levels and ge.AudioSource.samples_per_peak(level + 1) <= per_column:
        level += 1
    peaks = source.peaks(level)
    if len(peaks) == 0:
        return np.zeros(width, np.float32), np.zeros(width, np.float32)
    lo = peaks[:, :, 0].min(axis=1)
    hi = peaks[:, :, 1].max(axis=1)
    edges = np.minimum((np.arange(width + 1) * (len(peaks) / width)).astype(np.int64), len(peaks) - 1)
    starts = edges[:-1]
    starts = np.maximum.accumulate(starts)
    return np.minimum.reduceat(lo, starts), np.maximum.reduceat(hi, starts)


class SampleView(QWidget):
    """The sample's waveform, what plays of it, and the playhead."""

    def __init__(self, widget: SamplerWidget):
        super().__init__()
        self.widget = widget
        self.playhead = -1.0  # 0..1 of the sample; < 0: no note plays
        self._columns: tuple[tuple, tuple[np.ndarray, np.ndarray]] | None = None
        self._drag: tuple[str, object] | None = None  # the marker's parameter, the gesture
        self.setFixedWidth(VIEW_WIDTH)
        self.setMinimumHeight(60)
        self.setAcceptDrops(True)
        self.setMouseTracking(True)

    def set_playhead(self, where: float) -> None:
        if where != self.playhead:
            self.playhead = where
            self.update()

    # --- Markers ---------------------------------------------------------------------

    def _plot(self) -> QRectF:
        return QRectF(self.rect()).adjusted(1, 14, -1, -1)

    def _x(self, percent: float) -> float:
        plot = self._plot()
        return plot.left() + percent / 100.0 * plot.width()

    def _marker_at(self, x: float) -> str | None:
        if self.widget.sample_source() is None:
            return None
        distances = {pid: abs(self._x(self.widget.value(pid)) - x) for pid in ("start", "end")}
        nearest = min(distances, key=distances.get)
        return nearest if distances[nearest] <= MARKER_GRAB else None

    def mousePressEvent(self, event: QMouseEvent) -> None:
        marker = self._marker_at(event.position().x()) if event.button() == Qt.MouseButton.LeftButton else None
        if marker is None:
            super().mousePressEvent(event)
            return
        self._drag = (marker, object())
        self.widget.touch(marker)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._drag is None:
            near = self._marker_at(event.position().x())
            self.setCursor(Qt.CursorShape.SizeHorCursor if near else Qt.CursorShape.ArrowCursor)
            return
        marker, gesture = self._drag
        plot = self._plot()
        percent = min(100.0, max(0.0, (event.position().x() - plot.left()) / plot.width() * 100.0))
        if marker == "start":
            percent = min(percent, self.widget.value("end"))
        else:
            percent = max(percent, self.widget.value("start"))
        self.widget.set_param(marker, percent, gesture)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        if self._drag is None:
            super().mouseReleaseEvent(event)
        self._drag = None

    def mouseDoubleClickEvent(self, _event: QMouseEvent) -> None:
        self.widget.browse()

    # --- Dropping files --------------------------------------------------------------

    @staticmethod
    def _dropped_file(event) -> str | None:
        for url in event.mimeData().urls():
            path = url.toLocalFile()
            if path and is_audio_file(path):
                return path
        return None

    def dragEnterEvent(self, event: QDragEnterEvent) -> None:
        if self._dropped_file(event):
            event.acceptProposedAction()
        else:
            event.ignore()

    def dropEvent(self, event: QDropEvent) -> None:
        path = self._dropped_file(event)
        if path:
            event.acceptProposedAction()
            self.widget.load_sample(path)

    # --- Drawing ---------------------------------------------------------------------

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.fillRect(self.rect(), theme.METER_BG)
        plot = self._plot()
        path = self.widget.sample_path()
        p.setFont(theme.ui_font(8))
        if not path:
            p.setPen(QColor(theme.TEXT_DIM))
            p.drawText(self.rect(), Qt.AlignmentFlag.AlignCenter, "Drop a sample here\nor double-click to browse")
            return
        source = self.widget.sample_source()
        name = os.path.basename(path)
        error = self.widget.load_error()
        p.setPen(QColor(theme.TEXT_DIM if error is None else theme.RECORD_ON))
        title = QRectF(4, 0, self.width() - 8, plot.top())
        p.drawText(title, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft,
                   p.fontMetrics().elidedText(name if error is None else f"Missing: {name}",
                                              Qt.TextElideMode.ElideMiddle, int(title.width())))
        if source is None:
            if error is None:
                p.setPen(QColor(theme.TEXT_DIM))
                p.drawText(plot, Qt.AlignmentFlag.AlignCenter, "Loading…")
            return

        width = max(1, int(plot.width()))
        key = (source.path, source.frames, width)
        if self._columns is None or self._columns[0] != key:
            self._columns = (key, waveform_columns(source, width))
        lo, hi = self._columns[1]
        middle, half = plot.center().y(), plot.height() / 2
        p.setPen(QPen(QColor(theme.ACCENT), 1))
        for x, (a, b) in enumerate(zip(lo, hi)):
            top = middle - min(1.0, float(b)) * half
            bottom = middle - max(-1.0, float(a)) * half
            p.drawLine(QPointF(plot.left() + x + 0.5, top), QPointF(plot.left() + x + 0.5, max(bottom, top + 1)))

        start, end = self._x(self.widget.value("start")), self._x(self.widget.value("end"))
        p.fillRect(QRectF(plot.left(), plot.top(), start - plot.left(), plot.height()), theme.OUTSIDE_CLIP)
        p.fillRect(QRectF(end, plot.top(), plot.right() - end, plot.height()), theme.OUTSIDE_CLIP)
        looping = self.widget.value("loop") >= 0.5
        p.setPen(QPen(QColor(theme.TEXT if looping else theme.ACCENT), 1.5))
        for x in (start, end):
            p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()))
        if looping:  # a bracket over what loops
            p.drawLine(QPointF(start, plot.top() + 1), QPointF(end, plot.top() + 1))

        if self.playhead >= 0:
            x = plot.left() + self.playhead * plot.width()
            p.setPen(QPen(QColor(theme.PLAYHEAD), 1))
            p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()))


@device_editor("sampler")
class SamplerWidget(DeviceWidget):
    params_per_page = 6  # the sample's six, then the amplitude's
    param_columns = 3
    device_width = DEVICE_WIDTH + PARAM_WIDTH + 16 + 12 + VIEW_WIDTH  # a third column, and the waveform

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.defaults = {info.id: info.default_value for info in self.infos}
        self.view = SampleView(self)
        self.content.addWidget(self.view)
        self.bridge.source_ready.connect(self._on_source)
        self.bridge.source_failed.connect(self._on_source)
        self._fetch_source()

    # --- The sample ------------------------------------------------------------------

    def sample_path(self) -> str:
        return device_state.from_model(self.device().state).get("sample", "")

    def sample_source(self) -> ge.AudioSource | None:
        path = self.sample_path()
        return self.bridge.source(path) if path else None

    def load_error(self) -> str | None:
        path = self.sample_path()
        return self.bridge.load_error(path) if path else None

    def _fetch_source(self) -> None:
        """The sample's waveform: the engine decoded it for the sampler, so this is quick."""
        path = self.sample_path()
        if path and self.bridge.source(path) is None:
            self.bridge.request_source(path)

    def _on_source(self, path: str, *_message) -> None:
        if path == self.sample_path():
            self.view.update()

    def load_sample(self, path: str | None) -> None:
        """Play `path` (None: no sample), undoably."""
        device = self.device()
        new = device_state.to_model({"sample": os.path.normpath(path)} if path else {})
        if new != device.state:
            self.editor.set_device_state(self.track_id, self.device_id, device.state, new,
                                         "Load Sample" if path else "Clear Sample")

    def browse(self) -> None:
        current = self.sample_path()
        path, _ = QFileDialog.getOpenFileName(self, "Load Sample", os.path.dirname(current) if current else "",
                                              FILE_FILTER)
        if path:
            self.load_sample(path)

    def refresh_state(self) -> None:
        self._fetch_source()
        self.view.update()

    def add_menu_actions(self, menu: QMenu) -> None:
        menu.addAction("Load Sample…", self.browse)
        clear = menu.addAction("Clear Sample", lambda: self.load_sample(None))
        clear.setEnabled(bool(self.sample_path()))

    # --- Parameters ------------------------------------------------------------------

    def value(self, param_id: str) -> float:
        """A parameter as it is now: its automation's value while that plays."""
        if self.automation_state(param_id) == "on":
            value = self.bridge.current_value(self.track_id, device_key(self.device_id, param_id))
            if value is not None:
                return value
        return self.device().params.get(param_id, self.defaults.get(param_id, 0.0))

    def set_param(self, param_id: str, value: float, gesture: object) -> None:
        self.editor.set_device_param(self.track_id, self.device_id, param_id, value, gesture)

    def touch(self, param_id: str) -> None:
        self.editor.touch_parameter(self.track_id, device_key(self.device_id, param_id))

    def refresh(self, device) -> None:
        super().refresh(device)
        self.view.update()

    def refresh_automation(self) -> None:
        super().refresh_automation()
        if hasattr(self, "view"):  # (the first page is built before the view)
            self.view.update()

    def refresh_displays(self) -> None:
        positions = self.read_display("position")
        if len(positions):
            self.view.set_playhead(float(positions[-1]))
