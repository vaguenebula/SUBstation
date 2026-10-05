#include "devices/DisplayClock.h"

#include <QCoreApplication>
#include <QMetaMethod>

namespace sub::ui {

DisplayClock* DisplayClock::instance() {
    static DisplayClock* clock = new DisplayClock();  // (the application's child: it goes with it)
    return clock;
}

DisplayClock::DisplayClock() : QObject(QCoreApplication::instance()) {
    timer_.setInterval(kDisplayRefreshMs);
    timer_.setTimerType(Qt::PreciseTimer);
    connect(&timer_, &QTimer::timeout, this, &DisplayClock::tick);
}

void DisplayClock::connectNotify(const QMetaMethod& signal) {
    if (signal == QMetaMethod::fromSignal(&DisplayClock::tick) && !timer_.isActive()) timer_.start();
}

}  // namespace sub::ui
