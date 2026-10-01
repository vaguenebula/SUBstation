"""Track headers (right of the lanes, as in Ableton): name, activator, solo,
arm, volume, pan, input (audio channels, or a MIDI track's MIDI input) and
monitoring, meter, and while a track's automation shows, its automation
choosers (and those of the lanes below it). Plus the master track's header and
lane, which show the master's automation likewise. Volume and pan follow their
automation while it plays. Click the master's header to select it: the device
view shows its effects.

Group tracks have a header too (no arm or input: they record nothing). Every
header has a fold button: a track's (a triangle in a circle) folds it to its
name row; a group's (bars in a circle, filled while folded) hides its tracks.
Folded, neither shows its automation. The tracks in a group are indented under
it, with a band in the colour of each group they are in. Drag headers to move tracks:
between two tracks, or onto a group's header to put them in it."""

from __future__ import annotations

from PySide6.QtCore import QEvent, QObject, QPointF, QRect, QRectF, Qt
from PySide6.QtGui import (
    QColor,
    QContextMenuEvent,
    QCursor,
    QIcon,
    QMouseEvent,
    QPainter,
    QPainterPath,
    QPen,
    QPixmap,
    QWheelEvent,
)
from PySide6.QtWidgets import QApplication, QLineEdit, QMenu, QPushButton, QWidget

from ... import theme
from ...audio.engine_bridge import EngineBridge
from ...model.automation import MASTER, MIXER_PAN, MIXER_VOLUME
from ...model.editor import ProjectEditor
from ...model.project import MAX_TRACK_HEIGHT, MIN_TRACK_HEIGHT, TRACK_COLORS, MidiInput
from ...model.timebase import format_db, format_pan, parse_pan
from ..widgets import Knob, MeterWidget, ToggleButton, ValueBox
from . import automation_lanes
from .automation_header import CHOOSER_HEIGHT, AutomationControls
from .automation_lanes import EnvelopeArea, Hover
from .grid import draw_grid, draw_loop_region
from .lanes_canvas import SELECTION_TINT, resize_track_by_wheel
from .view_state import LaneRow, Row, Selection, TrackLayout, ViewState

RESIZE_GRAB = 4
NAME_ROW = 22
INDENT = 6  # per group a track is in: the group's colour band
FOLD_WIDTH = 14  # a group's fold triangle, before its name
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
    return ValueBox(value, -70.0, 6.0, step=0.25, decimals=1, formatter=format_db, sample_text="-70.0 dB",
                    default=0.0, wheel=False)


class _TouchFilter(QObject):
    """Pressing a mixer control shows its automation (clicking one, as in Ableton)."""

    def __init__(self, editor: ProjectEditor, owner: str, key: str, parent: QObject):
        super().__init__(parent)
        self.editor, self.owner, self.key = editor, owner, key

    def eventFilter(self, _obj: QObject, event: QEvent) -> bool:
        if event.type() in (QEvent.Type.MouseButtonPress, QEvent.Type.MouseButtonDblClick)                 and event.button() == Qt.MouseButton.LeftButton:
            self.editor.touch_parameter(self.owner, self.key)
        return False


def watch_mixer_touch(editor: ProjectEditor, owner: str, volume: ValueBox, pan: Knob) -> None:
    volume.installEventFilter(_TouchFilter(editor, owner, MIXER_VOLUME, volume))
    pan.installEventFilter(_TouchFilter(editor, owner, MIXER_PAN, pan))


def pan_knob(parent: QWidget) -> Knob:
    return Knob(-1.0, 1.0, 0.0, default=0.0, bipolar=True, formatter=format_pan, parser=parse_pan, wheel=False, parent=parent)


MONITOR_LABELS = {"in": "In", "auto": "Auto", "off": "Off"}
MONITOR_TIPS = {"in": "In: always hears its input, never its clips",
                "auto": "Auto: hears its input while armed, unless playing back",
                "off": "Off: never hears its input"}
