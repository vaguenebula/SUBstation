"""The piano roll's tool row: Legato, Quantize and Humanize. Each acts on the
selected notes, or on all of the clip's notes when none are selected, as one
undo step."""

from __future__ import annotations

from typing import TYPE_CHECKING

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QColor, QPainter
from PySide6.QtWidgets import QComboBox, QFrame, QHBoxLayout, QPushButton, QWidget

from ... import theme
from ...model.notes import HUMANIZE_BEATS, HUMANIZE_VELOCITY, QUANTIZE_GRIDS
from ..widgets import ValueBox

if TYPE_CHECKING:
    from .piano_roll import PianoRoll

TOOLS_HEIGHT = 30
DEFAULT_GRID = "1/16"
DEFAULT_HUMANIZE = 25.0  # %
WHICH = "the selected notes (all of them if none are selected)"


def _percent(value: float) -> str:
    return f"{value:.0f} %"


def _separator() -> QFrame:
    line = QFrame()
    line.setFrameShape(QFrame.Shape.VLine)
    line.setStyleSheet(f"color: {theme.BORDER};")
    return line


class NoteTools(QWidget):
    def __init__(self, roll: PianoRoll):
        super().__init__(roll)
        self.roll = roll
        self.setFixedHeight(TOOLS_HEIGHT)

        self.legato = self._button("Legato", f"Legato: make {WHICH} last until the next one starts;\n"
                                             "the last ones until the end of the clip")
        self.quantize = self._button("Quantize", f"Quantize {WHICH}: move their starts onto the grid "
                                                 "chosen next to it (Ctrl+U)")
        self.grid = QComboBox()
        for name, _beats in QUANTIZE_GRIDS:
            self.grid.addItem(name)
        self.grid.setCurrentText(DEFAULT_GRID)
        self.grid.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.grid.setToolTip("The grid Quantize moves notes onto (T: triplets)")
        self.amount = ValueBox(100.0, 0.0, 100.0, step=1.0, decimals=0, formatter=_percent, sample_text="100 %")
        self.amount.setToolTip("Quantize amount: how far notes move toward the grid (100 %: all the way)")
        self.humanize = self._button("Humanize", f"Humanize {WHICH}: nudge their timing and velocity at random,\n"
                                                 "as a player would")
        self.humanize_amount = ValueBox(DEFAULT_HUMANIZE, 0.0, 100.0, step=1.0, decimals=0, formatter=_percent,
                                        sample_text="100 %")
        self.humanize_amount.setToolTip(
            f"Humanize amount. At 100 % notes move by up to a 32nd note ({HUMANIZE_BEATS:g} beats)\n"
            f"either way and velocities change by up to {HUMANIZE_VELOCITY}.")

        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 3, 8, 4)
        layout.setSpacing(5)
        for widget in (self.legato, _separator(), self.quantize, self.grid, self.amount, _separator(),
                       self.humanize, self.humanize_amount):
            layout.addWidget(widget)
        layout.addStretch(1)

        self.legato.clicked.connect(roll.legato)
        self.quantize.clicked.connect(roll.quantize)
        self.humanize.clicked.connect(roll.humanize)

    @staticmethod
    def _button(text: str, tooltip: str) -> QPushButton:
        button = QPushButton(text)
        button.setToolTip(tooltip)
        button.setFocusPolicy(Qt.FocusPolicy.NoFocus)  # the notes keep the keyboard
        return button

    @property
    def quantize_step(self) -> float:
        return dict(QUANTIZE_GRIDS)[self.grid.currentText()]

    @property
    def quantize_amount(self) -> float:
        return self.amount.value() / 100.0

    @property
    def humanize_level(self) -> float:
        return self.humanize_amount.value() / 100.0

    def set_has_notes(self, has_notes: bool) -> None:
        for button in (self.legato, self.quantize, self.humanize):
            button.setEnabled(has_notes)

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.fillRect(self.rect(), QColor(theme.PANEL))
        p.fillRect(QRectF(0, self.height() - 1, self.width(), 1), QColor(theme.BORDER))
