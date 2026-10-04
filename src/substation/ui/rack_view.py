"""What a rack shows in the device view (device_panel.RackWidget): its macros,
and its chains with their mixers.

Macros: eight knobs, each turning the parameters mapped to it (right-click a
device's parameter in the rack to map it; right-click a macro to see what it
moves, or unmap it). Chains: a row each, with its activator (mute), name,
solo, volume, pan and meter, as a track header has. Click a chain to show its
devices beside the rack; double-click its name to rename it; right-click for
more (add, duplicate, delete). Solo among a rack's chains leaves the others out.
"""

from __future__ import annotations

from PySide6.QtCore import QPoint, Qt, Signal
from PySide6.QtGui import QColor, QContextMenuEvent, QFontMetrics, QMouseEvent, QPainter
from PySide6.QtWidgets import (
    QFrame,
    QGridLayout,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMenu,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QVBoxLayout,
    QWidget,
)

from .. import theme
from ..audio.engine_bridge import EngineBridge
from ..model import automation
from ..model.devices import device_name
from ..model.editor import ProjectEditor
from ..model.project import MACRO_COUNT, Chain, Device, iter_chains, macro_param
from .arrangement.mixer_controls import automation_state, pan_knob, volume_box
from .widgets import Knob, ToggleButton
from .widgets.meter import MeterWidget

MACRO_KNOB = 26
MACRO_WIDTH = 44
CHAIN_ROW_HEIGHT = 22


