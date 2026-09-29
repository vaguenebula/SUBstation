"""Track headers (right of the lanes, as in Ableton): name, activator, solo,
volume, pan, meter. Plus the master track header and lane."""

from __future__ import annotations

from PySide6.QtCore import QEvent, QObject, QRect, QRectF, Qt
from PySide6.QtGui import (
    QColor,
    QContextMenuEvent,
    QIcon,
    QMouseEvent,
    QPainter,
    QPixmap,
    QWheelEvent,
)
from PySide6.QtWidgets import QLineEdit, QMenu, QWidget

from ... import theme
from ...audio.engine_bridge import EngineBridge
from ...model.editor import ProjectEditor
from ...model.project import MAX_TRACK_HEIGHT, MIN_TRACK_HEIGHT, TRACK_COLORS
from ...model.timebase import format_db, format_pan
from ..widgets import Knob, MeterWidget, ToggleButton, ValueBox
from .grid import draw_grid, draw_loop_region
from .lanes_canvas import resize_track_by_wheel
from .view_state import Selection, TrackLayout, ViewState

RESIZE_GRAB = 4
NAME_ROW = 22


def volume_box(value: float = 0.0) -> ValueBox:
    return ValueBox(value, -70.0, 6.0, step=0.25, decimals=1, formatter=format_db, sample_text="-70.0 dB")


def color_swatch(color: str) -> QIcon:
    pixmap = QPixmap(14, 14)
    pixmap.fill(QColor(color))
    return QIcon(pixmap)


