#pragma once
// Erosion's maths, after Live 12.4's Erosion: the input is read out of a short
// delay (2 ms, the device's latency) whose read position a modulator wobbles at
// audio rate. That is phase modulation of every partial of the input: each gets
// sidebands at its frequency plus and minus the modulator's (a sine: discrete,
// metallic ones, as a ring modulator's; band-passed noise: a hiss around each
// partial). The phase deviation is 2 pi times a partial's frequency times the
// delay's excursion, so the higher a partial, the more it is eroded: lows
// survive, highs turn to fizz. Sidebands past Nyquist fold back, and that
// aliasing is part of the sound.
//
// The modulator is a sine, band-passed noise, or an equal-power blend of the
// two. The noise is white noise through two identical band-pass sections in a
// row (12 dB/octave skirts: a narrow band is selective, as the manual has it),
// scaled so its RMS is a unit sine's whatever its frequency and width
// (noisePowerGain() is the pair's exact power gain): Frequency and Filter Width
// move where the noise's energy is, never how hard it modulates.
//
// Shared by the device (builtin/devices/Erosion.cpp) and its editor (through the
// application layer's ErosionResponse.h), so the band drawn is the filter that
// plays and the excursion read out is the one applied. No engine headers: the
// application layer includes this one.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <utility>

