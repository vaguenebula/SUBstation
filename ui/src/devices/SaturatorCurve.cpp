#include "devices/SaturatorCurve.h"

#include "devices/EditorPaint.h"
#include "input/GestureKey.h"
#include "model/ParamSpec.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

using sub::app::SaturatorShape;

namespace {

// The loudest of a display's values (linear peaks), 0 for none or nonsense.
double loudest(const std::vector<float>& values) {
    double most = 0.0;
    for (const float value : values) {
        if (std::isfinite(value))
            most = std::max(most, double(std::abs(value)));
    }
    return most;
}

double toDb(double level) { return 20.0 * std::log10(std::max(level, 1e-6)); }

const QString kDrive = QStringLiteral("drive");
const QString kThreshold = QStringLiteral("threshold");
const QString kCurve = QStringLiteral("ws_curve");

}  // namespace

SaturatorCurve::SaturatorCurve(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(kWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton);
    setCursor(Qt::SizeVerCursor);
    morph_.snap(1.0);
    in_.reset(kMeterFloorDb);
    out_.reset(kMeterFloorDb);
    makeInputs();
}

QRectF SaturatorCurve::plot() const {
    return QRectF(1, 1, std::max(2.0, width() - 2 - kStrip - kGap), std::max(2.0, height() - 2 - kStrip - kGap));
}

double SaturatorCurve::xOf(double input) const {
    const QRectF r = plot();
    return r.center().x() + input * r.width() / 2;
}

double SaturatorCurve::yOf(double output) const {
    const QRectF r = plot();
    // (held a little past the plot: what is drawn is clipped to it)
    return r.center().y() - std::clamp(output, -1.3, 1.3) / kOutputRange * r.height() / 2;
}

void SaturatorCurve::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        makeInputs();
        sync();
    }
}

void SaturatorCurve::makeInputs() {
    // Two points per pixel across, from -1 to 1 (0 among them).
    const int half = std::max(8, int(std::floor(plot().width())));
    inputs_.resize(std::size_t(2 * half + 1));
    for (int i = 0; i <= 2 * half; ++i)
        inputs_[std::size_t(i)] = double(i - half) / half;
}

std::vector<double> SaturatorCurve::curveOf(const SaturatorShape& shape) const {
    const QList<double> outputs = sub::app::saturatorCurve(shape, QList<double>(inputs_.begin(), inputs_.end()));
    return std::vector<double>(outputs.begin(), outputs.end());
}

double SaturatorCurve::curveAt(double input) const {
    if (target_.size() < 2)
        return input;
    const double at = (std::clamp(input, -1.0, 1.0) + 1.0) / 2.0 * double(target_.size() - 1);
    const std::size_t i = std::min(std::size_t(at), target_.size() - 2);
    const double f = at - double(i);
    return target_[i] + (target_[i + 1] - target_[i]) * f;
}

int SaturatorCurve::bassShaperType() { return sub::app::saturatorBassShaperType(); }

int SaturatorCurve::waveshaperType() { return sub::app::saturatorWaveshaperType(); }

int SaturatorCurve::hqLatency() { return sub::app::saturatorHqLatency(); }

void SaturatorCurve::sync() {
    SaturatorShape shape;
    // (the type and Post Clip as indices: the engine's makeShape() and clipAt() hold them to their lists)
    shape.type = int(std::lround(value(QStringLiteral("type"))));
    shape.driveDb = value(kDrive);
    shape.thresholdDb = value(kThreshold);
    shape.wsDrive = value(QStringLiteral("ws_drive"));
    shape.wsLin = value(QStringLiteral("ws_lin"));
    shape.wsCurve = value(kCurve);
    shape.wsDamp = value(QStringLiteral("ws_damp"));
    shape.wsDepth = value(QStringLiteral("ws_depth"));
    shape.wsPeriod = value(QStringLiteral("ws_period"));
    shape.clip = int(std::lround(value(QStringLiteral("clip"))));
    shape_ = shape;
    mix_ = value(QStringLiteral("mix"));
    hq_ = value(QStringLiteral("hq")) >= 0.5;
    slope_ = sub::app::saturatorSlope(shape_);
    thresholdInput_ = sub::app::saturatorThresholdInput(shape_.thresholdDb, shape_.driveDb);

    std::vector<double> target = curveOf(shape_);
    if (!synced_ || drawn_.size() != target.size() || !gesture_.isEmpty()) {
        // The first, a new size, or under the mouse: no lag.
        drawn_ = target;
        morph_.snap(1.0);
    } else if (target != target_) {
        // From wherever the drawing is now (an automation's run of changes follows smoothly).
        from_ = drawn_;
        morph_.value = 0.0;
        morph_.target = 1.0;
        settled_ = false;
    }
    target_ = std::move(target);
    // (not before the device is there: until then value() gives the defaults, and its real settings would
    // morph in from them each time the editor opens)
    synced_ = device() != nullptr;
    setCursor(bass() || waveshaper() ? Qt::SizeAllCursor : Qt::SizeVerCursor);
    update();
}

