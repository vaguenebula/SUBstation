#include "devices/PhaserGraph.h"

#include "audio/EngineBridge.h"
#include "devices/EditorPaint.h"
#include "input/GestureKey.h"
#include "model/ParamSpec.h"
#include "model/Project.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QLinearGradient>
#include <QHoverEvent>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>

namespace sub::ui {

// --- PhaserGraph ------------------------------------------------------------------------

namespace {

// The displays, in the engine's order (their index is the playback's stream).
enum Stream { kPhase = 0, kPhaseRight, kLfo, kMod, kEnv, kSweepLeft, kSweepRight, kQLeft, kQRight, kInput, kOutput };
const QString& displayId(int stream) {
    static const QString kIds[PhaserGraph::kStreams] = {
        QStringLiteral("phase"),   QStringLiteral("phase_r"), QStringLiteral("lfo"),     QStringLiteral("mod"),
        QStringLiteral("env"),     QStringLiteral("sweep_l"), QStringLiteral("sweep_r"), QStringLiteral("q_l"),
        QStringLiteral("q_r"),     QStringLiteral("input"),   QStringLiteral("output")};
    return kIds[stream];
}

constexpr double kMeterFloor = -60.0;  // the meters' range
constexpr int kModeHold = 8;           // a mode change: frames still on their way of the old mode, at most
constexpr double kMinLog = -40.0;      // log2 of the least sweep or Q taken (no log of 0)

// The engine's: the parameters' ranges.
const sub::app::PhaserRanges& ranges() {
    static const sub::app::PhaserRanges kRanges = sub::app::phaserRanges();
    return kRanges;
}

double log2Of(double v) { return std::max(kMinLog, std::log2(std::max(v, 1e-12))); }

const QString& timeId(int mode) {
    static const QString kFlange = QStringLiteral("flange_time"), kDoubler = QStringLiteral("doubler_time");
    return mode == 2 ? kDoubler : kFlange;
}

}  // namespace

PhaserGraph::PhaserGraph(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(kWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);  // (the drag's cursor over the plot only: hoverMoveEvent)
    trace_.assign(kTrace, 0.0f);
    in_.reset(kMeterFloor);
    out_.reset(kMeterFloor);
    dotOpacity_.snap(0.35);
    sweepL_.snap(log2Of(1000.0));
    sweepR_ = sweepL_;
    qL_.snap(log2Of(sub::app::phaserQ(50.0)));
    qR_ = qL_;
    // The curve is the engine's at its sample rate: worked out again when the audio device changes.
    connect(this, &DeviceCanvas::deviceChanged, this, [this] {
        disconnect(bridgeConnection_);
        if (session())
            bridgeConnection_ = connect(session()->bridge(), &sub::app::EngineBridge::deviceChanged, this, [this] {
                updateCurve();
                update();
            });
        // Another device: nothing of the last one's is drawn.
        first_ = true;
        live_ = false;
        playback_.clear();
        for (std::vector<float>& values : pending_) values.clear();
        trailCount_ = traceCount_ = 0;
        traced_ = -1;
    });
}

QRectF PhaserGraph::plot() const {
    return QRectF(1, kHeader, width() - 2 - 3 * kMeterWidth, height() - kHeader - kStripHeight - 2);
}

QRectF PhaserGraph::strip() const { return QRectF(1, height() - kStripHeight - 1, width() - 2, kStripHeight); }

QRectF PhaserGraph::meters() const {
    const QRectF r = plot();
    return QRectF(r.right() + 1, r.top(), width() - 1 - (r.right() + 1), r.height());
}

LogAxis PhaserGraph::frequencyAxis() const {
    const QRectF r = plot();
    return {kLow, kHigh, r.left(), r.width()};
}

double PhaserGraph::xOf(double freq) const { return frequencyAxis().position(freq); }

double PhaserGraph::freqAt(double x) const { return frequencyAxis().valueAt(x); }

double PhaserGraph::yOf(double db) const {
    const QRectF r = plot();
    const double fraction = (kTopDb - (std::isfinite(db) ? db : kBottomDb)) / (kTopDb - kBottomDb);
    return r.top() + std::clamp(fraction, 0.0, 1.0) * r.height();
}

double PhaserGraph::xOfTime(double ms) const {
    const sub::app::PhaserRanges& range = ranges();
    if (mode_ != 2)
        return xOf(500.0 / std::clamp(ms, range.minFlangeMs, range.maxFlangeMs));
    const QRectF r = plot();
    const double held = std::clamp(ms, range.minDoublerMs, range.maxDoublerMs);
    const double span = std::log(range.maxDoublerMs / range.minDoublerMs);
    return r.left() + std::log(range.maxDoublerMs / held) / span * r.width();
}

double PhaserGraph::timeAt(double x) const {
    const sub::app::PhaserRanges& range = ranges();
    if (mode_ != 2)
        return std::clamp(500.0 / freqAt(x), range.minFlangeMs, range.maxFlangeMs);
    const QRectF r = plot();
    const double fraction = std::clamp((x - r.left()) / r.width(), 0.0, 1.0);
    return range.maxDoublerMs * std::pow(range.minDoublerMs / range.maxDoublerMs, fraction);
}

QString PhaserGraph::notchText() const {
    if (mode_ != 1)
        return QString();
    const double firstNotch = 500.0 / std::max(flangeTime_, 1e-3);  // Hz: half a cycle in the delay
    return QStringLiteral("Notch %1").arg(sub::app::formatValue(firstNotch, QStringLiteral("Hz")));
}

sub::app::PhaserCurve PhaserGraph::curveSettings() const {
    sub::app::PhaserCurve c;
    c.mode = mode_;
    c.notches = notches_;
    c.centerHz = sweepLeft();
    c.q = qLeft();
    c.delayMs = sweepLeft();
    c.feedback = sub::app::phaserFeedbackGain(feedback_, fbInvert_);
    c.warmth = warmth_ / 100.0;
    c.mix = mix_ / 100.0;
    c.safeBassHz = safeBass_;
    c.outputDb = output_;
    return c;
}

double PhaserGraph::staticSweepLog() const {
    const double rate = sampleRate();
    if (mode_ == 0)
        return log2Of(sub::app::phaserCenterHz(center_, blend_, 0.0, rate));
    return log2Of(sub::app::phaserDelayMs(mode_, mode_ == 2 ? doublerTime_ : flangeTime_, 0.0, rate));
}

double PhaserGraph::staticQLog() const { return log2Of(sub::app::phaserQ(spread_, blend_, 0.0)); }

bool PhaserGraph::randomShape() const { return sub::app::phaserWaveIsRandom(wave_); }

double PhaserGraph::shapeAt(double phase) const {
    return sub::app::phaserLfoValue(wave_, phase, duty_, rateHz_, 0);
}

void PhaserGraph::sync() {
    const int mode = std::clamp(int(std::lround(value(QStringLiteral("mode")))), 0, 2);
    notches_ = std::clamp(int(std::lround(value(QStringLiteral("notches")))), 1, ranges().maxNotches);
    center_ = value(QStringLiteral("center"));
    spread_ = value(QStringLiteral("spread"));
    blend_ = value(QStringLiteral("blend"));
    flangeTime_ = value(QStringLiteral("flange_time"));
    doublerTime_ = value(QStringLiteral("doubler_time"));
    feedback_ = value(QStringLiteral("feedback"));
    fbInvert_ = value(QStringLiteral("fb_invert")) >= 0.5;
    warmth_ = value(QStringLiteral("warmth"));
    mix_ = value(QStringLiteral("mix"));
    safeBass_ = value(QStringLiteral("safe_bass"));
    output_ = value(QStringLiteral("output"));
    static const int kLastWave = int(sub::app::phaserWaveLabels().size()) - 1;
    const int wave = std::clamp(int(std::lround(value(QStringLiteral("lfo_wave")))), 0, kLastWave);
    if (wave != wave_ && sub::app::phaserWaveIsRandom(wave)) {
        // A random shape chosen: its trace starts empty (four cycles of the shape until values come),
        // from the frames that come next (those already here are the shape's before).
        traceCount_ = traceNext_ = 0;
        traced_ = std::max(traced_, playback_.newest());
    }
    wave_ = wave;
    duty_ = value(QStringLiteral("lfo_duty")) / 100.0;
    const double tempo = session() && session()->project() ? session()->project()->tempo() : 120.0;
    rateHz_ = value(QStringLiteral("lfo_sync")) >= 0.5
                  ? sub::app::phaserSyncedRateHz(int(std::lround(value(QStringLiteral("lfo_rate")))), tempo)
                  : value(QStringLiteral("lfo_freq"));
    phaseOffset_ = value(QStringLiteral("phase")) / 360.0;
    spinOn_ = value(QStringLiteral("spin_on")) >= 0.5;
    lfo2Mix_ = value(QStringLiteral("lfo2_mix"));
    envOn_ = value(QStringLiteral("env_on")) >= 0.5;

    // A mode change: the old curve fades out under the new, and the frames on their way, the old
    // mode's, are not drawn as the new one's.
    const bool modeChanged = mode != mode_;
    if (modeChanged && !first_) {
        oldLayout_ = leftLayout_;
        oldFade_.snap(1.0);
        holdUntil_ = playback_.newest() + 1 + kModeHold;
    }
    mode_ = mode;
    // Not live (or the mode changed), the curve is where the parameters put it: an edit is drawn at once.
    if (first_ || !live_ || modeChanged) {
        for (Eased* sweep : {&sweepL_, &sweepR_}) sweep->snap(staticSweepLog());
        for (Eased* q : {&qL_, &qR_}) q->snap(staticQLog());
        catchUp_.snap(0.0);
    }
    if (first_) {
        oldFade_.snap(0.0);
        oldLayout_ = {};
    }
    if (!live_ && !spinOn_)
        lfoPhaseRight_ = lfoPhase_ + phaseOffset_ - std::floor(lfoPhase_ + phaseOffset_);
    if (!live_ && !randomShape())
        lfoValue_ = shapeAt(lfoPhase_);
    first_ = false;
    updateShape();
    updateCurve();
    update();
}

void PhaserGraph::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        updateShape();
        updateCurve();
    }
}

