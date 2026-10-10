#include "devices/SaturatorColorGraph.h"

#include "audio/EngineBridge.h"
#include "audio/SaturatorResponse.h"
#include "devices/EditorPaint.h"
#include "input/GestureKey.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QHoverEvent>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

using sub::app::analysis::EqAnalyzer;

namespace {

// A spectrum's columns smoothed a little (1-2-1, twice): calmer between a low note's harmonics,
// where the columns are narrower than the FFT's bins.
std::vector<double> smoothed(std::vector<double> db) {
    const std::size_t n = db.size();
    if (n < 3)
        return db;
    std::vector<double> pass(n);
    for (int twice = 0; twice < 2; ++twice) {
        pass.front() = db.front();
        pass.back() = db.back();
        for (std::size_t i = 1; i + 1 < n; ++i)
            pass[i] = 0.25 * db[i - 1] + 0.5 * db[i] + 0.25 * db[i + 1];
        db.swap(pass);
    }
    return db;
}

const QString kBase = QStringLiteral("base");
const QString kFreq = QStringLiteral("freq");
const QString kDepth = QStringLiteral("depth");
const QString kColor = QStringLiteral("color");

}  // namespace

SaturatorColorGraph::SaturatorColorGraph(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(kWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    makeColumns();
    // The curve is the engine's at its sample rate: worked out again when the audio device changes.
    connect(this, &DeviceCanvas::deviceChanged, this, [this] {
        disconnect(bridgeConnection_);
        if (session())
            bridgeConnection_ = connect(session()->bridge(), &sub::app::EngineBridge::deviceChanged, this, [this] {
                updateCurve(true);
                update();
            });
    });
}

QRectF SaturatorColorGraph::plot() const { return QRectF(0, 0, width(), height()).adjusted(1, 1, -1, -1); }

LogAxis SaturatorColorGraph::frequencyAxis() const {
    const QRectF r = plot();
    return {kLow, kHigh, r.left(), r.width()};
}

double SaturatorColorGraph::xOf(double hz) const { return frequencyAxis().position(hz); }

double SaturatorColorGraph::yOfDb(double db) const {
    const QRectF r = plot();
    return r.center().y() - std::clamp(db, -kRangeDb, kRangeDb) / kRangeDb * r.height() / 2;
}

QPointF SaturatorColorGraph::baseHandle() const { return QPointF(xOf(kBaseHandleHz), yOfDb(baseHandleDb_)); }

QPointF SaturatorColorGraph::peakHandle() const {
    return QPointF(xOf(std::clamp(peakHandleHz_, kLow, kHigh)), yOfDb(peakHandleDb_));
}

bool SaturatorColorGraph::live() const { return inLive_ || outLive_; }

void SaturatorColorGraph::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        makeColumns();
        updateCurve(baseEased_.value == baseEased_.target && logFreqEased_.value == logFreqEased_.target &&
                    widthEased_.value == widthEased_.target && depthEased_.value == depthEased_.target);
        inCols_.clear();
        outCols_.clear();
    }
}

void SaturatorColorGraph::makeColumns() {
    const int n = std::max(2, int(plot().width()));
    columns_.resize(std::size_t(n));
    for (int i = 0; i < n; ++i)
        columns_[std::size_t(i)] = kLow * std::pow(kHigh / kLow, double(i) / (n - 1));
}

void SaturatorColorGraph::sync() {
    base_ = value(kBase);
    freq_ = value(kFreq);
    width_ = value(QStringLiteral("width"));
    depth_ = value(kDepth);
    on_ = value(kColor) >= 0.5;
    baseEased_.target = base_;
    logFreqEased_.target = std::log(std::max(freq_, 1.0));
    widthEased_.target = width_;
    depthEased_.target = depth_;
    onEased_.target = on_ ? 1.0 : 0.0;
    if (!synced_ || !gesture_.isEmpty()) {
        // The first, or under the mouse: no lag.
        baseEased_.snap(baseEased_.target);
        logFreqEased_.snap(logFreqEased_.target);
        widthEased_.snap(widthEased_.target);
        depthEased_.snap(depthEased_.target);
        if (!synced_)
            onEased_.snap(onEased_.target);
        updateCurve(true);
    } else {
        settled_ = false;  // (the next tick eases towards it)
    }
    // (not before the device is there: until then value() gives the defaults, and its real settings would
    // ease in from them each time the editor opens)
    synced_ = device() != nullptr;
    update();
}