// --- Displays and animation ---------------------------------------------------------------

void SaturatorCurve::refreshDisplays() {
    const double dt = tickSeconds();
    const std::vector<float> ins = readDisplay(QStringLiteral("in_peak"));
    const std::vector<float> outs = readDisplay(QStringLiteral("out_peak"));
    const double lastIn = latestIn_, lastOut = latestOut_;
    // The values come a block of audio at a time: with a block longer than a tick, a tick without any
    // is a gap between blocks, not silence. The levels hold until none has come for kHoldSeconds.
    quietFor_ = ins.empty() && outs.empty() ? quietFor_ + dt : 0.0;
    const bool gone = quietFor_ > kHoldSeconds;
    if (!ins.empty() || gone)
        latestIn_ = loudest(ins);
    if (!outs.empty() || gone)
        latestOut_ = loudest(outs);

    // The dots, and the bars under and beside the curve with them: up at once to the newest peak, falling
    // back smoothly. The meters hold their peaks as ticks.
    const double fall = std::exp(-dt / kDotFallSeconds);
    dot_ = std::max(latestIn_, dot_ * fall);
    outLevel_ = std::max(latestOut_, outLevel_ * fall);
    in_.update(latestIn_ > 0.0 ? toDb(latestIn_) : -120.0, dt, 30.0, 1.0, kMeterFloorDb);
    out_.update(latestOut_ > 0.0 ? toDb(latestOut_) : -120.0, dt, 30.0, 1.0, kMeterFloorDb);

    // The afterglow: the highest dot of the last moments, held, then falling back to the dot.
    if (dot_ >= glow_) {
        glow_ = dot_;
        glowHeld_ = 0.0;
    } else {
        glowHeld_ += dt;
        if (glowHeld_ > kGlowHoldSeconds)
            glow_ = std::max(dot_, glow_ * std::exp(-dt / kGlowFallSeconds));
    }

    // How hard it saturates: how far the curve has bent from its small-signal line at the dot.
    const double at = std::min(dot_, 1.0);
    const double line = std::abs(slope_) * at;
    satTarget_ = at > 1e-4 && line > 1e-9 ? std::clamp(1.0 - std::abs(curveAt(at)) / line, 0.0, 1.0) : 0.0;
    sat_.target = satTarget_;
    bool moving = sat_.step(easeFraction(dt, 0.06));
    dotAlpha_.target = dot_ > 0.001 ? 1.0 : 0.0;
    moving = dotAlpha_.step(easeFraction(dt, 0.12)) || moving;
    over_ = latestIn_ > 1.0 ? 1.0 : over_ * std::exp(-dt / kOverSeconds);

    // A new shape eases in.
    if (morph_.step(easeFraction(dt, kMorphSeconds))) {
        moving = true;
        if (morph_.value >= 1.0 || from_.size() != target_.size()) {
            drawn_ = target_;  // (exactly: from + (target - from) * 1 can be an ulp off)
        } else {
            drawn_.resize(target_.size());
            for (std::size_t k = 0; k < target_.size(); ++k)
                drawn_[k] = from_[k] + (target_[k] - from_[k]) * morph_.value;
        }
    }

    // Quiet: what is left of the dots and the flash goes, so it settles.
    if (dot_ <= 1e-5)
        dot_ = 0.0;
    if (outLevel_ <= 1e-5)
        outLevel_ = 0.0;
    if (glow_ <= 1e-5)
        glow_ = 0.0;
    if (over_ <= 0.01)
        over_ = 0.0;
    const bool peaksDown = in_.peak <= kMeterFloorDb && out_.peak <= kMeterFloorDb;
    const bool wasSettled = settled_;
    settled_ = !moving && dot_ == 0.0 && outLevel_ == 0.0 && glow_ == 0.0 && over_ == 0.0 && peaksDown;
    if (!settled_ || !wasSettled)
        update();  // (once more as it settles, to draw it at rest)
    if (latestIn_ != lastIn || latestOut_ != lastOut)
        Q_EMIT levelsChanged();
}

