#pragma once
// The Limiter's maths that its editor shares (builtin/devices/Limiter.cpp):
// the lookahead's lengths, where the line is and how the signal is scaled
// around it, Soft Clip's knee, the M/S ceiling, and the true-peak detector's
// interpolator and peak refinement.
//
// The device works in a normalized domain where the line (the ceiling, or with
// Maximize the threshold) is 1: the input is multiplied by `pre` before the
// lookahead and the detector, and the output by `post` after limiting
// (scales()). Without Maximize pre = gain / ceiling and post = ceiling; with it
// pre = 1 / threshold and post = output, so the gain is Output - Threshold and
// the ceiling is the Output.
//
// The editor (through the application layer's LimiterResponse.h) draws the
// line, Soft Clip's band and its curve from these, so what it shows is where
// the device rounds off and limits.

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "rt/RtUtils.h"

namespace sub::limiter {

inline constexpr double kLookaheadMs[3] = {1.5, 3.0, 6.0};  // the Lookahead list's values
inline constexpr int kDetectorDelay = 12;   // samples the detector runs behind the input (kTaps / 2)
inline constexpr int kOversampling = 8;     // true-peak points per sample (phase 0 is the sample itself)
inline constexpr int kTaps = 24;            // the interpolator's taps per fractional position
inline constexpr double kKaiserBeta = 6.0;  // its window
inline constexpr float kTruePeakMargin = 0.99770006f;  // -0.02 dB: True Peak's target under the ceiling
inline constexpr float kSoftKnee = 0.5f;    // -6.02 dB: where Soft Clip's knee starts (of the ceiling)
inline constexpr int kMeterSamples = 128;   // audio per display value
inline constexpr float kFloorDb = -90.f;    // the displays' floor
inline constexpr double kAutoFastMs = 50.0;          // auto release: the fast stage's time constant
inline constexpr double kAutoSlowAttackMs = 400.0;   // the slow stage building up under sustained limiting
inline constexpr double kAutoSlowReleaseMs = 600.0;  // the slow stage letting go
inline constexpr double kRampSeconds = 0.02;  // pre, post, link, routing, mode and Auto's glides
inline constexpr float kMaxInput = 1e30f;     // input beyond it (and NaN) is taken as 0
inline constexpr double kDipSeconds = 0.002;  // the fade out and in around a lookahead change
inline constexpr double kDepthScale = 1073741824.0;  // 2^30: the box filters' fixed point
inline constexpr float kTinyDepth = 1e-9f;           // release states below it are flushed to 0

// The lookahead in samples for a choice of the list (0..2) at a rate: 72, 144
// or 288 at 48 kHz. Never less than the detector's delay plus one.
inline int lookaheadSamples(int choice, double sampleRate) noexcept {
    const double ms = kLookaheadMs[std::clamp(choice, 0, 2)];
    return std::max(kDetectorDelay + 1, static_cast<int>(std::lround(ms * sampleRate / 1000.0)));
}

// How the device scales the signal around its line: `pre` before limiting,
// `post` after (the same floats the device multiplies by). lineDb is where the
// line is (the ceiling, or with Maximize the threshold); postDb the output's
// ceiling (the ceiling, or the Output).
struct Scales {
    float pre, post, lineDb, postDb;
};
inline Scales scales(bool maximize, float gainDb, float ceilingDb, float thresholdDb, float outputDb) noexcept {
    if (maximize) return {expDbToGain(-thresholdDb), expDbToGain(outputDb), thresholdDb, outputDb};
    return {expDbToGain(gainDb - ceilingDb), expDbToGain(ceilingDb), ceilingDb, ceilingDb};
}

// --- Soft Clip ---------------------------------------------------------------------------

// Soft Clip's knee at a softness σ (0: Standard's plain clamp at 1, 1: Soft
// Clip): linear up to `start`, a quadratic from slope 1 there to slope 0 at
// `end` = 2 - start, where it is exactly 1.
struct Knee {
    float start, end, k;
};
inline Knee knee(float softness) noexcept {
    const float start = 1.f - (1.f - kSoftKnee) * std::clamp(softness, 0.f, 1.f);
    return {start, 2.f - start, start < 1.f ? 1.f / (4.f * (1.f - start)) : 0.f};
}

// A sample (in the normalized domain) through the knee: odd, continuous in value
// and slope, and never above 1.
inline float shape(float x, const Knee& knee) noexcept {
    const float a = std::abs(x);
    if (a <= knee.start) return x;
    const float over = a - knee.start;
    const float y = a >= knee.end ? 1.f : std::min(1.f, a - knee.k * over * over);
    return std::copysign(y, x);
}

// Soft Clip's curve (σ = 1) for a level in dB relative to the line: the level out.
inline float softClipDb(float inDb) noexcept {
    const float y = shape(std::pow(10.f, inDb / 20.f), knee(1.f));
    return 20.f * std::log10(std::max(y, 1e-12f));
}
inline float softKneeDb() noexcept { return 20.f * std::log10(kSoftKnee); }       // -6.02: it starts rounding off
inline float softTopDb() noexcept { return 20.f * std::log10(2.f - kSoftKnee); }  // +3.52: it reaches the line

// --- M/S -----------------------------------------------------------------------------------

// The gains (each ≤ 1) that keep a + b at most `target` for a mid peak `a` and
// a side peak `b` (|left| and |right| are at most |mid| + |side|): neither is
// touched while they fit; otherwise the quieter keeps its level while the
// louder takes what is left, and both meet at half the target when both are
// loud. ga a + gb b ≤ target always.
inline void sharedCeiling(float a, float b, float target, float& ga, float& gb) noexcept {
    if (a + b <= target) {
        ga = gb = 1.f;
        return;
    }
    const float lo = std::min(a, b);
    const float level = lo >= 0.5f * target ? 0.5f * target : target - lo;
    ga = a > level ? level / a : 1.f;
    gb = b > level ? level / b : 1.f;
}

// --- True peak -----------------------------------------------------------------------------

// The interpolator: for each fractional position t = k / 8 (k = 1..7), 24
// taps of a Kaiser-windowed sinc (β = 6) over the samples m - 11 .. m + 12,
// each phase normalized to pass DC exactly. Flat within ±0.01 dB to 0.42 of
// the rate (20.2 kHz at 48 kHz).
using Phase = std::array<float, kTaps>;
using Phases = std::array<Phase, kOversampling - 1>;

namespace detail {
inline double besselI0(double x) noexcept {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 30; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
    }
    return sum;
}
}  // namespace detail

