#include "devices/MultibandGraph.h"

#include "audio/MultibandResponse.h"
#include "input/GestureKey.h"
#include "input/Modifiers.h"
#include "model/Device.h"
#include "model/ParamSpec.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QHoverEvent>
#include <QMouseEvent>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <vector>

namespace sub::ui {

using sub::app::kMultibandMaxRatio;
using sub::app::kMultibandMaxThresholdDb;
using sub::app::kMultibandMinRatio;
using sub::app::kMultibandMinThresholdDb;

namespace {

const QColor kBoost(0x4f, 0xd1, 0xc5);  // a region pulled up (teal); pulled down is the accent (orange)
const char* const kBandIds[MultibandGraph::kBands] = {"low", "mid", "high"};
const char* const kSideNames[2] = {"Below", "Above"};
const QString kOffLabel = QStringLiteral("→ Mid");  // over a switched-off band's lane

// The time constants of what eases (s).
constexpr double kGainSeconds = 0.03;
constexpr double kGlowUpSeconds = 0.05, kGlowDownSeconds = 0.25;
constexpr double kLightSeconds = 0.06;
constexpr double kLaneSeconds = 0.08;
constexpr double kOffOpacity = 0.35, kMutedOpacity = 0.5;

double round1(double value) { return std::round(value * 10.0) / 10.0; }

double clampThreshold(double db) { return std::clamp(db, kMultibandMinThresholdDb, kMultibandMaxThresholdDb); }

// The texts the graph draws, with a true minus sign.
QString typeset(QString text) { return text.replace(QLatin1Char('-'), QChar(0x2212)); }

// The largest of the latest `count` values (NaN as the floor), or the value of largest magnitude.
double largest(const std::vector<float>& values, std::size_t count) {
    double most = -1e300;
    for (std::size_t i = values.size() - std::min(count, values.size()); i < values.size(); ++i) {
        const double v = std::isnan(values[i]) ? MultibandGraph::kFloorDb : double(values[i]);
        most = std::max(most, v);
    }
    return std::clamp(most, -1e3, 1e3);
}

double extreme(const std::vector<float>& values, std::size_t count) {
    double most = 0.0;
    for (std::size_t i = values.size() - std::min(count, values.size()); i < values.size(); ++i) {
        if (std::isfinite(values[i]) && std::abs(values[i]) > std::abs(most))
            most = values[i];
    }
    return most;
}

// What a side does to the level, by its ratio: the colour of its block (none at 1:1).
QColor sideColor(int side, double ratio) {
    if (ratio == 1.0)
        return Theme::kTextDim;
    const bool down = side == MultibandGraph::Above ? ratio > 1.0 : ratio < 1.0;
    return down ? Theme::kAccent : kBoost;
}

QColor mixed(const QColor& a, const QColor& b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(
        float(a.redF() + (b.redF() - a.redF()) * t), float(a.greenF() + (b.greenF() - a.greenF()) * t),
        float(a.blueF() + (b.blueF() - a.blueF()) * t), float(a.alphaF() + (b.alphaF() - a.alphaF()) * t));
}

// A rounded label behind `text`, `rect` its box.
void pill(SgPainter& p, const QRectF& rect, const QString& text, const QColor& color, const QFont& font,
          int alpha = 210) {
    p.fillRoundedRect(rect, 3, 3, withAlpha(Theme::kMeterBg, alpha));
    p.drawText(rect, Qt::AlignCenter, text, color, font);
}

}  // namespace

MultibandGraph::MultibandGraph(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(kWidth, kHeaderHeight + 3 * kMinRowHeight);
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    for (BandView& view : bands_) {
        view.in.reset(kFloorDb);
        view.out.reset(kFloorDb);
        view.opacity.snap(1.0);
        view.offLabel.snap(1.0);
    }
    // A device shown afresh is drawn as it is, not eased into.
    connect(this, &DeviceCanvas::deviceChanged, this, [this] { snap_ = true; });
}

QString MultibandGraph::paramId(int band, const char* field) {
    return QString::fromLatin1(kBandIds[index(band)]) + QLatin1Char('_') + QString::fromLatin1(field);
}

// --- Geometry ----------------------------------------------------------------------------------

double MultibandGraph::rowHeight() const { return std::max(0.0, (height() - kHeaderHeight) / 3.0); }

QRectF MultibandGraph::lane(int band) const {
    const double row = kHeaderHeight + (kBands - 1 - int(index(band))) * rowHeight();
    return QRectF(1.0, row + 2.0, std::max(1.0, width() - 2.0), std::max(1.0, rowHeight() - 4.0));
}

double MultibandGraph::xOfDb(double db) const {
    const QRectF l = lane(Mid);
    const double clamped = std::clamp(std::isfinite(db) ? db : kFloorDb, kFloorDb, kCeilingDb);
    return l.left() + (clamped - kFloorDb) / (kCeilingDb - kFloorDb) * l.width();
}

double MultibandGraph::dbAtX(double x) const {
    const QRectF l = lane(Mid);
    return std::clamp(kFloorDb + (x - l.left()) / l.width() * (kCeilingDb - kFloorDb), kFloorDb, kCeilingDb);
}

double MultibandGraph::pixelsPerDb() const { return lane(Mid).width() / (kCeilingDb - kFloorDb); }

QPointF MultibandGraph::aboveHandle(int band) const {
    return QPointF(xOfDb(bands_[index(band)].settings.above), lane(band).center().y());
}

QPointF MultibandGraph::belowHandle(int band) const {
    return QPointF(xOfDb(bands_[index(band)].settings.below), lane(band).center().y());
}

QPointF MultibandGraph::aboveBlockPoint(int band) const {
    const double edge = aboveHandle(band).x() + kHandleGrab + 3.0;
    return QPointF(std::max(edge, (aboveHandle(band).x() + lane(band).right()) / 2.0), lane(band).center().y());
}

QPointF MultibandGraph::belowBlockPoint(int band) const {
    const double edge = belowHandle(band).x() - kHandleGrab - 3.0;
    return QPointF(std::min(edge, (lane(band).left() + belowHandle(band).x()) / 2.0), lane(band).center().y());
}

void MultibandGraph::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size())
        Q_EMIT layoutChanged();
}

