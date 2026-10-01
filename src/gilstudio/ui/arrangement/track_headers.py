"""Track headers (right of the lanes, as in Ableton): name, activator, solo,
volume, pan, meter, and while a track's automation shows, its automation
choosers (and those of the lanes below it). Plus the master track's header and
lane, which show the master's automation likewise. Volume and pan follow their
automation while it plays. Click the master's header to select it: the device
view shows its effects."""

from __future__ import annotations

from PySide6.QtCore import QEvent, QObject, QPointF, QRect, QRectF, Qt
from PySide6.QtGui import (
    QColor,
    QContextMenuEvent,
    QCursor,
    QIcon,
    QMouseEvent,
    QPainter,
    QPixmap,
    QWheelEvent,
)
from PySide6.QtWidgets import QLineEdit, QMenu, QWidget

from ... import theme
from ...audio.engine_bridge import EngineBridge
from ...model.automation import MASTER, MIXER_PAN, MIXER_VOLUME
from ...model.editor import ProjectEditor
from ...model.project import MAX_TRACK_HEIGHT, MIN_TRACK_HEIGHT, TRACK_COLORS
from ...model.timebase import format_db, format_pan
from ..widgets import Knob, MeterWidget, ToggleButton, ValueBox
from . import automation_lanes
from .automation_header import CHOOSER_HEIGHT, AutomationControls
from .automation_lanes import EnvelopeArea, Hover
from .grid import draw_grid, draw_loop_region
from .lanes_canvas import SELECTION_TINT, resize_track_by_wheel
from .view_state import LaneRow, Row, Selection, TrackLayout, ViewState

RESIZE_GRAB = 4
NAME_ROW = 22
CHOOSER_ROW = NAME_ROW + 30  # the automation choosers, below volume and pan
MASTER_HEIGHT = 40


def automation_state(bridge: EngineBridge, owner: str, key: str) -> str | None:
    """How a control shows its automation: "on" while it plays, "off" when overridden."""
    if bridge.is_overridden(owner, key):
        return "off"
    return "on" if bridge.is_automated(owner, key) else None


def show_mixer_values(bridge: EngineBridge, owner: str, volume: ValueBox, pan: Knob, own: tuple[float, float]) -> None:
    """Volume and pan as they are heard: following their automation while it plays."""
    for widget, key, value in ((volume, MIXER_VOLUME, own[0]), (pan, MIXER_PAN, own[1])):
        state = automation_state(bridge, owner, key)
        widget.set_automation(state)
        if state == "on":
            value = bridge.current_value(owner, key)
        widget.setValue(value)


def paint_lane_headers(p: QPainter, rects: list[QRect], width: int) -> None:
    for rect in rects:
        p.fillRect(QRect(0, rect.top(), width, rect.height()), QColor(theme.PANEL))
        p.fillRect(QRect(0, rect.top(), width, 1), QColor(theme.GRID_BAR))


def volume_box(value: float = 0.0) -> ValueBox:
    return ValueBox(value, -70.0, 6.0, step=0.25, decimals=1, formatter=format_db, sample_text="-70.0 dB")


def color_swatch(color: str) -> QIcon:
    pixmap = QPixmap(14, 14)
    pixmap.fill(QColor(color))
    return QIcon(pixmap)


