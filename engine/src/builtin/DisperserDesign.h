#pragma once
// The Disperser's filters: a cascade of identical second-order all-passes.
// Each stage passes every frequency at the same level and turns its phase
// through a full circle around the stage's frequency, the more abruptly the
// higher its Q (the Pinch), so what is around that frequency comes out later
// than the rest: a group delay that peaks near the frequency and that the
// stages add up. A transient comes out as a chirp; nothing is louder or quieter.
//
// A stage is the bilinear transform of the analog all-pass
// (s² - s/Q + 1) / (s² + s/Q + 1), prewarped to the stage's frequency w0 (as
// the RBJ cookbook's): as a biquad (A2 + A1 z^-1 + z^-2) / (1 + A1 z^-1 + A2 z^-2),
// A1 = -2 cos w0 / (1 + a), A2 = (1 - a) / (1 + a), a = sin w0 / 2Q. Its
// magnitude is exactly 1.
//
// It runs as a normalized lattice (Gray and Markel): two rotations, by the
// reflection coefficients k1 = A1 / (1 + A2) = -cos w0 and k2 = A2, each with
// its c = sqrt(1 - k²). A rotation keeps the energy of what it turns, so a
// stage's output and states together hold exactly the energy that went in,
// however its coefficients move from one sample to the next: sweeping the
// frequency or the pinch can't make anything louder than what the stages have
// taken in (a state-variable filter can: its band-pass state holds Q times the
// signal, and lowering Q while it rings throws that out at once).
//
// Shared by the device (builtin/devices/Disperser.cpp) and its editor's graph
// (groupDelayMs(), through the application layer's disperserGroupDelayMs()), so
// the curve drawn is worked out from the very coefficients the engine plays.

#include <algorithm>
#include <cmath>
#include <complex>

namespace sub::disperser {

constexpr int kMaxStages = 64;
constexpr double kMinFrequency = 20.0;  // Hz: the Frequency parameter's range
constexpr double kMaxFrequency = 20000.0;
constexpr double kNyquistLimit = 0.45;  // of the sample rate: the highest a stage is tuned to
constexpr double kMinPinch = 0.1;       // the stages' Q
constexpr double kMaxPinch = 10.0;
constexpr double kPi = 3.14159265358979323846;

// The frequency the stages are tuned to: `freq`, kept below Nyquist (at 44.1 kHz,
// 20 kHz plays as 19.8 kHz).
inline double stageFrequency(double freq, double sampleRate) noexcept {
    return std::min(std::max(freq, 1.0), kNyquistLimit * sampleRate);
}

// One stage's coefficients (every stage and channel share them): the lattice's
// two rotations.
struct Stage {
    double k1 = 0.0, c1 = 1.0;  // -cos w0, sin w0
    double k2 = 0.0, c2 = 1.0;  // (1 - a) / (1 + a), 2 sqrt(a) / (1 + a)

