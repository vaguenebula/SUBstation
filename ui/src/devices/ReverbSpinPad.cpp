#include "devices/ReverbSpinPad.h"

#include "audio/ReverbResponse.h"
#include "devices/EditorPaint.h"
#include "input/GestureKey.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kAmountSeconds = 0.12;  // Spin switched: its amount eases in or out
constexpr double kPresenceSeconds = 0.3; // the swing settles once the reflections fall silent
constexpr double kLingerSeconds = 2.0;   // and stays this long after Spin's settings change (to show them)
constexpr double kSounding = 0.01;       // the reflections' light above this: they sound
constexpr double kFlashSeconds = 0.35;   // the reflections' light fades
constexpr double kFlashRangeDb = 54.0;   // their level from -54 dB (dark) to 0 (bright)
constexpr double kRadius = 1.5;          // px: a particle's radius, as quiet as can be
constexpr double kRadiusLoud = 3.0;      // px more, as loud as the loudest
constexpr double kGlow = 3.0;            // px of glow round the loudest, lit up
constexpr double kBob = 3.0;             // px a particle bobs up and down at Spin 100 %
constexpr double kMoved = 0.2;           // px: less is no reason to paint again
constexpr double kCaption = 13.0;        // px: the captions' strip at the top, clear of the handle and particles
// The first reflection's row: 1 px under the captions with the loudest particle lit and bobbing at its highest
// (the first is the loudest at every Shape).
constexpr double kRowTop = kCaption + kRadius + kRadiusLoud + kGlow + kBob + 1.0;
constexpr double kRowBottom = 13.0;      // the last's, above the L and R
// A long audio block: a tick reads the reflections' newest levels back this far (not a backlog's), and one
// without values keeps the last this long.
constexpr double kStaleSeconds = 0.1;

}  // namespace

ReverbSpinPad::ReverbSpinPad(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(kMinimumWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton);
    setCursor(Qt::SizeAllCursor);
}

QRectF ReverbSpinPad::plot() const { return QRectF(0, 0, width(), height()).adjusted(1, 1, -1, -1); }

QRectF ReverbSpinPad::inner() const { return plot().adjusted(6, kCaption + 6, -6, -6); }

double ReverbSpinPad::xOfRate(double hz) const {
    const QRectF r = inner();
    return LogAxis{kRateMin, kRateMax, r.left(), r.width()}.position(std::clamp(hz, kRateMin, kRateMax));
}

double ReverbSpinPad::rateAt(double x) const {
    const QRectF r = inner();
    return LogAxis{kRateMin, kRateMax, r.left(), r.width()}.valueAt(x);
}

double ReverbSpinPad::yOfAmount(double percent) const {
    const QRectF r = inner();
    return r.bottom() - std::clamp(percent, 0.0, 100.0) / 100.0 * r.height();
}

double ReverbSpinPad::amountAt(double y) const {
    const QRectF r = inner();
    return std::clamp((r.bottom() - y) / r.height(), 0.0, 1.0) * 100.0;
}

QPointF ReverbSpinPad::handle() const { return QPointF(xOfRate(rate_), yOfAmount(amountPercent_)); }

QList<QPointF> ReverbSpinPad::particles() const { return QList<QPointF>(at_.begin(), at_.end()); }

double ReverbSpinPad::particleReach(int k) const {
    if (!particleShown(k))
        return 0.0;
    const std::size_t i = std::size_t(k);
    return radius_[i] + kGlow * loudness_[i] + kBob;
}

QList<QPointF> ReverbSpinPad::trail(int k) const {
    if (k < 0 || k >= kTaps)
        return {};
    const std::array<QPointF, kTrail>& trail = trail_[std::size_t(k)];
    return QList<QPointF>(trail.begin(), trail.end());
}

