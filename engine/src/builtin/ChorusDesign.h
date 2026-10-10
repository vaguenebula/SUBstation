#pragma once
// The Chorus-Ensemble's voices: where each delay sits and how it moves, the
// warmth's curve and filter, and the feedback's limiter.
//
// Each voice reads the delay line at centre + swing * lfo (lfo in -1..1),
// both in ms and both set by the mode and Amount (0..1):
//
//   Layout                           Voices a side  Centre          Swing
//   Chorus, Time Auto                1 or 2 (Taps)  1.5 + 5 a ms    5 a ms
//   Chorus, Time 7/10/20/35/50 ms    1 or 2         that time       4 a ms
//   Ensemble                         3              7.5 ms          5 a ms
//   Vibrato                          1              6 ms            5.5 a ms
//
// Auto's centre grows with Amount (a short, gentle chorus at low Amount, a deep
// one at high: the classic device's dynamic delay); a fixed Time holds still
// whatever the Amount. The voices' LFO phases are spread so the two sides are
// always decorrelated: two taps a side are opposite (equal and opposite detune,
// their mean delay steady), Ensemble's six voices sit a sixth of a cycle apart,
// and Vibrato's sides are Offset apart. Chorus and Ensemble move by a sine;
// Vibrato's Shape morphs it towards a triangle in step with it.
//
// Shared by the device (builtin/devices/Chorus.cpp) and its editor's graph
// (through the application layer's ChorusVoices.h), so the delays drawn are the
// delays that play. All inline: no allocation, no Qt.

#include <algorithm>
#include <cmath>
#include <numbers>

#include "builtin/DspBlocks.h"

