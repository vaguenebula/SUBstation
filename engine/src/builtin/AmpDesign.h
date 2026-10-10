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
//   or log. With 4x oversampling it keeps what folds back 60-80 dB down even
//   at the highest gains.
// - The tone stack is the passive Fender/Marshall/Vox stack as Yeh and Smith
//   model the '59 Bassman's (DAFx-06): a third-order filter whose coefficients
//   are polynomials in the three pots' positions, so the controls interact as
//   a real amp's do. Each model has its own component values, and a make-up
//   gain that puts its response at noon at 0 dB at its peak.
// - Gain sets the preamp's input level (split between V1 and V3 on the
//   high-gain models), Volume the power stage's drive (±12 dB around noon).
//
// Softube's models are unpublished: this is a behavioural design, built to do
// what Live's manual says each control does and to sound like the amps the
// models are known to be.
//
// Shared by the device and, through the application layer's AmpResponse.h, its
// editor: the tone curve (toneResponseDb) and the transfer curve (Transfer) are
// worked out from the very numbers and functions the engine plays.

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>
#include <string>
#include <vector>

namespace sub::amp {

inline constexpr int kModels = 7;
inline constexpr int kOversamplingLog2 = 2;  // 4x
inline constexpr int kOversampling = 1 << kOversamplingLog2;
inline constexpr double kPi = std::numbers::pi;
inline constexpr double kDcBlockerHz = 10.0;  // after the oversampled section

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
// - Clean: V1 11 dB under its clipping point (5 % THD, second harmonic well
//   over the third), V2 a clean cathode follower, a strong V3 into a quiet
//   power stage that only breaks up with hot signals.
// - Boost: V1 at the knee, V2 pushed past it: crunch with the Vox stack.
// - Blues: a clean preamp, the Fender stack, a power stage near its knee with
//   the deepest sag: Volume adds distortion.
// - Rock: V1 at the knee into a cascaded V2: the Plexi's crunch, a late stack.
// - Lead: Gain split between V1 and V3 around an early stack, V2 and V3 far
//   over; an 80 Hz input and 120 Hz coupling keep it tight; little sag.
// - Heavy: the same structure looser and darker, a power stage that sags and
//   distorts with Volume.
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
     {{0.0, 0.0, 0.35, 8000.0, 20.0}, {12.0, -3.0, 0.40, 6000.0, 60.0}, {6.0, -3.0, 0.30, 6000.0, 50.0}},
     kVintage, 4.221411743695151, 3500.0, 9.0, -1.0, 0.05, 0.50, 10.0, 200.0, 8500.0, -5.2},
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

