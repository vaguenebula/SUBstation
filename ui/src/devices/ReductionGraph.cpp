#include "devices/ReductionGraph.h"

#include "devices/EditorPaint.h"
#include "model/Device.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <algorithm>
#include <cmath>

namespace sub::ui {

namespace {

// numpy.nan_to_num(values, nan, posinf, neginf).max()
double level(const std::vector<float>& values) {
    double peak = -1e300;
    for (float value : values) {
        double v = value;
        if (std::isnan(v))
            v = ReductionGraph::kMeterFloorDb;
        else if (std::isinf(v))
            v = v > 0 ? 0.0 : ReductionGraph::kMeterFloorDb;
        peak = std::max(peak, v);
    }
    return peak;
}

}  // namespace

ReductionGraph::ReductionGraph(QQuickItem* parent) : DeviceCanvas(parent), history_(kHistory, 0.0f) {
    setImplicitSize(kWidth, kMinimumHeight);
}

void ReductionGraph::sync() {
    const sub::app::Device* found = device();
    threshold_ = found ? found->params.value(QStringLiteral("threshold"), -18.0) : -18.0;
    update();
}

void ReductionGraph::refreshDisplays() {
    add(readDisplay(QStringLiteral("reduction")), readDisplay(QStringLiteral("input")),
        readDisplay(QStringLiteral("output")));
}

void ReductionGraph::add(const std::vector<float>& reduction, const std::vector<float>& levelIn,
                         const std::vector<float>& levelOut) {
    if (!reduction.empty()) {
        const std::size_t count = std::min<std::size_t>(reduction.size(), kHistory);
        std::move(history_.begin() + static_cast<std::ptrdiff_t>(count), history_.end(), history_.begin());
        for (std::size_t i = 0; i < count; ++i) {
            float value = reduction[reduction.size() - count + i];
            if (std::isnan(value))
                value = 0.0f;
            else if (std::isinf(value))
                value = value > 0 ? float(kReductionRangeDb) : 0.0f;
            history_[kHistory - count + i] = std::max(value, 0.0f);
        }
    }
    if (!levelIn.empty())
        levelIn_ = level(levelIn);
    if (!levelOut.empty())
        levelOut_ = level(levelOut);
    update();
    Q_EMIT levelsChanged();
}

namespace {

void meter(SgPainter& p, const QRectF& rect, double level, const double* marker) {
    auto y = [&](double db) {
        const double fraction = (std::min(0.0, std::max(ReductionGraph::kMeterFloorDb, db)) - ReductionGraph::kMeterFloorDb) /
                                -ReductionGraph::kMeterFloorDb;
        return rect.bottom() - fraction * rect.height();
    };
    p.fillRect(rect, Theme::panel());
    const double top = y(level);
    p.fillRect(QRectF(rect.left(), top, rect.width(), rect.bottom() - top), Theme::meterLow());
    // Only the hot section changes colour, preserving the meter's level scale.
    if (level > -6.0)
        p.fillRect(QRectF(rect.left(), top, rect.width(), y(-6.0) - top), Theme::meterHigh());
    for (double db : {-48.0, -36.0, -24.0, -12.0})
        p.drawLine(QPointF(rect.left(), y(db)), QPointF(rect.right(), y(db)), withAlpha(Theme::meterBg(), 145));
    // A separate cap makes near-full-scale levels visible at a glance.
    p.fillRect(QRectF(rect.left(), rect.top() - 4, rect.width(), 2),
               level > -0.1 ? Theme::meterHigh() : withAlpha(Theme::gridBeat(), 100));
    if (marker != nullptr) {
        const double at = y(*marker);
        p.drawLine(QPointF(rect.left() - 2, at), QPointF(rect.right() + 2, at), Theme::meterBg(), 3.5);
        p.drawLine(QPointF(rect.left() - 2, at), QPointF(rect.right() + 2, at), Theme::accent(), 1.5);
        const QPointF notch[3] = {{rect.left() - 6, at - 3}, {rect.left() - 3, at}, {rect.left() - 6, at + 3}};
        p.fillPolygon(notch, 3, Theme::accent());
    }
}

}  // namespace

void ReductionGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const double w = width(), h = height();
    const QRectF outer = QRectF(0, 0, w, h).adjusted(0.5, 0.5, -0.5, -0.5);
    p.fillRoundedRect(outer, 5, 5, Theme::meterBg());
    p.drawRoundedRect(outer, 5, 5, withAlpha(Theme::gridBeat(), 150));

