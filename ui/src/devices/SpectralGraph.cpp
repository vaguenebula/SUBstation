#include "devices/SpectralGraph.h"

#include "audio/BridgeTypes.h"
#include "audio/EngineBridge.h"
#include "audio/SpectralResponse.h"
#include "input/GestureKey.h"
#include "model/Device.h"
#include "model/ParamSpec.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QFontMetricsF>
#include <QHoverEvent>
#include <QLineF>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace sub::ui {

namespace {

// The threshold's pivot (the engine's: the tilt turns about it).
constexpr double kPivotHz = sub::app::kSpectralPivotHz;
constexpr double kSpectrumMarginDb = 6.0;    // spectra are held this far beyond the axis (no time spent off it)
constexpr double kStaleSeconds = 0.3;        // no frame for this long: the displays sink back
constexpr double kRecentSeconds = 0.1;       // a tick reads the meters' newest values back this far (a long block)
constexpr double kHoldSeconds = 0.8;         // the deepest cut's line holds, then falls
constexpr double kHoldFallDbPerSecond = 18.0;
constexpr double kHeldDrawnDb = 0.01;        // a held cut is drawn from this deep
constexpr double kDeltaFade = 0.7;           // with Delta on, what the output line now shows fades this far
constexpr int kFocusDimAlpha = 170;          // outside the Focus band (a weight of 0), dimmed this much

QString id(const char* text) { return QString::fromLatin1(text); }

// How much the Focus dim darkens where the band gives a frequency `weight` of its gain (0: not at all).
int focusDimAlpha(double weight) { return int(std::lround(kFocusDimAlpha * (1.0 - weight))); }

// The highest finite value (`otherwise` without one).
double highest(const std::vector<float>& values, double otherwise) {
    double most = -std::numeric_limits<double>::infinity();
    for (const float value : values)
        if (std::isfinite(value))
            most = std::max(most, double(value));
    return std::isfinite(most) ? most : otherwise;
}

// Moves each shown value `up` or `down` of the way to its target (a one-pole each way), snapping within `epsilon`,
// or once both are under `hidden` (off the axis, where the move can't be seen); whether any moved.
bool easeAll(std::vector<double>& shown, const std::vector<double>& target, double up, double down, double epsilon,
             double hidden = -std::numeric_limits<double>::infinity()) {
    bool moving = false;
    for (std::size_t j = 0; j < shown.size(); ++j) {
        double& s = shown[j];
        const double t = target[j];
        if (s == t)
            continue;
        s += (t - s) * (t > s ? up : down);
        if (std::abs(t - s) <= epsilon || (s <= hidden && t <= hidden))
            s = t;
        moving = true;
    }
    return moving;
}

// How clear of `line` (one of the thresholds drawn) a box is, 0..1: 0 where the line or the bright part of its glow
// runs through it, 1 from a few pixels off, smoothly between.
double clearOf(const QLineF& line, const QRectF& box) {
    if (line.dx() <= 0.0)
        return 1.0;
    const auto yAt = [&](double x) { return line.y1() + line.dy() * (x - line.x1()) / line.dx(); };
    const double a = yAt(box.left()), b = yAt(box.right());
    const double gap = std::max({0.0, std::min(a, b) - box.bottom(), box.top() - std::max(a, b)});
    return smoothstep((gap - 0.75) / 2.75);
}

// Calls `draw` with each run of `points` where `present(j)` (taking in its neighbours either side): a line lying
// along the floor or the ceiling where there is nothing would read as a line of its own.
template <typename Present, typename Draw>
void eachRun(const std::vector<QPointF>& points, Present present, Draw draw) {
    std::vector<QPointF> run;
    const auto there = [&](std::size_t k) { return k < points.size() && present(k); };
    for (std::size_t j = 0; j <= points.size(); ++j) {
        if (j < points.size() && (there(j) || (j > 0 && there(j - 1)) || there(j + 1))) {
            run.push_back(points[j]);
            continue;
        }
        if (run.size() >= 2)
            draw(run);
        run.clear();
    }
}

}  // namespace

// --- Frames ---------------------------------------------------------------------------------------

void SpectralGraph::FrameAssembler::add(qint64 first, const std::vector<float>& values) {
    for (std::size_t i = 0; i < values.size(); ++i) {
        const qint64 index = first + qint64(i);
        if (index != next)
            filled = -1;  // a gap: the frame it was in is lost
        next = index + 1;
        const int band = int(index % kPoints);
        if (band == 0)
            filled = 0;
        if (filled != band)
            continue;
        float value = values[i];
        if (!std::isfinite(value))
            value = merge == Merge::Gain ? 0.f : float(kFloorDb - kSpectrumMarginDb);  // (none: under the axis)
        building[std::size_t(band)] = value;
        if (++filled < kPoints)
            continue;
        latest = building;
        if (!fresh) {
            merged = building;
        } else if (merge == Merge::Highest) {
            for (std::size_t j = 0; j < merged.size(); ++j) merged[j] = std::max(merged[j], building[j]);
        } else {  // the deeper cut, else the bigger lift
            for (std::size_t j = 0; j < merged.size(); ++j) {
                const float lower = std::min(merged[j], building[j]);
                merged[j] = lower < 0.f ? lower : std::max(merged[j], building[j]);
            }
        }
        ++frames;
        fresh = true;
        filled = -1;
    }
}

// --- The item ---------------------------------------------------------------------------------------

