#include "devices/GateKeyGraph.h"

#include "audio/EngineBridge.h"
#include "audio/GateResponse.h"
#include "input/GestureKey.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QHoverEvent>
#include <QLinearGradient>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

namespace {

QColor mix(const QColor& from, const QColor& to, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(float(from.redF() + (to.redF() - from.redF()) * t),
                            float(from.greenF() + (to.greenF() - from.greenF()) * t),
                            float(from.blueF() + (to.blueF() - from.blueF()) * t));
}

}  // namespace

GateKeyGraph::GateKeyGraph(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(254, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
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

QRectF GateKeyGraph::plot() const { return QRectF(0, 0, width(), height()).adjusted(1, 1, -1, -1); }

LogAxis GateKeyGraph::frequencyAxis() const {
    const QRectF r = plot();
    return {kLow, kHigh, r.left(), r.width()};
}

double GateKeyGraph::xOf(double hz) const { return frequencyAxis().position(hz); }

double GateKeyGraph::freqAt(double x) const { return frequencyAxis().valueAt(x); }

double GateKeyGraph::yOf(double db) const {
    const QRectF r = plot().adjusted(0, 4, 0, -4);  // (the dot stays inside at the ends of the range)
    if (!std::isfinite(db))
        db = db > 0 ? kRangeDb : -kRangeDb;
    return r.center().y() - std::clamp(db, -kRangeDb, kRangeDb) / kRangeDb * r.height() / 2;
}

double GateKeyGraph::dbAt(double y) const {
    const QRectF r = plot().adjusted(0, 4, 0, -4);
    return std::clamp((r.center().y() - y) / (r.height() / 2) * kRangeDb, -kRangeDb, kRangeDb);
}

QPointF GateKeyGraph::dot() const {
    const double at = usesGain_ ? gain_
                                : sub::app::gateKeyFilterDb(type_, freq_, q_, gain_, sampleRate(), {freq_}).value(0);
    return QPointF(xOf(std::clamp(freq_, kLow, kHigh)), yOf(at));
}

void GateKeyGraph::sync() {
    active_ = value(QStringLiteral("sc_eq")) >= 0.5;
    type_ = std::clamp(int(std::lround(value(QStringLiteral("sc_eq_type")))), 0, 5);
    freq_ = value(QStringLiteral("sc_eq_freq"));
    q_ = value(QStringLiteral("sc_eq_q"));
    gain_ = value(QStringLiteral("sc_eq_gain"));
    usesGain_ = sub::app::gateKeyFilterUsesGain(type_);
    activeEase_.target = active_ ? 1.0 : 0.0;
    if (!clock_.isValid())  // (the first time: as it is, no easing)
        activeEase_.snap(activeEase_.target);
    updateCurve();
    update();
}

void GateKeyGraph::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        updateCurve();
}

void GateKeyGraph::updateCurve() {
    const int columns = std::max(2, int(plot().width()));
    QList<double> frequencies;
    frequencies.reserve(columns + 1);
    for (int i = 0; i <= columns; ++i)
        frequencies.append(kLow * std::pow(kHigh / kLow, double(i) / columns));
    const QList<double> response = sub::app::gateKeyFilterDb(type_, freq_, q_, gain_, sampleRate(), frequencies);
    frequencies_.assign(frequencies.begin(), frequencies.end());
    response_.assign(response.begin(), response.end());
    Q_EMIT curveChanged();
}

void GateKeyGraph::refreshDisplays() {
    double dt = 0.0;
    if (clock_.isValid())
        dt = std::clamp(double(clock_.nsecsElapsed()) * 1e-9, 0.0, 0.05);
    clock_.start();
    hoverEase_.target = hovered_ || !gesture_.isEmpty() ? 1.0 : 0.0;
    bool moving = activeEase_.step(easeFraction(dt, 0.1), 1e-3);
    moving = hoverEase_.step(easeFraction(dt, 0.08), 1e-3) || moving;
    if (moving)
        update();
}

// --- The mouse -----------------------------------------------------------------------------

bool GateKeyGraph::onDot(const QPointF& pos) const {
    const QPointF at = dot();
    return std::hypot(pos.x() - at.x(), pos.y() - at.y()) <= 8.0;
}

void GateKeyGraph::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    if (secondPressOfDoubleClick(event) || device() == nullptr)
        return;  // (the double-click follows)
    // Anywhere: the dot, relative to where the press was (nothing jumps).
    gesture_ = newGestureKey();
    lastPos_ = event->position();
    dragFreqX_ = xOf(std::clamp(freq_, kLow, kHigh));
    dragQ_ = q_;
    dragGain_ = gain_;
    touch(QStringLiteral("sc_eq_freq"));
    update();
}

