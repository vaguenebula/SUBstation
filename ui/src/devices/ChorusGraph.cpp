#include "devices/ChorusGraph.h"

#include "devices/DisplayClock.h"
#include "devices/EditorPaint.h"
#include "input/GestureKey.h"
#include "model/Device.h"
#include "model/ParamSpec.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QLinearGradient>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

namespace {

using sub::app::ChorusLayout;

constexpr double kGlowSeconds = 0.05;    // the glow's easing
constexpr double kWindowSeconds = 0.075;  // the window's, as the Rate changes
constexpr double kShapeSeconds = 0.04;    // the axis', the Amount's, Shape's and Offset's shown, the layout's fade
constexpr double kDimSeconds = 0.1;       // dimming as it freezes
constexpr double kPullSeconds = 0.05;     // the phase estimate's pull towards the engine's
constexpr double kFrozenDim = 0.6;
constexpr int kVibrato = 2;  // the mode's index

double frac(double x) { return x - std::floor(x); }

// `a` mixed with `b` by `t` (0: a), alpha and all.
QColor mix(const QColor& a, const QColor& b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    auto between = [t](double from, double to) { return float(from + (to - from) * t); };
    return QColor::fromRgbF(between(a.redF(), b.redF()), between(a.greenF(), b.greenF()),
                            between(a.blueF(), b.blueF()), between(a.alphaF(), b.alphaF()));
}

QColor scaledAlpha(const QColor& color, double alpha) {
    return withAlpha(color, int(std::lround(color.alpha() * std::clamp(alpha, 0.0, 1.0))));
}

// "11.5", "50": an axis' figure in ms.
QString msText(double ms) {
    return std::abs(ms - std::round(ms)) < 0.05 ? QString::number(std::lround(ms)) : pythonFixed(ms, 1);
}

}  // namespace

ChorusGraph::ChorusGraph(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(kMinimumWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton);
    setCursor(Qt::SizeAllCursor);
    window_.snap(1.0);
    axisLow_.snap(sub::app::chorusLowestMs(layout_));
    axisHigh_.snap(sub::app::chorusHighestMs(layout_));
    amountShown_.snap(amount_);
    fade_.snap(1.0);
    dim_.snap(kFrozenDim);
    meter_.reset();
}

double ChorusGraph::phase() const { return frac(estimate_); }

int ChorusGraph::voiceCount() const { return 2 * sub::app::chorusVoices(layout_); }

double ChorusGraph::voiceDelayMs(int channel, int voice) const {
    return sub::app::chorusDelayMs(layout_, amountShown_.value, shapeShown_.value,
                                   phase() + sub::app::chorusVoicePhase(layout_, channel, voice, offsetShown_.value));
}

QPointF ChorusGraph::voiceDot(int channel, int voice) const {
    return QPointF(nowX(), yOf(voiceDelayMs(channel, voice)));
}

double ChorusGraph::centreMs() const { return sub::app::chorusCentreMs(layout_, amountShown_.value); }

double ChorusGraph::swingMs() const { return sub::app::chorusSwingMs(layout_, amountShown_.value); }

QRectF ChorusGraph::plot() const { return QRectF(0, 0, width(), height()).adjusted(1, kHeader, -1, -1); }

double ChorusGraph::nowX() const { return plot().right() - kNowInset; }

double ChorusGraph::yOf(double ms) const {
    const QRectF r = plot();
    const double low = axisLow_.value, high = axisHigh_.value;
    const double span = std::max(high - low, 1e-6);
    const double bottom = low - kMargin * span, top = high + kMargin * span;
    return r.bottom() - (ms - bottom) / (top - bottom) * r.height();
}

double ChorusGraph::xOfCycles(double cyclesAgo) const {
    const double now = nowX();
    return now - cyclesAgo / std::max(window_.value, 1e-6) * (now - plot().left());
}

int ChorusGraph::index(const QString& paramId) const { return int(std::lround(value(paramId))); }

// --- The parameters, and the displays ---------------------------------------------------

