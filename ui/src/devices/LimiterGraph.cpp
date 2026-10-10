#include "devices/LimiterGraph.h"

#include "audio/LimiterResponse.h"
#include "input/GestureKey.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QCursor>
#include <QHoverEvent>
#include <QLinearGradient>
#include <QMouseEvent>

#include <algorithm>
#include <cmath>
#include <limits>

namespace sub::ui {

namespace {

const double kDisplayFloorDb = sub::app::limiterFloorDb();  // what the device publishes for silence

// Time constants (seconds) of what eases, and the meters' ballistics.
constexpr double kScrollEase = 0.12;   // the cursor catching up with the newest values
constexpr double kLineEase = 0.05;     // the line, moved by automation, undo, the box or Maximize
constexpr double kBadgeEase = 0.08;    // Soft Clip's band, the badges
constexpr double kGlowEase = 0.06;     // the line warming with the gain reduction
constexpr double kHoverEase = 0.04;
constexpr double kLevelFall = 24.0;    // dB a second, In and Out
constexpr double kLevelHold = 1.0;     // s
constexpr double kReductionFall = 30.0;
constexpr double kReductionHold = 0.5;
constexpr double kGlowFullDb = 6.0;    // the line is fully warm at this much gain reduction

// The meters, left to right (graph x), two bars each; the reduction's figures between In and GR.
constexpr double kMeterX[3] = {260.0, 287.0, 308.0};  // In, GR, Out
constexpr double kBarWidth = 4.0, kBarPitch = 5.0;
constexpr double kHandleX = 322.0, kHandleSize = 8.0;  // the line's handle: a triangle pointing left
constexpr double kFadeWidth = 24.0;  // the history's oldest end fades out over this

QColor mix(const QColor& a, const QColor& b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    auto at = [t](float from, float to) { return float(from + (to - from) * t); };
    return QColor::fromRgbF(at(a.redF(), b.redF()), at(a.greenF(), b.greenF()), at(a.blueF(), b.blueF()),
                            at(a.alphaF(), b.alphaF()));
}

// A level as the readouts show it: one decimal, "−∞" for nothing at all.
QString levelText(double db) {
    if (db <= kDisplayFloorDb + 0.5)
        return QStringLiteral("−∞");
    return std::abs(db) < 0.05 ? QStringLiteral("0.0") : pythonFixed(db, 1);  // (no "-0.0")
}

}  // namespace

LimiterGraph::LimiterGraph(QQuickItem* parent) : DeviceCanvas(parent) {
    setImplicitSize(kWidth, kMinimumHeight);
    setAcceptedMouseButtons(Qt::LeftButton);
    setAcceptHoverEvents(true);
    for (auto& ring : rings_)
        ring.assign(kRing, 0.0f);
    for (auto& ring : outDbfs_)
        ring.assign(kRing, float(kDisplayFloorDb));
    for (int c = 0; c < 2; ++c) {
        meterIn_[std::size_t(c)].reset(kFloorDb);
        meterOut_[std::size_t(c)].reset(kFloorDb);
        meterGr_[std::size_t(c)].reset(0.0);
    }
    meterClip_.reset(0.0);
    softKneeDb_ = sub::app::limiterSoftKneeDb();
    softTopDb_ = sub::app::limiterSoftTopDb();
    line_.snap(lineDb_);
    updateTexts();
}

// --- Geometry ----------------------------------------------------------------------------------

QRectF LimiterGraph::plot() const { return QRectF(6, 24, 246, std::max(10.0, height() - 40)); }

double LimiterGraph::yOf(double db) const { return dbToY(db, plot(), kFloorDb, kTopDb); }

double LimiterGraph::grY(double reductionDb) const {
    const QRectF r = plot();
    const double gr = std::isfinite(reductionDb) ? std::clamp(reductionDb, 0.0, kGrRangeDb) : 0.0;
    return r.top() + gr / kGrRangeDb * r.height();
}

double LimiterGraph::valuesPerSecond() const { return sampleRate() / sub::app::limiterMeterSamples(); }

double LimiterGraph::valuesPerColumn() const { return valuesPerSecond() * kHistorySeconds / plot().width(); }

QString LimiterGraph::lineParam() const { return maximize_ ? QStringLiteral("threshold") : QStringLiteral("ceiling"); }

void LimiterGraph::geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) {
    DeviceCanvas::geometryChange(newGeometry, oldGeometry);
    if (newGeometry.size() != oldGeometry.size()) {
        historyDirty_ = true;
        requestPaint();
    }
}