// --- The settings and the curve -------------------------------------------------------------------

double MultibandGraph::staticOutDb(int band, double inDb) const {
    const Settings& s = bands_[index(band)].settings;
    return inDb + sub::app::multibandGainDb(inDb, s.above, s.aboveRatio, s.below, s.belowRatio, softKnee_, amount_);
}

std::pair<double, double> MultibandGraph::changeSpan(int band) const {
    const BandView& view = bands_[index(band)];
    const double out = view.out.level, in = view.in.level, gain = view.gain.value;
    return {gain < 0.0 ? std::clamp(in, out, out - gain) : std::clamp(in, out - gain, out), out};
}

std::optional<double> MultibandGraph::targetMarkerDb(int band) const {
    const BandView& view = bands_[index(band)];
    if (view.inRead <= kFloorDb)
        return std::nullopt;
    // (By the readings, not the meters: they fall on after the audio stops.)
    const double target = staticOutDb(band, view.inRead);
    if (std::abs(std::max(target, kFloorDb) - std::max(view.outRead, kFloorDb)) <= 0.5)
        return std::nullopt;
    return target;
}

double MultibandGraph::sideGainDb(int band, int side, double levelDb) const {
    const Settings& s = bands_[index(band)].settings;
    return side == Above
               ? sub::app::multibandGainDb(levelDb, s.above, s.aboveRatio, s.below, 1.0, softKnee_, amount_)
               : sub::app::multibandGainDb(levelDb, s.above, 1.0, s.below, s.belowRatio, softKnee_, amount_);
}

QString MultibandGraph::ratioText(double ratio) const { return sub::app::formatValue(ratio, QStringLiteral("ratio")); }

double MultibandGraph::parseRatio(const QString& text) const { return sub::app::multibandParseRatio(text); }

QVariant MultibandGraph::parseTime(const QString& text) const {
    const std::optional<double> ms = sub::app::multibandParseMs(text);
    return ms ? QVariant(*ms) : QVariant::fromValue(nullptr);
}

void MultibandGraph::sync() {
    for (int b = 0; b < kBands; ++b) {
        BandView& view = bands_[std::size_t(b)];
        Settings& s = view.settings;
        s.above = value(paramId(b, "above"));
        s.aboveRatio = value(paramId(b, "above_ratio"));
        s.below = value(paramId(b, "below"));
        s.belowRatio = value(paramId(b, "below_ratio"));
        s.solo = value(paramId(b, "solo")) >= 0.5;
        view.on = b == Mid || value(paramId(b, "on")) >= 0.5;
    }
    amount_ = value(QStringLiteral("amount"));
    softKnee_ = value(QStringLiteral("soft_knee")) >= 0.5;
    const sub::app::Device* found = device();
    const bool sidechained = found != nullptr && found->sidechain.has_value();
    if (sidechained != sidechained_) {
        sidechained_ = sidechained;
        Q_EMIT sidechainedChanged();
    }
    setTargets();
    if (snap_ && alive()) {
        snap_ = false;
        for (BandView& view : bands_) {
            view.opacity.snap(view.opacity.target);
            view.offLabel.snap(view.offLabel.target);
            for (int side : {Below, Above}) {
                view.glow[side].snap(view.glow[side].target);
                view.handleLight[side].snap(view.handleLight[side].target);
                view.blockLight[side].snap(view.blockLight[side].target);
            }
        }
    }
    update();
}