void ChorusGraph::sync() {
    const sub::app::Device* found = device();
    if (!found) {
        enabled_ = false;
        update();
        return;
    }
    enabled_ = found->enabled;
    const ChorusLayout layout{index(QStringLiteral("mode")), index(QStringLiteral("taps")),
                              index(QStringLiteral("time"))};
    rate_ = value(QStringLiteral("rate"));
    amount_ = value(QStringLiteral("amount"));
    feedback_ = value(QStringLiteral("feedback"));
    width_ = value(QStringLiteral("width"));
    offset_ = value(QStringLiteral("offset"));
    shape_ = value(QStringLiteral("shape"));
    warmth_ = value(QStringLiteral("warmth"));
    mix_ = value(QStringLiteral("mix"));

    if (!synced_) {
        layout_ = layout;
    } else if (!(layout == layout_)) {
        // The old voices fade out as the new fade in; a change during a fade keeps whichever shows more.
        if (fade_.value >= 0.5)
            previous_ = layout_;
        layout_ = layout;
        fade_.value = 0.0;
        fade_.target = 1.0;
    } else {
        layout_ = layout;  // (the same voices: Taps or Time changed outside Chorus mode)
    }

    window_.target = std::clamp(rate_ * 1.2, 1.0, 8.0);
    axisLow_.target = sub::app::chorusLowestMs(layout_);
    axisHigh_.target = sub::app::chorusHighestMs(layout_);
    amountShown_.target = amount_;
    shapeShown_.target = shape_;
    offsetShown_.target = offset_;
    if (!synced_) {  // an editor opens settled
        for (Eased* eased : {&window_, &axisLow_, &axisHigh_, &amountShown_, &shapeShown_, &offsetShown_})
            eased->snap(eased->target);
        fade_.snap(1.0);
        synced_ = true;
    }

    detuneCents_ = sub::app::chorusPeakDetuneCents(layout_, rate_, amount_, shape_);
    if (detuneCents_ < 0.5)
        readout_ = QStringLiteral("0 ct");
    else if (detuneCents_ < 99.5)
        readout_ = QStringLiteral("±%1 ct").arg(std::lround(detuneCents_));
    else
        readout_ = QStringLiteral("±%1 st").arg(pythonFixed(detuneCents_ / 100.0, 1));
    Q_EMIT curveChanged();
    update();
}

void ChorusGraph::refreshDisplays() {
    // The time since the last tick: at least a clock period (so ticks by hand animate as the clock
    // would), at most 0.1 s for the easing; a longer gap (hidden, or the clock held up) counts whole
    // towards the values' absence.
    const double elapsed = clock_.isValid() ? double(clock_.restart()) / 1000.0 : 0.0;
    if (!clock_.isValid())
        clock_.start();
    const double dt = std::clamp(elapsed, kDisplayRefreshMs / 1000.0, 0.1);
    const double gap = std::max(dt, elapsed);

    // The phase: on at the rate, pulled towards the engine's newest (the first, or after a gap, taken as it is).
    if (!frozen_)
        estimate_ += rate_ * dt;
    const std::vector<float> phases = readDisplay(QStringLiteral("phase"));
    const std::vector<float> levels = readDisplay(QStringLiteral("level"));
    if (!phases.empty()) {
        const double newest = std::isfinite(phases.back()) ? frac(double(phases.back())) : 0.0;
        if (haveValues_ && sinceValues_ + gap <= kSnapSeconds) {
            const double error = frac(newest - frac(estimate_) + 0.5) - 0.5;  // -0.5..0.5
            estimate_ = std::abs(error) > 0.25 ? newest : estimate_ + error * easeFraction(dt, kPullSeconds);
        } else {
            estimate_ = newest;
        }
        haveValues_ = true;
        sinceValues_ = 0.0;
    } else {
        sinceValues_ += gap;
    }
    estimate_ = frac(estimate_);
    frozen_ = !enabled_ || !haveValues_ || sinceValues_ > kSnapSeconds;

    // The wet's level: its loudest since, through the meter's ballistics (held without values).
    if (frozen_) {
        meter_.reset();
    } else if (!levels.empty()) {
        double loudest = -120.0;
        for (const float level : levels) {
            if (std::isfinite(level))
                loudest = std::max(loudest, double(level));
        }
        meter_.update(loudest, dt, 24.0, 0.5);
    }
    resting_ = frozen_ || meter_.level <= kSilentDb;
    glow_.target = frozen_ ? 0.0 : std::clamp((meter_.level - kGlowFloorDb) / kGlowRangeDb, 0.0, 1.0);
    dim_.target = frozen_ ? kFrozenDim : 1.0;

    bool moving = false;
    moving |= glow_.step(easeFraction(dt, kGlowSeconds));
    moving |= window_.step(easeFraction(dt, kWindowSeconds));
    for (Eased* eased : {&axisLow_, &axisHigh_, &amountShown_, &shapeShown_, &offsetShown_, &fade_})
        moving |= eased->step(easeFraction(dt, kShapeSeconds));
    moving |= dim_.step(easeFraction(dt, kDimSeconds));
    if (moving || !resting_) {
        update();
        Q_EMIT animated();
    }
}

// --- Dragging -------------------------------------------------------------------------

