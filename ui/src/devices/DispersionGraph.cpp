#include "devices/DispersionGraph.h"

#include "audio/DisperserResponse.h"
#include "audio/EngineBridge.h"
#include "controls/KnobItem.h"
#include "devices/EditorPaint.h"
#include "model/ParamSpec.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

DispersionGraph::DispersionGraph(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(kWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton);
    setCursor(Qt::SizeAllCursor);
    // The curve is the engine's at its sample rate: worked out again when the audio device changes.
    connect(this, &DeviceCanvas::deviceChanged, this, [this] {
        disconnect(bridgeConnection_);
        if (session())
            bridgeConnection_ = connect(session()->bridge(), &sub::app::EngineBridge::deviceChanged, this, [this] {
                updateCurve();
                update();
            });
    });
}

double DispersionGraph::niceRange(double ms) {
    double range = 0.1;
    if (!std::isfinite(ms))
        return range;
    while (range < ms) {
        for (const double step : {2.0, 2.5, 2.0}) {  // 0.1 -> 0.2 -> 0.5 -> 1 ...
            range *= step;
            if (range >= ms)
                break;
        }
    }
    return range;
}

QRectF DispersionGraph::plot() const { return QRectF(0, 0, width(), height()).adjusted(1, 16, -1, -1); }

double DispersionGraph::xOf(double freq) const {
    const QRectF r = plot();
    return r.left() + std::log(freq / kLow) / std::log(kHigh / kLow) * r.width();
}

double DispersionGraph::freqAt(double x) const {
    const QRectF r = plot();
    const double fraction = std::clamp((x - r.left()) / r.width(), 0.0, 1.0);
    return kLow * std::pow(kHigh / kLow, fraction);
}

double DispersionGraph::yOf(double ms) const {
    const QRectF r = plot();
    return r.bottom() - std::clamp(ms / range_, 0.0, 1.0) * r.height();
}

QPointF DispersionGraph::dot() const { return QPointF(xOf(std::clamp(tuned_, kLow, kHigh)), yOf(tunedDelay_)); }

void DispersionGraph::sync() {
    stages_ = std::clamp(static_cast<int>(std::lround(value(QStringLiteral("amount")))), 0, 64);
    freq_ = value(QStringLiteral("freq"));
    pinch_ = value(QStringLiteral("pinch"));
    bypassed_ = value(QStringLiteral("bypass")) >= 0.5;
    updateCurve();
    update();
}

void DispersionGraph::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        updateCurve();
}

void DispersionGraph::updateCurve() {
    const double rate = sampleRate();
    const int columns = std::max(2, int(plot().width()));
    QList<double> frequencies;
    frequencies.reserve(columns + 3);
    for (int i = 0; i <= columns; ++i)
        frequencies.append(kLow * std::pow(kHigh / kLow, double(i) / columns));
    // High up its peak can be narrower than a column: it is drawn wherever it is.
    tuned_ = sub::app::disperserTunedFrequency(freq_, rate);
    for (const double extra : {sub::app::disperserPeakFrequency(freq_, pinch_, rate), tuned_}) {
        if (kLow < extra && extra < kHigh)
            frequencies.insert(std::lower_bound(frequencies.begin(), frequencies.end(), extra), extra);
    }
    const QList<double> delays = sub::app::disperserGroupDelayMs(stages_, freq_, pinch_, rate, frequencies);
    frequencies_.assign(frequencies.begin(), frequencies.end());
    delays_.assign(delays.begin(), delays.end());
    peak_ = delays_.empty() ? 0.0 : *std::max_element(delays_.begin(), delays_.end());
    range_ = niceRange(peak_);
    tunedDelay_ = sub::app::disperserGroupDelayMs(stages_, freq_, pinch_, rate, {tuned_}).value(0);
    Q_EMIT curveChanged();
}

// --- Dragging -------------------------------------------------------------------------

void DispersionGraph::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    gesture_ = newGestureKey();
    pressedAt_ = event->position();
    pressedPinch_ = pinch_;
    touch(QStringLiteral("freq"));
    dragTo(event->position());
}

