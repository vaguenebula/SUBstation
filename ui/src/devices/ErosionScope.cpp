#include "devices/ErosionScope.h"

#include "audio/ErosionResponse.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <algorithm>
#include <cmath>

namespace sub::ui {

namespace {

// As the graph's activity: the `erosion` display from -60 dB (0) over 48 dB (1), up quickly, down slowly.
constexpr double kActivityFloorDb = -60.0;
constexpr double kActivitySpanDb = 48.0;
constexpr double kActivityRise = 0.35;
constexpr double kActivityFall = 0.08;
constexpr int kPieces = 6;  // the trace's pieces, oldest (faintest) first
constexpr int kCloudPairs = 2048;  // the cloud is the RMS of the latest this many pairs (43 ms at 48 kHz),
constexpr double kCloudEase = 0.25;  // eased this share of the way per refresh

QColor mixed(const QColor& a, const QColor& b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    const auto mix = [t](float from, float to) { return float(from + (to - from) * t); };
    return QColor::fromRgbF(mix(a.redF(), b.redF()), mix(a.greenF(), b.greenF()), mix(a.blueF(), b.blueF()));
}

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
    settle_ = kSettleTicks;  // (the trace catches up with the change: the engine glides to it)
    update();
}

void ErosionScope::refreshDisplays() {
    // The modulators, paired by their absolute index (a value published between the two reads waits).
    const auto [leftAt, left] = readDisplayAt(QStringLiteral("mod_l"));
    const auto [rightAt, right] = readDisplayAt(QStringLiteral("mod_r"));
    const qint64 from = std::max(leftAt, rightAt);
    const qint64 to = std::min(leftAt + qint64(left.size()), rightAt + qint64(right.size()));
    for (qint64 i = std::max(from, to - kRing); i < to; ++i) {
        left_[size_t(head_)] = left[size_t(i - leftAt)];
        right_[size_t(head_)] = right[size_t(i - rightAt)];
        head_ = (head_ + 1) % kRing;
        count_ = std::min(count_ + 1, kRing);
    }
    const bool came = to > from;
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
    bool cloudMoved = midRms_.step(kCloudEase, 1e-3);
    cloudMoved = sideRms_.step(kCloudEase, 1e-3) || cloudMoved;

    double db = -90.0;
    for (const float value : readDisplay(QStringLiteral("erosion"))) {
        if (std::isfinite(value))
            db = std::max(db, double(value));
    }
    activity_.target = std::clamp((db - kActivityFloorDb) / kActivitySpanDb, 0.0, 1.0);
    const bool moved = activity_.step(activity_.target > activity_.value ? kActivityRise : kActivityFall);

    // Traced while it erodes, and for a moment after a change; in silence it holds still.
    const bool tracing = came && (activity_.value > 0.005 || settle_ > 0);
    if (came && settle_ > 0)
        --settle_;
    if (tracing || moved || (cloudMoved && settle_ > 0))
        update();
}

void ErosionScope::paint(SgPainter& p) {
    p.setAntialiasing(true);
    const QPointF c(width() / 2.0, height() / 2.0);
    const double radius = std::min(width(), height()) / 2.0 - 1.0;
    if (radius < 4.0)
        return;
    const QRectF well(c.x() - radius, c.y() - radius, 2 * radius, 2 * radius);
    p.fillEllipse(well, Theme::kMeterBg);
    p.drawEllipse(well, Theme::kBorder, 1.0);

    // The axes: left (up to the right) and right (up to the left) on the diagonals, mid upright.
    const QColor axis = withAlpha(Theme::kGridBeat, 160);
    const double d = (radius - 2.0) * std::sqrt(0.5);
    p.drawLine(QPointF(c.x() - d, c.y() + d), QPointF(c.x() + d, c.y() - d), axis);
    p.drawLine(QPointF(c.x() + d, c.y() + d), QPointF(c.x() - d, c.y() - d), axis);
    p.drawLine(QPointF(c.x(), c.y() - radius + 2.0), QPointF(c.x(), c.y() + radius - 2.0), axis);

    // The trace: the latest pairs, mid up and side across. Its radius eases towards the rim (tanh): a sine's
    // swing lands at 0.8 of it, and noise peaks round off into it instead of piling up on it.
    const int n = std::min(count_, std::max(2, int(std::lround(kTrailSeconds * sampleRate()))));
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
    const QColor color = mixed(Theme::kSoloOn, Theme::kAccent, noiseWeight_);
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