# A MIDI track's clips play on while it hears its input (Auto), as in Ableton.
MIDI_MONITOR_TIPS = {**MONITOR_TIPS, "auto": "Auto: hears its input while armed, beside its clips"}


def input_label(channels: tuple[int, ...]) -> str:
    if not channels:
        return "No Input"
    return "In " + "/".join(str(c + 1) for c in channels)


def midi_input_label(midi_input: MidiInput | None) -> str:
    if midi_input is None:
        return "No Input"
    name = midi_input.device or "All Ins"
    return f"{name} · Ch {midi_input.channel}" if midi_input.channel else name


def input_choices(names: list[str]) -> list[tuple[str, tuple[int, ...]]]:
    """(label, channels) for a device's inputs: each one (mono), then each pair."""
    choices = [(f"{input_label((c,))}  ({name})", (c,)) for c, name in enumerate(names)]
    choices += [(input_label((c, c + 1)), (c, c + 1)) for c in range(0, len(names) - 1, 2)]
    return choices


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
        self._press: QPointF | None = None  # where a press that may start dragging the track was
        self._dragging = False
        self.setMouseTracking(True)

        self.activator = ToggleButton("1", role="activator", tooltip="Track Activator (unmute)", parent=self)
        self.solo = ToggleButton("S", role="solo", tooltip="Solo (S); Ctrl-click to solo it along with others",
                                 parent=self)
        self.arm = ToggleButton("●", role="arm", tooltip="Arm Recording; Ctrl-click to arm it along with others",
                                parent=self)
        self.input = QPushButton(parent=self)
        self.input.setProperty("role", "small")
        self.input.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.monitor = QPushButton(parent=self)
        self.monitor.setProperty("role", "small")
        self.monitor.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.volume = volume_box()
        self.volume.setParent(self)
        self.volume.setToolTip("Track Volume (drag; select and type a number; double-click to reset)")
        self.pan = pan_knob(self)
        self.meter = MeterWidget(self)

        self.activator.toggled.connect(lambda on: self.editor.set_track_param(self.track_id, "mute", not on))
        self.solo.clicked.connect(self._solo_clicked)
        self.arm.clicked.connect(self._arm_clicked)
        self.input.clicked.connect(self._choose_input)
        self.monitor.clicked.connect(self._choose_monitor)
        self.volume.valueChanged.connect(lambda v, key: self._mixer_changed("volume_db", self.volume, v, key))
        self.pan.valueChanged.connect(lambda v, key: self._mixer_changed("pan", self.pan, v, key))
        for widget in (self.volume, self.pan, self.activator, self.solo, self.arm, self.input, self.monitor,
                       self.meter):
            widget.installEventFilter(self)  # Alt+wheel over a control still resizes the track
        watch_mixer_touch(editor, track_id, self.volume, self.pan)
        self.automation = AutomationControls(track_id, editor, bridge, self)
        self.refresh()

    @property
    def track(self):
        return self.project.track(self.track_id)

    def refresh(self) -> None:
        track = self.track
        self.activator.set_checked_silently(not track.mute)
        self.solo.set_checked_silently(track.solo)
        self.arm.set_checked_silently(track.armed)
        self.solo.setToolTip("Solo (S): a group solos what is in it; Ctrl-click to solo it along with others"
                             if track.is_group else "Solo (S); Ctrl-click to solo it along with others")
        if track.is_group:
            pass  # nothing to record: no input
        elif track.is_midi:
            self.input.setText(midi_input_label(track.midi_input))
            self.input.setToolTip("MIDI input (the MIDI inputs on in Preferences, and a channel)")
        else:
            self.input.setText(input_label(track.input))
            self.input.setToolTip("Audio input (the audio device's channels)")
        self.monitor.setText(MONITOR_LABELS.get(track.monitor, "Auto"))
        tips = MIDI_MONITOR_TIPS if track.is_midi else MONITOR_TIPS
        self.monitor.setToolTip(f"Monitoring. {tips.get(track.monitor, '')}")
        self._layout()
        self.refresh_mixer()
        self.automation.refresh()
        self.update()

    def _mixer_changed(self, attr: str, widget, value: float, key: object) -> None:
        """Volume or pan changed here; on a selected track, every selected track follows
        (by the same amount when dragged, to the same value when typed or reset)."""
        selected = self.selection.track_ids
        if self.track_id not in selected or len(selected) < 2:
            self.editor.set_track_param(self.track_id, attr, value, key)
            return
        delta = value - getattr(self.track, attr)
        values = {}
        for track in self.project.tracks:
            if track.id in selected:
                values[track.id] = (getattr(track, attr) + delta) if widget.relative else value
        self.editor.set_tracks_param(values, attr, key)

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

    @property
    def indent(self) -> int:
        return self.row.depth * INDENT

    def _name_left(self) -> int:
        return self.indent + 10 + FOLD_WIDTH

    def _fold_rect(self) -> QRect:
        return QRect(self.indent + 7, 3, FOLD_WIDTH, NAME_ROW - 4)

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
        arm_w = 20
        group = self.track.is_group
        self.arm.setGeometry(right - 18, 4, 18, 17)
        self.arm.setVisible(not group)
        self.solo.setGeometry(right - arm_w - 22, 4, 22, 17)
        self.activator.setGeometry(right - arm_w - 22 - 30, 4, 28, 17)
        second_row = h >= 48
        for widget in (self.volume, self.pan):
            widget.setVisible(second_row)
        for widget in (self.input, self.monitor):
            widget.setVisible(second_row and not group)
        # Everything below the name row starts after the group bands and ends where the
        # buttons above do; the monitor button is as wide as its label needs.
        left = 10 + self.indent
        if second_row:
            self.volume.setGeometry(left, NAME_ROW + 4, 72, 20)
            self.pan.setGeometry(left + 77, NAME_ROW + 1, 26, 26)
            monitor_w = max(40, self.monitor.sizeHint().width() + 4)
            self.monitor.setGeometry(right - monitor_w, NAME_ROW + 4, monitor_w, 20)
            input_left = left + 108
            self.input.setGeometry(input_left, NAME_ROW + 4, max(20, right - monitor_w - 4 - input_left), 20)
        main = QRect(left, CHOOSER_ROW, right - left, CHOOSER_HEIGHT) if self.row.automation else None
        self.automation.place(main, [rect.adjusted(left, 0, -(w - right), 0) for rect in self._lane_rects()])

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        track = self.track
        selected = self.track_id in self.selection.track_ids
        p.fillRect(self.rect(), QColor(theme.LANE_SELECTED if selected else theme.PANEL_ALT))
        paint_lane_headers(p, self._lane_rects(), self.width())
        # A band for each group it is in (outermost first), then its own colour.
        for depth, group_id in enumerate(reversed(self.project.ancestors(self.track_id))):
            p.fillRect(QRect(depth * INDENT, 0, INDENT - 1, self.height()), QColor(self.project.track(group_id).color))
        p.fillRect(QRect(self.indent, 0, 5, self.height() - 1), QColor(track.color))
        self._paint_fold(p, track.is_group, track.folded)
        if self._rename is None:
            p.setPen(QColor(theme.TEXT if not track.mute else theme.TEXT_DIM))
            p.setFont(theme.ui_font(9, bold=selected or track.is_group))
            left = self._name_left()
            name_rect = QRect(left, 3, self.activator.x() - left - 4, NAME_ROW - 4)
            name = p.fontMetrics().elidedText(track.name, Qt.TextElideMode.ElideRight, name_rect.width())
            p.drawText(name_rect, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft, name)
        p.fillRect(QRect(0, self.height() - 1, self.width(), 1), QColor(theme.BORDER))
        p.fillRect(QRect(0, 0, 1, self.height()), QColor(theme.BORDER))

    def _paint_fold(self, p: QPainter, group: bool, folded: bool) -> None:
        """The fold button, in a circle. A track's: a triangle, pointing right while
        folded, down while open. A group's: three bars (its tracks), the circle
        filled while folded (its tracks tucked away)."""
        rect = QRectF(self._fold_rect())
        c = rect.center()
        radius = 5.5
        ink = QColor(theme.TEXT)
        p.save()
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        p.setPen(QPen(ink, 1.2))
        p.setBrush(ink if group and folded else Qt.BrushStyle.NoBrush)
        p.drawEllipse(c, radius, radius)
        if group:
            bars = QColor(theme.PANEL_ALT) if folded else ink
            for dy in (-2.5, 0.0, 2.5):
                half = 2.8 if dy == 0.0 else 2.2
                p.fillRect(QRectF(c.x() - half, c.y() + dy - 0.6, 2 * half, 1.2), bars)
        else:
            path = QPainterPath()
            if folded:
                path.moveTo(c.x() - 1.5, c.y() - 3.0)
                path.lineTo(c.x() + 2.5, c.y())
                path.lineTo(c.x() - 1.5, c.y() + 3.0)
            else:
                path.moveTo(c.x() - 3.0, c.y() - 1.5)
                path.lineTo(c.x() + 3.0, c.y() - 1.5)
                path.lineTo(c.x(), c.y() + 2.5)
            path.closeSubpath()
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(ink)
            p.drawPath(path)
        p.restore()

    def toggle_fold(self) -> None:
        """Fold or unfold it; when it is one of several selected tracks, they all
        take its new state."""
        folded = not self.track.folded
        for track_id in self.dragged_tracks():
            self.editor.set_folded(track_id, folded)

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
        """The bottom edge of the track's own lane (automation lanes below keep their
        height). A folded track keeps the height it had: it can't be resized."""
        if self.row.folded:
            return False
        return self.row.main_height - RESIZE_GRAB <= y < self.row.main_height

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() != Qt.MouseButton.LeftButton:
            return
        if self._in_resize_zone(event.position().y()):
            self._resize = (event.globalPosition().y(), self.row.main_height)  # as tall as it shows
        elif self._fold_rect().contains(event.position().toPoint()):
            self.toggle_fold()
        else:
            mods = event.modifiers()
            mode = ("toggle" if mods & Qt.KeyboardModifier.ControlModifier
                    else "range" if mods & Qt.KeyboardModifier.ShiftModifier else "")
            if not (mode == "" and self.track_id in self.selection.track_ids and len(self.selection.track_ids) > 1):
                self.selection.select_track(self.track_id, focus_track=True, mode=mode,
                                            order=[t.id for t in self.project.tracks])
            self._press = event.position()  # dragging moves the track (the selected tracks)

    def dragged_tracks(self) -> list[str]:
        """What dragging this header moves: the selected tracks if it is one of them."""
        selected = [t for t in self.selection.track_ids if self.project.has_track(t)]
        return selected if self.track_id in selected else [self.track_id]

    def _solo_clicked(self, on: bool) -> None:
        """Soloing a track unsoloes the others, and unsoloing one unsoloes them
        all, unless Ctrl is held. Clicking a selected track's solo acts on all
        the selected tracks."""
        selected = self.selection.track_ids
        tracks = selected if self.track_id in selected else (self.track_id,)
        exclusive = not QApplication.keyboardModifiers() & Qt.KeyboardModifier.ControlModifier
        if exclusive and not on:
            tracks = [t.id for t in self.project.tracks]
        self.editor.solo_tracks(tracks, on, exclusive=exclusive)
        self.solo.set_checked_silently(self.track.solo)

    def _arm_clicked(self, on: bool) -> None:
        """Arming a track disarms the others, unless Ctrl is held (as in Ableton).
        Clicking a selected track's arm acts on all the selected tracks."""
        selected = self.selection.track_ids
        tracks = selected if self.track_id in selected else (self.track_id,)
        exclusive = not QApplication.keyboardModifiers() & Qt.KeyboardModifier.ControlModifier
        self.editor.arm_tracks(tracks, on, exclusive=exclusive)
        self.arm.set_checked_silently(self.track.armed)
        if on and not self.track.has_input:
            kind = "MIDI input" if self.track.is_midi else "input"
            self.bridge.status_message.emit(f"{self.track.name} has no {kind}: choose one to record.")

    def input_menu(self) -> QMenu:
        if self.track.is_midi:
            return self.midi_input_menu()
        menu = QMenu(self)
        current = self.track.input
        none = menu.addAction("No Input", lambda: self.editor.set_track_input(self.track_id, ()))
        none.setCheckable(True)
        none.setChecked(not current)
        names = self.bridge.input_names()
        if not names:
            menu.addAction("The audio device has no inputs (choose an ASIO driver)").setEnabled(False)
        for label, channels in input_choices(names):
            if len(channels) == 2 and len(names) > 2 and channels == (0, 1):
                menu.addSeparator()
            action = menu.addAction(label, lambda c=channels: self.editor.set_track_input(self.track_id, c))
            action.setCheckable(True)
            action.setChecked(channels == current)
        return menu

    def midi_input_menu(self) -> QMenu:
        """No input, every input or one (those connected, and the one chosen if it
        isn't), and in a submenu the channel."""
        menu = QMenu(self)
        current = self.track.midi_input
        channel = current.channel if current is not None else 0

        def choose(device: str | None) -> None:
            midi_input = None if device is None else MidiInput(device, channel)
            self.editor.set_track_midi_input(self.track_id, midi_input)

        def add(label: str, device: str | None) -> None:
            action = menu.addAction(label, lambda: choose(device))
            action.setCheckable(True)
            action.setChecked(current == MidiInput(device, channel) if device is not None else current is None)

        add("No Input", None)
        add("All Ins", "")
        names = self.bridge.midi_input_choices()
        if current is not None and current.device and current.device not in names:
            names.append(current.device)
        if names:
            menu.addSeparator()
        for name in names:
            connected = name in self.bridge.midi_input_choices()
            add(name if connected else f"{name} (not connected)", name)
        if not names:
            menu.addAction("No MIDI input is connected").setEnabled(False)
        menu.addSeparator()
        channels = menu.addMenu(f"Channel: {channel or 'All'}")
        channels.setEnabled(current is not None)
        for number in range(17):
            action = channels.addAction(
                "All Channels" if number == 0 else f"Channel {number}",
                lambda n=number: self.editor.set_track_midi_input(
                    self.track_id, MidiInput(current.device if current else "", n)))
            action.setCheckable(True)
            action.setChecked(number == channel)
            if number == 0:
                channels.addSeparator()
        return menu

    def _choose_input(self) -> None:
        self.input_menu().exec(self.input.mapToGlobal(self.input.rect().bottomLeft()))

    def monitor_menu(self) -> QMenu:
        menu = QMenu(self)
        tips = MIDI_MONITOR_TIPS if self.track.is_midi else MONITOR_TIPS
        for mode in ("in", "auto", "off"):
            action = menu.addAction(tips[mode], lambda m=mode: self.editor.set_track_monitor(self.track_id, m))
            action.setCheckable(True)
            action.setChecked(self.track.monitor == mode)
        return menu

    def _choose_monitor(self) -> None:
        self.monitor_menu().exec(self.monitor.mapToGlobal(self.monitor.rect().bottomLeft()))

    def mouseMoveEvent(self, event: QMouseEvent) -> None:
        if self._resize:
            start_y, start_h = self._resize
            height = int(start_h + event.globalPosition().y() - start_y)
            self.editor.set_track_height(self.track_id, max(MIN_TRACK_HEIGHT, min(MAX_TRACK_HEIGHT, height)))
            return
        column = self.parentWidget()
        if self._press is not None:  # the left button is down (pressed here)
            if not self._dragging and (event.position() - self._press).manhattanLength() >= \
                    QApplication.startDragDistance():
                self._dragging = True
                self.setCursor(Qt.CursorShape.ClosedHandCursor)
            if self._dragging and isinstance(column, TrackHeaderColumn):
                column.drag_tracks(self.dragged_tracks(), column.mapFromGlobal(event.globalPosition().toPoint()).y())
            return
        resize = self._in_resize_zone(event.position().y())
        self.setCursor(Qt.CursorShape.SplitVCursor if resize else Qt.CursorShape.ArrowCursor)

    def mouseReleaseEvent(self, event: QMouseEvent) -> None:
        self._resize = None
        dragging, self._dragging = self._dragging, False
        pressed, self._press = self._press, None
        column = self.parentWidget()
        if dragging and isinstance(column, TrackHeaderColumn):
            self.setCursor(Qt.CursorShape.ArrowCursor)
            column.drop_tracks(self.dragged_tracks(), column.mapFromGlobal(event.globalPosition().toPoint()).y())
        elif pressed is not None and self.track_id in self.selection.track_ids and len(self.selection.track_ids) > 1 \
                and not event.modifiers() & (Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.ShiftModifier):
            # A click (not a drag) on one of several selected tracks selects just it.
            self.selection.select_track(self.track_id, focus_track=True)

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        if self._fold_rect().contains(event.position().toPoint()):
            self.toggle_fold()  # each click of a double-click counts
        elif event.position().y() < NAME_ROW:
            self.start_rename()

    def start_rename(self) -> None:
        if self._rename:
            return
        editor = QLineEdit(self.track.name, self)
        left = self._name_left() - 2
        editor.setGeometry(left, 2, self.activator.x() - left - 4, NAME_ROW - 2)
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
        index, parent = self.editor.insertion_point(self.track_id)
        menu.addAction("Insert Audio Track", lambda: self.editor.add_audio_track(index, parent=parent))
        menu.addAction("Insert MIDI Track", lambda: self.editor.add_midi_track(index, parent=parent))
        menu.addAction("Delete Track" if len(selected) == 1 else "Delete Tracks",
                       lambda: self.editor.delete_tracks(list(selected)))
        menu.addSeparator()
        group = menu.addAction("Group Tracks", lambda: self._group(list(selected)))
        group.setShortcut("Ctrl+G")  # (as a tip: the window's action handles the key)
        group.setShortcutVisibleInContextMenu(True)
        groups = [t for t in selected if self.project.track(t).is_group]
        if groups:
            ungroup = menu.addAction("Ungroup Tracks", lambda: self.editor.ungroup(groups))
            ungroup.setShortcut("Ctrl+Shift+G")
            ungroup.setShortcutVisibleInContextMenu(True)
        folding = [self.project.track(t) for t in self.dragged_tracks()]
        kind = ("Groups" if all(t.is_group for t in folding) else "Tracks") if len(folding) > 1             else "Group" if self.track.is_group else "Track"
        menu.addAction(f"Unfold {kind}" if self.track.folded else f"Fold {kind}", self.toggle_fold)
        if self.track.parent is not None:
            menu.addAction("Move Out of Group", lambda: self.editor.move_tracks(
                list(selected), self.project.subtree_end(self.project.track_index(self.track.parent)),
                self.project.track(self.track.parent).parent))
        menu.addSeparator()
        if self.track.folded:
            pass  # its automation doesn't show while folded: unfold it first
        elif self.track.automation_view.shown:
            menu.addAction("Hide Automation", lambda: self.editor.hide_automation(self.track_id))
            menu.addAction("Show Automation in New Lane", lambda: self.editor.add_automation_lane(self.track_id))
        else:
            menu.addAction("Show Automation", lambda: self.editor.show_automation(self.track_id))
        overridden = self.bridge.is_overridden
        if any(overridden(self.track_id, key) for key in self.track.automation):
            menu.addAction("Re-Enable Automation", lambda: self.bridge.re_enable_automation(self.track_id))
        menu.exec(event.globalPos())

    def _group(self, track_ids: list[str]) -> None:
        group = self.editor.group_tracks(track_ids)
        if group is not None:
            self.selection.select_track(group.id, focus_track=True)


