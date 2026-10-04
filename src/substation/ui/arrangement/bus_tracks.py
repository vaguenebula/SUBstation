"""The master and the return tracks, shown apart below the tracks (the returns
above the master), as compact rows: each a header (MasterHeader,
ReturnHeader) and a lane for its automation (BusLane, MasterLane), as the
tracks have. Click the master's header (or a return's) to select it: the
device view shows its effects."""

from __future__ import annotations

from PySide6.QtCore import QPointF, QRect, QRectF, Qt
from PySide6.QtGui import QColor, QContextMenuEvent, QCursor, QMouseEvent, QPainter
from PySide6.QtWidgets import QApplication, QLineEdit, QMenu, QWidget

from ... import theme
from ...audio.engine_bridge import EngineBridge
from ...model.automation import MASTER
from ...model.editor import ProjectEditor
from ...model.project import TRACK_COLORS
from ..widgets import MeterWidget, ToggleButton
from . import automation_lanes
from .automation_header import CHOOSER_HEIGHT, AutomationControls
from .automation_lanes import EnvelopeArea, Hover
from .grid import draw_grid, draw_loop_region
from .lanes_canvas import SELECTION_TINT
from .mixer_controls import (
    SendControls,
    pan_knob,
    show_mixer_values,
    volume_box,
    watch_mixer_touch,
)
from .track_headers import (
    NAME_ROW,
    SNOWFLAKE,
    add_freeze_actions,
    color_swatch,
    paint_frozen,
    paint_lane_headers,
)
from .view_state import LaneRow, Selection, ViewState, automation_rows

MASTER_HEIGHT = 40
RETURN_HEIGHT = 52  # a return track's row: its name, then volume, pan and its sends


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
        self.pan = pan_knob(self)
        self.pan.setToolTip("Master Pan")
        self.meter = MeterWidget(self)
        self.automation = AutomationControls(MASTER, editor, bridge, self)
        watch_mixer_touch(editor, MASTER, self.volume, self.pan)
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


def return_rows(project, return_id: str) -> tuple[int, tuple[LaneRow, ...]]:
    """A return's own row (taller while its automation shows, for the choosers) and its lanes below it."""
    return automation_rows(project.automation_view(return_id), 0, RETURN_HEIGHT,
                           RETURN_HEIGHT + CHOOSER_HEIGHT + 10)


