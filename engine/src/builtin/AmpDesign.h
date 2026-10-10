#pragma once
// The Amp's design (builtin/devices/Amp.cpp): seven amp models as voicings of
// one structure, the triode stages' curve with its anti-aliasing, the passive
// tone stack, the dials' mappings, and the curves its editor draws.
//
// A model is an input stage (a coupling high-pass and a bright cap's shelf),
// three triode stages (V1, V2, then the tone stack, then V3), Presence (a high
// shelf in the power amp's feedback), a push-pull power stage whose supply
// sags under load, and the output transformer. Everything between the input
// stage and the transformer runs 4x oversampled.
//
// - A stage is a Miller low-pass, a gain, a cubic soft clipper around its bias
//   point (a positive bias clips the top first: the even harmonics of a triode
//   driven into grid current) and a coupling high-pass. The curve is
//   1.5 x - 0.5 x³ inside |x| < 1 and ±1 outside; its antiderivative is a
//   polynomial, so first-order antiderivative anti-aliasing (ADAA, Parker,
//   Zavalishin and Le Bivic, DAFx-16) costs a division per sample and no tanh
//   or log. With 4x oversampling it keeps what folds back 55 to 90 dB down,
//   every dial at 10 included.
// - The tone stack is the passive Fender/Marshall/Vox stack as Yeh and Smith
//   model the '59 Bassman's (DAFx-06): a third-order filter whose coefficients
//   are polynomials in the three pots' positions, so the controls interact as
//   a real amp's do. Each model has its own component values, and a make-up
//   gain that puts its response at noon at 0 dB at its peak.
// - Gain sets the preamp's input level (split between V1 and V3 on the
//   high-gain models), Volume the power stage's drive (±12 dB around noon).
//   A one-pole low-pass at the power tubes' input (kGridHz) sits between
//   Presence and the power stage.
//
// Softube's models are unpublished: this is a behavioural design, built to do
// what Live's manual says each control does and to sound like the amps the
// models are known to be.
//
// Shared by the device and, through the application layer's AmpResponse.h, its
// editor: the tone curve (toneResponseDb) and the transfer curve (Transfer) are
// worked out from the very numbers and functions the engine plays. The device
// also keeps a model morph level-matched with Transfer's levels.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <numbers>
#include <string>
#include <vector>

#include "builtin/DspBlocks.h"

namespace sub::amp {

inline constexpr int kModels = 7;
inline constexpr int kOversamplingLog2 = 2;  // 4x
inline constexpr int kOversampling = 1 << kOversamplingLog2;
inline constexpr double kPi = std::numbers::pi;
inline constexpr double kDcBlockerHz = 10.0;  // after the oversampled section
// The device's displays: audio per value, and the floor of those in dB (the editor takes the floor
// through the application layer's AmpResponse.h).
inline constexpr int kDisplaySamples = 256;
inline constexpr float kDisplayFloorDb = -90.f;
// The power tubes' input (their grids' Miller capacitance): a one-pole low-pass
// between Presence and the power stage. It keeps the edges of a preamp already
// clipped and Presence's boost from reaching the power stage's curve at full
// height (what folds back from there: 10 to 13 dB less with every dial at 10),
// and takes 0.03 dB off 1 kHz.
inline constexpr double kGridHz = 12000.0;

enum Model { Clean = 0, Boost, Blues, Rock, Lead, Heavy, Bass };

// The models' names, in the Amp Type parameter's order.
inline const std::vector<std::string>& modelLabels() {
    static const std::vector<std::string> kLabels = {"Clean", "Boost", "Blues", "Rock", "Lead", "Heavy", "Bass"};
    return kLabels;
}

// --- The stages' curve --------------------------------------------------------------------

// 1.5 x - 0.5 x³ inside |x| < 1, ±1 outside: slope 1.5 at 0, flat (C1) where it
// meets ±1. (Clamped first, so it has no branches: at ±1 it is exactly ±1.)
inline double shape(double x) noexcept {
    const double c = std::min(1.0, std::max(-1.0, x));
    return c * (1.5 - 0.5 * c * c);
}

// Its antiderivative: 0.75 x² - 0.125 x⁴ inside, |x| - 0.375 outside (continuous
// at ±1: 0.625). Outside, |x| - 0.375 is exact (for |x| < 2^50), which Adaa relies on.
inline double shapeIntegral(double x) noexcept {
    const double a = std::abs(x), x2 = x * x;
    const double inside = x2 * (0.75 - 0.125 * x2);
    return a >= 1.0 ? a - 0.375 : inside;
}

// Its slope: 1.5 (1 - x²) inside, 0 outside.
inline double shapeSlope(double x) noexcept { return std::abs(x) >= 1.0 ? 0.0 : 1.5 * (1.0 - x * x); }

// What a stage biased at `bias` puts out at rest: the curve at the bias, worked
// out as Adaa::process() works it out for a constant input (bit for bit), so a
// stage at rest that subtracts it puts out exact zeros.
inline double restValue(double bias) noexcept { return shape(0.5 * (bias + bias)); }

// First-order antiderivative anti-aliasing of shape(): (F(x) - F(x0)) / (x - x0),
// which is shape() averaged over the straight line from the last input to this
// one. For small signals it is 1.5 (x0 + x) / 2: half a sample late. When the
// two are closer than 1e-6 (where the division would cancel) it is shape() at
// their midpoint. Two clipped samples give exactly ±1: F's clipped part is
// exact, so F(x) - F(x0) rounds as x - x0 does. No branches: every case is
// worked out and the right one chosen, so successive samples overlap in the CPU.
struct Adaa {
    double x0 = 0.0, f0 = 0.0;  // the last input and shapeIntegral() of it

