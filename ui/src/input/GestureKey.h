#pragma once

// The undo merge key of one gesture: every edit a drag (or a run of wheel
// notches, or a reset) makes with the same key is one undo step. QML gets
// one with each Knob's and ValueBox's `moved`; the items' gestures make their
// own.

#include <QElapsedTimer>
#include <QString>
#include <QUuid>

namespace sub::ui {

// A new gesture key.
inline QString newGestureKey() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

// The key of a burst of wheel notches: notches less than kWindowMs apart share
// one (one undo step); one after a longer pause starts a new one.
class WheelGesture {
public:
    static constexpr qint64 kWindowMs = 400;

    // The key of a notch now.
    QString key() {
        if (key_.isEmpty() || !clock_.isValid() || clock_.elapsed() > kWindowMs)
            key_ = newGestureKey();
        clock_.start();
        return key_;
    }

private:
    QString key_;
    QElapsedTimer clock_;  // since the last notch
};

}  // namespace sub::ui
