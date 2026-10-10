#pragma once

// Reading a QML Action's or Shortcut's `shortcut` as Qt Quick does: a string
// is portable text ("Ctrl+Shift+S", never translated), a
// QKeySequence::StandardKey its platform's bindings (Copy: Ctrl+C, Command+C
// on macOS). The menus' shortcut text and the keys taken from plug-in editors
// read it here, so they agree with what Qt Quick binds.

#include <QKeySequence>
#include <QList>
#include <QVariant>

namespace sub::ui {

// Its key sequences: from a string, a QKeySequence::StandardKey, a
// QKeySequence, or a list of them (none for anything else).
QList<QKeySequence> keySequences(const QVariant& shortcut);

}  // namespace sub::ui
