#pragma once

// Telling a double-click's second press from a first. Qt Quick delivers that
// press before the double-click (widgets didn't): an item whose first click
// changes what a double-click does skips the second press.

#include <QMouseEvent>
#include <QPointF>

namespace sub::ui {

class DoubleClicks {
public:
    // Notes a press: true if it is a double-click's second, as QGuiApplication
    // decides to follow a press with a double-click (the same button, soon
    // enough and near enough; a third press starts again).
    bool isSecondPress(const QMouseEvent* event);

private:
    ulong lastPress_ = 0;  // the last press's timestamp, where and with which button
    QPointF lastPressAt_;
    Qt::MouseButton lastButton_ = Qt::NoButton;
    bool lastWasSecond_ = false;
};

}  // namespace sub::ui