// --- The parameters -----------------------------------------------------------------------------

void LimiterGraph::sync() {
    if (!alive()) {  // (no device yet, or no longer: nothing to show; the first sync with one shows it as it is)
        synced_ = false;
        return;
    }
    const bool maximize = value(QStringLiteral("maximize")) >= 0.5;
    const sub::app::LimiterLine line =
        sub::app::limiterLine(maximize, value(QStringLiteral("gain")), value(QStringLiteral("ceiling")),
                              value(QStringLiteral("threshold")), value(QStringLiteral("output")));
    const int mode = std::clamp(int(std::lround(value(QStringLiteral("mode")))), 0, 2);
    const int routing = value(QStringLiteral("routing")) >= 0.5 ? 1 : 0;
    const bool lineMoved = maximize != maximize_ || line.lineDb != lineDb_;
    const bool changed = !synced_ || lineMoved || mode != mode_ || routing != routing_ ||
                         line.outputShiftDb != outputShift_;
    maximize_ = maximize;
    lineDb_ = line.lineDb;
    outputShift_ = line.outputShiftDb;
    mode_ = mode;
    routing_ = routing;
    if (mode_ == 1)
        badgeText_ = QStringLiteral("SOFT CLIP");
    else if (mode_ == 2)
        badgeText_ = QStringLiteral("TRUE PEAK");
    soft_.target = mode_ == 1 ? 1.0 : 0.0;
    badge_.target = mode_ != 0 ? 1.0 : 0.0;
    maxBadge_.target = maximize_ ? 1.0 : 0.0;
    if (!synced_) {  // (as it is, without animating in)
        soft_.snap(soft_.target);
        badge_.snap(badge_.target);
        maxBadge_.snap(maxBadge_.target);
    }
    if (!synced_ || dragging())
        line_.snap(lineDb_);
    else
        line_.target = lineDb_;
    synced_ = true;
    if (lineMoved)
        Q_EMIT lineChanged();
    if (changed) {
        updateTexts();
        requestPaint();
    }
}

// --- The displays -------------------------------------------------------------------------------