QRectF PhaserGraph::lane() const { return strip().adjusted(5, 12, -11, -4); }

void PhaserGraph::updateShape() {
    // The shape over one cycle, a value per pixel across the lane; the random shapes' four cycles
    // from cycle 0 (what is drawn before any of their values came).
    const QRectF r = lane();
    const int count = std::max(2, int(std::ceil(r.width())) + 1);
    shape_.resize(size_t(count));
    const bool random = randomShape();
    for (int i = 0; i < count; ++i) {
        const double x = double(i) / (count - 1);
        if (random) {
            const double cycles = 4.0 * std::min(x, 0.99999);
            shape_[size_t(i)] = float(sub::app::phaserLfoValue(wave_, cycles - std::floor(cycles), duty_, rateHz_,
                                                               quint32(std::floor(cycles))));
        } else {
            shape_[size_t(i)] = float(shapeAt(std::min(x, 0.99999)));
        }
    }
}

void PhaserGraph::updateCurve() {
    const double rate = sampleRate();
    const int columns = std::max(2, int(plot().width()));
    sub::app::PhaserCurve c = curveSettings();
    left_ = sub::app::phaserCurvePoints(c, kLow, kHigh, columns, rate);
    // The right channel's, while it differs.
    constexpr double kApart = 0.0072;  // log2(1.005): half a percent
    const bool apart = std::abs(sweepR_.value - sweepL_.value) > kApart ||
                       (mode_ == 0 && std::abs(qR_.value - qL_.value) > kApart);
    if (apart) {
        sub::app::PhaserCurve right = c;
        right.centerHz = right.delayMs = sweepRight();
        right.q = qRight();
        right_ = sub::app::phaserCurvePoints(right, kLow, kHigh, columns, rate);
    } else {
        right_ = {};
    }
    leftLayout_ = layOut(left_, true);
    rightLayout_ = layOut(right_, false);
    // The notches along the bottom: the Phaser's every one; a comb's while they are apart enough to tell.
    markers_.clear();
    if (mode_ == 0) {
        for (const double f : sub::app::phaserNotchFrequencies(notches_, c.centerHz, c.q, rate)) {
            if (kLow <= f && f <= kHigh)
                markers_.push_back(f);
        }
    } else {
        double lastX = -1e9;
        for (const double f : sub::app::phaserCombNotchFrequencies(c.delayMs, kHigh, 64)) {
            if (f < kLow)
                continue;
            const double x = xOf(f);
            if (x - lastX < kMarkerSpacing)
                break;
            markers_.push_back(f);
            lastX = x;
        }
    }
    // Each as bright as its notch is deep: faint at -12 dB, full from -30 dB.
    const QList<double> depths =
        sub::app::phaserResponseDb(c, rate, QList<double>(markers_.begin(), markers_.end()));
    markerAlpha_.resize(markers_.size());
    for (size_t i = 0; i < markers_.size(); ++i)
        markerAlpha_[i] = std::clamp((-3.0 - depths.value(qsizetype(i))) / 27.0, 0.0, 1.0);
    drawnLeft_ = sweepL_.value;
    drawnRight_ = sweepR_.value;
    drawnQLeft_ = qL_.value;
    drawnQRight_ = qR_.value;
    Q_EMIT curveChanged();
    Q_EMIT sweepChanged();
}