namespace sub::chorus {

enum class Mode { Chorus = 0, Ensemble, Vibrato };

constexpr double kPi = std::numbers::pi;
constexpr double kTwoPi = 2.0 * kPi;

constexpr double kMinRate = 0.1, kMaxRate = 15.0;  // Hz
constexpr double kMinHighPass = 20.0, kMaxHighPass = 2000.0;  // Hz
constexpr double kHighPassNyquistLimit = 0.45;  // of the sample rate: the highest the split goes
constexpr double kAutoMinMs = 1.5, kAutoSweepMs = 10.0;  // Auto: from 1.5 ms up, the sweep scaled by Amount
constexpr double kTimesMs[] = {0.0, 7.0, 10.0, 20.0, 35.0, 50.0};  // Time's fixed delays (index 0: Auto)
constexpr int kNumTimes = 6;
constexpr double kFixedDepthMs = 4.0;  // every fixed Time swings this far at Amount 100
constexpr double kEnsembleCentreMs = 7.5, kEnsembleDepthMs = 5.0;
constexpr double kVibratoCentreMs = 6.0, kVibratoDepthMs = 5.5;
constexpr double kMaxDelayMs = 60.0;  // the lines: the longest delay is 50 + 4 ms
constexpr int kMaxVoices = 3;         // a side
constexpr double kFeedbackScale = 0.97;  // Feedback 100 % is a loop gain of 0.97
constexpr double kWarmDrive = 0.65, kWarmBias = 0.15;  // the warmth's curve: a sigmoid of 0.65 (x + 0.15)
constexpr double kWarmSmallStep = 1e-5;  // steps smaller than this: the curve at their midpoint (anti-aliasing)
constexpr double kWarmCutoff = 4000.0, kWarmCutoffSlope = 0.6;  // the warmth's low-pass: 4 kHz w^-0.6
constexpr int kChunk = 16;            // samples per update of the modulation
constexpr int kDisplaySamples = 128;  // samples per display value (phase, level)

// Which voices play: the mode, and in Chorus mode the taps a side and the Time.
struct Layout {
    Mode mode = Mode::Chorus;
    int taps = 2;  // voices a side in Chorus mode (1 or 2); normalised: Ensemble 3, Vibrato 1
    int time = 0;  // Time's index (Chorus only; 0 elsewhere)
    bool operator==(const Layout&) const = default;
};

// From the parameters' indices (mode 0..2, taps 0..1, time 0..5, clamped),
// normalised so a change that doesn't apply to the mode (Taps while in
// Ensemble) is the same layout.
inline Layout layout(int mode, int tapsIndex, int timeIndex) noexcept {
    Layout l;
    l.mode = static_cast<Mode>(std::clamp(mode, 0, 2));
    switch (l.mode) {
    case Mode::Chorus:
        l.taps = std::clamp(tapsIndex, 0, 1) + 1;
        l.time = std::clamp(timeIndex, 0, kNumTimes - 1);
        break;
    case Mode::Ensemble:
        l.taps = 3;
        l.time = 0;
        break;
    case Mode::Vibrato:
        l.taps = 1;
        l.time = 0;
        break;
    }
    return l;
}

// Voices a side.
inline int voices(const Layout& l) noexcept { return std::clamp(l.taps, 1, kMaxVoices); }

// The unmodulated delay in ms (Auto's grows with `amount`, 0..1).
inline double centreMs(const Layout& l, double amount) noexcept {
    switch (l.mode) {
    case Mode::Ensemble: return kEnsembleCentreMs;
    case Mode::Vibrato: return kVibratoCentreMs;
    case Mode::Chorus: break;
    }
    if (l.time <= 0) return kAutoMinMs + 0.5 * kAutoSweepMs * amount;
    return kTimesMs[std::min(l.time, kNumTimes - 1)];
}

// How far the delay moves either way of the centre, in ms.
inline double swingMs(const Layout& l, double amount) noexcept {
    switch (l.mode) {
    case Mode::Ensemble: return kEnsembleDepthMs * amount;
    case Mode::Vibrato: return kVibratoDepthMs * amount;
    case Mode::Chorus: break;
    }
    return (l.time <= 0 ? 0.5 * kAutoSweepMs : kFixedDepthMs) * amount;
}

// The delay's range at Amount 100: the editor's axis, and (the highest) the tail's.
inline double lowestMs(const Layout& l) noexcept { return centreMs(l, 1.0) - swingMs(l, 1.0); }
inline double highestMs(const Layout& l) noexcept { return centreMs(l, 1.0) + swingMs(l, 1.0); }

// A voice's place in the LFO's cycle (cycles, added to the LFO's phase), on
// `channel` 0 (left) or 1 (right); `offsetCycles` is Vibrato's Offset (0..0.5).
inline double voicePhase(const Layout& l, int channel, int voice, double offsetCycles) noexcept {
    const double c = channel > 0 ? 1.0 : 0.0;
    switch (l.mode) {
    case Mode::Ensemble: return c / 6.0 + voice / 3.0;   // 0, 1/3, 2/3 | 1/6, 1/2, 5/6
    case Mode::Vibrato: return c * offsetCycles;          // 0 | Offset
    case Mode::Chorus: break;
    }
    if (voices(l) == 1) return 0.5 * c;   // 0 | 1/2
    return 0.25 * c + 0.5 * voice;        // 0, 1/2 | 1/4, 3/4
}

// A triangle in step with sin(2 pi phase): 0 at 0, 1 at 0.25, -1 at 0.75.
inline double triangle(double phase) noexcept {
    const double p = phase + 0.25;
    return 1.0 - 4.0 * std::abs(p - std::floor(p) - 0.5);
}

// The modulation at absolute phase `phase` (cycles): a sine, or in Vibrato
// the sine morphed towards the triangle by `shape` (0..1).
inline double lfoValue(Mode mode, double phase, double shape) noexcept {
    const double sine = std::sin(kTwoPi * phase);
    if (mode != Mode::Vibrato || shape <= 0.0) return sine;
    return sine + shape * (triangle(phase) - sine);
}

inline double delayMs(const Layout& l, double amount, double lfo) noexcept {
    return centreMs(l, amount) + swingMs(l, amount) * lfo;
}

// The modulation's steepest slope per cycle (at phase 0): 2 pi for the sine,
// 4 for the triangle, between for the morph.
inline double peakSlope(Mode mode, double shape) noexcept {
    const double s = mode == Mode::Vibrato ? std::clamp(shape, 0.0, 1.0) : 0.0;
    return (1.0 - s) * kTwoPi + 4.0 * s;
}

// The largest speed the delay moves at, in ms per ms (0..0.99): how far a voice's
// pitch goes from the input's either way.
inline double peakDelaySpeed(const Layout& l, double rate, double amount, double shape) noexcept {
    return std::clamp(swingMs(l, amount) / 1000.0 * rate * peakSlope(l.mode, shape), 0.0, 0.99);
}

// The largest detune up and down, in cents (both positive): a delay shrinking at
// speed m plays 1 + m times as fast, growing 1 - m. The down one is the larger.
inline double detuneUpCents(const Layout& l, double rate, double amount, double shape) noexcept {
    return 1200.0 * std::log2(1.0 + peakDelaySpeed(l, rate, amount, shape));
}
inline double peakDetuneCents(const Layout& l, double rate, double amount, double shape) noexcept {
    return -1200.0 * std::log2(1.0 - peakDelaySpeed(l, rate, amount, shape));
}

// What is fed back: x within ±1; beyond, sign(x) (1 + tanh(|x| - 1)): smooth
// (its slope 1) at ±1, never beyond ±2.
inline float limitFeedback(float x) noexcept {
    const float a = std::abs(x);
    if (a <= 1.f) return x;
    const float limited = 1.f + dsp::fastTanh(a - 1.f);
    return x < 0.f ? -limited : limited;
}

// The warmth's curve, all of it at Warmth 100: a soft sigmoid, u / sqrt(1 + u²)
// of u = 0.65 (x + 0.15) (the bias for even harmonics), its offset taken out (0
// in, 0 out) and its slope at silence brought back to 1 (quiet sounds pass at
// their level). Gentle, a colour rather than a limiter: a sine at -6 dBFS comes
// out 0.3 dB down with 2.5 % THD (its 2nd harmonic -33 dBc, its 3rd -39 dBc), at
// 0 dBFS 1.1 dB down with 5.4 %; it never goes beyond +1.41 or -1.71. Its
// harmonics are within a few dB of a tanh's at this drive, and its
// antiderivative, sqrt(1 + u²), costs only a square root (the anti-aliasing's;
// a tanh's, log cosh, would cost an exp and a log).
struct WarmCurve {
    double offset = sigmoid(kWarmDrive * kWarmBias);
    double scale = 1.0 / (kWarmDrive * slope(kWarmDrive * kWarmBias));