class ReturnHeader(QWidget):
    """A return track's header, in the returns' rows above the master: its letter
    and name, activator, solo, volume, pan, its sends (to the other returns) and
    meter; and while its automation shows, its automation choosers (and those of
    the lanes below it). Click it to select it: the device view shows its effects."""

    def __init__(self, return_id: str, editor: ProjectEditor, bridge: EngineBridge, selection: Selection,
                 parent: QWidget | None = None):
        super().__init__(parent)
        self.track_id = return_id
        self.editor = editor
        self.project = editor.project
        self.bridge = bridge
        self.selection = selection
        self.main_height = RETURN_HEIGHT
        self.lanes: tuple[LaneRow, ...] = ()
        self._rename: QLineEdit | None = None
        self.activator = ToggleButton("A", role="activator", tooltip="Track Activator (unmute)", parent=self)
        self.solo = ToggleButton("S", role="solo", tooltip="Solo: keeps what sends to it sending; "
                                 "Ctrl-click to solo it along with others", parent=self)
        self.volume = volume_box()
        self.volume.setParent(self)
        self.volume.setToolTip("Return Volume")
        self.pan = pan_knob(self)
        self.pan.setToolTip("Return Pan")
        self.meter = MeterWidget(self)
        self.sends = SendControls(return_id, editor, bridge, self)
        self.automation = AutomationControls(return_id, editor, bridge, self)
        watch_mixer_touch(editor, return_id, self.volume, self.pan)
        self.activator.toggled.connect(lambda on: self.editor.set_track_param(self.track_id, "mute", not on))
        self.solo.clicked.connect(self._solo_clicked)
        self.volume.valueChanged.connect(lambda v, key: self.editor.set_track_param(self.track_id, "volume_db", v, key))
        self.pan.valueChanged.connect(lambda v, key: self.editor.set_track_param(self.track_id, "pan", v, key))
        # Methods, not lambdas: the header can go (its return deleted) before the signals do.
        selection.changed.connect(self._repaint)
        bridge.meters_updated.connect(self._update_meter)
        bridge.position_changed.connect(self._follow_automation)
        bridge.automation_state_changed.connect(self._automation_state_changed)
        self.project.freeze_changed.connect(self._repaint)
        self.refresh()

    def _repaint(self, *_args) -> None:
        self.update()

    def _update_meter(self) -> None:
        self.meter.set_levels(*self.bridge.meters.get(self.track_id, (0.0, 0.0)))

    def _automation_state_changed(self, owner: str) -> None:
        if owner == self.track_id:
            self.refresh()

    @property
    def track(self):
        return self.project.track(self.track_id)

    @property
    def letter(self) -> str:
        return self.project.return_letter(self.track_id)

    def refresh(self) -> None:
        if not self.project.has_return(self.track_id):
            return
        track = self.track
        self.activator.setText(self.letter)
        self.activator.set_checked_silently(not track.mute)
        self.solo.set_checked_silently(track.solo)
        self.sends.sync()
        self._show_mixer()
        self._layout()
        self.automation.refresh()
        self.update()

    def _show_mixer(self) -> None:
        track = self.track
        show_mixer_values(self.bridge, self.track_id, self.volume, self.pan, (track.volume_db, track.pan))
        self.sends.refresh()

    def _follow_automation(self) -> None:
        if not self.project.has_return(self.track_id):
            return
        if self.volume.automation() == "on" or self.pan.automation() == "on" or self.sends.automated:
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
        meter_w = 8
        self.meter.setGeometry(w - meter_w - 4, 4, meter_w, RETURN_HEIGHT - 9)
        right = w - meter_w - 10
        self.solo.setGeometry(right - 22, 4, 22, 17)
        self.activator.setGeometry(right - 22 - 30, 4, 28, 17)
        left = 10
        self.volume.setGeometry(left, NAME_ROW + 4, 72, 20)
        self.pan.setGeometry(left + 77, NAME_ROW + 1, 26, 26)
        self.sends.place(QRect(left + 108, NAME_ROW + 3, right - left - 108, 22))
        shown = self.track.automation_view.shown if self.project.has_return(self.track_id) else False
        main = QRect(left, RETURN_HEIGHT + 4, right - left, CHOOSER_HEIGHT) if shown else None
        self.automation.place(main, [rect.adjusted(left, 0, -(w - right), 0) for rect in self._lane_rects()])

    @property
    def selected(self) -> bool:
        return self.track_id in self.selection.track_ids

    def paintEvent(self, _event) -> None:
        if not self.project.has_return(self.track_id):
            return
        p = QPainter(self)
        track = self.track
        p.fillRect(self.rect(), QColor(theme.LANE_SELECTED if self.selected else theme.PANEL_ALT))
        paint_lane_headers(p, self._lane_rects(), self.width())
        p.fillRect(QRect(0, 0, 5, self.height()), QColor(track.color))
        p.fillRect(QRect(0, 0, self.width(), 1), QColor(theme.BORDER))
        p.fillRect(QRect(0, 0, 1, self.height()), QColor(theme.BORDER))
        left = 10 + (SNOWFLAKE + 3 if paint_frozen(p, self.project, self.track_id, 10) else 0)
        if self._rename is None:
            p.setPen(QColor(theme.TEXT if not track.mute else theme.TEXT_DIM))
            p.setFont(theme.ui_font(9, bold=True))
            name_rect = QRect(left, 3, self.activator.x() - left - 4, NAME_ROW - 4)
            name = p.fontMetrics().elidedText(track.name, Qt.TextElideMode.ElideRight, name_rect.width())
            p.drawText(name_rect, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft, name)

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            mode = "toggle" if event.modifiers() & Qt.KeyboardModifier.ControlModifier else ""
            self.selection.select_track(self.track_id, focus_track=True, mode=mode)

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        if event.position().y() < NAME_ROW:
            self.start_rename()

    def _solo_clicked(self, on: bool) -> None:
        """As a track's: soloing it unsoloes the others unless Ctrl is held."""
        selected = self.selection.track_ids
        tracks = selected if self.track_id in selected else (self.track_id,)
        exclusive = not QApplication.keyboardModifiers() & Qt.KeyboardModifier.ControlModifier
        if exclusive and not on:
            tracks = [t.id for t in self.project.senders()]
        self.editor.solo_tracks(tracks, on, exclusive=exclusive)
        self.solo.set_checked_silently(self.track.solo)

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
        selected = self.selection.track_ids
        if self.track_id not in selected:
            self.selection.select_track(self.track_id, focus_track=True)
            selected = (self.track_id,)
        menu = QMenu(self)
        menu.addAction("Rename", self.start_rename)
        colors = menu.addMenu("Color")
        for color in TRACK_COLORS:
            colors.addAction(color_swatch(color), color, lambda c=color: self.editor.set_track_color(self.track_id, c))
        menu.addSeparator()
        insert = menu.addAction("Insert Return Track", lambda: self.editor.add_return_track(
            self.project.return_index(self.track_id) + 1))
        insert.setShortcut("Ctrl+Alt+T")
        insert.setShortcutVisibleInContextMenu(True)
        menu.addAction("Delete Return Track" if len(selected) == 1 else "Delete Tracks",
                       lambda: self.editor.delete_tracks(list(selected)))
        menu.addSeparator()
        add_freeze_actions(menu, self.editor, self.bridge, self.selection, list(selected))
        menu.addSeparator()
        if self.track.automation_view.shown:
            menu.addAction("Hide Automation", lambda: self.editor.hide_automation(self.track_id))
            menu.addAction("Show Automation in New Lane", lambda: self.editor.add_automation_lane(self.track_id))
        else:
            menu.addAction("Show Automation", lambda: self.editor.show_automation(self.track_id))
        if any(self.bridge.is_overridden(self.track_id, key) for key in self.track.automation):
            menu.addAction("Re-Enable Automation", lambda: self.bridge.re_enable_automation(self.track_id))
        menu.exec(event.globalPos())


