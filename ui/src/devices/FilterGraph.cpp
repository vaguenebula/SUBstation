#include "devices/FilterGraph.h"

#include "controls/KnobItem.h"
#include "devices/EditorPaint.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

FilterGraph::FilterGraph(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(kWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton);
    setCursor(Qt::SizeAllCursor);
}

double FilterGraph::response(double freq, double center, double width) {
    const double low = center * std::pow(2.0, -width / 2), high = center * std::pow(2.0, width / 2);
    const double hp = std::pow(freq / low, 2) / std::sqrt(1 + std::pow(freq / low, 4));
    const double lp = 1 / std::sqrt(1 + std::pow(freq / high, 4));
    return 20 * std::log10(std::max(hp * lp, 1e-9));
}

QRectF FilterGraph::plot() const { return QRectF(0, 0, width(), height()).adjusted(1, 1, -1, -1); }

LogAxis FilterGraph::frequencyAxis() const {
    const QRectF r = plot();
    return {kLow, kHigh, r.left(), r.width()};
}

double FilterGraph::xOf(double freq) const { return frequencyAxis().position(freq); }

double FilterGraph::freqAt(double x) const { return frequencyAxis().valueAt(x); }

double FilterGraph::dotY(double width) const {
    const QRectF inner = plot().adjusted(0, 8, 0, -8);
    return inner.bottom() - (width - kWidthMin) / (kWidthMax - kWidthMin) * inner.height();
}

double FilterGraph::widthAt(double y) const {
    const QRectF inner = plot().adjusted(0, 8, 0, -8);
    const double fraction = std::clamp((inner.bottom() - y) / inner.height(), 0.0, 1.0);
    return kWidthMin + fraction * (kWidthMax - kWidthMin);
}

void FilterGraph::sync() {
    on_ = value(QStringLiteral("filter")) >= 0.5;
    center_ = value(QStringLiteral("freq"));
    width_ = value(QStringLiteral("width"));
    update();
}

void FilterGraph::refreshDisplays() { addSamples(readDisplay(QStringLiteral("input")), sampleRate()); }

void FilterGraph::addSamples(const std::vector<float>& samples, double sampleRate) {
    if (spectrum_.add(samples.data(), samples.size(), sampleRate)) {
        updateColumns();
        update();
    }
}

void FilterGraph::updateColumns() {
    const int columns = std::max(2, int(plot().width()));
    columns_ = spectrum_.columns(columns, kLow, kHigh);
}

void FilterGraph::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        updateColumns();
}

// --- Dragging -------------------------------------------------------------------------

void FilterGraph::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    gesture_ = newGestureKey();
    touch(QStringLiteral("freq"));
    dragTo(event->position());
}

void FilterGraph::mouseMoveEvent(QMouseEvent* event) {
    if (!gesture_.isEmpty())
        dragTo(event->position());
}

void FilterGraph::mouseReleaseEvent(QMouseEvent*) { gesture_.clear(); }

void FilterGraph::mouseUngrabEvent() { gesture_.clear(); }

void FilterGraph::dragTo(const QPointF& pos) {
    const double width = std::nearbyint(widthAt(pos.y()) * 100.0) / 100.0;
    setParams({{QStringLiteral("freq"), std::clamp(freqAt(pos.x()), 50.0, 18000.0)},
               {QStringLiteral("width"), width}},
              gesture_);
}

// --- Painting ---------------------------------------------------------------------------

void FilterGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF r = plot();
    p.fillRect(QRectF(0, 0, width(), height()), Theme::meterBg());
    drawDecadeGrid(p, r, frequencyAxis());

    // The input's spectrum, behind the curve.
    const int columns = int(columns_.size());
    if (columns >= 2 && *std::max_element(columns_.begin(), columns_.end()) > sub::app::analysis::FallingSpectrum::kFloorDb) {
        const double floor = sub::app::analysis::FallingSpectrum::kFloorDb;
        std::vector<QPointF> shape;
        shape.reserve(std::size_t(columns) + 2);
        shape.emplace_back(r.left(), r.bottom());
        for (int i = 0; i < columns; ++i) {
            const double h = std::clamp((columns_[std::size_t(i)] - floor) / -floor, 0.0, 1.0) * r.height();
            shape.emplace_back(r.left() + (i + 0.5) * r.width() / columns, r.bottom() - h);
        }
        shape.emplace_back(r.right(), r.bottom());
        p.fillToBaseline(shape.data(), int(shape.size()), r.bottom(), withAlpha(Theme::text(), 34));
        p.drawPolygon(shape.data(), int(shape.size()), withAlpha(Theme::text(), 70), 1.0);
    }

    const QColor color = on_ ? Theme::scopeLine() : Theme::textDisabled();
    const double top = r.top() + 10;
    auto y = [&](double db) { return top + std::clamp(db / kFloorDb, 0.0, 1.0) * (r.bottom() - top); };
    const int steps = std::max(2, int(r.width()));
    std::vector<QPointF> curve;
    curve.reserve(std::size_t(steps) + 1);
    for (int i = 0; i <= steps; ++i) {
        const double x = r.left() + i * r.width() / steps;
        curve.emplace_back(x, y(on_ ? response(freqAt(x), center_, width_) : 0.0));
    }
    p.fillToBaseline(curve.data(), int(curve.size()), r.bottom(), withAlpha(color, 28));
    p.drawPolyline(curve.data(), int(curve.size()), color, 1.5);

    const QPointF dot(xOf(center_), dotY(width_));
    p.drawEllipse(QRectF(dot.x() - 5, dot.y() - 5, 10, 10), on_ ? Theme::accent() : Theme::textDim(), 2);
}

}  // namespace sub::ui