class _DropMarker(QWidget):
    """Where dragged tracks would go: a line between tracks, or a frame around
    the group they would go into."""

    def __init__(self, parent: QWidget):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents)
        self.into = False
        self.hide()

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        color = QColor(theme.ACCENT)
        if self.into:
            p.setPen(QPen(color, 2))
            p.drawRect(QRectF(self.rect()).adjusted(1, 1, -1, -1))
        else:
            p.fillRect(self.rect(), color)


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
        self._marker = _DropMarker(self)
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
                header.setVisible(not row.hidden)  # in a folded group
                header.setGeometry(0, row.top - self.view.scroll_y, self.width(), row.height)
                header.set_row(row)
                header.set_number(index + 1)
                header.update()  # its group bands follow the groups
        self._marker.raise_()

    # --- Dragging tracks ----------------------------------------------------------

    def drop_target(self, track_ids: list[str], y: float) -> tuple[int, str | None, str | None, int] | None:
        """Where tracks dropped at `y` go: (index, group, the group shown as
        taking them or None, y of the line between tracks). None: nowhere they can go.
        Onto the middle of a group's header: into it, last; on the upper half of
        a header: before it, in its group; on the lower half: after it (into an
        open group, first)."""
        project = self.editor.project
        content_y = y + self.view.scroll_y
        target = None
        for index, row in enumerate(self.layout_model.rows):
            if row.hidden or content_y >= row.bottom:
                continue
            track = project.tracks[index]
            offset = content_y - row.top
            if track.is_group and row.main_height * 0.25 <= offset <= row.main_height * 0.75:
                target = (project.subtree_end(index), track.id, track.id, row.top)
            elif offset < row.main_height / 2:
                target = (index, track.parent, None, row.top)
            else:
                after = project.subtree_end(index) if track.is_group and track.folded else index + 1
                target = (after, project.parent_at(after), None, row.bottom)
            break
        if target is None:  # below the tracks: last, in no group
            target = (len(project.tracks), None, None, self.layout_model.total_height)
        index, parent, _, _ = target
        return target if self.editor.can_move_tracks(track_ids, index, parent) else None

    def drag_tracks(self, track_ids: list[str], y: float) -> None:
        """Tracks being dragged over the headers: show where they would go."""
        target = self.drop_target(track_ids, y)
        if target is None:
            self._marker.hide()
            return
        _, _, into, line = target
        if into is not None:
            row = self.layout_model.row_for(into)
            self._marker.into = True
            self._marker.setGeometry(0, row.top - self.view.scroll_y, self.width(), row.main_height)
        else:
            self._marker.into = False
            self._marker.setGeometry(0, line - self.view.scroll_y - 1, self.width(), 3)
        self._marker.show()
        self._marker.raise_()
        self._marker.update()

    def drop_tracks(self, track_ids: list[str], y: float) -> None:
        self._marker.hide()
        target = self.drop_target(track_ids, y)
        if target is not None:
            self.editor.move_tracks(track_ids, target[0], target[1])

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
        self._playhead: float | None = None
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

    def set_playhead(self, beat: float | None) -> None:
        """None hides it (playback stopped)."""
        for b in (self._playhead, beat):
            if b is not None:
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
        elif self.project.master.automation_view.shown:
            menu.addAction("Hide Automation", lambda: self.editor.hide_automation(MASTER))
        else:
            menu.addAction("Show Automation", lambda: self.editor.show_automation(MASTER))
        menu.exec(event.globalPos())
