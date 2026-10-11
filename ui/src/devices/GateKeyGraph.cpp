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
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

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

QPointF GateKeyGraph::dot() const { return QPointF(xOf(std::clamp(freq_, kLow, kHigh)), yOf(dotDb_)); }

void GateKeyGraph::sync() {
    active_ = value(QStringLiteral("sc_eq")) >= 0.5;
    type_ = std::clamp(int(std::lround(value(QStringLiteral("sc_eq_type")))), 0, 5);
    freq_ = value(QStringLiteral("sc_eq_freq"));
    q_ = value(QStringLiteral("sc_eq_q"));
    gain_ = value(QStringLiteral("sc_eq_gain"));
    usesGain_ = sub::app::gateKeyFilterUsesGain(type_);
    usesQ_ = sub::app::gateKeyFilterUsesQ(type_);
    activeEase_.target = active_ ? 1.0 : 0.0;
    if (!synced_)  // (the first time: as it is, no easing)
        activeEase_.snap(activeEase_.target);
    synced_ = true;
    updateCurve();
    update();
}

void GateKeyGraph::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        updateCurve();
}

// The curve and the dot's level, worked out here (the GUI thread) so paint() only reads them (the
// engine's rate is the bridge's, read under the engine's lock).
void GateKeyGraph::updateCurve() {
    const int columns = std::max(2, int(plot().width()));
    QList<double> frequencies;
    frequencies.reserve(columns + 2);
    for (int i = 0; i <= columns; ++i)
        frequencies.append(kLow * std::pow(kHigh / kLow, double(i) / columns));
    frequencies.append(freq_);  // (the dot's, last)
    const QList<double> response = sub::app::gateKeyFilterDb(type_, freq_, q_, gain_, sampleRate(), frequencies);
    frequencies_.assign(frequencies.begin(), frequencies.end() - 1);
    response_.assign(response.begin(), response.end() - 1);
    dotDb_ = usesGain_ ? gain_ : response.back();
    Q_EMIT curveChanged();
}

void GateKeyGraph::refreshDisplays() {
    const double dt = tickSeconds();
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
    values.insert(QStringLiteral("sc_eq_freq"), sub::app::gateKeyFreqRange().clamp(freqAt(dragFreqX_)));
    // Up and down: the Q for the pass filters (and the bell's with Ctrl, as the EQ's bands), else the gain.
    if (usesQ_ && (!usesGain_ || (event->modifiers() & Qt::ControlModifier))) {
        dragQ_ *= std::pow(2.0, up / kQPixels);
        values.insert(QStringLiteral("sc_eq_q"), sub::app::gateKeyQRange().clamp(dragQ_));
    } else {
        const QRectF r = plot().adjusted(0, 4, 0, -4);
        dragGain_ += up / std::max(1.0, r.height() / 2) * kRangeDb;
        values.insert(QStringLiteral("sc_eq_gain"), sub::app::gateKeyGainRange().clamp(dragGain_));
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

void GateKeyGraph::wheelEvent(QWheelEvent* event) {
    const int delta = event->angleDelta().y() ? event->angleDelta().y() : event->angleDelta().x();
    // Over the dot (or while it is dragged): its Q; elsewhere the wheel goes on (the chain scrolls).
    if (!usesQ_ || delta == 0 || device() == nullptr || (gesture_.isEmpty() && !onDot(event->position()))) {
        event->ignore();
        return;
    }
    event->accept();
    const QString burst = wheelGesture_.key();  // (notches close together are one step)
    const double step = event->modifiers() & Qt::ShiftModifier ? 1.03 : 1.15;
    const double q = sub::app::gateKeyQRange().clamp(q_ * std::pow(step, delta / 120.0));
    if (!gesture_.isEmpty())
        dragQ_ = q;  // (a drag goes on from it)
    setParams({{QStringLiteral("sc_eq_q"), q}}, burst, QStringLiteral("Change Gate Key Filter Q"));
}

// --- Painting ------------------------------------------------------------------------------

void GateKeyGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    p.fillRoundedRect(QRectF(0, 0, width(), height()), 3, 3, Theme::meterBg());
    const QRectF r = plot();
    drawDecadeGrid(p, r, frequencyAxis());
    const double zero = yOf(0.0);
    p.drawLine(QPointF(r.left(), zero), QPointF(r.right(), zero), withAlpha(Theme::gridBar(), 160));

    const double on = std::clamp(activeEase_.value, 0.0, 1.0);
    const QColor color = mixColor(Theme::textDisabled(), Theme::accent(), on);
    points_.resize(frequencies_.size());
    for (std::size_t i = 0; i < frequencies_.size(); ++i)
        points_[i] = QPointF(xOf(frequencies_[i]), yOf(response_[i]));
    if (points_.size() >= 2) {
        p.save();
        p.setClipRect(r);
        if (on > 0.01) {  // under the curve: the band that keys the gate
            QLinearGradient fill(r.topLeft(), r.bottomLeft());
            fill.setColorAt(0, withAlpha(Theme::accent(), int(std::lround(70 * on))));
            fill.setColorAt(1, withAlpha(Theme::accent(), int(std::lround(12 * on))));
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
    p.fillEllipse(at, 1.6, 1.6, Theme::meterBg());

    if (on < 0.99) {
        p.save();
        p.setOpacity(1.0 - on);
        p.drawText(QRectF(r.right() - 64, r.top() + 2, 60, 12), Qt::AlignRight | Qt::AlignVCenter | Qt::TextDontClip,
                   QStringLiteral("EQ off"), Theme::textDim(), uiFont(7));
        p.restore();
    }
}

}  // namespace sub::ui