PhaserGraph::CurveLayout PhaserGraph::layOut(const sub::app::PhaserCurvePoints& curve, bool withBands) const {
    CurveLayout out;
    const qsizetype columns = curve.top.size();
    if (columns == 0 || curve.lineHz.size() < 2)
        return out;
    const QRectF r = plot();
    const double column = r.width() / double(columns);  // (pixels)
    // The columns too fine to draw as a line (a cycle of the comb in fewer than kBandPeriod pixels: its
    // glow would merge into a block), and the comb's top and bottom there: the highest and lowest over
    // a cycle's worth of columns around each (one column holds only part of a cycle).
    constexpr double kCycle = 2.0 * 3.14159265358979323846;
    std::vector<char> fine(static_cast<size_t>(columns), 0);
    std::vector<double> top(static_cast<size_t>(columns), 0.0), bottom(static_cast<size_t>(columns), 0.0);
    for (qsizetype c = 0; c < columns; ++c) {
        const double turn = curve.turn.value(c, 0.0);
        fine[size_t(c)] = curve.dense[c] || turn * kBandPeriod / column > kCycle;
        if (!fine[size_t(c)])
            continue;
        const qsizetype half = curve.dense[c] ? 0 : std::min<qsizetype>(8, qsizetype(std::ceil(kCycle / turn / 2)));
        double high = -1e300, low = 1e300;
        for (qsizetype k = std::max<qsizetype>(0, c - half); k <= std::min(columns - 1, c + half); ++k) {
            high = std::max(high, curve.top[k]);
            low = std::min(low, curve.bottom[k]);
        }
        top[size_t(c)] = high;
        bottom[size_t(c)] = low;
    }
    // The line: the curve's own points over the other columns, the comb's top over these.
    out.line.reserve(size_t(curve.lineHz.size()));
    qsizetype i = 0;
    for (qsizetype c = 0; c < columns; ++c) {
        const double x0 = r.left() + double(c) * column, x1 = x0 + column;
        if (fine[size_t(c)]) {
            out.line.emplace_back(x0 + column / 2, yOf(top[size_t(c)]));
            while (i < curve.lineHz.size() && xOf(curve.lineHz[i]) < x1 - 1e-9) ++i;
            continue;
        }
        for (; i < curve.lineHz.size() && (xOf(curve.lineHz[i]) < x1 - 1e-9 || c == columns - 1); ++i)
            out.line.emplace_back(xOf(curve.lineHz[i]), yOf(curve.lineDb[i]));
    }
    if (!withBands)
        return out;
    for (qsizetype c = 0; c < columns;) {
        if (!fine[size_t(c)]) {
            ++c;
            continue;
        }
        CurveLayout::Band band;
        band.x = r.left() + double(c) * column;
        for (; c < columns && fine[size_t(c)]; ++c) {
            band.tops.push_back(float(yOf(top[size_t(c)])));
            band.bottoms.push_back(float(yOf(bottom[size_t(c)])));
        }
        out.bands.push_back(std::move(band));
    }
    return out;
}

