#include "devices/ChorusGraph.h"

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
constexpr double kShapeSeconds = 0.04;    // the axis', the Amount's, Shape's and Offset's shown, the layouts' fades
constexpr double kDimSeconds = 0.1;       // dimming as it freezes
constexpr double kPullSeconds = 0.05;     // the phase estimate's pull towards the engine's
constexpr double kFrozenDim = 0.6;
constexpr int kVibrato = 2;  // the mode's index

double frac(double x) { return x - std::floor(x); }

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
    layers_[0].alpha.snap(1.0);
    dim_.snap(kFrozenDim);
    meter_.reset();
}

double ChorusGraph::phase() const { return frac(drawn_); }

int ChorusGraph::voiceCount() const { return 2 * sub::app::chorusVoices(layout_); }

int ChorusGraph::sideVoices(int mode) const {
    return sub::app::chorusVoices(ChorusLayout{mode, layout_.taps, layout_.time});
}

double ChorusGraph::voiceDelayMs(int channel, int voice) const { return delayMs(layout_, channel, voice); }

double ChorusGraph::delayMs(const ChorusLayout& layout, int channel, int voice) const {
    return sub::app::chorusDelayMs(layout, amountShown_.value, shapeShown_.value,
                                   phase() + sub::app::chorusVoicePhase(layout, channel, voice, offsetShown_.value));
}

