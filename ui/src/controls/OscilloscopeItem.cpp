#include "controls/OscilloscopeItem.h"

#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QMetaObject>
#include <QQuickWindow>
#include <QVariant>

#include <algorithm>
#include <cmath>

namespace sub::ui {

OscilloscopeItem::OscilloscopeItem(QQuickItem* parent) : SgCanvas(parent) {
    setImplicitSize(150, 30);  // oscilloscope.py's fixed size
    timer_.setInterval(kUpdateMs);
    connect(&timer_, &QTimer::timeout, this, &OscilloscopeItem::poll);
    timer_.start();
}

void OscilloscopeItem::setFeed(QObject* feed) {
    if (feed == feed_)
        return;
    feed_ = feed;
    hasWritten_ = false;
    Q_EMIT feedChanged();
}

int OscilloscopeItem::trigger(const float* samples, int count, int window) {
    const int latest = count - window;
    if (latest <= 0)
        return 0;
    for (int i = latest - 1; i >= 0; --i)
        if (samples[i] < 0.0f && samples[i + 1] >= 0.0f)
            return i + 1;
    return latest;
}

std::vector<QPointF> OscilloscopeItem::trace(const float* samples, int count, double left, int columns, double mid,
                                             double half) {
    std::vector<QPointF> points;
    if (count < 2 || columns < 1)
        return points;
    points.reserve(size_t(columns) * 2);
    // The columns' first samples, as numpy's linspace(0, count, columns + 1).astype(int)[:-1].
    const double stepSize = double(count) / columns;
    auto edge = [&](int column) { return column >= columns ? count : int(column * stepSize); };
    for (int column = 0; column < columns; ++column) {
        const int begin = edge(column);
        // numpy's reduceat: an empty run (more columns than samples) is its first sample alone.
        const int end = std::max(begin + 1, edge(column + 1));
        float high = -1.0f, low = 1.0f;
        for (int i = begin; i < end && i < count; ++i) {
            const float s = std::clamp(samples[i], -1.0f, 1.0f);
            high = std::max(high, s);
            low = std::min(low, s);
        }
        const double x = left + column + 0.5;
        const double top = mid - high * half, bottom = mid - low * half;
        points.emplace_back(x, top);
        if (bottom - top >= 1.0)
            points.emplace_back(x, bottom);
    }
    return points;
}

void OscilloscopeItem::poll() {
    if (!isVisible() || !window() || !window()->isVisible() || !feed_)
        return;
    const quint64 written = feed_->property("scopeWritten").toULongLong();
    if (hasWritten_ && written == written_) {
        if (samples_.empty())
            return;
        float peak = 0.0f;
        for (float s : samples_)
            peak = std::max(peak, std::abs(s));
        if (peak > kSilent) {
            for (float& s : samples_)
                s = float(s * kFadePerUpdate);
        } else {
            samples_.clear();  // flat: one last repaint, then idle
        }
        rebuild();
        return;
    }
    written_ = written;
    hasWritten_ = true;
    QList<float> fetched;
    if (!QMetaObject::invokeMethod(feed_, "scopeSamples", Qt::DirectConnection, Q_RETURN_ARG(QList<float>, fetched),
                                   Q_ARG(int, 2 * kWindow)))
        return;
    const int start = trigger(fetched.constData(), int(fetched.size()), kWindow);
    const int end = std::min(int(fetched.size()), start + kWindow);
    samples_.assign(fetched.constBegin() + start, fetched.constBegin() + end);
    rebuild();
}

void OscilloscopeItem::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    SgCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        rebuild();
}

void OscilloscopeItem::rebuild() {
    trace_ = trace(samples_.data(), int(samples_.size()), 1.0, int(width()) - 2, height() / 2, (height() - 4) / 2);
    update();
}

void OscilloscopeItem::paint(SgPainter& p) {
    const QRectF rect(0, 0, width(), height());
    p.fillRect(rect, Theme::kMeterBg);
    const double mid = rect.center().y();
    p.drawLine(QPointF(rect.left(), mid), QPointF(rect.right(), mid), Theme::kScopeAxis, 1.0);
    p.drawRect(rect.adjusted(0, 0, -1, -1), Theme::kBorder, 1.0);
    if (trace_.size() < 2)
        return;
    p.setClipRect(rect.adjusted(1, 1, -1, -1));
    // The wide glow is soft anyway, so it skips antialiasing; the line keeps it.
    p.drawPolyline(trace_.data(), int(trace_.size()), Theme::kScopeGlow, 3.0);
    p.setAntialiasing(true);
    p.drawPolyline(trace_.data(), int(trace_.size()), Theme::kScopeLine, 1.0);
}

}  // namespace sub::ui