void ChorusGraph::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    gesture_ = newGestureKey();
    lastAt_ = event->position();
    lastRate_ = std::clamp(rate_, sub::app::kChorusMinRate, sub::app::kChorusMaxRate);
    lastAmount_ = amount_;
    startDrag(event->modifiers().testFlag(Qt::ShiftModifier));
    touch(QStringLiteral("rate"));
    update();
}

void ChorusGraph::mouseMoveEvent(QMouseEvent* event) {
    if (gesture_.isEmpty())
        return;
    const QPointF at = event->position();
    const bool fine = event->modifiers().testFlag(Qt::ShiftModifier);
    if (fine != fine_)  // Shift pressed or let go: on from the last move, without a jump
        startDrag(fine);
    const double scale = fine ? 4.0 : 1.0;
    const double rate = pressedRate_ * std::pow(2.0, (pressedAt_.y() - at.y()) / kRatePixels / scale);
    const double amount = pressedAmount_ + (at.x() - pressedAt_.x()) * 100.0 / kAmountPixels / scale;
    lastAt_ = at;
    lastRate_ = std::clamp(rate, sub::app::kChorusMinRate, sub::app::kChorusMaxRate);
    lastAmount_ = std::clamp(amount, 0.0, 100.0);
    setParams({{QStringLiteral("rate"), std::round(lastRate_ * 1000.0) / 1000.0},
               {QStringLiteral("amount"), std::round(lastAmount_ * 10.0) / 10.0}},
              gesture_);
}

void ChorusGraph::startDrag(bool fine) {
    pressedAt_ = lastAt_;
    pressedRate_ = lastRate_;
    pressedAmount_ = lastAmount_;
    fine_ = fine;
}

void ChorusGraph::mouseReleaseEvent(QMouseEvent*) {
    gesture_.clear();
    update();
}

void ChorusGraph::mouseUngrabEvent() {
    gesture_.clear();
    update();
}

// --- Painting ---------------------------------------------------------------------------

QColor ChorusGraph::voiceColour(const ChorusLayout& layout, int channel, int voice) const {
    if (mix_ <= 0.0)
        return scaledAlpha(Theme::kTextDisabled, dim_.value);  // none of it heard
    QColor colour = mix(channel == 0 ? Theme::kScopeLine : kRightColour, Theme::kText, 0.22 * voice);
    colour = mix(colour, Theme::kMeterHigh, 0.45 * std::clamp(warmth_ / 100.0, 0.0, 1.0));
    double alpha = (0.55 + 0.45 * glow_.value) * dim_.value;
    if (channel == 1 && layout.mode != kVibrato)
        alpha *= 0.45 + 0.55 * std::min(1.0, std::max(0.0, width_) / 100.0);  // narrow: towards mono
    return scaledAlpha(colour, alpha);
}

void ChorusGraph::drawVoices(SgPainter& p, const ChorusLayout& layout, double alpha) {
    if (alpha <= 0.0)
        return;
    const QRectF r = plot();
    const double now = nowX(), span = now - r.left();
    const double window = std::max(window_.value, 1e-6);
    const double stroke = 1.5 + std::clamp(feedback_ / 100.0, 0.0, 1.0);
    const bool heard = mix_ > 0.0;
    const int voices = sub::app::chorusVoices(layout);
    const int columns = std::max(1, int(std::ceil(span)));
    for (int channel = 1; channel >= 0; --channel) {  // the right side behind the left
        for (int voice = voices - 1; voice >= 0; --voice) {
            const double offset = sub::app::chorusVoicePhase(layout, channel, voice, offsetShown_.value);
            points_.clear();
            for (int i = 0; i <= columns; ++i) {
                const double x = std::min(r.left() + i, now);
                const double cyclesAgo = (now - x) / span * window;
                const double ms = sub::app::chorusDelayMs(layout, amountShown_.value, shapeShown_.value,
                                                          estimate_ - cyclesAgo + offset);
                points_.emplace_back(x, yOf(ms));
            }
            const QColor colour = scaledAlpha(voiceColour(layout, channel, voice), alpha);
            if (!heard) {
                p.drawPolyline(points_.data(), int(points_.size()), colour, stroke, Qt::RoundCap);
                continue;
            }
            if (glow_.value > 0.01)  // the bloom as sound passes
                p.drawPolyline(points_.data(), int(points_.size()),
                               scaledAlpha(colour, 0.09 * glow_.value), stroke + 8.0, Qt::RoundCap);
            drawGlowPolyline(p, points_, colour, stroke);
        }
    }
}