void GateKeyGraph::mouseMoveEvent(QMouseEvent* event) {
    if (gesture_.isEmpty())
        return;
    const QPointF pos = event->position();
    const double fine = event->modifiers() & Qt::ShiftModifier ? 0.1 : 1.0;
    const double dx = (pos.x() - lastPos_.x()) * fine, up = (lastPos_.y() - pos.y()) * fine;
    lastPos_ = pos;
    dragFreqX_ += dx;
    sub::app::OrderedMap<QString, double> values;
    values.insert(QStringLiteral("sc_eq_freq"), std::clamp(freqAt(dragFreqX_), kFreqMin, kFreqMax));
    if (usesGain_) {
        const QRectF r = plot().adjusted(0, 4, 0, -4);
        dragGain_ += up / std::max(1.0, r.height() / 2) * kRangeDb;
        values.insert(QStringLiteral("sc_eq_gain"), std::clamp(dragGain_, kGainMin, kGainMax));
    } else {
        dragQ_ *= std::pow(2.0, up / kQPixels);
        values.insert(QStringLiteral("sc_eq_q"), std::clamp(dragQ_, kQMin, kQMax));
    }
    setParams(values, gesture_, QStringLiteral("Change Gate Key Filter"));
}

void GateKeyGraph::endDrag() {
    if (gesture_.isEmpty())
        return;
    gesture_.clear();
    update();
}

void GateKeyGraph::mouseReleaseEvent(QMouseEvent* event) {
    endDrag();
    hovered_ = onDot(event->position());
}

void GateKeyGraph::mouseUngrabEvent() { endDrag(); }

void GateKeyGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton)
        return;
    endDrag();
    setParams({{QStringLiteral("sc_eq_freq"), defaultValue(QStringLiteral("sc_eq_freq"))},
               {QStringLiteral("sc_eq_q"), defaultValue(QStringLiteral("sc_eq_q"))},
               {QStringLiteral("sc_eq_gain"), defaultValue(QStringLiteral("sc_eq_gain"))}},
              QString(), QStringLiteral("Reset Gate Key Filter"));
}

void GateKeyGraph::hoverMoveEvent(QHoverEvent* event) { hovered_ = onDot(event->position()); }

void GateKeyGraph::hoverLeaveEvent(QHoverEvent*) { hovered_ = false; }

// --- Painting ------------------------------------------------------------------------------

void GateKeyGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    p.fillRoundedRect(QRectF(0, 0, width(), height()), 3, 3, Theme::kMeterBg);
    const QRectF r = plot();
    drawDecadeGrid(p, r, frequencyAxis());
    const double zero = yOf(0.0);
    p.drawLine(QPointF(r.left(), zero), QPointF(r.right(), zero), withAlpha(Theme::kGridBar, 160));

    const double on = std::clamp(activeEase_.value, 0.0, 1.0);
    const QColor color = mix(Theme::kTextDisabled, Theme::kAccent, on);
    points_.resize(frequencies_.size());
    for (std::size_t i = 0; i < frequencies_.size(); ++i)
        points_[i] = QPointF(xOf(frequencies_[i]), yOf(response_[i]));
    if (points_.size() >= 2) {
        p.save();
        p.setClipRect(r);
        if (on > 0.01) {  // under the curve: the band that keys the gate
            QLinearGradient fill(r.topLeft(), r.bottomLeft());
            fill.setColorAt(0, withAlpha(Theme::kAccent, int(std::lround(70 * on))));
            fill.setColorAt(1, withAlpha(Theme::kAccent, int(std::lround(12 * on))));
            p.fillToBaseline(points_.data(), int(points_.size()), r.bottom(), fill);
        }
        drawGlowPolyline(p, points_, color, 1.3);
        p.restore();
    }

    // The dot where it is tuned, a ring round it while hovered or dragged.
    const QPointF at = dot();
    const double hover = std::clamp(hoverEase_.value, 0.0, 1.0);
    if (hover > 0.01)
        p.drawEllipse(QRectF(at.x() - 6.5, at.y() - 6.5, 13, 13), withAlpha(color, int(std::lround(200 * hover))), 1.2);
    p.fillEllipse(at, 4, 4, color);
    p.fillEllipse(at, 1.6, 1.6, Theme::kMeterBg);

    if (on < 0.99) {
        p.save();
        p.setOpacity(1.0 - on);
        p.drawText(QRectF(r.right() - 64, r.top() + 2, 60, 12), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("EQ off"), Theme::kTextDim, uiFont(7));
        p.restore();
    }
}

}  // namespace sub::ui