void LimiterGraph::refreshDisplays() {
    static const QString kIds[kStreams] = {QStringLiteral("in_l"),  QStringLiteral("in_r"), QStringLiteral("out_l"),
                                           QStringLiteral("out_r"), QStringLiteral("gr_a"), QStringLiteral("gr_b"),
                                           QStringLiteral("clip")};
    for (int s = 0; s < kStreams; ++s) {
        auto [start, values] = readDisplayAt(kIds[s]);
        if (values.empty())
            continue;
        std::vector<float>& pending = pending_[std::size_t(s)];
        qint64& pendingStart = pendingStart_[std::size_t(s)];
        if (pending.empty() || start != pendingStart + qint64(pending.size())) {
            pending.assign(values.begin(), values.end());
            pendingStart = start;
        } else {
            pending.insert(pending.end(), values.begin(), values.end());
        }
        if (pending.size() > std::size_t(kRing)) {  // (a stream that came alone: only the latest matter)
            const std::size_t drop = pending.size() - std::size_t(kRing);
            pending.erase(pending.begin(), pending.begin() + std::ptrdiff_t(drop));
            pendingStart += qint64(drop);
        }
    }

    // What every stream has: the values for the same indices, in step.
    qint64 from = std::numeric_limits<qint64>::min(), to = std::numeric_limits<qint64>::max();
    for (int s = 0; s < kStreams; ++s) {
        from = std::max(from, pendingStart_[std::size_t(s)]);
        to = std::min(to, pendingStart_[std::size_t(s)] + qint64(pending_[std::size_t(s)].size()));
    }
    const bool arrived = to > from;
    if (arrived) {
        if (from != end_) {
            if (from > end_ && end_ > begin_ && from - end_ < kRing) {
                for (qint64 i = end_; i < from; ++i) {  // a gap (the reader fell behind): silence
                    for (int s = 0; s < kStreams; ++s)
                        rings_[std::size_t(s)][std::size_t(i & (kRing - 1))] =
                            s <= OutR ? float(kDisplayFloorDb) : 0.0f;
                    for (auto& ring : outDbfs_)
                        ring[std::size_t(i & (kRing - 1))] = float(kDisplayFloorDb);
                }
            } else {  // a new start (the device's processor made again): the history begins again
                begin_ = from;
                lastLoud_ = -1;
            }
        }
        float in = float(kDisplayFloorDb), out = float(kDisplayFloorDb), gr = 0.0f, clip = 0.0f;
        for (qint64 i = from; i < to; ++i) {
            float v[kStreams];
            for (int s = 0; s < kStreams; ++s) {
                const float raw = pending_[std::size_t(s)][std::size_t(i - pendingStart_[std::size_t(s)])];
                v[s] = std::isfinite(raw) ? raw : (s <= OutR ? float(kDisplayFloorDb) : 0.0f);
            }
            in = std::max({in, v[InL], v[InR]});
            out = std::max({out, v[OutL], v[OutR]});
            outDbfs_[0][std::size_t(i & (kRing - 1))] = v[OutL];
            outDbfs_[1][std::size_t(i & (kRing - 1))] = v[OutR];
            gr = std::max({gr, v[GrA], v[GrB]});
            clip = std::max(clip, v[Clip]);
            // The history's output in the line's domain, converted as it comes, so it stays put when the
            // parameters change (the Out meter and its figure keep the dBFS).
            v[OutL] = std::max(float(kDisplayFloorDb), v[OutL] - float(outputShift_));
            v[OutR] = std::max(float(kDisplayFloorDb), v[OutR] - float(outputShift_));
            for (int s = 0; s < kStreams; ++s)
                rings_[std::size_t(s)][std::size_t(i & (kRing - 1))] = v[s];
            if (std::max(v[InL], v[InR]) > kFloorDb || std::max(v[OutL], v[OutR]) > kFloorDb || v[GrA] > 0.0f ||
                v[GrB] > 0.0f || v[Clip] > 0.0f)
                lastLoud_ = i;
        }
        end_ = to;
        begin_ = std::max(begin_, end_ - kRing);
        for (int s = 0; s < kStreams; ++s) {
            std::vector<float>& pending = pending_[std::size_t(s)];
            const qint64 used = std::clamp<qint64>(to - pendingStart_[std::size_t(s)], 0, qint64(pending.size()));
            pending.erase(pending.begin(), pending.begin() + std::ptrdiff_t(used));
            pendingStart_[std::size_t(s)] += used;
        }
        levelIn_ = in;
        levelOut_ = out;
        reduction_ = gr;
        clipping_ = clip;
        historyDirty_ = true;
    }

    double seconds = 1.0 / 60;
    if (clock_.isValid())
        seconds = double(clock_.restart()) / 1000.0;
    else
        clock_.start();
    advance(seconds);
    if (arrived)
        Q_EMIT levelsChanged();
}

float LimiterGraph::mostDbfs(int channel, qint64 from, qint64 to) const {
    from = std::max(from, begin_);
    to = std::min(to, end_);
    float m = float(kDisplayFloorDb);
    for (qint64 i = from; i < to; ++i) {
        for (int c = 0; c < 2; ++c) {
            if (channel < 0 || channel == c)
                m = std::max(m, outDbfs_[std::size_t(c)][std::size_t(i & (kRing - 1))]);
        }
    }
    return m;
}

float LimiterGraph::most(int stream, int pair, qint64 from, qint64 to, float otherwise) const {
    from = std::max(from, begin_);
    to = std::min(to, end_);
    if (from >= to)
        return otherwise;
    float m = -std::numeric_limits<float>::infinity();
    for (qint64 i = from; i < to; ++i) {
        m = std::max(m, ring(stream, i));
        if (pair >= 0)
            m = std::max(m, ring(pair, i));
    }
    return m;
}

