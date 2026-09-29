from __future__ import annotations

from PySide6.QtCore import QSize, Qt
from PySide6.QtGui import QIcon
from PySide6.QtWidgets import QPushButton, QWidget


class ToggleButton(QPushButton):
    """Checkable button that never takes keyboard focus (so Space stays play/stop).
    `role` selects the checked colour in the stylesheet (activator, solo, play...)."""

    def __init__(self, text: str = "", *, role: str | None = None, icon: QIcon | None = None,
                 tooltip: str = "", checkable: bool = True, parent: QWidget | None = None):
        super().__init__(text, parent)
        self.setCheckable(checkable)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        if role:
            self.setProperty("role", role)
        if icon is not None:
            self.setIcon(icon)
            self.setIconSize(QSize(14, 14))
        if tooltip:
            self.setToolTip(tooltip)

    def set_checked_silently(self, checked: bool) -> None:
        self.blockSignals(True)
        self.setChecked(checked)
        self.blockSignals(False)
