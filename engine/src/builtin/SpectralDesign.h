#pragma once
// The Spectral Compressor's maths: the frame it analyses with, how its levels
// are measured (pink-referenced dB), its threshold curves, the Focus band's
// weights, Smoothing's width and the gain computer. Shared by the device
// (builtin/devices/Spectral.cpp, which runs float copies of these in its
// per-bin loop) and its editor (through the application layer's
// SpectralResponse.h), so the threshold drawn is the one that plays.
//
// Levels are pink-referenced: pink noise of RMS L dBFS (20 Hz-20 kHz) reads L
// dB at every frequency, so a dense mix reads near its loudness across the
// spectrum and a threshold means about what a compressor's does. A pure tone
// reads about 20 dB above its peak in dBFS (a 0 dBFS sine at 1 kHz reads
// +19.9 dB): it is all in one bin, where noise of the same power is spread
// over hundreds.
//
// No FFT here, and nothing but the standard library: the application layer
// includes it too.

#include <algorithm>
#include <cmath>

namespace sub::spectral {

// The curves turn about 1 kHz; under 20 Hz bins take 20 Hz's tilt and pink reference.
inline constexpr double kPivotHz = 1000.0;
inline constexpr double kLowestHz = 20.0;
inline constexpr double kPinkDbPerOctave = 3.0102999566398120;  // 10 log10(2): pink noise per bin per octave

// The frame: 2048 at 44.1 and 48 kHz (kReferenceRate's), hops of a quarter of it.
inline constexpr int kOverlap = 4;
inline constexpr double kReferenceRate = 48000.0;
inline constexpr int kReferenceFrame = 2048;
inline constexpr int kMinFrame = 256, kMaxFrame = 8192;

// Levels are floored at kFloorDb. Nothing is lifted below kUpwardFloorDb; the lift is whole from kUpwardFadeDb
// above it, so silence and dither stay put.
inline constexpr double kFloorDb = -150.0;
inline constexpr double kUpwardFloorDb = -90.0;
inline constexpr double kUpwardFadeDb = 20.0;

// Smoothing's width: just above 0 %, and at 100 %.
inline constexpr double kMinSmoothingOctaves = 1.0 / 48.0;
inline constexpr double kMaxSmoothingOctaves = 2.0;

// The displays: points per frame, log-spaced over the audible range.
inline constexpr int kDisplayPoints = 128;
inline constexpr double kDisplayLowHz = 20.0, kDisplayHighHz = 20000.0;

inline constexpr double kFocusEdgeOctaves = 1.0 / 3.0;  // a Focus edge fades out over this, outside the band
inline constexpr double kGlideSeconds = 0.030;          // the curves' per-hop glides' time constant

// The frame for a sample rate: 2048 at 44.1/48 kHz, doubling per octave of rate
// (4096 at 88.2/96, 8192 at 176.4/192, 1024 at 22.05..32, 512 at 16, 256 at
// 8 kHz), so a bin is about 23 Hz wide at any rate. It is also the latency.
inline int frameSize(double sampleRate) noexcept {
    const double octaves = std::round(std::log2(std::max(1.0, sampleRate) / kReferenceRate));
    return std::clamp(static_cast<int>(std::lround(kReferenceFrame * std::exp2(octaves))), kMinFrame, kMaxFrame);
}

inline double octavesFromPivot(double freq) noexcept { return std::log2(std::max(freq, kLowestHz) / kPivotHz); }
// What pink noise reads per bin, relative to 1 kHz: added to every level, so pink noise reads flat.
inline double pinkDb(double freq) noexcept { return kPinkDbPerOctave * octavesFromPivot(freq); }
// The (downward) threshold at a frequency: Threshold, turned about 1 kHz by Tilt (dB per octave).
inline double thresholdDb(double threshold, double tilt, double freq) noexcept {
    return threshold + tilt * octavesFromPivot(freq);
}
// The upward threshold at a frequency: Below, never above Threshold, tilted as it is.
inline double belowDb(double threshold, double below, double tilt, double freq) noexcept {
    return std::min(below, threshold) + tilt * octavesFromPivot(freq);
}

// Octaves from 1 kHz for the Focus, not held at 20 Hz (so DC and subsonics fall outside it): 1 Hz at the least.
inline double focusOctaves(double freq) noexcept { return std::log2(std::max(freq, 1.0) / kPivotHz); }
// How much of its gain a frequency gets (0..1): all of it from `low` to `high`, fading linearly in octaves to
// none kFocusEdgeOctaves outside each edge. The engine works it per bin from glided edges in octaves:
// clamp((o - lowOct) / E + 1, 0, 1) * clamp((highOct - o) / E + 1, 0, 1).
inline double focusWeight(double low, double high, double freq) noexcept {
    const double o = focusOctaves(freq);
    const double lower = std::clamp((o - focusOctaves(low)) / kFocusEdgeOctaves + 1.0, 0.0, 1.0);
    const double upper = std::clamp((focusOctaves(high) - o) / kFocusEdgeOctaves + 1.0, 0.0, 1.0);
    return lower * upper;
}

// Added to 10 log10 of a bin's power |X|² (an unscaled FFT of a periodic-Hann-windowed frame, Σw² = 3N/8) so
// that, with pinkDb(), pink noise of RMS L dBFS between 20 Hz and 20 kHz reads L dB at every frequency. Pink
// noise's one-sided power per hertz is σ² / (f ln 1000); a bin's expected power is S·sr·Σw²/2. At 48 kHz and 2048,
// -34.26 dB (a full-scale sine at a bin's centre then reads 20 log10(N/4) + this = +19.93 dB at 1 kHz).
inline double calibrationDb(double sampleRate, int frame) noexcept {
    return -10.0 * std::log10(3.0 * sampleRate * frame / (16.0 * std::log(kDisplayHighHz / kDisplayLowHz) * kPivotHz));
}

// Smoothing's width in octaves: 0 at 0 % (each bin alone), then 1/48 octave growing exponentially to 2 octaves
// at 100 % (40 %: 0.129, about 1/8 octave). Each bin's level is the mean power over a band this wide, centred on
// it in log frequency.
inline double smoothingOctaves(double percent) noexcept {
    return percent <= 0.0
               ? 0.0
               : kMinSmoothingOctaves * std::pow(kMaxSmoothingOctaves / kMinSmoothingOctaves, percent / 100.0);
}

// The frequency display point j stands for (0..kDisplayPoints - 1: log-spaced from 20 Hz to 20 kHz).
inline double displayFrequency(int j) noexcept {
    return kDisplayLowHz * std::pow(kDisplayHighHz / kDisplayLowHz, double(j) / (kDisplayPoints - 1));
}

// The gain computer's slopes (dB of gain per dB past a threshold): downward 1/R - 1 (≤ 0), upward 1 - 1/Ru (≥ 0).
inline double downSlope(double ratio) noexcept { return 1.0 / std::max(1.0, ratio) - 1.0; }
inline double upSlope(double upward) noexcept { return 1.0 - 1.0 / std::max(1.0, upward); }

// How far past the threshold a level counts, with a quadratic knee `knee` dB wide (the Compressor's shape):
// 0 below -knee/2, (over + knee/2)² / (2 knee) across it, `over` above it.
inline double kneed(double over, double knee) noexcept {
    if (knee <= 0.0) return std::max(over, 0.0);
    if (2.0 * over <= -knee) return 0.0;
    if (2.0 * over < knee) {
        const double x = over + 0.5 * knee;
        return x * x / (2.0 * knee);
    }
    return over;
}

// The gain in dB for a level (pink-referenced dB) against the thresholds there (`belowDb` ≤ `thresholdDb`, as
// belowDb() gives), with the slopes downSlope() and upSlope() give: down above the threshold, up under Below
// (fading out between kUpwardFloorDb and kUpwardFloorDb + kUpwardFadeDb, so silence and dither stay put), held
// within ±range. The static curve out = level + gainDb(...) rises everywhere: where both knees overlap their
// shares sum to at most 1, so the slope stays between 1/Ru and 1/R; the fade only steepens it.
inline double gainDb(double levelDb, double thresholdDb, double belowDb, double down, double up, double knee,
                     double range) noexcept {
    const double fade = std::clamp((levelDb - kUpwardFloorDb) / kUpwardFadeDb, 0.0, 1.0);
    const double gain = down * kneed(levelDb - thresholdDb, knee) + up * fade * kneed(belowDb - levelDb, knee);
    return std::clamp(gain, -range, range);
}

}  // namespace sub::spectral