// --- Animation ----------------------------------------------------------------------------------

void LimiterGraph::advance(double seconds) {
    const double dt = std::clamp(seconds, 0.0, 0.1);
    const double vps = valuesPerSecond();
    bool changed = false;

    // The history scrolls on at a steady speed, easing towards kLagSeconds behind the newest value
    // (the same catch-up at any tick rate), snapping when far off, never past the newest.
    const double before = cursor_;
    if (end_ > begin_ && vps > 0.0) {
        cursor_ += dt * vps;
        const double target = double(end_) - kLagSeconds * vps;
        cursor_ += easeFraction(dt, kScrollEase) * (target - cursor_);
        if (std::abs(target - cursor_) > 0.25 * vps)
            cursor_ = target;
        cursor_ = std::min(cursor_, double(end_));
    }
    const bool scrolled = cursor_ != before;
    if (scrolled)
        historyDirty_ = true;

    // The meters read what the cursor passed this tick (the history's newest edge).
    const qint64 hi = qint64(std::floor(cursor_));
    const qint64 lo = cursor_ >= before && cursor_ - before <= 0.1 * vps ? qint64(std::floor(before))
                                                                          : qint64(std::floor(cursor_ - dt * vps));
    auto meter = [&](MeterBallistics& m, double db, double fall, double hold, double floor) {
        const double level = m.level, peak = m.peak;
        m.update(db, dt, fall, hold, floor);
        changed = changed || m.level != level || m.peak != peak;
    };
    for (int c = 0; c < 2; ++c) {
        const std::size_t i = std::size_t(c);
        meter(meterIn_[i], most(InL + c, -1, lo, hi, float(kDisplayFloorDb)), kLevelFall, kLevelHold, kFloorDb);
        meter(meterOut_[i], mostDbfs(c, lo, hi), kLevelFall, kLevelHold, kFloorDb);
        meter(meterGr_[i], most(GrA + c, -1, lo, hi, 0.0f), kReductionFall, kReductionHold, 0.0);
    }
    meter(meterClip_, most(Clip, -1, lo, hi, 0.0f), kReductionFall, kReductionHold, 0.0);

    if (!dragging() && line_.step(easeFraction(dt, kLineEase))) {
        changed = true;
        updateHover();  // (the line moved under a still mouse)
    }
    changed = soft_.step(easeFraction(dt, kBadgeEase)) || changed;
    changed = badge_.step(easeFraction(dt, kBadgeEase)) || changed;
    changed = maxBadge_.step(easeFraction(dt, kBadgeEase)) || changed;
    glow_.target = std::clamp(std::max(meterGr_[0].level, meterGr_[1].level) / kGlowFullDb, 0.0, 1.0);
    changed = glow_.step(easeFraction(dt, kGlowEase), 1e-3) || changed;
    changed = hover_.step(easeFraction(dt, kHoverEase), 1e-3) || changed;
    changed = updateTextsChanged() || changed;
    grTint_.target = grFigure_ >= 0.05 ? 1.0 : 0.0;
    changed = grTint_.step(easeFraction(dt, kBadgeEase), 1e-3) || changed;

    // The history only repaints while what it shows moves: not once everything in view is silent.
    if (historyDirty_ && lastLoud_ >= 0) {
        const double span = plot().width() * valuesPerColumn();
        if (double(lastLoud_) >= std::min(before, cursor_) - span - 2.0 * valuesPerColumn())
            changed = true;
    }
    if (changed)
        requestPaint();
}

bool LimiterGraph::updateTextsChanged() {
    const QString gr = grText_, in = inText_, grPeak = grPeakText_, out = outText_;
    updateTexts();
    return gr != grText_ || in != inText_ || grPeak != grPeakText_ || out != outText_;
}