SpectralGraph::SpectralGraph(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(kWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    gain_.merge = FrameAssembler::Merge::Gain;
    frequencies_ = sub::app::spectralDisplayFrequencies();
    for (int i = 0; i < kFocusStops; ++i)
        focusFrequencies_.append(kLow * std::pow(kHigh / kLow, i / (kFocusStops - 1.0)));
    const double floor = kFloorDb - kSpectrumMarginDb;
    for (std::vector<double>* spectrum : {&targetInput_, &targetKey_, &targetOutput_, &shownInput_, &shownKey_,
                                          &shownOutput_})
        spectrum->assign(kPoints, floor);
    for (std::vector<double>* gains : {&targetGain_, &shownGain_, &heldCut_, &heldFor_}) gains->assign(kPoints, 0.0);
    meterIn_.reset(kMeterFloorDb);
    meterOut_.reset(kMeterFloorDb);
    thresholdShown_.snap(threshold_);
    belowShown_.snap(below_);
    tiltShown_.snap(tilt_);
    focusLowShown_.snap(std::log2(focusLow_));
    focusHighShown_.snap(std::log2(focusHigh_));
    updateLines();
}

QRectF SpectralGraph::plot() const { return QRectF(1, 15, width() - 24, height() - 27); }

LogAxis SpectralGraph::frequencyAxis() const {
    const QRectF r = plot();
    return {kLow, kHigh, r.left(), r.width()};
}

double SpectralGraph::xOf(double hz) const { return frequencyAxis().position(hz); }

double SpectralGraph::freqAt(double x) const { return frequencyAxis().valueAt(x); }

double SpectralGraph::yOfLevel(double db) const { return dbToY(db, plot(), kFloorDb, kCeilingDb); }

double SpectralGraph::dbPerPixel() const { return (kCeilingDb - kFloorDb) / std::max(1.0, plot().height()); }

double SpectralGraph::thresholdAt(double hz) const {
    return sub::app::spectralThresholdDb(thresholdShown_.value, tiltShown_.value, {hz}).value(0);
}

double SpectralGraph::belowAt(double hz) const {
    return sub::app::spectralBelowDb(thresholdShown_.value, belowShown_.value, tiltShown_.value, {hz}).value(0);
}

double SpectralGraph::lineY(double db) const {
    const QRectF r = plot();
    return r.bottom() - (db - kFloorDb) / (kCeilingDb - kFloorDb) * r.height();
}

// Each line is straight on these axes (dB against log frequency), so its two ends draw it; mapped as they are, not
// held to the axis, so a line steep enough to leave the plot is drawn where it is up to the edge it leaves by.
QLineF SpectralGraph::thresholdLine() const {
    const QRectF r = plot();
    return {r.left(), lineY(thresholdAt(kLow)), r.right(), lineY(thresholdAt(kHigh))};
}

QLineF SpectralGraph::belowLine() const {
    const QRectF r = plot();
    return {r.left(), lineY(belowAt(kLow)), r.right(), lineY(belowAt(kHigh))};
}

QString SpectralGraph::levelFigure(double db) { return db == 0.0 ? id("0") : QStringLiteral("−%1").arg(-db); }

// A figure sits just over its line; one a threshold runs through fades out (struck through, it would read as that
// threshold's level), back in as the line moves off it.
double SpectralGraph::figureShown(double db) const {
    const QFont font = uiFont(7);
    const QFontMetricsF metrics(font);
    const double baseline = yOfLevel(db) - 1 - metrics.descent();  // (drawn bottom-aligned 1 px over the line)
    const QRectF glyphs(plot().left() + 9, baseline - metrics.capHeight(),
                        SgPainter::textWidth(levelFigure(db), font), metrics.capHeight());
    return std::min(clearOf(thresholdLine(), glyphs),
                    1.0 - belowOpacity_.value * (1.0 - clearOf(belowLine(), glyphs)));
}

QPointF SpectralGraph::onLine(const QLineF& line, double hz) const {
    const QRectF r = plot();
    const double x = xOf(hz);
    if (line.dx() <= 0.0 || r.height() <= 0.0)  // (not laid out yet)
        return {x, line.y1()};
    const double slope = line.dy() / line.dx();
    const double y = line.y1() + slope * (x - line.x1());
    if (r.top() <= y && y <= r.bottom())
        return {x, y};
    // Off the plot here: where the line leaves it, between here and 1 kHz (both lines' levels there are within the
    // axis whatever the parameters, so a handle held this way stays on its line, inside the plot).
    const double edge = y < r.top() ? r.top() : r.bottom();
    const double pivot = xOf(kPivotHz);
    const double across = slope != 0.0 ? line.x1() + (edge - line.y1()) / slope : x;
    return {std::clamp(across, std::min(x, pivot), std::max(x, pivot)), edge};
}

double SpectralGraph::focusLowShown() const { return std::exp2(focusLowShown_.value); }

double SpectralGraph::focusHighShown() const { return std::exp2(focusHighShown_.value); }

double SpectralGraph::edgeX(int which) const {
    const QRectF r = plot();
    if (which == FocusLowEdge) {
        const double low = focusLowShown();
        return low <= kLow * 1.001 ? r.left() + 3.0 : std::max(xOf(low), r.left() + 3.0);
    }
    const double high = focusHighShown();
    return high >= kHigh * 0.999 ? r.right() - 3.0 : std::min(xOf(high), r.right() - 3.0);
}

QPointF SpectralGraph::handle(int which) const {
    switch (which) {
    case ThresholdHandle:
        return onLine(thresholdLine(), kPivotHz);
    case TiltLow:
        return onLine(thresholdLine(), kLowHandleHz);
    case TiltHigh:
        return onLine(thresholdLine(), kHighHandleHz);
    case BelowHandle:
        return onLine(belowLine(), kBelowHandleHz);
    case FocusLowEdge:
    case FocusHighEdge:
        return {edgeX(which), plot().bottom() - 6.0};
    default:
        return {};
    }
}

QString SpectralGraph::paramOf(int which) const {
    switch (which) {
    case ThresholdHandle:
        return id("threshold");
    case TiltLow:
    case TiltHigh:
        return id("tilt");
    case BelowHandle:
        return id("below");
    case FocusLowEdge:
        return id("focus_lo");
    case FocusHighEdge:
        return id("focus_hi");
    default:
        return {};
    }
}

// --- Parameters -------------------------------------------------------------------------------------

void SpectralGraph::sync() {
    threshold_ = value(id("threshold"));
    ratio_ = value(id("ratio"));
    below_ = value(id("below"));
    upward_ = value(id("upward"));
    tilt_ = value(id("tilt"));
    range_ = value(id("range"));
    focusLow_ = std::clamp(value(id("focus_lo")), kLow, kHigh);
    focusHigh_ = std::clamp(value(id("focus_hi")), kLow, kHigh);
    mix_ = value(id("mix"));
    delta_ = value(id("delta")) >= 0.5;
    const sub::app::Device* found = device();
    if (found && !spansRead_)
        readSpans();
    const bool keyed = found && found->sidechain.has_value();
    if (keyed != keyed_) {
        keyed_ = keyed;
        Q_EMIT keyedChanged();
    }
    active_ = range_ > 0.0 && mix_ > 0.0 && (ratio_ > 1.0001 || upward_ > 1.0001);

    thresholdShown_.target = threshold_;
    belowShown_.target = below_;
    tiltShown_.target = tilt_;
    focusLowShown_.target = std::log2(focusLow_);
    focusHighShown_.target = std::log2(focusHigh_);
    belowOpacity_.target = belowShown() ? 1.0 : 0.0;
    deltaShown_.target = delta_ ? 1.0 : 0.0;
    // The glow says "turned down here": only while anything over the threshold is (not at 1:1, Range 0 or Dry/Wet 0).
    hotShown_.target = range_ > 0.0 && mix_ > 0.0 && ratio_ > 1.0001 ? 1.0 : 0.0;
    if (!primed_ && found) {  // the first time: drawn where it is, not easing in from the defaults
        primed_ = true;
        for (Eased* eased : {&thresholdShown_, &belowShown_, &tiltShown_, &focusLowShown_, &focusHighShown_,
                             &belowOpacity_, &deltaShown_, &hotShown_})
            eased->snap(eased->target);
    }
    // What is dragged follows the mouse at once.
    switch (dragged_) {
    case ThresholdHandle:
        thresholdShown_.snap(threshold_);
        break;
    case TiltLow:
    case TiltHigh:
        tiltShown_.snap(tilt_);
        break;
    case BelowHandle:
        belowShown_.snap(below_);
        break;
    case FocusLowEdge:
        focusLowShown_.snap(focusLowShown_.target);
        break;
    case FocusHighEdge:
        focusHighShown_.snap(focusHighShown_.target);
        break;
    default:
        break;
    }
    updateLines();
    updateReadout();
    update();
}

void SpectralGraph::readSpans() {
    for (const sub::app::ProcessorParam& param : session()->bridge()->deviceParams(trackId(), deviceId())) {
        const Span span{param.minValue, param.maxValue};
        if (param.id == u"threshold")
            thresholdSpan_ = span;
        else if (param.id == u"below")
            belowSpan_ = span;
        else if (param.id == u"tilt")
            tiltSpan_ = span;
        else if (param.id == u"range")
            rangeSpan_ = span;
        else
            continue;
        spansRead_ = true;
    }
}

void SpectralGraph::updateLines() {
    const QList<double> curve = sub::app::spectralThresholdDb(thresholdShown_.value, tiltShown_.value, frequencies_);
    thresholdCurve_.assign(curve.begin(), curve.end());
    focusWeights_ = sub::app::spectralFocusWeights(focusLowShown(), focusHighShown(), focusFrequencies_);
    // Drawn where any of it shows (at the default band the edges' round trip through log2 leaves a weight a hair
    // under 1 at 20 kHz, which darkens nothing).
    focusDimmed_ =
        std::any_of(focusWeights_.begin(), focusWeights_.end(), [](double w) { return focusDimAlpha(w) > 0; });
}

void SpectralGraph::updateReadout() {
    QString text = readoutText_;  // (kept while it fades out)
    switch (hoveredHandle()) {
    case ThresholdHandle:
        text = QStringLiteral("Threshold ") + sub::app::formatValue(thresholdShown_.value, id("dB"));
        break;
    case TiltLow:
    case TiltHigh:
        text = QStringLiteral("Tilt ") + sub::app::formatValue(tiltShown_.value, id("dB/oct"));
        break;
    case BelowHandle:
        text = QStringLiteral("Below ") +
               sub::app::formatValue(std::min(belowShown_.value, thresholdShown_.value), id("dB"));
        break;
    case FocusLowEdge:
        text = QStringLiteral("Focus Low ") + sub::app::formatValue(std::round(focusLowShown()), id("Hz"));
        break;
    case FocusHighEdge:
        text = QStringLiteral("Focus High ") + sub::app::formatValue(std::round(focusHighShown()), id("Hz"));
        break;
    default:
        break;
    }
    if (text != readoutText_) {
        readoutText_ = text;
        Q_EMIT hoverChanged();
    }
}

// --- Displays ---------------------------------------------------------------------------------------

void SpectralGraph::refreshDisplays() {
    const double dt = tickSeconds();

    for (const auto& [assembler, display] : {std::pair{&input_, "input"}, std::pair{&key_, "key"},
                                             std::pair{&output_, "output"}, std::pair{&gain_, "gain"}}) {
        const auto [first, values] = readDisplayAt(id(display));
        assembler->add(first, values);
    }
    // The meters' newest values (shown again after the sound stopped, a read holds the loud past).
    const std::vector<float> levelsIn = readRecent(id("in_level"), kRecentSeconds);
    const std::vector<float> levelsOut = readRecent(id("out_level"), kRecentSeconds);

    // The targets: the frames that came since the last tick (merged); none for a while, the floor.
    const bool fresh = input_.fresh || key_.fresh || output_.fresh || gain_.fresh;
    sinceFrame_ = fresh ? 0.0 : sinceFrame_ + dt;
    const double floor = kFloorDb - kSpectrumMarginDb, ceiling = kCeilingDb + kSpectrumMarginDb;
    auto take = [](FrameAssembler& frames, std::vector<double>& target, double low, double high) {
        if (!frames.fresh)
            return;
        for (std::size_t j = 0; j < target.size(); ++j) target[j] = std::clamp(double(frames.merged[j]), low, high);
        frames.take();
    };
    take(input_, targetInput_, floor, ceiling);
    take(key_, targetKey_, floor, ceiling);
    take(output_, targetOutput_, floor, ceiling);
    take(gain_, targetGain_, -rangeSpan_.high, rangeSpan_.high);  // (the engine's are within Range: held there)
    if (sinceFrame_ > kStaleSeconds) {
        for (std::vector<double>* spectrum : {&targetInput_, &targetKey_, &targetOutput_})
            std::fill(spectrum->begin(), spectrum->end(), floor);
        std::fill(targetGain_.begin(), targetGain_.end(), 0.0);
    }

    // Spectra rise at once and fall back slowly; the gains flow.
    bool moved = false;
    const double rise = easeFraction(dt, 0.015), fall = easeFraction(dt, 0.25), flow = easeFraction(dt, 0.045);
    moved |= easeAll(shownInput_, targetInput_, rise, fall, 0.01, kFloorDb);
    moved |= easeAll(shownKey_, targetKey_, rise, fall, 0.01, kFloorDb);
    moved |= easeAll(shownOutput_, targetOutput_, rise, fall, 0.01, kFloorDb);
    moved |= easeAll(shownGain_, targetGain_, flow, flow, 0.002);

    // The deepest recent cut per point holds, then falls (drawn for as long as it is there, after the cut itself
    // has let go).
    double mostCut = 0.0, mostLift = 0.0;
    bool held = false;
    for (std::size_t j = 0; j < shownGain_.size(); ++j) {
        const double cut = std::max(0.0, -shownGain_[j]);
        mostCut = std::max(mostCut, cut);
        mostLift = std::max(mostLift, shownGain_[j]);
        if (cut >= heldCut_[j]) {
            moved |= heldCut_[j] != cut;
            heldCut_[j] = cut;
            heldFor_[j] = 0.0;
        } else {
            heldFor_[j] += dt;
            if (heldFor_[j] > kHoldSeconds) {
                heldCut_[j] = std::max(cut, heldCut_[j] - kHoldFallDbPerSecond * dt);
                moved = true;
            }
        }
        held = held || heldCut_[j] > kHeldDrawnDb;
    }
    moved |= held != anyHeld_;
    anyHeld_ = held;
    maxCut_.target = mostCut;
    maxLift_.target = mostLift;
    const double levels = easeFraction(dt, 0.08);
    bool readings = maxCut_.step(levels, 1e-3);
    readings = maxLift_.step(levels, 1e-3) || readings;

    // The meters: the loudest of the newest hops' peaks since they last had any; none for a while, they fall.
    meterDt_ += dt;
    sinceLevel_ = levelsIn.empty() && levelsOut.empty() ? sinceLevel_ + dt : 0.0;
    if (!levelsIn.empty() || !levelsOut.empty() || sinceLevel_ > kStaleSeconds) {
        const MeterBallistics in = meterIn_, out = meterOut_;
        meterIn_.update(highest(levelsIn, kMeterFloorDb), meterDt_, 24.0, 1.0, kMeterFloorDb);
        meterOut_.update(highest(levelsOut, kMeterFloorDb), meterDt_, 24.0, 1.0, kMeterFloorDb);
        meterDt_ = 0.0;
        moved |= in.level != meterIn_.level || in.peak != meterIn_.peak || out.level != meterOut_.level ||
                 out.peak != meterOut_.peak;
    }

    // The lines and edges glide to automated or undone values; Below fades in and out; Delta tints the output.
    const double glide = easeFraction(dt, 0.06);
    bool lines = thresholdShown_.step(glide, 1e-3);
    lines |= belowShown_.step(glide, 1e-3);
    lines |= tiltShown_.step(glide, 1e-4);
    lines |= focusLowShown_.step(glide, 1e-4);
    lines |= focusHighShown_.step(glide, 1e-4);
    if (lines) {
        updateLines();
        updateReadout();
    }
    const double fade = easeFraction(dt, 0.08);
    moved |= lines;
    moved |= belowOpacity_.step(fade, 1e-3);
    moved |= deltaShown_.step(fade, 1e-3);
    moved |= hotShown_.step(fade, 1e-3);

    // The handle under the mouse grows; the header's readout fades in and out.
    const int lit = hoveredHandle();
    for (int h = ThresholdHandle; h <= FocusHighEdge; ++h) {
        Eased& grow = handleGrow_[std::size_t(h)];
        grow.target = h == lit ? 1.0 : 0.0;
        moved |= grow.step(easeFraction(dt, 0.04), 1e-3);
    }
    readoutOpacity_.target = lit != None ? 1.0 : 0.0;
    moved |= readoutOpacity_.step(easeFraction(dt, readoutOpacity_.target > readoutOpacity_.value ? 0.04 : 0.15),
                                  1e-3);

    if (readings)
        Q_EMIT levelsChanged();
    if (moved || readings)
        update();
}

// --- The mouse --------------------------------------------------------------------------------------

int SpectralGraph::hit(const QPointF& pos) const {
    const QRectF r = plot();
    if (!r.adjusted(-kHandleHit, -kHandleHit, kHandleHit, kHandleHit).contains(pos))
        return None;
    // A handle: the nearest (on a tie, the one drawn on top: the pivot, the tilt handles, Below's).
    int found = None;
    double nearest = std::numeric_limits<double>::infinity();
    for (const int which : {int(ThresholdHandle), int(TiltLow), int(TiltHigh), int(BelowHandle)}) {
        if (which == BelowHandle && !belowShown())
            continue;
        const double distance = QLineF(handle(which), pos).length();
        if (distance < nearest) {
            nearest = distance;
            found = which;
        }
    }
    if (nearest <= kHandleHit)
        return found;
    // A line, where it is drawn (inside the plot): the nearer (the threshold on a tie).
    if (r.left() <= pos.x() && pos.x() <= r.right()) {
        const auto away = [&](const QLineF& line) {
            const double y = line.y1() + line.dy() * (pos.x() - line.x1()) / std::max(1e-9, line.dx());
            return r.top() <= y && y <= r.bottom() ? std::abs(pos.y() - y) : std::numeric_limits<double>::infinity();
        };
        const double threshold = away(thresholdLine());
        const double below = belowShown() ? away(belowLine()) : std::numeric_limits<double>::infinity();
        if (std::min(threshold, below) <= kLineHit)
            return threshold <= below ? ThresholdHandle : BelowHandle;
    }
    // A Focus edge, anywhere over the plot.
    if (r.top() <= pos.y() && pos.y() <= r.bottom()) {
        const double low = std::abs(pos.x() - edgeX(FocusLowEdge)), high = std::abs(pos.x() - edgeX(FocusHighEdge));
        if (std::min(low, high) <= kEdgeHit)
            return low <= high ? FocusLowEdge : FocusHighEdge;
    }
    return None;
}

void SpectralGraph::setHovered(int which) {
    if (which == ThresholdHandle || which == TiltLow || which == TiltHigh || which == BelowHandle)
        setCursor(Qt::SizeVerCursor);
    else if (which == FocusLowEdge || which == FocusHighEdge)
        setCursor(Qt::SizeHorCursor);
    else
        unsetCursor();
    if (which == hovered_)
        return;
    hovered_ = which;
    updateReadout();
    Q_EMIT hoverChanged();
    update();
}

void SpectralGraph::hoverMoveEvent(QHoverEvent* event) {
    if (dragged_ == None)
        setHovered(hit(event->position()));
}

void SpectralGraph::hoverLeaveEvent(QHoverEvent*) {
    if (dragged_ == None)
        setHovered(None);
}

void SpectralGraph::mousePressEvent(QMouseEvent* event) {
    const bool second = secondPressOfDoubleClick(event);
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    const int which = hit(event->position());
    if (which == None) {
        event->ignore();  // (the frame selects the device)
        return;
    }
    if (second)
        return;  // (the double-click follows)
    gesture_ = newGestureKey();
    dragged_ = which;
    lastPos_ = event->position();
    switch (which) {
    case ThresholdHandle:
        dragValue_ = threshold_;
        break;
    case TiltLow:
    case TiltHigh:
        dragValue_ = tilt_;
        break;
    case BelowHandle:
        dragValue_ = std::min(below_, threshold_);  // as drawn: a Below held at the threshold moves at once
        break;
    case FocusLowEdge:
        dragValue_ = focusLow_;
        break;
    default:
        dragValue_ = focusHigh_;
        break;
    }
    touch(paramOf(which));
    hovered_ = which;
    updateReadout();
    Q_EMIT hoverChanged();
    update();
}

void SpectralGraph::mouseMoveEvent(QMouseEvent* event) {
    if (dragged_ != None && !gesture_.isEmpty())
        dragTo(event->position(), event->modifiers());
}

void SpectralGraph::dragTo(const QPointF& pos, Qt::KeyboardModifiers modifiers) {
    // Each move adds its own distance (from where the mouse last was), so pressing or letting go of Shift mid-drag
    // changes the rate from there on, not the whole drag's (as the knobs do). Held at a limit, the value turns
    // back as soon as the mouse does.
    const double fine = modifiers & Qt::ShiftModifier ? kFineDrag : 1.0;
    const double up = (lastPos_.y() - pos.y()) * dbPerPixel() * fine;               // dB
    const double across = (pos.x() - lastPos_.x()) * fine / std::max(1.0, plot().width());  // of the plot
    lastPos_ = pos;
    double value = 0.0;
    switch (dragged_) {
    case ThresholdHandle:
    case BelowHandle:
        dragValue_ = (dragged_ == ThresholdHandle ? thresholdSpan_ : belowSpan_).clamp(dragValue_ + up);
        value = std::round(dragValue_ * 10.0) / 10.0;
        break;
    case TiltLow:
    case TiltHigh: {
        // That end of the line follows the mouse: the tilt changes by the move over its octaves from 1 kHz.
        const double octaves =
            sub::app::spectralThresholdDb(0.0, 1.0, {dragged_ == TiltLow ? kLowHandleHz : kHighHandleHz}).value(0);
        dragValue_ = tiltSpan_.clamp(dragValue_ + up / octaves);
        value = std::round(dragValue_ * 100.0) / 100.0;
        break;
    }
    case FocusLowEdge:
    case FocusHighEdge: {
        // Evenly in log frequency (the plot is three decades wide), a third of an octave from the other edge.
        const double apart = std::exp2(kMinFocusOctaves);
        double low = kLow, high = kHigh;
        if (dragged_ == FocusLowEdge)
            high = std::max(kLow, focusHigh_ / apart);
        else
            low = std::min(kHigh, focusLow_ * apart);
        dragValue_ = std::clamp(dragValue_ * std::pow(kHigh / kLow, across), low, high);
        value = std::clamp(std::round(dragValue_), std::ceil(low), std::floor(high));
        break;
    }
    default:
        return;
    }
    setParams({{paramOf(dragged_), value}}, gesture_);
}

void SpectralGraph::mouseReleaseEvent(QMouseEvent* event) {
    gesture_.clear();
    dragged_ = None;
    setHovered(hit(event->position()));
    Q_EMIT hoverChanged();
    update();
}

void SpectralGraph::mouseUngrabEvent() {
    gesture_.clear();
    if (dragged_ != None) {
        dragged_ = None;
        Q_EMIT hoverChanged();
        update();
    }
}

void SpectralGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }
    const int which = hit(event->position());
    if (which == None) {
        event->ignore();
        return;
    }
    gesture_.clear();
    dragged_ = None;
    QString text;
    switch (which) {
    case ThresholdHandle:
        text = QStringLiteral("Reset Threshold");
        break;
    case TiltLow:
    case TiltHigh:
        text = QStringLiteral("Reset Tilt");
        break;
    case BelowHandle:
        text = QStringLiteral("Reset Below");
        break;
    case FocusLowEdge:
        text = QStringLiteral("Reset Focus Low");
        break;
    default:
        text = QStringLiteral("Reset Focus High");
        break;
    }
    const QString param = paramOf(which);
    setParams({{param, defaultValue(param)}}, newGestureKey(), text);
}

