"""The computer MIDI keyboard (as in Ableton): while it is on (M), the letter
keys play notes into the MIDI tracks that hear it, as a MIDI input called
"Computer Keyboard" (every track on All Ins hears it too).

The middle row is the white keys from C (A, S, D, F, G, H, J, K, L, ;, '), the
row above it the black keys between them (W, E, T, Y, U, O, P). Z and X move
the octave down and up.

The keys reach it before the window's shortcuts (S, A and Z do other things
while it is off), but not while typing in a text field. Notes still held when it
is turned off, or when the application loses the keyboard, are released."""

from __future__ import annotations

from PySide6.QtCore import QEvent, QObject, Qt, Signal
from PySide6.QtGui import QKeyEvent
from PySide6.QtWidgets import (
    QAbstractSpinBox,
    QApplication,
    QComboBox,
    QLineEdit,
    QPlainTextEdit,
    QTextEdit,
)

from ..audio.engine_bridge import COMPUTER_KEYBOARD, EngineBridge
from ..model.notes import note_name

# Key -> semitones above the octave's C.
NOTE_KEYS = {
    Qt.Key.Key_A: 0, Qt.Key.Key_W: 1, Qt.Key.Key_S: 2, Qt.Key.Key_E: 3, Qt.Key.Key_D: 4, Qt.Key.Key_F: 5,
    Qt.Key.Key_T: 6, Qt.Key.Key_G: 7, Qt.Key.Key_Y: 8, Qt.Key.Key_H: 9, Qt.Key.Key_U: 10, Qt.Key.Key_J: 11,
    Qt.Key.Key_K: 12, Qt.Key.Key_O: 13, Qt.Key.Key_L: 14, Qt.Key.Key_P: 15, Qt.Key.Key_Semicolon: 16,
    Qt.Key.Key_Apostrophe: 17,
}
OCTAVE_KEYS = {Qt.Key.Key_Z: -1, Qt.Key.Key_X: 1}
DEFAULT_OCTAVE = 5  # C3 (60) at A
MAX_OCTAVE = 9  # its C is 108; the highest key is F of the next octave (125)
VELOCITY = 100
TEXT_INPUTS = (QLineEdit, QTextEdit, QPlainTextEdit, QAbstractSpinBox, QComboBox)


class ComputerKeyboard(QObject):
    changed = Signal()  # turned on or off, or another octave

    def __init__(self, bridge: EngineBridge, parent: QObject | None = None):
        super().__init__(parent)
        self.bridge = bridge
        self.enabled = False
        self.octave = DEFAULT_OCTAVE
        self._held: dict[int, int] = {}  # key -> the note it plays (kept when the octave changes)
        app = QApplication.instance()
        app.installEventFilter(self)
        app.applicationStateChanged.connect(self._application_state_changed)

    @property
    def lowest_note(self) -> int:
        """The note A plays."""
        return self.octave * 12

    def octave_label(self) -> str:
        return note_name(self.lowest_note)

    def set_enabled(self, enabled: bool) -> None:
        if enabled == self.enabled:
            return
        self.enabled = enabled
        if not enabled:
            self.release_all()
        self.changed.emit()
        state = f"on: A plays {self.octave_label()}, Z and X change the octave" if enabled else "off"
        self.bridge.status_message.emit(f"Computer MIDI keyboard {state}.")

    def toggle(self) -> None:
        self.set_enabled(not self.enabled)

    def shift_octave(self, delta: int) -> None:
        octave = max(0, min(MAX_OCTAVE, self.octave + delta))
        if octave != self.octave:
            self.octave = octave
            self.changed.emit()
        self.bridge.status_message.emit(f"Computer MIDI keyboard: A plays {self.octave_label()}.")

    def press(self, key: int) -> None:
        if key in OCTAVE_KEYS:
            self.shift_octave(OCTAVE_KEYS[key])
            return
        if key in self._held or key not in NOTE_KEYS:
            return
        note = self.lowest_note + NOTE_KEYS[key]
        if note > 127:
            return
        self._held[key] = note
        self.bridge.send_midi([0x90, note, VELOCITY], COMPUTER_KEYBOARD)

    def release(self, key: int) -> None:
        note = self._held.pop(key, None)
        if note is not None:
            self.bridge.send_midi([0x80, note, 0], COMPUTER_KEYBOARD)

    def release_all(self) -> None:
        for key in list(self._held):
            self.release(key)

    def _application_state_changed(self, state: Qt.ApplicationState) -> None:
        if state != Qt.ApplicationState.ApplicationActive:
            self.release_all()  # its key-ups would go elsewhere

    def _handles(self, event: QKeyEvent) -> bool:
        if not self.enabled or event.modifiers() & ~Qt.KeyboardModifier.KeypadModifier:
            return False
        key = event.key()
        if key not in NOTE_KEYS and key not in OCTAVE_KEYS:
            return False
        return not isinstance(QApplication.focusWidget(), TEXT_INPUTS)

    def eventFilter(self, watched: QObject, event: QEvent) -> bool:
        kind = event.type()
        if kind == QEvent.Type.ShortcutOverride:
            if self._handles(event):
                event.accept()  # the key comes as a key press, not as the window's shortcut
            return False
        if kind in (QEvent.Type.KeyPress, QEvent.Type.KeyRelease):
            # Key-ups go through even when they no longer would be ours (a modifier pressed meanwhile).
            if kind == QEvent.Type.KeyRelease and not event.isAutoRepeat() and event.key() in self._held:
                self.release(event.key())
                return True
            if not self._handles(event):
                return False
            # A key event reaches the filter once per widget it is delivered to: take it the first time.
            if kind == QEvent.Type.KeyPress and not event.isAutoRepeat():
                self.press(event.key())
            return True
        return False