void MultibandGraph::setTargets() {
    const BandView &low = bands_[Low], &mid = bands_[Mid], &high = bands_[High];
    // Solo as the engine has it: a band switched off follows Mid's.
    const bool anySolo = mid.settings.solo || (low.settings.solo && low.on) || (high.settings.solo && high.on);
    // (A drag's bubble sits over the lane above the handle, or by the mouse: a "→ Mid" under it makes way.)
    const QRectF bubble = bubbleRect().adjusted(-1.0, -1.0, 1.0, 1.0);
    for (int b = 0; b < kBands; ++b) {
        BandView& view = bands_[std::size_t(b)];
        const bool solo = b == Mid || !view.on ? mid.settings.solo : view.settings.solo;
        view.opacity.target = !view.on ? kOffOpacity : (!anySolo || solo ? 1.0 : kMutedOpacity);
        view.offLabel.target = !view.on && bubble.isValid() && bubble.intersects(offLabelRect(b)) ? 0.0 : 1.0;
        // Working by the level the display last reported (held a while, then the floor), not by the falling
        // meter: once the audio stops, a meter passing through a Below region isn't the band being lifted.
        const bool sounding = view.on && view.inRead > kFloorDb + 0.5;
        for (int side : {Below, Above}) {
            const bool working = sounding && std::abs(sideGainDb(b, side, view.inRead)) >= 0.1;
            view.glow[side].target = working ? 1.0 : 0.0;
            auto held = [&](bool handle) {
                if (drag_) {
                    const Target& t = drag_->target;
                    return t.handle == handle && (drag_->every || t.band == b) &&
                           (t.side == side || (handle && drag_->both));
                }
                return hover_ && *hover_ == Target{b, side, handle};
            };
            view.handleLight[side].target = held(true) ? 1.0 : 0.0;
            view.blockLight[side].target = held(false) ? 1.0 : 0.0;
        }
    }
}

// --- Displays and animation ----------------------------------------------------------------------

double MultibandGraph::lettingGoGain(const BandView& view) {
    // The meters fall at one rate, so the change between them is the reading until one reaches the floor; from
    // there the change goes on past it (as changeSpan draws it while playing), and the figure keeps the reading
    // until the other meter is at the floor too.
    const double in = view.in.level, out = view.out.level, between = out - in;
    if (out <= kFloorDb && in > kFloorDb)  // a cut, on under the floor
        return std::min(between, view.gainRead);
    if (in <= kFloorDb && out > kFloorDb)  // a lift, from under it
        return std::max(between, view.gainRead);
    return between;  // (both at the floor: none)
}

void MultibandGraph::refreshDisplays() {
    static const char* const kKinds[3] = {"in", "out", "gain"};
    std::array<std::array<std::vector<float>, 3>, kBands> read;
    bool any = false;
    for (int b = 0; b < kBands; ++b) {  // (all nine, so they stay in step)
        for (int k = 0; k < 3; ++k) {
            read[std::size_t(b)][std::size_t(k)] = readDisplay(paramId(b, kKinds[k]));
            any = any || !read[std::size_t(b)][std::size_t(k)].empty();
        }
    }
    quietTicks_ = any ? 0 : quietTicks_ + 1;
    const bool lettingGo = !any && quietTicks_ > kHoldTicks;  // the audio stopped (or the device)
    // Only the recent values: a buffer's worth arrives at once, but after a stall the backlog is old audio.
    const auto latest = std::size_t(std::max(1.0, std::ceil(kRecentSpan * sampleRate() / kMeterSamples)));
    bool moved = false;
    for (int b = 0; b < kBands; ++b) {
        BandView& view = bands_[std::size_t(b)];
        const auto& values = read[std::size_t(b)];
        if (any) {
            if (!values[0].empty())
                view.inRead = largest(values[0], latest);
            if (!values[1].empty())
                view.outRead = largest(values[1], latest);
            if (!values[2].empty())
                view.gainRead = extreme(values[2], latest);
        } else if (lettingGo) {
            view.inRead = view.outRead = kFloorDb;
        }
        const MeterBallistics in = view.in, out = view.out;
        view.in.update(view.inRead, kTick, 36.0, 0.8, kFloorDb);
        view.out.update(view.outRead, kTick, 36.0, 1.0, kFloorDb);
        moved = moved || in.level != view.in.level || in.peak != view.in.peak || out.level != view.out.level ||
                out.peak != view.out.peak;
        view.gain.target = lettingGo ? lettingGoGain(view) : view.gainRead;
        moved = view.gain.step(easeFraction(kTick, kGainSeconds), 1e-3) || moved;
    }
    setTargets();
    for (BandView& view : bands_) {
        for (int side : {Below, Above}) {
            Eased& glow = view.glow[side];
            const double seconds = glow.target > glow.value ? kGlowUpSeconds : kGlowDownSeconds;
            moved = glow.step(easeFraction(kTick, seconds), 1e-3) || moved;
            moved = view.handleLight[side].step(easeFraction(kTick, kLightSeconds), 1e-3) || moved;
            moved = view.blockLight[side].step(easeFraction(kTick, kLightSeconds), 1e-3) || moved;
        }
        moved = view.opacity.step(easeFraction(kTick, kLaneSeconds), 1e-3) || moved;
        moved = view.offLabel.step(easeFraction(kTick, kLightSeconds), 1e-3) || moved;
    }
    animating_ = moved;
    if (moved) {
        update();
        Q_EMIT levelsChanged();
    }
}