// --- The displays -------------------------------------------------------------------------

bool PhaserGraph::frameFits() const {
    if (playback_.head() < double(holdUntil_))
        return false;
    // The Phaser publishes its stages' Q, the delay modes 0.
    return (playback_.value(kQLeft) > 0.0) == (mode_ == 0);
}

void PhaserGraph::trace() {
    if (playback_.empty())
        return;
    const auto head = static_cast<qint64>(std::floor(playback_.head()));
    const qint64 from = std::max({traced_ + 1, playback_.newest() - Playback::kCapacity + 1, qint64(0)});
    for (qint64 i = from; i <= head; ++i) {
        trace_[size_t(traceNext_)] = playback_.at(i, kLfo);
        traceNext_ = (traceNext_ + 1) % kTrace;
        traceCount_ = std::min(traceCount_ + 1, kTrace);
    }
    traced_ = std::max(traced_, head);
}

void PhaserGraph::refreshDisplays() {
    const double dt = tickSeconds();

    // What came, as whole frames (the streams are published together; a read between two of them
    // leaves the rest for the next tick).
    for (int s = 0; s < kStreams; ++s) {
        const std::vector<float> values = readDisplay(displayId(s));
        pending_[size_t(s)].insert(pending_[size_t(s)].end(), values.begin(), values.end());
    }
    size_t count = pending_[0].size();
    for (const std::vector<float>& values : pending_) count = std::min(count, values.size());
    double inPeak = -1e300, outPeak = -1e300;
    for (size_t i = 0; i < count; ++i) {
        Playback::Frame frame;
        for (int s = 0; s < kStreams; ++s) {
            const float v = pending_[size_t(s)][i];
            frame[size_t(s)] = std::isfinite(v) ? v : 0.0f;
        }
        inPeak = std::max(inPeak, double(frame[kInput]));
        outPeak = std::max(outPeak, double(frame[kOutput]));
        playback_.append(frame);
    }
    for (std::vector<float>& values : pending_) values.erase(values.begin(), values.begin() + std::ptrdiff_t(count));
    playback_.endBatch(int(count));

    bool moving = false;
    const bool wasLive = live_;
    if (count > 0) {
        sinceArrival_ = 0.0;
        if (!live_) {  // coming back: from the newest, the curve catching up from where it is
            live_ = true;
            playback_.snapToNewest();
            traced_ = std::max(traced_, playback_.newest() - 1);
            startCatchUp();
        }
    } else {
        sinceArrival_ += dt;
    }
    playback_.advance(dt, sampleRate() / sub::app::phaserDisplaySamples());
    if (live_ && sinceArrival_ > kStaleSeconds)
        live_ = false;

    if (live_) {
        const bool fits = frameFits();
        if (fits && !fitted_)
            startCatchUp();
        fitted_ = fits;
        if (fits) {
            // The engine's sweep at the playhead (catching up over a few ticks after a pause or a mode change).
            const double toward = catchUp_.value;
            const auto follow = [toward](Eased& drawn, double live, double from) {
                drawn.snap(live + toward * (from - live));
            };
            follow(sweepL_, log2Of(playback_.logValue(kSweepLeft)), catchFrom_[0]);
            follow(sweepR_, log2Of(playback_.logValue(kSweepRight)), catchFrom_[1]);
            if (mode_ == 0) {
                follow(qL_, log2Of(playback_.logValue(kQLeft)), catchFrom_[2]);
                follow(qR_, log2Of(playback_.logValue(kQRight)), catchFrom_[3]);
            }
            catchUp_.target = 0.0;
            catchUp_.step(easeFraction(dt, 0.03), 1e-3);
        } else {
            for (Eased* sweep : {&sweepL_, &sweepR_}) sweep->snap(staticSweepLog());
            for (Eased* q : {&qL_, &qR_}) q->snap(staticQLog());
        }
        lfoPhase_ = playback_.phase(kPhase);
        lfoPhaseRight_ = playback_.phase(kPhaseRight);
        lfoValue_ = playback_.value(kLfo);
        modulation_ = playback_.value(kMod);
        envBar_.target = envOn_ ? std::clamp(playback_.value(kEnv), 0.0, 1.0) : 0.0;
        if (randomShape())
            trace();
        moving = true;
    } else {
        // Quiet: back to where the parameters put it.
        const double fraction = easeFraction(dt, 0.05);
        sweepL_.target = sweepR_.target = staticSweepLog();
        qL_.target = qR_.target = staticQLog();
        moving |= sweepL_.step(fraction, 1e-5) | sweepR_.step(fraction, 1e-5);
        moving |= qL_.step(fraction, 1e-5) | qR_.step(fraction, 1e-5);
        if (modulation_ != 0.0) {
            modulation_ *= 1.0 - fraction;
            if (std::abs(modulation_) < 1e-3)
                modulation_ = 0.0;
            moving = true;
        }
        envBar_.target = 0.0;
        if (!spinOn_)
            lfoPhaseRight_ = lfoPhase_ + phaseOffset_ - std::floor(lfoPhase_ + phaseOffset_);
    }

    // The dots brighten while live; the trail follows the dot (settling onto it once quiet).
    dotOpacity_.target = live_ ? 1.0 : 0.35;
    moving |= dotOpacity_.step(easeFraction(dt, 0.08), 1e-3);
    if (live_ || trailSettle_ > 0) {
        // How far the dot went since the last tick (forward: under half a cycle a tick at any rate
        // the displays resolve), and so how far it went over the trail's ticks, however fast.
        const Dot& last = trail_[size_t((trailNext_ - 1 + kTrail) % kTrail)];
        const double step = trailCount_ > 0 ? lfoPhase_ - last.phase - std::floor(lfoPhase_ - last.phase) : 0.0;
        trail_[size_t(trailNext_)] = {lfoPhase_, lfoValue_, step};
        trailNext_ = (trailNext_ + 1) % kTrail;
        trailCount_ = std::min(trailCount_ + 1, kTrail);
        trailSettle_ = live_ ? kTrail : trailSettle_ - 1;
        trailSpan_ = 0.0;
        for (int k = 0; k + 1 < trailCount_; ++k)  // (the newest's steps: the oldest's own came before it)
            trailSpan_ += trail_[size_t((trailNext_ - 1 - k + kTrail) % kTrail)].step;
        moving = true;
    }

    // The meters want the newest: what came this tick, or on a tick that brought nothing (a large
    // audio block's gap) the last that came, so they go on falling and holding at their pace; quiet,
    // they fall.
    if (count > 0) {
        lastInPeak_ = inPeak;
        lastOutPeak_ = outPeak;
    }
    if (live_) {
        in_.update(lastInPeak_, dt, 24.0, 1.0, kMeterFloor);
        out_.update(lastOutPeak_, dt, 24.0, 1.0, kMeterFloor);
    } else {
        in_.update(kMeterFloor, dt, 24.0, 1.0, kMeterFloor);
        out_.update(kMeterFloor, dt, 24.0, 1.0, kMeterFloor);
    }
    for (const MeterBallistics* meter : {&in_, &out_})
        moving |= meter->level > kMeterFloor || meter->peak > kMeterFloor;
    moving |= envBar_.step(easeFraction(dt, 0.03), 1e-3);
    if (oldFade_.value > 0.0) {
        oldFade_.target = 0.0;
        oldFade_.step(easeFraction(dt, 0.06), 0.01);
        if (oldFade_.value <= 0.0)
            oldLayout_ = {};
        moving = true;
    }

    // The curve again where the sweep moved (by more than a hundredth of a percent).
    constexpr double kMoved = 1.5e-4;  // log2(1.0001)
    if (std::abs(sweepL_.value - drawnLeft_) > kMoved || std::abs(sweepR_.value - drawnRight_) > kMoved ||
        std::abs(qL_.value - drawnQLeft_) > kMoved || std::abs(qR_.value - drawnQRight_) > kMoved) {
        updateCurve();
        moving = true;
    }
    if (live_ != wasLive)
        Q_EMIT displaysChanged();
    moving_ = moving;
    if (moving)
        update();
}