class TrackHeader(QWidget):
    def __init__(self, track_id: str, editor: ProjectEditor, selection: Selection, bridge: EngineBridge,
                 parent: QWidget | None = None):
        super().__init__(parent)
        self.track_id = track_id
        self.editor = editor
        self.project = editor.project
        self.selection = selection
        self.bridge = bridge
        self.number = 1
        self.row = Row(track_id, 0, self.project.track(track_id).height)
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
        self.automation = AutomationControls(track_id, editor, bridge, self)
        self.refresh()

    @property
    def track(self):
        return self.project.track(self.track_id)

    def refresh(self) -> None:
        track = self.track
        self.activator.set_checked_silently(not track.mute)
        self.solo.set_checked_silently(track.solo)
        self.refresh_mixer()
        self.automation.refresh()
        self.update()

    def refresh_mixer(self) -> None:
        track = self.track
        show_mixer_values(self.bridge, self.track_id, self.volume, self.pan, (track.volume_db, track.pan))

    @property
    def mixer_automated(self) -> bool:
        return self.volume.automation() == "on" or self.pan.automation() == "on"

    def set_row(self, row: Row) -> None:
        """Where the track's own lane and its automation lanes are (content coordinates)."""
        if row != self.row:
            self.row = row
            self._layout()
            self.update()

    def set_number(self, number: int) -> None:
        if number != self.number:
            self.number = number
            self.activator.setText(str(number))

    def resizeEvent(self, _event) -> None:
        self._layout()

    def _lane_rects(self) -> list[QRect]:
        return [QRect(0, lane.top - self.row.top, self.width(), lane.height) for lane in self.row.lanes]

    def _layout(self) -> None:
        w, h = self.width(), self.row.main_height
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
        main = QRect(10, CHOOSER_ROW, right - 10, CHOOSER_HEIGHT) if self.row.automation else None
        self.automation.place(main, [rect.adjusted(10, 0, -(w - right), 0) for rect in self._lane_rects()])

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        track = self.track
        selected = self.selection.track_id == self.track_id
        p.fillRect(self.rect(), QColor(theme.LANE_SELECTED if selected else theme.PANEL_ALT))
        paint_lane_headers(p, self._lane_rects(), self.width())
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
        """The bottom edge of the track's own lane (automation lanes below keep their height)."""
        return self.row.main_height - RESIZE_GRAB <= y < self.row.main_height

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        if self._in_resize_zone(event.position().y()):
            self._resize = (event.globalPosition().y(), self.row.main_height)  # as tall as it shows
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
        menu.addSeparator()
        if self.track.automation_view.shown:
            menu.addAction("Hide Automation", lambda: self.editor.hide_automation(self.track_id))
            menu.addAction("Show Automation in New Lane", lambda: self.editor.add_automation_lane(self.track_id))
        else:
            menu.addAction("Show Automation", lambda: self.editor.show_automation(self.track_id))
        overridden = self.bridge.is_overridden
        if any(overridden(self.track_id, key) for key in self.track.automation):
            menu.addAction("Re-Enable Automation", lambda: self.bridge.re_enable_automation(self.track_id))
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
        bridge.position_changed.connect(self._follow_automation)
        bridge.automation_state_changed.connect(self.refresh)
        editor.project.devices_changed.connect(self.refresh)
        bridge.plugin_params_rebuilt.connect(lambda track_id, _device_id: self.refresh(track_id))
        view.vscroll_changed.connect(self.relayout)

    def sync(self) -> None:
        """Create/remove headers to match the project, then position them."""
        wanted = [row.track_id for row in self.layout_model.rows]
        for track_id in list(self.headers):
            if track_id not in wanted:
                self.headers.pop(track_id).deleteLater()
        for track_id in wanted:
            if track_id not in self.headers:
                header = TrackHeader(track_id, self.editor, self.selection, self.bridge, self)
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
                header.set_row(row)
                header.set_number(index + 1)

    def _follow_automation(self) -> None:
        for header in self.headers.values():
            if header.mixer_automated:
                header.refresh_mixer()

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
    """The master's volume, pan and meter; and while its automation shows, its
    automation choosers (and those of the lanes below it)."""

    def __init__(self, editor: ProjectEditor, bridge: EngineBridge, selection: Selection | None = None,
                 parent: QWidget | None = None):
        super().__init__(parent)
        self.editor = editor
        self.project = editor.project
        self.bridge = bridge
        self.selection = selection
        self.main_height = MASTER_HEIGHT
        self.lanes: tuple[LaneRow, ...] = ()
        self.volume = volume_box(self.project.master.volume_db)
        self.volume.setParent(self)
        self.volume.setToolTip("Master Volume")
        self.pan = Knob(-1.0, 1.0, 0.0, default=0.0, bipolar=True, formatter=format_pan, parent=self)
        self.pan.setToolTip("Master Pan")
        self.meter = MeterWidget(self)
        self.automation = AutomationControls(MASTER, editor, bridge, self)
        self.volume.valueChanged.connect(lambda v, key: self.editor.set_track_param(MASTER, "volume_db", v, key))
        self.pan.valueChanged.connect(lambda v, key: self.editor.set_track_param(MASTER, "pan", v, key))
        for signal in (self.project.track_changed, self.project.devices_changed):
            signal.connect(lambda track_id: self.refresh() if track_id == MASTER else None)
        bridge.plugin_params_rebuilt.connect(lambda track_id, _device_id: self.refresh() if track_id == MASTER else None)
        self.project.reset.connect(self.refresh)
        if selection is not None:
            selection.changed.connect(self.update)
        bridge.automation_state_changed.connect(lambda owner: self.refresh() if owner == MASTER else None)
        bridge.position_changed.connect(self._follow_automation)
        bridge.meters_updated.connect(lambda: self.meter.set_levels(*bridge.meters.get(MASTER, (0.0, 0.0))))
        self.refresh()

    def refresh(self) -> None:
        self._show_mixer()
        self.automation.refresh()
        self.update()

    def _show_mixer(self) -> None:
        master = self.project.master
        show_mixer_values(self.bridge, MASTER, self.volume, self.pan, (master.volume_db, master.pan))

    def _follow_automation(self) -> None:
        if self.volume.automation() == "on" or self.pan.automation() == "on":
            self._show_mixer()

    def set_rows(self, main_height: int, lanes: tuple[LaneRow, ...]) -> None:
        self.main_height = main_height
        self.lanes = lanes
        self._layout()
        self.update()

    def _lane_rects(self) -> list[QRect]:
        return [QRect(0, lane.top, self.width(), lane.height) for lane in self.lanes]

    def resizeEvent(self, _event) -> None:
        self._layout()

    def _layout(self) -> None:
        w = self.width()
        self.meter.setGeometry(w - 12, 4, 8, MASTER_HEIGHT - 8)
        self.volume.setGeometry(w - 12 - 6 - 76, (MASTER_HEIGHT - 20) // 2, 76, 20)
        self.pan.setGeometry(w - 12 - 6 - 76 - 32, (MASTER_HEIGHT - 26) // 2, 26, 26)
        shown = self.project.master.automation_view.shown
        main = QRect(12, MASTER_HEIGHT + 6, w - 12 - 18, CHOOSER_HEIGHT) if shown else None
        self.automation.place(main, [rect.adjusted(12, 0, -18, 0) for rect in self._lane_rects()])

    @property
    def selected(self) -> bool:
        return self.selection is not None and self.selection.track_id == MASTER

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(theme.LANE_SELECTED if self.selected else theme.PANEL_ALT))
        paint_lane_headers(p, self._lane_rects(), self.width())
        p.fillRect(QRect(0, 0, 5, self.height()), QColor(theme.TEXT_DIM))
        p.fillRect(QRect(0, 0, self.width(), 1), QColor(theme.BORDER))
        p.fillRect(QRect(0, 0, 1, self.height()), QColor(theme.BORDER))
        p.setPen(QColor(theme.TEXT))
        p.setFont(theme.ui_font(9, bold=True))
        p.drawText(QRect(12, 0, 100, MASTER_HEIGHT), Qt.AlignmentFlag.AlignVCenter, "Master")

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton and self.selection is not None:
            self.selection.select_track(MASTER, focus_track=True)  # the device view shows its effects

    def contextMenuEvent(self, event: QContextMenuEvent) -> None:
        if self.selection is not None:
            self.selection.select_track(MASTER, focus_track=True)
        menu = QMenu(self)
        if self.project.master.automation_view.shown:
            menu.addAction("Hide Automation", lambda: self.editor.hide_automation(MASTER))
            menu.addAction("Show Automation in New Lane", lambda: self.editor.add_automation_lane(MASTER))
        else:
            menu.addAction("Show Automation", lambda: self.editor.show_automation(MASTER))
        if any(self.bridge.is_overridden(MASTER, key) for key in self.project.master.automation):
            menu.addAction("Re-Enable Automation", lambda: self.bridge.re_enable_automation(MASTER))
        menu.exec(event.globalPos())


class MasterLane(QWidget):
    """The master track's lane: grid, loop region and playhead; and the master's
    automation, in it and in lanes below it (see automation_lanes.py)."""

    def __init__(self, editor: ProjectEditor, view: ViewState, selection: Selection, bridge: EngineBridge,
                 parent: QWidget | None = None):
        super().__init__(parent)
        self.editor = editor
        self.project = editor.project
        self.view = view
        self.selection = selection
        self.bridge = bridge
        self.main_height = MASTER_HEIGHT
        self.lanes: tuple[LaneRow, ...] = ()
        self._playhead = 0.0
        self._gesture = None
        self._hover_point: Hover | None = None
        self.setMouseTracking(True)
        for signal in (view.changed, view.grid_changed, self.project.settings_changed, selection.changed,
                       self.project.automation_changed, self.project.automation_view_changed,
                       self.project.devices_changed, bridge.automation_state_changed):
            signal.connect(lambda *_args: self.update())

    def set_rows(self, main_height: int, lanes: tuple[LaneRow, ...]) -> None:
        self.main_height = main_height
        self.lanes = lanes
        self.update()

    def set_playhead(self, beat: float) -> None:
        for b in (self._playhead, beat):
            x = int(self.view.beat_to_x(b))
            self.update(QRect(x - 2, 0, 5, self.height()))
        self._playhead = beat

    def envelope_areas(self) -> list[EnvelopeArea]:
        view = self.project.master.automation_view
        if not view.shown:
            return []
        width = float(self.width())
        areas = []
        if view.key:
            areas.append(EnvelopeArea(MASTER, view.key, -1, QRectF(0, 2, width, self.main_height - 3)))
        for lane in self.lanes:
            areas.append(EnvelopeArea(MASTER, lane.key, lane.index, QRectF(0, lane.top, width, lane.height - 1)))
        return areas

    def paintEvent(self, event) -> None:
        p = QPainter(self)
        visible = QRectF(event.rect())
        p.fillRect(visible, QColor(theme.LANE))
        draw_grid(p, self.view, visible.left(), visible.right(), 1, self.height())
        draw_loop_region(p, self.view, visible.left(), visible.right(), 1, self.height())
        p.fillRect(QRectF(visible.left(), 0, visible.width(), 1), QColor(theme.BORDER))
        for lane in self.lanes:
            p.fillRect(QRectF(visible.left(), lane.top - 1, visible.width(), 1), QColor(theme.GRID_BAR))
        areas = self.envelope_areas()
        for area in areas:
            automation_lanes.draw_area(p, self, area, visible, self._hover_point, shade=False)
        automation_lanes.draw_range(p, self, areas, SELECTION_TINT)
        x = round(self.view.beat_to_x(self._playhead))
        p.fillRect(QRectF(x, 0, 1, self.height()), QColor(theme.PLAYHEAD))
        automation_lanes.draw_readout(p, self, self._gesture)

    # --- Mouse: automation -----------------------------------------------------------

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        area = automation_lanes.area_at(self.envelope_areas(), event.position())
        if area is not None:
            self._gesture = automation_lanes.press(self, area, event.position(), event.modifiers())
            self.update()

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._gesture is not None:
            self._gesture.move(event.position(), event.modifiers())
            self.update()
            return
        self._update_cursor(event.position(), event.modifiers())

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        gesture, self._gesture = self._gesture, None
        if gesture is not None:
            gesture.finish()
        self._update_cursor(event.position(), event.modifiers())
        self.update()

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        # Each click of a double-click counts on its own.
        self.mousePressEvent(event)

    def _update_cursor(self, pos: QPointF, mods) -> None:
        area = automation_lanes.area_at(self.envelope_areas(), pos)
        point, shape = automation_lanes.hover(self, area, pos, mods)
        if point != self._hover_point:
            self._hover_point = point
            self.update()
        self.setCursor(shape)

    def keyPressEvent(self, event) -> None:
        self._update_cursor(QPointF(self.mapFromGlobal(QCursor.pos())), event.modifiers())
        super().keyPressEvent(event)

    def keyReleaseEvent(self, event) -> None:
        self._update_cursor(QPointF(self.mapFromGlobal(QCursor.pos())), event.modifiers())
        super().keyReleaseEvent(event)

    def leaveEvent(self, _event) -> None:
        if self._hover_point is not None:
            self._hover_point = None
            self.update()

    def contextMenuEvent(self, event: QContextMenuEvent) -> None:
        pos = QPointF(event.pos())
        menu = QMenu(self)
        area = automation_lanes.area_at(self.envelope_areas(), pos)
        if area is not None:
            automation_lanes.add_menu_actions(self, area, pos, menu)
        elif self.project.master.automation_view.shown:
            menu.addAction("Hide Automation", lambda: self.editor.hide_automation(MASTER))
        else:
            menu.addAction("Show Automation", lambda: self.editor.show_automation(MASTER))
        menu.exec(event.globalPos())
