#include "pianoroll/ClipWaveform.h"

#include "audio/EngineBridge.h"
#include "audio/Waveform.h"
#include "model/Numbers.h"
#include "model/Timebase.h"
#include "sg/SgPainter.h"
#include "theme/Theme.h"

#include <QFontMetrics>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace sub::ui {

namespace {

constexpr std::array<double, 19> kTimeSteps{0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1,
                                            2,     5,     10,    15,   30,   60,   120, 300,  600};

// A gain (linear) to 0.1 dB: steps that look the same, and a quiet clip's
// waveform stays as small as it is (never flat, as rounding the linear gain
// would make it). (waveform_cache.quantized_gain)
double quantizedGain(double gain) {
    if (gain <= 0.0) return 0.0;
    return std::pow(10.0, app::roundHalfEven(20.0 * std::log10(gain) * 10.0) / 10.0 / 20.0);
}

// A source's waveform over `area`, its whole length fitted to the width, a
// column per pixel: each column's minimum and maximum (from the source's
// peaks, or its samples when zoomed in that far), scaled by `gain`, lightly
// blurred when from peaks; both channels apart when `split` (and stereo). As
// waveform_cache.render_tile drew it, without its per-row antialiasing.
void drawWaveform(SgPainter& p, const app::Waveform& source, const QRectF& area, const QColor& color, bool split,
                  double gain) {
    const qint64 frames = source.frames();
    const int columns = static_cast<int>(std::ceil(area.width()));
    const int height = static_cast<int>(area.height());
    const int channels = std::max(1, source.channels());
    if (frames <= 0 || columns <= 0 || height < 2) return;
    const double fpp = static_cast<double>(frames) / std::max(1.0, area.width());

    int level = -1;
    for (int candidate = source.peakLevels() - 1; candidate >= 0; --candidate) {
        if (app::Waveform::samplesPerPeak(candidate) <= fpp) {
            level = candidate;
            break;
        }
    }
    const qint64 n = level >= 0 ? source.peakCount(level) : frames;
    const double perIndex = level >= 0 ? app::Waveform::samplesPerPeak(level) : 1.0;
    const float* peaks = level >= 0 ? source.peaks(level) : nullptr;
    if (n <= 0) return;

    // Columns -1 .. columns: one padding column each side for the blur.
    const int padded = columns + 2;
    std::vector<float> lo(static_cast<size_t>(padded * channels)), hi(lo.size());
    std::vector<bool> valid(static_cast<size_t>(padded));
    for (int j = 0; j < padded; ++j) {
        const double e0 = std::max(0.0, (j - 1) * fpp), e1 = std::max(0.0, j * fpp);
        const auto i0 = static_cast<qint64>(std::floor(e0 / perIndex));
        const auto i1 = static_cast<qint64>(std::floor(e1 / perIndex));
        valid[static_cast<size_t>(j)] = i0 < n;
        const qint64 start = std::clamp<qint64>(i0, 0, n - 1);
        const qint64 end = std::max(std::clamp<qint64>(i1, 0, n), start + 1);
        for (int c = 0; c < channels; ++c) {
            float low = 0.0f, high = 0.0f;
            if (valid[static_cast<size_t>(j)]) {
                low = 1e9f;
                high = -1e9f;
                if (peaks) {
                    for (qint64 k = start; k < end; ++k) {
                        low = std::min(low, peaks[(k * channels + c) * 2]);
                        high = std::max(high, peaks[(k * channels + c) * 2 + 1]);
                    }
                } else {
                    const float* samples = source.channelData(c);
                    for (qint64 k = start; k < end; ++k) {
                        low = std::min(low, samples[k]);
                        high = std::max(high, samples[k]);
                    }
                }
                low *= static_cast<float>(gain);
                high *= static_cast<float>(gain);
            }
            lo[static_cast<size_t>(j * channels + c)] = low;
            hi[static_cast<size_t>(j * channels + c)] = high;
        }
    }
    // Envelopes (not raw samples) read smoother with a light blur.
    auto column = [&](const std::vector<float>& values, int x, int c) {
        const auto at = [&](int j) { return values[static_cast<size_t>(j * channels + c)]; };
        return peaks ? (at(x) + 2.0f * at(x + 1) + at(x + 2)) * 0.25f : at(x + 1);
    };

    struct Lane {
        int channel;  // -1: all channels together
        int top;
        int height;
    };
    std::vector<Lane> lanes;
    if (split && channels == 2) {
        const int half = height / 2;
        lanes = {{0, 0, half}, {1, half, height - half}};
    } else {
        lanes = {{-1, 0, height}};
    }
    std::vector<float> yTop(static_cast<size_t>(columns)), yBottom(yTop.size());
    for (const Lane& lane : lanes) {
        const double center = lane.top + lane.height / 2.0;
        const double half = std::max(1.0, lane.height / 2.0 - 1.0);
        const double laneTop = lane.top, laneBottom = lane.top + lane.height;
        for (int x = 0; x < columns; ++x) {
            float low = 0.0f, high = 0.0f;
            if (lane.channel >= 0) {
                low = column(lo, x, lane.channel);
                high = column(hi, x, lane.channel);
            } else {
                low = column(lo, x, 0);
                high = column(hi, x, 0);
                for (int c = 1; c < channels; ++c) {
                    low = std::min(low, column(lo, x, c));
                    high = std::max(high, column(hi, x, c));
                }
            }
            double top = std::clamp(center - std::clamp<double>(high, -1, 1) * half, laneTop, laneBottom);
            double bottom = std::clamp(center - std::clamp<double>(low, -1, 1) * half, laneTop, laneBottom);
            bottom = std::min(std::max(bottom, top + 1.0), laneBottom);  // at least a pixel thick
            top = std::min(top, bottom - 1.0);
            if (!valid[static_cast<size_t>(x + 1)]) top = bottom;  // past the end: nothing
            yTop[static_cast<size_t>(x)] = static_cast<float>(area.top() + top);
            yBottom[static_cast<size_t>(x)] = static_cast<float>(area.top() + bottom);
        }
        p.fillColumns(area.left(), 1.0, yTop.data(), yBottom.data(), columns, color);
    }
}

}  // namespace

