#include "input/DoubleClicks.h"

#include <QGuiApplication>
#include <QStyleHints>

#include <cmath>

namespace sub::ui {

bool DoubleClicks::isSecondPress(const QMouseEvent* event) {
    const QStyleHints* hints = QGuiApplication::styleHints();
    const QPointF at = event->scenePosition();
    const bool second = !lastWasSecond_ && event->button() == lastButton_ && lastPress_ != 0 &&
                        event->timestamp() - lastPress_ < ulong(hints->mouseDoubleClickInterval()) &&
                        std::abs(at.x() - lastPressAt_.x()) <= hints->mouseDoubleClickDistance() &&
                        std::abs(at.y() - lastPressAt_.y()) <= hints->mouseDoubleClickDistance();
    lastPress_ = event->timestamp();
    lastPressAt_ = at;
    lastButton_ = event->button();
    lastWasSecond_ = second;
    return second;
}

}  // namespace sub::ui