    double operator()(double x) const noexcept { return scale * (sigmoid(kWarmDrive * (x + kWarmBias)) - offset); }
    // Its antiderivative (less a constant): scale ((sqrt(1 + u²) - 1) / drive - offset x).
    double integral(double x) const noexcept {
        const double u = kWarmDrive * (x + kWarmBias);
        return scale * (u * u / (1.0 + std::sqrt(1.0 + u * u)) / kWarmDrive - offset * x);
    }

    static double sigmoid(double u) noexcept { return u / std::sqrt(1.0 + u * u); }
    static double slope(double u) noexcept { return 1.0 / ((1.0 + u * u) * std::sqrt(1.0 + u * u)); }
};

// The warmth at `w` (0..1) on one value: x + w (curve(x) - x), exactly x at
// w = 0, and 0 at 0 at any w. (The device plays it through WarmStage.)
inline double warm(double w, double x) noexcept {
    const WarmCurve curve;
    return x + w * (curve(x) - x);
}

// The warmth as the device plays it, one value after another: x + w b, where
// the bend b (the curve less the identity) is averaged over the step from the
// last value to this one, (B(x) - B(x')) / (x - x') with B its antiderivative
// (first-order antiderivative anti-aliasing): a bright sound's harmonics above
// Nyquist fold back 10 to 17 dB lower than through the curve itself. The
// identity part has no delay or droop, so Warmth 0 is exact and quiet sounds
// pass as they are; the bend is half a sample late, which a feedback loop of
// at least 0.5 ms doesn't feel. No allocation; a square root a value.
class WarmStage {
public:
    void reset() noexcept {
        last_ = 0.0;
        known_ = false;
    }
    // The value passes untouched (Warmth 0); it is remembered for when Warmth comes up.
    void pass(double x) noexcept {
        last_ = x;
        known_ = false;
    }
    double process(double x, double w) noexcept {
        const double step = x - last_;
        double bend;
        if (std::abs(step) < kWarmSmallStep) {  // (the quotient loses its precision: the curve at the midpoint)
            const double middle = 0.5 * (x + last_);
            bend = curve_(middle) - middle;
            known_ = false;
        } else {
            if (!known_) lastIntegral_ = curve_.integral(last_);
            const double integral = curve_.integral(x);
            bend = (integral - lastIntegral_) / step - 0.5 * (x + last_);
            lastIntegral_ = integral;
            known_ = true;
        }
        last_ = x;
        return x + w * bend;
    }

private:
    WarmCurve curve_;
    double last_ = 0.0;
    double lastIntegral_ = 0.0;  // the curve's integral at last_, while known_
    bool known_ = false;
};

// The warmth low-pass's one-pole coefficient (dsp::OnePole's): its cutoff
// 4 kHz w^-0.6 (about 16 kHz at w 0.1, 6 kHz at 0.5, 4 kHz at 1), exactly 0 (no
// filtering) at w = 0 and continuous as w grows from there.
inline double warmLowpassCoefficient(double w, double sampleRate) noexcept {
    if (w <= 0.0) return 0.0;
    const double cutoff = kWarmCutoff * std::pow(std::min(w, 1.0), -kWarmCutoffSlope);
    return std::exp(-kTwoPi * cutoff / sampleRate);
}

// The high-pass's frequency as it plays: 20..2000 Hz, at most 0.45 of the rate.
inline double highPassFrequency(double freq, double sampleRate) noexcept {
    return std::min(std::clamp(freq, kMinHighPass, kMaxHighPass), kHighPassNyquistLimit * sampleRate);
}

}  // namespace sub::chorus