void LimiterGraph::updateTexts() {
    const double vps = valuesPerSecond();
    const qint64 at = qint64(std::floor(cursor_));
    // The gain reduction, Soft Clip's share with it (as the GR bars stack it), over the last half second
    // at the bottom left (so it reads steadily) and the last second under the GR meters.
    const auto reduction = [&](qint64 from) {
        double gr = 0.0;
        for (qint64 i = std::max(begin_, from); i < std::min(at, end_); ++i)
            gr = std::max(gr, double(std::max(ring(GrA, i), ring(GrB, i)) + ring(Clip, i)));
        return gr;
    };
    grFigure_ = reduction(at - qint64(0.5 * vps));
    grText_ = grFigure_ >= 0.05 ? QStringLiteral("GR −%1 dB").arg(pythonFixed(grFigure_, 1))
                                : QStringLiteral("GR 0.0 dB");
    // The meters' peaks over the last second: In in the line's domain, GR negative as the footer's (with the
    // narrow minus of the figures beside it), Out in dBFS.
    const qint64 second = at - qint64(vps);
    inText_ = levelText(most(InL, InR, second, at, float(kDisplayFloorDb)));
    outText_ = levelText(mostDbfs(-1, second, at));
    const double gr = reduction(second);
    grPeakText_ = gr >= 0.05 ? pythonFixed(-gr, 1) : QStringLiteral("0.0");
}

void LimiterGraph::requestPaint() {
    if (historyDirty_)
        rebuildHistory();
    ++updates_;
    update();
}

// The history's columns: bin k holds the most of the values [k vpc, (k + 1) vpc) (absolute indices),
// so a column never changes once complete, whatever the scroll; its centre is drawn at
// plot.right - (cursor / vpc - k - 0.5): the whole plot scrolls by subpixels at a steady speed.
void LimiterGraph::rebuildHistory() {
    historyDirty_ = false;
    inPoints_.clear();
    outPoints_.clear();
    grPoints_.clear();
    clipPoints_.clear();
    grTops_.clear();
    clipBottoms_.clear();
    grRuns_.clear();
    anyClip_ = false;
    const QRectF r = plot();
    const double vpc = valuesPerColumn();
    if (!(vpc > 0.0) || end_ <= begin_)
        return;
    const double last = cursor_ / vpc;
    // (the newest column is the one the cursor is in, and holds a value)
    const qint64 first = qint64(std::floor(last - r.width() - 1.5));
    qint64 final = qint64(std::floor(last - 1e-9));
    while (final > first && qint64(std::ceil(double(final) * vpc)) >= end_)
        --final;
    binX0_ = r.right() - (last - double(first));
    int runStart = -1;
    for (qint64 k = first; k <= final; ++k) {
        const qint64 a = qint64(std::ceil(double(k) * vpc)), b = qint64(std::ceil(double(k + 1) * vpc));
        const double x = r.right() - (last - double(k) - 0.5);
        const double in = most(InL, InR, a, b, float(kDisplayFloorDb));
        const double out = most(OutL, OutR, a, b, float(kDisplayFloorDb));
        const double gr = most(GrA, GrB, a, b, 0.0f);
        const double clip = most(Clip, -1, a, b, 0.0f);
        inPoints_.emplace_back(x, yOf(in));
        outPoints_.emplace_back(x, yOf(out));
        grPoints_.emplace_back(x, grY(gr));
        clipPoints_.emplace_back(x, grY(gr + clip));
        grTops_.push_back(float(grY(gr)));
        clipBottoms_.push_back(float(grY(gr + clip)));
        anyClip_ = anyClip_ || clip > 0.0;
        // The reduction's edge is stroked only where there is some (and a column either side).
        const int index = int(grPoints_.size()) - 1;
        if (gr > 0.0 || clip > 0.0) {
            if (runStart < 0)
                runStart = std::max(0, index - 1);
        } else if (runStart >= 0) {
            grRuns_.emplace_back(runStart, index + 1);
            runStart = -1;
        }
    }
    if (runStart >= 0)
        grRuns_.emplace_back(runStart, int(grPoints_.size()));
}

// --- Painting -----------------------------------------------------------------------------------