class MacroPanel(QWidget):
    """A rack's eight macros, four to a row."""

    def __init__(self, track_id: str, rack: Device, editor: ProjectEditor, bridge: EngineBridge,
                 parent: QWidget | None = None):
        super().__init__(parent)
        self.track_id = track_id
        self.rack_id = rack.id
        self.editor = editor
        self.bridge = bridge
        self.knobs: list[Knob] = []
        self.names: list[QLabel] = []
        grid = QGridLayout(self)
        grid.setContentsMargins(0, 0, 0, 0)
        grid.setHorizontalSpacing(4)
        grid.setVerticalSpacing(2)
        for index in range(MACRO_COUNT):
            cell = QWidget()
            cell.setFixedWidth(MACRO_WIDTH)
            column = QVBoxLayout(cell)
            column.setContentsMargins(0, 0, 0, 0)
            column.setSpacing(0)
            name = QLabel(f"Macro {index + 1}")
            name.setAlignment(Qt.AlignmentFlag.AlignCenter)
            name.setStyleSheet(f"color: {theme.TEXT_DIM}; font-size: 7pt;")
            knob = Knob(0.0, 1.0, rack.params.get(macro_param(index), 0.0), default=0.0,
                        formatter=lambda v: f"{v * 100:.0f} %")
            knob.setFixedSize(MACRO_KNOB, MACRO_KNOB)
            knob.valueChanged.connect(lambda v, key, i=index: editor.set_macro(track_id, self.rack_id, i, v, key))
            cell.setContextMenuPolicy(Qt.ContextMenuPolicy.CustomContextMenu)
            cell.customContextMenuRequested.connect(
                lambda pos, i=index, c=cell: self._menu(i, c.mapToGlobal(pos)))
            column.addWidget(name)
            column.addWidget(knob, 0, Qt.AlignmentFlag.AlignHCenter)
            self.names.append(name)
            self.knobs.append(knob)
            grid.addWidget(cell, index // 4, index % 4, Qt.AlignmentFlag.AlignTop)
        self.refresh(rack)

    def _mapping_names(self, rack: Device, index: int) -> list[str]:
        project = self.editor.project
        names = []
        for mapping in rack.macros:
            if mapping.macro != index or not project.has_device(self.track_id, mapping.device_id):
                continue
            device = project.device(self.track_id, mapping.device_id)
            spec = self.bridge.param_spec(self.track_id, automation.device_key(device.id, mapping.param_id))
            names.append(f"{device_name(device)}: {spec.name if spec else mapping.param_id}")
        return names

    def refresh(self, rack: Device) -> None:
        for index, (knob, name) in enumerate(zip(self.knobs, self.names, strict=True)):
            knob.setValue(rack.params.get(macro_param(index), 0.0))
            mapped = self._mapping_names(rack, index)
            name.setStyleSheet(f"color: {theme.TEXT if mapped else theme.TEXT_DIM}; font-size: 7pt;")
            tip = "\n".join(mapped) if mapped else "Nothing mapped: right-click a parameter of a device in the rack"
            knob.setToolTip(f"Macro {index + 1}\n{tip}")

    def _menu(self, index: int, at: QPoint) -> None:
        project = self.editor.project
        rack = project.device(self.track_id, self.rack_id)
        menu = QMenu(self)
        mappings = [m for m in rack.macros if m.macro == index and project.has_device(self.track_id, m.device_id)]
        if not mappings:
            menu.addAction("Nothing mapped (right-click a parameter in the rack to map it)").setEnabled(False)
        for mapping, name in zip(mappings, self._mapping_names(rack, index), strict=True):
            menu.addAction(f"Unmap {name}", lambda m=mapping: self.editor.unmap_macro(
                self.track_id, self.rack_id, m.device_id, m.param_id))
        menu.exec(at)


class _ChainRow(QFrame):
    """A chain: its activator, name, solo, volume, pan and meter."""

    clicked = Signal(str)  # chain id

    def __init__(self, track_id: str, rack_id: str, chain: Chain, editor: ProjectEditor, bridge: EngineBridge,
                 parent: QWidget | None = None):
        super().__init__(parent)
        self.track_id = track_id
        self.rack_id = rack_id
        self.chain_id = chain.id
        self.editor = editor
        self.bridge = bridge
        self.selected = False
        self.setObjectName("chainRow")
        self.setFixedHeight(CHAIN_ROW_HEIGHT)
        row = QHBoxLayout(self)
        row.setContentsMargins(3, 1, 3, 1)
        row.setSpacing(3)
        self.activator = ToggleButton(role="activator", tooltip="Chain Activator (unmute)")
        self.activator.setFixedSize(14, 14)
        self.activator.toggled.connect(lambda on: editor.set_chain_param(track_id, self.chain_id, "mute", not on))
        self.name = QLabel()
        self.name.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        self.name.setMinimumWidth(30)
        self.solo = ToggleButton("S", role="solo", tooltip="Solo: only the soloed chains of the rack are heard")
        self.solo.setFixedSize(18, 16)
        self.solo.toggled.connect(lambda on: editor.set_chain_param(track_id, self.chain_id, "solo", on))
        self.volume = volume_box(chain.volume_db)
        self.volume.setFixedWidth(52)
        self.volume.valueChanged.connect(
            lambda v, key: editor.set_chain_param(track_id, self.chain_id, "volume_db", v, key))
        self.pan = pan_knob(self)
        self.pan.setFixedSize(18, 18)
        self.pan.valueChanged.connect(lambda v, key: editor.set_chain_param(track_id, self.chain_id, "pan", v, key))
        self.meter = MeterWidget()
        self.meter.setFixedWidth(6)
        self._editing: QLineEdit | None = None
        for widget in (self.activator, self.name, self.solo, self.volume, self.pan, self.meter):
            row.addWidget(widget, 1 if widget is self.name else 0)
        self.refresh(chain)

    def automation_keys(self) -> tuple[str, str]:
        return (automation.chain_key(self.rack_id, self.chain_id, automation.CHAIN_VOLUME),
                automation.chain_key(self.rack_id, self.chain_id, automation.CHAIN_PAN))

    def refresh(self, chain: Chain) -> None:
        self.activator.set_checked_silently(not chain.mute)
        self.solo.set_checked_silently(chain.solo)
        self.name.setText(QFontMetrics(self.name.font()).elidedText(chain.name, Qt.TextElideMode.ElideRight, 120))
        self.name.setToolTip(chain.name)
        self.refresh_automation(chain)

    def refresh_automation(self, chain: Chain | None = None) -> None:
        """Volume and pan as they are heard: following their automation while it plays."""
        chain = chain or self.editor.project.chain(self.track_id, self.chain_id)
        for widget, key, own in zip((self.volume, self.pan), self.automation_keys(), (chain.volume_db, chain.pan), strict=True):
            state = automation_state(self.bridge, self.track_id, key)
            widget.set_automation(state)
            value = self.bridge.current_value(self.track_id, key) if state == "on" else own
            widget.setValue(own if value is None else value)

    def set_selected(self, selected: bool) -> None:
        if selected != self.selected:
            self.selected = selected
            self.update()

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(theme.SURFACE_HOVER if self.selected else theme.SURFACE))
        if self.selected:
            p.fillRect(0, 0, 2, self.height(), QColor(theme.ACCENT))

    def mousePressEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self.clicked.emit(self.chain_id)
        event.accept()

    def mouseDoubleClickEvent(self, event: QMouseEvent) -> None:
        if event.button() == Qt.MouseButton.LeftButton and self.name.geometry().contains(event.position().toPoint()):
            self.start_rename()
        event.accept()

    def start_rename(self) -> None:
        """Rename it in place: Enter keeps the name, Escape (or leaving it) too."""
        if self._editing is not None:
            return
        editor = QLineEdit(self.editor.project.chain(self.track_id, self.chain_id).name, self)
        editor.setGeometry(self.name.geometry())
        editor.selectAll()
        editor.editingFinished.connect(self._finish_rename)
        self._editing = editor
        editor.show()
        editor.setFocus()

    def _finish_rename(self) -> None:
        editor, self._editing = self._editing, None
        if editor is None:
            return
        name = editor.text().strip()
        editor.hide()
        editor.deleteLater()
        if name:
            self.editor.rename_chain(self.track_id, self.chain_id, name)

    def contextMenuEvent(self, event: QContextMenuEvent) -> None:
        editor, track_id, chain_id = self.editor, self.track_id, self.chain_id
        menu = QMenu(self)
        rename = menu.addAction("Rename", self.start_rename)
        menu.addAction("Duplicate", lambda: editor.duplicate_rack_chain(track_id, chain_id))
        menu.addAction("Delete", lambda: editor.remove_rack_chains(track_id, [chain_id]))
        menu.addSeparator()
        menu.addAction("Add Chain", lambda: editor.add_rack_chain(track_id, self.rack_id))
        volume, pan = self.automation_keys()
        menu.addSeparator()
        menu.addAction("Show Volume Automation", lambda: editor.show_automation(track_id, volume))
        menu.addAction("Show Pan Automation", lambda: editor.show_automation(track_id, pan))
        chosen = menu.exec(event.globalPos())
        # Showing the chain rebuilds the panel (and this row with it): so after the menu's
        # action, not before it; and not for a rename (it edits this row) or a deleted chain.
        if chosen is not rename and any(c.id == chain_id for _, c in iter_chains(
                editor.project.track(track_id).devices)):
            self.clicked.emit(chain_id)


