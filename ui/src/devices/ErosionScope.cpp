#include "devices/ErosionScope.h"

#include "audio/ErosionResponse.h"
#include "devices/ErosionGraph.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sub::ui {

namespace {

constexpr int kPieces = 6;               // the trace's pieces, oldest (faintest) first
constexpr int kCloudPairs = 2048;        // the cloud is the RMS of the latest this many pairs (43 ms at 48 kHz),
constexpr double kCloudSeconds = 0.056;  // eased with this time constant

}  // namespace

ErosionScope::ErosionScope(QQuickItem* parent)
    : DeviceCanvas(parent), left_(size_t(kRing), 0.0f), right_(size_t(kRing), 0.0f) {
    setImplicitSize(kSize, kSize);
    setAcceptedMouseButtons(Qt::NoButton);  // (clicks go on to the frame)
}

double ErosionScope::spread() const {
    double mid = 0.0, side = 0.0, mid2 = 0.0, side2 = 0.0;
    for (int i = 0; i < count_; ++i) {
        const double m = 0.5 * (double(left_[size_t(i)]) + right_[size_t(i)]);
        const double s = 0.5 * (double(left_[size_t(i)]) - right_[size_t(i)]);
        mid += m;
        side += s;
        mid2 += m * m;
        side2 += s * s;
    }
    if (count_ == 0)
        return 0.0;
    const double n = count_;
    const double midVariance = mid2 / n - (mid / n) * (mid / n);
    const double sideVariance = std::max(0.0, side2 / n - (side / n) * (side / n));
    return midVariance > 0.0 ? std::sqrt(sideVariance / midVariance) : 0.0;
}

void ErosionScope::sync() {
    amount_ = value(QStringLiteral("amount"));
    noiseWeight_ = sub::app::erosionBlendWeights(value(QStringLiteral("blend"))).second;
    settle_ = kSettleSeconds;  // (the trace catches up with the change: the engine glides to it)
    update();
}

void ErosionScope::take(Unpaired& unpaired, std::pair<qint64, std::vector<float>> read) {
    auto& [at, values] = read;
    if (values.empty())
        return;
    if (!unpaired.values.empty() && unpaired.at + qint64(unpaired.values.size()) == at) {
        unpaired.values.insert(unpaired.values.end(), values.begin(), values.end());
    } else {  // (the first read, or one after a gap: read too late to follow on, or from a new processor)
        unpaired.at = at;
        unpaired.values = std::move(values);
    }
    if (unpaired.values.size() > size_t(kRing)) {  // (only the latest can go in the ring)
        const size_t extra = unpaired.values.size() - size_t(kRing);
        unpaired.values.erase(unpaired.values.begin(), unpaired.values.begin() + std::ptrdiff_t(extra));
        unpaired.at += qint64(extra);
    }
}

void ErosionScope::refreshDisplays() {
    const double dt = tickSeconds();
    // The modulators, paired by their absolute index. They are read one after the other, so the second
    // read may hold values the engine published between the two: those wait for their partners.
    take(unpairedLeft_, readDisplayAt(QStringLiteral("mod_l")));
    take(unpairedRight_, readDisplayAt(QStringLiteral("mod_r")));
    const qint64 leftAt = unpairedLeft_.at, rightAt = unpairedRight_.at;
    const qint64 from = std::max(leftAt, rightAt);
    const qint64 to = std::min(leftAt + qint64(unpairedLeft_.values.size()),
                               rightAt + qint64(unpairedRight_.values.size()));
    for (qint64 i = std::max(from, to - kRing); i < to; ++i) {
        left_[size_t(head_)] = unpairedLeft_.values[size_t(i - leftAt)];
        right_[size_t(head_)] = unpairedRight_.values[size_t(i - rightAt)];
        head_ = (head_ + 1) % kRing;
        count_ = std::min(count_ + 1, kRing);
    }
    const bool came = to > from;
    // What was paired (or can't be any more: older than the other's) goes; the rest waits.
    for (Unpaired* unpaired : {&unpairedLeft_, &unpairedRight_}) {
        const qint64 end = unpaired->at + qint64(unpaired->values.size());
        const qint64 keep = std::max(unpaired->at, std::min(end, std::max(to, from)));
        unpaired->values.erase(unpaired->values.begin(),
                               unpaired->values.begin() + std::ptrdiff_t(keep - unpaired->at));
        unpaired->at = keep;
    }
    trail_ = std::max(2, int(std::lround(kTrailSeconds * sampleRate())));
    // The cloud's size: the mid's and the side's RMS over the latest pairs, eased.
    if (came) {
        const int n = std::min(count_, kCloudPairs);
        double mid2 = 0.0, side2 = 0.0;
        for (int k = 0; k < n; ++k) {
            const size_t i = size_t((head_ - 1 - k + kRing) % kRing);
            const double mid = 0.5 * (double(left_[i]) + right_[i]), side = 0.5 * (double(left_[i]) - right_[i]);
            mid2 += mid * mid;
            side2 += side * side;
        }
        midRms_.target = n > 0 ? std::sqrt(mid2 / n) : 0.0;
        sideRms_.target = n > 0 ? std::sqrt(side2 / n) : 0.0;
    }
    const double cloudEase = easeFraction(dt, kCloudSeconds);
    bool cloudMoved = midRms_.step(cloudEase, 1e-3);
    cloudMoved = sideRms_.step(cloudEase, 1e-3) || cloudMoved;

    // How much is being eroded now, as the graph has it: the newest values (the read may hold a backlog's).
    const double db = sub::app::erosionPeakDb(readRecent(QStringLiteral("erosion"), ErosionGraph::kRecentSeconds));
    const bool moved = ErosionGraph::easeActivity(activity_, db, dt);

    // Traced while it erodes, and for a moment after a change; in silence it holds still.
    const bool tracing = came && (activity_.value > 0.005 || settle_ > 0.0);
    if (came && settle_ > 0.0)
        settle_ = std::max(0.0, settle_ - dt);
    if (tracing || moved || (cloudMoved && settle_ > 0.0))
        update();
}