void DispersionGraph::mouseMoveEvent(QMouseEvent* event) {
    if (!gesture_.isEmpty())
        dragTo(event->position());
}

void DispersionGraph::mouseReleaseEvent(QMouseEvent*) { gesture_.clear(); }

void DispersionGraph::mouseUngrabEvent() { gesture_.clear(); }

void DispersionGraph::dragTo(const QPointF& pos) {
    const double pinch = pressedPinch_ * std::pow(2.0, (pressedAt_.y() - pos.y()) / kPinchPixels);
    const double rounded = std::round(std::clamp(pinch, 0.1, 10.0) * 1000.0) / 1000.0;
    setParams({{QStringLiteral("freq"), std::clamp(freqAt(pos.x()), kLow, kHigh)}, {QStringLiteral("pinch"), rounded}},
              gesture_);
}

// --- Painting ---------------------------------------------------------------------------

namespace {

// "0.5 ms", "20 ms", "2 s": the axis' figures.
QString axisText(double ms) {
    return ms >= 1000.0 ? pythonGeneral(ms / 1000.0) + QStringLiteral(" s") : pythonGeneral(ms) + QStringLiteral(" ms");
}

}  // namespace

void DispersionGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF r = plot();
    p.fillRect(QRectF(0, 0, width(), height()), Theme::kMeterBg);
    for (double decade : {10.0, 100.0, 1000.0, 10000.0}) {  // a line per decade, fainter ones between
        for (int multiple = 1; multiple < 10; ++multiple) {
            const double freq = decade * multiple;
            if (kLow < freq && freq < kHigh) {
                const double x = xOf(freq);
                p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()),
                           withAlpha(Theme::kGridBeat, multiple == 1 ? 200 : 90));
            }
        }
    }
    const QFont font = uiFont(7);
    for (const double fraction : {0.5, 1.0}) {  // the delay axis: its top and half way, figures under the lines
        const double y = yOf(fraction * range_);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y), withAlpha(Theme::kGridBeat, fraction == 1.0 ? 160 : 90));
        p.drawText(QRectF(r.left() + 3, y + 1, 60, 12), Qt::AlignLeft | Qt::AlignVCenter, axisText(fraction * range_),
                   Theme::kTextDim, font);
    }
    p.drawText(QRectF(r.left() + 3, r.bottom() - 13, 60, 12), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("0"),
               Theme::kTextDim, font);

    const bool on = !bypassed_ && stages_ > 0;
    const QColor color = on ? Theme::kScopeLine : Theme::kTextDisabled;
    std::vector<QPointF> curve;
    curve.reserve(frequencies_.size());
    for (size_t i = 0; i < frequencies_.size(); ++i) curve.emplace_back(xOf(frequencies_[i]), yOf(delays_[i]));
    if (curve.size() >= 2) {
        p.save();
        p.setClipRect(r.adjusted(-1, -1, 1, 1));
        p.fillToBaseline(curve.data(), int(curve.size()), r.bottom(), withAlpha(color, 28));
        p.drawPolyline(curve.data(), int(curve.size()), color, 1.5);
        p.restore();
    }

    // The dot where the stages are tuned, and what it reads.
    const QPointF at = dot();
    p.drawEllipse(QRectF(at.x() - 5, at.y() - 5, 10, 10), on ? Theme::kAccent : Theme::kTextDim, 2);
    const QString readout = bypassed_ ? QStringLiteral("Bypassed")
                                      : QStringLiteral("%1 at %2").arg(sub::app::formatValue(tunedDelay_, QStringLiteral("ms")),
                                                                      sub::app::formatValue(tuned_, QStringLiteral("Hz")));
    p.drawText(QRectF(r.left(), 1, r.width() - 3, 14), Qt::AlignRight | Qt::AlignVCenter, readout,
               on ? Theme::kText : Theme::kTextDim, font);
    p.drawText(QRectF(r.left() + 3, 1, 90, 14), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Group delay"),
               Theme::kTextDim, font);
}

}  // namespace sub::ui