class TrackHeader(QWidget):
    def __init__(self, track_id: str, editor: ProjectEditor, selection: Selection, parent: QWidget | None = None):
        super().__init__(parent)
        self.track_id = track_id
        self.editor = editor
        self.project = editor.project
        self.selection = selection
        self.number = 1
        self._resize: tuple[float, int] | None = None
        self._rename: QLineEdit | None = None
        self.setMouseTracking(True)

        self.activator = ToggleButton("1", role="activator", tooltip="Track Activator (unmute)", parent=self)
        self.solo = ToggleButton("S", role="solo", tooltip="Solo", parent=self)
        self.volume = volume_box()
        self.volume.setParent(self)
        self.volume.setToolTip("Track Volume (drag, double-click to type)")
        self.pan = Knob(-1.0, 1.0, 0.0, default=0.0, bipolar=True, formatter=format_pan, parent=self)
        self.meter = MeterWidget(self)

        self.activator.toggled.connect(lambda on: self.editor.set_track_param(self.track_id, "mute", not on))
        self.solo.toggled.connect(lambda on: self.editor.set_track_param(self.track_id, "solo", on))
        self.volume.valueChanged.connect(
            lambda v, key: self.editor.set_track_param(self.track_id, "volume_db", v, key))
        self.pan.valueChanged.connect(lambda v, key: self.editor.set_track_param(self.track_id, "pan", v, key))
        for widget in (self.volume, self.pan, self.activator, self.solo, self.meter):
            widget.installEventFilter(self)  # Alt+wheel over a control still resizes the track
        self.refresh()

    @property
    def track(self):
        return self.project.track(self.track_id)

    def refresh(self) -> None:
        track = self.track
        self.activator.set_checked_silently(not track.mute)
        self.solo.set_checked_silently(track.solo)
        self.volume.setValue(track.volume_db)
        self.pan.setValue(track.pan)
        self.update()

    def set_number(self, number: int) -> None:
        if number != self.number:
            self.number = number
            self.activator.setText(str(number))

    def resizeEvent(self, _event) -> None:
        w, h = self.width(), self.height()
        meter_w = 8
        self.meter.setGeometry(w - meter_w - 4, 4, meter_w, max(8, h - 9))
        right = w - meter_w - 10
        self.solo.setGeometry(right - 22, 4, 22, 17)
        self.activator.setGeometry(right - 22 - 30, 4, 28, 17)
        second_row = h >= 48
        for widget in (self.volume, self.pan):
            widget.setVisible(second_row)
        if second_row:
            self.volume.setGeometry(10, NAME_ROW + 4, 76, 20)
            self.pan.setGeometry(92, NAME_ROW + 1, 26, 26)

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        track = self.track
        selected = self.selection.track_id == self.track_id
        p.fillRect(self.rect(), QColor(theme.LANE_SELECTED if selected else theme.PANEL_ALT))
        p.fillRect(QRect(0, 0, 5, self.height() - 1), QColor(track.color))
        if self._rename is None:
            p.setPen(QColor(theme.TEXT if not track.mute else theme.TEXT_DIM))
            p.setFont(theme.ui_font(9, bold=selected))
            name_rect = QRect(10, 3, self.activator.x() - 14, NAME_ROW - 4)
            name = p.fontMetrics().elidedText(track.name, Qt.TextElideMode.ElideRight, name_rect.width())
            p.drawText(name_rect, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft, name)
        p.fillRect(QRect(0, self.height() - 1, self.width(), 1), QColor(theme.BORDER))
        p.fillRect(QRect(0, 0, 1, self.height()), QColor(theme.BORDER))

    # --- Interaction -----------------------------------------------------------------

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:
        if event.type() == QEvent.Type.Wheel and self._alt_wheel(event):
            return True
        return super().eventFilter(watched, event)

    def wheelEvent(self, event: QWheelEvent) -> None:
        if not self._alt_wheel(event):
            event.ignore()

    def _alt_wheel(self, event: QWheelEvent) -> bool:
        """Alt+wheel resizes this track (Qt may report it on either axis)."""
        mods = event.modifiers()
        if not mods & Qt.KeyboardModifier.AltModifier or mods & Qt.KeyboardModifier.ControlModifier:
            return False
        delta = event.angleDelta()
        resize_track_by_wheel(self.editor, self.track_id, delta.y() or delta.x())
        event.accept()
        return True

    def _in_resize_zone(self, y: float) -> bool:
        return y >= self.height() - RESIZE_GRAB

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        if self._in_resize_zone(event.position().y()):
            self._resize = (event.globalPosition().y(), self.track.height)
        else:
            self.selection.select_track(self.track_id, focus_track=True)

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._resize:
            start_y, start_h = self._resize
            height = int(start_h + event.globalPosition().y() - start_y)
            self.editor.set_track_height(self.track_id, max(MIN_TRACK_HEIGHT, min(MAX_TRACK_HEIGHT, height)))
            return
        resize = self._in_resize_zone(event.position().y())
        self.setCursor(Qt.CursorShape.SplitVCursor if resize else Qt.CursorShape.ArrowCursor)

    def mouseReleaseEvent(self, _event: QMouseEvent) -> None:
        self._resize = None

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        if event.position().y() < NAME_ROW:
            self.start_rename()

    def start_rename(self) -> None:
        if self._rename:
            return
        editor = QLineEdit(self.track.name, self)
        editor.setGeometry(8, 2, self.activator.x() - 12, NAME_ROW - 2)
        editor.selectAll()
        editor.show()
        editor.setFocus()
        editor.editingFinished.connect(lambda: self._finish_rename(editor))
        self._rename = editor
        self.update()

    def _finish_rename(self, editor: QLineEdit) -> None:
        if self._rename is not editor:
            return
        self._rename = None
        name = editor.text().strip()
        editor.deleteLater()
        if name:
            self.editor.rename_track(self.track_id, name)
        self.update()

    def contextMenuEvent(self, event: QContextMenuEvent) -> None:
        self.selection.select_track(self.track_id, focus_track=True)
        menu = QMenu(self)
        menu.addAction("Rename", self.start_rename)
        colors = menu.addMenu("Color")
        for color in TRACK_COLORS:
            colors.addAction(color_swatch(color), color, lambda c=color: self.editor.set_track_color(self.track_id, c))
        menu.addSeparator()
        index = self.project.track_index(self.track_id)
        menu.addAction("Insert Audio Track", lambda: self.editor.add_audio_track(index + 1))
        menu.addAction("Insert MIDI Track", lambda: self.editor.add_midi_track(index + 1))
        menu.addAction("Delete Track", lambda: self.editor.delete_tracks([self.track_id]))
        menu.exec(event.globalPos())


