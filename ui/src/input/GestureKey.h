#pragma once

// The undo merge key of one gesture: every edit a drag (or a run of wheel
// notches, or a reset) makes with the same key is one undo step. QML gets
// one with each Knob's and ValueBox's `moved`; the items' gestures make their
// own.

#include <QString>
#include <QUuid>

namespace sub::ui {

// A new gesture key.
inline QString newGestureKey() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

}  // namespace sub::ui