double ChorusGraph::layoutAlpha(const ChorusLayout& layout) const {
    double alpha = 0.0;
    for (const Layer& layer : layers_) {
        if (layer.layout == layout)
            alpha += layer.alpha.value;
    }
    return std::min(alpha, 1.0);
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
        layers_[0].layout = layout;
    } else if (!(layout == layout_)) {
        // The old voices fade out as the new fade in, each from the alpha it has: a layout still fading out
        // comes back from where it is; otherwise the least visible of those fading out makes room.
        int slot = -1;
        for (int i = 1; i < kLayers; ++i) {
            if (layers_[i].alpha.value > 0.0 && layers_[i].layout == layout)
                slot = i;
        }
        if (slot < 0) {
            slot = 1;
            for (int i = 2; i < kLayers; ++i) {
                if (layers_[i].alpha.value < layers_[slot].alpha.value)
                    slot = i;
            }
            layers_[slot].layout = layout;
            layers_[slot].alpha.snap(0.0);
        }
        std::swap(layers_[0], layers_[slot]);
        for (int i = 0; i < kLayers; ++i)
            layers_[i].alpha.target = i == 0 ? 1.0 : 0.0;
        layout_ = layout;
    } else {
        layout_ = layout;  // (the same voices: Taps or Time changed outside Chorus mode)
        layers_[0].layout = layout;
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
        layers_[0].alpha.snap(1.0);
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
    const double dt = tickSeconds();

    // The phase: on at the rate, pulled towards the engine's newest (the first values, or those after a while
    // without any, taken as they are).
    if (!frozen_)
        estimate_ += rate_ * dt;
    const std::vector<float> phases = readDisplay(QStringLiteral("phase"));
    const std::vector<float> levels = readRecent(QStringLiteral("level"), dt);
    const bool afresh = !haveValues_ || sinceValues_ > kSnapSeconds;
    if (!phases.empty()) {
        const double newest = std::isfinite(phases.back()) ? frac(double(phases.back())) : 0.0;
        if (!afresh) {
            const double error = frac(newest - frac(estimate_) + 0.5) - 0.5;  // -0.5..0.5
            estimate_ = std::abs(error) > 0.25 ? newest : estimate_ + error * easeFraction(dt, kPullSeconds);
        } else {
            estimate_ = newest;
        }
        haveValues_ = true;
        sinceValues_ = 0.0;
    } else {
        sinceValues_ += dt;
    }
    estimate_ = frac(estimate_);
    frozen_ = !enabled_ || !haveValues_ || sinceValues_ > kSnapSeconds;

    // The wet's level: its loudest over the last tick's sound, through the meter's ballistics (held without
    // values). Only the values covering the tick count (readRecent): a read can bring a backlog (an editor
    // opening, or shown again, reads the display's history, seconds of it), which is no longer sounding.
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
    // Resting, the traces hold where they are (a change then reshapes them in place); sound back, they take
    // the phase again. The first values, or values after a while without any, are taken at once either way.
    const bool snapped = resting_ && afresh && !phases.empty() && drawn_ != estimate_;
    if (!resting_ || snapped)
        drawn_ = estimate_;
    glow_.target = frozen_ ? 0.0 : std::clamp((meter_.level - kGlowFloorDb) / kGlowRangeDb, 0.0, 1.0);
    dim_.target = frozen_ ? kFrozenDim : 1.0;

    bool moving = false;
    moving |= glow_.step(easeFraction(dt, kGlowSeconds));
    moving |= window_.step(easeFraction(dt, kWindowSeconds));
    for (Eased* eased : {&axisLow_, &axisHigh_, &amountShown_, &shapeShown_, &offsetShown_})
        moving |= eased->step(easeFraction(dt, kShapeSeconds));
    for (Layer& layer : layers_)
        moving |= layer.alpha.step(easeFraction(dt, kShapeSeconds), 1e-3);
    moving |= dim_.step(easeFraction(dt, kDimSeconds));
    if (moving || !resting_ || snapped) {
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
    axis_ = DragAxis::None;
    lastAt_ = event->position();
    lastRate_ = std::clamp(rate_, sub::app::chorusMinRate(), sub::app::chorusMaxRate());
    lastAmount_ = amount_;
    startDrag(event->modifiers().testFlag(Qt::ShiftModifier));
    update();
}

void ChorusGraph::mouseMoveEvent(QMouseEvent* event) {
    if (gesture_.isEmpty())
        return;
    const QPointF at = event->position();
    const bool fine = event->modifiers().testFlag(Qt::ShiftModifier);
    if (fine != fine_)  // Shift pressed or let go: on from the last move, without a jump
        startDrag(fine);
    if (axis_ == DragAxis::None) {
        // The first move past a few pixels picks what the drag sets, for the rest of it: up and down the
        // Rate, across the Amount (only that one: the other's automation plays on, and a hand's drift
        // sideways doesn't move the Amount).
        const QPointF moved = at - pressedAt_;
        if (std::abs(moved.x()) < kLockPixels && std::abs(moved.y()) < kLockPixels)
            return;
        axis_ = std::abs(moved.y()) >= std::abs(moved.x()) ? DragAxis::Rate : DragAxis::Amount;
        touch(axis_ == DragAxis::Rate ? QStringLiteral("rate") : QStringLiteral("amount"));
    }
    const double scale = fine ? 4.0 : 1.0;
    lastAt_ = at;
    if (axis_ == DragAxis::Rate) {
        const double rate = pressedRate_ * std::pow(2.0, (pressedAt_.y() - at.y()) / kRatePixels / scale);
        lastRate_ = std::clamp(rate, sub::app::chorusMinRate(), sub::app::chorusMaxRate());
        setParams({{QStringLiteral("rate"), std::round(lastRate_ * 1000.0) / 1000.0}}, gesture_);
    } else {
        const double amount = pressedAmount_ + (at.x() - pressedAt_.x()) * 100.0 / kAmountPixels / scale;
        lastAmount_ = std::clamp(amount, 0.0, 100.0);
        setParams({{QStringLiteral("amount"), std::round(lastAmount_ * 10.0) / 10.0}}, gesture_);
    }
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
    QColor colour = mixColor(channel == 0 ? Theme::kScopeLine : kRightColour, Theme::kText, 0.22 * voice);
    colour = mixColor(colour, Theme::kMeterHigh, 0.45 * std::clamp(warmth_ / 100.0, 0.0, 1.0));
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
    const double feedback = layout.mode == kVibrato ? 0.0 : std::clamp(feedback_ / 100.0, 0.0, 1.0);  // (none there)
    const double stroke = 1.5 + feedback;
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
                                                          drawn_ - cyclesAgo + offset);
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

void ChorusGraph::drawDots(SgPainter& p, const ChorusLayout& layout, double alpha) {
    if (alpha <= 0.0)
        return;
    const double glow = mix_ > 0.0 ? glow_.value : 0.0;
    const int voices = sub::app::chorusVoices(layout);
    for (int channel = 1; channel >= 0; --channel) {
        for (int voice = voices - 1; voice >= 0; --voice) {
            const QPointF at(nowX(), yOf(delayMs(layout, channel, voice)));
            const QColor colour = scaledAlpha(voiceColour(layout, channel, voice), alpha);
            const double shown = colour.alphaF();
            if (glow > 0.0) {  // the halo, as sound passes
                const QColor opaque = withAlpha(colour, 255);
                const double halo = 5.0 + 3.0 * glow;
                p.fillEllipse(at, halo, halo, scaledAlpha(opaque, 70.0 / 255 * glow * shown));
                p.fillEllipse(at, 5.0, 5.0, scaledAlpha(opaque, 120.0 / 255 * glow * shown));
            }
            p.fillEllipse(at, 3.0, 3.0, colour);
            const double rim = std::clamp((glow - 0.3) / 0.5, 0.0, 1.0) * shown;
            if (rim > 0.0)
                p.drawEllipse(QRectF(at.x() - 3.0, at.y() - 3.0, 6.0, 6.0), scaledAlpha(Theme::kText, rim), 1.0);
        }
    }
}

void ChorusGraph::drawAxis(SgPainter& p) {
    const QRectF r = plot();
    const double yLow = yOf(axisLow_.value), yHigh = yOf(axisHigh_.value);
    const QColor line = withAlpha(Theme::kGridBeat, 160);
    p.drawLine(QPointF(r.left(), yHigh), QPointF(r.right(), yHigh), line);
    p.drawLine(QPointF(r.left(), yLow), QPointF(r.right(), yLow), line);
    // Each layout's centre, faded with it (one where they meet; a fading layout's may lie outside the plot).
    p.save();
    p.setClipRect(r);
    for (int i = 0; i < kLayers; ++i) {
        double alpha = layers_[i].alpha.value;
        if (alpha <= 0.0)
            continue;
        const double y = yOf(sub::app::chorusCentreMs(layers_[i].layout, amountShown_.value));
        bool drawn = false;
        for (int j = 0; j < kLayers; ++j) {
            if (j == i || layers_[j].alpha.value <= 0.0)
                continue;
            if (std::abs(yOf(sub::app::chorusCentreMs(layers_[j].layout, amountShown_.value)) - y) < 0.5) {
                drawn |= j < i;
                alpha += j > i ? layers_[j].alpha.value : 0.0;
            }
        }
        if (drawn)
            continue;
        const std::vector<QPointF> centre{QPointF(r.left(), y), QPointF(nowX(), y)};
        drawDashedPolyline(p, centre, scaledAlpha(Theme::kGridBar, alpha), 1.0);
    }
    p.restore();
    p.drawLine(QPointF(nowX(), r.top() + 3), QPointF(nowX(), r.bottom() - 3), Theme::kScopeAxis);
}

void ChorusGraph::drawFigures(SgPainter& p) {
    // Each layout's figures (its range's ends where the axis' ends are now, and its centre where there is
    // room), faded with it: while the axis eases from one range to the next, the old figures fade out over
    // the first half of the fade and the new in over the second (never two figures over each other). The
    // same figure in the same place stays.
    const QRectF r = plot();
    const QFont font = uiFont(7);
    struct Figure {
        QString text;
        double y = 0.0;
        bool above = false;
        double alpha = 0.0;
    };
    Figure figures[3 * kLayers];
    int count = 0;
    auto add = [&](double ms, double y, bool above, double alpha) {
        const QString text = msText(ms);
        for (int k = 0; k < count; ++k) {
            if (figures[k].text == text && figures[k].above == above && std::abs(figures[k].y - y) < 0.5) {
                figures[k].alpha += alpha;
                return;
            }
        }
        figures[count++] = {text, y, above, alpha};
    };
    const double yLow = yOf(axisLow_.value), yHigh = yOf(axisHigh_.value);
    for (const Layer& layer : layers_) {
        const double alpha = layer.alpha.value;
        if (alpha <= 0.0)
            continue;
        const double centre = sub::app::chorusCentreMs(layer.layout, amountShown_.value), yCentre = yOf(centre);
        add(sub::app::chorusHighestMs(layer.layout), yHigh, false, alpha);
        add(sub::app::chorusLowestMs(layer.layout), yLow, true, alpha);
        if (yCentre - yHigh > 24.0 && yLow - yCentre > 24.0)
            add(centre, yCentre, false, alpha);
    }
    for (int k = 0; k < count; ++k) {
        const Figure& f = figures[k];
        const double alpha = std::clamp(2.0 * f.alpha - 1.0, 0.0, 1.0);
        if (alpha > 0.0)
            p.drawText(QRectF(r.left() + 3, f.above ? f.y - 12.0 : f.y + 1.0, 60.0, 11.0),
                       Qt::AlignLeft | Qt::AlignVCenter, f.text, scaledAlpha(Theme::kTextDim, alpha), font);
    }
}

void ChorusGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF r = plot();
    p.fillRect(QRectF(0, 0, width(), height()), Theme::kMeterBg);
    drawAxis(p);

    // The layouts fading out under the current one, each with its dots.
    p.save();
    p.setClipRect(r);
    for (int i = kLayers - 1; i >= 0; --i) {
        drawVoices(p, layers_[i].layout, layers_[i].alpha.value);
        drawDots(p, layers_[i].layout, layers_[i].alpha.value);
    }
    p.restore();

    // The oldest end fades into the ground, and the axis' figures stand there.
    QLinearGradient fade(QPointF(r.left(), 0), QPointF(r.left() + kFadeWidth, 0));
    fade.setColorAt(0, withAlpha(Theme::kMeterBg, 235));
    fade.setColorAt(1, withAlpha(Theme::kMeterBg, 0));
    p.fillRect(QRectF(r.left(), r.top(), kFadeWidth, r.height()), fade);
    drawFigures(p);

    // The header: what the axis is, and the largest detune (while dragging: the Rate and the Amount).
    const QFont font = uiFont(7);
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
