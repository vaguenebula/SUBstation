#pragma once
// The Saturator's maths: its eight curves, Post Clip, and Color's emphasis
// filters. A curve is memoryless and odd (a symmetric input gets odd harmonics
// only, and no DC): Drive sets how far up it the input reaches, and what is
// past its straight part comes out rounded, clipped or folded.
//
// Color is an EQ around the curve: before it a low shelf at 150 Hz (Base) and a
// peak (Frequency, Width, Depth), after it their exact inverses. Where the
// curve is straight the two cancel, so a clean sound comes through unchanged;
// where it bends, a band boosted before it saturates harder and is turned back
// down after it (less of it in the result), and a band cut before it stays
// clean and is restored on top (more of it, ringing like a resonance).
//
// Shared by the device (builtin/devices/Saturator.cpp) and its editor (through
// the application layer's SaturatorResponse), so the curve drawn is the very
// arithmetic that plays: the same float functions, from the same parameters
// (makeShape(), params()).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "builtin/DspBlocks.h"
#include "rt/RtUtils.h"

namespace sub::saturator {

enum class Type { AnalogClip = 0, SoftSine, BassShaper, MediumCurve, HardCurve, SinoidFold, DigitalClip, Waveshaper };
enum class Clip { None = 0, Soft, Hard };

inline constexpr int kTypes = 8;
inline constexpr int kClips = 3;

inline constexpr float kAnalogKneeLow = 0.75f;     // Analog Clip: straight up to here,
inline constexpr float kAnalogKneeHigh = 1.25f;    // then a rounded corner reaching 1 here
inline constexpr float kHardCurveEdge = 1.5f;      // Hard Curve: its cubic reaches 1 here
inline constexpr float kRippleSpan = 15.f;         // Waveshaper: Period adds up to 15 more half-waves per unit
inline constexpr float kDampScale = 0.25f;         // Waveshaper: Damp's gate is -6 dB at 0.25 * damp²
inline constexpr float kWaveshaperLimit = 1000.f;  // its input is held within this (the cubic stays finite)
inline constexpr float kHalfPi = 1.57079632679f;
inline constexpr float kPiF = 3.14159265359f;
inline constexpr double kBaseHz = 150.0;  // Color's shelf
inline constexpr double kShelfQ = 0.7071;
inline constexpr double kMaxFreqFraction = 0.45;  // Color's peak is kept below this much of the sample rate

inline const std::vector<std::string>& typeLabels() {
    static const std::vector<std::string> kLabels = {"Analog Clip", "Soft Sine",   "Bass Shaper",  "Medium Curve",
                                                     "Hard Curve",  "Sinoid Fold", "Digital Clip", "Waveshaper"};
    return kLabels;
}
inline const std::vector<std::string>& clipLabels() {
    static const std::vector<std::string> kLabels = {"No Clip", "Soft Clip", "Hard Clip"};
    return kLabels;
}

// --- Small pieces ----------------------------------------------------------------------------

// sin(x) for any x, a few times cheaper than std::sin: reduced to |r| <= pi/2
// (in double, so a large argument keeps its phase), then the odd Taylor
// polynomial to r^11, within 6e-8 there. Beyond ±65536 it holds there.
inline float sine(float x) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    const double held = std::max(-65536.0, std::min(65536.0, static_cast<double>(x)));  // (a NaN holds at the top)
    const double k = std::floor(held * (1.0 / kPi) + 0.5);
    const auto r = static_cast<float>(held - k * kPi);
    const float r2 = r * r;
    const float p =
        r * (1.f + r2 * (-1.f / 6.f +
                         r2 * (1.f / 120.f + r2 * (-1.f / 5040.f + r2 * (1.f / 362880.f - r2 * (1.f / 39916800.f))))));
    return (static_cast<int64_t>(k) & 1) != 0 ? -p : p;
}