// --- Mouse -------------------------------------------------------------------------------------

std::optional<MultibandGraph::Target> MultibandGraph::targetAt(const QPointF& pos) const {
    for (int b = 0; b < kBands; ++b) {
        const QRectF l = lane(b);
        if (pos.y() < l.top() || pos.y() > l.bottom() || pos.x() < 0.0 || pos.x() > width())
            continue;
        const Settings& s = bands_[std::size_t(b)].settings;
        const double xAbove = xOfDb(s.above), xBelow = xOfDb(s.below);
        const double toAbove = std::abs(pos.x() - xAbove), toBelow = std::abs(pos.x() - xBelow);
        if (std::min(toAbove, toBelow) <= kHandleGrab) {
            int side = toAbove < toBelow ? Above : Below;
            if (toAbove == toBelow)  // (on top of each other: the side the mouse is on)
                side = pos.x() < xAbove ? Below : Above;
            return Target{b, side, true};
        }
        if (pos.x() < xBelow)
            return Target{b, Below, false};
        if (pos.x() > xAbove)
            return Target{b, Above, false};
        return std::nullopt;
    }
    return std::nullopt;
}

void MultibandGraph::setHover(const std::optional<Target>& target, const std::optional<QPointF>& at) {
    if (target != hover_) {
        if (!target)
            unsetCursor();
        else
            setCursor(target->handle ? Qt::SizeHorCursor : Qt::SizeVerCursor);
        hover_ = target;
        setTargets();
    }
    if (at != hoverAt_) {
        hoverAt_ = at;
        update();
    }
}

void MultibandGraph::hoverEnterEvent(QHoverEvent* event) { hoverMoveEvent(event); }

void MultibandGraph::hoverMoveEvent(QHoverEvent* event) {
    if (drag_)
        return;
    const QPointF pos = event->position();
    bool inLane = false;
    for (int b = 0; b < kBands; ++b)
        inLane = inLane || (pos.y() >= lane(b).top() && pos.y() <= lane(b).bottom());
    setHover(targetAt(pos), inLane ? std::optional<QPointF>(pos) : std::nullopt);
}

void MultibandGraph::hoverLeaveEvent(QHoverEvent*) {
    if (!drag_)
        setHover(std::nullopt, std::nullopt);
}

void MultibandGraph::mousePressEvent(QMouseEvent* event) {
    const bool second = secondPressOfDoubleClick(event);
    // Ctrl+Alt-drag scrolls the device chain (its area takes the press before the graph sees it).
    const std::optional<Target> target = event->button() == Qt::LeftButton && !isPanModifier(event->modifiers())
                                             ? targetAt(event->position())
                                             : std::nullopt;
    if (!target) {  // between the thresholds, the header: the frame's (selecting the device)
        event->ignore();
        return;
    }
    if (second)
        return;  // (the double-click follows)
    wheel_.reset();  // (a wheel run after this starts from what the drag leaves)
    Drag drag;
    drag.target = *target;
    drag.gesture = newGestureKey();
    drag.lastX = event->position().x();
    drag.lastY = event->position().y();
    drag.at = event->position();
    drag.every = event->modifiers() & Qt::ControlModifier;
    drag.both = target->handle && (event->modifiers() & Qt::AltModifier);
    for (int b = 0; b < kBands; ++b)
        drag.start[std::size_t(b)] = bands_[std::size_t(b)].settings;
    drag_ = drag;
    const char* field = target->handle ? (target->side == Above ? "above" : "below")
                                       : (target->side == Above ? "above_ratio" : "below_ratio");
    touch(paramId(target->band, field));
    hoverAt_.reset();
    setTargets();
    update();
}

