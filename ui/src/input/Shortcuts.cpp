#include "input/Shortcuts.h"

#include <QVariantList>

namespace sub::ui {

QList<QKeySequence> keySequences(const QVariant& shortcut) {
    QList<QKeySequence> out;
    if (!shortcut.isValid()) return out;
    if (shortcut.typeId() == QMetaType::QVariantList) {
        for (const QVariant& each : shortcut.toList()) out << keySequences(each);
    } else if (shortcut.typeId() == QMetaType::QKeySequence) {
        out << shortcut.value<QKeySequence>();
    } else if (shortcut.typeId() == QMetaType::QString) {
        out << QKeySequence::fromString(shortcut.toString(), QKeySequence::PortableText);
    } else if (shortcut.canConvert<int>()) {
        out << QKeySequence::keyBindings(static_cast<QKeySequence::StandardKey>(shortcut.toInt()));
    }
    return out;
}

}  // namespace sub::ui
