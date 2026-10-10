#include "devices/ReverbDecayGraph.h"

#include "audio/EngineBridge.h"
#include "devices/EditorPaint.h"
#include "input/GestureKey.h"
#include "model/ParamSpec.h"
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

constexpr double kPi = 3.14159265358979323846;
constexpr double kSwitchSeconds = 0.04;  // a switch's change eases over about 120 ms
constexpr double kEaseSeconds = 0.15;    // Freeze, the glow, the chorus's ripple
constexpr double kGrowSeconds = 0.06;    // a handle grows under the mouse
constexpr double kRipplePx = 1.2;        // the ripple's height at Chorus 100 % while the tail sounds
constexpr double kSpectrumTopDb = -12.0;  // the tail's spectrum over the axis' height
constexpr double kFineRatio = 0.25;       // Shift: the drag's ratios to this power
constexpr double kHandleInset = 6.0;      // px: a handle's centre stays this far inside the plot (its largest ring)
constexpr double kStaleSeconds = 0.1;     // a tick without values keeps the last this long (a long audio block)

// a + (b - a) t, of colours.
QColor mix(const QColor& a, const QColor& b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    const auto lerp = [t](float from, float to) { return float(from + (to - from) * t); };
    return QColor::fromRgbF(lerp(a.redF(), b.redF()), lerp(a.greenF(), b.greenF()), lerp(a.blueF(), b.blueF()),
                            lerp(a.alphaF(), b.alphaF()));
}

// "0.1 s", "1 s", "10 s": the axis' figures.
QString axisText(double seconds) { return pythonGeneral(seconds) + QStringLiteral(" s"); }

}  // namespace

