#include "builtin/SampleSlicing.h"

#include <algorithm>
#include <cmath>

namespace sub::slicing {
namespace {

constexpr double kHopsPerSecond = 375.0;  // the detector's hop: 128 samples at 48 kHz
constexpr int kLookBack = 3;              // a rise is measured over the loudest of the hops before
constexpr double kFloorDb = 60.0;         // energies are floored this far below the loudest
constexpr double kMinRiseDb = 3.0;        // weaker rises are no transients
constexpr double kPeakSeconds = 0.03;     // a transient is the biggest rise within this either side
constexpr double kAttackShare = 0.25;     // its attack starts where the signal first reaches this of its peak
constexpr double kZeroSearchSeconds = 0.002;
constexpr double kMinSliceSeconds = 0.01;

double toDb(double energy, double floor) { return 10.0 * std::log10(std::max(energy, floor)); }

}  // namespace

std::vector<Onset> detectOnsets(const float* const* channels, int numChannels, int64_t frames, double sampleRate,
                                bool reversed) {
    if (frames <= 0 || numChannels <= 0 || !(sampleRate > 0.0)) return {};
    // The sample summed to mono, in the order it plays.
    std::vector<float> mono(static_cast<size_t>(frames));
    for (int64_t i = 0; i < frames; ++i) {
        float sum = 0.f;
        for (int c = 0; c < numChannels; ++c) sum += channels[c][i];
        mono[static_cast<size_t>(reversed ? frames - 1 - i : i)] = sum / static_cast<float>(numChannels);
    }
    const auto at = [&](int64_t i) { return mono[static_cast<size_t>(i)]; };

    // Each hop's energy over a window of two hops: the whole band, and the highs
    // (the first difference), so hats and snares over a kick's tail rise too.
    const int64_t hop = std::max<int64_t>(16, std::llround(sampleRate / kHopsPerSecond));
    const int64_t hops = (frames + hop - 1) / hop;
    std::vector<double> full(static_cast<size_t>(hops)), highs(static_cast<size_t>(hops));
    for (int64_t k = 0; k < hops; ++k) {
        const int64_t from = k * hop, to = std::min(frames, from + 2 * hop);
        double f = 0.0, h = 0.0;
        for (int64_t i = from; i < to; ++i) {
            const double x = at(i), d = x - (i > 0 ? at(i - 1) : 0.f);
            f += x * x;
            h += d * d;
        }
        full[static_cast<size_t>(k)] = f;
        highs[static_cast<size_t>(k)] = h;
    }
    const double loudest = *std::max_element(full.begin(), full.end());
    const double loudestHighs = *std::max_element(highs.begin(), highs.end());
    if (!(loudest > 0.0)) return {};  // silence
    const double floor = loudest * std::pow(10.0, -kFloorDb / 10.0);
    const double floorHighs = std::max(loudestHighs, 1e-30) * std::pow(10.0, -kFloorDb / 10.0);
    for (int64_t k = 0; k < hops; ++k) {
        full[static_cast<size_t>(k)] = toDb(full[static_cast<size_t>(k)], floor);
        highs[static_cast<size_t>(k)] = toDb(highs[static_cast<size_t>(k)], floorHighs);
    }

    // How far each hop rises over the loudest of the few before it (in either band).
    std::vector<double> rise(static_cast<size_t>(hops), 0.0);
    const double silentFull = toDb(0.0, floor), silentHighs = toDb(0.0, floorHighs);
    for (int64_t k = 0; k < hops; ++k) {
        double beforeFull = silentFull, beforeHighs = silentHighs;
        for (int64_t j = std::max<int64_t>(0, k - kLookBack); j < k; ++j) {
            beforeFull = std::max(beforeFull, full[static_cast<size_t>(j)]);
            beforeHighs = std::max(beforeHighs, highs[static_cast<size_t>(j)]);
        }
        rise[static_cast<size_t>(k)] = std::max({0.0, full[static_cast<size_t>(k)] - beforeFull,
                                                 highs[static_cast<size_t>(k)] - beforeHighs});
    }

    // Transients: the biggest rise within kPeakSeconds either side (the first of equals).
    const int64_t reach = std::max<int64_t>(1, std::llround(kPeakSeconds * sampleRate / static_cast<double>(hop)));
    std::vector<std::pair<int64_t, double>> peaks;  // (hop, rise)
    double strongest = 0.0;
    for (int64_t k = 0; k < hops; ++k) {
        const double r = rise[static_cast<size_t>(k)];
        if (r < kMinRiseDb) continue;
        bool peak = true;
        for (int64_t j = std::max<int64_t>(0, k - reach); peak && j <= std::min(hops - 1, k + reach); ++j) {
            const double other = rise[static_cast<size_t>(j)];
            peak = j < k ? other < r : other <= r;
        }
        if (!peak) continue;
        peaks.emplace_back(k, r);
        strongest = std::max(strongest, r);
    }

    // Where each starts: the first sample near the rise reaching kAttackShare of
    // the attack's peak, then back to the zero crossing before it.
    std::vector<Onset> onsets;
    onsets.reserve(peaks.size());
    const int64_t zeroSearch = std::max<int64_t>(1, std::llround(kZeroSearchSeconds * sampleRate));
    for (const auto& [k, r] : peaks) {
        const int64_t from = std::max<int64_t>(0, (k - 1) * hop);
        const int64_t peakTo = std::min(frames, (k + 3) * hop);
        float peak = 0.f;
        for (int64_t i = from; i < peakTo; ++i) peak = std::max(peak, std::abs(at(i)));
        int64_t first = from;
        const float reachLevel = static_cast<float>(kAttackShare) * peak;
        while (first < peakTo - 1 && std::abs(at(first)) < reachLevel) ++first;
        int64_t frame = first;
        for (int64_t i = first; i > std::max<int64_t>(0, first - zeroSearch); --i) {
            if (at(i) == 0.f || (at(i - 1) < 0.f) != (at(i) < 0.f)) {
                frame = i;
                break;
            }
        }
        if (!onsets.empty() && frame <= onsets.back().frame) continue;
        onsets.push_back({frame, static_cast<float>(r / strongest)});
    }
    return onsets;
}

int64_t nearestZeroCrossing(const float* const* channels, int numChannels, int64_t frames, bool reversed,
                            int64_t frame, int64_t reach) {
    if (frame <= 0 || frame >= frames || numChannels <= 0) return frame;
    const auto sum = [&](int64_t i) {
        const int64_t at = reversed ? frames - 1 - i : i;
        float total = 0.f;
        for (int c = 0; c < numChannels; ++c) total += channels[c][at];
        return total;
    };
    // A crossing at i: between i - 1 and i the sign changes, or i is silent.
    const auto crosses = [&](int64_t i) {
        if (i <= 0 || i >= frames) return false;
        const float before = sum(i - 1), here = sum(i);
        return here == 0.f || (before < 0.f) != (here < 0.f);
    };
    for (int64_t distance = 0; distance <= reach; ++distance) {
        if (crosses(frame - distance)) return frame - distance;
        if (distance > 0 && crosses(frame + distance)) return frame + distance;
    }
    return frame;
}

int sliceStarts(const SliceSettings& settings, const Onset* onsets, size_t count, int64_t start, int64_t end,
                double sampleRate, int64_t* out) {
    if (end <= start) return 0;
    int n = 0;
    out[n++] = start;
    const auto add = [&](int64_t frame) {
        if (n < kMaxSlices && frame > out[n - 1] && frame < end) out[n++] = frame;
    };
    const double length = static_cast<double>(end - start);
    switch (settings.by) {
        case SliceBy::Transient: {
            const auto gap = static_cast<int64_t>(kMinSliceSeconds * sampleRate);
            const float threshold = 1.f - std::clamp(settings.sensitivity, 0.f, 1.f) - 1e-6f;
            for (size_t i = 0; i < count && n < kMaxSlices; ++i) {
                const Onset& onset = onsets[i];
                if (onset.strength < threshold || onset.frame - out[n - 1] < gap) continue;
                if (onset.frame >= end - gap) break;
                add(onset.frame);
            }
            break;
        }
        case SliceBy::Beat: {
            if (!(settings.regionBeats > 0.0) || !(settings.divisionBeats > 0.0)) break;
            const double slices = settings.regionBeats / settings.divisionBeats;
            for (int i = 1; i < kMaxSlices && i < slices - 1e-9; ++i) {
                add(start + static_cast<int64_t>(std::llround(length * i / slices)));
            }
            break;
        }
        case SliceBy::Region: {
            const int regions = std::clamp(settings.regions, 1, kMaxSlices);
            for (int i = 1; i < regions; ++i) add(start + static_cast<int64_t>(std::llround(length * i / regions)));
            break;
        }
    }
    return n;
}

}  // namespace sub::slicing