ClipWaveform::ClipWaveform(QQuickItem* parent) : SgCanvas(parent) {}

void ClipWaveform::setSession(app::Session* session) {
    if (session == session_) return;
    if (session_) disconnect(session_->bridge(), nullptr, this, nullptr);
    session_ = session;
    if (session) {
        connect(session->bridge(), &app::EngineBridge::sourceReady, this, &QQuickItem::update);
        connect(session->bridge(), &app::EngineBridge::sourceFailed, this, &QQuickItem::update);
    }
    Q_EMIT sessionChanged();
    update();
}

void ClipWaveform::setController(ClipViewController* controller) {
    if (controller == controller_) return;
    if (controller_) disconnect(controller_, nullptr, this, nullptr);
    controller_ = controller;
    if (controller) connect(controller, &ClipViewController::changed, this, &QQuickItem::update);
    Q_EMIT controllerChanged();
    update();
}

QString ClipWaveform::formatTime(double seconds, double step) {
    const double minutes = std::floor(seconds / 60.0);
    const double secs = seconds - minutes * 60.0;
    const int decimals = step >= 1 ? 0 : std::min(3, static_cast<int>(std::ceil(-std::log10(step) - 1e-9)));
    QString text = app::formatFixed(secs, decimals);
    if (!minutes) return text;
    const int width = decimals ? decimals + 3 : 2;
    if (text.size() < width) text.prepend(QString(width - text.size(), u'0'));
    return QStringLiteral("%1:%2").arg(static_cast<long long>(minutes)).arg(text);
}

void ClipWaveform::paint(SgPainter& p) {
    const QRectF rect = p.rect();
    p.fillRect(rect, Theme::kLane);
    if (!controller_ || !session_) return;
    const auto& clips = controller_->audioClips();
    if (clips.empty()) return;
    if (clips.size() == 1) {
        const QRectF area(0, kRulerHeight, width(), height() - kRulerHeight);
        const double totalSec = drawBand(p, clips.front().first, clips.front().second, area);
        if (totalSec > 0) drawRuler(p, totalSec);
        return;
    }
    const double bandHeight = std::max<double>(kMinBandHeight, height() / std::max<size_t>(1, clips.size()));
    const size_t shown = std::min(clips.size(), static_cast<size_t>(std::max(1.0, std::floor(height() / bandHeight))));
    for (size_t i = 0; i < shown; ++i) {
        const QRectF band(0, static_cast<double>(i) * bandHeight, width(), bandHeight - 1);
        drawBand(p, clips[i].first, clips[i].second, band);
        drawLabel(p, band, clips[i].first.name);
        p.fillRect(QRectF(0, band.bottom(), width(), 1), Theme::kBorder);
    }
    if (shown < clips.size()) {
        p.drawText(rect.adjusted(0, 0, -8, -4), Qt::AlignRight | Qt::AlignBottom,
                   QStringLiteral("+%1 more").arg(clips.size() - shown), Theme::kText, uiFont(8));
    }
}