// The seven phases (a function-local static: the first call works them out, so
// the device makes it in prepare(), off the audio thread).
inline const Phases& truePeakPhases() {
    static const Phases kPhases = [] {
        Phases phases{};
        constexpr int half = kTaps / 2;
        const double i0Beta = detail::besselI0(kKaiserBeta);
        for (int k = 1; k < kOversampling; ++k) {
            const double t = static_cast<double>(k) / kOversampling;
            double h[kTaps];
            double sum = 0.0;
            for (int i = 0; i < kTaps; ++i) {
                const double u = t - static_cast<double>(i - (half - 1));  // tap i reads x[m - 11 + i]
                const double sinc = std::sin(std::numbers::pi * u) / (std::numbers::pi * u);
                const double r = u / half;
                const double window = detail::besselI0(kKaiserBeta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0Beta;
                h[i] = sinc * window;
                sum += h[i];
            }
            Phase& phase = phases[static_cast<size_t>(k - 1)];
            for (int i = 0; i < kTaps; ++i) phase[static_cast<size_t>(i)] = static_cast<float>(h[i] / sum);
        }
        return phases;
    }();
    return kPhases;
}

// The signal at m + k / 8 from window[i] = x[m - 11 + i] (i = 0..23).
inline float interpolate(const float* window, const Phase& h) noexcept {
    float sum = 0.f;
    for (int i = 0; i < kTaps; ++i) sum += h[static_cast<size_t>(i)] * window[i];
    return sum;
}

// The peak of the parabola through three neighbouring points of the 8x grid,
// the middle one the largest (sign-corrected): y0 - (ym - yp)² / (8 (ym - 2 y0 + yp))
// when it curves down and y0 is at least both, else y0 (the peak is then in the
// neighbour's interval, which finds it). Never less than y0, the vertex at most
// half a step away.
inline float refinedPeak(float ym, float y0, float yp) noexcept {
    const float curvature = ym - 2.f * y0 + yp;
    if (!(curvature < 0.f) || y0 < ym || y0 < yp) return y0;
    const float slope = ym - yp;
    return y0 - slope * slope / (8.f * curvature);
}

// A gain as the gain reduction it is, in dB (positive): exactly 0 at 1.
inline float reductionDb(float gain) noexcept { return gain >= 1.f ? 0.f : -20.f * std::log10(std::max(1e-9f, gain)); }

}  // namespace sub::limiter