// --- Dragging -------------------------------------------------------------------------------

void SaturatorCurve::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    if (secondPressOfDoubleClick(event))
        return;  // (the double-click follows)
    gesture_ = newGestureKey();
    lastAt_ = event->position();
    movedAcross_ = movedUp_ = 0.0;
    dragDrive_ = shape_.driveDb;
    dragThreshold_ = shape_.thresholdDb;
    dragCurve_ = shape_.wsCurve;
    driveRange_ = sub::app::saturatorRange(kDrive);
    thresholdRange_ = sub::app::saturatorRange(kThreshold);
    curveRange_ = sub::app::saturatorRange(kCurve);
    across_.clear();
    if (bass())
        across_ = kThreshold;
    else if (waveshaper())
        across_ = kCurve;
    if (across_.isEmpty())
        touch(kDrive);  // (else the first moves say which: setParams shows the first one's automation)
}

void SaturatorCurve::mouseMoveEvent(QMouseEvent* event) {
    if (gesture_.isEmpty())
        return;
    const QPointF pos = event->position();
    const double fine = event->modifiers() & Qt::ShiftModifier ? kFine : 1.0;
    const double dx = pos.x() - lastAt_.x(), dy = lastAt_.y() - pos.y();
    lastAt_ = pos;
    movedAcross_ += std::abs(dx);
    movedUp_ += std::abs(dy);
    dragDrive_ = driveRange_.clamp(dragDrive_ + dy * kDrivePerPixel * fine);
    dragThreshold_ = thresholdRange_.clamp(dragThreshold_ + dx * kThresholdPerPixel * fine);
    dragCurve_ = curveRange_.clamp(dragCurve_ + dx * kCurvePerPixel * fine);
    auto rounded = [](double v) { return std::round(v * 100.0) / 100.0; };
    // The same parameters every move (one undo step), the one moved most so far first: its automation shows.
    const double across = across_ == kThreshold ? rounded(dragThreshold_) : rounded(dragCurve_);
    const bool acrossFirst = !across_.isEmpty() && movedAcross_ > movedUp_;
    sub::app::OrderedMap<QString, double> values;
    if (acrossFirst)
        values.insert(across_, across);
    values.insert(kDrive, rounded(dragDrive_));
    if (!across_.isEmpty() && !acrossFirst)
        values.insert(across_, across);
    setParams(values, gesture_, QStringLiteral("Change Saturator"));
}

void SaturatorCurve::mouseReleaseEvent(QMouseEvent*) { gesture_.clear(); }

void SaturatorCurve::mouseUngrabEvent() { gesture_.clear(); }

void SaturatorCurve::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    gesture_.clear();
    setParams({{kDrive, 0.0}}, QString(), QStringLiteral("Change Saturator"));
}

// --- Painting -------------------------------------------------------------------------------

double SaturatorCurve::drawnAt(double input) const {
    const std::size_t n = std::min(inputs_.size(), drawn_.size());
    if (n < 2)
        return input;
    const double at = (std::clamp(input, -1.0, 1.0) + 1.0) / 2.0 * double(n - 1);
    const std::size_t i = std::min(std::size_t(at), n - 2);
    return drawn_[i] + (drawn_[i + 1] - drawn_[i]) * (at - double(i));
}

std::vector<QPointF> SaturatorCurve::pointsWithin(double from, double to) const {
    std::vector<QPointF> points;
    const std::size_t n = std::min(inputs_.size(), drawn_.size());
    if (n < 2 || !(to > from))
        return points;
    points.emplace_back(xOf(from), yOf(drawnAt(from)));
    for (std::size_t i = 0; i < n; ++i) {
        if (inputs_[i] > from && inputs_[i] < to)
            points.emplace_back(xOf(inputs_[i]), yOf(drawn_[i]));
    }
    points.emplace_back(xOf(to), yOf(drawnAt(to)));
    return points;
}