double ClipWaveform::drawBand(SgPainter& p, const app::Clip& clip, const QColor& color, const QRectF& area) const {
    const app::EngineBridge* bridge = session_->bridge();
    const app::Waveform source = bridge->waveform(clip.path);
    if (source.isNull() || source.frames() <= 0) {
        const bool missing = !bridge->loadError(clip.path).isEmpty();
        p.drawText(area, Qt::AlignCenter, missing ? QStringLiteral("Missing file") : QStringLiteral("Loading…"),
                   Theme::kTextDim, uiFont());
        return 0.0;
    }
    const double totalSec = static_cast<double>(source.frames()) / source.sampleRate();
    const auto xAt = [&](double seconds) { return area.left() + seconds / totalSec * area.width(); };

    // Clip region, waveform, then dim what the clip doesn't play.
    const double x0 = xAt(clip.offsetSec), x1 = xAt(clip.offsetSec + clip.durationSec);
    QColor region = color;
    region.setAlpha(45);
    p.fillRect(QRectF(x0, area.top(), x1 - x0, area.height()), region);
    const bool split = area.height() >= 60;
    std::vector<double> mids;
    if (split)
        mids = {area.top() + area.height() / 4, area.top() + area.height() * 3 / 4};
    else
        mids = {area.center().y()};
    for (double mid : mids) p.drawLine(QPointF(area.left(), mid), QPointF(area.right(), mid), Theme::kGridBeat);
    p.save();
    p.setClipRect(area);
    QColor opaque = color;
    opaque.setAlpha(255);
    drawWaveform(p, source, area, opaque, split, quantizedGain(app::dbToGain(clip.gainDb)));
    p.restore();
    const QColor dim(0, 0, 0, 120);
    p.fillRect(QRectF(area.left(), area.top(), x0 - area.left(), area.height()), dim);
    p.fillRect(QRectF(x1, area.top(), area.right() - x1, area.height()), dim);

    // Start and end markers with flags, as in Ableton's sample editor.
    const QFont font = uiFont(7.5, true);
    for (const auto& [x, label] : {std::pair{x0, QStringLiteral("S")}, std::pair{x1, QStringLiteral("E")}}) {
        const bool end = label == u"E";
        p.fillRect(QRectF(app::roundHalfEven(x) - (end ? 1 : 0), area.top(), 1, area.height()), Theme::kText);
        const QRectF flag(end ? x - 12 : x, area.top(), 12, 12);
        p.fillRect(flag, Theme::kText);
        p.drawText(flag, Qt::AlignCenter, label, Theme::kAccentText, font);
    }
    return totalSec;
}

void ClipWaveform::drawLabel(SgPainter& p, const QRectF& band, const QString& name) const {
    const QFont font = uiFont(8);
    const QFontMetrics metrics(font);
    const QString text = metrics.elidedText(name, Qt::ElideRight, static_cast<int>(band.width() / 2));
    const QRectF box(band.left() + 4, band.bottom() - metrics.height() - 5, metrics.horizontalAdvance(text) + 8,
                     metrics.height() + 2);
    p.fillRect(box, QColor(0, 0, 0, 150));
    p.drawText(box, Qt::AlignCenter, text, Theme::kText, font);
}

void ClipWaveform::drawRuler(SgPainter& p, double totalSec) const {
    p.fillRect(QRectF(0, 0, width(), kRulerHeight), Theme::kPanel);
    const double pxPerSec = width() / totalSec;
    double step = kTimeSteps.back();
    for (double s : kTimeSteps) {
        if (s * pxPerSec >= 70) {
            step = s;
            break;
        }
    }
    const QFont font = uiFont(7.5);
    const auto ticks = static_cast<long long>(totalSec / step);
    for (long long i = 0; i <= ticks; ++i) {
        const double seconds = static_cast<double>(i) * step;
        const double x = app::roundHalfEven(seconds * pxPerSec);
        p.fillRect(QRectF(x, kRulerHeight - 6, 1, 6), Theme::kTextDim);
        p.drawText(QPointF(x + 3, kRulerHeight - 7), formatTime(seconds, step), Theme::kTextDim, font);
    }
    p.fillRect(QRectF(0, kRulerHeight - 1, width(), 1), Theme::kBorder);  // the 1 px line at 19.5
}

}  // namespace sub::ui
