#pragma once
// The Phaser-Flanger's maths: its modes and LFO shapes, how the modulation
// moves the sweep in each mode, and what plays as a linear filter. Shared by the
// device (builtin/devices/Phaser.cpp) and its editor's graph (through the
// application layer's PhaserResponse.h), so the curve drawn is worked out from
// the very mappings and stages the engine plays.
//
// - Phaser: N identical second-order all-passes (the Disperser's stages,
//   DisperserDesign.h) tuned to the centre, their Q the Spread. Mixed with the
//   input at an even blend, every frequency the cascade turns an odd number of
//   half circles cancels: N notches, closer together the higher the Q.
// - Flanger and Doubler: the input delayed (0.1-20 ms, a comb; 20-150 ms, a
//   second take). Both feed back, with or without a polarity flip.
// - Warmth darkens and gently saturates what the effect puts out; Safe Bass
//   keeps everything below its frequency dry (a Linkwitz-Riley split).

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <numbers>
#include <string>
#include <vector>

#include "builtin/DisperserDesign.h"
#include "builtin/DspBlocks.h"

namespace sub::phaser {

enum class Mode { Phaser = 0, Flanger, Doubler };
enum class Wave {
    Sine = 0, Triangle, TriangleAnalog, Triangle8, Triangle16, SawUp, SawDown, Rectangle, Random, RandomHold
};

constexpr double kPi = std::numbers::pi;

constexpr int kMaxNotches = 42;
constexpr double kMinCenter = 70.0, kMaxCenter = 18500.0;  // the Center parameter's range (abl.dsp.phaser~'s)
constexpr double kMinRate = 0.01, kMaxRate = 5.0;          // Hz: Freq and Freq 2
constexpr double kMinStageHz = 10.0;        // the lowest the modulation takes the stages
constexpr double kMaxQ = 5.0, kMinQ = 0.15;  // Spread 0 % .. 100 %
constexpr double kPhaserOctaves = 3.0;      // modulation 1.0 moves Center this many octaves
constexpr double kSpreadSwing = 0.5;        // modulation 1.0 moves Spread by half its range (Blend 1)
constexpr double kFlangerOctaves = 2.0;     // modulation 1.0 moves the flanger's delay two octaves
constexpr double kDoublerDepth = 0.15;      // modulation 1.0 moves the doubler's delay 15 %
constexpr double kMaxFeedback = 0.95;       // Feedback 100 %
constexpr double kMaxDelayMs = 250.0;       // the delay lines' longest read
constexpr double kMinDelaySamples = 2.0;    // the shortest (DelayLine::hermite reads one newer sample)
constexpr double kWarmthCutoffHz = 5000.0;  // Warmth's one-pole low-pass
constexpr double kWarmthDrive = 2.0;        // Warmth's saturation: tanh(2 y) / 2
constexpr double kAnalogSeconds = 0.1;      // Triangle Analog: the rectangle's RC time constant
constexpr double kSafeBassOff = 5.0;        // Hz: Safe Bass at (or below) this is off
constexpr double kEnvRangeDb = 48.0;        // the envelope's 0..1 spans -48..0 dBFS
constexpr double kModLimit = 2.0;           // the summed modulation is held to ±2
constexpr double kSafetyKnee = 2.0, kSafetyLimit = 4.0;  // the feedback path's soft limit

// Timing, shared with the tests.
constexpr int kChunk = 16;                  // frames per control chunk
constexpr int kDisplaySamples = 256;        // frames per display value
constexpr double kGlideSeconds = 0.020;     // each of the two one-poles of the continuous controls' glides
constexpr double kTimeGlideSeconds = 0.050; // the same for the delay times (a gentler swoop of pitch)
constexpr double kGainSeconds = 0.005;      // each of the two one-poles of Feedback, Warmth, Dry/Wet, Output
constexpr double kModSeconds = 0.001;       // each of the two one-poles smoothing the summed modulation
constexpr double kOffsetSeconds = 0.05;     // the right LFO's offset gliding to Phase
constexpr double kLfoFadeSeconds = 0.02;    // an LFO's value crossfading over a jump (waveform, sync, transport)
constexpr double kNotchFadeSeconds = 0.02;  // a change of Notches
constexpr double kModeFadeSeconds = 0.03;   // a change of Mode
constexpr double kSafeFadeSeconds = 0.02;   // Safe Bass switching on or off

inline const std::vector<std::string>& modeLabels() {
    static const std::vector<std::string> kLabels = {"Phaser", "Flanger", "Doubler"};
    return kLabels;
}

inline const std::vector<std::string>& waveLabels() {
    static const std::vector<std::string> kLabels = {
        "Sine",   "Triangle", "Triangle Analog", "Triangle 8", "Triangle 16",
        "Saw Up", "Saw Down", "Rectangle",       "Random",     "Random S&H",
    };
    return kLabels;
}

// The stages' Q for a Spread of 0..1: 5 at 0, 0.866 at a half, 0.15 at 1 (evenly in log).
// Unclamped, the same line beyond (the modulation's soft edges, below).
inline double qOfSpreadUnclamped(double spread) noexcept {
    constexpr double kLog2Max = 2.321928094887362;     // log2(kMaxQ)
    constexpr double kLog2Ratio = -5.058893689053568;  // log2(kMinQ / kMaxQ)
    return std::exp2(kLog2Max + kLog2Ratio * spread);
}
inline double qOfSpread(double spread01) noexcept { return qOfSpreadUnclamped(std::clamp(spread01, 0.0, 1.0)); }

// How far the modulation may take Spread past its range, and the knee it gets
// there: as it is within 0..1, then bending smoothly (tanh: its slope and
// curvature carry on) towards -kSpreadEdge or 1 + kSpreadEdge. A hard clamp
// would put a corner in the sweep wherever it reached an end, which the stages
// turn into a kink in what they put out, at the LFO's rate.
constexpr double kSpreadEdge = 0.1;
inline double softSpread(double spread) noexcept {
    if (spread > 1.0) return 1.0 + kSpreadEdge * std::tanh((spread - 1.0) / kSpreadEdge);
    if (spread < 0.0) return kSpreadEdge * std::tanh(spread / kSpreadEdge);
    return spread;
}

// The loop gain of Feedback (0..100 %), negative with the polarity flipped (Ø).
inline double feedbackGain(double percent, bool invert) noexcept {
    const double g = kMaxFeedback * std::clamp(percent, 0.0, 100.0) / 100.0;
    return invert ? -g : g;
}

// The envelope follower's peak level as 0..1: -48 dBFS (and below) 0, 0 dBFS 1.
inline double envelopeAmount(double peakLevel) noexcept {
    if (!(peakLevel > 0.0)) return 0.0;
    return std::clamp(1.0 + 20.0 * std::log10(peakLevel) / kEnvRangeDb, 0.0, 1.0);
}

// Duty Cycle's bend of a phase (0..1; duty -1..1): the first half of the shape
// takes `w` of the cycle (5 % .. 95 %), the second the rest. At 0, the phase as it is.
inline double dutyWidth(double duty) noexcept { return 0.5 + 0.45 * std::clamp(duty, -1.0, 1.0); }
inline double warpPhase(double phase, double duty) noexcept {
    const double w = dutyWidth(duty);
    return phase < w ? 0.5 * phase / w : 0.5 + 0.5 * (phase - w) / (1.0 - w);
}

// Triangle Analog: a ±1 rectangle (high for the fraction `w` of the cycle)
// through a one-pole of kAnalogSeconds, in its steady state. Slow, it is nearly
// square; fast, a quieter rounded triangle (0.46 high at 5 Hz): its shape and
// level follow the rate, as an analog LFO's do. y0 is its value as a cycle
// starts, y1 where the rectangle falls.
struct AnalogShape {
    double w = 0.5, periodOverTau = 20.0, y0 = -1.0, y1 = 1.0;
};

inline AnalogShape analogShape(double duty, double rateHz) noexcept {
    AnalogShape s;
    s.w = dutyWidth(duty);
    s.periodOverTau = 1.0 / std::max(rateHz, 1e-3) / kAnalogSeconds;
    if (s.periodOverTau < 1e-6) {  // (1 - ab would lose its precision: analogValue gives the limit, 2w - 1)
        s.y0 = s.y1 = 2.0 * s.w - 1.0;
        return s;
    }
    const double a = std::exp(-s.w * s.periodOverTau), b = std::exp(-(1.0 - s.w) * s.periodOverTau);
    s.y0 = (2.0 * b - 1.0 - a * b) / (1.0 - a * b);
    s.y1 = 1.0 + (s.y0 - 1.0) * a;
    return s;
}

inline double analogValue(const AnalogShape& s, double phase) noexcept {
    if (s.periodOverTau < 1e-6) return 2.0 * s.w - 1.0;
    if (phase < s.w) return 1.0 + (s.y0 - 1.0) * std::exp(-phase * s.periodOverTau);
    return -1.0 + (s.y1 + 1.0) * std::exp(-(phase - s.w) * s.periodOverTau);
}

// A shape's value (-1..1) at `phase` (0..1) of cycle number `cycle` (the random
// shapes' values come from it), bent by `duty` (-1..1). `rateHz` is only
// Triangle Analog's (its shape follows the rate).
inline float waveValue(Wave wave, double phase, uint32_t cycle, double duty, double rateHz) noexcept {
    using dsp::Lfo;
    using dsp::LfoShape;
    switch (wave) {
    case Wave::Sine: return Lfo::shape(LfoShape::Sine, warpPhase(phase, duty), cycle);
    case Wave::Triangle: return Lfo::shape(LfoShape::Triangle, warpPhase(phase, duty), cycle);
    case Wave::TriangleAnalog: return static_cast<float>(analogValue(analogShape(duty, rateHz), phase));
    case Wave::Triangle8:
    case Wave::Triangle16: {
        const double steps = wave == Wave::Triangle8 ? 8.0 : 16.0;
        return Lfo::shape(LfoShape::Triangle, std::floor(warpPhase(phase, duty) * steps) / steps, cycle);
    }
    case Wave::SawUp: return Lfo::shape(LfoShape::SawUp, warpPhase(phase, duty), cycle);
    case Wave::SawDown: return Lfo::shape(LfoShape::SawDown, warpPhase(phase, duty), cycle);
    case Wave::Rectangle: return phase < dutyWidth(duty) ? 1.f : -1.f;
    case Wave::Random: return Lfo::shape(LfoShape::RandomSmooth, warpPhase(phase, duty), cycle);
    case Wave::RandomHold: return Lfo::shape(LfoShape::Random, warpPhase(phase, duty), cycle);
    }
    return 0.f;
}

// A synced rate in Hz: one cycle per division (dsp::syncedDivisionLabels()) at the tempo.
inline double syncedRateHz(int division, double tempo) noexcept {
    return std::max(0.0, tempo) / 60.0 / dsp::syncedCycleBeats(division);
}

// --- The modulation's mappings (mod: the summed, smoothed modulation, -2..2) -------------

// The stages' frequency for a centre given in log2 Hz: moved kPhaserOctaves per
// unit of modulation (less as Blend gives the modulation to Spread), held
// between kMinStageHz and the stages' limit below Nyquist.
inline double centerHzAt(double log2Center, double blend, double mod, double sampleRate) noexcept {
    const double hz = std::exp2(log2Center + (1.0 - blend) * kPhaserOctaves * mod);
    return std::clamp(hz, kMinStageHz, disperser::kNyquistLimit * sampleRate);
}
inline double phaserCenterHz(double centerHz, double blend, double mod, double sampleRate) noexcept {
    return centerHzAt(std::log2(std::clamp(centerHz, kMinCenter, kMaxCenter)), blend, mod, sampleRate);
}

// The stages' Q: Spread (0..1) moved by Blend's share of the modulation, with soft edges.
inline double phaserQ(double spread01, double blend, double mod) noexcept {
    return qOfSpreadUnclamped(softSpread(std::clamp(spread01, 0.0, 1.0) + blend * kSpreadSwing * mod));
}

// A delay in samples, as the line reads it: at least kMinDelaySamples, at most kMaxDelayMs.
inline double delaySamples(double delayMs, double sampleRate) noexcept {
    return std::clamp(delayMs * sampleRate / 1000.0, kMinDelaySamples, kMaxDelayMs * sampleRate / 1000.0);
}

// The flanger's delay for a time in ms (two octaves per unit of modulation),
// and the doubler's (15 % per unit), unclamped.
inline double flangerMsAt(double timeMs, double mod) noexcept { return timeMs * std::exp2(kFlangerOctaves * mod); }
inline double doublerMsAt(double timeMs, double mod) noexcept { return timeMs * (1.0 + kDoublerDepth * mod); }

// The delay a delay mode plays (Phaser: the flanger's mapping), in ms, held to what the line reads.
inline double delayMs(Mode mode, double timeMs, double mod, double sampleRate) noexcept {
    const double ms = mode == Mode::Doubler ? doublerMsAt(timeMs, mod) : flangerMsAt(timeMs, mod);
    return delaySamples(ms, sampleRate) * 1000.0 / sampleRate;
}

// --- What plays, as a linear filter (Warmth's saturation left out) ------------------------

struct Response {
    Mode mode = Mode::Phaser;
    int notches = 4;
    double centerHz = 1000.0, q = 0.866;  // Phaser: the stages as tuned now
    double delayMs = 2.5;                 // Flanger/Doubler: the delay now
    double feedback = 0.0;                // the signed loop gain (feedbackGain)
    double warmth = 0.0;                  // 0..1
    double mix = 0.5;                     // 0..1
    double safeBassHz = kSafeBassOff;     // ≤ kSafeBassOff: off
    double outputDb = 0.0;
};

namespace detail {

// Warmth as a filter: (1 - warmth) + warmth L, L its one-pole low-pass (as OnePole::lowpass runs it).
inline std::complex<double> warmthTransfer(double warmth, std::complex<double> z1, double sampleRate) noexcept {
    if (warmth <= 0.0) return 1.0;
    const double a = dsp::onePoleCutoff(kWarmthCutoffHz, sampleRate);
    return (1.0 - warmth) + warmth * (1.0 - a) / (1.0 - a * z1);
}

// Safe Bass's bands at a frequency: the Linkwitz-Riley crossover's low (LP²) and high (HP²), the
// bilinear transforms of Butterworth sections (the TPT state-variable filters dsp::Crossover runs),
// written in z so that Nyquist needs no special case.
struct Bands {
    bool on = false;
    std::complex<double> low = 0.0, high = 1.0;
};
inline Bands safeBands(double safeBassHz, std::complex<double> z1, double sampleRate) noexcept {
    Bands b;
    if (!(safeBassHz > kSafeBassOff + 1e-3)) return b;
    b.on = true;
    const double g = std::tan(kPi * std::clamp(safeBassHz, 1.0, 0.49 * sampleRate) / sampleRate);
    const std::complex<double> minus = 1.0 - z1, plus = 1.0 + z1;
    const std::complex<double> den = minus * minus + std::sqrt(2.0) * g * minus * plus + g * g * plus * plus;
    const std::complex<double> lp = g * g * plus * plus / den, hp = minus * minus / den;
    b.low = lp * lp;
    b.high = hp * hp;
    return b;
}

// Output as a gain.
inline double outputGain(const Response& r) noexcept { return std::pow(10.0, r.outputDb / 20.0); }

// The whole device from the wet path's transfer: Dry/Wet, Safe Bass's bands, Output (its `gain`).
inline std::complex<double> combine(const Response& r, std::complex<double> wet, const Bands& b, double gain) noexcept {
    const double mix = std::clamp(r.mix, 0.0, 1.0);
    const std::complex<double> h = b.on ? b.low + (1.0 - mix) * b.high + mix * wet * b.high : (1.0 - mix) + mix * wet;
    return h * gain;
}

// The wet path from the turning factor T (the cascade's A^N, or the delay's e^{-jwd}).
inline std::complex<double> wetFrom(const Response& r, std::complex<double> turn, std::complex<double> warmth,
                                    std::complex<double> z1) noexcept {
    // The Phaser's feedback is taken a sample late (z1); the delay's loop is the delay itself.
    const std::complex<double> path = warmth * turn;
    return r.mode == Mode::Phaser ? path / (1.0 - r.feedback * z1 * path) : path / (1.0 - r.feedback * path);
}

inline double toDb(double magnitude) noexcept { return 20.0 * std::log10(std::max(magnitude, 1e-6)); }
inline double toDb(std::complex<double> h) noexcept { return toDb(std::abs(h)); }

// The Phaser's stage (any other mode's is unused). Designed once for a whole curve, not per point.
inline disperser::Stage stageOf(const Response& r, double sampleRate) noexcept {
    return r.mode == Mode::Phaser ? disperser::design(r.centerHz, r.q, sampleRate) : disperser::Stage{};
}

// The wet path at w with the stage given (wetTransfer, below).
inline std::complex<double> wetAt(const Response& r, const disperser::Stage& stage, double w,
                                  double sampleRate) noexcept {
    const std::complex<double> z1 = std::polar(1.0, -w);
    std::complex<double> turn;
    if (r.mode == Mode::Phaser) {
        const std::complex<double> a = disperser::response(stage, w);
        const int n = std::clamp(r.notches, 1, kMaxNotches);
        turn = std::polar(std::pow(std::abs(a), n), n * std::arg(a));
    } else {
        turn = std::polar(1.0, -w * delaySamples(r.delayMs, sampleRate));
    }
    return wetFrom(r, turn, warmthTransfer(r.warmth, z1, sampleRate), z1);
}

// The whole device at `freqHz` with the stage and Output's gain given (transfer, below).
inline std::complex<double> transferAt(const Response& r, const disperser::Stage& stage, double gain, double freqHz,
                                       double sampleRate) noexcept {
    const double w = 2.0 * kPi * std::clamp(freqHz, 0.0, 0.5 * sampleRate) / sampleRate;
    const std::complex<double> z1 = std::polar(1.0, -w);
    return combine(r, wetAt(r, stage, w, sampleRate), safeBands(r.safeBassHz, z1, sampleRate), gain);
}

}  // namespace detail

// The wet path's transfer at w (radians per sample): Phaser, W A^N / (1 - g z^-1 W A^N);
// Flanger/Doubler, W D / (1 - g W D) with D = e^{-jwd}. W is Warmth's filter.
inline std::complex<double> wetTransfer(const Response& r, double w, double sampleRate) noexcept {
    return detail::wetAt(r, detail::stageOf(r, sampleRate), w, sampleRate);
}

// The whole device's transfer at `freqHz`.
inline std::complex<double> transfer(const Response& r, double freqHz, double sampleRate) noexcept {
    return detail::transferAt(r, detail::stageOf(r, sampleRate), detail::outputGain(r), freqHz, sampleRate);
}

// Its level at `freqHz`, in dB, floored at -120 dB.
inline double responseDb(const Response& r, double freqHz, double sampleRate) noexcept {
    return detail::toDb(transfer(r, freqHz, sampleRate));
}

// A stage's phase at freqHz, unwrapped: 0 at DC falling to -2π at Nyquist (-π
// at the stage's frequency). The bilinear transform keeps the analog
// all-pass's phase at the warped frequency Ω = tan(π f / sr) / tan(π f0 / sr):
// -2 atan2(Ω / Q, 1 - Ω²).
inline double stagePhase(double freqHz, double centerHz, double q, double sampleRate) noexcept {
    if (freqHz >= 0.5 * sampleRate) return -2.0 * kPi;
    const double f0 = disperser::stageFrequency(centerHz, sampleRate);
    const double omega = std::tan(kPi * std::max(0.0, freqHz) / sampleRate) / std::tan(kPi * f0 / sampleRate);
    const double pinch = std::clamp(q, disperser::kMinPinch, disperser::kMaxPinch);
    return -2.0 * std::atan2(omega / pinch, 1.0 - omega * omega);
}

// Where the notches are with no feedback, rising, in Hz: where N stages turn
// the phase an odd number of half circles, N φ = -(2k - 1) π for k = 1..N. With
// θ = (2k - 1) π / 2N that is Ω² + (cot θ / Q) Ω - 1 = 0, whose positive root
// is mapped back through the bilinear transform's warp.
inline std::vector<double> notchFrequencies(int notches, double centerHz, double q, double sampleRate) {
    std::vector<double> out;
    const int n = std::clamp(notches, 1, kMaxNotches);
    const double t0 = std::tan(kPi * disperser::stageFrequency(centerHz, sampleRate) / sampleRate);
    const double pinch = std::clamp(q, disperser::kMinPinch, disperser::kMaxPinch);
    out.reserve(static_cast<size_t>(n));
    for (int k = 1; k <= n; ++k) {
        const double theta = (2.0 * k - 1.0) * kPi / (2.0 * n);
        const double b = std::cos(theta) / std::sin(theta) / pinch;
        const double omega = 0.5 * (-b + std::sqrt(b * b + 4.0));
        out.push_back(sampleRate / kPi * std::atan(omega * t0));
    }
    return out;
}

// The comb's notches with no feedback, (2k - 1) / 2d, rising, below `highHz`, at most `limit` of them.
inline std::vector<double> combNotchFrequencies(double delayMs, double highHz, int limit) {
    std::vector<double> out;
    if (!(delayMs > 0.0)) return out;
    const double spacing = 1000.0 / delayMs;
    for (int k = 0; k < limit; ++k) {
        const double f = (k + 0.5) * spacing;
        if (f >= highHz) break;
        out.push_back(f);
    }
    return out;
}

// The curve the editor draws, `columns` columns from lowHz to highHz (evenly in
// log), worked out so that every notch is drawn at its depth and a comb denser
// than the columns is drawn as its envelope, never as aliasing that flickers.
struct CurvePoint {
    double freqHz = 0.0, db = 0.0;
};
struct Curve {
    std::vector<CurvePoint> line;     // the polyline: the curve where it is sparse, its top where dense; rising
    std::vector<double> top, bottom;  // per column: the highest and lowest the response reaches in it (dB)
    std::vector<uint8_t> dense;       // per column: 1 where the wet path turns a whole cycle or more within it
};

// In each column the wet path turns its phase by Δψ (Phaser: N times a stage's
// turn across it; the delay modes: 2π d times its width). Less than a cycle:
// up to 12 points a column (one per 30° of turn), the notches inside it added
// where they fall, so each is drawn at its depth. A cycle or more: the
// column's other factors (Warmth, Safe Bass's bands) at its centre, and the
// turning factor swept round the circle at 48 fixed angles, its top and bottom
// the highest and lowest of them; the same angles every time, so the band
// stays still as the sweep moves through it. Frequencies at or above 0.499 of
// the rate take that frequency's value. Allocates (the editor's and the tests').
inline void curve(const Response& r, double lowHz, double highHz, int columns, double sampleRate, Curve& out) {
    columns = std::max(1, columns);
    out.line.clear();
    out.top.assign(static_cast<size_t>(columns), 0.0);
    out.bottom.assign(static_cast<size_t>(columns), 0.0);
    out.dense.assign(static_cast<size_t>(columns), 0);
    const double nyquist = 0.499 * sampleRate;
    const double ratio = highHz / lowHz;
    const auto edge = [&](int c) {
        return c >= columns ? highHz : lowHz * std::pow(ratio, static_cast<double>(c) / columns);
    };
    // (as responseDb, the stage and Output's gain worked out once)
    const disperser::Stage stage = detail::stageOf(r, sampleRate);
    const double gain = detail::outputGain(r);
    const auto db = [&](double f) {
        return detail::toDb(detail::transferAt(r, stage, gain, std::min(f, nyquist), sampleRate));
    };
    const bool phaser = r.mode == Mode::Phaser;
    const int n = std::clamp(r.notches, 1, kMaxNotches);
    const double delaySeconds = delaySamples(r.delayMs, sampleRate) / sampleRate;
    const std::vector<double> notches =
        phaser ? notchFrequencies(n, r.centerHz, r.q, sampleRate) : std::vector<double>{};
    size_t nextNotch = 0;

    for (int c = 0; c < columns; ++c) {
        const double f0 = edge(c), f1 = edge(c + 1);
        const double a = std::min(f0, nyquist), b = std::min(f1, nyquist);
        const double turn = phaser ? n * std::abs(stagePhase(b, r.centerHz, r.q, sampleRate) -
                                                  stagePhase(a, r.centerHz, r.q, sampleRate))
                                   : 2.0 * kPi * (b - a) * delaySeconds;
        double top = -1e300, bottom = 1e300;
        const auto add = [&](double f, bool onLine) {
            const double v = db(f);
            top = std::max(top, v);
            bottom = std::min(bottom, v);
            if (onLine && (out.line.empty() || f > out.line.back().freqHz)) out.line.push_back({f, v});
        };
        if (turn < 2.0 * kPi) {
            const int steps = std::clamp(static_cast<int>(std::ceil(turn / (kPi / 6.0))), 1, 12);
            // The notches inside [f0, f1): the Phaser's from its list, the comb's worked out.
            std::vector<double> inside;
            if (phaser) {
                while (nextNotch < notches.size() && notches[nextNotch] < f0) ++nextNotch;
                for (size_t k = nextNotch; k < notches.size() && notches[k] < f1; ++k) inside.push_back(notches[k]);
            } else if (delaySeconds > 0.0) {
                for (double k = std::ceil(f0 * delaySeconds - 0.5); (k + 0.5) / delaySeconds < f1; k += 1.0) {
                    const double f = (k + 0.5) / delaySeconds;
                    if (f >= f0) inside.push_back(f);
                }
            }
            size_t k = 0;
            for (int j = 0; j < steps; ++j) {
                const double f = f0 * std::pow(f1 / f0, static_cast<double>(j) / steps);
                for (; k < inside.size() && inside[k] <= f; ++k) add(inside[k], true);
                add(f, true);
            }
            for (; k < inside.size(); ++k) add(inside[k], true);
            add(f1, c == columns - 1);  // the column's far edge (the next column's first point)
        } else {
            const double centre = std::min(std::sqrt(f0 * f1), nyquist);
            const double w = 2.0 * kPi * centre / sampleRate;
            const std::complex<double> z1 = std::polar(1.0, -w);
            const std::complex<double> warmth = detail::warmthTransfer(r.warmth, z1, sampleRate);
            const detail::Bands bands = detail::safeBands(r.safeBassHz, z1, sampleRate);
            static const std::array<std::complex<double>, 48> kTurns = [] {
                std::array<std::complex<double>, 48> turns;
                for (int i = 0; i < 48; ++i) turns[static_cast<size_t>(i)] = std::polar(1.0, -2.0 * kPi * i / 48.0);
                return turns;
            }();
            double most = 0.0, least = 1e300;
            for (const std::complex<double>& turnFactor : kTurns) {
                const double v = std::abs(detail::combine(r, detail::wetFrom(r, turnFactor, warmth, z1), bands, gain));
                most = std::max(most, v);
                least = std::min(least, v);
            }
            top = detail::toDb(most);
            bottom = detail::toDb(least);
            out.dense[static_cast<size_t>(c)] = 1;
            if (out.line.empty() || f0 > out.line.back().freqHz) out.line.push_back({f0, top});
        }
        out.top[static_cast<size_t>(c)] = top;
        out.bottom[static_cast<size_t>(c)] = bottom;
    }
}

}  // namespace sub::phaser
