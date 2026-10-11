#include "devices/AmpToneGraph.h"

#include "audio/AmpResponse.h"
#include "audio/EngineBridge.h"
#include "controls/KnobItem.h"
#include "input/GestureKey.h"
#include "model/ParamSpec.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QHoverEvent>
#include <QList>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

namespace {

constexpr double kMorphSeconds = 0.04;   // a new model's curve (through smoothstep: mostly there in 0.12 s)
constexpr double kHandleSeconds = 0.06;  // a handle growing under the mouse

QString dialText(double value) { return sub::app::formatValue(value, QStringLiteral("dial")); }

// A dial's value as a drag or the wheel leaves it: within its range, to a hundredth.
double dialValue(double value, const AmpToneGraph::Range& range) {
    return std::round(range.clamp(value) * 100.0) / 100.0;
}

// The gutter the figures need in `font`.
double gutterFor(const QFont& font) {
    double widest = 0.0;
    for (const double db : AmpToneGraph::kFigureDbs)
        widest = std::max(widest, SgPainter::textWidth(AmpToneGraph::figureText(db), font));
    return std::max(AmpToneGraph::kGutter, std::ceil(widest) + 4.0);
}

}  // namespace

AmpToneGraph::AmpToneGraph(QQuickItem* parent) : DeviceCanvas(parent), gutter_(gutterFor(figureFont())) {
    setImplicitSize(kWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    morph_.snap(1.0);
    for (Eased& r : radius_) r.snap(kRadius);
    // The curve is the engine's at its sample rate: worked out again when the audio device changes.
    connect(this, &DeviceCanvas::deviceChanged, this, [this] {
        rangesRead_ = false;
        disconnect(bridgeConnection_);
        if (session())
            bridgeConnection_ = connect(session()->bridge(), &sub::app::EngineBridge::deviceChanged, this, [this] {
                if (model_ >= 0)
                    updateCurve();
                update();
            });
    });
}

QString AmpToneGraph::figureText(double db) {
    return db == 0.0 ? QStringLiteral("0") : QString::number(db).replace(QLatin1Char('-'), QChar(0x2212));
}

QFont AmpToneGraph::figureFont() { return uiFont(7); }

QRectF AmpToneGraph::plot() const { return QRectF(0, 0, width(), height()).adjusted(1, 1, -1 - gutter_, -1); }

LogAxis AmpToneGraph::frequencyAxis() const {
    const QRectF r = plot();
    return {kLow, kHigh, r.left(), r.width()};
}

double AmpToneGraph::xOf(double hz) const { return frequencyAxis().position(hz); }

double AmpToneGraph::yOf(double db) const { return dbToY(db, plot(), kFloorDb, kCeilingDb); }

double AmpToneGraph::shownAt(double hz) const {
    const auto it = std::lower_bound(frequencies_.begin(), frequencies_.end(), hz);
    if (it == frequencies_.end() || shown_.size() != frequencies_.size())
        return 0.0;
    return shown_[size_t(it - frequencies_.begin())];
}

QPointF AmpToneGraph::handlePos(int i) const {
    if (i < 0 || i >= kHandles)
        return {};
    const double hz = kHandleList[size_t(i)].frequency;
    return QPointF(xOf(hz), yOf(shownAt(hz)));
}

int AmpToneGraph::handleAt(QPointF pos) const {
    int found = -1;
    double nearest = kHitPixels;
    for (int i = 0; i < kHandles; ++i) {
        const double distance = std::abs(pos.x() - xOf(kHandleList[size_t(i)].frequency));
        if (distance <= nearest) {
            nearest = distance;
            found = i;
        }
    }
    return found;
}

double AmpToneGraph::handleRadius(int i) const { return i >= 0 && i < kHandles ? radius_[size_t(i)].value : 0.0; }

QString AmpToneGraph::handleAutomation(int i) const {
    return i >= 0 && i < kHandles ? automation_[size_t(i)] : QString();
}

double AmpToneGraph::wheelStep(int i) const {
    const Range range = handleRange(i);
    return (range.high - range.low) / KnobItem::kWheelNotches;
}

bool AmpToneGraph::readRanges() {
    int found = 0;
    for (const sub::app::ProcessorParam& param : session()->bridge()->deviceParams(trackId(), deviceId())) {
        for (int i = 0; i < kHandles; ++i) {
            if (param.id == QLatin1String(kHandleList[size_t(i)].id)) {
                ranges_[size_t(i)] = {param.minValue, param.maxValue};
                ++found;
            }
        }
    }
    return found == kHandles;
}

void AmpToneGraph::sync() {
    if (device() == nullptr) {  // (not yet, as it is being made: the first sync with it shows its curve at once)
        rangesRead_ = false;
        update();
        return;
    }
    if (!rangesRead_)
        rangesRead_ = readRanges();
    const int model =
        std::clamp(static_cast<int>(std::lround(value(QStringLiteral("type")))), 0, sub::app::ampModelCount() - 1);
    for (int i = 0; i < kHandles; ++i) {
        const QString id = QString::fromLatin1(kHandleList[size_t(i)].id);
        dials_[i] = value(id);
        automation_[size_t(i)] = automationState(id);
    }
    const bool newModel = model_ >= 0 && model != model_;
    if (newModel && shown_.size() == target_.size() && !shown_.empty()) {  // morphs from the curve as drawn
        from_ = shown_;
        morph_.value = 0.0;
        morph_.target = 1.0;
    }
    model_ = model;
    updateCurve();
    update();
}

void AmpToneGraph::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size() && model_ >= 0)
        updateCurve();
}