    // The same filter as a biquad.
    double biquadA1() const noexcept { return k1 * (1.0 + k2); }
    double biquadA2() const noexcept { return k2; }
};

// The stages for a frequency (Hz) and a pinch (Q).
inline Stage design(double freq, double pinch, double sampleRate) noexcept {
    const double w0 = 2.0 * kPi * stageFrequency(freq, sampleRate) / sampleRate;
    const double a = std::sin(w0) / (2.0 * std::clamp(pinch, kMinPinch, kMaxPinch));
    Stage s;
    s.k1 = -std::cos(w0);
    s.c1 = std::sin(w0);
    s.k2 = (1.0 - a) / (1.0 + a);
    s.c2 = 2.0 * std::sqrt(a) / (1.0 + a);
    return s;
}

// A stage's two states (per channel).
struct State {
    double d1 = 0.0, d2 = 0.0;
};

// One sample through a stage: its all-pass output. The first rotation gives the
// output, straight from the input (one multiply-add on the path through the
// cascade); the second turns what goes on into the states.
inline double process(const Stage& s, State& state, double x) noexcept {
    const double f = s.c2 * x - s.k2 * state.d2;
    const double y = s.k2 * x + s.c2 * state.d2;
    const double d1 = s.c1 * f - s.k1 * state.d1;
    state.d2 = s.k1 * f + s.c1 * state.d1;
    state.d1 = d1;
    return y;
}

// A stage's response at w (radians per sample): magnitude 1, the phase it turns.
inline std::complex<double> response(const Stage& s, double w) noexcept {
    const double A1 = s.biquadA1(), A2 = s.biquadA2();
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
    return (A2 + A1 * z1 + z2) / (1.0 + A1 * z1 + A2 * z2);
}

// A stage's group delay at w, in samples: -d(phase)/dw, from its coefficients
// (for each polynomial, Re(sum n c_n z^-n / sum c_n z^-n); the numerator's less the
// denominator's).
inline double groupDelaySamples(const Stage& s, double w) noexcept {
    const double A1 = s.biquadA1(), A2 = s.biquadA2();
    const std::complex<double> z1 = std::polar(1.0, -w), z2 = z1 * z1;
    const auto delay = [&](double c0, double c1, double c2) {
        return std::real((c1 * z1 + 2.0 * c2 * z2) / (c0 + c1 * z1 + c2 * z2));
    };
    return delay(A2, A1, 1.0) - delay(1.0, A1, A2);
}

// The group delay of `stages` stages tuned to `freq` (Hz) with `pinch`, at
// `frequency` (Hz; above Nyquist: Nyquist's), in milliseconds: the curve the
// Disperser's editor draws.
inline double groupDelayMs(int stages, double freq, double pinch, double sampleRate, double frequency) noexcept {
    if (stages <= 0 || !(sampleRate > 0.0)) return 0.0;
    const double w = 2.0 * kPi * std::clamp(frequency, 0.0, 0.5 * sampleRate) / sampleRate;
    return stages * groupDelaySamples(design(freq, pinch, sampleRate), w) * 1000.0 / sampleRate;
}

// Where a stage delays most, in radians per sample: its poles' angle (a complex
// pair), else 0 Hz. High up, the bilinear transform squeezes this peak into a
// band narrower than an even spread of frequencies can be sure to hit, so
// whatever looks for the largest delay looks here too.
inline double peakAngle(const Stage& s) noexcept {
    const double A1 = s.biquadA1(), A2 = s.biquadA2();
    if (A1 * A1 - 4.0 * A2 >= 0.0) return 0.0;
    return std::acos(std::clamp(-A1 / (2.0 * std::sqrt(A2)), -1.0, 1.0));
}

// A stage's largest group delay, in samples: at its peak, or anywhere from 1e-5
// of Nyquist up to Nyquist (evenly in log).
inline double maxGroupDelaySamples(const Stage& s) noexcept {
    double longest = groupDelaySamples(s, peakAngle(s));
    for (int i = 0; i <= 256; ++i) {
        longest = std::max(longest, groupDelaySamples(s, kPi * std::pow(1e-5, 1.0 - i / 256.0)));
    }
    return longest;
}

// How fast a stage rings down: the -log of its slowest pole's radius, per sample.
inline double decayPerSample(const Stage& s) noexcept {
    const double A1 = s.biquadA1(), A2 = s.biquadA2();
    const double discriminant = A1 * A1 - 4.0 * A2;
    double radius = std::sqrt(std::max(0.0, A2));  // a complex pair
    if (discriminant >= 0.0) {                     // two real poles: the larger
        const double root = std::sqrt(discriminant);
        radius = 0.5 * std::max(std::abs(-A1 + root), std::abs(-A1 - root));
    }
    return -std::log(std::clamp(radius, 1e-9, 1.0 - 1e-12));
}

}  // namespace sub::disperser
