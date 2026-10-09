// The shape of a sound over time, as the extractor sums it up (internal to
// similarity/): preparing a sound (its level relative to its peak, its leading
// silence skipped, a fixed pre-roll), its amplitude envelope in 2 ms blocks,
// the level after the peak, counting onsets after the first from frames' mel
// bands, and summing up a sound's pitch estimates. Essentia computes the
// frames' and the envelope's descriptors (EssentiaExtractor.cpp); these are
// what it has no algorithm for, or one that doesn't suit one-shots.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <vector>

namespace sub::intelligence::descriptors {

inline constexpr double kStartDb = -45.0;  // a sound starts where it first comes this near its peak
inline constexpr double kPreRollSeconds = 0.002;
inline constexpr double kBlockSeconds = 0.002;  // the amplitude envelope's resolution
inline constexpr int kDecayPoints = 8;
inline constexpr double kContourFloorDb = -60.0;
inline constexpr double kContourFirstSeconds = 0.02;
// Onsets: a rise of the mel spectrum this far above the usual that raises the
// level at least 3 dB over the frames just before, at least 50 ms apart, and
// not in the first 60 ms (the sound's own attack).
inline constexpr double kOnsetMinRiseDb = 1.5;
inline constexpr double kOnsetGapSeconds = 0.05;
inline constexpr double kOnsetIgnoreSeconds = 0.06;
inline constexpr double kOnsetRangeDb = 40.0;
inline constexpr double kOnsetFloorDb = 60.0;
inline constexpr double kOnsetLevelRiseDb = 3.0;
inline constexpr size_t kOnsetLookBack = 4;  // frames

inline double dB(double power, double floor = 1e-12) { return 10.0 * std::log10(power + floor); }

// A weighted mean and standard deviation.
struct Weighted {
    double sw = 0.0, sx = 0.0, sxx = 0.0;
    void add(double x, double w) {
        sw += w;
        sx += w * x;
        sxx += w * x * x;
    }
    double mean() const { return sw > 0.0 ? sx / sw : 0.0; }
    double sd() const {
        if (sw <= 0.0) return 0.0;
        const double m = sx / sw;
        return std::sqrt(std::max(0.0, sxx / sw - m * m));
    }
};

// A sound made ready for analysis: its level relative to its peak, from a
// fixed pre-roll before where it starts, for at most `window` samples.
struct Prepared {
    std::vector<float> signal;
    size_t length = 0;         // signal.size()
    double seconds = 0.0;      // its length
    double fileSeconds = 0.0;  // the whole file's
    bool cut = false;          // whether the sound goes on after it
};

// False if the sound is silent (its peak below -100 dBFS, or not finite).
inline bool prepare(const float* samples, size_t count, double rate, size_t window, double fileSeconds, bool truncated,
                    Prepared& p) {
    float peak = 0.f;
    for (size_t i = 0; i < count; ++i) peak = std::max(peak, std::fabs(samples[i]));
    if (!(peak > 1e-5f) || !std::isfinite(peak)) return false;
    const float gain = 1.f / peak;
    const float threshold = static_cast<float>(std::pow(10.0, kStartDb / 20.0));
    size_t first = 0;
    while (first < count && std::fabs(samples[first]) * gain < threshold) ++first;
    // Always the same pre-roll before the start (silence where the file has
    // none), so where the frames fall doesn't depend on the silence before it.
    const auto preRoll = static_cast<size_t>(kPreRollSeconds * rate);
    const size_t lead = preRoll > first ? preRoll - first : 0;
    const size_t from = first - (preRoll - lead);
    const size_t length = std::min(count - from + lead, window);
    p.cut = truncated || from + (length - lead) < count;
    p.signal.assign(lead, 0.f);
    p.signal.insert(p.signal.end(), samples + from, samples + from + (length - lead));
    for (float& x : p.signal) x *= gain;
    p.length = length;
    p.seconds = static_cast<double>(length) / rate;
    p.fileSeconds = fileSeconds > 0.0 ? fileSeconds : static_cast<double>(count) / rate;
    return true;
}

// The amplitude envelope: each 2 ms block's mean energy.
struct Envelope {
    std::vector<double> blocks;
    size_t blockLength = 1;  // samples
    double blockSeconds = 0.0;
    size_t peakBlock = 0;
    double peakEnergy = 0.0;
};

inline void envelope(const float* x, size_t length, double rate, Envelope& e) {
    e.blockLength = std::max<size_t>(1, static_cast<size_t>(std::lround(kBlockSeconds * rate)));
    e.blockSeconds = static_cast<double>(e.blockLength) / rate;
    const size_t blockCount = (length + e.blockLength - 1) / e.blockLength;
    e.blocks.assign(blockCount, 0.0);
    for (size_t b = 0; b < blockCount; ++b) {
        const size_t begin = b * e.blockLength, end = std::min(length, begin + e.blockLength);
        double sum = 0.0;
        for (size_t i = begin; i < end; ++i) sum += static_cast<double>(x[i]) * x[i];
        e.blocks[b] = sum / static_cast<double>(e.blockLength);
    }
    e.peakBlock = 0;
    for (size_t b = 1; b < blockCount; ++b)
        if (e.blocks[b] > e.blocks[e.peakBlock]) e.peakBlock = b;
    e.peakEnergy = blockCount ? std::max(e.blocks[e.peakBlock], 1e-20) : 1e-20;
}

// The level (dB relative to the peak, -60..0) in kDecayPoints octave-wide
// windows after the peak, around 20, 40, ... 2560 ms. Past the end of the
// sound is silence; past the end of what was analysed (`cut`), unknown: the
// window before it holds.
inline void decayContour(const Envelope& e, bool cut, float* out) {
    const size_t blockCount = e.blocks.size();
    double previous = 0.0;
    for (int i = 0; i < kDecayPoints; ++i) {
        const double centre = kContourFirstSeconds * std::pow(2.0, i);
        const double from = centre / std::numbers::sqrt2, to = centre * std::numbers::sqrt2;
        const auto b0 = e.peakBlock + static_cast<size_t>(from / e.blockSeconds);
        const auto b1 = e.peakBlock + std::max<size_t>(static_cast<size_t>(to / e.blockSeconds), 1);
        double energy = 0.0;
        size_t n = 0;
        for (size_t b = b0; b < b1; ++b) {
            if (b < blockCount) {
                energy += e.blocks[b];
                ++n;
            } else if (!cut) {
                ++n;
            }
        }
        const double level = n ? std::clamp(dB(energy / static_cast<double>(n) / e.peakEnergy, 1e-9), kContourFloorDb, 0.0)
                               : (i ? previous : 0.0);
        out[i] = static_cast<float>(level);
        previous = level;
    }
}

// Onsets after the first, from frames' mel bands (dB) and levels (dB):
// `mel(t, b)` and `level(t)` for frames t < frameCount, `hopSeconds` apart.
// The spectral flux (how much the bands rise from frame to frame, not counting
// bands far below the loudest: the noise floor's jitter), peaks of it well
// above the usual that also raise the level (a new hit does; a sound dying
// away, even one whose pitch falls as a kick's, doesn't).
template <typename Mel, typename Level>
size_t countOnsets(size_t frameCount, int bands, Mel mel, Level level, double loudest, double hopSeconds) {
    float top = -1e9f;
    for (size_t t = 0; t < frameCount; ++t)
        for (int b = 0; b < bands; ++b) top = std::max(top, mel(t, b));
    const float floor = top - static_cast<float>(kOnsetFloorDb);
    std::vector<double> flux(frameCount, 0.0), levelRise(frameCount, 0.0);
    Weighted usual;
    for (size_t t = 1; t < frameCount; ++t) {
        if (level(t) < loudest - kOnsetRangeDb) continue;
        double rise = 0.0;
        for (int b = 0; b < bands; ++b) rise += std::max(0.f, std::max(mel(t, b), floor) - std::max(mel(t - 1, b), floor));
        flux[t] = rise / bands;
        usual.add(flux[t], 1.0);
        double before = level(t - 1);
        for (size_t k = 2; k <= kOnsetLookBack && k <= t; ++k) before = std::min(before, level(t - k));
        levelRise[t] = level(t) - before;
    }
    const double limit = std::max(kOnsetMinRiseDb, usual.mean() + usual.sd());
    const auto gap = static_cast<size_t>(std::ceil(kOnsetGapSeconds / hopSeconds));
    const auto skip = static_cast<size_t>(std::ceil(kOnsetIgnoreSeconds / hopSeconds));
    size_t onsets = 0, last = 0;
    bool any = false;
    for (size_t t = std::max<size_t>(skip, 1); t < frameCount; ++t) {
        if (flux[t] < limit || levelRise[t] < kOnsetLevelRiseDb) continue;
        bool isPeak = true;
        for (size_t u = t > 2 ? t - 2 : 0; u <= std::min(frameCount - 1, t + 2) && isPeak; ++u)
            isPeak = u == t || flux[u] < flux[t] || (flux[u] == flux[t] && u > t);
        if (!isPeak || (any && t - last < gap)) continue;
        ++onsets;
        last = t;
        any = true;
    }
    return onsets;
}

// A pitch estimate of one window: log2(f0 / 220 Hz), how aperiodic (0: perfectly
// periodic), and the window's energy.
struct PitchEstimate {
    double pitch, aperiodicity, energy;
};

// How pitched a sound is (0..1: the share of its energy in periodic windows,
// times how periodic they are) and that confidence times its energy-weighted
// median pitch. `voiced` are the periodic windows; `totalEnergy`, all of them.
inline void pitchSummary(std::vector<PitchEstimate>& voiced, double totalEnergy, float& confidence, float& pitch) {
    double energy = 0.0, aperiodic = 0.0;
    for (const PitchEstimate& e : voiced) {
        energy += e.energy;
        aperiodic += e.energy * e.aperiodicity;
    }
    confidence = pitch = 0.f;
    if (!(energy > 0.0 && totalEnergy > 0.0)) return;
    const double c = std::clamp((energy / totalEnergy) * (1.0 - aperiodic / energy), 0.0, 1.0);
    std::sort(voiced.begin(), voiced.end(), [](const PitchEstimate& a, const PitchEstimate& b) { return a.pitch < b.pitch; });
    double half = energy / 2.0, median = voiced.front().pitch;
    for (const PitchEstimate& e : voiced) {
        median = e.pitch;
        half -= e.energy;
        if (half <= 0.0) break;
    }
    confidence = static_cast<float>(c);
    pitch = static_cast<float>(c * median);
}

}  // namespace sub::intelligence::descriptors