void PhaserGraph::startCatchUp() {
    catchFrom_ = {sweepL_.value, sweepR_.value, qL_.value, qR_.value};
    catchUp_.snap(1.0);
}

// --- Dragging -------------------------------------------------------------------------

void PhaserGraph::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !plot().contains(event->position())) {
        lastGesture_.clear();
        event->ignore();  // (the strip and the meters: the frame's)
        return;
    }
    if (secondPressOfDoubleClick(event))
        return;
    gesture_ = lastGesture_ = newGestureKey();
    pressedAt_ = event->position();
    pressedValue_ = mode_ == 0 ? spread_ : feedback_;
    touch(mode_ == 0 ? QStringLiteral("center") : timeId(mode_));
    dragTo(event->position());
}

void PhaserGraph::mouseMoveEvent(QMouseEvent* event) {
    if (!gesture_.isEmpty())
        dragTo(event->position());
}

void PhaserGraph::mouseReleaseEvent(QMouseEvent*) { gesture_.clear(); }

void PhaserGraph::mouseUngrabEvent() { gesture_.clear(); }

void PhaserGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !plot().contains(event->position()))
        return;
    gesture_.clear();
    const QString across = mode_ == 0 ? QStringLiteral("center") : timeId(mode_);
    const QString upDown = mode_ == 0 ? QStringLiteral("spread") : QStringLiteral("feedback");
    // Under the first click's key: its jump and the reset are one undo step, which undoes to what was
    // there before the double-click.
    setParams({{across, defaultValue(across)}, {upDown, defaultValue(upDown)}},
              lastGesture_.isEmpty() ? newGestureKey() : lastGesture_);
}