void LimiterGraph::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const double w = width(), h = height();
    const QRectF outer = QRectF(0, 0, w, h).adjusted(0.5, 0.5, -0.5, -0.5);
    p.fillRoundedRect(outer, 5, 5, Theme::kMeterBg);
    p.drawRoundedRect(outer, 5, 5, withAlpha(Theme::kGridBeat, 150));

    const QRectF r = plot();
    const QFont font7 = uiFont(7), font8 = uiFont(8);
    const double ly = lineY();

    // The level's grid (0 dB a little brighter), which the reduction's 6, 12 and 18 dB share.
    for (const double db : {12.0, 0.0, -12.0, -24.0}) {
        const double y = yOf(db);
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y), withAlpha(Theme::kGridBeat, db == 0.0 ? 255 : 150));
    }

    // Soft Clip's knee: where it starts rounding off up to where it reaches the line.
    if (soft_.value > 0.004) {
        const double top = yOf(line_.value + softTopDb_), bottom = yOf(line_.value + softKneeDb_);
        QLinearGradient band(QPointF(0, bottom), QPointF(0, top));
        band.setColorAt(0, withAlpha(Theme::kAccent, 0));
        band.setColorAt(1, withAlpha(Theme::kAccent, int(45 * soft_.value)));
        p.fillRect(QRectF(r.left(), top, r.width(), bottom - top), band);
        drawDashedPolyline(p, {QPointF(r.left(), top), QPointF(r.right(), top)},
                           withAlpha(Theme::kAccent, int(90 * soft_.value)), 1.0);
    }

    // The history: the input (red over the line), the output inside it, the gain reduction from the top.
    const int n = int(inPoints_.size());
    if (n >= 2) {
        p.save();
        p.setClipRect(r);
        p.fillToBaseline(inPoints_.data(), n, r.bottom(), withAlpha(Theme::kTextDim, 85));
        if (ly > r.top()) {
            p.save();
            p.setClipRect(QRectF(r.left(), r.top(), r.width(), ly - r.top()));
            p.fillToBaseline(inPoints_.data(), n, r.bottom(), withAlpha(Theme::kMeterHigh, 130));
            p.restore();
        }
        p.fillToBaseline(outPoints_.data(), n, r.bottom(), withAlpha(Theme::kText, 40));
        p.drawPolyline(outPoints_.data(), n, withAlpha(Theme::kText, 95), 1.0);
        if (!grRuns_.empty()) {
            QLinearGradient reduction(QPointF(0, r.top()), QPointF(0, r.bottom()));
            reduction.setColorAt(0, withAlpha(Theme::kAccent, 30));
            reduction.setColorAt(1, withAlpha(Theme::kAccent, 150));
            p.fillToBaseline(grPoints_.data(), n, r.top(), reduction);
            if (anyClip_) {  // Soft Clip's share: what the knee rounded off rather than turned down
                p.fillBand(binX0_, 1.0, grTops_.data(), clipBottoms_.data(), n, withAlpha(Theme::kAccent, 60));
                p.drawPolyline(clipPoints_.data(), n, withAlpha(Theme::kAccent, 130), 1.0);
            }
            for (const auto& [from, to] : grRuns_) {
                const std::vector<QPointF> run(grPoints_.begin() + from, grPoints_.begin() + to);
                drawGlowPolyline(p, run, Theme::kAccent, 1.25);
            }
        }
        // The oldest end fades into the well.
        QLinearGradient fade(QPointF(r.left(), 0), QPointF(r.left() + kFadeWidth, 0));
        fade.setColorAt(0, Theme::kMeterBg);
        fade.setColorAt(1, withAlpha(Theme::kMeterBg, 0));
        p.fillRect(QRectF(r.left(), r.top(), kFadeWidth, r.height()), fade);
        p.restore();
    }
    // The level's figures, over the history (dim), left.
    for (const double db : {12.0, 0.0, -12.0, -24.0}) {
        p.drawText(QRectF(r.left() + 3, yOf(db) + 1, 30, 10), Qt::AlignLeft | Qt::AlignTop,
                   db > 0 ? QStringLiteral("+%1").arg(int(db)) : QString::number(int(db)),
                   withAlpha(Theme::kTextDim, 190), font7);
    }

    // The meters, in wells: In and Out on the level axis (In red over the line, as the history), GR on
    // the reduction's (the knee's share under it).
    for (const double x : kMeterX)
        p.fillRoundedRect(QRectF(x - 2, r.top() - 2, 2 * kBarWidth + 1 + 4, r.height() + 4), 2, 2, Theme::kPanel);
    for (int c = 0; c < 2; ++c) {
        const std::size_t i = std::size_t(c);
        const QRectF in(kMeterX[0] + c * kBarPitch, r.top(), kBarWidth, r.height());
        drawLevelMeter(p, in, meterIn_[i].level, meterIn_[i].peak, kFloorDb, kTopDb);
        const double top = yOf(meterIn_[i].level);
        if (top < ly)
            p.fillRect(QRectF(in.left(), top, in.width(), std::min(ly, in.bottom()) - top), Theme::kMeterHigh);
        const QRectF gr(kMeterX[1] + c * kBarPitch, r.top(), kBarWidth, r.height());
        drawReductionMeter(p, gr, meterGr_[i].level, kGrRangeDb);
        if (meterClip_.level > 0.0 && meterGr_[i].level < kGrRangeDb) {
            const double from = grY(meterGr_[i].level), to = grY(meterGr_[i].level + meterClip_.level);
            p.fillRect(QRectF(gr.left(), from, gr.width(), to - from), withAlpha(Theme::kAccent, 90));
        }
        const QRectF out(kMeterX[2] + c * kBarPitch, r.top(), kBarWidth, r.height());
        drawLevelMeter(p, out, meterOut_[i].level, meterOut_[i].peak, kFloorDb, kTopDb);
    }

    // The line, across the plot and the meters, warming with the gain reduction; its handle.
    const QColor lineColor = mix(Theme::kText, Theme::kAccent, glow_.value);
    drawGlowPolyline(p, {QPointF(r.left(), ly), QPointF(kHandleX, ly)}, lineColor, 1.5 + hover_.value);
    const QPointF handle[3] = {{kHandleX, ly}, {kHandleX + kHandleSize, ly - kHandleSize / 2},
                               {kHandleX + kHandleSize, ly + kHandleSize / 2}};
    p.fillPolygon(handle, 3, mix(Theme::kAccent, QColor(255, 236, 200), 0.6 * hover_.value));

    // The reduction's figures beside its meters, over the line (which passes behind them: at the
    // default ceiling it runs right through the 6).
    for (const double db : {6.0, 12.0, 18.0}) {
        const QRectF box(kMeterX[1] - 16, grY(db) - 5, 13, 10);
        p.fillRect(box, Theme::kMeterBg);
        p.drawText(box, Qt::AlignRight | Qt::AlignVCenter, QString::number(int(db)), withAlpha(Theme::kAccent, 140),
                   font7);
    }

    // The header: the meters' names (GR's channels by Routing), the mode's badge and MAX.
    const char* names[3] = {"In", "GR", "Out"};
    for (int m = 0; m < 3; ++m) {
        const double cx = kMeterX[m] + (kBarPitch + kBarWidth) / 2;
        p.drawText(QRectF(cx - 12, 2, 24, 11), Qt::AlignCenter, QString::fromLatin1(names[m]), Theme::kTextDim,
                   font7);
        const QString channels = m == 1 && routing_ == 1 ? QStringLiteral("M S") : QStringLiteral("L R");
        p.drawText(QRectF(cx - 12, 11, 24, 10), Qt::AlignCenter, channels, Theme::kTextDisabled, font7);
    }
    const auto pill = [&](const QString& text, double rightEdge, double opacity) {
        const double tw = SgPainter::textWidth(text, font7) + 10;
        const QRectF box(rightEdge - tw, 5, tw, 13);
        p.save();
        p.setOpacity(opacity);
        p.fillRoundedRect(box, 6.5, 6.5, withAlpha(Theme::kAccent, 30));
        p.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), 6, 6, withAlpha(Theme::kAccent, 170));
        p.drawText(box, Qt::AlignCenter, text, Theme::kAccent, font7);
        p.restore();
        return tw;
    };
    const double badgeWidth = badgeText_.isEmpty() ? 0.0 : SgPainter::textWidth(badgeText_, font7) + 10;
    if (badge_.value > 0.004 && !badgeText_.isEmpty())
        pill(badgeText_, r.right(), badge_.value);
    if (maxBadge_.value > 0.004)
        pill(QStringLiteral("MAX"), r.right() - badge_.value * (badgeWidth + 4), maxBadge_.value);

    // The footer: the gain reduction now, the meters' peaks.
    const double footer = h - 14;
    p.drawText(QRectF(r.left(), footer, 120, 12), Qt::AlignLeft | Qt::AlignVCenter, grText_,
               mix(Theme::kTextDim, Theme::kAccent, grTint_.value), font8);
    const QString* readouts[3] = {&inText_, &grPeakText_, &outText_};
    for (int m = 0; m < 3; ++m) {
        const double cx = kMeterX[m] + (kBarPitch + kBarWidth) / 2;
        p.drawText(QRectF(cx - 13, footer, 26, 12), Qt::AlignCenter, *readouts[m], Theme::kTextDim, font7);
    }
}

