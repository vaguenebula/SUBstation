#include "devices/ReverbDecayGraph.h"

#include "audio/EngineBridge.h"
#include "devices/EditorPaint.h"
#include "input/GestureKey.h"
#include "model/ParamSpec.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QFontMetricsF>
#include <QHoverEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

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
// A long audio block: a tick reads a level display's newest values back this far (not a backlog's), and one
// without values keeps the last this long.
constexpr double kStaleSeconds = 0.1;
// The ranges its handles drag: the engine's, through the application layer.
constexpr double kMinShelfFreq = sub::app::kReverbMinShelfFreq;
constexpr double kMaxLoFreq = sub::app::kReverbMaxLoFreq, kMaxHiFreq = sub::app::kReverbMaxHiFreq;
constexpr double kMinShelfGain = sub::app::kReverbMinShelfGain, kMaxShelfGain = sub::app::kReverbMaxShelfGain;
constexpr double kMinDecayMs = sub::app::kReverbMinDecayMs, kMaxDecayMs = sub::app::kReverbMaxDecayMs;

// "0.1 s", "1 s", "10 s": the axis' figures.
QString axisText(double seconds) { return pythonGeneral(seconds) + QStringLiteral(" s"); }

// Where an axis figure drawn in `box` (left, middle) puts its ink: its width, the digits' height.
QRectF inkOf(const QRectF& box, const QString& text, const QFontMetricsF& metrics) {
    const double baseline = box.top() + (box.height() - metrics.height()) / 2.0 + metrics.ascent();
    return QRectF(box.left(), baseline - metrics.capHeight(), metrics.horizontalAdvance(text), metrics.capHeight());
}

// Whether a line (its points left to right) passes within `margin` px of `rect`.
bool passesNear(const std::vector<QPointF>& line, const QRectF& rect, double margin = 1.0) {
    const double left = rect.left() - margin, right = rect.right() + margin;
    const double top = rect.top() - margin, bottom = rect.bottom() + margin;
    for (std::size_t i = 1; i < line.size(); ++i) {
        const QPointF a = line[i - 1], b = line[i];
        if (b.x() < left || a.x() > right)
            continue;
        const auto yAt = [&](double x) {
            return b.x() == a.x() ? a.y() : a.y() + (b.y() - a.y()) * (x - a.x()) / (b.x() - a.x());
        };
        const double y0 = yAt(std::max(a.x(), left)), y1 = yAt(std::min(b.x(), right));
        if (std::max(y0, y1) >= top && std::min(y0, y1) <= bottom)
            return true;
    }
    return false;
}

// How far `text` reaches from where it starts: its advance, or its last glyph's ink past it.
double extentOf(const QString& text, const QFontMetricsF& metrics) {
    return std::max(metrics.horizontalAdvance(text), metrics.boundingRect(text).right());
}
double extentOf(const QString& text, const QFont& font) { return extentOf(text, QFontMetricsF(font)); }

// The readout's texts: a shelf's ("Hi 4.50 kHz · 70 %"), the Low-pass's, the decay time's.
QString shelfText(const QString& name, double hz, double percent) {
    return QStringLiteral("%1 %2 · %3")
        .arg(name, sub::app::formatValue(hz, QStringLiteral("Hz")),
             sub::app::formatValue(percent, QStringLiteral("%")));
}
QString lowpassText(double hz) {
    return QStringLiteral("Low-pass %1").arg(sub::app::formatValue(hz, QStringLiteral("Hz")));
}
QString decayText(double ms) { return sub::app::formatValue(ms, QStringLiteral("ms")); }

// Of the values from `from` to `to` (log), the one whose text is widest.
template <typename Text>
double widestOver(double from, double to, Text text, const QFontMetricsF& metrics) {
    constexpr int kSteps = 1000;
    double widest = from, most = -1.0;
    for (int i = 0; i <= kSteps; ++i) {
        const double v = from * std::pow(to / from, double(i) / kSteps);
        if (const double extent = extentOf(text(v), metrics); extent > most) {
            most = extent;
            widest = v;
        }
    }
    return widest;
}

