"""The main window's Ctrl/Alt shortcuts while a plug-in's editor has the focus.

Plug-in editors are plain Win32 windows (engine/src/plugins/EditorWindow.cpp), so
Qt never sees their keys as key events, but their messages still come through
Qt's event loop. A native event filter catches a key press there, and if it is
one of the main window's Ctrl/Alt shortcuts, triggers that action instead of
letting the plug-in have it.

Keys without Ctrl or Alt (Space, Delete, letters...) and the text-editing
shortcuts (Ctrl+A/C/V/X/Z/Y) stay with the plug-in: it may be typing into a
field of its own."""

from __future__ import annotations

import ctypes
import sys
from ctypes import wintypes

from PySide6.QtCore import QAbstractNativeEventFilter, QKeyCombination, Qt
from PySide6.QtGui import QAction, QKeySequence
from PySide6.QtWidgets import QWidget

EDITOR_WINDOW_CLASS = "GILStudioPluginEditor"  # EditorWindow.cpp's kWindowClass

WM_KEYDOWN = 0x0100
WM_SYSKEYDOWN = 0x0104
VK_SHIFT, VK_CONTROL, VK_MENU = 0x10, 0x11, 0x12
GA_ROOT = 2

# The plug-in's own text editing, never taken from it.
KEEP_FOR_PLUGIN = [QKeySequence(s) for s in ("Ctrl+A", "Ctrl+C", "Ctrl+V", "Ctrl+X", "Ctrl+Z", "Ctrl+Y",
                                              "Ctrl+Shift+Z")]

_VK_KEYS = {
    0xBB: Qt.Key.Key_Equal, 0xBC: Qt.Key.Key_Comma, 0xBD: Qt.Key.Key_Minus, 0xBE: Qt.Key.Key_Period,
    0x08: Qt.Key.Key_Backspace, 0x09: Qt.Key.Key_Tab, 0x2E: Qt.Key.Key_Delete, 0x24: Qt.Key.Key_Home,
    0x20: Qt.Key.Key_Space,
}


def qt_key(vk: int) -> Qt.Key | None:
    """The Qt key for a Windows virtual-key code, for the keys shortcuts use."""
    if 0x41 <= vk <= 0x5A:
        return Qt.Key(Qt.Key.Key_A.value + vk - 0x41)
    if 0x30 <= vk <= 0x39:
        return Qt.Key(Qt.Key.Key_0.value + vk - 0x30)
    if 0x70 <= vk <= 0x7B:
        return Qt.Key(Qt.Key.Key_F1.value + vk - 0x70)
    return _VK_KEYS.get(vk)


def is_plugin_editor(hwnd: int) -> bool:
    """Whether `hwnd` is (inside) a plug-in editor window."""
    user32 = ctypes.windll.user32
    root = user32.GetAncestor(wintypes.HWND(hwnd), GA_ROOT)
    if not root:
        return False
    name = ctypes.create_unicode_buffer(64)
    user32.GetClassNameW(root, name, len(name))
    return name.value == EDITOR_WINDOW_CLASS


def pressed_modifiers() -> Qt.KeyboardModifier:
    state = ctypes.windll.user32.GetKeyState
    mods = Qt.KeyboardModifier.NoModifier
    for vk, mod in ((VK_CONTROL, Qt.KeyboardModifier.ControlModifier), (VK_SHIFT, Qt.KeyboardModifier.ShiftModifier),
                    (VK_MENU, Qt.KeyboardModifier.AltModifier)):
        if state(vk) & 0x8000:
            mods |= mod
    return mods


class PluginEditorShortcuts(QAbstractNativeEventFilter):
    """Install on the application (Windows only) for `window`'s actions."""

    def __init__(self, window: QWidget):
        super().__init__()
        self.window = window

    def nativeEventFilter(self, event_type, message) -> tuple[bool, int]:
        if bytes(event_type) != b"windows_generic_MSG":
            return False, 0
        msg = wintypes.MSG.from_address(int(message))
        if msg.message not in (WM_KEYDOWN, WM_SYSKEYDOWN) or not is_plugin_editor(msg.hWnd):
            return False, 0
        action = self.action_for(msg.wParam, pressed_modifiers())
        if action is None:
            return False, 0
        action.trigger()
        return True, 0

    def action_for(self, vk: int, mods: Qt.KeyboardModifier) -> QAction | None:
        """The main window's action for this key, if the plug-in shouldn't keep it."""
        key = qt_key(vk)
        if key is None or not mods & (Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.AltModifier):
            return None
        pressed = QKeySequence(QKeyCombination(mods, key))
        if any(pressed.matches(k) == QKeySequence.SequenceMatch.ExactMatch for k in KEEP_FOR_PLUGIN):
            return None
        for action in self.window.findChildren(QAction):
            if action.isEnabled() and any(pressed.matches(s) == QKeySequence.SequenceMatch.ExactMatch
                                          for s in action.shortcuts()):
                return action
        return None


def supported() -> bool:
    return sys.platform == "win32"