void MultibandGraph::mouseMoveEvent(QMouseEvent* event) {
    if (drag_)
        dragTo(event->position(), event->modifiers());
}

void MultibandGraph::mouseReleaseEvent(QMouseEvent* event) {
    if (!drag_)
        return;
    drag_.reset();
    // What is under the mouse now (the cursor too).
    hover_.reset();
    unsetCursor();
    setHover(targetAt(event->position()), std::nullopt);
    setTargets();
    update();
}

void MultibandGraph::mouseUngrabEvent() {
    if (!drag_)
        return;
    drag_.reset();
    hover_.reset();
    unsetCursor();
    setTargets();
    update();
}

void MultibandGraph::dragTo(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    Drag& drag = *drag_;
    // Shift scales what is moved from here on (pressed or let go mid-drag, nothing jumps).
    const double fine = modifiers & Qt::ShiftModifier ? kFineFactor : 1.0;
    drag.dx += (pos.x() - drag.lastX) * fine;
    drag.dy += (pos.y() - drag.lastY) * fine;
    drag.lastX = pos.x();
    drag.lastY = pos.y();
    drag.at = pos;
    if (drag.target.handle) {
        writeThresholds(drag, drag.dx / pixelsPerDb(), drag.gesture);
    } else {
        // The block's level follows the mouse: down in Above's compresses, up in Below's lifts.
        const double sign = drag.target.side == Above ? 1.0 : -1.0;
        writeRatios(drag, std::pow(2.0, sign * drag.dy / kRatioPixels), drag.gesture);
    }
    setTargets();  // (the bubble moved)
    update();
}

void MultibandGraph::writeThresholds(const Drag& drag, double deltaDb, const QString& gesture) {
    sub::app::OrderedMap<QString, double> values;
    const int side = drag.target.side;
    for (const int b : {drag.target.band, int(Low), int(Mid), int(High)}) {  // (the one grabbed first: its lane shows)
        if (values.contains(paramId(b, "above")) || (b != drag.target.band && !drag.every))
            continue;
        const Settings& s = drag.start[index(b)];
        double above = s.above, below = s.below;
        if (drag.both) {  // the pair moves, keeping its gap, and stops at either end
            const double delta = std::clamp(deltaDb, kMultibandMinThresholdDb - std::min(above, below),
                                            kMultibandMaxThresholdDb - std::max(above, below));
            above = clampThreshold(round1(above + delta));
            below = clampThreshold(round1(below + delta));
        } else if (side == Above) {  // pushing the other along: Above never under Below
            above = clampThreshold(round1(above + deltaDb));
            below = std::min(below, above);
        } else {
            below = clampThreshold(round1(below + deltaDb));
            above = std::max(above, below);
        }
        if (side == Above) {
            values.insert(paramId(b, "above"), above);
            values.insert(paramId(b, "below"), below);
        } else {
            values.insert(paramId(b, "below"), below);
            values.insert(paramId(b, "above"), above);
        }
    }
    setParams(values, gesture, QStringLiteral("Change Threshold"));
}

double MultibandGraph::ratioStep(double ratio) {
    ratio = std::clamp(ratio, kMultibandMinRatio, kMultibandMaxRatio);
    if (std::abs(ratio - 1.0) <= kRatioDetent)
        return 1.0;
    const double scale = std::pow(10.0, 2.0 - std::floor(std::log10(ratio)));  // 3 significant digits
    return std::clamp(std::round(ratio * scale) / scale, kMultibandMinRatio, kMultibandMaxRatio);
}

void MultibandGraph::writeRatios(const Drag& drag, double factor, const QString& gesture) {
    sub::app::OrderedMap<QString, double> values;
    const int side = drag.target.side;
    const char* field = side == Above ? "above_ratio" : "below_ratio";
    for (const int b : {drag.target.band, int(Low), int(Mid), int(High)}) {
        if (values.contains(paramId(b, field)) || (b != drag.target.band && !drag.every))
            continue;
        values.insert(paramId(b, field), ratioStep(drag.start[index(b)].ratio(side) * factor));
    }
    setParams(values, gesture, QStringLiteral("Change Ratio"));
}

void MultibandGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    const std::optional<Target> target = event->button() == Qt::LeftButton && !isPanModifier(event->modifiers())
                                             ? targetAt(event->position())
                                             : std::nullopt;
    if (!target) {
        event->ignore();
        return;
    }
    drag_.reset();
    wheel_.reset();
    Drag reset;
    reset.target = *target;
    for (int b = 0; b < kBands; ++b)
        reset.start[std::size_t(b)] = bands_[std::size_t(b)].settings;
    const Settings& s = reset.start[index(target->band)];
    if (target->handle) {  // the threshold to its default, pushing the other if need be
        const double to = defaultValue(paramId(target->band, target->side == Above ? "above" : "below"));
        writeThresholds(reset, to - s.threshold(target->side), newGestureKey());
    } else {  // the ratio to 1:1
        setParams({{paramId(target->band, target->side == Above ? "above_ratio" : "below_ratio"), 1.0}},
                  newGestureKey(), QStringLiteral("Change Ratio"));
    }
    setTargets();
    update();
}

void MultibandGraph::wheelEvent(QWheelEvent* event) {
    const int delta = event->angleDelta().y() ? event->angleDelta().y() : event->angleDelta().x();  // (Alt: sideways)
    const std::optional<Target> target = targetAt(event->position());
    // (Shift+wheel scrolls the device chain, as everywhere over it.)
    if (!target || delta == 0 || drag_ || (event->modifiers() & Qt::ShiftModifier)) {
        event->ignore();
        return;
    }
    event->accept();
    // A run of notches on one target is one gesture (one undo step), worked out from where it began.
    if (!wheel_ || wheel_->target != *target || !wheelClock_.isValid() ||
        wheelClock_.elapsed() > kWheelGesture * 1000.0) {
        Drag run;
        run.target = *target;
        run.gesture = newGestureKey();
        for (int b = 0; b < kBands; ++b)
            run.start[std::size_t(b)] = bands_[std::size_t(b)].settings;
        wheel_ = run;
        wheelNotches_ = 0.0;
    }
    wheelClock_.start();
    wheelNotches_ += delta / 120.0;
    if (target->handle) {  // half a dB a notch
        writeThresholds(*wheel_, wheelNotches_ * 0.5, wheel_->gesture);
    } else {  // 2^(1/8) a notch, up: louder in that region (a lower ratio above, a higher one below)
        const double sign = target->side == Above ? -1.0 : 1.0;
        writeRatios(*wheel_, std::pow(2.0, sign * wheelNotches_ / 8.0), wheel_->gesture);
    }
}

// --- Painting ----------------------------------------------------------------------------------

void MultibandGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QFont font7 = uiFont(7);
    // The level axis' figures over the lanes. (Not the +6 dB at the right edge: 6 dB from the 0, it read as one
    // figure with it, "0 +6"; the brighter 0 dB line says what lies past it.)
    const QRectF top = lane(High);
    for (const int db : {-80, -60, -40, -20, 0}) {
        const double x = xOfDb(db);
        const bool edge = db == -80;  // (at the lanes' left edge: from it)
        const QRectF rect = edge ? QRectF(top.left() + 1, 0, 40, kHeaderHeight) : QRectF(x - 20, 0, 40, kHeaderHeight);
        p.drawText(rect, (edge ? Qt::AlignLeft : Qt::AlignHCenter) | Qt::AlignVCenter, typeset(QString::number(db)),
                   Theme::kTextDim, font7);
    }
    for (const int band : {High, Mid, Low})
        paintLane(p, band);
    paintBubble(p);
}