void ErosionScope::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QPointF c(width() / 2.0, height() / 2.0);
    const double radius = std::min(width(), height()) / 2.0 - 1.0;
    if (radius < 4.0)
        return;
    const QRectF well(c.x() - radius, c.y() - radius, 2 * radius, 2 * radius);
    p.fillEllipse(well, Theme::meterBg());
    p.drawEllipse(well, Theme::border(), 1.0);

    // The axes: left (up to the right) and right (up to the left) on the diagonals, mid upright.
    const QColor axis = withAlpha(Theme::gridBeat(), 160);
    const double d = (radius - 2.0) * std::sqrt(0.5);
    p.drawLine(QPointF(c.x() - d, c.y() + d), QPointF(c.x() + d, c.y() - d), axis);
    p.drawLine(QPointF(c.x() + d, c.y() + d), QPointF(c.x() - d, c.y() - d), axis);
    p.drawLine(QPointF(c.x(), c.y() - radius + 2.0), QPointF(c.x(), c.y() + radius - 2.0), axis);

    // The trace: the latest pairs, mid up and side across. Its radius eases towards the rim (tanh): a sine's
    // swing lands at 0.8 of it, and noise peaks round off into it instead of piling up on it.
    const int n = std::min(count_, trail_);
    if (n < 2)
        return;
    const double r = radius - 2.0;
    std::vector<QPointF> points(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) {
        const size_t i = size_t((head_ - n + k + kRing) % kRing);
        const double side = 0.5 * (double(left_[i]) - right_[i]);
        const double mid = 0.5 * (double(left_[i]) + right_[i]);
        const double length = std::hypot(side, mid);
        const double scale = length > 1e-9 ? std::tanh(kGain * length) / length * r : 0.0;
        points[size_t(k)] = QPointF(c.x() + side * scale, c.y() - mid * scale);
    }
    const double brightness = amount_ <= 0.0 ? 0.45 : 0.6 + 0.4 * activity_.value;
    const QColor color = mixColor(Theme::soloOn(), Theme::accent(), noiseWeight_);
    // Under it, the cloud the modulation fills (two RMS out): an upright sliver in mono, round at full Stereo.
    const auto reach = [&](double rms) { return std::max(1.0, std::tanh(kGain * 2.0 * rms) * r); };
    const QRectF cloud(c.x() - reach(sideRms_.value), c.y() - reach(midRms_.value), 2 * reach(sideRms_.value),
                       2 * reach(midRms_.value));
    p.fillEllipse(cloud, withAlpha(color, int(std::lround((24 + 32 * noiseWeight_) * brightness))));
    p.drawEllipse(cloud, withAlpha(color, int(std::lround((50 + 40 * noiseWeight_) * brightness))), 1.0);
    // The trace over it: a sine's is its shape (a circle, a line), noise's a scribble, kept fainter.
    const double traced = brightness * (1.0 - 0.5 * noiseWeight_);
    for (int piece = 0; piece < kPieces; ++piece) {
        const int first = piece * (n - 1) / kPieces;
        const int last = (piece + 1) * (n - 1) / kPieces;  // (shared with the next piece)
        if (last <= first)
            continue;
        const QPointF* from = points.data() + first;
        const int count = last - first + 1;
        if (piece == kPieces - 1)  // the newest: a glow under it
            p.drawPolyline(from, count, withAlpha(color, int(std::lround(36 * traced))), 3.0, Qt::RoundCap);
        const double alpha = (16.0 + 184.0 * (piece + 1) / kPieces) * traced;
        p.drawPolyline(from, count, withAlpha(color, int(std::lround(alpha))), 1.0, Qt::RoundCap);
    }
}

}  // namespace sub::ui