void ReverbSpinPad::sync() {
    const bool wasSpinning = spin_;
    spin_ = value(QStringLiteral("spin")) >= 0.5;
    const double rateBefore = rate_;
    rate_ = value(QStringLiteral("spin_rate"));
    amountPercent_ = value(QStringLiteral("spin_amount"));
    const double widthBefore = width_;
    const std::array<double, kTaps> rowsBefore = row_, radiiBefore = radius_;
    width_ = sub::app::reverbStereoWidth(value(QStringLiteral("stereo")));
    const double size = value(QStringLiteral("size")), shape = value(QStringLiteral("shape"));
    const int density = int(std::lround(value(QStringLiteral("density"))));
    const QList<sub::app::ReverbTap> taps = sub::app::reverbEarlyTaps(size, shape, density);
    double loudest = 0.0, last = 0.0;
    for (const sub::app::ReverbTap& tap : taps) {
        loudest = std::max(loudest, std::abs(tap.gain));
        last = std::max(last, tap.ms);
    }
    for (int k = 0; k < kTaps && k < taps.size(); ++k) {
        const sub::app::ReverbTap& tap = taps[k];
        const std::size_t i = std::size_t(k);
        row_[i] = last > 0.0 ? tap.ms / last : 0.0;
        loudness_[i] = loudest > 0.0 ? std::abs(tap.gain) / loudest : 0.0;
        radius_[i] = tap.gain != 0.0 ? kRadius + kRadiusLoud * loudness_[i] : 0.0;
    }
    const double onset = sub::app::reverbDiffuseOnsetMs(size, shape, density) + value(QStringLiteral("predelay"));
    onsetText_ = QStringLiteral("tail +%1 ms").arg(qRound(onset));
    const double amount = spin_ ? std::clamp(amountPercent_ / 100.0, 0.0, 1.0) : 0.0;
    if (synced_ && (amount != amount_.target || rate_ != rateBefore))
        linger_ = kLingerSeconds;  // Spin's settings changed: show how they swing, even in silence
    if (synced_ && (spin_ != wasSpinning || amount_.value != amount_.target))
        amount_.target = amount;  // switched (or still easing from a switch): it eases, in advance()
    else
        amount_.snap(amount);  // dragged: it follows at once
    // The trails start again only where the particles' homes moved (Size, Shape, Density, Stereo): any
    // parameter's change (an automated one's, every playhead move) syncs, and must not cut them short.
    const bool moved = !synced_ || width_ != widthBefore || row_ != rowsBefore || radius_ != radiiBefore;
    synced_ = true;
    place();
    painted_ = at_;
    if (moved)
        for (std::size_t k = 0; k < at_.size(); ++k) trail_[k].fill(at_[k]);
    update();
}

void ReverbSpinPad::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        place();
        painted_ = at_;
        for (std::size_t k = 0; k < at_.size(); ++k) trail_[k].fill(at_[k]);
    }
}

QPointF ReverbSpinPad::positionOf(int k, double swing) const {
    const QRectF r = plot();
    const std::size_t i = std::size_t(k);
    const double pan = std::clamp(sub::app::reverbSpinPan(k, swing, drawPhase_) * width_, -1.0, 1.0);
    const double x = r.center().x() + (r.width() / 2 - 6.0) * pan;
    const double top = r.top() + kRowTop, bottom = r.bottom() - kRowBottom;
    const double bob = swing * kBob * std::sin(2.0 * kPi * (drawPhase_ + double(k) / kTaps));
    return QPointF(x, top + row_[i] * std::max(0.0, bottom - top) + bob);
}

void ReverbSpinPad::place() {
    for (int k = 0; k < kTaps; ++k) at_[std::size_t(k)] = positionOf(k, amountShown());
}

bool ReverbSpinPad::swingSettled() const {
    const double now = amountShown(), end = amount_.target * presence_.target;
    for (int k = 0; k < kTaps; ++k) {
        if (!particleShown(k))
            continue;
        const QPointF left = positionOf(k, end) - positionOf(k, now);
        if (std::hypot(left.x(), left.y()) >= kMoved)
            return false;
    }
    return true;
}

// --- Displays and animation -------------------------------------------------------------