void SaturatorColorGraph::updateCurve(bool exact) {
    const double base = exact ? base_ : baseEased_.value;
    const double freq = exact ? freq_ : std::exp(logFreqEased_.value);
    const double width = exact ? width_ : widthEased_.value;
    const double depth = exact ? depth_ : depthEased_.value;
    const double rate = sampleRate();
    const QList<double> db =
        sub::app::saturatorColorDb(base, freq, width, depth, rate, QList<double>(columns_.begin(), columns_.end()));
    curveDb_.assign(db.begin(), db.end());
    const QList<double> handles = sub::app::saturatorColorDb(base, freq, width, depth, rate, {kBaseHandleHz, freq});
    baseHandleDb_ = handles.value(0);
    peakHandleDb_ = handles.value(1);
    peakHandleHz_ = freq;
}

// --- Displays and animation ---------------------------------------------------------------

void SaturatorColorGraph::feed(EqAnalyzer::Channel channel, const std::vector<float>& samples, double dt) {
    const double rate = sampleRate();
    // The values come a block of audio at a time: a tick without any soon after some is a gap between
    // blocks (longer than a tick), not silence. The spectrum holds until none has come for kHoldSeconds.
    double& quiet = quietFor_[channel];
    if (samples.empty()) {
        quiet += dt;
        if (quiet > kHoldSeconds && analyzer_.live(channel))
            analyzer_.feed(channel, nullptr, 0, rate);  // (falls back)
        return;
    }
    quiet = 0.0;
    // Silence once its window is all silent, and the spectrum down: nothing to work out.
    const bool silent = std::all_of(samples.begin(), samples.end(), [](float s) { return s == 0.0f; });
    int& zeros = zeros_[channel];
    zeros = silent ? std::min<int>(EqAnalyzer::kFftSize, zeros + int(samples.size())) : 0;
    if (zeros >= EqAnalyzer::kFftSize && !analyzer_.live(channel))
        return;
    analyzer_.feed(channel, samples.data(), samples.size(), rate);
}

void SaturatorColorGraph::refreshDisplays() {
    const double dt = tickSeconds();
    const bool wasInLive = inLive_, wasOutLive = outLive_;
    feed(EqAnalyzer::Input, readDisplay(QStringLiteral("input")), dt);
    feed(EqAnalyzer::Output, readDisplay(QStringLiteral("output")), dt);
    inLive_ = analyzer_.live(EqAnalyzer::Input);
    outLive_ = analyzer_.live(EqAnalyzer::Output);
    // (once more as one falls silent, to draw it at the floor)
    if (inLive_ || wasInLive)
        inCols_ = smoothed(analyzer_.columns(EqAnalyzer::Input, columns_));
    if (outLive_ || wasOutLive)
        outCols_ = smoothed(analyzer_.columns(EqAnalyzer::Output, columns_));

    const double fraction = easeFraction(dt, kEaseSeconds);
    bool moving = baseEased_.step(fraction);
    moving = logFreqEased_.step(fraction, 1e-6) || moving;
    moving = widthEased_.step(fraction) || moving;
    moving = depthEased_.step(fraction) || moving;
    if (moving) {
        // Once there, the curve is the parameters' exactly (exp(log f) can be an ulp off).
        const bool there = baseEased_.value == baseEased_.target && logFreqEased_.value == logFreqEased_.target &&
                           widthEased_.value == widthEased_.target && depthEased_.value == depthEased_.target;
        updateCurve(there);
    }
    moving = onEased_.step(easeFraction(dt, 0.08), 1e-3) || moving;

    const bool wasSettled = settled_;
    settled_ = !moving && !inLive_ && !outLive_ && !wasInLive && !wasOutLive;
    if (!settled_ || !wasSettled)
        update();
}

// --- Dragging -------------------------------------------------------------------------------

SaturatorColorGraph::Handle SaturatorColorGraph::handleAt(const QPointF& pos) const {
    auto near = [&](const QPointF& handle) {
        return std::hypot(pos.x() - handle.x(), pos.y() - handle.y()) <= kHandleHit;
    };
    if (near(peakHandle()))
        return Handle::Peak;
    if (near(baseHandle()))
        return Handle::Base;
    return Handle::None;
}