class ChainList(QFrame):
    """A rack's chains, one row each, and a button adding one."""

    chain_clicked = Signal(str)  # chain id

    def __init__(self, track_id: str, rack: Device, editor: ProjectEditor, bridge: EngineBridge,
                 parent: QWidget | None = None):
        super().__init__(parent)
        self.track_id = track_id
        self.rack_id = rack.id
        self.editor = editor
        self.bridge = bridge
        self.rows: dict[str, _ChainRow] = {}
        self.setObjectName("chainList")
        self.setStyleSheet(f"#chainList {{ background: {theme.PANEL}; border: 1px solid {theme.BORDER}; }}")
        outer = QVBoxLayout(self)
        outer.setContentsMargins(1, 1, 1, 1)
        outer.setSpacing(1)
        self.body = QWidget()
        self.rows_layout = QVBoxLayout(self.body)
        self.rows_layout.setContentsMargins(0, 0, 0, 0)
        self.rows_layout.setSpacing(1)
        self.scroll = QScrollArea()
        self.scroll.setWidget(self.body)
        self.scroll.setWidgetResizable(True)
        self.scroll.setFrameShape(QFrame.Shape.NoFrame)
        self.scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        self.hint = QLabel("No chains: the rack passes its input on.\nDrop devices here, or add a chain.")
        self.hint.setWordWrap(True)
        self.hint.setStyleSheet(f"color: {theme.TEXT_DISABLED}; font-size: 8pt;")
        self.add = QPushButton("+ Chain")
        self.add.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.add.setToolTip("Add a chain to the rack")
        self.add.clicked.connect(lambda: editor.add_rack_chain(track_id, self.rack_id))
        outer.addWidget(self.scroll, 1)
        outer.addWidget(self.add)
        self.rows_layout.addWidget(self.hint)
        self.rebuild(rack)

    def rebuild(self, rack: Device) -> None:
        for row in self.rows.values():
            row.hide()
            row.deleteLater()
        self.rows.clear()
        while self.rows_layout.count():
            self.rows_layout.takeAt(0)
        for chain in rack.chains:
            row = _ChainRow(self.track_id, self.rack_id, chain, self.editor, self.bridge)
            row.clicked.connect(self.chain_clicked)
            self.rows[chain.id] = row
            self.rows_layout.addWidget(row)
            row.show()
        self.rows_layout.addWidget(self.hint)
        self.rows_layout.addStretch(1)
        self.hint.setVisible(not rack.chains)

    def chain_ids(self) -> list[str]:
        return list(self.rows)

    def set_selected(self, chain_id: str | None) -> None:
        for row_id, row in self.rows.items():
            row.set_selected(row_id == chain_id)
        if chain_id in self.rows:
            self.scroll.ensureWidgetVisible(self.rows[chain_id], 0, 0)

    def refresh(self, rack: Device) -> None:
        if [c.id for c in rack.chains] != list(self.rows):
            self.rebuild(rack)
            return
        for chain in rack.chains:
            self.rows[chain.id].refresh(chain)

    def refresh_chain(self, chain: Chain) -> None:
        if chain.id in self.rows:
            self.rows[chain.id].refresh(chain)

    def refresh_automation(self) -> None:
        for row in self.rows.values():
            row.refresh_automation()

    def follows_automation(self) -> bool:
        return any(automation_state(self.bridge, self.track_id, key) == "on"
                   for row in self.rows.values() for key in row.automation_keys())

    def refresh_meters(self) -> None:
        for chain_id, row in self.rows.items():
            row.meter.set_levels(*self.bridge.chain_meters.get(chain_id, (0.0, 0.0)))

    def chain_at(self, pos: QPoint) -> str | None:
        """The chain whose row is at `pos` (this widget's coordinates), if any."""
        for chain_id, row in self.rows.items():
            if row.rect().contains(row.mapFrom(self, pos)):
                return chain_id
        return None