// 0 at 0, 1 at 1, flat at both ends: the crossfades' shape.
inline float sCurve(float t) noexcept { return t * t * (3.f - 2.f * t); }

// The Bass Shaper's Threshold (dB) as a level.
inline float thresholdGain(float db) noexcept { return expDbToGain(db); }

// --- The curves --------------------------------------------------------------------------------

// Straight to 0.75 (-2.5 dBFS), then u - (u - 0.75)² up to 1 at 1.25 (its slope
// falling smoothly to 0 there), then 1.
inline float analogClip(float u) noexcept {
    const float a = std::abs(u);
    if (a <= kAnalogKneeLow) return u;
    const float over = std::min(a, kAnalogKneeHigh) - kAnalogKneeLow;
    const float y = kAnalogKneeLow + over - over * over;
    return u < 0.f ? -y : y;
}

// sin(pi/2 u) up to full scale (a gain of pi/2 for quiet input), then 1.
inline float softSine(float u) noexcept {
    if (u >= 1.f) return 1.f;
    if (u <= -1.f) return -1.f;
    return sine(kHalfPi * u);
}

// Straight up to the threshold `t`, then a tanh from there to 1: a soft clip
// low down, a hard one at 0 dB. `span` is 1 / (1 - t) (0 when t is 1: a hard clip).
inline float bassShaper(float u, float t, float span) noexcept {
    const float a = std::abs(u);
    if (a <= t) return u;
    if (span == 0.f) return std::clamp(u, -1.f, 1.f);
    const float y = t + (1.f - t) * dsp::fastTanh((a - t) * span);
    return u < 0.f ? -y : y;
}

inline float mediumCurve(float u) noexcept { return dsp::fastTanh(u); }

// u - 4/27 u³ up to 1 at 1.5 (flat there), then 1.
inline float hardCurve(float u) noexcept {
    if (u >= kHardCurveEdge) return 1.f;
    if (u <= -kHardCurveEdge) return -1.f;
    return u - (4.f / 27.f) * u * u * u;
}

// sin(pi/2 u) for any u: up to full scale as Soft Sine, then folding back down.
inline float sinoidFold(float u) noexcept { return sine(kHalfPi * u); }

inline float digitalClip(float u) noexcept { return std::clamp(u, -1.f, 1.f); }

// The Waveshaper's six controls, as fractions (0..1).
struct Waveshaper {
    float drive = 0.5f, lin = 0.5f, curve = 0.5f, damp = 0.f, depth = 0.f, period = 0.f;
};

// A curve's settings: its type, the Bass Shaper's threshold (a level), the Waveshaper's controls.
struct Shape {
    Type type = Type::AnalogClip;
    float threshold = 0.125893f;  // -18 dB
    Waveshaper ws;
};

// What the curves work with, worked out from a Shape's settings (once, or per
// sample by the device while they glide).
struct Params {
    float threshold = 0.125893f;  // Bass Shaper: t
    float span = 1.144025f;       // 1 / (1 - t); 0 for a hard clip
    float drive = 0.5f;           // Waveshaper: its blend with the input
    float lin = 1.f;              // 2 lin: the straight part's slope
    float cubic = 1.f;            // 2 curve
    float depth = 0.f;            // the ripples' amplitude
    float omega = kPiF;           // pi (1 + 15 period): their density
    float damp = 0.f;             // (0.25 damp²)²: the gate's knee, squared
};

inline Params params(float threshold, const Waveshaper& ws) noexcept {
    Params p;
    p.threshold = threshold;
    p.span = 1.f - threshold < 1e-6f ? 0.f : 1.f / (1.f - threshold);
    p.drive = ws.drive;
    p.lin = 2.f * ws.lin;
    p.cubic = 2.f * ws.curve;
    p.depth = ws.depth;
    p.omega = kPiF * (1.f + kRippleSpan * ws.period);
    const float knee = kDampScale * ws.damp * ws.damp;
    p.damp = knee * knee;
    return p;
}
inline Params params(const Shape& s) noexcept { return params(s.threshold, s.ws); }

