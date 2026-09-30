"""Dark, Ableton-like look: Fusion style + palette + a stylesheet."""

from __future__ import annotations

from PySide6.QtGui import QColor, QFont, QPalette
from PySide6.QtWidgets import QApplication

# Base colours
WINDOW = "#1c1c1c"
PANEL = "#252525"
PANEL_ALT = "#2c2c2c"
SURFACE = "#363636"
SURFACE_HOVER = "#404040"
BORDER = "#111111"
TEXT = "#d9d9d9"
TEXT_DIM = "#8e8e8e"
TEXT_DISABLED = "#5c5c5c"
ACCENT = "#ffa62b"
ACCENT_TEXT = "#1a1a1a"

# Arrangement
LANE = "#2b2b2b"
LANE_SELECTED = "#343434"
EMPTY_AREA = "#222222"
GRID_BAR = "#4a4a4a"
GRID_BEAT = "#393939"
GRID_SUB = "#313131"
PLAYHEAD = "#f2f2f2"
INSERT_MARKER = "#ffa62b"
LOOP_ON = "#c9c9c9"
LOOP_OFF = "#5d5d5d"
LOOP_REGION = QColor(255, 255, 255, 12)
SELECTION_OUTLINE = "#ffffff"
RUBBER_BAND = QColor(255, 166, 43, 40)
WAVEFORM = QColor(22, 22, 22, 230)  # also MIDI notes drawn in clips

# Piano roll
KEY_WHITE = "#d2d2d2"
KEY_BLACK = "#1f1f1f"
KEY_LABEL = "#4a4a4a"
BLACK_KEY_ROW = "#252525"
OUTSIDE_CLIP = QColor(0, 0, 0, 110)  # content a clip has but doesn't play

# Controls
ACTIVATOR_ON = "#ffc233"
SOLO_ON = "#4fa3ff"
PLAY_ON = "#5fd35f"
METER_LOW = QColor("#3fcf55")
METER_MID = QColor("#f2d024")
METER_HIGH = QColor("#ff4a3d")
METER_BG = QColor("#141414")

MONO_FONT = "Consolas"


def ui_font(point_size: float = 9.0, bold: bool = False) -> QFont:
    font = QFont("Segoe UI")
    font.setPointSizeF(point_size)
    font.setBold(bold)
    return font