void PhaserGraph::hoverMoveEvent(QHoverEvent* event) {
    // The drag's cursor over the plot; the strip and the meters take no drag.
    if (plot().contains(event->position()))
        setCursor(Qt::SizeAllCursor);
    else
        unsetCursor();
}

void PhaserGraph::dragTo(const QPointF& pos) {
    const double up = pressedAt_.y() - pos.y();
    if (mode_ == 0) {
        const double spread = std::clamp(pressedValue_ + up / kSpreadPixels * 100.0, 0.0, 100.0);
        setParams({{QStringLiteral("center"), std::clamp(freqAt(pos.x()), ranges().minCenterHz, ranges().maxCenterHz)},
                   {QStringLiteral("spread"), spread}},
                  gesture_);
    } else {
        const double feedback = std::clamp(pressedValue_ + up / kFeedbackPixels * 100.0, 0.0, 100.0);
        setParams({{timeId(mode_), timeAt(pos.x())}, {QStringLiteral("feedback"), feedback}}, gesture_);
    }
}

// --- Painting ---------------------------------------------------------------------------

void PhaserGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    p.fillRect(QRectF(0, 0, width(), height()), Theme::kMeterBg);
    paintResponse(p);
    paintStrip(p);
}

void PhaserGraph::paintResponse(SgPainter& p) {
    const QRectF r = plot();
    const QFont font = uiFont(7);
    drawDecadeGrid(p, r, frequencyAxis());
    for (const int db : {12, 0, -12, -24}) {
        const double y = yOf(db);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y),
                   db == 0 ? Theme::kGridBar : withAlpha(Theme::kGridBeat, 160));
        p.drawText(QRectF(r.left() + 3, y + 1, 30, 10), Qt::AlignLeft | Qt::AlignTop,
                   db > 0 ? QStringLiteral("+%1").arg(db) : QString::number(db), Theme::kTextDim, font);
    }

    const bool heard = mix_ > 0.0;
    const QColor color = heard ? Theme::kScopeLine : Theme::kTextDisabled;
    const std::vector<QPointF>& line = leftLayout_.line;
    const double column = r.width() / double(std::max<qsizetype>(1, left_.top.size()));
    p.save();
    p.setClipRect(r);
    // Under the curve, a glow fading downwards.
    if (line.size() >= 2) {
        QLinearGradient gradient(r.topLeft(), r.bottomLeft());
        gradient.setColorAt(0, withAlpha(heard ? Theme::kAccent : Theme::kTextDisabled, 70));
        gradient.setColorAt(1, withAlpha(heard ? Theme::kAccent : Theme::kTextDisabled, 6));
        p.fillToBaseline(line.data(), int(line.size()), r.bottom(), gradient);
    }
    // A comb finer than the eye can follow: a band between its peaks and its notches.
    for (const CurveLayout::Band& band : leftLayout_.bands)
        p.fillBand(band.x, column, band.tops.data(), band.bottoms.data(), int(band.tops.size()), withAlpha(color, 64));
    if (oldFade_.value > 0.0 && oldLayout_.line.size() >= 2)
        p.drawPolyline(oldLayout_.line.data(), int(oldLayout_.line.size()),
                       withAlpha(Theme::kTextDim, int(200 * oldFade_.value)), 1.0);
    if (rightLayout_.line.size() >= 2)
        p.drawPolyline(rightLayout_.line.data(), int(rightLayout_.line.size()),
                       withAlpha(Theme::kFrozen, heard ? 120 : 50), 1.0);
    drawGlowPolyline(p, line, color, 1.6);
    p.restore();

    // The notches along the bottom, as bright as they are deep; the Phaser's centre dashed.
    if (mode_ == 0) {
        const double x = xOf(std::clamp(sweepLeft(), kLow, kHigh));
        drawDashedPolyline(p, {QPointF(x, r.top()), QPointF(x, r.bottom())}, withAlpha(Theme::kAccent, 60), 1.0);
    }
    for (size_t i = 0; i < markers_.size(); ++i) {
        const double x = xOf(markers_[i]);
        const QPointF tip[3] = {{x - 3, r.bottom() - 5}, {x + 3, r.bottom() - 5}, {x, r.bottom()}};
        p.fillPolygon(tip, 3, withAlpha(heard ? Theme::kAccent : Theme::kTextDim, int(255 * markerAlpha_[i])));
    }

    // The envelope's level at the left edge.
    if (envOn_ && envBar_.value > 0.0) {
        const double h = envBar_.value * r.height();
        p.fillRect(QRectF(r.left(), r.bottom() - h, 3, h), withAlpha(Theme::kAccent, int(90 + 130 * envBar_.value)));
    }

    // In and Out.
    const QRectF m = meters();
    const double meterX = m.left() + (m.width() - 2 * kMeterWidth - 2) / 2;
    drawLevelMeter(p, QRectF(meterX, m.top(), kMeterWidth, m.height()), in_.level, in_.peak, kMeterFloor, 0.0);
    drawLevelMeter(p, QRectF(meterX + kMeterWidth + 2, m.top(), kMeterWidth, m.height()), out_.level, out_.peak,
                   kMeterFloor, 0.0);

    // What it is, and where it is now.
    static const QString kModes[] = {QStringLiteral("Phaser"), QStringLiteral("Flanger"), QStringLiteral("Doubler")};
    QString title = kModes[mode_];
    if (mode_ == 0)
        title += notches_ == 1 ? QStringLiteral(" \u00B7 1 notch") : QStringLiteral(" \u00B7 %1 notches").arg(notches_);
    p.drawText(QRectF(r.left() + 3, 1, r.width() / 2, kHeader - 2), Qt::AlignLeft | Qt::AlignVCenter, title,
               Theme::kTextDim, font);
    const QString now = mode_ == 0 ? sub::app::formatValue(sweepLeft(), QStringLiteral("Hz"))
                                   : sub::app::formatValue(sweepLeft(), QStringLiteral("ms"));
    p.drawText(QRectF(r.center().x(), 1, width() - 4 - r.center().x(), kHeader - 2), Qt::AlignRight | Qt::AlignVCenter,
               now, live_ ? Theme::kText : Theme::kTextDim, font);
}