// --- The mouse ----------------------------------------------------------------------------------

bool LimiterGraph::nearLine(const QPointF& pos) const {
    const QRectF r = plot();
    return pos.x() >= r.left() && pos.x() <= kHandleX + kHandleSize + 2 && std::abs(pos.y() - lineY()) <= kLineGrab;
}

void LimiterGraph::mousePressEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    const bool second = secondPressOfDoubleClick(event);
    if (event->button() != Qt::LeftButton || !nearLine(pos)) {
        event->ignore();  // (the frame's: selecting the device)
        return;
    }
    if (second)
        return;  // (the double-click follows: it resets the line)
    hoverPos_ = pos;
    hovering_ = true;
    dragId_ = lineParam();
    touch(dragId_);
    gesture_ = newGestureKey();
    dragDb_ = value(dragId_);
    lastY_ = pos.y();
    line_.snap(lineDb_);
    hover_.target = 1.0;
    resizeCursor_ = true;
    setCursor(Qt::SizeVerCursor);
    Q_EMIT draggingChanged();
    requestPaint();
}

void LimiterGraph::mouseMoveEvent(QMouseEvent* event) {
    hoverPos_ = event->position();  // (no hover events while a button is held)
    if (!dragging())
        return;
    const double y = event->position().y();
    const double rate = (kTopDb - kFloorDb) / plot().height() * (event->modifiers() & Qt::ShiftModifier ? 0.25 : 1.0);
    dragDb_ = std::clamp(dragDb_ + (lastY_ - y) * rate, -24.0, 0.0);
    lastY_ = y;
    const double rounded = std::round(dragDb_ * 10.0) / 10.0 + 0.0;  // (+ 0.0: no "-0")
    if (rounded != value(dragId_))
        setParam(dragId_, rounded, gesture_);
    line_.snap(lineDb_);
    requestPaint();
}