    // As if it had been fed `x` for ever: what a stage at rest holds (its input
    // is its bias in silence).
    void prime(double x) noexcept {
        x0 = x;
        f0 = shapeIntegral(x);
    }
    void reset() noexcept { prime(0.0); }

    double process(double x) noexcept {
        const double f = shapeIntegral(x);
        const double d = x - x0;
        const bool close = std::abs(d) < 1e-6;
        const double ratio = (f - f0) / (close ? 1.0 : d);
        const double y = close ? shape(0.5 * (x0 + x)) : ratio;
        x0 = x;
        f0 = f;
        return y;
    }
};

// --- The voicings -------------------------------------------------------------------------

// A triode stage: its small-signal gain, where it clips (a signal level re 1.0),
// its bias (asymmetry, 0..0.45), the Miller low-pass before its curve and the
// coupling high-pass after it.
struct Stage {
    double levelDb, headDb, bias, lowpassHz, highpassHz;
};

// The tone stack's parts (ohms, farads): treble pot R1, bass pot R2, mid pot R3,
// slope resistor R4, treble cap C1, bass cap C2, mid cap C3.
struct ToneParts {
    double r1, r2, r3, r4, c1, c2, c3;
};

struct Voicing {
    double inputHighpassHz;       // the input coupling: a first-order high-pass
    double brightHz, brightDb;    // a first-order high shelf before V1 (a bright cap)
    double gainMinDb, gainMaxDb;  // Gain 0 and 10, in dB (linear in the dial)
    double gainSplit;             // Gain's share at V1; the rest at V3
    Stage stages[3];              // V1, V2, V3 (the tone stack is between V2 and V3)
    ToneParts tone;
    double toneMakeupDb;          // toneMakeupDb(tone), a constant here
    double presenceHz, presenceRangeDb;
    double volumeDb;              // the power stage's drive at Volume 5 (2.4 dB a step: ±12 dB)
    double powerBias;
    double sag, sagAttackMs, sagReleaseMs;
    double transformerHz;
    double trimDb;                // output trim: level-matched at the defaults
};

inline constexpr ToneParts kVox = {1e6, 1e6, 25e3, 47e3, 100e-12, 22e-9, 47e-9};
inline constexpr ToneParts kFender = {250e3, 1e6, 25e3, 56e3, 250e-12, 20e-9, 20e-9};
inline constexpr ToneParts kMarshall = {220e3, 1e6, 22e3, 33e3, 470e-12, 22e-9, 22e-9};
inline constexpr ToneParts kModern = {250e3, 250e3, 25e3, 47e3, 500e-12, 22e-9, 22e-9};
inline constexpr ToneParts kVintage = {250e3, 1e6, 25e3, 47e3, 500e-12, 22e-9, 22e-9};
inline constexpr ToneParts kPa = {250e3, 1e6, 25e3, 100e3, 250e-12, 47e-9, 22e-9};

// Each model, at 48 kHz with every dial at 5 and a 220 Hz sine at -12 dBFS
// (peak): its RMS out -15 dBFS (the trims), and
// - Clean: V1 12 dB under its clipping point (4 % THD, second harmonic well
//   over the third), V2 a clean cathode follower, a strong V3 into a quiet
//   power stage that only breaks up with hot signals.
// - Boost: V1 at the knee, V2 pushed past it: crunch with the Vox stack.
// - Blues: a clean preamp, the Fender stack, a power stage near its knee with
//   the deepest sag: Volume adds distortion.
// - Rock: V1 at the knee into a cascaded V2: the Plexi's crunch, a late stack.
// - Lead: Gain split between V1 and V3 around an early stack, V2 and V3 far
//   over; an 80 Hz input and 120 Hz coupling keep it tight; little sag.
// - Heavy: the same structure looser and darker, V2 and V3 at their knees at
//   noon (a crunch, 34 % THD, the even harmonics strong), so the power stage,
//   which sags and is driven hard, adds distortion with Volume (44 % at 10).
// - Bass: big asymmetric bias at V1 and V2 (fuzz), the PA stack's lows, a
//   power stage that distorts with Volume.
inline constexpr Voicing kVoicings[kModels] = {
    // Clean
    {30.0, 2500.0, 4.0, -18.0, 18.0, 1.0,
     {{0.0, 0.0, 0.25, 12000.0, 20.0}, {0.0, 12.0, 0.0, 16000.0, 15.0}, {12.0, 6.0, 0.10, 14000.0, 20.0}},
     kVox, 4.41088204116207, 3000.0, 8.0, -12.0, 0.05, 0.35, 15.0, 150.0, 11000.0, 6.0},
    // Boost
    {50.0, 1800.0, 6.0, -6.0, 30.0, 1.0,
     {{0.0, 0.0, 0.30, 10000.0, 30.0}, {6.0, 0.0, 0.20, 8000.0, 40.0}, {6.0, 6.0, 0.10, 12000.0, 20.0}},
     kVox, 4.41088204116207, 3000.0, 8.0, -10.0, 0.05, 0.35, 15.0, 150.0, 10000.0, -0.3},
    // Blues
    {25.0, 1200.0, 5.0, -12.0, 24.0, 1.0,
     {{0.0, 0.0, 0.20, 12000.0, 20.0}, {0.0, 18.0, 0.0, 18000.0, 10.0}, {6.0, 3.0, 0.15, 11000.0, 30.0}},
     kFender, 4.590662590180511, 4500.0, 9.0, 0.0, 0.0, 0.60, 10.0, 250.0, 12000.0, -5.3},
    // Rock
    {35.0, 1500.0, 4.0, -6.0, 30.0, 1.0,
     {{0.0, 0.0, 0.30, 10000.0, 30.0}, {6.0, -3.0, 0.25, 7500.0, 60.0}, {0.0, 12.0, 0.0, 16000.0, 10.0}},
     kMarshall, 3.477602963774195, 3500.0, 9.0, -5.0, 0.03, 0.40, 12.0, 180.0, 10000.0, -0.4},
    // Lead
    {80.0, 2500.0, 3.0, 0.0, 30.0, 0.5,
     {{0.0, 0.0, 0.30, 9000.0, 30.0}, {12.0, -6.0, 0.35, 7000.0, 120.0}, {12.0, -6.0, 0.25, 6500.0, 80.0}},
     kModern, 4.21872455726953, 4000.0, 9.0, -6.0, 0.02, 0.15, 5.0, 80.0, 9000.0, 1.1},
    // Heavy
    {40.0, 1000.0, 0.0, -6.0, 32.0, 0.5,
     {{0.0, 0.0, 0.35, 8000.0, 20.0}, {3.0, -3.0, 0.40, 6000.0, 60.0}, {-3.0, -3.0, 0.30, 6000.0, 50.0}},
     kVintage, 4.221411743695151, 3500.0, 9.0, 5.0, 0.05, 0.50, 10.0, 200.0, 8500.0, -7.6},
    // Bass
    {15.0, 1000.0, 0.0, -12.0, 30.0, 1.0,
     {{0.0, 0.0, 0.40, 7000.0, 15.0}, {6.0, 0.0, 0.45, 6000.0, 20.0}, {0.0, 12.0, 0.0, 12000.0, 10.0}},
     kPa, 5.07784385902861, 2500.0, 9.0, 3.0, 0.10, 0.55, 15.0, 300.0, 7500.0, -5.1},
};

inline const Voicing& voicing(int model) noexcept { return kVoicings[std::clamp(model, 0, kModels - 1)]; }

// Between two voicings, `t` of the way (0: a, 1: b, exactly): dB values,
// biases, Gain's split and the sag's amount linearly; frequencies, the sag's
// times and the tone stack's parts geometrically. Every field moves
// continuously, so a morph moves every coefficient continuously.
inline Voicing blend(const Voicing& a, const Voicing& b, double t) noexcept {
    if (t <= 0.0) return a;
    if (t >= 1.0) return b;
    const auto lin = [t](double x, double y) { return (1.0 - t) * x + t * y; };
    const auto geo = [t](double x, double y) { return std::exp((1.0 - t) * std::log(x) + t * std::log(y)); };
    Voicing v;
    v.inputHighpassHz = geo(a.inputHighpassHz, b.inputHighpassHz);
    v.brightHz = geo(a.brightHz, b.brightHz);
    v.brightDb = lin(a.brightDb, b.brightDb);
    v.gainMinDb = lin(a.gainMinDb, b.gainMinDb);
    v.gainMaxDb = lin(a.gainMaxDb, b.gainMaxDb);
    v.gainSplit = lin(a.gainSplit, b.gainSplit);
    for (int k = 0; k < 3; ++k) {
        const Stage& x = a.stages[k];
        const Stage& y = b.stages[k];
        v.stages[k] = {lin(x.levelDb, y.levelDb), lin(x.headDb, y.headDb), lin(x.bias, y.bias),
                       geo(x.lowpassHz, y.lowpassHz), geo(x.highpassHz, y.highpassHz)};
    }
    v.tone = {geo(a.tone.r1, b.tone.r1), geo(a.tone.r2, b.tone.r2), geo(a.tone.r3, b.tone.r3),
              geo(a.tone.r4, b.tone.r4), geo(a.tone.c1, b.tone.c1), geo(a.tone.c2, b.tone.c2),
              geo(a.tone.c3, b.tone.c3)};
    v.toneMakeupDb = lin(a.toneMakeupDb, b.toneMakeupDb);
    v.presenceHz = geo(a.presenceHz, b.presenceHz);
    v.presenceRangeDb = lin(a.presenceRangeDb, b.presenceRangeDb);
    v.volumeDb = lin(a.volumeDb, b.volumeDb);
    v.powerBias = lin(a.powerBias, b.powerBias);
    v.sag = lin(a.sag, b.sag);
    v.sagAttackMs = geo(a.sagAttackMs, b.sagAttackMs);
    v.sagReleaseMs = geo(a.sagReleaseMs, b.sagReleaseMs);
    v.transformerHz = geo(a.transformerHz, b.transformerHz);
    v.trimDb = lin(a.trimDb, b.trimDb);
    return v;
}

// --- The dials ----------------------------------------------------------------------------

// Gain, in dB: from the model's Gain 0 to its Gain 10, linear in the dial.
inline double gainDb(const Voicing& v, double gain) noexcept {
    return v.gainMinDb + (v.gainMaxDb - v.gainMinDb) * std::clamp(gain, 0.0, 10.0) / 10.0;
}
// The power stage's drive, in dB: ±12 dB around the model's noon (Volume 0 is
// quieter, not silent; Volume 10 keeps the hottest output under +3.5 dBFS).
inline double volumeDb(const Voicing& v, double volume) noexcept {
    return v.volumeDb + 2.4 * (std::clamp(volume, 0.0, 10.0) - 5.0);
}
// Presence's shelf, in dB: flat at 5, ±presenceRangeDb at the ends.
inline double presenceDb(const Voicing& v, double presence) noexcept {
    return (std::clamp(presence, 0.0, 10.0) - 5.0) / 5.0 * v.presenceRangeDb;
}
// The bass pot's position for its dial: an audio taper (9 % at noon, as a real bass pot).
inline double bassTaper(double bass) noexcept {
    return (std::pow(10.0, std::clamp(bass, 0.0, 10.0) / 5.0) - 1.0) / 99.0;
}

// --- The tone stack -----------------------------------------------------------------------

// The analog stack's coefficients for pot positions t (treble), m (middle), l
// (bass), each 0..1: H(s) = (b1 s + b2 s² + b3 s³) / (1 + a1 s + a2 s² + a3 s³)
// (Yeh and Smith, checked against a nodal analysis of the circuit).
struct ToneAnalog {
    double b1, b2, b3, a1, a2, a3;
};

inline ToneAnalog toneAnalog(const ToneParts& p, double t, double m, double l) noexcept {
    const double R1 = p.r1, R2 = p.r2, R3 = p.r3, R4 = p.r4, C1 = p.c1, C2 = p.c2, C3 = p.c3;
    ToneAnalog a;
    a.b1 = t * C1 * R1 + m * C3 * R3 + l * (C1 * R2 + C2 * R2) + (C1 * R3 + C2 * R3);
    a.b2 = t * (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
           m * (C1 * C3 * R1 * R3 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
           l * (C1 * C2 * R1 * R2 + C1 * C2 * R2 * R4 + C1 * C3 * R2 * R4) +
           l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) +
           (C1 * C2 * R1 * R3 + C1 * C2 * R3 * R4 + C1 * C3 * R3 * R4);
    a.b3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
           m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
           m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) + t * C1 * C2 * C3 * R1 * R3 * R4 -
           t * m * C1 * C2 * C3 * R1 * R3 * R4 + t * l * C1 * C2 * C3 * R1 * R2 * R4;
    a.a1 = (C1 * R1 + C1 * R3 + C2 * R3 + C2 * R4 + C3 * R4) + m * C3 * R3 + l * (C1 * R2 + C2 * R2);
    a.a2 = m * (C1 * C3 * R1 * R3 - C2 * C3 * R3 * R4 + C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
           l * m * (C1 * C3 * R2 * R3 + C2 * C3 * R2 * R3) - m * m * (C1 * C3 * R3 * R3 + C2 * C3 * R3 * R3) +
           l * (C1 * C2 * R2 * R4 + C1 * C2 * R1 * R2 + C1 * C3 * R2 * R4 + C2 * C3 * R2 * R4) +
           (C1 * C2 * R1 * R4 + C1 * C3 * R1 * R4 + C1 * C2 * R3 * R4 + C1 * C2 * R1 * R3 + C1 * C3 * R3 * R4 +
            C2 * C3 * R3 * R4);
    a.a3 = l * m * (C1 * C2 * C3 * R1 * R2 * R3 + C1 * C2 * C3 * R2 * R3 * R4) -
           m * m * (C1 * C2 * C3 * R1 * R3 * R3 + C1 * C2 * C3 * R3 * R3 * R4) +
           m * (C1 * C2 * C3 * R3 * R3 * R4 + C1 * C2 * C3 * R1 * R3 * R3 - C1 * C2 * C3 * R1 * R3 * R4) +
           l * C1 * C2 * C3 * R1 * R2 * R4 + C1 * C2 * C3 * R1 * R3 * R4;
    return a;
}

// The pots' positions for the dials (0..10): treble and middle linear, bass an audio taper.
inline ToneAnalog toneAnalogForDials(const ToneParts& p, double bass, double middle, double treble) noexcept {
    return toneAnalog(p, std::clamp(treble, 0.0, 10.0) / 10.0, std::clamp(middle, 0.0, 10.0) / 10.0, bassTaper(bass));
}

// The analog stack's response at `freq` Hz (no make-up).
inline std::complex<double> toneStackAnalog(const ToneParts& p, double bass, double middle, double treble,
                                            double freq) noexcept {
    const ToneAnalog a = toneAnalogForDials(p, bass, middle, treble);
    const std::complex<double> s(0.0, 2.0 * kPi * freq);
    return (a.b1 * s + a.b2 * s * s + a.b3 * s * s * s) / (1.0 + a.a1 * s + a.a2 * s * s + a.a3 * s * s * s);
}

// The digital stack: y = (b0 + b1 z⁻¹ + b2 z⁻² + b3 z⁻³) x / (1 + a1 z⁻¹ + a2 z⁻² + a3 z⁻³).
struct ToneCoefficients {
    double b0 = 0.0, b1 = 0.0, b2 = 0.0, b3 = 0.0, a1 = 0.0, a2 = 0.0, a3 = 0.0;
};

// The bilinear transform of the analog stack at `rate` (the oversampled rate),
// without prewarping: the poles sit far below the rate, and at 192 kHz the
// digital response is within 0.005 dB of the analog one from 30 Hz to 16 kHz.
inline ToneCoefficients toneStack(const ToneParts& p, double bass, double middle, double treble, double rate) noexcept {
    const ToneAnalog a = toneAnalogForDials(p, bass, middle, treble);
    const double c = 2.0 * rate, c2 = c * c, c3 = c2 * c;
    const double B0 = a.b1 * c + a.b2 * c2 + a.b3 * c3;
    const double B1 = a.b1 * c - a.b2 * c2 - 3.0 * a.b3 * c3;
    const double B2 = -a.b1 * c - a.b2 * c2 + 3.0 * a.b3 * c3;
    const double B3 = -a.b1 * c + a.b2 * c2 - a.b3 * c3;
    const double A0 = 1.0 + a.a1 * c + a.a2 * c2 + a.a3 * c3;
    const double A1 = 3.0 + a.a1 * c - a.a2 * c2 - 3.0 * a.a3 * c3;
    const double A2 = 3.0 - a.a1 * c - a.a2 * c2 + 3.0 * a.a3 * c3;
    const double A3 = 1.0 - a.a1 * c + a.a2 * c2 - a.a3 * c3;
    const double k = 1.0 / A0;
    return {B0 * k, B1 * k, B2 * k, B3 * k, A1 * k, A2 * k, A3 * k};
}

// The stack's state: transposed direct form II, in double (its lowest pole can
// sit at a few hertz at the oversampled rate).
struct ToneState {
    double z1 = 0.0, z2 = 0.0, z3 = 0.0;

    double process(const ToneCoefficients& c, double x) noexcept {
        const double y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y + z3;
        z3 = c.b3 * x - c.a3 * y;
        return y;
    }
    void reset() noexcept { z1 = z2 = z3 = 0.0; }
    // Zero once all three are tiny, never one at a time: with its poles near
    // z = 1 the states nearly cancel, and clearing some of them would kick the
    // stack back into ringing (for ever, at the threshold's level).
    void flush() noexcept {
        if (std::max({std::abs(z1), std::abs(z2), std::abs(z3)}) < 1e-20) reset();
    }
    bool atRest() const noexcept { return z1 == 0.0 && z2 == 0.0 && z3 == 0.0; }
};

// z⁻¹ at `freq` Hz: what the responses below are worked out at (several of them
// at one frequency can share it).
inline std::complex<double> unitDelay(double freq, double rate) noexcept {
    return std::polar(1.0, -2.0 * kPi * freq / rate);
}

// a / b, worked out plainly: std::complex's division guards against overflow and
// infinities, which these responses never meet, at several times the cost.
inline std::complex<double> divide(std::complex<double> a, std::complex<double> b) noexcept {
    const double d = 1.0 / (b.real() * b.real() + b.imag() * b.imag());
    return {(a.real() * b.real() + a.imag() * b.imag()) * d, (a.imag() * b.real() - a.real() * b.imag()) * d};
}

// The digital stack's response at z⁻¹ = `z`, and at `freq` Hz.
inline std::complex<double> toneStackDigital(const ToneCoefficients& c, std::complex<double> z) noexcept {
    return divide(c.b0 + z * (c.b1 + z * (c.b2 + z * c.b3)), 1.0 + z * (c.a1 + z * (c.a2 + z * c.a3)));
}
inline std::complex<double> toneStackDigital(const ToneCoefficients& c, double freq, double rate) noexcept {
    return toneStackDigital(c, unitDelay(freq, rate));
}

// What kVoicings' toneMakeupDb holds for a stack: minus the analog response's
// maximum in dB with every dial at 5, over 64 log-spaced frequencies from 40 Hz
// to 10 kHz, so each model's stack peaks at 0 dB at noon.
inline double toneMakeupDb(const ToneParts& p) noexcept {
    double most = -1e9;
    for (int i = 0; i < 64; ++i) {
        const double freq = 40.0 * std::pow(10000.0 / 40.0, i / 63.0);
        most = std::max(most, 20.0 * std::log10(std::abs(toneStackAnalog(p, 5.0, 5.0, 5.0, freq))));
    }
    return -most;
}

// --- One-pole responses -------------------------------------------------------------------

// A one-pole low-pass's response (coefficient c, dsp::onePoleCutoff's as the device keeps it, a float:
// y = x + c (y[-1] - x)): (1 - c) / (1 - c z⁻¹), at z⁻¹ = `z` and at `freq`.
inline std::complex<double> onePoleLowpass(double c, std::complex<double> z) noexcept {
    return divide(1.0 - c, 1.0 - c * z);
}
inline std::complex<double> onePoleLowpass(double c, double freq, double rate) noexcept {
    return onePoleLowpass(c, unitDelay(freq, rate));
}
inline double onePoleLowpassGain(double c, double freq, double rate) noexcept {
    return std::abs(onePoleLowpass(c, freq, rate));
}
// The high-pass it leaves (x minus the low-pass).
inline double onePoleHighpassGain(double c, double freq, double rate) noexcept {
    return std::abs(1.0 - onePoleLowpass(c, freq, rate));
}
// A first-order high shelf made of one (x + (G - 1)(x - lowpass(x))): its response.
inline std::complex<double> onePoleShelf(double c, double gain, std::complex<double> z) noexcept {
    return 1.0 + (gain - 1.0) * (1.0 - onePoleLowpass(c, z));
}
inline std::complex<double> onePoleShelf(double c, double gain, double freq, double rate) noexcept {
    return onePoleShelf(c, gain, unitDelay(freq, rate));
}

// --- The curves the editor draws ----------------------------------------------------------

// The tone section as it plays, in dB at `freq`: the tone stack's digital
// response at the oversampled rate (kOversampling × sampleRate) plus the
// make-up, times the presence shelf's (its one-pole at that rate).
inline double toneResponseDb(const Voicing& v, double bass, double middle, double treble, double presence,
                             double sampleRate, double freq) noexcept {
    const double rate = kOversampling * sampleRate;
    const ToneCoefficients c = toneStack(v.tone, bass, middle, treble, rate);
    const double shelf = std::pow(10.0, presenceDb(v, presence) / 20.0);
    const std::complex<double> h =
        toneStackDigital(c, freq, rate) * onePoleShelf(dsp::onePoleCutoff(v.presenceHz, rate), shelf, freq, rate);
    return 20.0 * std::log10(std::max(std::abs(h), 1e-30)) + v.toneMakeupDb;
}

// What comes out for a 1 kHz tone with these settings: the output's highest and
// lowest values for each peak level going in, as the amp plays the tone once it
// has settled. Worked out by harmonic balance over one period of the tone
// (kPoints samples of it): each curve sample by sample, and each linear part
// between the curves harmonic by harmonic, by its own response at that
// harmonic, as the device runs it (the input high-pass and bright shelf at the
// base rate; each stage's Miller low-pass, the stages' anti-aliasing as the
// half-sample average it is for a smooth signal, (1 + z⁻¹) / 2, the coupling
// high-passes, the tone section with its make-up, the power tubes' input
// low-pass and the transformer at the oversampled rate, with every harmonic
// above its half dropped; the down-sampler keeping what lies under half the
// base rate; the DC blocker and the trim). So each stage's asymmetry makes the
// DC the next coupling removes, and each harmonic is filtered where it goes, as
// in the device: played at the defaults, a tone's peaks land within 0.1 dB of
// these. The supply's sag is the slow envelope it is: the power drive `sagDb`
// lower. The preamp's part (up to the power stage's input) doesn't depend on
// the sag, so an editor can keep a curve's worth of it while the sag moves.
// Allocates nothing (the period lives on the stack): the device works its
// model morph's levels out with it in real time.
class Transfer {
public:
    static constexpr double kFrequency = 1000.0;
    static constexpr int kPoints = 256;  // samples of a period at most: the harmonics up to the 128th
    using Wave = std::array<double, kPoints>;
    struct Peaks {
        double high = 0.0, low = 0.0;
    };

    // `points` (a power of two up to kPoints) samples a period: fewer cost less (the device's morph
    // takes 64 for its levels), the curves drawn take kPoints.
    Transfer(const Voicing& v, double gain, double bass, double middle, double treble, double presence, double volume,
             double sampleRate, int points = kPoints) noexcept
        : v_(v), points_(std::clamp(points, 8, kPoints)) {
        const double rate = kOversampling * sampleRate, f0 = kFrequency;
        const double g = gainDb(v, gain);
        const ToneCoefficients tone = toneStack(v.tone, bass, middle, treble, rate);
        const double makeup = std::pow(10.0, v.toneMakeupDb / 20.0);
        const double shelf = std::pow(10.0, presenceDb(v, presence) / 20.0);
        double linear = 1.0;  // the small-signal gain through the stages' curves
        for (int k = 0; k < 3; ++k) {
            const Stage& s = v.stages[k];
            const double knob = k == 0 ? v.gainSplit * g : (k == 2 ? (1.0 - v.gainSplit) * g : 0.0);
            gIn_[k] = std::pow(10.0, (s.levelDb + knob - s.headDb) / 20.0);
            gOut_[k] = std::pow(10.0, s.headDb / 20.0) / shapeSlope(s.bias);
            offset_[k] = restValue(s.bias);
            linear *= std::pow(10.0, (s.levelDb + knob) / 20.0);
        }
        drive_ = std::pow(10.0, volumeDb(v, volume) / 20.0);
        powerOffset_ = restValue(v.powerBias);
        powerOut_ = 1.0 / shapeSlope(v.powerBias);
        // The linear parts' responses at the oversampled rate (oversampled: dropped above its half), each
        // one-pole's coefficient worked out once and each harmonic's z⁻¹ once for them all.
        double highpassC[3], millerC[3];  // each stage's coupling high-pass and Miller low-pass
        for (int k = 0; k < 3; ++k) {
            highpassC[k] = dsp::onePoleCutoff(v.stages[k].highpassHz, rate);
            millerC[k] = dsp::onePoleCutoff(v.stages[k].lowpassHz, rate);
        }
        const double presenceC = dsp::onePoleCutoff(v.presenceHz, rate);
        const double gridC = dsp::onePoleCutoff(kGridHz, rate);
        const double transformerC = dsp::onePoleCutoff(v.transformerHz, rate);
        const auto adaa = [](std::complex<double> z) { return (1.0 + z) / 2.0; };
        // Into V1 there is the tone alone: its gain is all that matters.
        const std::complex<double> z0 = unitDelay(f0, rate);
        const double bright = std::pow(10.0, v.brightDb / 20.0);
        const std::complex<double> input =
            (1.0 - onePoleLowpass(dsp::onePoleCutoff(v.inputHighpassHz, sampleRate), f0, sampleRate)) *
            onePoleShelf(dsp::onePoleCutoff(v.brightHz, sampleRate), bright, f0, sampleRate) *
            onePoleLowpass(millerC[0], z0);
        input_ = std::abs(input * adaa(z0));
        const double r = dsp::onePoleCutoff(kDcBlockerHz, sampleRate);  // dsp::DcBlocker: y = x - x[-1] + r y[-1]
        const double trim = std::pow(10.0, v.trimDb / 20.0);
        for (int k = 0; k <= points_ / 2; ++k) {
            const double f = k * f0;
            if (f >= 0.5 * rate) {
                link_[0][k] = link_[1][k] = link_[2][k] = out_[k] = 0.0;
                continue;
            }
            const std::complex<double> z = unitDelay(f, rate);
            const auto highpass = [&](int s) { return 1.0 - onePoleLowpass(highpassC[s], z); };
            const auto into = [&](int s) { return onePoleLowpass(millerC[s], z) * adaa(z); };
            link_[0][k] = highpass(0) * into(1);
            link_[1][k] = highpass(1) * toneStackDigital(tone, z) * makeup * into(2);
            link_[2][k] = highpass(2) * onePoleShelf(presenceC, shelf, z) * onePoleLowpass(gridC, z) * adaa(z);
            const std::complex<double> base = unitDelay(f, sampleRate);
            out_[k] = f < 0.5 * sampleRate
                          ? onePoleLowpass(transformerC, z) * divide(1.0 - base, 1.0 - r * base) * trim
                          : 0.0;
        }
        gain_ = input_ * linear * std::abs(link_[0][1] * link_[1][1] * link_[2][1] * out_[1]);
        // The filters alone (the anti-aliasing's half samples are part of the device's latency).
        const std::complex<double> filters =
            input * link_[0][1] * link_[1][1] * link_[2][1] * out_[1] / (adaa(z0) * adaa(z0) * adaa(z0));
        phaseDelay_ = -std::arg(filters) / (2.0 * kPi * f0 / sampleRate);
        // The FFT's twiddles (and the input tone's samples): e^(-2 pi i j / points), j up to half.
        for (int j = 0; j < points_ / 2; ++j) twiddle_[j] = unitDelay(j, points_);
    }

    // The power stage's input for a tone of peak `amplitude` (1.0: 0 dBFS): one period.
    Wave preamp(double amplitude) const noexcept {
        Wave w{};
        const double q = amplitude * input_ * gIn_[0];
        const int half = points_ / 2;  // sin(2 pi n / points), from the twiddles: -Im, and odd past the half
        for (int n = 0; n < half; ++n) w[n] = stage(0, q * -twiddle_[n].imag());
        for (int n = half; n < points_; ++n) w[n] = stage(0, q * twiddle_[n - half].imag());
        for (int k = 0; k < 3; ++k) {
            filter(w, link_[k]);
            if (k < 2)
                for (int n = 0; n < points_; ++n) w[n] = stage(k + 1, w[n] * gIn_[k + 1]);
        }
        return w;
    }
    // What comes out for that input with the power drive `sagDb` lower: one period.
    Wave power(const Wave& in, double sagDb) const noexcept {
        const double drive = drive_ * std::pow(10.0, -sagDb / 20.0);
        Wave w{};
        for (int n = 0; n < points_; ++n) w[n] = powerOut_ * (shape(in[n] * drive + v_.powerBias) - powerOffset_);
        filter(w, out_);
        return w;
    }
    // A period's highest and lowest values, and its RMS level.
    Peaks peaksOf(const Wave& w) const noexcept {
        Peaks p{w[0], w[0]};
        for (int n = 0; n < points_; ++n) {
            p.high = std::max(p.high, w[n]);
            p.low = std::min(p.low, w[n]);
        }
        return p;
    }
    double rmsOf(const Wave& w) const noexcept {
        double sum = 0.0;
        for (int n = 0; n < points_; ++n) sum += w[n] * w[n];
        return std::sqrt(sum / points_);
    }
    // A tone of peak `amplitude`: the output's peaks, and its RMS level.
    Peaks peaks(double amplitude, double sagDb) const noexcept { return peaksOf(power(preamp(amplitude), sagDb)); }
    double rms(double amplitude, double sagDb) const noexcept { return rmsOf(power(preamp(amplitude), sagDb)); }
    // The curve as drawn: at x ≥ 0 the high peak for a tone of peak x, below 0 the low one for -x.
    double operator()(double x, double sagDb) const noexcept {
        const Peaks p = peaks(std::abs(x), sagDb);
        return x >= 0.0 ? p.high : p.low;
    }
    // For small signals, what comes out over what goes in: the curve's slope through 0.
    double smallSignalGain(double sagDb) const noexcept { return gain_ * drive_ * std::pow(10.0, -sagDb / 20.0); }
    // How late its filters put a small tone out besides the device's latency (base-rate
    // samples, its phase over its frequency): the one-pole low-passes a fraction of a sample
    // each, the high-passes a little early.
    double phaseDelay() const noexcept { return phaseDelay_; }

private:
    using Response = std::array<std::complex<double>, kPoints / 2 + 1>;  // harmonics 0 (DC) .. kPoints / 2

    double stage(int k, double q) const noexcept { return gOut_[k] * (shape(q + v_.stages[k].bias) - offset_[k]); }

    // A period through a linear part: each harmonic times the part's response at it.
    void filter(Wave& w, const Response& h) const noexcept {
        const int n = points_;
        Wave re, im;  // (their first points_ only)
        std::copy_n(w.begin(), n, re.begin());
        std::fill_n(im.begin(), n, 0.0);
        fft(re, im, false);
        for (int k = 0; k <= n / 2; ++k) {
            const std::complex<double> x = std::complex<double>(re[k], im[k]) * h[k];
            re[k] = x.real();
            im[k] = x.imag();
            if (k > 0 && k < n - k) {  // the negative frequencies, mirrored
                re[n - k] = x.real();
                im[n - k] = -x.imag();
            }
        }
        fft(re, im, true);
        std::copy_n(re.begin(), n, w.begin());
    }

    // In place over points_, radix 2; `inverse` divides by points_. The twiddles are
    // the Transfer's own (nothing static, so nothing to set up on first use).
    void fft(Wave& re, Wave& im, bool inverse) const noexcept {
        const int n = points_;
        for (int i = 1, j = 0; i < n; ++i) {
            int bit = n >> 1;
            for (; j & bit; bit >>= 1) j ^= bit;
            j ^= bit;
            if (i < j) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        const double sign = inverse ? -1.0 : 1.0;  // the inverse's twiddles are the conjugates
        for (int length = 2; length <= n; length <<= 1) {
            const int stride = n / length;
            for (int i = 0; i < n; i += length) {
                for (int j = 0; j < length / 2; ++j) {
                    const double wr = twiddle_[j * stride].real(), wi = sign * twiddle_[j * stride].imag();
                    const int a = i + j, b = a + length / 2;
                    const double vr = re[b] * wr - im[b] * wi, vi = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - vr;
                    im[b] = im[a] - vi;
                    re[a] += vr;
                    im[a] += vi;
                }
            }
        }
        if (inverse)
            for (int i = 0; i < n; ++i) re[i] /= n;
    }

    Voicing v_;
    int points_ = kPoints;
    double input_ = 1.0, gIn_[3] = {}, gOut_[3] = {}, offset_[3] = {};
    double drive_ = 1.0, powerOffset_ = 0.0, powerOut_ = 1.0, gain_ = 1.0, phaseDelay_ = 0.0;
    Response link_[3] = {};  // V1 to V2, V2 to V3 (the tone section), V3 to the power stage
    Response out_ = {};      // the power stage to the output
    std::array<std::complex<double>, kPoints / 2> twiddle_ = {};
};

// One point of the curve (Transfer works the parts out once for a curve's worth).
inline double transfer(const Voicing& v, double gain, double bass, double middle, double treble, double presence,
                       double volume, double sagDb, double sampleRate, double x) noexcept {
    return Transfer(v, gain, bass, middle, treble, presence, volume, sampleRate)(x, sagDb);
}

}  // namespace sub::amp
