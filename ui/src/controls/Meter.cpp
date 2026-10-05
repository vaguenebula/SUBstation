#include "controls/Meter.h"

#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QLinearGradient>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

Meter::Meter(QQuickItem* parent) : SgCanvas(parent) {
    setAcceptedMouseButtons(Qt::AllButtons);
    setImplicitSize(10, 40);  // at least 8 wide, as the Python UI's meter
}

double Meter::fraction(double level) {
    if (level <= 0.0)
        return 0.0;
    const double db = 20.0 * std::log10(level);
    return std::clamp((db - kFloorDb) / (kCeilingDb - kFloorDb), 0.0, 1.0);
}

void Meter::setLevels(qreal left, qreal right) {
    bool changed = false;
    const double levels[2] = {left, right};
    for (int i = 0; i < 2; ++i) {
        const double value = std::max(fraction(levels[i]), display_[i] - kFallPerUpdate);
        if (std::abs(value - display_[i]) > 1e-4) {
            display_[i] = value;
            changed = true;
        }
    }
    if (changed)
        Q_EMIT levelsChanged();
    if (std::max(left, right) >= 1.0 && !clipped_) {
        clipped_ = true;
        changed = true;
        Q_EMIT clippedChanged();
    }
    if (changed)
        update();
}

void Meter::reset() {
    display_[0] = display_[1] = 0.0;
    Q_EMIT levelsChanged();
    clearClip();
    update();
}

void Meter::clearClip() {
    if (!clipped_)
        return;
    clipped_ = false;
    Q_EMIT clippedChanged();
    update();
}

void Meter::mousePressEvent(QMouseEvent*) {
    clearClip();  // a click clears the clip light
}

void Meter::paint(SgPainter& p) {
    const QRectF rect(0, 0, width(), height());
    p.fillRect(rect, Theme::kMeterBg);
    const double clipHeight = 3.0;
    const QRectF bars = rect.adjusted(1, clipHeight + 1, -1, -1);
    const double gap = 1.0;
    const double barWidth = (bars.width() - gap) / 2;
    const double zeroDb = 1.0 - (0.0 - kFloorDb) / (kCeilingDb - kFloorDb);
    QLinearGradient gradient(0, bars.bottom(), 0, bars.top());
    gradient.setColorAt(0.0, Theme::kMeterLow);
    gradient.setColorAt(std::max(0.0, 1.0 - zeroDb - 0.18), Theme::kMeterLow);
    gradient.setColorAt(std::max(0.0, 1.0 - zeroDb - 0.05), Theme::kMeterMid);
    gradient.setColorAt(1.0 - zeroDb, Theme::kMeterHigh);
    gradient.setColorAt(1.0, Theme::kMeterHigh);
    for (int i = 0; i < 2; ++i) {
        if (display_[i] <= 0.0)
            continue;
        const double h = bars.height() * display_[i];
        p.fillRect(QRectF(bars.left() + i * (barWidth + gap), bars.bottom() - h, barWidth, h), gradient);
    }
    if (clipped_)
        p.fillRect(QRectF(rect.left() + 1, rect.top() + 1, rect.width() - 2, clipHeight - 1), Theme::kMeterHigh);
}

}  // namespace sub::ui