ReverbDecayGraph::ReverbDecayGraph(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(kWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    setCursor(Qt::SizeVerCursor);
    meter_.reset(kMeterFloorDb);
    // The curve is the engine's at its sample rate: worked out again when the audio device changes.
    connect(this, &DeviceCanvas::deviceChanged, this, [this] {
        disconnect(bridgeConnection_);
        if (session())
            bridgeConnection_ = connect(session()->bridge(), &sub::app::EngineBridge::deviceChanged, this, [this] {
                updateCurve();
                switch_.snap(1.0);
                updateShown();
                update();
            });
    });
}

// --- Coordinates ------------------------------------------------------------------------

QRectF ReverbDecayGraph::plot() const {
    return QRectF(0, 0, width(), height()).adjusted(1, 1, -(1 + kMeterWidth + 2), -1);
}

QRectF ReverbDecayGraph::axisRect() const { return plot().adjusted(0, kHeader, 0, -3); }

QRectF ReverbDecayGraph::meterRect() const { return QRectF(width() - 1 - kMeterWidth, 1, kMeterWidth, height() - 2); }

double ReverbDecayGraph::xOf(double freq) const {
    const QRectF r = plot();
    return LogAxis{kLow, kHigh, r.left(), r.width()}.position(freq);
}

double ReverbDecayGraph::freqAt(double x) const {
    const QRectF r = plot();
    return LogAxis{kLow, kHigh, r.left(), r.width()}.valueAt(x);
}

double ReverbDecayGraph::logOf(double seconds) {
    // Held to the axis: what is off it (frozen: 1000 s) is drawn along its edge, and eases into and out of it.
    return std::log(std::clamp(std::isfinite(seconds) ? seconds : kMaxSeconds, kMinSeconds, kMaxSeconds));
}

double ReverbDecayGraph::yOfLog(double logSeconds) const {
    const QRectF r = axisRect();
    const double fraction = (logSeconds - std::log(kMinSeconds)) / std::log(kMaxSeconds / kMinSeconds);
    return r.bottom() - std::clamp(fraction, 0.0, 1.0) * r.height();
}

double ReverbDecayGraph::yOf(double seconds) const { return yOfLog(logOf(seconds)); }

double ReverbDecayGraph::secondsAt(double y) const {
    const QRectF r = axisRect();
    const double fraction = std::clamp((r.bottom() - y) / r.height(), 0.0, 1.0);
    return kMinSeconds * std::pow(kMaxSeconds / kMinSeconds, fraction);
}

bool ReverbDecayGraph::shelfOn(Handle handle) const {
    return handle == Lo ? settings_.loShelf : handle == Hi ? settings_.hiFilter : true;
}

QPointF ReverbDecayGraph::handleAt(Handle handle) const {
    const double decay = settings_.decayMs / 1000.0;
    QPointF at;
    switch (handle) {
    case Lo:
        at = QPointF(xOf(settings_.loFreq), yOf(settings_.loShelf ? decay * settings_.loGain / 100.0 : decay));
        break;
    case Hi: {
        const double seconds = !settings_.hiFilter ? decay
                               : settings_.hiLowpass ? lowpassAtHi_
                                                     : decay * settings_.hiGain / 100.0;
        at = QPointF(xOf(settings_.hiFreq), yOf(seconds));
        break;
    }
    case Decay: at = QPointF(xOf(std::sqrt(settings_.loFreq * settings_.hiFreq)), yOf(decay)); break;
    case None: return {};
    }
    // Inside the plot, ring and all (a shelf at the end of its range sits at the plot's edge).
    const QRectF inside = plot().adjusted(kHandleInset, kHandleInset, -kHandleInset, -kHandleInset);
    return QPointF(std::clamp(at.x(), inside.left(), inside.right()),
                   std::clamp(at.y(), inside.top(), inside.bottom()));
}

bool ReverbDecayGraph::gainless(Handle handle) const {
    return (handle == Lo && !settings_.loShelf) || (handle == Hi && (!settings_.hiFilter || settings_.hiLowpass));
}

ReverbDecayGraph::Handle ReverbDecayGraph::handleNear(const QPointF& pos, double within) const {
    Handle nearest = None;
    double best = within;
    for (const Handle handle : {Decay, Lo, Hi}) {  // (Decay first: it wins a tie)
        const QPointF d = handleAt(handle) - pos;
        const double distance = std::hypot(d.x(), d.y());
        if (distance < best) {
            best = distance;
            nearest = handle;
        }
    }
    return nearest;
}

QString ReverbDecayGraph::readout() const {
    const Handle shown = pressed_ != None ? pressed_ : hovered_;
    const auto hz = [](double v) { return sub::app::formatValue(v, QStringLiteral("Hz")); };
    const auto percent = [](double v) { return sub::app::formatValue(v, QStringLiteral("%")); };
    switch (shown) {
    case Lo:
        return settings_.loShelf ? QStringLiteral("Lo %1 · %2").arg(hz(settings_.loFreq), percent(settings_.loGain))
                                 : QStringLiteral("Lo off");
    case Hi:
        if (!settings_.hiFilter)
            return QStringLiteral("Hi off");
        return settings_.hiLowpass ? QStringLiteral("Low-pass %1").arg(hz(settings_.hiFreq))
                                   : QStringLiteral("Hi %1 · %2").arg(hz(settings_.hiFreq), percent(settings_.hiGain));
    case Decay:
    case None: break;
    }
    if (settings_.freeze && shown == None)
        return QStringLiteral("Frozen");
    return sub::app::formatValue(settings_.decayMs, QStringLiteral("ms"));
}

// --- The curve --------------------------------------------------------------------------

void ReverbDecayGraph::sync() {
    const bool first = !synced_;
    const sub::app::ReverbDecaySettings before = settings_;
    sub::app::ReverbDecaySettings& s = settings_;
    s.decayMs = value(QStringLiteral("decay"));
    s.size = value(QStringLiteral("size"));
    s.scale = value(QStringLiteral("scale"));
    s.density = int(std::lround(value(QStringLiteral("density"))));
    s.loShelf = value(QStringLiteral("lo_shelf")) >= 0.5;
    s.hiFilter = value(QStringLiteral("hi_filter")) >= 0.5;
    s.hiLowpass = value(QStringLiteral("hi_type")) >= 0.5;
    s.freeze = value(QStringLiteral("freeze")) >= 0.5;
    s.flat = value(QStringLiteral("flat")) >= 0.5;
    s.cut = value(QStringLiteral("cut")) >= 0.5;
    s.loFreq = value(QStringLiteral("lo_freq"));
    s.loGain = value(QStringLiteral("lo_gain"));
    s.hiFreq = value(QStringLiteral("hi_freq"));
    s.hiGain = value(QStringLiteral("hi_gain"));
    chorus_ = value(QStringLiteral("chorus")) >= 0.5;
    chorusAmount_ = value(QStringLiteral("chorus_amount"));
    dry_ = value(QStringLiteral("mix")) <= 0.0;

    // A switch eases the curve from where it is drawn now; Freeze has an easing of its own.
    const bool switched = !first && (before.loShelf != s.loShelf || before.hiFilter != s.hiFilter ||
                                      before.hiLowpass != s.hiLowpass || before.density != s.density ||
                                      before.flat != s.flat || before.cut != s.cut);
    if (switched && shown_.size() == target_.size()) {
        from_ = shown_;
        switch_.value = 0.0;
        switch_.target = 1.0;
    }
    frozen_.target = s.freeze ? 1.0 : 0.0;
    depth_.target = chorus_ ? std::clamp(chorusAmount_ / 100.0, 0.0, 1.0) : 0.0;
    if (first) {
        frozen_.snap(frozen_.target);
        depth_.snap(depth_.target);
        switch_.snap(1.0);
    }
    synced_ = true;
    updateCurve();
    updateShown();
    if (first || before.freeze != s.freeze)
        Q_EMIT curveChanged();
    update();
}

void ReverbDecayGraph::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        updateCurve();
        switch_.snap(1.0);  // (a different number of columns: nothing to ease from)
        updateShown();
        updateColumns();
    }
}