// v = u (held within ±1000); s = 2 lin v + 2 curve v³ + depth sin(pi (1 + 15 period) v);
// with Damp, s v² / (v² + knee²) (a gate near 0); then u + drive (tanh(s) - u):
// WS Drive 0 leaves the input as it is, 100 % is the shaper alone.
inline float waveshape(float u, const Params& p) noexcept {
    const float v = std::clamp(u, -kWaveshaperLimit, kWaveshaperLimit);
    const float v2 = v * v;
    float s = v * (p.lin + p.cubic * v2);
    if (p.depth != 0.f) s += p.depth * sine(p.omega * v);
    if (p.damp > 0.f) s *= v2 / (v2 + p.damp);
    return u + p.drive * (dsp::fastTanh(s) - u);
}

// A curve of a type at `u` (the input after Drive).
inline float curve(Type type, const Params& p, float u) noexcept {
    switch (type) {
    case Type::AnalogClip: return analogClip(u);
    case Type::SoftSine: return softSine(u);
    case Type::BassShaper: return bassShaper(u, p.threshold, p.span);
    case Type::MediumCurve: return mediumCurve(u);
    case Type::HardCurve: return hardCurve(u);
    case Type::SinoidFold: return sinoidFold(u);
    case Type::DigitalClip: return digitalClip(u);
    case Type::Waveshaper: return waveshape(u, p);
    }
    return u;
}

// Whether a type's curve reads the threshold or the Waveshaper's controls.
inline bool usesParams(Type type) noexcept { return type == Type::BassShaper || type == Type::Waveshaper; }

inline Type typeAt(int index) noexcept { return static_cast<Type>(std::clamp(index, 0, kTypes - 1)); }
inline Clip clipAt(int index) noexcept { return static_cast<Clip>(std::clamp(index, 0, kClips - 1)); }

// A Shape from the parameters as they are set: the type's index, the threshold
// in dB, the Waveshaper's controls in percent.
inline Shape makeShape(int type, float thresholdDb, float wsDrive, float wsLin, float wsCurve, float wsDamp,
                       float wsDepth, float wsPeriod) noexcept {
    Shape s;
    s.type = typeAt(type);
    s.threshold = thresholdGain(thresholdDb);
    s.ws = {wsDrive / 100.f, wsLin / 100.f, wsCurve / 100.f, wsDamp / 100.f, wsDepth / 100.f, wsPeriod / 100.f};
    return s;
}

inline float shape(const Shape& s, float u) noexcept { return curve(s.type, params(s), u); }

// After the curve (and Color's de-emphasis): Soft is the Analog Clip curve
// again, Hard a clip at 1, so the output never passes full scale (times Output).
inline float postClip(Clip clip, float y) noexcept {
    switch (clip) {
    case Clip::None: return y;
    case Clip::Soft: return analogClip(y);
    case Clip::Hard: return std::clamp(y, -1.f, 1.f);
    }
    return y;
}

// The curve as the editor draws it: Drive (a gain), the curve, Post Clip.
// (Color cancels where the curve is straight; Dry/Wet and Output aren't drawn.)
inline float transfer(const Shape& s, Clip clip, float driveGain, float x) noexcept {
    return postClip(clip, shape(s, driveGain * x));
}

// --- Color ----------------------------------------------------------------------------------

// Color's emphasis (before the curve): a low shelf at kBaseHz of Base dB, then
// a peak at Frequency of Depth dB, Width wide.
struct ColorDesign {
    dsp::BiquadCoefficients shelf, peak;
};

// Width (%) as the peak's Q: 0.25 · 16^(width / 100) octaves (a quarter of an
// octave to four), Q = 1 / (2 sinh(ln 2 / 2 · octaves)): 5.77, 1.41 at 50 %, 0.27.
inline double widthToQ(double widthPercent) noexcept {
    const double octaves = 0.25 * std::pow(16.0, std::clamp(widthPercent, 0.0, 100.0) / 100.0);
    return 1.0 / (2.0 * std::sinh(0.5 * 0.69314718055994531 * octaves));
}