void SaturatorCurve::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF r = plot();
    const double cx = r.center().x(), cy = r.center().y();
    p.fillRect(QRectF(0, 0, width(), height()), Theme::kMeterBg);

    // The grid: the axes, the half-scale lines, full scale up and down, the border; the straight line dashed.
    p.drawLine(QPointF(xOf(-0.5), r.top()), QPointF(xOf(-0.5), r.bottom()), Theme::kGridSub);
    p.drawLine(QPointF(xOf(0.5), r.top()), QPointF(xOf(0.5), r.bottom()), Theme::kGridSub);
    for (const double level : {-1.0, -0.5, 0.5, 1.0})
        p.drawLine(QPointF(r.left(), yOf(level)), QPointF(r.right(), yOf(level)), Theme::kGridSub);
    p.drawLine(QPointF(cx, r.top()), QPointF(cx, r.bottom()), withAlpha(Theme::kGridBar, 170));
    p.drawLine(QPointF(r.left(), cy), QPointF(r.right(), cy), withAlpha(Theme::kGridBar, 170));
    p.drawRect(r, Theme::kGridBeat);
    drawDashedPolyline(p, {QPointF(xOf(-1.0), yOf(-1.0)), QPointF(xOf(1.0), yOf(1.0))}, Theme::kScopeAxis, 1.0);

    p.save();
    p.setClipRect(r);
    // The Bass Shaper's threshold (where it leaves the straight line), Post Clip's ceiling.
    // (Not within a few pixels of the middle, where Drive far above the threshold puts them: two lines
    // on the axis would say nothing.)
    if (bass() && thresholdInput_ < 1.0 && xOf(thresholdInput_) - cx >= kThresholdMarkerGap) {
        for (const double x : {xOf(-thresholdInput_), xOf(thresholdInput_)})
            drawDashedPolyline(p, {QPointF(x, r.top()), QPointF(x, r.bottom())}, withAlpha(Theme::kAccent, 110), 1.0);
    }
    if (shape_.clip != 0) {
        for (const double y : {yOf(1.0), yOf(-1.0)})
            drawDashedPolyline(p, {QPointF(r.left(), y), QPointF(r.right(), y)}, withAlpha(Theme::kMeterHigh, 80), 1.0);
    }

    // The curve, filled towards the middle.
    const std::size_t n = std::min(inputs_.size(), drawn_.size());
    std::vector<QPointF> curve(n);
    for (std::size_t i = 0; i < n; ++i)
        curve[i] = QPointF(xOf(inputs_[i]), yOf(drawn_[i]));
    const bool heard = mix_ > 0.0;
    const QColor line = heard ? Theme::kScopeLine : Theme::kTextDisabled;
    if (n >= 2) {
        QLinearGradient fill(QPointF(0, r.top()), QPointF(0, r.bottom()));
        fill.setColorAt(0.0, withAlpha(line, 48));
        fill.setColorAt(0.5, withAlpha(line, 6));
        fill.setColorAt(1.0, withAlpha(line, 48));
        p.fillToBaseline(curve.data(), int(n), cy, fill);
        p.drawPolyline(curve.data(), int(n), withAlpha(line, 105), 1.5);  // (dimmer than the lit stretch)
    }

    // The signal on it: the stretch it reaches lit, the afterglow beyond, the dots.
    const QColor hot = mixColor(Theme::kScopeLine, Theme::kMeterHigh, sat_.value);
    const double dot = std::min(dot_, 1.0);
    const double alpha = dotAlpha_.value;
    if (alpha > 0.0 && n >= 2) {
        p.save();
        p.setOpacity(alpha);
        const double glow = std::min(glow_, 1.0);
        if (glow > dot + 0.002) {
            // Fading out along the way, in a few pieces (a stroke has one colour).
            constexpr int kPieces = 4;
            for (int k = 0; k < kPieces; ++k) {
                const double a = dot + (glow - dot) * k / kPieces, b = dot + (glow - dot) * (k + 1) / kPieces;
                const QColor faint = withAlpha(hot, 160 - 30 * k);
                drawGlowPolyline(p, pointsWithin(-b, -a), faint, 1.5);
                drawGlowPolyline(p, pointsWithin(a, b), faint, 1.5);
            }
        }
        if (dot > 0.0)
            drawGlowPolyline(p, pointsWithin(-dot, dot), hot, 2.0);
        const double sat = sat_.value;
        for (const double input : {-dot, dot}) {
            const QPointF at(xOf(input), yOf(drawnAt(input)));
            const double radius = 5.0 + 5.0 * sat;
            p.fillEllipse(at, radius, radius, withAlpha(hot, int(55 + 80 * sat)));
            p.drawEllipse(QRectF(at.x() - 6, at.y() - 6, 12, 12), withAlpha(hot, 120), 1.0);
            p.fillEllipse(at, 3.2, 3.2, Theme::kPlayhead);
        }
        p.restore();
    }

    // Over full scale: the sides flash.
    if (over_ > 0.01) {
        const QColor flash = withAlpha(Theme::kMeterHigh, int(140 * over_));
        p.fillRect(QRectF(r.left(), r.top(), 3, r.height()), flash);
        p.fillRect(QRectF(r.right() - 3, r.top(), 3, r.height()), flash);
    }
    p.restore();

    // The In strip under the curve (its x axis), the Out strip at its right (its y axis): bars from the
    // middle out to the level either way (the In bar's reach is the dots'), green, yellow from -12 dB, red
    // from -3 dB, the held peak a tick.
    const QRectF inStrip(r.left(), r.bottom() + kGap, r.width(), kStrip);
    const QRectF outStrip(r.right() + kGap, r.top(), kStrip, r.height());
    const double yellow = std::pow(10.0, -12.0 / 20.0), red = std::pow(10.0, -3.0 / 20.0);
    auto strip = [&](const QRectF& well, bool across, double level, double peakDb) {
        p.fillRect(well, Theme::kPanel);
        const double unit = across ? well.width() / 2 : well.height() / 2 / kOutputRange;  // pixels per 1.0
        const double half = across ? well.width() / 2 : well.height() / 2;
        const double reach = std::min(level * unit, half);
        const QPointF mid = well.center();
        auto band = [&](double from, double to, const QColor& color) {
            const double a = std::min(from * unit, reach), b = std::min(to * unit, reach);
            if (b - a < 0.05)
                return;
            if (across) {
                p.fillRect(QRectF(mid.x() + a, well.top(), b - a, well.height()), color);
                p.fillRect(QRectF(mid.x() - b, well.top(), b - a, well.height()), color);
            } else {
                p.fillRect(QRectF(well.left(), mid.y() - b, well.width(), b - a), color);
                p.fillRect(QRectF(well.left(), mid.y() + a, well.width(), b - a), color);
            }
        };
        band(0.0, yellow, Theme::kMeterLow);
        band(yellow, red, Theme::kMeterMid);
        band(red, kOutputRange * 2, Theme::kMeterHigh);
        if (peakDb > kMeterFloorDb) {
            const double at = std::min(std::pow(10.0, peakDb / 20.0) * unit, half - 1);
            if (at >= 1.0) {
                if (across) {
                    p.fillRect(QRectF(mid.x() + at, well.top(), 1, well.height()), Theme::kText);
                    p.fillRect(QRectF(mid.x() - at - 1, well.top(), 1, well.height()), Theme::kText);
                } else {
                    p.fillRect(QRectF(well.left(), mid.y() - at - 1, well.width(), 1), Theme::kText);
                    p.fillRect(QRectF(well.left(), mid.y() + at, well.width(), 1), Theme::kText);
                }
            }
        }
    };
    strip(inStrip, true, std::min(dot_, 1.0), in_.peak);
    strip(outStrip, false, outLevel_, out_.peak);

    // The drive and HQ in the bottom right corner, which an odd curve takes only when it folds (Sinoid
    // Fold, the Waveshaper's ripples); the type is the list beside it. Each on a dark backing, so a grid
    // line, Post Clip's ceiling or a fold passing behind doesn't run through it.
    const QFont font = uiFont(7);
    const double drive = shape_.driveDb;
    const QString driveText =
        (drive > 0.0 ? QStringLiteral("+") : QString()) + sub::app::formatValue(drive, QStringLiteral("dB"));
    auto corner = [&](double bottom, const QString& text, const QColor& color) {
        const double w = SgPainter::textWidth(text, font);
        const QRectF at(r.right() - 3 - w, bottom - 12, w, 12);
        p.fillRoundedRect(at.adjusted(-2, 0, 2, 0), 2, 2, withAlpha(Theme::kMeterBg, 200));
        p.drawText(at, Qt::AlignRight | Qt::AlignVCenter, text, color, font);
    };
    corner(r.bottom() - 1, driveText, std::abs(drive) > 1e-9 ? Theme::kText : Theme::kTextDim);
    if (hq_)
        corner(r.bottom() - 13, QStringLiteral("HQ"), Theme::kAccent);
}

}  // namespace sub::ui