void PhaserGraph::paintStrip(SgPainter& p) {
    const QRectF s = strip();
    const QRectF r = lane();
    const QFont font = uiFont(7);
    p.fillRect(s, Theme::kPanel);
    p.drawLine(QPointF(s.left(), s.top()), QPointF(s.right(), s.top()), Theme::kBorder);
    const double mid = r.center().y();
    p.drawLine(QPointF(r.left(), mid), QPointF(r.right(), mid), withAlpha(Theme::kText, 26));
    const auto xAt = [&r](double phase) { return r.left() + phase * r.width(); };
    const auto yAt = [&r, mid](double v) { return mid - std::clamp(v, -1.0, 1.0) * r.height() / 2; };
    const int count = int(shape_.size());
    const auto shapeY = [&](double phase) {
        return yAt(shape_[size_t(std::clamp(int(std::lround(phase * (count - 1))), 0, count - 1))]);
    };
    const double opacity = dotOpacity_.value;

    // Its name, and LFO 2's share while it has one.
    static const QStringList kNames = sub::app::phaserWaveLabels();
    p.drawText(QRectF(s.left() + 5, s.top() + 1, 100, 11), Qt::AlignLeft | Qt::AlignVCenter, kNames.value(wave_),
               Theme::kTextDim, font);
    if (lfo2Mix_ > 0.0)
        p.drawText(QRectF(s.right() - 105, s.top() + 1, 94, 11), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("LFO 2 %1").arg(sub::app::formatValue(lfo2Mix_, QStringLiteral("%"))),
                   Theme::kTextDim, font);

    p.save();
    p.setClipRect(s.adjusted(0, 1, 0, 0));
    QPointF dot;
    if (!randomShape() && count >= 2) {
        // The shape over a cycle, lit up to the dot.
        const double step = r.width() / (count - 1);
        const int split = std::clamp(int(std::round(lfoPhase_ * (count - 1))), 0, count - 1);
        std::vector<QPointF> done, ahead;
        for (int i = 0; i < count; ++i) {
            const QPointF at(r.left() + i * step, yAt(shape_[size_t(i)]));
            if (i <= split)
                done.push_back(at);
            if (i >= split)
                ahead.push_back(at);
        }
        p.drawPolyline(ahead.data(), int(ahead.size()), Theme::kTextDim, 1.2);
        p.drawPolyline(done.data(), int(done.size()), withAlpha(Theme::kAccent, 150), 1.2);
        // The comet's tail: the way the dot came over the last ticks, brighter towards it (a fast LFO's
        // at most kTrailSpan of a cycle, however far it went).
        if (trailCount_ > 1) {
            const double span = std::min(trailSpan_, kTrailSpan);
            if (span > 0.0) {
                const int steps = std::max(1, int(std::ceil(span * r.width() / 2)));
                QPointF from;
                for (int k = 0; k <= steps; ++k) {
                    const double t = double(k) / steps;
                    double phase = lfoPhase_ - span * (1.0 - t);
                    phase -= std::floor(phase);
                    const QPointF at(xAt(phase), shapeY(phase));
                    if (k > 0 && at.x() >= from.x())  // (not across the cycle's end)
                        p.drawLine(from, at, withAlpha(Theme::kAccent, int(190 * t * opacity)), 3.0, Qt::RoundCap);
                    from = at;
                }
            }
        }
        // The right LFO's dot.
        p.fillEllipse(QPointF(xAt(lfoPhaseRight_), shapeY(lfoPhaseRight_)), 2.5, 2.5,
                      withAlpha(Theme::kFrozen, int(230 * opacity)));
        dot = QPointF(xAt(lfoPhase_), yAt(lfoValue_));
    } else {
        // The values as they came, newest at the right; before any, four cycles of the shape, dim.
        std::vector<QPointF> points;
        if (traceCount_ > 0) {
            const double step = r.width() / (kTrace - 1);
            points.reserve(size_t(traceCount_));
            for (int k = traceCount_ - 1; k >= 0; --k) {  // oldest first
                const float v = trace_[size_t((traceNext_ - 1 - k + kTrace) % kTrace)];
                points.emplace_back(r.right() - k * step, yAt(v));
            }
            p.drawPolyline(points.data(), int(points.size()), withAlpha(Theme::kAccent, 170), 1.2);
        } else if (count >= 2) {
            const double step = r.width() / (count - 1);
            for (int i = 0; i < count; ++i) points.emplace_back(r.left() + i * step, yAt(shape_[size_t(i)]));
            p.drawPolyline(points.data(), int(points.size()), Theme::kTextDim, 1.2);
        }
        dot = QPointF(r.right(), yAt(lfoValue_));
    }
    p.fillEllipse(dot, 9, 9, withAlpha(Theme::kAccent, int(25 * opacity)));
    p.fillEllipse(dot, 6, 6, withAlpha(Theme::kAccent, int(60 * opacity)));
    p.fillEllipse(dot, 3.5, 3.5, withAlpha(Theme::kAccent, int(255 * std::max(opacity, 0.5))));
    p.restore();

    // The modulation the sweep follows (both LFOs and the envelope), from the middle: a full swing
    // of one (Amount 100 %) reaches the lane's edge.
    const double x = s.right() - 6;
    const double reach = std::clamp(modulation_, -1.0, 1.0) * r.height() / 2;
    p.fillRect(QRectF(x, r.top(), 3, r.height()), withAlpha(Theme::kMeterBg, 200));
    if (std::abs(reach) > 0.25)
        p.fillRect(QRectF(x, std::min(mid, mid - reach), 3, std::abs(reach)), withAlpha(Theme::kAccent, 210));
}

}  // namespace sub::ui
