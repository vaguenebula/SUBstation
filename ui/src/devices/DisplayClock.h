#pragma once

// What paces the device editors' displays (the EQ's spectrum, the Delay's,
// the Compressor's and the Sidechain's graphs, the Sampler's playhead): a tick
// about 60 times a second, as the displays were made for (the EQ's spectrum
// rises and falls by a share of the way per refresh). One for the whole
// application, running from when a display first listens; the tests tick it
// by hand.

#include <QObject>
#include <QTimer>

namespace sub::ui {

inline constexpr int kDisplayRefreshMs = 16;

class DisplayClock : public QObject {
    Q_OBJECT

public:
    static DisplayClock* instance();

Q_SIGNALS:
    void tick();

protected:
    void connectNotify(const QMetaMethod& signal) override;

private:
    DisplayClock();

    QTimer timer_;
};

}  // namespace sub::ui