void ReverbSpinPad::refreshDisplays() {
    const double seconds = tickSeconds();
    // The newest phase, and the loudest of the newest levels (shown again after the sound stopped, a read holds
    // the loud past: DeviceCanvas::readRecent). A tick that brings no values (the audio's blocks longer than a
    // tick) keeps the last for a moment: the phase runs on at Spin's rate (set right by the next one published),
    // the light holds.
    const std::vector<float> phases = readDisplay(QStringLiteral("spin"));
    const std::vector<float> early = readRecent(QStringLiteral("early"), kStaleSeconds);
    const bool stale = phases.empty() && early.empty() && (stale_ += seconds) > kStaleSeconds;
    if (!phases.empty()) {
        phase_ = phases.back();
        if (phase_ >= 0.0)
            drawPhase_ = phase_;
    } else if (phase_ >= 0.0 && !stale) {
        drawPhase_ += rate_ * seconds;
        drawPhase_ -= std::floor(drawPhase_);
    }
    if (!early.empty()) {
        double loudest = sub::app::kReverbMeterFloorDb;
        for (const float v : early) loudest = std::max(loudest, double(v));
        flash_.target = std::clamp((loudest + kFlashRangeDb) / kFlashRangeDb, 0.0, 1.0);
        stale_ = 0.0;
    } else if (stale) {
        flash_.target = 0.0;
    }
    advance(seconds);
}

void ReverbSpinPad::advance(double seconds) {
    bool moving = false;
    if (flash_.target > flash_.value) {  // it lights at once, and fades
        flash_.value = flash_.target;
        moving = true;
    } else {
        moving = flash_.step(easeFraction(seconds, kFlashSeconds), 1e-3);
    }
    amount_.step(easeFraction(seconds, kAmountSeconds), 1e-4);
    // The reflections swing while they sound (and for a moment after Spin is changed), and settle at rest
    // in silence: nothing moves while nothing plays.
    linger_ = std::max(0.0, linger_ - seconds);
    presence_.target = flash_.value > kSounding || linger_ > 0.0 ? 1.0 : 0.0;
    presence_.step(easeFraction(seconds, kPresenceSeconds), 1e-4);
    // The swing eases until no particle is kMoved from where it ends, then ends there, and moves all the while:
    // an easing's last steps are each less than kMoved, and one left to creep on unpainted would rest with a way
    // still to go, then wake to paint the creep once it added up (again and again, for seconds of silence).
    if (amount_.value != amount_.target || presence_.value != presence_.target) {
        if (swingSettled()) {
            amount_.snap(amount_.target);
            presence_.snap(presence_.target);
        } else {
            moving = true;
        }
    }
    place();
    for (std::size_t k = 0; k < at_.size(); ++k) {
        if (radius_[k] <= 0.0)
            continue;
        std::array<QPointF, kTrail>& trail = trail_[k];
        std::rotate(trail.rbegin(), trail.rbegin() + 1, trail.rend());  // the latest first
        trail[0] = at_[k];
        const QPointF moved = at_[k] - painted_[k], tail = trail[kTrail - 1] - at_[k];
        if (std::hypot(moved.x(), moved.y()) >= kMoved || std::hypot(tail.x(), tail.y()) >= kMoved)
            moving = true;
    }
    if (moving)
        painted_ = at_;
    if (moving != animating_ || moving) {
        animating_ = moving;
        Q_EMIT levelsChanged();
    }
    if (moving)
        update();
}

// --- Dragging -------------------------------------------------------------------------

void ReverbSpinPad::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    gesture_ = newGestureKey();
    fine_ = event->modifiers().testFlag(Qt::ShiftModifier);
    anchor_ = anchorHandle_ = event->position();  // the handle comes to the mouse
    touch(QStringLiteral("spin_amount"));
    dragTo(event->position(), event->modifiers());
}

void ReverbSpinPad::mouseMoveEvent(QMouseEvent* event) {
    if (!gesture_.isEmpty())
        dragTo(event->position(), event->modifiers());
}

void ReverbSpinPad::mouseReleaseEvent(QMouseEvent*) { gesture_.clear(); }

void ReverbSpinPad::mouseUngrabEvent() { gesture_.clear(); }