void AmpToneGraph::updateCurve() {
    const int columns = std::max(2, int(plot().width()));
    QList<double> frequencies;
    frequencies.reserve(columns + 1 + kHandles);
    for (int i = 0; i <= columns; ++i) frequencies.append(kLow * std::pow(kHigh / kLow, double(i) / columns));
    for (const Handle& handle : kHandleList)  // exactly where the handles sit
        frequencies.insert(std::lower_bound(frequencies.begin(), frequencies.end(), handle.frequency),
                           handle.frequency);
    const QList<double> response = sub::app::ampToneResponseDb(std::max(model_, 0), dials_[0], dials_[1], dials_[2],
                                                               dials_[3], sampleRate(), frequencies);
    frequencies_.assign(frequencies.begin(), frequencies.end());
    target_.assign(response.begin(), response.end());
    if (morph_.value >= 1.0 || from_.size() != target_.size()) {
        morph_.snap(1.0);
        shown_ = target_;
    } else {
        shown_.resize(target_.size());
        const double t = smoothstep(morph_.value);  // (eases in and out: a morph starts and lands gently)
        for (size_t k = 0; k < target_.size(); ++k) shown_[k] = from_[k] + (target_[k] - from_[k]) * t;
    }
    Q_EMIT curveChanged();
}

bool AmpToneGraph::stepHandles(double dt) {
    bool moving = false;
    for (int i = 0; i < kHandles; ++i) {
        Eased& r = radius_[size_t(i)];
        r.target = i == hovered_ || i == dragging_ ? kHoverRadius : kRadius;
        moving = r.step(easeFraction(dt, kHandleSeconds), 1e-3) || moving;
    }
    return moving;
}

void AmpToneGraph::refreshDisplays() {
    const double dt = tickSeconds();
    bool moving = stepHandles(dt);
    if (morph_.step(easeFraction(dt, kMorphSeconds))) {
        moving = true;
        if (morph_.value >= 1.0 || from_.size() != target_.size()) {
            shown_ = target_;  // (exactly: from + (target - from) * 1 can be an ulp off)
        } else {
            const double t = smoothstep(morph_.value);
            for (size_t k = 0; k < target_.size(); ++k) shown_[k] = from_[k] + (target_[k] - from_[k]) * t;
        }
        Q_EMIT curveChanged();
    }
    if (moving)
        update();
}

