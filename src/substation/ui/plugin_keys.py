"""The main window's shortcuts while a plug-in's editor has the focus.

Plug-in editors are plain Win32 windows (engine/src/plugins/EditorWindow.cpp), so
Qt never sees their keys as key events, but their messages still come through
Qt's event loop. A native event filter catches a key press there, and if it is
one of the main window's Ctrl/Alt shortcuts, triggers that action instead of
letting the plug-in have it.

Space (play / stop) and S (solo) are the window's too: the DAW comes first.
They stay with the plug-in only while it types into a text field Windows knows
(an Edit or RichEdit control: most plug-ins draw their own, which can't be told
apart), and S while the computer MIDI keyboard plays notes with it, as in the
main window. Held down, they act once (the key's repeats go nowhere).

Other keys without Ctrl or Alt (Delete, letters...) and the text-editing
shortcuts (Ctrl+A/C/V/X/Z/Y) stay with the plug-in: it may be typing into a
field of its own.

Ctrl+W (the main window's Close Plug-in Editor) closes the foremost editor,
from the main window or from any editor."""

from __future__ import annotations

import ctypes
import os
import sys
from ctypes import wintypes

from PySide6.QtCore import QAbstractNativeEventFilter, QKeyCombination, Qt
from PySide6.QtGui import QAction, QKeySequence
from PySide6.QtWidgets import QWidget

EDITOR_WINDOW_CLASS = "SUBstationPluginEditor"  # EditorWindow.cpp's kWindowClass

WM_KEYDOWN = 0x0100
WM_SYSKEYDOWN = 0x0104
WM_CLOSE = 0x0010
VK_SHIFT, VK_CONTROL, VK_MENU = 0x10, 0x11, 0x12
GA_ROOT = 2

# Keys without Ctrl or Alt that are the main window's in a plug-in's editor too.
DAW_KEYS = (Qt.Key.Key_Space, Qt.Key.Key_S)
TEXT_FIELD_CLASSES = ("edit", "richedit")  # (prefixes of) Windows' text field classes, lower case
REPEAT_BIT = 1 << 30  # a WM_KEYDOWN's lParam: the key was down already (a repeat)

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


def _class_name(hwnd) -> str:
    name = ctypes.create_unicode_buffer(64)
    ctypes.windll.user32.GetClassNameW(wintypes.HWND(hwnd), name, len(name))
    return name.value


def _is_editor_class(hwnd) -> bool:
    return _class_name(hwnd) == EDITOR_WINDOW_CLASS


def is_text_field(hwnd: int) -> bool:
    """Whether `hwnd` is one of Windows' own text fields (a plug-in typing into it keeps its keys)."""
    return _class_name(hwnd).lower().startswith(TEXT_FIELD_CLASSES)


def is_plugin_editor(hwnd: int) -> bool:
    """Whether `hwnd` is (inside) a plug-in editor window."""
    root = ctypes.windll.user32.GetAncestor(wintypes.HWND(hwnd), GA_ROOT)
    return bool(root) and _is_editor_class(root)


def foremost_editor() -> int | None:
    """This process's shown plug-in editor that is highest in the z-order."""
    user32 = ctypes.windll.user32
    found: list[int] = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def visit(hwnd, _lparam):  # top-level windows, from the top down
        pid = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value == os.getpid() and user32.IsWindowVisible(hwnd) and _is_editor_class(hwnd):
            found.append(hwnd)
            return False
        return True

    user32.EnumWindows(visit, 0)
    return found[0] if found else None


def close_foremost_editor() -> bool:
    """Close the foremost plug-in editor, as its close button would. False if none is shown."""
    hwnd = foremost_editor()
    if hwnd is None:
        return False
    ctypes.windll.user32.PostMessageW(wintypes.HWND(hwnd), WM_CLOSE, 0, 0)
    return True


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
        mods = pressed_modifiers()
        action = self.action_for(msg.wParam, mods, text_field=is_text_field(msg.hWnd))
        if action is None:
            return False, 0
        held = msg.lParam & REPEAT_BIT and not mods & (Qt.KeyboardModifier.ControlModifier
                                                       | Qt.KeyboardModifier.AltModifier)
        if not held:  # (Space held down doesn't start and stop playing over and over)
            action.trigger()
        return True, 0

    def action_for(self, vk: int, mods: Qt.KeyboardModifier, text_field: bool = False) -> QAction | None:
        """The main window's action for this key, if the plug-in shouldn't keep it."""
        key = qt_key(vk)
        if key is None:
            return None
        if not mods & (Qt.KeyboardModifier.ControlModifier | Qt.KeyboardModifier.AltModifier):
            if key not in DAW_KEYS or mods & Qt.KeyboardModifier.ShiftModifier or text_field:
                return None
            keyboard = getattr(self.window, "computer_keyboard", None)
            if keyboard is not None and keyboard.takes_key(key):
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