void ReverbSpinPad::dragTo(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    const bool fine = modifiers.testFlag(Qt::ShiftModifier);
    if (fine != fine_) {  // Shift went down or up: measure from here
        fine_ = fine;
        anchor_ = pos;
        anchorHandle_ = handle();
    }
    const QPointF at = anchorHandle_ + (pos - anchor_) * (fine_ ? 0.25 : 1.0);
    setParams({{QStringLiteral("spin_rate"), std::clamp(rateAt(at.x()), kRateMin, kRateMax)},
               {QStringLiteral("spin_amount"), std::nearbyint(amountAt(at.y()) * 10.0) / 10.0}},
              gesture_, QStringLiteral("Change Reverb Spin"));
}

// --- Painting ---------------------------------------------------------------------------

void ReverbSpinPad::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF r = plot();
    p.fillRect(QRectF(0, 0, width(), height()), Theme::kMeterBg);
    const QFont font = uiFont(7);

    // The stereo field's middle, under the captions.
    p.drawLine(QPointF(r.center().x(), r.top() + kCaption), QPointF(r.center().x(), r.bottom() - 3),
               withAlpha(Theme::kGridBeat, 200));

    // The captions: what it is, and when the tail starts after the input.
    p.drawText(QRectF(r.left() + 3, r.top() + 1, 40, 12), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Early"),
               Theme::kTextDim, font);
    p.drawText(QRectF(r.left() + 30, r.top() + 1, r.width() - 33, 12), Qt::AlignRight | Qt::AlignVCenter,
               onsetText_, Theme::kTextDim, font);

    // The handle's crosshairs, under the particles.
    const QPointF h = handle();
    p.drawLine(QPointF(r.left(), h.y()), QPointF(r.right(), h.y()), withAlpha(Theme::kText, 30));
    p.drawLine(QPointF(h.x(), r.top()), QPointF(h.x(), r.bottom()), withAlpha(Theme::kText, 30));

    // The reflections: their trails, then them, lit as they sound.
    const double flash = flash_.value;
    for (std::size_t k = 0; k < at_.size(); ++k) {
        if (radius_[k] <= 0.0)
            continue;
        const std::array<QPointF, kTrail>& trail = trail_[k];
        for (int i = kTrail - 1; i >= 1; --i) {
            const QPointF d = trail[std::size_t(i)] - at_[k];
            if (std::hypot(d.x(), d.y()) < 0.5)
                continue;
            const int alpha = 70 - (70 - 10) * (i - 1) / (kTrail - 2);  // fading behind it
            const double radius = radius_[k] * (1.0 - 0.1 * i);
            p.fillEllipse(trail[std::size_t(i)], radius, radius, withAlpha(Theme::kScopeLine, alpha));
        }
    }
    for (std::size_t k = 0; k < at_.size(); ++k) {
        if (radius_[k] <= 0.0)
            continue;
        const double light = flash * loudness_[k];
        if (light > 0.05)  // a soft glow round a bright one
            p.fillEllipse(at_[k], radius_[k] + kGlow * light, radius_[k] + kGlow * light,
                          withAlpha(Theme::kScopeLine, int(50 * light)));
        p.fillEllipse(at_[k], radius_[k], radius_[k], withAlpha(Theme::kScopeLine, int(60 + 170 * light)));
    }

    // L and R at the bottom corners, over the particles: the last reflection, swung to a side at a high Spin,
    // passes under the letter.
    p.drawText(QRectF(r.left() + 3, r.bottom() - 12, 12, 11), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("L"),
               Theme::kTextDisabled, font);
    p.drawText(QRectF(r.right() - 15, r.bottom() - 12, 12, 11), Qt::AlignRight | Qt::AlignVCenter,
               QStringLiteral("R"), Theme::kTextDisabled, font);

    // The handle: a ring with a dot in it.
    const QColor ring = spin_ ? Theme::kAccent : Theme::kTextDim;
    p.drawEllipse(QRectF(h.x() - 5, h.y() - 5, 10, 10), ring, 2);
    p.fillEllipse(h, 1.5, 1.5, ring);
}

}  // namespace sub::ui