void MultibandGraph::paintLane(SgPainter& p, int band) const {
    const BandView& view = bands_[index(band)];
    const Settings& s = view.settings;
    const QRectF l = lane(band);
    const double ym = std::round(l.center().y());
    const QFont font7 = uiFont(7);
    const double amount = std::max(0.25, std::clamp(amount_ / 100.0, 0.0, 1.0));  // (blocks stay visible at 0 %)

    p.save();
    p.setOpacity(view.opacity.value);
    p.fillRoundedRect(l, 3, 3, Theme::kMeterBg);
    for (int db = -70; db <= 0; db += 10)  // the grid, 0 dB brighter
        p.fillRect(QRectF(std::round(xOfDb(db)), l.top() + 1, 1, l.height() - 2),
                   withAlpha(Theme::kGridBeat, db == 0 ? 160 : 70));

    // The regions: tinted by what they do, hatched as densely as their ratio is far from 1:1.
    const QRectF inner = l.adjusted(1, 1, -1, -1);
    for (const int side : {Below, Above}) {
        const double ratio = s.ratio(side);
        const double x = xOfDb(s.threshold(side));
        const QRectF block = side == Below
                                 ? QRectF(inner.left(), inner.top(), std::max(0.0, x - inner.left()), inner.height())
                                 : QRectF(x, inner.top(), std::max(0.0, inner.right() - x), inner.height());
        if (block.width() <= 0.0)
            continue;
        const double light = view.blockLight[side].value;
        if (ratio == 1.0) {  // doing nothing: only an outline while it is under the mouse
            if (light > 0.01)
                p.drawRect(block.adjusted(0.5, 0.5, -0.5, -0.5), withAlpha(Theme::kTextDim, int(60 * light)));
            continue;
        }
        const QColor color = sideColor(side, ratio);
        const double far = std::abs(std::log(ratio));
        const double alpha =
            (18.0 + 42.0 * std::min(1.0, far / std::log(8.0))) * amount + 30.0 * view.glow[side].value + 14.0 * light;
        p.fillRect(block, withAlpha(color, int(alpha)));
        const double spacing = 3.0 + 9.0 / (1.0 + 2.0 * far);
        const int hatch = int(90.0 * std::min(1.0, far / std::log(2.0)) * amount);
        if (hatch > 0) {
            const QColor line = withAlpha(color, hatch);
            for (double at = spacing; at < block.width(); at += spacing) {
                const double hx = side == Below ? x - at : x + at;
                p.fillRect(QRectF(std::round(hx), block.top(), 1, block.height()), line);
            }
        }
    }
    p.drawRoundedRect(l.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3, withAlpha(Theme::kGridBeat, 150));

    // The level after the dynamics (thick), the change between it and the level before (thin).
    const double outDb = view.out.level, inDb = view.in.level, gain = view.gain.value;
    const double left = l.left() + 1.0;
    if (outDb > kFloorDb || inDb > kFloorDb) {
        const double outX = xOfDb(outDb);
        const double fromX = xOfDb(changeSpan(band).first);  // where it would be without the change
        const double barTop = ym - 6.0, barHeight = 12.0;
        const double meterEnd = std::min(outX, fromX);
        const double yellow = xOfDb(-12.0), red = xOfDb(-3.0);
        if (meterEnd > left) {
            p.fillRect(QRectF(left, barTop, std::min(meterEnd, yellow) - left, barHeight), Theme::kMeterLow);
            if (meterEnd > yellow)
                p.fillRect(QRectF(yellow, barTop, std::min(meterEnd, red) - yellow, barHeight), Theme::kMeterMid);
            if (meterEnd > red)
                p.fillRect(QRectF(red, barTop, meterEnd - red, barHeight), Theme::kMeterHigh);
        }
        if (std::abs(gain) >= 0.05 && std::abs(fromX - outX) >= 0.5) {
            const QColor color = gain < 0.0 ? Theme::kAccent : kBoost;
            const int alpha = int(std::min(220.0, 60.0 + 160.0 * std::abs(gain) / 12.0));
            const double x0 = std::max(left, std::min(outX, fromX)), x1 = std::max(outX, fromX);
            p.fillRect(QRectF(x0, barTop, x1 - x0, barHeight), withAlpha(color, alpha));
            p.fillRect(QRectF(outX - 0.75, barTop, 1.5, barHeight), color);  // the bright edge, where it is now
        }
        if (view.out.peak > kFloorDb) {
            const double px = xOfDb(view.out.peak);
            p.fillRect(QRectF(std::max(left, px - 1.5), barTop, 1.5, barHeight),
                       view.out.peak >= 0.0 ? Theme::kMeterHigh : Theme::kText);
        }
        if (inDb > kFloorDb)
            p.fillRect(QRectF(left, ym + 8.0, xOfDb(inDb) - left, 2.0), withAlpha(Theme::kText, 150));
        // Where the static curve is taking it (leading while attack or release catch up).
        if (const std::optional<double> target = targetMarkerDb(band)) {
            const double tx = xOfDb(*target);
            const QPointF tip[3] = {{tx - 2.5, ym - 11.0}, {tx + 2.5, ym - 11.0}, {tx, ym - 7.0}};
            p.drawPolygon(tip, 3, withAlpha(Theme::kText, 200), 1.0);
        }
    }

    // The thresholds' handles, glowing while their side works.
    for (const int side : {Below, Above}) {
        const double x = xOfDb(s.threshold(side));
        const QColor color = sideColor(side, s.ratio(side));
        const double light = view.handleLight[side].value, glow = view.glow[side].value;
        if (glow > 0.01)
            drawGlowPolyline(p, {QPointF(x, l.top() + 2), QPointF(x, l.bottom() - 2)},
                             withAlpha(color, int(140 * glow)), 2.0);
        const QColor line = mixed(color, Theme::kText, 0.5 * light);
        const double w = 2.0 + light;
        p.fillRect(QRectF(x - w / 2, l.top() + 1, w, l.height() - 2), line);
        const QRectF grip(x - 2.0 - light / 2, ym - 6.0, 4.0 + light, 12.0);
        p.fillRoundedRect(grip, 1.5, 1.5, mixed(color, Theme::kText, 0.35 + 0.45 * light));
        p.drawRoundedRect(grip, 1.5, 1.5, withAlpha(Theme::kMeterBg, 200), 1.0);
    }

    // The gain change in figures, while the band sounds (or is still letting go).
    if (view.on && (inDb > kFloorDb || std::abs(gain) >= 0.05)) {
        const QString text = std::abs(gain) < 0.05 ? QStringLiteral("0.0")
                                                   : typeset((gain > 0 ? QStringLiteral("+") : QString()) +
                                                             QString::number(gain, 'f', 1));
        const QColor color = std::abs(gain) < 0.05 ? Theme::kTextDim : (gain < 0 ? Theme::kAccent : kBoost);
        const double w = SgPainter::textWidth(text, font7) + 8.0;
        pill(p, QRectF(l.right() - 2.0 - w, l.top() + 1.0, w, std::min(11.0, ym - 7.0 - l.top())), text, color,
             font7);
    }

    // Under the mouse: the static curve at that level.
    if (hoverAt_ && !drag_ && hoverAt_->y() >= l.top() && hoverAt_->y() <= l.bottom()) {
        const double x = std::clamp(hoverAt_->x(), l.left() + 1, l.right() - 1);
        p.fillRect(QRectF(std::round(x), l.top() + 1, 1, l.height() - 2), withAlpha(Theme::kText, 110));
        const double level = dbAtX(x);
        const QString text =
            typeset(QStringLiteral("%1 → %2 dB")
                        .arg(QString::number(level, 'f', 1), QString::number(staticOutDb(band, level), 'f', 1)));
        const double w = SgPainter::textWidth(text, font7) + 8.0;
        const double tx = x + 4.0 + w <= l.right() - 2.0 ? x + 4.0 : x - 4.0 - w;
        pill(p, QRectF(tx, l.top() + 1.0, w, 11.0), text, Theme::kText, font7, 230);
    }
    p.restore();

    // A band switched off: the Mid band takes it.
    if (const double off = offLabelOpacity(band); off > 0.01) {
        p.save();
        p.setOpacity(off);
        pill(p, offLabelRect(band), kOffLabel, Theme::kTextDim, uiFont(8), 230);
        p.restore();
    }
}