void ReverbDecayGraph::updateCurve() {
    const double rate = sampleRate();
    const int columns = std::max(2, int(plot().width()));
    QList<double> frequencies;
    frequencies.reserve(columns + 1);
    for (int i = 0; i <= columns; ++i) frequencies.append(kLow * std::pow(kHigh / kLow, double(i) / columns));
    const QList<double> seconds = sub::app::reverbDecaySeconds(settings_, rate, frequencies);
    sub::app::ReverbDecaySettings other = settings_;
    other.freeze = !other.freeze;
    const QList<double> others = sub::app::reverbDecaySeconds(other, rate, frequencies);
    frequencies_.assign(frequencies.begin(), frequencies.end());
    seconds_.assign(seconds.begin(), seconds.end());
    other_.assign(others.begin(), others.end());
    // The Low-pass's handle sits on the curve (unfrozen) at its frequency.
    sub::app::ReverbDecaySettings unfrozen = settings_;
    unfrozen.freeze = false;
    lowpassAtHi_ = sub::app::reverbDecaySeconds(unfrozen, rate, {settings_.hiFreq}).value(0, 1.0);
    updateTarget();
}

void ReverbDecayGraph::updateTarget() {
    // Between the unfrozen curve and the frozen one, in log seconds (each held to the axis, so a curve off
    // it eases into and out of its edge rather than racing to it and stopping dead), as Freeze eases.
    const std::vector<double>& unfrozen = settings_.freeze ? other_ : seconds_;
    const std::vector<double>& frozen = settings_.freeze ? seconds_ : other_;
    const double t = frozen_.value;
    target_.resize(seconds_.size());
    for (std::size_t i = 0; i < target_.size(); ++i) {
        const double a = logOf(unfrozen[i]), b = logOf(frozen[i]);
        target_[i] = a + (b - a) * t;
    }
}

void ReverbDecayGraph::updateShown() {
    if (from_.size() != target_.size() || switch_.value >= 1.0) {
        shown_ = target_;
        return;
    }
    shown_.resize(target_.size());
    const double t = switch_.value;
    for (std::size_t i = 0; i < target_.size(); ++i) shown_[i] = from_[i] + (target_[i] - from_[i]) * t;
}

std::vector<double> ReverbDecayGraph::shownY() const {
    std::vector<double> ys;
    ys.reserve(shown_.size());
    for (const double l : shown_) ys.push_back(yOfLog(l));
    return ys;
}

void ReverbDecayGraph::updateColumns() { columns_ = spectrum_.columns(std::max(2, int(plot().width())), kLow, kHigh); }

// --- Displays and animation -------------------------------------------------------------

void ReverbDecayGraph::refreshDisplays() {
    const double seconds = clock_.isValid() ? std::clamp(clock_.restart() / 1000.0, 0.0, 0.1) : 1.0 / 60.0;
    if (!clock_.isValid())
        clock_.start();
    const std::vector<float> tail = readDisplay(QStringLiteral("tail"));
    if (spectrum_.add(tail.data(), tail.size(), sampleRate())) {
        updateColumns();
        spectrumChanged_ = true;
    }
    // The tick's loudest value. A tick that brings none (the audio's blocks longer than a tick) keeps the
    // last for a moment: only a while without any is silence (asleep, the engine publishes the floor).
    const std::vector<float> diffuse = readDisplay(QStringLiteral("diffuse"));
    if (!diffuse.empty()) {
        diffuseDb_ = kMeterFloorDb;
        for (const float v : diffuse) diffuseDb_ = std::max(diffuseDb_, double(v));
        stale_ = 0.0;
    } else if ((stale_ += seconds) > kStaleSeconds) {
        diffuseDb_ = kMeterFloorDb;
    }
    const std::vector<float> chorus = readDisplay(QStringLiteral("chorus"));
    if (!chorus.empty() && chorus.back() >= 0.0f && double(chorus.back()) != chorusPhase_) {
        chorusPhase_ = chorus.back();
        chorusMoved_ = true;
    }
    advance(seconds);
}

