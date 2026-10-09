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

WaveformOutline waveformOutline(const app::Waveform& source, double framesPerPixel, qint64 first, int count,
                                int height, bool splitChannels, double gain) {
    WaveformOutline outline;
    const qint64 frames = source.frames();
    const int channels = std::max(1, source.channels());
    const double fpp = framesPerPixel;
    if (source.isNull() || frames <= 0 || fpp <= 0 || count <= 0 || height < 2) return outline;
    // Points up to the last whose column's centre is in the source.
    const double centre = (static_cast<double>(first) + 0.5) * fpp;
    count = static_cast<int>(std::clamp(std::ceil((static_cast<double>(frames) - centre) / fpp), 0.0, double(count)));
    if (count <= 0) return outline;

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
    if (n <= 0) return outline;

    // Each point's minimum and maximum, per channel: of the frames from half a
    // column before its column to half a column after it.
    std::vector<float> lo(static_cast<size_t>(count * channels)), hi(lo.size());
    for (int j = 0; j < count; ++j) {
        const double column = static_cast<double>(first + j);
        const auto i0 = static_cast<qint64>(std::floor((column - 0.5) * fpp / perIndex));
        const auto i1 = static_cast<qint64>(std::floor((column + 1.5) * fpp / perIndex));
        const qint64 from = std::clamp<qint64>(i0, 0, n - 1);
        const qint64 to = std::max(std::clamp<qint64>(i1, 0, n), from + 1);
        for (int c = 0; c < channels; ++c) {
            float low = 1e9f, high = -1e9f;
            if (peaks) {
                for (qint64 k = from; k < to; ++k) {
                    low = std::min(low, peaks[(k * channels + c) * 2]);
                    high = std::max(high, peaks[(k * channels + c) * 2 + 1]);
                }
            } else {
                const float* samples = source.channelData(c);
                for (qint64 k = from; k < to; ++k) {
                    low = std::min(low, samples[k]);
                    high = std::max(high, samples[k]);
                }
            }
            lo[static_cast<size_t>(j * channels + c)] = low * static_cast<float>(gain);
            hi[static_cast<size_t>(j * channels + c)] = high * static_cast<float>(gain);
        }
    }
    const auto at = [&](const std::vector<float>& values, int j, int c) {
        return values[static_cast<size_t>(j * channels + c)];
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
    outline.count = count;
    for (const Lane& lane : lanes) {
        std::vector<float> tops(static_cast<size_t>(count)), bottoms(tops.size());
        const double middle = std::floor(lane.top + lane.height / 2.0) + 0.5;  // (a pixel's centre)
        const double half = std::max(1.0, lane.height / 2.0 - 1.0);
        const double laneTop = lane.top, laneBottom = lane.top + lane.height;
        const double thickness = std::min<double>(WaveformOutline::kMinThickness, lane.height);
        for (int j = 0; j < count; ++j) {
            float low = 0.0f, high = 0.0f;
            if (lane.channel >= 0) {
                low = at(lo, j, lane.channel);
                high = at(hi, j, lane.channel);
            } else {
                low = at(lo, j, 0);
                high = at(hi, j, 0);
                for (int c = 1; c < channels; ++c) {
                    low = std::min(low, at(lo, j, c));
                    high = std::max(high, at(hi, j, c));
                }
            }
            double top = middle - std::clamp<double>(high, -1, 1) * half;
            double bottom = middle - std::clamp<double>(low, -1, 1) * half;
            if (bottom - top < thickness) {  // as thick as the line through silence, about its middle
                const double centre = (top + bottom) / 2;
                top = centre - thickness / 2;
                bottom = centre + thickness / 2;
            }
            tops[static_cast<size_t>(j)] = static_cast<float>(std::clamp(top, laneTop, laneBottom));
            bottoms[static_cast<size_t>(j)] = static_cast<float>(std::clamp(bottom, laneTop, laneBottom));
        }
        outline.tops.push_back(std::move(tops));
        outline.bottoms.push_back(std::move(bottoms));
    }
    return outline;
}

void WaveformOutline::draw(SgPainter& p, double x0, double top, const QColor& color) const {
    for (size_t lane = 0; lane < tops.size(); ++lane) {
        p.save();
        p.translate(0.0, top);
        p.fillBand(x0, 1.0, tops[lane].data(), bottoms[lane].data(), count, color);
        p.restore();
    }
}

std::shared_ptr<const WaveformCache::Tile> WaveformCache::renderTile(const app::Waveform& source,
                                                                     double framesPerPixel, int index, int height,
                                                                     bool splitChannels, double gain) {
    auto tile = std::make_shared<Tile>(waveformOutline(source, framesPerPixel, qint64(index) * kTile - 1, kTile + 2,
                                                       height, splitChannels, gain));
    if (tile->count < 2) return nullptr;  // (past the source's end)
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
        p.setClipRect(QRectF(x, body.top(), kTile, body.height()));  // (its outer points are its neighbours')
        made->draw(p, x - 1.0, body.top(), color);
        p.restore();
    }
}

}  // namespace sub::ui::arrangement