// --- Painting ---------------------------------------------------------------------------------------

void SpectralGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const double w = width(), h = height();
    const QRectF outer = QRectF(0, 0, w, h).adjusted(0.5, 0.5, -0.5, -0.5);
    p.fillRoundedRect(outer, 4, 4, Theme::kMeterBg);
    p.drawRoundedRect(outer, 4, 4, withAlpha(Theme::kGridBeat, 150));

    const QRectF r = plot();
    const QFont font7 = uiFont(7), font8 = uiFont(8);
    const QLineF thresholdLine = this->thresholdLine(), belowLine = this->belowLine();
    const double belowOpacity = belowOpacity_.value;

    // The grid: decades across, every 12 dB up (0 dB brighter), figures under the plot (those inside it at the left
    // come after the Focus dim, over it).
    drawDecadeGrid(p, r, frequencyAxis());
    for (double db = -72.0; db <= 12.0; db += 12.0) {
        const double y = yOfLevel(db);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y), withAlpha(Theme::kGridBeat, db == 0.0 ? 170 : 110));
    }
    for (const auto& [hz, label] : {std::pair{50.0, "50"}, std::pair{100.0, "100"}, std::pair{500.0, "500"},
                                    std::pair{1000.0, "1k"}, std::pair{5000.0, "5k"}, std::pair{10000.0, "10k"}}) {
        p.drawText(QRectF(xOf(hz) - 15, r.bottom() + 1, 30, h - r.bottom() - 2), Qt::AlignCenter, id(label),
                   Theme::kTextDim, font7);
    }

    std::vector<QPointF> points(kPoints);
    std::vector<double> xs(kPoints);
    for (int j = 0; j < kPoints; ++j) xs[std::size_t(j)] = xOf(frequencies_.value(j));
    auto spectrum = [&](const std::vector<double>& levels) {
        for (std::size_t j = 0; j < points.size(); ++j) points[j] = QPointF(xs[j], yOfLevel(levels[j]));
    };
    auto depth = [&](double db) { return std::clamp(db, 0.0, kGainScaleDb) / kGainScaleDb * kCurtain * r.height(); };

    p.save();
    p.setClipRect(r);

    // The input, filled.
    spectrum(shownInput_);
    {
        QLinearGradient fill(QPointF(0, r.top()), QPointF(0, r.bottom()));
        fill.setColorAt(0, withAlpha(Theme::kText, 72));
        fill.setColorAt(1, withAlpha(Theme::kText, 14));
        p.fillToBaseline(points.data(), kPoints, r.bottom(), fill);
    }

    // What is brought up, rising from the bottom (green), and what is taken away, hanging from the top (orange).
    if (maxLift_.value > 0.05 || *std::max_element(shownGain_.begin(), shownGain_.end()) > 0.05) {
        for (std::size_t j = 0; j < points.size(); ++j)
            points[j] = QPointF(xs[j], r.bottom() - depth(shownGain_[j]));
        QLinearGradient fill(QPointF(0, r.bottom()), QPointF(0, r.bottom() - kCurtain * r.height()));
        fill.setColorAt(0, withAlpha(Theme::kPlayOn, 30));
        fill.setColorAt(1, withAlpha(Theme::kPlayOn, 120));
        p.fillToBaseline(points.data(), kPoints, r.bottom(), fill);
        eachRun(
            points, [&](std::size_t j) { return shownGain_[j] > 0.01; },
            [&](const std::vector<QPointF>& run) { p.drawPolyline(run.data(), int(run.size()), Theme::kPlayOn, 1.2); });
    }
    // (With Delta on, the output line shows what is taken away: the curtain, its held line and the glow, which
    // show it too, fade back so the line reads.)
    const double shown = 1.0 - kDeltaFade * deltaShown_.value;
    if (anyHeld_) {  // the recent deepest cut, held, then falling: for as long as it is there
        for (std::size_t j = 0; j < points.size(); ++j)
            points[j] = QPointF(xs[j], r.top() + depth(heldCut_[j]));
        eachRun(points, [&](std::size_t j) { return heldCut_[j] > kHeldDrawnDb; },
                [&](const std::vector<QPointF>& run) {
                    p.drawPolyline(run.data(), int(run.size()), withAlpha(Theme::kAccent, int(150 * shown)), 1.0);
                });
    }
    if (maxCut_.value > 0.05 || *std::min_element(shownGain_.begin(), shownGain_.end()) < -0.05) {
        for (std::size_t j = 0; j < points.size(); ++j)
            points[j] = QPointF(xs[j], r.top() + depth(-shownGain_[j]));
        QLinearGradient fill(QPointF(0, r.top()), QPointF(0, r.top() + kCurtain * r.height()));
        fill.setColorAt(0, withAlpha(Theme::kAccent, int(110 * shown)));
        fill.setColorAt(1, withAlpha(Theme::kAccent, int(24 * shown)));
        p.fillToBaseline(points.data(), kPoints, r.top(), fill);
        const QColor edge = withAlpha(Theme::kAccent, int(255 * shown));
        eachRun(
            points, [&](std::size_t j) { return shownGain_[j] < -0.01; },
            [&](const std::vector<QPointF>& run) { p.drawPolyline(run.data(), int(run.size()), edge, 1.2); });
    }

    // Where the level compared is over the threshold, glowing: why a frequency is turned down (so not while
    // nothing is: Ratio 1:1, Range 0 or Dry/Wet 0).
    const int glow = int(64 * hotShown_.value * shown);
    if (glow > 0) {
        std::vector<float> tops(kPoints), bottoms(kPoints);
        bool over = false;
        for (std::size_t j = 0; j < tops.size(); ++j) {
            const double threshold = thresholdCurve_[j];
            bottoms[j] = float(yOfLevel(threshold));
            tops[j] = float(yOfLevel(std::max(shownKey_[j], threshold)));
            over = over || tops[j] < bottoms[j];
        }
        if (over) {
            const double dx = r.width() / (kPoints - 1);
            p.fillBand(r.left() - dx / 2, dx, tops.data(), bottoms.data(), kPoints, withAlpha(Theme::kAccent, glow));
        }
    }

    // Outside the Focus band, dimmed by the share of its gain each frequency doesn't get (the engine's Focus weights
    // for the edges as drawn): fully where it gets none, fading across each edge as the sound does.
    if (focusDimmed_) {
        QGradientStops stops;
        const double last = double(focusWeights_.size() - 1);
        for (int i = 0; i < focusWeights_.size(); ++i) {
            stops.append({i / last, withAlpha(Theme::kMeterBg, focusDimAlpha(focusWeights_[i]))});
        }
        QLinearGradient dim(QPointF(r.left(), 0), QPointF(r.right(), 0));
        dim.setStops(stops);
        p.fillRect(r, dim);
    }

    // The level figures, inside at the left: over the dim (read wherever the Focus band starts), under the lines.
    for (const double db : kLevelFigures) {
        const double clear = figureShown(db);
        if (clear > 0.01)
            p.drawText(QRectF(r.left() + 9, yOfLevel(db) - 11, 40, 10), Qt::AlignLeft | Qt::AlignBottom,
                       levelFigure(db), withAlpha(Theme::kTextDim, int(std::lround(255 * clear))), font7);
    }

    // The sidechain's levels (what is compared), dashed, while keyed; the output. (Each only where there is any:
    // lying along the floor, a line would read as one of its own.)
    if (keyed_) {
        spectrum(shownKey_);
        eachRun(points, [&](std::size_t j) { return shownKey_[j] > kFloorDb; },
                [&](const std::vector<QPointF>& run) { drawDashedPolyline(p, run, Theme::kMeterMid, 1.0); });
    }
    // With Delta on it is what is taken away, in a red of its own (the threshold and the curtain are orange).
    spectrum(shownOutput_);
    const QColor outputColor = mixColor(Theme::kFrozen, Theme::kMeterHigh, deltaShown_.value);
    eachRun(points, [&](std::size_t j) { return shownOutput_[j] > kFloorDb; },
            [&](const std::vector<QPointF>& run) { drawGlowPolyline(p, run, outputColor, 1.25); });

    // The thresholds: Below (green, while Upward is on) under the threshold (orange). (Clipped to the plot, where a
    // line steep enough leaves it.)
    if (belowOpacity > 0.001) {
        p.setOpacity(belowOpacity);
        drawGlowPolyline(p, {belowLine.p1(), belowLine.p2()}, Theme::kPlayOn, 1.25);
        p.setOpacity(1.0);
    }
    const QColor lineColor = active_ ? Theme::kAccent : Theme::kTextDim;
    drawGlowPolyline(p, {thresholdLine.p1(), thresholdLine.p2()}, lineColor, 1.5);
    p.restore();

    // The Focus edges: a line each, and a grip at the bottom (an open edge, only its grip).
    const double low = focusLowShown(), high = focusHighShown();
    for (const int which : {int(FocusLowEdge), int(FocusHighEdge)}) {
        const double grow = handleGrow_[std::size_t(which)].value;
        const QColor color = withAlpha(Theme::kText, int(50 + 110 * grow));
        const double x = edgeX(which);
        const bool open = which == FocusLowEdge ? low <= kLow * 1.001 : high >= kHigh * 0.999;
        if (!open)
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()), color, 1.0);
        p.fillRoundedRect(QRectF(x - 2, r.bottom() - 12, 4, 12), 2, 2, color);
    }

    // The handles, ringed in the background's colour so they stand off the lines.
    auto circle = [&](const QPointF& at, double radius, const QColor& color) {
        p.fillEllipse(at, radius + 1.0, radius + 1.0, Theme::kMeterBg);
        p.fillEllipse(at, radius, radius, color);
    };
    auto diamond = [&](const QPointF& at, double radius, const QColor& color) {
        const double ring = radius + 1.4;
        const QPointF outline[4] = {{at.x(), at.y() - ring}, {at.x() + ring, at.y()}, {at.x(), at.y() + ring},
                                    {at.x() - ring, at.y()}};
        const QPointF inside[4] = {{at.x(), at.y() - radius}, {at.x() + radius, at.y()}, {at.x(), at.y() + radius},
                                   {at.x() - radius, at.y()}};
        p.fillPolygon(outline, 4, Theme::kMeterBg);
        p.fillPolygon(inside, 4, color);
    };
    if (belowOpacity > 0.001) {
        p.setOpacity(belowOpacity);
        circle(handle(BelowHandle), 4.0 + 1.5 * handleGrow_[std::size_t(BelowHandle)].value, Theme::kPlayOn);
        p.setOpacity(1.0);
    }
    if (dragged_ == ThresholdHandle)
        p.fillEllipse(handle(ThresholdHandle), 11, 11, withAlpha(Theme::kAccent, 60));
    diamond(handle(TiltLow), 4.0 + handleGrow_[std::size_t(TiltLow)].value, lineColor);
    diamond(handle(TiltHigh), 4.0 + handleGrow_[std::size_t(TiltHigh)].value, lineColor);
    circle(handle(ThresholdHandle), 5.0 + 2.0 * handleGrow_[std::size_t(ThresholdHandle)].value, lineColor);

    // In and Out meters at the right edge.
    const QRectF meterIn(w - 19, r.top(), 6, r.height()), meterOut(w - 10, r.top(), 6, r.height());
    drawLevelMeter(p, meterIn, meterIn_.level, meterIn_.peak, kMeterFloorDb, 0.0);
    drawLevelMeter(p, meterOut, meterOut_.level, meterOut_.peak, kMeterFloorDb, 0.0);
    for (const auto& [meter, label] : {std::pair{meterIn, "I"}, std::pair{meterOut, "O"}})
        p.drawText(QRectF(meter.center().x() - 6, r.bottom() + 1, 12, h - r.bottom() - 2), Qt::AlignCenter, id(label),
                   Theme::kTextDim, font7);

    // The header: the deepest cut (and the biggest lift) now, right; the value hovered or dragged, left.
    const double cut = maxCut_.value, lift = maxLift_.value;
    const QString cutText = cut >= 0.05 ? QStringLiteral("−%1 dB").arg(cut, 0, 'f', 1) : QStringLiteral("0.0 dB");
    const double cutWidth = SgPainter::textWidth(cutText, font8);
    double headerRight = w - 4;
    p.drawText(QRectF(headerRight - cutWidth - 2, 1, cutWidth + 2, 14), Qt::AlignRight | Qt::AlignVCenter, cutText,
               cut >= 0.05 ? Theme::kAccent : Theme::kTextDim, font8);
    headerRight -= cutWidth + 8;
    if (belowShown()) {
        const QString liftText = QStringLiteral("+%1 dB").arg(std::max(lift, 0.0), 0, 'f', 1);
        const double liftWidth = SgPainter::textWidth(liftText, font8);
        p.drawText(QRectF(headerRight - liftWidth - 2, 1, liftWidth + 2, 14), Qt::AlignRight | Qt::AlignVCenter,
                   liftText, lift >= 0.05 ? Theme::kPlayOn : Theme::kTextDim, font8);
        headerRight -= liftWidth + 8;
    }
    if (readoutOpacity_.value > 0.001 && !readoutText_.isEmpty()) {
        p.setOpacity(readoutOpacity_.value);
        p.drawText(QRectF(kHeaderLeft, 1, std::max(0.0, headerRight - kHeaderLeft), 14),
                   Qt::AlignLeft | Qt::AlignVCenter, readoutText_, Theme::kText, font8);
        p.setOpacity(1.0);
    }
}

}  // namespace sub::ui