void SaturatorColorGraph::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    const bool second = secondPressOfDoubleClick(event);
    const Handle handle = handleAt(event->position());
    if (handle == Handle::None) {
        event->ignore();  // (the frame's: selecting the device)
        return;
    }
    if (second)
        return;  // (the double-click follows)
    gesture_ = newGestureKey();
    handle_ = handle;
    setHovered(handle);
    lastAt_ = event->position();
    movedAcross_ = movedUp_ = 0.0;
    dragBase_ = base_;
    dragFreqX_ = xOf(freq_);
    dragDepth_ = depth_;
    baseRange_ = sub::app::saturatorRange(kBase);
    freqRange_ = sub::app::saturatorRange(kFreq);
    depthRange_ = sub::app::saturatorRange(kDepth);
    if (handle == Handle::Base)
        touch(kBase);  // (the peak's first moves say which of its two: setParams shows the first one's automation)
    update();
}

void SaturatorColorGraph::mouseMoveEvent(QMouseEvent* event) {
    if (gesture_.isEmpty())
        return;
    const QPointF pos = event->position();
    const double fine = event->modifiers() & Qt::ShiftModifier ? kFine : 1.0;
    const double up = (lastAt_.y() - pos.y()) * 2.0 * kRangeDb / std::max(1.0, plot().height()) * fine;
    const double across = (pos.x() - lastAt_.x()) * fine;
    movedAcross_ += std::abs(pos.x() - lastAt_.x());
    movedUp_ += std::abs(pos.y() - lastAt_.y());
    lastAt_ = pos;
    auto rounded = [](double v, double to) { return std::round(v / to) * to; };
    // The same parameters every move (one undo step), the one moved most first: its automation shows.
    if (handle_ == Handle::Base) {
        // (the handle is on the shelf at 60 Hz, which moves a little less than Amt Lo: so it follows the mouse)
        dragBase_ = baseRange_.clamp(dragBase_ + up / baseHandleSlope(dragBase_));
        setParams({{kBase, rounded(dragBase_, 0.01)}, {kColor, 1.0}}, gesture_,
                  QStringLiteral("Change Saturator Color"));
    } else if (handle_ == Handle::Peak) {
        dragFreqX_ = std::clamp(dragFreqX_ + across, xOf(freqRange_.low), xOf(freqRange_.high));
        dragDepth_ = depthRange_.clamp(dragDepth_ + up);
        const double freq = rounded(freqRange_.clamp(frequencyAxis().valueAt(dragFreqX_)), 0.1);
        const double depth = rounded(dragDepth_, 0.01);
        sub::app::OrderedMap<QString, double> values;
        if (movedUp_ > movedAcross_)
            values.insert(kDepth, depth);
        values.insert(kFreq, freq);
        if (movedUp_ <= movedAcross_)
            values.insert(kDepth, depth);
        values.insert(kColor, 1.0);
        setParams(values, gesture_, QStringLiteral("Change Saturator Color"));
    }
}

double SaturatorColorGraph::baseHandleSlope(double base) const {
    const sub::app::SaturatorRange range = sub::app::saturatorRange(kBase);
    const double low = std::max(range.low, base - 0.5), high = std::min(range.high, base + 0.5);
    const QList<double> at = {kBaseHandleHz};
    const double rate = sampleRate();
    const double rise = sub::app::saturatorColorDb(high, freq_, width_, depth_, rate, at).value(0) -
                        sub::app::saturatorColorDb(low, freq_, width_, depth_, rate, at).value(0);
    return std::max(0.25, rise / (high - low));
}

void SaturatorColorGraph::mouseReleaseEvent(QMouseEvent* event) {
    gesture_.clear();
    handle_ = Handle::None;
    setHovered(handleAt(event->position()));
    update();
}

void SaturatorColorGraph::mouseUngrabEvent() {
    gesture_.clear();
    handle_ = Handle::None;
    setHovered(Handle::None);
    update();
}

void SaturatorColorGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    const Handle handle = event->button() == Qt::LeftButton ? handleAt(event->position()) : Handle::None;
    if (handle == Handle::None) {
        event->ignore();
        return;
    }
    gesture_.clear();
    handle_ = Handle::None;
    setParams({{handle == Handle::Base ? kBase : kDepth, 0.0}}, QString(), QStringLiteral("Change Saturator Color"));
}

void SaturatorColorGraph::setHovered(Handle handle) {
    if (handle == hovered_)
        return;
    hovered_ = handle;
    if (handle == Handle::None)
        unsetCursor();
    else
        setCursor(Qt::PointingHandCursor);
    update();
}

void SaturatorColorGraph::hoverMoveEvent(QHoverEvent* event) { setHovered(handleAt(event->position())); }

void SaturatorColorGraph::hoverLeaveEvent(QHoverEvent*) {
    if (gesture_.isEmpty())
        setHovered(Handle::None);
}