STYLESHEET = f"""
QWidget {{ color: {TEXT}; }}
QMainWindow, QDialog {{ background: {WINDOW}; }}
QMenuBar {{ background: {PANEL}; border-bottom: 1px solid {BORDER}; }}
QMenuBar::item {{ padding: 4px 10px; background: transparent; }}
QMenuBar::item:selected {{ background: {SURFACE}; }}
QMenu {{ background: {PANEL_ALT}; border: 1px solid {BORDER}; padding: 3px; }}
QMenu::item {{ padding: 4px 22px 4px 18px; }}
QMenu::item:selected {{ background: {ACCENT}; color: {ACCENT_TEXT}; }}
QMenu::item:disabled {{ color: {TEXT_DISABLED}; }}
QMenu::separator {{ height: 1px; background: {BORDER}; margin: 3px 6px; }}
QStatusBar {{ background: {PANEL}; border-top: 1px solid {BORDER}; color: {TEXT_DIM}; }}
QStatusBar::item {{ border: none; }}
QSplitter::handle {{ background: {BORDER}; }}
QToolTip {{ background: {PANEL_ALT}; color: {TEXT}; border: 1px solid {BORDER}; padding: 3px; }}

QLineEdit {{
    background: {SURFACE}; border: 1px solid {BORDER}; border-radius: 3px; padding: 3px 6px;
    selection-background-color: {ACCENT}; selection-color: {ACCENT_TEXT};
}}
QLineEdit:focus {{ border: 1px solid {ACCENT}; }}

QPushButton {{
    background: {SURFACE}; border: 1px solid {BORDER}; border-radius: 3px; padding: 4px 12px;
}}
QPushButton:hover {{ background: {SURFACE_HOVER}; }}
QPushButton:pressed {{ background: {PANEL}; }}
QPushButton:disabled {{ color: {TEXT_DISABLED}; }}
QPushButton:checked {{ background: {ACCENT}; color: {ACCENT_TEXT}; }}
QPushButton[role="activator"]:checked {{ background: {ACTIVATOR_ON}; color: {ACCENT_TEXT}; }}
QPushButton[role="solo"]:checked {{ background: {SOLO_ON}; color: {ACCENT_TEXT}; }}
QPushButton[role="play"]:checked {{ background: {PLAY_ON}; }}
QPushButton[role="re-enable"] {{ padding: 2px; min-width: 26px; min-height: 22px; }}
QPushButton[role="re-enable"]:checked {{ background: {ACCENT}; }}
QPushButton[role="tool"] {{ padding: 2px; min-width: 26px; min-height: 22px; }}
QPushButton[role="activator"], QPushButton[role="solo"] {{
    padding: 0px; font-size: 8pt; font-weight: 600; border-radius: 2px;
}}
QPushButton[role="flat"] {{ padding: 0px; border: none; background: transparent; color: {TEXT_DIM}; }}
QPushButton[role="small"] {{ padding: 0px 6px; font-size: 8pt; }}
QPushButton[role="flat"]:hover {{ color: {TEXT}; }}

QComboBox {{
    background: {SURFACE}; border: 1px solid {BORDER}; border-radius: 3px; padding: 3px 8px;
}}
QComboBox QAbstractItemView {{
    background: {PANEL_ALT}; border: 1px solid {BORDER};
    selection-background-color: {ACCENT}; selection-color: {ACCENT_TEXT};
}}
QCheckBox::indicator {{ width: 14px; height: 14px; }}

QTreeView, QListView, QTreeWidget {{
    background: {PANEL}; border: none; outline: 0;
    selection-background-color: {ACCENT}; selection-color: {ACCENT_TEXT};
}}
QTreeView::item, QListView::item {{ padding: 2px 0px; }}
QTreeView::item:hover, QListView::item:hover {{ background: {PANEL_ALT}; }}
QTreeView::item:selected, QListView::item:selected {{ background: {ACCENT}; color: {ACCENT_TEXT}; }}
QHeaderView::section {{ background: {PANEL_ALT}; border: none; padding: 3px; }}

QScrollBar:vertical {{ background: {PANEL}; width: 12px; margin: 0; }}
QScrollBar:horizontal {{ background: {PANEL}; height: 12px; margin: 0; }}
QScrollBar::handle {{ background: {SURFACE_HOVER}; border-radius: 4px; margin: 2px; }}
QScrollBar::handle:hover {{ background: #555555; }}
QScrollBar::handle:vertical {{ min-height: 24px; }}
QScrollBar::handle:horizontal {{ min-width: 24px; }}
QScrollBar::add-line, QScrollBar::sub-line {{ width: 0; height: 0; }}
QScrollBar::add-page, QScrollBar::sub-page {{ background: none; }}

QTabWidget::pane {{ border: 1px solid {BORDER}; }}
QTabBar::tab {{ background: {PANEL}; padding: 5px 14px; border: 1px solid {BORDER}; }}
QTabBar::tab:selected {{ background: {SURFACE}; }}
QGroupBox {{ border: 1px solid {BORDER}; border-radius: 4px; margin-top: 12px; padding-top: 6px; }}
QGroupBox::title {{ subcontrol-origin: margin; left: 8px; color: {TEXT_DIM}; }}
"""


def apply(app: QApplication) -> None:
    app.setStyle("Fusion")
    app.setFont(ui_font())
    palette = QPalette()
    roles = {
        QPalette.ColorRole.Window: WINDOW,
        QPalette.ColorRole.WindowText: TEXT,
        QPalette.ColorRole.Base: PANEL,
        QPalette.ColorRole.AlternateBase: PANEL_ALT,
        QPalette.ColorRole.Text: TEXT,
        QPalette.ColorRole.Button: SURFACE,
        QPalette.ColorRole.ButtonText: TEXT,
        QPalette.ColorRole.Highlight: ACCENT,
        QPalette.ColorRole.HighlightedText: ACCENT_TEXT,
        QPalette.ColorRole.ToolTipBase: PANEL_ALT,
        QPalette.ColorRole.ToolTipText: TEXT,
        QPalette.ColorRole.PlaceholderText: TEXT_DIM,
        QPalette.ColorRole.Link: ACCENT,
    }
    for role, color in roles.items():
        palette.setColor(role, QColor(color))
    for role in (QPalette.ColorRole.Text, QPalette.ColorRole.ButtonText, QPalette.ColorRole.WindowText):
        palette.setColor(QPalette.ColorGroup.Disabled, role, QColor(TEXT_DISABLED))
    app.setPalette(palette)
    app.setStyleSheet(STYLESHEET)