// The digital stack's response at `freq` Hz.
inline std::complex<double> toneStackDigital(const ToneCoefficients& c, double freq, double rate) noexcept {
    const std::complex<double> z = std::polar(1.0, -2.0 * kPi * freq / rate);  // z⁻¹
    return (c.b0 + z * (c.b1 + z * (c.b2 + z * c.b3))) / (1.0 + z * (c.a1 + z * (c.a2 + z * c.a3)));
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

// The coefficient of a one-pole low-pass at `cutoffHz` (dsp::onePoleCutoff's, as a float as the device keeps it).
inline double onePoleCoeff(double cutoffHz, double rate) noexcept {
    return static_cast<float>(std::exp(-2.0 * kPi * std::max(0.0, cutoffHz) / rate));
}
// A one-pole low-pass's response (coefficient c: y = x + c (y[-1] - x)) at `freq`: (1 - c) / (1 - c z⁻¹).
inline std::complex<double> onePoleLowpass(double c, double freq, double rate) noexcept {
    const std::complex<double> z = std::polar(1.0, -2.0 * kPi * freq / rate);
    return (1.0 - c) / (1.0 - c * z);
}
inline double onePoleLowpassGain(double c, double freq, double rate) noexcept {
    return std::abs(onePoleLowpass(c, freq, rate));
}
// The high-pass it leaves (x minus the low-pass).
inline double onePoleHighpassGain(double c, double freq, double rate) noexcept {
    return std::abs(1.0 - onePoleLowpass(c, freq, rate));
}
// A first-order high shelf made of one (x + (G - 1)(x - lowpass(x))): its response.
inline std::complex<double> onePoleShelf(double c, double gain, double freq, double rate) noexcept {
    return 1.0 + (gain - 1.0) * (1.0 - onePoleLowpass(c, freq, rate));
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
        toneStackDigital(c, freq, rate) * onePoleShelf(onePoleCoeff(v.presenceHz, rate), shelf, freq, rate);
    return 20.0 * std::log10(std::max(std::abs(h), 1e-30)) + v.toneMakeupDb;
}

// One stage's static curve (no filters, no anti-aliasing):
// head (shape(u + bias) - shape(bias)) / shapeSlope(bias), u = x 10^(gainDb / 20) / head.
inline double staticStage(double x, double gainDb, double headDb, double bias) noexcept {
    const double head = std::pow(10.0, headDb / 20.0);
    const double u = x * std::pow(10.0, gainDb / 20.0) / head;
    return head * (shape(u + bias) - shape(bias)) / shapeSlope(bias);
}

// What comes out for an input sample x of a 1 kHz tone with these settings:
// every linear part as its gain at 1 kHz where it sits (the input high-pass and
// bright shelf at the base rate; each stage's Miller low-pass before its curve
// and coupling high-pass after it, the tone section by toneResponseDb, the
// transformer, all at the oversampled rate; the DC blocker), the stages' and the
// power stage's static curves, the power drive less `sagDb`, times the trim. In
// the linear region transfer(x) / x is the device's gain for a 1 kHz tone, to
// within the anti-aliasing's averaging and the oversampler's ripple (both under
// 0.01 dB). A Transfer works the linear gains out once (the editor evaluates a
// curve's worth of points at a time); transfer() is one point.
class Transfer {
public:
    static constexpr double kFrequency = 1000.0;

    Transfer(const Voicing& v, double gain, double bass, double middle, double treble, double presence, double volume,
             double sagDb, double sampleRate) noexcept
        : v_(v) {
        const double rate = kOversampling * sampleRate;
        const double f = kFrequency;
        const double g = gainDb(v, gain);
        pre_ = onePoleHighpassGain(onePoleCoeff(v.inputHighpassHz, sampleRate), f, sampleRate) *
               std::abs(onePoleShelf(onePoleCoeff(v.brightHz, sampleRate), std::pow(10.0, v.brightDb / 20.0), f,
                                     sampleRate));
        for (int k = 0; k < 3; ++k) {
            const Stage& s = v.stages[k];
            lowpass_[k] = onePoleLowpassGain(onePoleCoeff(s.lowpassHz, rate), f, rate);
            highpass_[k] = onePoleHighpassGain(onePoleCoeff(s.highpassHz, rate), f, rate);
            const double knob = k == 0 ? v.gainSplit * g : (k == 2 ? (1.0 - v.gainSplit) * g : 0.0);
            gain_[k] = std::pow(10.0, (s.levelDb + knob) / 20.0);
            head_[k] = std::pow(10.0, s.headDb / 20.0);
            outGain_[k] = head_[k] / shapeSlope(s.bias);
            offset_[k] = shape(s.bias);
        }
        tone_ = std::pow(10.0, toneResponseDb(v, bass, middle, treble, presence, sampleRate, f) / 20.0);
        drive_ = std::pow(10.0, (volumeDb(v, volume) - sagDb) / 20.0);
        powerOffset_ = shape(v.powerBias);
        powerOut_ = 1.0 / shapeSlope(v.powerBias);
        const double r = onePoleCoeff(kDcBlockerHz, sampleRate);  // dsp::DcBlocker: y = x - x[-1] + r y[-1]
        const std::complex<double> z = std::polar(1.0, -2.0 * kPi * f / sampleRate);
        const double dc = std::abs((1.0 - z) / (1.0 - r * z));
        post_ = onePoleLowpassGain(onePoleCoeff(v.transformerHz, rate), f, rate) * dc * std::pow(10.0, v.trimDb / 20.0);
    }

    double operator()(double x) const noexcept {
        double y = x * pre_;
        y = highpass_[0] * stage(0, lowpass_[0] * y);
        y = highpass_[1] * stage(1, lowpass_[1] * y);
        y *= tone_;
        y = highpass_[2] * stage(2, lowpass_[2] * y);
        const double u = y * drive_;
        y = (shape(u + v_.powerBias) - powerOffset_) * powerOut_;
        return y * post_;
    }

private:
    double stage(int k, double x) const noexcept {
        const double u = x * gain_[k] / head_[k];
        return outGain_[k] * (shape(u + v_.stages[k].bias) - offset_[k]);
    }

    Voicing v_;
    double pre_ = 1.0, tone_ = 1.0, drive_ = 1.0, post_ = 1.0, powerOffset_ = 0.0, powerOut_ = 1.0;
    double lowpass_[3] = {}, highpass_[3] = {}, gain_[3] = {}, head_[3] = {}, outGain_[3] = {}, offset_[3] = {};
};

inline double transfer(const Voicing& v, double gain, double bass, double middle, double treble, double presence,
                       double volume, double sagDb, double sampleRate, double x) noexcept {
    return Transfer(v, gain, bass, middle, treble, presence, volume, sagDb, sampleRate)(x);
}

}  // namespace sub::amp