// --- The mouse ---------------------------------------------------------------------------

void AmpToneGraph::mousePressEvent(QMouseEvent* event) {
    const bool second = secondPressOfDoubleClick(event);
    const int handle = handleAt(event->position());
    if (handle < 0) {
        event->ignore();  // (the frame's: it selects the device, its menu, its double-click)
        return;
    }
    const QString id = QString::fromLatin1(kHandleList[size_t(handle)].id);
    if (event->button() == Qt::RightButton) {  // the parameter's menu, as its knob's
        Q_EMIT handleMenuRequested(id, event->position());
        return;
    }
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    // A double-click's second press: its double-click follows, and a drag would carry the old value
    // back. Before the device's ranges are there, nothing to drag.
    if (second || !rangesRead_)
        return;
    gesture_ = newGestureKey();
    dragging_ = handle;
    lastY_ = event->position().y();
    dragValue_ = value(id);
    touch(id);
    setCursor(Qt::SizeVerCursor);
    Q_EMIT handlesChanged();
    update();
}

void AmpToneGraph::mouseMoveEvent(QMouseEvent* event) {
    if (gesture_.isEmpty() || dragging_ < 0)
        return;
    // On from where the mouse was last, so pressing or letting go of Shift mid-drag
    // changes the rate from here on (not the whole drag), as the knobs do.
    const double scale = event->modifiers().testFlag(Qt::ShiftModifier) ? kFine : 1.0;
    const double y = event->position().y();
    const Range& range = ranges_[size_t(dragging_)];
    dragValue_ = range.clamp(dragValue_ + (lastY_ - y) / kPixelsPerStep * scale);
    lastY_ = y;
    setParams({{QString::fromLatin1(kHandleList[size_t(dragging_)].id), dialValue(dragValue_, range)}}, gesture_);
}

void AmpToneGraph::endDrag() {
    gesture_.clear();
    if (dragging_ >= 0) {
        dragging_ = -1;
        Q_EMIT handlesChanged();
        update();
    }
}

void AmpToneGraph::mouseReleaseEvent(QMouseEvent* event) {
    endDrag();
    const int handle = handleAt(event->position());
    setHovered(contains(event->position()) ? handle : -1);
}

void AmpToneGraph::mouseUngrabEvent() { endDrag(); }

void AmpToneGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    const int handle = event->button() == Qt::LeftButton ? handleAt(event->position()) : -1;
    if (handle < 0) {
        event->ignore();
        return;
    }
    endDrag();
    const QString id = QString::fromLatin1(kHandleList[size_t(handle)].id);
    setParams({{id, defaultValue(id)}}, newGestureKey());
}

void AmpToneGraph::wheelEvent(QWheelEvent* event) {
    const int handle = dragging_ >= 0 ? dragging_ : handleAt(event->position());
    const int delta = event->angleDelta().y() ? event->angleDelta().y() : event->angleDelta().x();
    if (handle < 0 || delta == 0 || !rangesRead_) {
        event->ignore();  // (the view scrolls)
        return;
    }
    event->accept();
    const QString id = QString::fromLatin1(kHandleList[size_t(handle)].id);
    const Range& range = ranges_[size_t(handle)];
    const double step = wheelStep(handle) * (event->modifiers().testFlag(Qt::ShiftModifier) ? kFine : 1.0);
    if (dragging_ >= 0) {  // the drag goes on from the new value, in its undo step
        dragValue_ = range.clamp(dragValue_ + delta / 120.0 * step);
        setParams({{id, dialValue(dragValue_, range)}}, gesture_);
        return;
    }
    // A burst of notches is one undo step. (One on another handle shares the burst's key but not its
    // step: an edit merges only into one of the same parameters.)
    touch(id);
    setParams({{id, dialValue(value(id) + delta / 120.0 * step, range)}}, wheelGesture_.key());
}

void AmpToneGraph::setHovered(int handle) {
    if (handle != hovered_) {
        hovered_ = handle;
        Q_EMIT handlesChanged();
        update();
    }
    if (handle >= 0 || dragging_ >= 0)
        setCursor(Qt::SizeVerCursor);
    else
        unsetCursor();
}