namespace sub::erosion {

constexpr double kMinFrequency = 20.0, kMaxFrequency = 18000.0;  // Hz: Frequency's range
constexpr double kMinWidth = 0.1, kMaxWidth = 10.0;              // octaves: Filter Width's range
constexpr double kNyquistLimit = 0.45;                           // of the sample rate: the highest the modulator goes
constexpr double kCentreSeconds = 0.002;                         // the delay's centre: the latency (Live 12.4's 2 ms)
constexpr double kNoiseRms = 0.70710678118654752;                // the noise's RMS: a unit sine's
constexpr double kPi = std::numbers::pi;

// The delay's centre in whole samples (its latency): 96 at 48 kHz.
inline int centreDelaySamples(double sampleRate) noexcept {
    return std::max(4, static_cast<int>(std::lround(kCentreSeconds * sampleRate)));
}

// How far the read position may move either way: the centre less 2 (Hermite
// reads one sample on either side), so it never reads the future or past the line.
inline double limitSamples(double sampleRate) noexcept { return centreDelaySamples(sampleRate) - 2.0; }

// The excursion at Amount 100 %: the limit over sqrt 2. The modulator's RMS is
// 1/sqrt 2, so the limit is 2 RMS out, beyond a sine's peak: only noise peaks reach it.
inline double maxExcursionSamples(double sampleRate) noexcept {
    return limitSamples(sampleRate) / std::numbers::sqrt2;
}

// The excursion for Amount (%), growing with its square (fine control low down,
// where most use is): 25 % is 1/16 of the most.
inline double excursionSamples(double amountPercent, double sampleRate) noexcept {
    const double a = std::clamp(amountPercent, 0.0, 100.0) / 100.0;
    return maxExcursionSamples(sampleRate) * a * a;
}

// The same in ms (the editor's readout): ±87 µs at 25 %, ±1.38 ms at 100 % (48 kHz).
inline double excursionMs(double amountPercent, double sampleRate) noexcept {
    return sampleRate > 0.0 ? 1000.0 * excursionSamples(amountPercent, sampleRate) / sampleRate : 0.0;
}

// The frequency the modulator plays: `freq` kept within 1 Hz..0.45 of the rate.
inline double modFrequency(double freq, double sampleRate) noexcept {
    return std::min(std::max(freq, 1.0), std::max(1.0, kNyquistLimit * sampleRate));
}

// The noise's band-pass is two identical state-variable sections in series
// (12 dB/octave skirts). A section's Q for a band whose -3 dB edges are `width`
// octaves apart: each section is 1.5 dB down there, so
// Q = sqrt(sqrt 2 - 1) / (h - 1/h), h = 2^(width/2) (width kept to kMinWidth..kMaxWidth).
constexpr double kSectionQ = 0.64359425290558262;  // sqrt(sqrt 2 - 1)
inline double bandQ(double widthOctaves) noexcept {
    const double h = std::exp2(0.5 * std::clamp(widthOctaves, kMinWidth, kMaxWidth));
    return kSectionQ / (h - 1.0 / h);
}

// The power white noise keeps through two identical sections
// b0 (1 - z^-2) / (1 + a1 z^-1 + a2 z^-2): the sum over all lags of R[k]², R the
// autocorrelation of one section's impulse response (|H|⁴ is the transform of
// R * R). R[0..2] come from the poles' autocorrelation (that of 1 / (1 + a1 z^-1 +
// a2 z^-2), ra, through the numerator's 2 - z^2 - z^-2); from lag 3 on R follows
// the section's recursion, so the vector (R[k]², R[k] R[k-1], R[k-1]²) is
// multiplied by a fixed matrix each lag and its sum from lag 2 has a closed form.
// Exact (it matches summing the impulse response); double, as the poles sit
// close to 1 low down.
inline double noisePowerGain(double b0, double a1, double a2) noexcept {
    const double ra0 = (1.0 + a2) / ((1.0 - a2) * ((1.0 + a2) * (1.0 + a2) - a1 * a1));
    const double ra1 = -a1 * ra0 / (1.0 + a2);
    const double ra2 = -a1 * ra1 - a2 * ra0, ra3 = -a1 * ra2 - a2 * ra1, ra4 = -a1 * ra3 - a2 * ra2;
    const double s = b0 * b0;  // (1 - z^-2)(1 - z^2) = 2 - z^2 - z^-2
    const double r0 = s * (2.0 * ra0 - 2.0 * ra2), r1 = s * (ra1 - ra3), r2 = s * (2.0 * ra2 - ra0 - ra4);
    const double v0 = r2 * r2, v1 = r2 * r1, v2 = r1 * r1;
    const double tail = (v0 + 2.0 * a1 * a2 * v1 / (1.0 + a2) + a2 * a2 * v2) /
                        (1.0 - a1 * a1 - a2 * a2 + 2.0 * a1 * a1 * a2 / (1.0 + a2));  // the sum of R[k]², k >= 2
    return r0 * r0 + 2.0 * (r1 * r1 + tail);
}

// The noise's band-pass for a frequency and width: a section's g (tan(pi f / sr))
// and k (1/Q), each section's k v1 being the RBJ band-pass at 0 dB peak, and the
// gain that takes uniform noise in [-1, 1] (variance 1/3) through the pair to an
// RMS of kNoiseRms.
struct Band {
    double g = 0.0, k = 1.0, scale = 1.0;
};
inline Band band(double freq, double width, double sampleRate) noexcept {
    const double g = std::tan(kPi * modFrequency(freq, sampleRate) / sampleRate);
    const double k = 1.0 / bandQ(width);
    const double a0 = 1.0 + g * k + g * g;
    const double gain = noisePowerGain(g * k / a0, 2.0 * (g * g - 1.0) / a0, (1.0 - g * k + g * g) / a0);
    return {g, k, std::sqrt(1.5 / gain)};  // variance 1/3 times 1.5 / gain: kNoiseRms² (1/2)
}

// The pair's magnitude at `frequency` (0..1, 1 at its centre; 0 at and past
// Nyquist): through the bilinear transform the digital response at w is the
// analog one at tan(w/2) / tan(w0/2), squared for the two sections.
inline double bandMagnitude(double freq, double width, double sampleRate, double frequency) noexcept {
    if (!(sampleRate > 0.0) || frequency <= 0.0 || frequency >= 0.5 * sampleRate) return 0.0;
    const double omega =
        std::tan(kPi * frequency / sampleRate) / std::tan(kPi * modFrequency(freq, sampleRate) / sampleRate);
    const double x = omega / bandQ(width);
    return x * x / ((1.0 - omega * omega) * (1.0 - omega * omega) + x * x);
}

// The band's -3 dB edges (Hz): the analog edges f 2^(-width/2), f 2^(width/2),
// through the bilinear transform.
inline std::pair<double, double> bandEdges(double freq, double width, double sampleRate) noexcept {
    if (!(sampleRate > 0.0)) return {0.0, 0.0};
    const double t = std::tan(kPi * modFrequency(freq, sampleRate) / sampleRate);
    const double h = std::exp2(0.5 * std::clamp(width, kMinWidth, kMaxWidth));
    return {sampleRate / kPi * std::atan(t / h), sampleRate / kPi * std::atan(t * h)};
}

// Noise Blend's equal-power weights {sine, noise}: {cos(pi/2 b), sin(pi/2 b)},
// b = blend/100 (kept to 0..1). Both made with sin, so each is exactly 1 or 0 at
// the ends (cos(pi/2) in double isn't 0): at 0 % or 100 % the other source adds
// exactly nothing.
struct Weights {
    double sine = 0.0, noise = 1.0;
};
inline Weights blendWeights(double blendPercent) noexcept {
    const double b = std::clamp(blendPercent, 0.0, 100.0) / 100.0;
    return {std::sin(0.5 * kPi * (1.0 - b)), std::sin(0.5 * kPi * b)};
}

// Stereo Width's spread: the noise's mid and side gains (mid² + side² = 1, so
// each side keeps its RMS; the sides' correlation is (1 - w²) / (1 + w²)) and
// the sine's half-offset in radians (the sides are twice that apart: a quarter
// cycle at 100 %, uncorrelated as the noise is). w = stereo/100.
struct Spread {
    double mid = 1.0, side = 0.0, half = 0.0;
};
inline Spread stereoSpread(double stereoPercent) noexcept {
    const double w = std::clamp(stereoPercent, 0.0, 100.0) / 100.0;
    const double mid = 1.0 / std::sqrt(1.0 + w * w);
    return {mid, w * mid, w * 0.25 * kPi};
}

// The noise generators' seeds (dsp::Noise, DspBlocks.h): mid and side, each
// salted with the instance's own number, hashed (dsp::hash32: consecutive
// numbers give unrelated salts; 0 gives 0, the plain seeds), so Erosions set
// alike don't modulate alike: two on double-tracked parts would otherwise
// wobble in lockstep, and an export, which resets every device at once, would
// line them all up. An instance keeps its salt for life: its reset() restarts
// its own noise, so its renders repeat.
constexpr uint32_t kMidSeed = 0x2545F491u, kSideSeed = 0x9E3779B9u;

// How many Erosions have been made: each takes the count, as it is made, for
// its number. (Tests set it back to 0 to make devices that play alike.)
inline std::atomic<uint32_t> instancesMade{0};

// A seed salted (never 0, which dsp::Noise has no sequence from).
inline uint32_t saltedSeed(uint32_t seed, uint32_t salt) noexcept { return (seed ^ salt) != 0 ? seed ^ salt : seed; }

// Audio frames per value of the device's `erosion` display (what it changed,
// in dB): the editor reads that many frames into each value.
constexpr int kMeterSamples = 256;

}  // namespace sub::erosion