void ReverbDecayGraph::advance(double seconds) {
    bool moving = spectrumChanged_;
    spectrumChanged_ = false;

    // The tail meter falls at least as fast as the tail does (60 dB in Decay), and the curve glows with it.
    const double fall = std::max(24.0, 1.2 * 60.0 / std::max(settings_.decayMs / 1000.0, 0.01));
    meter_.update(diffuseDb_, seconds, settings_.freeze ? 24.0 : fall, 1.0, kMeterFloorDb);
    if (meter_.level != paintedLevel_ || meter_.peak != paintedPeak_) {
        paintedLevel_ = meter_.level;
        paintedPeak_ = meter_.peak;
        moving = true;
    }
    glow_.target = std::clamp((meter_.level + 60.0) / 60.0, 0.0, 1.0);
    moving = glow_.step(easeFraction(seconds, kEaseSeconds), 1e-3) || moving;

    // The chorus's ripple runs along the curve while the tail sounds.
    moving = depth_.step(easeFraction(seconds, kEaseSeconds), 1e-3) || moving;
    if (chorusMoved_ && depth_.value * glow_.value > 0.0)
        moving = true;
    chorusMoved_ = false;

    // Freeze lifts the curve; a switch eases it to its new shape.
    bool curveMoved = frozen_.step(easeFraction(seconds, kEaseSeconds), 1e-3);
    if (curveMoved)
        updateTarget();
    curveMoved = switch_.step(easeFraction(seconds, kSwitchSeconds), 1e-3) || curveMoved;
    if (curveMoved) {
        updateShown();
        moving = true;
    }

    for (int h = 0; h < 3; ++h) {
        const Handle handle = Handle(h);
        grow_[std::size_t(h)].target = (handle == hovered_ || handle == pressed_) ? 1.0 : 0.0;
        moving = grow_[std::size_t(h)].step(easeFraction(seconds, kGrowSeconds), 1e-3) || moving;
    }

    if (moving != animating_ || moving) {
        animating_ = moving;
        Q_EMIT levelsChanged();
    }
    if (moving)
        update();
}

// --- The mouse ----------------------------------------------------------------------------

void ReverbDecayGraph::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    if (secondPressOfDoubleClick(event))  // the double-click switches the shelf instead
        return;
    // The handle under the mouse, else the one nearest across (the shelves' ranges overlap, so the
    // low one may be right of the high one).
    Handle handle = handleNear(event->position(), kGrab);
    if (handle == None) {
        double best = 1e9;
        for (const Handle h : {Decay, Lo, Hi}) {
            const double distance = std::abs(handleAt(h).x() - event->position().x());
            if (distance < best) {
                best = distance;
                handle = h;
            }
        }
    }
    pressed_ = handle;
    gesture_ = newGestureKey();
    fine_ = event->modifiers().testFlag(Qt::ShiftModifier);
    startDrag(event->position());
    touch(handle == Lo   ? QStringLiteral("lo_freq")
          : handle == Hi ? QStringLiteral("hi_freq")
                         : QStringLiteral("decay"));
    Q_EMIT levelsChanged();
    update();
}

void ReverbDecayGraph::startDrag(const QPointF& pos) {
    anchor_ = pos;
    startDecay_ = settings_.decayMs;
    startFreq_ = pressed_ == Lo ? settings_.loFreq : settings_.hiFreq;
    startGain_ = pressed_ == Lo ? settings_.loGain : settings_.hiGain;
}

void ReverbDecayGraph::mouseMoveEvent(QMouseEvent* event) {
    if (!gesture_.isEmpty())
        dragTo(event->position(), event->modifiers());
}

void ReverbDecayGraph::mouseReleaseEvent(QMouseEvent*) { endDrag(); }

void ReverbDecayGraph::mouseUngrabEvent() { endDrag(); }

void ReverbDecayGraph::endDrag() {
    if (gesture_.isEmpty() && pressed_ == None)
        return;
    gesture_.clear();
    pressed_ = None;
    Q_EMIT levelsChanged();
    update();
}

