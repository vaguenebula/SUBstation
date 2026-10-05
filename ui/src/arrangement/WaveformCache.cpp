#include "arrangement/WaveformCache.h"

#include "model/Numbers.h"
#include "sg/SgPainter.h"

#include <algorithm>
#include <cmath>

namespace sub::ui::arrangement {

double quantizedGain(double gain) {
    if (gain <= 0.0) return 0.0;
    return std::pow(10.0, app::roundHalfEven(20.0 * std::log10(gain) * 10.0) / 10.0 / 20.0);
}

std::shared_ptr<const WaveformCache::Tile> WaveformCache::renderTile(const app::Waveform& source,
                                                                     double framesPerPixel, int index, int height,
                                                                     bool splitChannels, double gain) {
    const qint64 frames = source.frames();
    const int channels = std::max(1, source.channels());
    const double fpp = framesPerPixel;
    // Column edges in source frames, with one padding column each side, so the
    // smoothing is seamless across tile borders: kTile + 3 edges, kTile + 2 columns.
    const auto edge = [&](int j) { return std::max(0.0, (static_cast<double>(index) * kTile - 1 + j) * fpp); };
    if (source.isNull() || frames <= 0 || fpp <= 0 || (static_cast<double>(index) * kTile) * fpp >= frames ||
        height < 2)
        return nullptr;

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
    if (n <= 0) return nullptr;

    constexpr int kPadded = kTile + 2;
    std::vector<float> lo(static_cast<size_t>(kPadded * channels)), hi(lo.size());
    std::vector<bool> valid(kPadded);
    for (int j = 0; j < kPadded; ++j) {
        const auto i0 = static_cast<qint64>(std::floor(edge(j) / perIndex));
        const auto i1 = static_cast<qint64>(std::floor(edge(j + 1) / perIndex));
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
    const auto column = [&](const std::vector<float>& values, int x, int c) {
        const auto at = [&](int j) { return values[static_cast<size_t>(j * channels + c)]; };
        return peaks ? (at(x) + 2.0f * at(x + 1) + at(x + 2)) * 0.25f : at(x + 1);
    };

    struct Lane {
        int channel;  // -1: all channels together
        int top;
        int height;
    };
    std::vector<Lane> lanes;
    if (splitChannels && channels == 2) {
        const int half = height / 2;
        lanes = {{0, 0, half}, {1, half, height - half}};
    } else {
        lanes = {{-1, 0, height}};
    }
    auto tile = std::make_shared<Tile>();
    for (const Lane& lane : lanes) {
        std::vector<float> tops(kTile), bottoms(kTile);
        const double center = lane.top + lane.height / 2.0;
        const double half = std::max(1.0, lane.height / 2.0 - 1.0);
        const double laneTop = lane.top, laneBottom = lane.top + lane.height;
        for (int x = 0; x < kTile; ++x) {
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
            tops[static_cast<size_t>(x)] = static_cast<float>(top);
            bottoms[static_cast<size_t>(x)] = static_cast<float>(bottom);
        }
        tile->tops.push_back(std::move(tops));
        tile->bottoms.push_back(std::move(bottoms));
    }
    return tile;
}

void WaveformCache::clear() {
    const std::lock_guard lock(mutex_);
    tiles_.clear();
    order_.clear();
}

int WaveformCache::size() const {
    const std::lock_guard lock(mutex_);
    return static_cast<int>(tiles_.size());
}

std::shared_ptr<const WaveformCache::Tile> WaveformCache::tile(const app::Waveform& source, double framesPerPixel,
                                                               int index, int height, bool split, double gain) {
    const Key key{source.path(), source.frames(), source.sampleRate(), app::roundHalfEven(framesPerPixel * 1e9) / 1e9,
                  index,         height,          split,               gain};
    const std::lock_guard lock(mutex_);
    if (const auto found = tiles_.find(key); found != tiles_.end()) {
        order_.splice(order_.end(), order_, found->second.second);  // the most recently used now
        return found->second.first;
    }
    std::shared_ptr<const Tile> made = renderTile(source, framesPerPixel, index, height, split, gain);
    order_.push_back(key);
    tiles_.emplace(key, std::make_pair(made, std::prev(order_.end())));
    while (static_cast<int>(tiles_.size()) > maxTiles_) {
        tiles_.erase(order_.front());
        order_.pop_front();
    }
    return made;
}

void WaveformCache::draw(SgPainter& p, const app::Waveform& source, const QRectF& body, double clipX,
                         double offsetSec, double framesPerPixel, const QColor& color, bool splitChannels,
                         const QRectF& visible, double gain) {
    const double x0 = std::max(body.left(), visible.left());
    const double x1 = std::min(body.right(), visible.right());
    if (x1 <= x0 || framesPerPixel <= 0 || source.isNull()) return;
    const double offset = offsetSec * source.sampleRate() / framesPerPixel;  // source pixels before the clip start
    const auto first = static_cast<int>(std::floor((x0 - clipX + offset) / kTile));
    const auto last = static_cast<int>(std::floor((x1 - clipX + offset) / kTile));
    const int height = static_cast<int>(body.height());
    const double quantized = quantizedGain(gain);
    for (int index = std::max(0, first); index <= last; ++index) {
        const auto made = tile(source, framesPerPixel, index, height, splitChannels, quantized);
        if (!made) break;
        const double x = app::roundHalfEven(clipX + index * kTile - offset);
        p.save();
        p.translate(x, body.top());
        for (size_t lane = 0; lane < made->tops.size(); ++lane)
            p.fillColumns(0.0, 1.0, made->tops[lane].data(), made->bottoms[lane].data(), kTile, color);
        p.restore();
    }
}

}  // namespace sub::ui::arrangement