void ChorusGraph::drawDots(SgPainter& p) {
    const double fade = fade_.value;
    const double glow = mix_ > 0.0 ? glow_.value : 0.0;
    const int voices = sub::app::chorusVoices(layout_);
    for (int channel = 1; channel >= 0; --channel) {
        for (int voice = voices - 1; voice >= 0; --voice) {
            const QPointF at = voiceDot(channel, voice);
            const QColor colour = scaledAlpha(voiceColour(layout_, channel, voice), fade);
            const double alpha = colour.alphaF();
            if (glow > 0.0) {  // the halo, as sound passes
                const QColor opaque = withAlpha(colour, 255);
                const double halo = 5.0 + 3.0 * glow;
                p.fillEllipse(at, halo, halo, scaledAlpha(opaque, 70.0 / 255 * glow * alpha));
                p.fillEllipse(at, 5.0, 5.0, scaledAlpha(opaque, 120.0 / 255 * glow * alpha));
            }
            p.fillEllipse(at, 3.0, 3.0, colour);
            const double rim = std::clamp((glow - 0.3) / 0.5, 0.0, 1.0) * alpha;
            if (rim > 0.0)
                p.drawEllipse(QRectF(at.x() - 3.0, at.y() - 3.0, 6.0, 6.0), scaledAlpha(Theme::kText, rim), 1.0);
        }
    }
}

void ChorusGraph::drawAxis(SgPainter& p) {
    const QRectF r = plot();
    const double yLow = yOf(axisLow_.value), yHigh = yOf(axisHigh_.value), yCentre = yOf(centreMs());
    const QColor line = withAlpha(Theme::kGridBeat, 160);
    p.drawLine(QPointF(r.left(), yHigh), QPointF(r.right(), yHigh), line);
    p.drawLine(QPointF(r.left(), yLow), QPointF(r.right(), yLow), line);
    const std::vector<QPointF> centre{QPointF(r.left(), yCentre), QPointF(nowX(), yCentre)};
    drawDashedPolyline(p, centre, Theme::kGridBar, 1.0);
    p.drawLine(QPointF(nowX(), r.top() + 3), QPointF(nowX(), r.bottom() - 3), Theme::kScopeAxis);
}

void ChorusGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF r = plot();
    p.fillRect(QRectF(0, 0, width(), height()), Theme::kMeterBg);
    drawAxis(p);

    p.save();
    p.setClipRect(r);
    if (fade_.value < 1.0)
        drawVoices(p, previous_, 1.0 - fade_.value);
    drawVoices(p, layout_, fade_.value);
    drawDots(p);
    p.restore();

    // The oldest end fades into the ground, and the axis' figures stand there: its ends, and the centre
    // where there is room.
    QLinearGradient fade(QPointF(r.left(), 0), QPointF(r.left() + kFadeWidth, 0));
    fade.setColorAt(0, withAlpha(Theme::kMeterBg, 235));
    fade.setColorAt(1, withAlpha(Theme::kMeterBg, 0));
    p.fillRect(QRectF(r.left(), r.top(), kFadeWidth, r.height()), fade);
    const QFont font = uiFont(7);
    auto figure = [&](double ms, double y, bool above) {
        p.drawText(QRectF(r.left() + 3, above ? y - 12.0 : y + 1.0, 60.0, 11.0), Qt::AlignLeft | Qt::AlignVCenter,
                   msText(ms), Theme::kTextDim, font);
    };
    const double yLow = yOf(axisLow_.value), yHigh = yOf(axisHigh_.value), yCentre = yOf(centreMs());
    figure(axisHigh_.value, yHigh, false);
    figure(axisLow_.value, yLow, true);
    if (yCentre - yHigh > 24.0 && yLow - yCentre > 24.0)
        figure(centreMs(), yCentre, false);

    // The header: what the axis is, and the largest detune (while dragging: the Rate and the Amount).
    const QColor text = frozen_ ? Theme::kTextDim : Theme::kText;
    p.drawText(QRectF(r.left() + 3, 1, 60, kHeader - 1), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Delay (ms)"),
               Theme::kTextDim, font);
    if (!gesture_.isEmpty()) {
        const QString dragged = sub::app::formatValue(rate_, QStringLiteral("Hz")) + QStringLiteral("  ·  ") +
                                sub::app::formatValue(amount_, QStringLiteral("%"));
        p.drawText(QRectF(r.left(), 1, r.width(), kHeader - 1), Qt::AlignCenter, dragged, Theme::kText, font);
    }
    p.drawText(QRectF(r.left(), 1, r.width() - 3, kHeader - 1), Qt::AlignRight | Qt::AlignVCenter, readout_, text,
               font);
}

}  // namespace sub::ui
