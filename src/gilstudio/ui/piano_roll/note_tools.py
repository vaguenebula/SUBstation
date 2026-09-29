"""The note tools: Legato, Quantize and Humanize, in a small floating bar that
appears over the note grid next to the selected notes whenever there are some
(select with the mouse, the keys or Ctrl+A). Each tool is one undo step."""

from __future__ import annotations

from typing import TYPE_CHECKING

from PySide6.QtCore import QRectF, Qt
from PySide6.QtGui import QColor, QPainter, QPen
from PySide6.QtWidgets import QComboBox, QFrame, QHBoxLayout, QLabel, QPushButton, QWidget

from ... import theme
from ...model.notes import HUMANIZE_BEATS, HUMANIZE_VELOCITY, QUANTIZE_GRIDS
from ..widgets import ValueBox

if TYPE_CHECKING:
    from .piano_roll import PianoRoll

DEFAULT_GRID = "1/16"
DEFAULT_HUMANIZE = 25.0  # %
GAP = 8  # between the bar and the notes
MARGIN = 4  # between the bar and the grid's edges
RADIUS = 5.0


def _percent(value: float) -> str:
    return f"{value:.0f} %"


def _separator() -> QFrame:
    line = QFrame()
    line.setFrameShape(QFrame.Shape.VLine)
    line.setFixedHeight(16)
    line.setStyleSheet(f"color: {theme.SURFACE_HOVER};")
    return line


class NoteTools(QWidget):
    def __init__(self, roll: PianoRoll, parent: QWidget):
        super().__init__(parent)
        self.roll = roll
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, False)
        self.setCursor(Qt.CursorShape.ArrowCursor)

        self.count = QLabel()
        self.count.setStyleSheet(f"color: {theme.TEXT_DIM}; font-size: 8pt;")
        self.legato = self._button("Legato", "Make the selected notes last until the next one starts;\n"
                                             "the last ones until the end of the clip")
        self.quantize = self._button("Quantize", "Move the selected notes' starts onto the grid chosen "
                                                 "next to it (Ctrl+U)")
        self.grid = QComboBox()
        for name, _beats in QUANTIZE_GRIDS:
            self.grid.addItem(name)
        self.grid.setCurrentText(DEFAULT_GRID)
        self.grid.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.grid.setToolTip("The grid Quantize moves notes onto (T: triplets)")
        self.amount = ValueBox(100.0, 0.0, 100.0, step=1.0, decimals=0, formatter=_percent, sample_text="100 %")
        self.amount.setToolTip("Quantize amount: how far notes move toward the grid (100 %: all the way)")
        self.humanize = self._button("Humanize", "Nudge the selected notes' timing and velocity at random,\n"
                                                 "as a player would")
        self.humanize_amount = ValueBox(DEFAULT_HUMANIZE, 0.0, 100.0, step=1.0, decimals=0, formatter=_percent,
                                        sample_text="100 %")
        self.humanize_amount.setToolTip(
            f"Humanize amount. At 100 % notes move by up to a 32nd note ({HUMANIZE_BEATS:g} beats)\n"
            f"either way and velocities change by up to {HUMANIZE_VELOCITY}.")

        layout = QHBoxLayout(self)
        layout.setContentsMargins(8, 4, 5, 4)
        layout.setSpacing(5)
        for widget in (self.count, _separator(), self.legato, _separator(), self.quantize, self.grid, self.amount,
                       _separator(), self.humanize, self.humanize_amount):
            layout.addWidget(widget)

        self.legato.clicked.connect(roll.legato)
        self.quantize.clicked.connect(roll.quantize)
        self.humanize.clicked.connect(roll.humanize)
        self.hide()

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

    def show_near(self, notes_rect: QRectF | None, count: int) -> None:
        """Float above the selected notes (`notes_rect`, in the grid's coordinates),
        or below them when there is no room above; hide for None."""
        if notes_rect is None or count == 0:
            self.hide()
            return
        self.count.setText(f"{count} note{'s' if count != 1 else ''}")
        self.adjustSize()
        area = self.parentWidget().rect()
        width, height = self.width(), self.height()
        x = notes_rect.center().x() - width / 2
        y = notes_rect.top() - GAP - height
        if y < MARGIN:
            y = notes_rect.bottom() + GAP
        x = max(MARGIN, min(area.width() - width - MARGIN, x))
        y = max(MARGIN, min(area.height() - height - MARGIN, y))
        self.move(round(x), round(y))
        self.show()
        self.raise_()

    def paintEvent(self, _event) -> None:
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        body = QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5)
        p.setPen(QPen(QColor(theme.BORDER), 1.0))
        p.setBrush(QColor(theme.PANEL_ALT))
        p.drawRoundedRect(body, RADIUS, RADIUS)

    # The bar keeps its clicks to itself (the grid is underneath); wheel turns
    # outside its boxes still scroll the grid.
    def mousePressEvent(self, event) -> None:
        event.accept()

    def mouseDoubleClickEvent(self, event) -> None:
        event.accept()