void AmpToneGraph::hoverMoveEvent(QHoverEvent* event) {
    if (dragging_ < 0)
        setHovered(handleAt(event->position()));
}

void AmpToneGraph::hoverLeaveEvent(QHoverEvent*) {
    if (dragging_ < 0)
        setHovered(-1);
}

// --- Painting ---------------------------------------------------------------------------

void AmpToneGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF r = plot();
    p.fillRoundedRect(QRectF(0, 0, width(), height()), 4, 4, Theme::kMeterBg);
    drawDecadeGrid(p, r, frequencyAxis());
    const QFont font = figureFont();
    for (const double db : kFigureDbs) {  // the dB lines, their figures in the gutter
        const double y = yOf(db);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y),
                   db == 0.0 ? withAlpha(Theme::kGridBar, 220) : withAlpha(Theme::kGridBeat, 200));
        p.drawText(QRectF(r.right() + 1, y - 6, gutter_ - 3, 12), Qt::AlignRight | Qt::AlignVCenter, figureText(db),
                   Theme::kTextDim, font);
    }

    std::vector<QPointF> curve;
    curve.reserve(frequencies_.size());
    for (size_t i = 0; i < frequencies_.size() && i < shown_.size(); ++i)
        curve.emplace_back(xOf(frequencies_[i]), yOf(shown_[i]));
    if (curve.size() >= 2) {
        p.save();
        p.setClipRect(r);
        p.fillToBaseline(curve.data(), int(curve.size()), yOf(0.0), withAlpha(Theme::kAccent, 34));
        drawGlowPolyline(p, curve, Theme::kAccent, 1.5);
        p.restore();
    }
    p.drawText(QRectF(r.left() + 4, r.top() + 2, 60, 12), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Tone"),
               Theme::kTextDim, font);

    // The handles, their letters over them (under them near the top) with their parameters'
    // automation dots; the dragged one filled, what it sets beside it instead of its letter.
    const QFont letterFont = uiFont(7, true);
    for (int i = 0; i < kHandles; ++i) {
        const QPointF at = handlePos(i);
        const double radius = radius_[size_t(i)].value;
        const bool dragged = i == dragging_;
        p.fillEllipse(at, radius, radius, dragged ? Theme::kAccent : Theme::kMeterBg);
        p.drawEllipse(QRectF(at.x() - radius, at.y() - radius, 2 * radius, 2 * radius), Theme::kAccent, 1.5);
        if (dragged)
            continue;
        const bool below = at.y() - r.top() < 12.0 + radius;
        const QRectF letter(at.x() - 8, below ? at.y() + radius + 1 : at.y() - radius - 11, 16, 10);
        p.drawText(letter, Qt::AlignCenter, QString::fromLatin1(kHandleList[size_t(i)].letter), Theme::kText,
                   letterFont);
        drawAutomationDot(p, automation_[size_t(i)], QPointF(at.x() + 7.5, letter.center().y()));
    }
    if (dragging_ >= 0) {
        const QPointF at = handlePos(dragging_);
        const QString text = QStringLiteral("%1 %2").arg(QString::fromLatin1(kHandleList[size_t(dragging_)].name),
                                                         dialText(dials_[dragging_]));
        const double w = SgPainter::textWidth(text, font) + 10, h = 14;
        const double gap = radius_[size_t(dragging_)].value + 5;
        const double x = at.x() + gap + w <= r.right() ? at.x() + gap : at.x() - gap - w;
        const QRectF box(x, std::clamp(at.y() - h / 2, r.top() + 1, r.bottom() - h - 1), w, h);
        p.fillRoundedRect(box, 3, 3, withAlpha(Theme::kMeterBg, 220));
        p.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3, withAlpha(Theme::kAccent, 120));
        p.drawText(box, Qt::AlignCenter, text, Theme::kText, font);
    }
}

}  // namespace sub::ui