// --- Painting -------------------------------------------------------------------------------

void SaturatorColorGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF r = plot();
    p.fillRect(QRectF(0, 0, width(), height()), Theme::kMeterBg);
    drawDecadeGrid(p, r, frequencyAxis());
    for (const double db : {-24.0, -12.0, 12.0, 24.0})
        p.drawLine(QPointF(r.left(), yOfDb(db)), QPointF(r.right(), yOfDb(db)), Theme::kGridSub);
    const double zero = yOfDb(0.0);
    p.drawLine(QPointF(r.left(), zero), QPointF(r.right(), zero), Theme::kGridBar);

    p.save();
    p.setClipRect(r);
    const std::size_t n = columns_.size();
    auto xAt = [&](std::size_t i) {
        return r.left() + double(i) / double(std::max<std::size_t>(1, n - 1)) * r.width();
    };
    auto spectrumY = [&](double db) {
        const double fraction = (db - EqAnalyzer::kFloorDb) / (EqAnalyzer::kCeilDb - EqAnalyzer::kFloorDb);
        return r.bottom() - std::clamp(fraction, 0.0, 1.0) * r.height();
    };
    // The input's spectrum filled, the output's a line over it.
    if (inLive_ && inCols_.size() == n && n >= 2) {
        std::vector<QPointF> shape(n);
        for (std::size_t i = 0; i < n; ++i)
            shape[i] = QPointF(xAt(i), spectrumY(inCols_[i]));
        p.fillToBaseline(shape.data(), int(n), r.bottom(), withAlpha(Theme::kTextDim, 56));
    }
    if (outLive_ && outCols_.size() == n && n >= 2) {
        std::vector<QPointF> line(n);
        for (std::size_t i = 0; i < n; ++i)
            line[i] = QPointF(xAt(i), spectrumY(outCols_[i]));
        p.drawPolyline(line.data(), int(n), withAlpha(Theme::kScopeLine, 120), 1.0);
    }

    // The EQ: lit while Color is on, grey while off.
    const double on = onEased_.value;
    const QColor color = mixColor(Theme::kTextDisabled, Theme::kAccent, on);
    if (curveDb_.size() == n && n >= 2) {
        std::vector<QPointF> curve(n);
        for (std::size_t i = 0; i < n; ++i)
            curve[i] = QPointF(xAt(i), yOfDb(curveDb_[i]));
        if (on > 0.01)
            p.fillToBaseline(curve.data(), int(n), zero, withAlpha(Theme::kAccent, int(28 * on)));
        if (on > 0.5)
            drawGlowPolyline(p, curve, color, 1.5);
        else
            p.drawPolyline(curve.data(), int(n), color, 1.2);
    }
    p.restore();

    // The handles: Amt Lo's on the shelf, the peak's at its top.
    const QColor ring = on > 0.5 ? Theme::kAccent : Theme::kTextDim;
    for (const Handle handle : {Handle::Base, Handle::Peak}) {
        const QPointF at = handle == Handle::Base ? baseHandle() : peakHandle();
        const bool lit = hovered_ == handle || handle_ == handle;
        if (lit) {
            p.fillEllipse(at, 9, 9, withAlpha(ring, 60));
            p.fillEllipse(at, 4.5, 4.5, ring);
        } else {
            p.fillEllipse(at, 4.5, 4.5, Theme::kMeterBg);
        }
        p.drawEllipse(QRectF(at.x() - 4.5, at.y() - 4.5, 9, 9), ring, 1.5);
    }

    // Which spectrum is which, on a dark backing (the output's line runs along the bottom right).
    const QFont font = uiFont(7);
    const QString in = QStringLiteral("In"), out = QStringLiteral("Out");
    const double outWidth = SgPainter::textWidth(out, font);
    const double legendWidth = SgPainter::textWidth(in, font) + kLegendGap + outWidth;
    const QRectF corner(r.right() - 3 - legendWidth, r.bottom() - 13, legendWidth, 12);
    p.fillRoundedRect(corner.adjusted(-2, 0, 2, 0), 2, 2, withAlpha(Theme::kMeterBg, 200));
    p.drawText(corner, Qt::AlignRight | Qt::AlignVCenter, out, withAlpha(Theme::kScopeLine, 200), font);
    p.drawText(corner.adjusted(0, 0, -outWidth - kLegendGap, 0), Qt::AlignRight | Qt::AlignVCenter, in, Theme::kTextDim,
               font);
}

}  // namespace sub::ui