class TrackHeaderColumn(QWidget):
    """Hosts one TrackHeader per track, positioned to match the lanes' scroll."""

    def __init__(self, editor: ProjectEditor, view: ViewState, layout: TrackLayout, selection: Selection,
                 bridge: EngineBridge, parent: QWidget | None = None):
        super().__init__(parent)
        self.editor = editor
        self.view = view
        self.layout_model = layout
        self.selection = selection
        self.bridge = bridge
        self.headers: dict[str, TrackHeader] = {}
        selection.changed.connect(self._repaint_headers)
        bridge.meters_updated.connect(self._update_meters)
        view.vscroll_changed.connect(self.relayout)

    def sync(self) -> None:
        """Create/remove headers to match the project, then position them."""
        wanted = [row.track_id for row in self.layout_model.rows]
        for track_id in list(self.headers):
            if track_id not in wanted:
                self.headers.pop(track_id).deleteLater()
        for track_id in wanted:
            if track_id not in self.headers:
                header = TrackHeader(track_id, self.editor, self.selection, self)
                header.show()
                self.headers[track_id] = header
        self.relayout()

    def refresh(self, track_id: str) -> None:
        if track_id in self.headers:
            self.headers[track_id].refresh()

    def relayout(self) -> None:
        for index, row in enumerate(self.layout_model.rows):
            header = self.headers.get(row.track_id)
            if header:
                header.setGeometry(0, row.top - self.view.scroll_y, self.width(), row.height)
                header.set_number(index + 1)

    def resizeEvent(self, _event) -> None:
        self.relayout()

    def paintEvent(self, _event) -> None:
        QPainter(self).fillRect(self.rect(), QColor(theme.EMPTY_AREA))

    def _repaint_headers(self) -> None:
        for header in self.headers.values():
            header.update()

    def _update_meters(self) -> None:
        for track_id, header in self.headers.items():
            header.meter.set_levels(*self.bridge.meters.get(track_id, (0.0, 0.0)))

    def mousePressEvent(self, _event) -> None:
        self.selection.select_track(None)

    def wheelEvent(self, event: QWheelEvent) -> None:
        self.view.set_scroll_y(self.view.scroll_y - event.angleDelta().y() / 120.0 * 48)
        event.accept()


class MasterHeader(QWidget):
    def __init__(self, editor: ProjectEditor, bridge: EngineBridge, parent: QWidget | None = None):
        super().__init__(parent)
        self.editor = editor
        self.project = editor.project
        self.bridge = bridge
        self.volume = volume_box(self.project.master_volume_db)
        self.volume.setParent(self)
        self.volume.setToolTip("Master Volume")
        self.meter = MeterWidget(self)
        self.volume.valueChanged.connect(lambda v, key: self.editor.set_master_volume(v, key))
        self.project.settings_changed.connect(lambda: self.volume.setValue(self.project.master_volume_db))
        self.project.reset.connect(lambda: self.volume.setValue(self.project.master_volume_db))
        bridge.meters_updated.connect(lambda: self.meter.set_levels(*bridge.meters.get("master", (0.0, 0.0))))

    def resizeEvent(self, _event) -> None:
        w, h = self.width(), self.height()
        self.meter.setGeometry(w - 12, 4, 8, h - 8)
        self.volume.setGeometry(w - 12 - 6 - 76, (h - 20) // 2, 76, 20)

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(theme.PANEL_ALT))
        p.fillRect(QRect(0, 0, 5, self.height()), QColor(theme.TEXT_DIM))
        p.fillRect(QRect(0, 0, self.width(), 1), QColor(theme.BORDER))
        p.fillRect(QRect(0, 0, 1, self.height()), QColor(theme.BORDER))
        p.setPen(QColor(theme.TEXT))
        p.setFont(theme.ui_font(9, bold=True))
        p.drawText(QRect(12, 0, 100, self.height()), Qt.AlignmentFlag.AlignVCenter, "Master")


class MasterLane(QWidget):
    """The master track's lane: grid, loop region and playhead."""

    def __init__(self, view: ViewState, parent: QWidget | None = None):
        super().__init__(parent)
        self.view = view
        self._playhead = 0.0
        view.changed.connect(self.update)
        view.grid_changed.connect(self.update)
        view.project.settings_changed.connect(self.update)

    def set_playhead(self, beat: float) -> None:
        for b in (self._playhead, beat):
            x = int(self.view.beat_to_x(b))
            self.update(QRect(x - 2, 0, 5, self.height()))
        self._playhead = beat

    def paintEvent(self, event) -> None:
        p = QPainter(self)
        visible = QRectF(event.rect())
        p.fillRect(visible, QColor(theme.LANE))
        draw_grid(p, self.view, visible.left(), visible.right(), 1, self.height())
        draw_loop_region(p, self.view, visible.left(), visible.right(), 1, self.height())
        p.fillRect(QRectF(visible.left(), 0, visible.width(), 1), QColor(theme.BORDER))
        x = round(self.view.beat_to_x(self._playhead))
        p.fillRect(QRectF(x, 0, 1, self.height()), QColor(theme.PLAYHEAD))