    // A quiet header/footer keeps text clear of the moving waveform.
    const QRectF plot(9, 26, w - 82, h - 53);
    const double meterX = w - 48;
    const QRectF meterIn(meterX, plot.top(), kMeterWidth, plot.height());
    const QRectF meterOut(meterX + 26, plot.top(), kMeterWidth, plot.height());
    const int left = Qt::AlignLeft | Qt::AlignVCenter, right = Qt::AlignRight | Qt::AlignVCenter;

    const QFont font8 = uiFont(8), font7 = uiFont(7);
    p.drawText(QRectF(9, 4, plot.width(), 18), left, QStringLiteral("Gain reduction"), Theme::textDim(), font8);
    p.drawText(QRectF(meterIn.center().x() - 13, 4, 26, 18), Qt::AlignCenter, QStringLiteral("In"), Theme::textDim(),
               font8);
    p.drawText(QRectF(meterOut.center().x() - 13, 4, 26, 18), Qt::AlignCenter, QStringLiteral("Out"),
               Theme::textDim(), font8);

    // All grid lines share the same exact mapping as the trace.
    for (int db : {0, 6, 12, 18, 24}) {
        const double y = plot.top() + db / kReductionRangeDb * plot.height();
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y), withAlpha(Theme::gridBeat(), db == 0 ? 180 : 80));
        p.drawText(QRectF(plot.right() + 2, y - 6, 18, 12), right, QString::number(db), Theme::textDim(), font7);
    }

    const double step = plot.width() / (kHistory - 1);
    std::vector<QPointF> trace(kHistory);
    for (int i = 0; i < kHistory; ++i) {
        const double depth = std::clamp(history_[std::size_t(i)] / kReductionRangeDb, 0.0, 1.0) * plot.height();
        trace[std::size_t(i)] = QPointF(plot.left() + i * step, plot.top() + depth);
    }
    QLinearGradient gradient(plot.topLeft(), plot.bottomLeft());
    gradient.setColorAt(0, withAlpha(Theme::accent(), 28));
    gradient.setColorAt(1, withAlpha(Theme::accent(), 112));
    p.save();
    p.setClipRect(plot.adjusted(-1, -1, 1, 1));
    p.fillToBaseline(trace.data(), int(trace.size()), plot.top(), gradient);
    p.drawPolyline(trace.data(), int(trace.size()), Theme::accent(), 1.5);  // only the signal edge is stroked
    p.restore();

    const double threshold = std::isfinite(threshold_) ? threshold_ : kMeterFloorDb;
    meter(p, meterIn, levelIn_, &threshold);
    meter(p, meterOut, levelOut_, nullptr);

    // The current reduction is visually distinct from the threshold annotation.
    const double footerY = plot.bottom() + 5;
    const double reduction = history_.back();
    const QString text = reduction >= 0.05 ? QStringLiteral("−%1 dB").arg(reduction, 0, 'f', 1)
                                           : QStringLiteral("0.0 dB");
    p.drawText(QRectF(9, footerY, 70, 19), left, text, Theme::accent(), uiFont(9));
    p.drawText(QRectF(80, footerY, plot.right() - 80, 19), right,
               QStringLiteral("T %1").arg(pythonFixed(threshold, 0)), Theme::textDim(), font7);
    for (const auto& [rect, level] : {std::pair{meterIn, levelIn_}, std::pair{meterOut, levelOut_}}) {
        const QString label = level <= kMeterFloorDb ? QStringLiteral("−∞") : pythonFixed(level, 0);
        p.drawText(QRectF(rect.center().x() - 13, footerY, 26, 19), Qt::AlignCenter, label, Theme::textDim(), font7);
    }
}

}  // namespace sub::ui
