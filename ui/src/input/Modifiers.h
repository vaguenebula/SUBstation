#pragma once

// What the modifier keys mean in every view, read from events alike
// everywhere. Ctrl is the shortcut modifier: on macOS Qt reports Command as
// Qt::ControlModifier (and the Control key as Meta), so there these mean
// Command, as Mac users expect.

#include <QKeyEvent>

namespace sub::ui {

// Ctrl+Alt: drag to scroll (the arrangement, the piano roll, the device chain), as in Ableton.
inline bool isPanModifier(Qt::KeyboardModifiers modifiers) {
    return (modifiers & Qt::ControlModifier) && (modifiers & Qt::AltModifier);
}

// Whether a shortcut's modifiers (Ctrl, Alt or Meta) are held: a key pressed with them isn't typing.
inline bool hasShortcutModifier(Qt::KeyboardModifiers modifiers) {
    return modifiers & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
}

// The modifiers held once a key event is through: a modifier key's own press
// or release isn't in its event's modifiers on every platform (X11 reports the
// state before it).
inline Qt::KeyboardModifiers heldModifiers(const QKeyEvent* event) {
    Qt::KeyboardModifiers modifiers = event->modifiers();
    Qt::KeyboardModifier own = Qt::NoModifier;
    switch (event->key()) {
        case Qt::Key_Control: own = Qt::ControlModifier; break;
        case Qt::Key_Alt: own = Qt::AltModifier; break;
        case Qt::Key_Shift: own = Qt::ShiftModifier; break;
        case Qt::Key_Meta: own = Qt::MetaModifier; break;
        default: break;
    }
    if (own != Qt::NoModifier) modifiers.setFlag(own, event->type() == QEvent::KeyPress);
    return modifiers;
}

}  // namespace sub::ui