QString captionText() { return QStringLiteral("Decay time"); }

}  // namespace

QFont ReverbDecayGraph::captionFont() { return uiFont(7); }

ReverbDecayGraph::ReverbDecayGraph(QQuickItem* parent) : DeviceCanvas(parent) {
    // As wide as the captions need: the caption and the widest readout (each value at its widest over its
    // parameter's range), apart and in from the plot's sides. (The font is the process's: worked out once.)
    static const double kCaptions = [] {
        const QFontMetricsF metrics(captionFont());
        const auto percent = [](double v) { return sub::app::formatValue(v, QStringLiteral("%")); };
        const double gain = widestOver(kMinShelfGain, kMaxShelfGain, percent, metrics);
        const auto shelf = [gain](const QString& name) {
            return [name, gain](double hz) { return shelfText(name, hz, gain); };
        };
        const double lo = widestOver(kMinShelfFreq, kMaxLoFreq, shelf(QStringLiteral("Lo")), metrics);
        const double hi = widestOver(kMinShelfFreq, kMaxHiFreq, shelf(QStringLiteral("Hi")), metrics);
        const QStringList readouts = {shelfText(QStringLiteral("Lo"), lo, gain),
                                      shelfText(QStringLiteral("Hi"), hi, gain),
                                      lowpassText(widestOver(kMinShelfFreq, kMaxHiFreq, lowpassText, metrics)),
                                      decayText(widestOver(kMinDecayMs, kMaxDecayMs, decayText, metrics)),
                                      QStringLiteral("Frozen"),
                                      QStringLiteral("Lo off"),
                                      QStringLiteral("Hi off")};
        double widest = 0.0;
        for (const QString& text : readouts) widest = std::max(widest, std::ceil(extentOf(text, metrics)));
        return 2 * kCaptionInset + std::ceil(extentOf(captionText(), metrics)) + kCaptionGap + widest;
    }();
    // (The plot is the item less 1 px either side and the meter, 2 px from it, at the right.)
    setImplicitSize(std::max(double(kMinimumWidth), std::ceil(kCaptions) + 1 + kMeterWidth + 2 + 1), kMinimumHeight);
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

QRectF ReverbDecayGraph::captionRect() const {
    const QRectF r = plot();
    return {r.left() + kCaptionInset, r.top(), std::ceil(extentOf(captionText(), captionFont())), kHeader};
}

QRectF ReverbDecayGraph::readoutRect() const {
    const QRectF r = plot();
    const double right = r.right() - kCaptionInset, room = std::max(0.0, right - captionRect().right() - kCaptionGap);
    const double width = std::min(std::ceil(extentOf(readout(), captionFont())), room);
    return {right - width, r.top(), width, kHeader};
}

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
    switch (shown) {
    case Lo:
        return settings_.loShelf ? shelfText(QStringLiteral("Lo"), settings_.loFreq, settings_.loGain)
                                 : QStringLiteral("Lo off");
    case Hi:
        if (!settings_.hiFilter)
            return QStringLiteral("Hi off");
        return settings_.hiLowpass ? lowpassText(settings_.hiFreq)
                                   : shelfText(QStringLiteral("Hi"), settings_.hiFreq, settings_.hiGain);
    case Decay:
    case None: break;
    }
    if (settings_.freeze && shown == None)
        return QStringLiteral("Frozen");
    return decayText(settings_.decayMs);
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
    const double seconds = tickSeconds();
    const std::vector<float> tail = readDisplay(QStringLiteral("tail"));
    if (spectrum_.add(tail.data(), tail.size(), sampleRate())) {
        updateColumns();
        spectrumChanged_ = true;
    }
    // The loudest of the tick's newest values (shown again after the sound stopped, a read holds the loud past:
    // DeviceCanvas::readRecent). A tick that brings none (the audio's blocks longer than a tick) keeps the last
    // for a moment: only a while without any is silence (asleep, the engine publishes the floor).
    const std::vector<float> diffuse = readRecent(QStringLiteral("diffuse"), kStaleSeconds);
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
            setParams({{QStringLiteral("lo_freq"), std::clamp(startFreq_ * rx, kMinShelfFreq, kMaxLoFreq)}}, gesture_,
                      text);
        else
            setParams({{QStringLiteral("lo_freq"), std::clamp(startFreq_ * rx, kMinShelfFreq, kMaxLoFreq)},
                       {QStringLiteral("lo_gain"), std::clamp(startGain_ * ry, kMinShelfGain, kMaxShelfGain)}},
                      gesture_, text);
        break;
    case Hi:
        if (gainless(Hi))
            setParams({{QStringLiteral("hi_freq"), std::clamp(startFreq_ * rx, kMinShelfFreq, kMaxHiFreq)}}, gesture_,
                      text);
        else
            setParams({{QStringLiteral("hi_freq"), std::clamp(startFreq_ * rx, kMinShelfFreq, kMaxHiFreq)},
                       {QStringLiteral("hi_gain"), std::clamp(startGain_ * ry, kMinShelfGain, kMaxShelfGain)}},
                      gesture_, text);
        break;
    case Decay:
        setParams({{QStringLiteral("decay"), std::clamp(startDecay_ * ry, kMinDecayMs, kMaxDecayMs)}}, gesture_,
                  text);
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
    const QFont font = captionFont();
    p.fillRect(QRectF(0, 0, width(), height()), Theme::meterBg());

    // The curve and the shelves' guides, worked out first: the axis' figures keep clear of them (of where the
    // curve rests: its ripple would flip a figure to and fro). Frozen, the curve no longer settles onto the
    // guides (the handles are what thaws): they fade, and the handles dim.
    const double frozen = frozen_.value;
    const int guide = int(90 * (1.0 - frozen));
    std::vector<QPointF> curve;
    curve.reserve(shown_.size());
    for (std::size_t i = 0; i < shown_.size() && i < frequencies_.size(); ++i)
        curve.emplace_back(xOf(frequencies_[i]), yOfLog(shown_[i]));
    std::vector<std::vector<QPointF>> guides;
    if (settings_.loShelf && guide > 0)
        guides.push_back({QPointF(r.left(), handleAt(Lo).y()), handleAt(Lo)});
    if (settings_.hiFilter && !settings_.hiLowpass && guide > 0)
        guides.push_back({handleAt(Hi), QPointF(r.right(), handleAt(Hi).y())});
    const auto crosses = [&](const QRectF& ink) {
        return passesNear(curve, ink) ||
               std::any_of(guides.begin(), guides.end(), [&](const auto& line) { return passesNear(line, ink); });
    };

    // The decay axis: a line per decade, its figure under it. Where the curve or a guide runs through a figure,
    // the figure goes over its line if there is room (in the axis, clear of them and of the figure above), else
    // over them, on a chip of the background.
    p.save();
    p.setClipRect(r);
    drawDecadeGrid(p, r, LogAxis{kLow, kHigh, r.left(), r.width()});
    const QFontMetricsF metrics(font);
    constexpr std::array<double, 3> kDecades = {0.1, 1.0, 10.0};
    const auto underBox = [&](double seconds) { return QRectF(r.left() + 3, yOf(seconds) + 1, 40, 11); };
    std::vector<std::pair<QRectF, QString>> chipped;
    for (std::size_t k = 0; k < kDecades.size(); ++k) {
        const double y = yOf(kDecades[k]);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y), withAlpha(Theme::gridBeat(), 120));
        const QString text = axisText(kDecades[k]);
        QRectF box = underBox(kDecades[k]);
        if (crosses(inkOf(box, text, metrics))) {
            const QRectF over(box.left(), y - 12, 40, 11), ink = inkOf(over, text, metrics);
            double top = axis.top() + 1;
            if (k + 1 < kDecades.size())  // (the next decade's figure hangs under its line, up there)
                top = std::max(top, inkOf(underBox(kDecades[k + 1]), axisText(kDecades[k + 1]), metrics).bottom() + 8);
            if (ink.top() < top || crosses(ink)) {
                chipped.emplace_back(box, text);
                continue;
            }
            box = over;
        }
        p.drawText(box, Qt::AlignLeft | Qt::AlignVCenter, text, Theme::textDisabled(), font);
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
        gradient.setColorAt(0, withAlpha(Theme::scopeLine(), 70));
        gradient.setColorAt(1, withAlpha(Theme::scopeLine(), 0));
        p.fillToBaseline(shape.data(), int(shape.size()), r.bottom(), gradient);
    }

    // Frozen: the plot takes the frozen tint.
    if (frozen > 0.0)
        p.fillRect(r, withAlpha(Theme::frozen(), int(34 * frozen)));

    // The curve: filled, glowing while the tail sounds, rippling with the chorus.
    const double glow = glow_.value;
    const QColor color = dry_ ? Theme::textDisabled() : mixColor(Theme::scopeLine(), Theme::frozen(), frozen);
    const double ripple = kRipplePx * depth_.value * glow;
    if (ripple > 0.0) {
        for (std::size_t i = 0; i < curve.size(); ++i) {
            const double along = 3.0 * double(i) / double(shown_.size());  // three ripples across
            curve[i].ry() += ripple * std::sin(2.0 * kPi * (chorusPhase_ + along));
        }
    }
    if (curve.size() >= 2) {
        p.fillToBaseline(curve.data(), int(curve.size()), r.bottom(), withAlpha(color, int(22 + 40 * glow)));
        drawGlowPolyline(p, curve, color, 1.5 + 1.0 * glow);
    }

    // The shelves' guides: how long their bands ring, level out to their edges.
    const QColor accent = dry_ ? Theme::textDisabled() : Theme::accent();
    for (const std::vector<QPointF>& line : guides)
        drawDashedPolyline(p, line, withAlpha(accent, guide), 1.0);

    for (const auto& [box, text] : chipped) {
        p.fillRoundedRect(inkOf(box, text, metrics).adjusted(-2, -2, 2, 2), 2, 2, withAlpha(Theme::meterBg(), 220));
        p.drawText(box, Qt::AlignLeft | Qt::AlignVCenter, text, Theme::textDisabled(), font);
    }
    p.restore();

    // The captions: what it shows, and the handle under the mouse (else Decay, or Frozen).
    p.drawText(captionRect(), Qt::AlignLeft | Qt::AlignVCenter, captionText(), Theme::textDim(), font);
    const bool lit = pressed_ != None || hovered_ != None;
    p.drawText(readoutRect(), Qt::AlignRight | Qt::AlignVCenter, readout(),
               lit ? Theme::text() : frozen > 0.5 ? Theme::frozen() : Theme::textDim(), font);

    // The handles: rings, growing under the mouse, filled while held; a switched-off shelf's hollow and dim;
    // all of them dimmed while frozen (unless held or hovered).
    for (const Handle handle : {Lo, Hi, Decay}) {
        const QPointF at = handleAt(handle);
        const double grown = grow_[std::size_t(handle)].value;
        const double radius = 4.0 + 2.0 * grown;
        const bool on = shelfOn(handle);
        QColor ring = on ? accent : Theme::textDim();
        ring.setAlphaF(float(ring.alphaF() * (1.0 - 0.6 * frozen * (1.0 - grown))));
        p.fillEllipse(at, radius, radius, handle == pressed_ && on ? ring : Theme::meterBg());
        p.drawEllipse(QRectF(at.x() - radius, at.y() - radius, 2 * radius, 2 * radius), ring, on ? 2.0 : 1.2);
        if (handle == Decay && pressed_ != Decay)
            p.fillEllipse(at, 1.5, 1.5, ring);
    }

    // The tail's meter.
    drawLevelMeter(p, meterRect(), meter_.level, meter_.peak, kMeterFloorDb, 0.0);
}

}  // namespace sub::ui