QRectF MultibandGraph::offLabelRect(int band) const {
    const QRectF l = lane(band);
    const double w = SgPainter::textWidth(kOffLabel, uiFont(8)) + 12.0;
    return QRectF(l.center().x() - w / 2, std::round(l.center().y()) - 7.0, w, 14.0);
}

double MultibandGraph::offLabelOpacity(int band) const {
    const BandView& view = bands_[index(band)];
    if (view.on)
        return 0.0;
    return std::clamp((1.0 - view.opacity.value) / (1.0 - kOffOpacity), 0.0, 1.0) * view.offLabel.value;
}

QString MultibandGraph::bubbleText() const {
    const Target& t = drag_->target;
    const Settings& s = bands_[index(t.band)].settings;
    const QString value = t.handle ? sub::app::formatValue(s.threshold(t.side), QStringLiteral("dB"))
                                   : ratioText(s.ratio(t.side));
    return typeset(QStringLiteral("%1 %2").arg(QLatin1String(kSideNames[t.side]), value));
}

QRectF MultibandGraph::bubbleRect() const {
    if (!drag_)
        return {};
    const Target& t = drag_->target;
    const Settings& s = bands_[index(t.band)].settings;
    const double w = SgPainter::textWidth(bubbleText(), uiFont(8)) + 12.0, h = 16.0;
    QPointF at = t.handle ? QPointF(xOfDb(s.threshold(t.side)), lane(t.band).top() - 2.0 - h / 2)
                          : QPointF(drag_->at.x(), drag_->at.y() - 18.0);
    at.setX(std::clamp(at.x(), w / 2 + 1, width() - w / 2 - 1));
    at.setY(std::clamp(at.y(), h / 2, height() - h / 2));
    return QRectF(at.x() - w / 2, at.y() - h / 2, w, h);
}

void MultibandGraph::paintBubble(SgPainter& p) const {
    if (!drag_)
        return;
    const QRectF rect = bubbleRect();
    p.fillRoundedRect(rect, 3, 3, Theme::kPanelAlt);
    p.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3, Theme::kGridBar);
    p.drawText(rect, Qt::AlignCenter, bubbleText(), Theme::kText, uiFont(8));
}

}  // namespace sub::ui
