#include "PluginEditorKeys.h"

#include <QCoreApplication>
#include <QKeyCombination>
#include <QMetaObject>
#include <QVariant>
#include <QVariantList>

#include "input/Shortcuts.h"
#include "session/ComputerKeyboard.h"
#include "session/RenderProgress.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwctype>
#include <string>
#endif

namespace sub::ui {

namespace {

constexpr int kVirtualSpace = 0x20;

// The plug-in's own text editing, never taken from it.
const QList<QKeySequence>& keptForPlugin() {
    static const QList<QKeySequence> kept{
        QKeySequence(QStringLiteral("Ctrl+A")), QKeySequence(QStringLiteral("Ctrl+C")),
        QKeySequence(QStringLiteral("Ctrl+V")), QKeySequence(QStringLiteral("Ctrl+X")),
        QKeySequence(QStringLiteral("Ctrl+Z")), QKeySequence(QStringLiteral("Ctrl+Y")),
        QKeySequence(QStringLiteral("Ctrl+Shift+Z"))};
    return kept;
}

// Keys without Ctrl or Alt that are the main window's in a plug-in's editor too.
bool dawKey(int key) { return key == Qt::Key_Space || key == Qt::Key_S; }

bool matches(const QKeySequence& pressed, const QList<QKeySequence>& sequences) {
    for (const QKeySequence& sequence : sequences)
        if (!sequence.isEmpty() && pressed.matches(sequence) == QKeySequence::ExactMatch) return true;
    return false;
}

#ifdef Q_OS_WIN
std::wstring className(HWND hwnd) {
    wchar_t name[64] = {};
    const int length = GetClassNameW(hwnd, name, 64);
    return std::wstring(name, length > 0 ? size_t(length) : 0);
}

bool isEditorClass(HWND hwnd) { return className(hwnd) == PluginEditorKeys::kEditorWindowClass; }

// Whether `hwnd` is (inside) a plug-in editor window.
bool isPluginEditor(HWND hwnd) {
    const HWND root = GetAncestor(hwnd, GA_ROOT);
    return root != nullptr && isEditorClass(root);
}

// Whether `hwnd` is one of Windows' own text fields (a plug-in typing into it keeps its keys).
bool isTextField(HWND hwnd) {
    std::wstring name = className(hwnd);
    for (wchar_t& c : name) c = static_cast<wchar_t>(std::towlower(c));
    return name.rfind(L"edit", 0) == 0 || name.rfind(L"richedit", 0) == 0;
}

int pressedModifiers() {
    int modifiers = Qt::NoModifier;
    if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= Qt::ControlModifier;
    if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= Qt::ShiftModifier;
    if (GetKeyState(VK_MENU) & 0x8000) modifiers |= Qt::AltModifier;
    return modifiers;
}

BOOL CALLBACK findEditor(HWND hwnd, LPARAM found) {  // top-level windows, from the top down
    DWORD process = 0;
    GetWindowThreadProcessId(hwnd, &process);
    if (process == GetCurrentProcessId() && IsWindowVisible(hwnd) && isEditorClass(hwnd)) {
        *reinterpret_cast<HWND*>(found) = hwnd;
        return FALSE;
    }
    return TRUE;
}
#endif

}  // namespace

PluginEditorKeys::PluginEditorKeys(QObject* parent) : QObject(parent) {
    if (supported() && QCoreApplication::instance()) {
        QCoreApplication::instance()->installNativeEventFilter(this);
        installed_ = true;
    }
}

PluginEditorKeys::~PluginEditorKeys() {
    if (installed_ && QCoreApplication::instance()) QCoreApplication::instance()->removeNativeEventFilter(this);
}

void PluginEditorKeys::setTarget(QObject* target) {
    if (target == target_) return;
    target_ = target;
    Q_EMIT targetChanged();
}

void PluginEditorKeys::setSession(app::Session* session) {
    if (session == session_) return;
    session_ = session;
    Q_EMIT sessionChanged();
}

bool PluginEditorKeys::supported() {
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

int PluginEditorKeys::qtKey(int vk) {
    if (vk >= 0x41 && vk <= 0x5A) return Qt::Key_A + (vk - 0x41);
    if (vk >= 0x30 && vk <= 0x39) return Qt::Key_0 + (vk - 0x30);
    if (vk >= 0x70 && vk <= 0x7B) return Qt::Key_F1 + (vk - 0x70);
    switch (vk) {
        case 0xBB: return Qt::Key_Equal;   // VK_OEM_PLUS: the = + key
        case 0xBC: return Qt::Key_Comma;   // VK_OEM_COMMA
        case 0xBD: return Qt::Key_Minus;   // VK_OEM_MINUS
        case 0xBE: return Qt::Key_Period;  // VK_OEM_PERIOD
        case 0x08: return Qt::Key_Backspace;
        case 0x09: return Qt::Key_Tab;
        case 0x2E: return Qt::Key_Delete;
        case 0x24: return Qt::Key_Home;
        case kVirtualSpace: return Qt::Key_Space;
        default: return 0;
    }
}

QObject* PluginEditorKeys::actionFor(int virtualKey, int modifiers, bool textField) const {
    const int key = qtKey(virtualKey);
    if (key == 0 || !target_) return nullptr;
    const auto mods = Qt::KeyboardModifiers(modifiers) & (Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
    if (!(mods & (Qt::ControlModifier | Qt::AltModifier))) {
        if (!dawKey(key) || (mods & Qt::ShiftModifier) || textField) return nullptr;
        if (session_ && session_->computerKeyboard()->takesKey(key)) return nullptr;
    }
    const QKeySequence pressed(QKeyCombination(mods, Qt::Key(key)));
    if (matches(pressed, keptForPlugin())) return nullptr;
    // The window's Actions (the menus') and its Shortcuts (their other keys), in the order they were made.
    QList<QObject*> candidates{target_.data()};
    candidates += target_->findChildren<QObject*>();
    for (QObject* candidate : candidates) {
        QVariant shortcut;
        if (candidate->inherits("QQuickAction"))
            shortcut = candidate->property("shortcut");
        else if (candidate->inherits("QQuickShortcut"))
            shortcut = QVariantList{candidate->property("sequence"), candidate->property("sequences")};
        else
            continue;
        if (!candidate->property("enabled").toBool()) continue;
        if (matches(pressed, keySequences(shortcut))) return candidate;
    }
    return nullptr;
}

bool PluginEditorKeys::keyPressed(int virtualKey, int modifiers, bool textField, bool repeat) {
    if (session_ && session_->render()->active()) return false;  // (the window takes no keys meanwhile)
    QObject* action = actionFor(virtualKey, modifiers, textField);
    if (!action) return false;
    const bool held = repeat && !(Qt::KeyboardModifiers(modifiers) & (Qt::ControlModifier | Qt::AltModifier));
    if (!held) {  // (Space held down doesn't start and stop playing over and over)
        if (action->inherits("QQuickShortcut"))
            QMetaObject::invokeMethod(action, "activated");
        else
            QMetaObject::invokeMethod(action, "trigger");
    }
    return true;
}

bool PluginEditorKeys::closeForemostEditor() {
#ifdef Q_OS_WIN
    HWND found = nullptr;
    EnumWindows(findEditor, reinterpret_cast<LPARAM>(&found));
    if (!found) return false;
    PostMessageW(found, WM_CLOSE, 0, 0);
    return true;
#else
    return false;
#endif
}

bool PluginEditorKeys::nativeEventFilter(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    if (eventType != "windows_generic_MSG" || !message) return false;
    const MSG* msg = static_cast<const MSG*>(message);
    if ((msg->message != WM_KEYDOWN && msg->message != WM_SYSKEYDOWN) || !isPluginEditor(msg->hwnd)) return false;
    constexpr LPARAM kRepeatBit = LPARAM(1) << 30;  // a WM_KEYDOWN's lParam: the key was down already
    if (!keyPressed(int(msg->wParam), pressedModifiers(), isTextField(msg->hwnd), (msg->lParam & kRepeatBit) != 0))
        return false;
    if (result) *result = 0;
    return true;
#else
    Q_UNUSED(eventType);
    Q_UNUSED(message);
    Q_UNUSED(result);
    return false;
#endif
}

}  // namespace sub::ui
