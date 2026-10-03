"""The mixer controls every strip's header has (a track's, a group's, a
return's, the master's; a rack chain's too): the volume box and pan knob, which
follow their automation while it plays and show it when taken hold of, and the
send knobs, one per return."""

from __future__ import annotations

from PySide6.QtCore import QEvent, QObject, QRect, Qt
from PySide6.QtWidgets import QLabel, QMenu, QWidget

from ... import theme
from ...audio.engine_bridge import EngineBridge
from ...model import automation as automation_model
from ...model.automation import MIXER_PAN, MIXER_VOLUME
from ...model.editor import ProjectEditor
from ...model.project import RoutingGraph, Send, feeds, routing_graph
from ...model.timebase import format_db, format_pan, parse_pan
from ..widgets import Knob, ValueBox

SEND_SLOT = 36  # a send knob and its letter, at most


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


def parse_db(text: str) -> float | None:
    try:
        return float(text.lower().replace("db", "").strip())
    except ValueError:
        return None


class SendControls(QObject):
    """A strip's send knobs (a track's, a group's or a return's), one per return,
    each with the return's letter. A knob shows its send's level (following its
    automation while that plays); turning it sets the send, making it if need be.
    Its context menu taps before or after the fader, or removes the send."""

    def __init__(self, owner: str, editor: ProjectEditor, bridge: EngineBridge, parent: QWidget):
        super().__init__(parent)
        self.owner = owner
        self.editor = editor
        self.project = editor.project
        self.bridge = bridge
        self.parent_widget = parent
        self.knobs: dict[str, tuple[QLabel, Knob]] = {}  # return id -> its letter and knob

    def sync(self) -> None:
        """A knob for each return, in their order."""
        wanted = [r.id for r in self.project.returns]
        for return_id in [r for r in self.knobs if r not in wanted]:
            for widget in self.knobs.pop(return_id):
                widget.deleteLater()
        for return_id in wanted:
            if return_id in self.knobs:
                continue
            label = QLabel(self.parent_widget)
            label.setAlignment(Qt.AlignmentFlag.AlignRight | Qt.AlignmentFlag.AlignVCenter)
            label.setFont(theme.ui_font(8, bold=True))
            knob = Knob(0.0, 1.0, 0.0, default=0.0, parser=lambda text: (
                None if (db := parse_db(text)) is None else automation_model.volume_to_normalized(db)),
                wheel=False, parent=self.parent_widget)
            knob.setMinimumSize(18, 18)
            knob.valueChanged.connect(lambda value, key, r=return_id: self.editor.set_send(
                self.owner, r, automation_model.normalized_to_volume(value), merge_key=key))
            knob.installEventFilter(_TouchFilter(self.editor, self.owner, automation_model.send_key(return_id), knob))
            knob.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
            knob.customContextMenuRequested.connect(lambda pos, r=return_id: self._menu(r, pos))
            self.knobs[return_id] = (label, knob)
        self.refresh()

    def place(self, rect: QRect | None) -> None:
        """Where the knobs go (None: hidden), side by side."""
        count = len(self.knobs)
        if rect is None or not count:
            for label, knob in self.knobs.values():
                label.hide()
                knob.hide()
            return
        slot = max(24, min(SEND_SLOT, rect.width() // count))
        size = max(16, min(rect.height(), slot - 12))
        for i, (label, knob) in enumerate(self.knobs.values()):
            x = rect.left() + i * slot
            visible = x + slot <= rect.right() + 1
            label.setGeometry(x, rect.top(), slot - size - 1, rect.height())
            knob.setGeometry(x + slot - size, rect.top() + (rect.height() - size) // 2, size, size)
            label.setVisible(visible)
            knob.setVisible(visible)

    def refresh(self, graph: RoutingGraph | None = None) -> None:
        """Values, automation, letters and which can be used, as they are now.
        `graph` is the project's routing graph, when the caller has it already."""
        if not self.project.has_owner(self.owner):
            return
        if graph is None:
            graph = routing_graph(self.project.tracks, self.project.returns)
        track = self.project.track(self.owner)
        for return_id, (label, knob) in self.knobs.items():
            if not self.project.has_return(return_id):
                continue
            letter = self.project.return_letter(return_id)
            send = track.sends.get(return_id, Send())
            key = automation_model.send_key(return_id)
            state = automation_state(self.bridge, self.owner, key)
            level = send.level_db
            if state == "on":
                level = self.bridge.current_value(self.owner, key)
            knob.set_automation(state)
            knob.setValue(automation_model.volume_to_normalized(level))
            usable = not feeds(graph, return_id, self.owner)
            knob.setEnabled(usable)
            label.setEnabled(usable)
            if label.text() != letter:
                label.setText(letter)
            color = theme.ACCENT if send.pre_fader and return_id in track.sends else theme.TEXT_DIM
            if label.property("sendColor") != color:  # a style sheet is slow to apply: only when it changes
                label.setProperty("sendColor", color)
                label.setStyleSheet(f"color: {color};")
            name = self.project.track(return_id).name
            if not usable:
                tip = f"Send {letter} ({name}): it feeds this track"
            else:
                tap = "before the fader" if send.pre_fader else "after the fader"
                tip = f"Send {letter} to {name}: {format_db(level)}, {tap} (right-click: pre/post-fader)"
            knob.set_formatter(lambda _v, t=tip: t)

    @property
    def automated(self) -> bool:
        return any(knob.automation() == "on" for _, knob in self.knobs.values())

    def _menu(self, return_id: str, pos) -> None:
        knob = self.knobs[return_id][1]
        if not knob.isEnabled():
            return
        track = self.project.track(self.owner)
        send = track.sends.get(return_id)
        menu = QMenu(self.parent_widget)
        pre = menu.addAction("Pre-Fader", lambda: self.editor.set_send(
            self.owner, return_id, pre_fader=not (send is not None and send.pre_fader)))
        pre.setCheckable(True)
        pre.setChecked(send is not None and send.pre_fader)
        remove = menu.addAction("Remove Send", lambda: self.editor.remove_send(self.owner, return_id))
        remove.setEnabled(send is not None)
        menu.addSeparator()
        menu.addAction("Show Automation", lambda: self.editor.show_automation(
            self.owner, automation_model.send_key(return_id)))
        menu.exec(knob.mapToGlobal(pos))