// The peak's frequency as it plays: kept below 0.45 of the sample rate (at
// 22.05 kHz, 18.5 kHz plays at 9.9 kHz).
inline double colorFrequency(double freq, double sampleRate) noexcept {
    return std::clamp(freq, 1.0, kMaxFreqFraction * sampleRate);
}

inline dsp::BiquadCoefficients colorShelf(double baseDb, double sampleRate) noexcept {
    return dsp::BiquadCoefficients::lowShelf(kBaseHz, kShelfQ, baseDb, sampleRate);
}
inline dsp::BiquadCoefficients colorPeak(double freq, double widthPercent, double depthDb, double sampleRate) noexcept {
    return dsp::BiquadCoefficients::peak(colorFrequency(freq, sampleRate), widthToQ(widthPercent), depthDb, sampleRate);
}
inline ColorDesign colorDesign(double baseDb, double freq, double widthPercent, double depthDb,
                               double sampleRate) noexcept {
    return {colorShelf(baseDb, sampleRate), colorPeak(freq, widthPercent, depthDb, sampleRate)};
}

// A section's exact inverse, 1 / H: its numerator and denominator swapped and
// normalized again. Stable for the shelf and the peak, whose zeros (at any
// finite gain) are inside the unit circle.
inline dsp::BiquadCoefficients inverse(const dsp::BiquadCoefficients& c) noexcept {
    const double g = 1.0 / c.b0;
    return {g, c.a1 * g, c.a2 * g, c.b1 * g, c.b2 * g};
}

// Sample `k` (0..length - 1) of a straight line from section `a` to section `b`
// over `length` samples, landing exactly on `b`: how the device moves between
// the designs it works out every few samples while Color glides.
inline dsp::BiquadCoefficients between(const dsp::BiquadCoefficients& a, const dsp::BiquadCoefficients& b, int k,
                                       int length) noexcept {
    if (k + 1 >= length) return b;
    const double f = static_cast<double>(k + 1) / static_cast<double>(length);
    return {a.b0 + (b.b0 - a.b0) * f, a.b1 + (b.b1 - a.b1) * f, a.b2 + (b.b2 - a.b2) * f, a.a1 + (b.a1 - a.a1) * f,
            a.a2 + (b.a2 - a.a2) * f};
}

// The emphasis in dB at `f` Hz: what the editor draws (the de-emphasis is its mirror image).
inline double colorResponseDb(const ColorDesign& d, double f, double sampleRate) noexcept {
    return d.shelf.magnitudeDb(f, sampleRate) + d.peak.magnitudeDb(f, sampleRate);
}

// How long the emphasis and its inverse ring, in seconds (a time constant):
// the peak's poles decay in about Q·A / (pi f), its zeros (the inverse's poles)
// in Q / (pi f A), A = 10^(depth / 40); the shelf's corners sit at
// 150 Hz · A^(±1/2), A = 10^(base / 40). The slower of the four; 0 when both
// gains are 0 (the sections are then exactly 1).
inline double colorTimeConstant(double baseDb, double freq, double widthPercent, double depthDb,
                                double sampleRate) noexcept {
    constexpr double kPi = 3.14159265358979323846;
    double seconds = 0.0;
    if (baseDb != 0.0) seconds = 0.7071 / (kPi * kBaseHz * std::pow(10.0, -std::abs(baseDb) / 80.0));
    if (depthDb != 0.0) {
        const double a = std::pow(10.0, std::abs(depthDb) / 40.0);
        seconds = std::max(seconds, widthToQ(widthPercent) * a / (kPi * colorFrequency(freq, sampleRate)));
    }
    return seconds;
}

}  // namespace sub::saturator