class BusLane(QWidget):
    """A lane of a strip without clips, the master's or a return track's: grid,
    loop region and playhead; and its automation, in it and in lanes below it
    (see automation_lanes.py)."""

    def __init__(self, owner: str, editor: ProjectEditor, view: ViewState, selection: Selection,
                 bridge: EngineBridge, parent: QWidget | None = None):
        super().__init__(parent)
        self.owner = owner
        self.editor = editor
        self.project = editor.project
        self.view = view
        self.selection = selection
        self.bridge = bridge
        self.main_height = MASTER_HEIGHT if owner == MASTER else RETURN_HEIGHT
        self.lanes: tuple[LaneRow, ...] = ()
        self._playhead: float | None = None
        self._gesture = None
        self._hover_point: Hover | None = None
        self.setMouseTracking(True)
        for signal in (view.changed, view.grid_changed, self.project.settings_changed, selection.changed,
                       self.project.automation_changed, self.project.automation_view_changed,
                       self.project.devices_changed, bridge.automation_state_changed):
            signal.connect(self._repaint)  # (a method: a return's lane can go before the signals do)

    def _repaint(self, *_args) -> None:
        self.update()

    def set_rows(self, main_height: int, lanes: tuple[LaneRow, ...]) -> None:
        self.main_height = main_height
        self.lanes = lanes
        self.update()

    def set_playhead(self, beat: float | None) -> None:
        """None hides it (playback stopped)."""
        for b in (self._playhead, beat):
            if b is not None:
                x = int(self.view.beat_to_x(b))
                self.update(QRect(x - 2, 0, 5, self.height()))
        self._playhead = beat

    def envelope_areas(self) -> list[EnvelopeArea]:
        if not self.project.has_owner(self.owner):
            return []
        view = self.project.automation_view(self.owner)
        if not view.shown:
            return []
        width = float(self.width())
        areas = []
        if view.key:
            areas.append(EnvelopeArea(self.owner, view.key, -1, QRectF(0, 2, width, self.main_height - 3)))
        for lane in self.lanes:
            areas.append(EnvelopeArea(self.owner, lane.key, lane.index, QRectF(0, lane.top, width, lane.height - 1)))
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
        if self._playhead is not None:
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
        elif self.project.automation_view(self.owner).shown:
            menu.addAction("Hide Automation", lambda: self.editor.hide_automation(self.owner))
        else:
            menu.addAction("Show Automation", lambda: self.editor.show_automation(self.owner))
        menu.exec(event.globalPos())


class MasterLane(BusLane):
    """The master track's lane (see BusLane)."""

    def __init__(self, editor: ProjectEditor, view: ViewState, selection: Selection, bridge: EngineBridge,
                 parent: QWidget | None = None):
        super().__init__(MASTER, editor, view, selection, bridge, parent)