void ReverbDecayGraph::dragTo(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    const bool fine = modifiers.testFlag(Qt::ShiftModifier);
    if (fine != fine_) {  // Shift went down or up: measure from here
        fine_ = fine;
        startDrag(pos);
    }
    // Relative to the press, as ratios: across, of the frequency; up and down, of the time.
    const QRectF r = plot(), axis = axisRect();
    double rx = std::pow(kHigh / kLow, (pos.x() - anchor_.x()) / r.width());
    double ry = std::pow(kMaxSeconds / kMinSeconds, (anchor_.y() - pos.y()) / axis.height());
    if (fine_) {
        rx = std::pow(rx, kFineRatio);
        ry = std::pow(ry, kFineRatio);
    }
    const QString text = QStringLiteral("Change Reverb Decay");
    switch (pressed_) {
    // (A shelf switched off, or the Low-pass, has no gain the handle shows: across only.)
    case Lo:
        if (gainless(Lo))
            setParams({{QStringLiteral("lo_freq"), std::clamp(startFreq_ * rx, 20.0, 15000.0)}}, gesture_, text);
        else
            setParams({{QStringLiteral("lo_freq"), std::clamp(startFreq_ * rx, 20.0, 15000.0)},
                       {QStringLiteral("lo_gain"), std::clamp(startGain_ * ry, 20.0, 100.0)}},
                      gesture_, text);
        break;
    case Hi:
        if (gainless(Hi))
            setParams({{QStringLiteral("hi_freq"), std::clamp(startFreq_ * rx, 20.0, 16000.0)}}, gesture_, text);
        else
            setParams({{QStringLiteral("hi_freq"), std::clamp(startFreq_ * rx, 20.0, 16000.0)},
                       {QStringLiteral("hi_gain"), std::clamp(startGain_ * ry, 20.0, 100.0)}},
                      gesture_, text);
        break;
    case Decay:
        setParams({{QStringLiteral("decay"), std::clamp(startDecay_ * ry, 200.0, 60000.0)}}, gesture_, text);
        break;
    case None: break;
    }
}

void ReverbDecayGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    endDrag();
    const Handle handle = handleNear(event->position(), kGrab);
    if (handle != Lo && handle != Hi)
        return;
    const QString id = handle == Lo ? QStringLiteral("lo_shelf") : QStringLiteral("hi_filter");
    setParamValue(id, shelfOn(handle) ? 0.0 : 1.0, newGestureKey(), QStringLiteral("Switch Reverb Shelf"));
}

void ReverbDecayGraph::hoverMoveEvent(QHoverEvent* event) {
    const Handle handle = handleNear(event->position(), kGrab);
    setHovered(handle);
    setCursor(handle == Decay || handle == None ? Qt::SizeVerCursor
              : gainless(handle)                ? Qt::SizeHorCursor
                                                : Qt::SizeAllCursor);
}

void ReverbDecayGraph::hoverLeaveEvent(QHoverEvent*) { setHovered(None); }

void ReverbDecayGraph::setHovered(Handle handle) {
    if (handle == hovered_)
        return;
    hovered_ = handle;
    Q_EMIT levelsChanged();
    update();
}

// --- Painting ---------------------------------------------------------------------------

void ReverbDecayGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QRectF r = plot(), axis = axisRect();
    const QFont font = uiFont(7);
    p.fillRect(QRectF(0, 0, width(), height()), Theme::kMeterBg);
    p.save();
    p.setClipRect(r);
    drawDecadeGrid(p, r, LogAxis{kLow, kHigh, r.left(), r.width()});
    for (const double seconds : {0.1, 1.0, 10.0}) {  // the decay axis: a line per decade, figures under
        const double y = yOf(seconds);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y), withAlpha(Theme::kGridBeat, 120));
        p.drawText(QRectF(r.left() + 3, y + 1, 40, 11), Qt::AlignLeft | Qt::AlignVCenter, axisText(seconds),
                   Theme::kTextDisabled, font);
    }

    // The tail's spectrum as it dies away.
    const double floor = sub::app::analysis::FallingSpectrum::kFloorDb;
    const int columns = int(columns_.size());
    if (columns >= 2 && *std::max_element(columns_.begin(), columns_.end()) > floor) {
        std::vector<QPointF> shape;
        shape.reserve(std::size_t(columns));
        for (int i = 0; i < columns; ++i) {
            const double fraction = std::clamp((columns_[std::size_t(i)] - floor) / (kSpectrumTopDb - floor), 0.0, 1.0);
            shape.emplace_back(r.left() + (i + 0.5) * r.width() / columns, axis.bottom() - fraction * axis.height());
        }
        QLinearGradient gradient(QPointF(0, axis.top()), QPointF(0, r.bottom()));
        gradient.setColorAt(0, withAlpha(Theme::kScopeLine, 70));
        gradient.setColorAt(1, withAlpha(Theme::kScopeLine, 0));
        p.fillToBaseline(shape.data(), int(shape.size()), r.bottom(), gradient);
    }

    // Frozen: the plot takes the frozen tint.
    const double frozen = frozen_.value;
    if (frozen > 0.0)
        p.fillRect(r, withAlpha(Theme::kFrozen, int(34 * frozen)));

    // The curve: filled, glowing while the tail sounds, rippling with the chorus.
    const double glow = glow_.value;
    const QColor color = dry_ ? Theme::kTextDisabled : mix(Theme::kScopeLine, Theme::kFrozen, frozen);
    std::vector<QPointF> curve;
    curve.reserve(shown_.size());
    const double ripple = kRipplePx * depth_.value * glow;
    for (std::size_t i = 0; i < shown_.size() && i < frequencies_.size(); ++i) {
        const double along = 3.0 * double(i) / double(shown_.size());  // three ripples across
        const double wave = ripple > 0.0 ? ripple * std::sin(2.0 * kPi * (chorusPhase_ + along)) : 0.0;
        curve.emplace_back(xOf(frequencies_[i]), yOfLog(shown_[i]) + wave);
    }
    if (curve.size() >= 2) {
        p.fillToBaseline(curve.data(), int(curve.size()), r.bottom(), withAlpha(color, int(22 + 40 * glow)));
        drawGlowPolyline(p, curve, color, 1.5 + 1.0 * glow);
    }

    // The shelves' guides: how long their bands ring, level out to their edges. Frozen, the curve no
    // longer settles onto them (the handles are what thaws): they fade, and the handles dim.
    const QColor accent = dry_ ? Theme::kTextDisabled : Theme::kAccent;
    const int guide = int(90 * (1.0 - frozen));
    if (settings_.loShelf && guide > 0) {
        const QPointF at = handleAt(Lo);
        drawDashedPolyline(p, {QPointF(r.left(), at.y()), at}, withAlpha(accent, guide), 1.0);
    }
    if (settings_.hiFilter && !settings_.hiLowpass && guide > 0) {
        const QPointF at = handleAt(Hi);
        drawDashedPolyline(p, {at, QPointF(r.right(), at.y())}, withAlpha(accent, guide), 1.0);
    }
    p.restore();

    // The captions: what it shows, and the handle under the mouse (else Decay, or Frozen).
    p.drawText(QRectF(r.left() + 3, r.top(), 60, kHeader), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("Decay time"), Theme::kTextDim, font);
    const bool lit = pressed_ != None || hovered_ != None;
    p.drawText(QRectF(r.left() + 50, r.top(), r.width() - 53, kHeader), Qt::AlignRight | Qt::AlignVCenter, readout(),
               lit ? Theme::kText : frozen > 0.5 ? Theme::kFrozen : Theme::kTextDim, font);

    // The handles: rings, growing under the mouse, filled while held; a switched-off shelf's hollow and dim;
    // all of them dimmed while frozen (unless held or hovered).
    for (const Handle handle : {Lo, Hi, Decay}) {
        const QPointF at = handleAt(handle);
        const double grown = grow_[std::size_t(handle)].value;
        const double radius = 4.0 + 2.0 * grown;
        const bool on = shelfOn(handle);
        QColor ring = on ? accent : Theme::kTextDim;
        ring.setAlphaF(float(ring.alphaF() * (1.0 - 0.6 * frozen * (1.0 - grown))));
        p.fillEllipse(at, radius, radius, handle == pressed_ && on ? ring : Theme::kMeterBg);
        p.drawEllipse(QRectF(at.x() - radius, at.y() - radius, 2 * radius, 2 * radius), ring, on ? 2.0 : 1.2);
        if (handle == Decay && pressed_ != Decay)
            p.fillEllipse(at, 1.5, 1.5, ring);
    }

    // The tail's meter.
    drawLevelMeter(p, meterRect(), meter_.level, meter_.peak, kMeterFloorDb, 0.0);
}

}  // namespace sub::ui