void LimiterGraph::mouseReleaseEvent(QMouseEvent* event) {
    hoverPos_ = event->position();
    endDrag();
}

void LimiterGraph::mouseUngrabEvent() { endDrag(); }

void LimiterGraph::endDrag() {
    if (!dragging())
        return;
    gesture_.clear();
    Q_EMIT draggingChanged();
    updateHover();
}

void LimiterGraph::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton || !nearLine(event->position())) {
        event->ignore();
        return;
    }
    endDrag();
    const QString id = lineParam();
    setParam(id, defaultValue(id));  // (a step of its own)
}

void LimiterGraph::hoverMoveEvent(QHoverEvent* event) {
    hoverPos_ = event->position();
    hovering_ = true;
    updateHover();
}

void LimiterGraph::hoverLeaveEvent(QHoverEvent*) {
    hovering_ = false;
    updateHover();
}

// The line lit and the resize cursor while the mouse is over the line: checked as the mouse moves and
// as the line moves under it (eased there by a double-click, undo, automation or Maximize).
void LimiterGraph::updateHover() {
    if (dragging())
        return;
    const bool near = hovering_ && nearLine(hoverPos_);
    hover_.target = near ? 1.0 : 0.0;
    if (near == resizeCursor_)
        return;
    resizeCursor_ = near;
    if (near)
        setCursor(Qt::SizeVerCursor);
    else
        unsetCursor();
}

}  // namespace sub::ui
