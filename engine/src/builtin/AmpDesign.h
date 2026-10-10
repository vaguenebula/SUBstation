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
// also keeps a model morph level-matched with Transfer's levels, from tables it
// works out beforehand (BasicTransfer's Rate and Voice, the logarithms blend()
// takes).

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

// The fields blend() moves geometrically (frequencies, the sag's times, the tone
// stack's parts), each handed to `f` in one order.
inline constexpr int kGeometricFields = 19;
template <class V, class F>
void forGeometricFields(V& v, F&& f) {
    f(v.inputHighpassHz);
    f(v.brightHz);
    for (auto& s : v.stages) {
        f(s.lowpassHz);
        f(s.highpassHz);
    }
    for (auto* part : {&v.tone.r1, &v.tone.r2, &v.tone.r3, &v.tone.r4, &v.tone.c1, &v.tone.c2, &v.tone.c3}) f(*part);
    f(v.presenceHz);
    f(v.sagAttackMs);
    f(v.sagReleaseMs);
    f(v.transformerHz);
}

// Their logarithms, what a blend interpolates: worked out once for a morph's ends (the
// device keeps each model's), a blend on the way costs an exp a field and no log.
using VoicingLogs = std::array<double, kGeometricFields>;
inline VoicingLogs logsOf(const Voicing& v) noexcept {
    VoicingLogs logs{};
    size_t i = 0;
    forGeometricFields(v, [&](double x) { logs[i++] = std::log(x); });
    return logs;
}

// Between two voicings, `t` of the way (0: a, 1: b, exactly): dB values,
// biases, Gain's split and the sag's amount linearly; frequencies, the sag's
// times and the tone stack's parts geometrically (`la` and `lb`: logsOf(a) and
// logsOf(b)). Every field moves continuously, so a morph moves every
// coefficient continuously.
inline Voicing blend(const Voicing& a, const VoicingLogs& la, const Voicing& b, const VoicingLogs& lb,
                     double t) noexcept {
    if (t <= 0.0) return a;
    if (t >= 1.0) return b;
    const auto lin = [t](double x, double y) { return (1.0 - t) * x + t * y; };
    Voicing v = a;
    v.brightDb = lin(a.brightDb, b.brightDb);
    v.gainMinDb = lin(a.gainMinDb, b.gainMinDb);
    v.gainMaxDb = lin(a.gainMaxDb, b.gainMaxDb);
    v.gainSplit = lin(a.gainSplit, b.gainSplit);
    for (int k = 0; k < 3; ++k) {
        const Stage& x = a.stages[k];
        const Stage& y = b.stages[k];
        v.stages[k].levelDb = lin(x.levelDb, y.levelDb);
        v.stages[k].headDb = lin(x.headDb, y.headDb);
        v.stages[k].bias = lin(x.bias, y.bias);
    }
    v.toneMakeupDb = lin(a.toneMakeupDb, b.toneMakeupDb);
    v.presenceRangeDb = lin(a.presenceRangeDb, b.presenceRangeDb);
    v.volumeDb = lin(a.volumeDb, b.volumeDb);
    v.powerBias = lin(a.powerBias, b.powerBias);
    v.sag = lin(a.sag, b.sag);
    v.trimDb = lin(a.trimDb, b.trimDb);
    size_t i = 0;
    forGeometricFields(v, [&](double& x) {
        x = std::exp((1.0 - t) * la[i] + t * lb[i]);
        ++i;
    });
    return v;
}
inline Voicing blend(const Voicing& a, const Voicing& b, double t) noexcept {
    if (t <= 0.0) return a;
    if (t >= 1.0) return b;
    return blend(a, logsOf(a), b, logsOf(b), t);
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
// (`points` samples of it, a power of two up to MaxPoints): each curve sample by
// sample, and each linear part between the curves harmonic by harmonic, by its
// own response at that harmonic, as the device runs it (the input high-pass and
// bright shelf at the base rate; each stage's Miller low-pass, the stages'
// anti-aliasing as the half-sample average it is for a smooth signal,
// (1 + z⁻¹) / 2, the coupling high-passes, the tone section with its make-up,
// the power tubes' input low-pass and the transformer at the oversampled rate,
// with every harmonic above its half dropped; the down-sampler keeping what lies
// under half the base rate; the DC blocker and the trim). So each stage's
// asymmetry makes the DC the next coupling removes, and each harmonic is
// filtered where it goes, as in the device: played at the defaults, a tone's
// peaks land within 0.1 dB of these. The supply's sag is the slow envelope it
// is: the power drive `sagDb` lower. The preamp's part (up to the power stage's
// input) doesn't depend on the sag, so an editor can keep a curve's worth of it
// while the sag moves.
//
// What the rate alone sets (a Rate: every sine and cosine) and what the voicing
// alone sets (a Voice: every filter's coefficient and most of their responses)
// can be worked out beforehand and handed in. The device works its model morph's
// levels out in real time from a Rate and each model's Voice, made in prepare(),
// so a level costs what the dials add and the period's FFTs (a blend's, in a
// morph, its Voice too). Allocates nothing (the period lives on the stack).
// Transfer takes up to 256 samples a period (the curves drawn); the device's
// levels take 64, in a BasicTransfer<64> (its tables a quarter of the size).
template <int MaxPoints>
class BasicTransfer {
public:
    static constexpr double kFrequency = 1000.0;
    static constexpr int kPoints = MaxPoints;  // samples of a period at most: the harmonics up to kPoints / 2
    static_assert(kPoints >= 8 && (kPoints & (kPoints - 1)) == 0);
    using Wave = std::array<double, kPoints>;
    using Response = std::array<std::complex<double>, kPoints / 2 + 1>;  // harmonics 0 (DC) .. kPoints / 2
    struct Peaks {
        double high = 0.0, low = 0.0;
    };
    using Half = std::array<double, kPoints / 2>;
    // The FFT's tables for a period of `points` samples. A period is real: it goes through a
    // complex FFT of half its length (its even samples the real part, its odd ones the
    // imaginary), its harmonics are split out of that and joined back into it.
    struct Fft {
        // The half-length FFT's twiddles, e^(-2 pi i j / length), each pass's in a row (length 8,
        // 16, .. points / 2, j under half of it: the first two passes need none), and its order:
        // each index's bits reversed.
        Half twiddleRe{}, twiddleIm{};
        std::array<int, kPoints / 2> reversed{};
        // e^(-2 pi i k / points), k under points / 2: what splits harmonic k out (and, its
        // imaginary parts, the tone's samples).
        std::array<std::complex<double>, kPoints / 2> split{};
    };

    // What a Transfer works out from the rate alone for `points` samples a period:
    // each harmonic's z⁻¹ at the oversampled rate, the parts that depend on nothing
    // else (the stages' anti-aliasing, the power tubes' input low-pass and, at the
    // base rate, the DC blocker), and the FFT's tables.
    struct Rate {
        explicit Rate(double baseRate, int periodPoints = kPoints) noexcept
            : sampleRate(baseRate), points(std::clamp(periodPoints, 8, kPoints)) {
            const double rate = kOversampling * sampleRate, f0 = kFrequency;
            base = unitDelay(f0, sampleRate);
            const double r = dsp::onePoleCutoff(kDcBlockerHz, sampleRate);  // dsp::DcBlocker: y = x - x[-1] + r y[-1]
            const double gridC = dsp::onePoleCutoff(kGridHz, rate);
            for (int k = 0; k <= points / 2; ++k) {
                const double f = k * f0;
                if (f >= 0.5 * rate) break;
                harmonics = k + 1;
                z[k] = unitDelay(f, rate);
                adaa[k] = (1.0 + z[k]) / 2.0;
                grid[k] = onePoleLowpass(gridC, z[k]);
                if (f < 0.5 * sampleRate) {
                    const std::complex<double> b = unitDelay(f, sampleRate);
                    dc[k] = divide(1.0 - b, 1.0 - r * b);
                    baseHarmonics = k + 1;
                }
            }
            const int m = points / 2;
            for (int k = 0; k < m; ++k) fft.split[k] = unitDelay(k, points);
            for (int i = 1, j = 0; i < m; ++i) {
                int bit = m >> 1;
                for (; j & bit; bit >>= 1) j ^= bit;
                j ^= bit;
                fft.reversed[i] = j;
            }
            for (int half = 4, at = 0; half < m; at += half, half <<= 1) {
                for (int j = 0; j < half; ++j) {  // e^(-2 pi i j / (2 half))
                    fft.twiddleRe[at + j] = fft.split[j * (points / (2 * half))].real();
                    fft.twiddleIm[at + j] = fft.split[j * (points / (2 * half))].imag();
                }
            }
        }

        double sampleRate;
        int points;
        int harmonics = 0;            // those under half the oversampled rate (the rest are dropped)
        int baseHarmonics = 0;        // and under half the base rate (all the output keeps)
        std::complex<double> base;    // the tone's z⁻¹ at the base rate
        Response z{};                 // each harmonic's z⁻¹ at the oversampled rate
        Response adaa{};              // the stages' anti-aliasing: (1 + z⁻¹) / 2
        Response grid{};              // the power tubes' input low-pass
        Response dc{};                // the DC blocker
        Fft fft;
    };

    // What a Transfer works out from a voicing at a Rate before the dials: the
    // stages' output gains and rest values, the input's gain at the tone, and each
    // filter's response at each harmonic (bar the two the dials move: the tone stack's
    // and Presence's shelf's gain).
    struct Voice {
        Voice() = default;
        Voice(const Voicing& v, const Rate& r) noexcept { set(v, r); }

        // Made again in place (allocating nothing): the device's blends in a morph. It keeps
        // a pointer to `r`.
        void set(const Voicing& v, const Rate& r) noexcept {
            rate = &r;
            voicing = v;
            const double sampleRate = r.sampleRate, overRate = kOversampling * sampleRate;
            makeup = std::pow(10.0, v.toneMakeupDb / 20.0);
            for (int k = 0; k < 3; ++k) {
                const Stage& s = v.stages[k];
                gOut[k] = std::pow(10.0, s.headDb / 20.0) / shapeSlope(s.bias);
                offset[k] = restValue(s.bias);
            }
            powerOffset = restValue(v.powerBias);
            powerOut = 1.0 / shapeSlope(v.powerBias);
            double highpassC[3], millerC[3];  // each stage's coupling high-pass and Miller low-pass
            for (int k = 0; k < 3; ++k) {
                highpassC[k] = dsp::onePoleCutoff(v.stages[k].highpassHz, overRate);
                millerC[k] = dsp::onePoleCutoff(v.stages[k].lowpassHz, overRate);
            }
            const double presenceC = dsp::onePoleCutoff(v.presenceHz, overRate);
            const double transformerC = dsp::onePoleCutoff(v.transformerHz, overRate);
            // Into V1 there is the tone alone: its gain is all that matters.
            const double bright = std::pow(10.0, v.brightDb / 20.0);
            inputResponse = (1.0 - onePoleLowpass(dsp::onePoleCutoff(v.inputHighpassHz, sampleRate), r.base)) *
                            onePoleShelf(dsp::onePoleCutoff(v.brightHz, sampleRate), bright, r.base) *
                            onePoleLowpass(millerC[0], r.z[1]);
            input = std::abs(inputResponse * r.adaa[1]);
            const double trim = std::pow(10.0, v.trimDb / 20.0);
            for (int k = 0; k < r.harmonics; ++k) {
                const std::complex<double> z = r.z[k];
                const auto highpass = [&](int s) { return 1.0 - onePoleLowpass(highpassC[s], z); };
                const auto into = [&](int s) { return onePoleLowpass(millerC[s], z) * r.adaa[k]; };
                link0[k] = highpass(0) * into(1);
                highpass1[k] = highpass(1);
                into2[k] = into(2);
                highpass2[k] = highpass(2);
                presence[k] = 1.0 - onePoleLowpass(presenceC, z);  // (the shelf's boosted part)
                out[k] = k < r.baseHarmonics ? onePoleLowpass(transformerC, z) * r.dc[k] * trim : 0.0;
            }
        }

        const Rate* rate = nullptr;
        Voicing voicing{};
        double makeup = 1.0;  // the tone stack's
        double input = 1.0;   // the input stage's gain at the tone, into V1's curve
        std::complex<double> inputResponse{};  // (without V1's anti-aliasing: its half sample is latency)
        double gOut[3] = {}, offset[3] = {}, powerOffset = 0.0, powerOut = 1.0;
        // V1 to V2; V2's coupling and V3's input around the tone stack; V3's coupling and
        // Presence's shelf's boosted part; the power stage to the output.
        Response link0{}, highpass1{}, into2{}, highpass2{}, presence{}, out{};
    };

    // From a Voice (at its Rate): the device's way.
    BasicTransfer(const Voice& voice, double gain, double bass, double middle, double treble, double presence,
                  double volume) noexcept
        : v_(voice.voicing), points_(voice.rate->points), sampleRate_(voice.rate->sampleRate) {
        const Rate& rate = *voice.rate;
        gainDb_ = gainDb(v_, gain);
        const ToneCoefficients tone = toneStack(v_.tone, bass, middle, treble, kOversampling * sampleRate_);
        const double shelf = std::pow(10.0, presenceDb(v_, presence) / 20.0);
        for (int k = 0; k < 3; ++k) {
            const Stage& s = v_.stages[k];
            gIn_[k] = std::pow(10.0, (s.levelDb + knob(k) - s.headDb) / 20.0);
            gOut_[k] = voice.gOut[k];
            offset_[k] = voice.offset[k];
        }
        drive_ = std::pow(10.0, volumeDb(v_, volume) / 20.0);
        powerOffset_ = voice.powerOffset;
        powerOut_ = voice.powerOut;
        input_ = voice.input;
        inputResponse_ = voice.inputResponse;
        adaa1_ = rate.adaa[1];
        for (int k = 0; k <= points_ / 2; ++k) {
            if (k >= rate.harmonics) {  // at or above half the oversampled rate: dropped
                link_[0][k] = link_[1][k] = link_[2][k] = out_[k] = 0.0;
                continue;
            }
            link_[0][k] = voice.link0[k];
            link_[1][k] = voice.highpass1[k] * toneStackDigital(tone, rate.z[k]) * voice.makeup * voice.into2[k];
            link_[2][k] = voice.highpass2[k] * (1.0 + (shelf - 1.0) * voice.presence[k]) * rate.grid[k] * rate.adaa[k];
            out_[k] = voice.out[k];
        }
        fft_ = rate.fft;
    }

    // From the voicing at a rate: `points` samples a period (fewer cost less; the curves
    // drawn take kPoints).
    BasicTransfer(const Voicing& v, double gain, double bass, double middle, double treble, double presence,
                  double volume, double sampleRate, int points = kPoints) noexcept
        : BasicTransfer(Voice(v, Rate(sampleRate, points)), gain, bass, middle, treble, presence, volume) {}

    // The power stage's input for a tone of peak `amplitude` (1.0: 0 dBFS): one period.
    Wave preamp(double amplitude) const noexcept {
        Wave w{};
        const double q = amplitude * input_ * gIn_[0];
        const int half = points_ / 2;  // sin(2 pi n / points): -Im of e^(-2 pi i n / points), odd past the half
        for (int n = 0; n < half; ++n) w[n] = stage(0, q * -fft_.split[n].imag());
        for (int n = half; n < points_; ++n) w[n] = stage(0, q * fft_.split[n - half].imag());
        for (int k = 0; k < 3; ++k) {
            filter(w, link_[k]);
            if (k < 2)
                for (int n = 0; n < points_; ++n) w[n] = stage(k + 1, w[n] * gIn_[k + 1]);
        }
        return w;
    }
    // What comes out for that input with the power drive `sagDb` lower: one period.
    Wave power(const Wave& in, double sagDb) const noexcept {
        const double drive = sagDb == 0.0 ? drive_ : drive_ * std::pow(10.0, -sagDb / 20.0);  // (pow(10, 0) is 1)
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
    double smallSignalGain(double sagDb) const noexcept {
        double linear = 1.0;  // the small-signal gain through the stages' curves
        for (int k = 0; k < 3; ++k) linear *= std::pow(10.0, (v_.stages[k].levelDb + knob(k)) / 20.0);
        const double gain = input_ * linear * std::abs(link_[0][1] * link_[1][1] * link_[2][1] * out_[1]);
        return gain * drive_ * std::pow(10.0, -sagDb / 20.0);
    }
    // How late its filters put a small tone out besides the device's latency (base-rate
    // samples, its phase over its frequency): the one-pole low-passes a fraction of a sample
    // each, the high-passes a little early. (The anti-aliasing's half samples are part of
    // the device's latency: the filters alone.)
    double phaseDelay() const noexcept {
        const std::complex<double> filters =
            inputResponse_ * link_[0][1] * link_[1][1] * link_[2][1] * out_[1] / (adaa1_ * adaa1_ * adaa1_);
        return -std::arg(filters) / (2.0 * kPi * kFrequency / sampleRate_);
    }

private:
    // Gain's share at stage k, in dB.
    double knob(int k) const noexcept {
        return k == 0 ? v_.gainSplit * gainDb_ : (k == 2 ? (1.0 - v_.gainSplit) * gainDb_ : 0.0);
    }

    double stage(int k, double q) const noexcept { return gOut_[k] * (shape(q + v_.stages[k].bias) - offset_[k]); }

    // A period through a linear part: each harmonic times the part's response at it (of
    // harmonics 0 and points / 2, the real part: all a real period keeps of them). Harmonics
    // k and m - k (m = points / 2) come out of the half-length transform's bins k and m - k,
    // a and b, together, with W = e^(-2 pi i k / points) (W at m - k is -conj W):
    //   X(k) = (s - d) / 2, X(m - k) = conj(s + d) / 2, s = a + conj b, d = i W (a - conj b),
    // and go back into them the same way the other way round (conj W, the inverse's).
    void filter(Wave& w, const Response& h) const noexcept {
        const int m = points_ / 2;
        Half re, im;  // (their first m only)
        for (int j = 0; j < m; ++j) {
            re[j] = w[2 * j];
            im[j] = w[2 * j + 1];
        }
        fft(re, im, false);
        const auto i = [](std::complex<double> x) { return std::complex<double>(-x.imag(), x.real()); };  // i x
        const double dc = ((re[0] + im[0]) * h[0]).real(), top = ((re[0] - im[0]) * h[m]).real();
        re[0] = 0.5 * (dc + top);
        im[0] = 0.5 * (dc - top);
        for (int k = 1; k <= m / 2; ++k) {
            const int l = m - k;
            const std::complex<double> a(re[k], im[k]), b(re[l], im[l]), wk = fft_.split[k];
            const std::complex<double> s = a + std::conj(b), d = i(wk * (a - std::conj(b)));
            const std::complex<double> xk = (s - d) * h[k], xl = std::conj(s + d) * h[l];  // (twice X times h)
            const std::complex<double> e = xk + std::conj(xl), g = i(std::conj(wk) * (xk - std::conj(xl)));
            re[k] = 0.25 * (e.real() + g.real());
            im[k] = 0.25 * (e.imag() + g.imag());
            re[l] = 0.25 * (e.real() - g.real());
            im[l] = 0.25 * (g.imag() - e.imag());
        }
        fft(re, im, true);
        for (int j = 0; j < m; ++j) {
            w[2 * j] = re[j];
            w[2 * j + 1] = im[j];
        }
    }

    // In place over points_ / 2: its first two passes at once (their twiddles are 1 and -i),
    // then radix 2; `inverse` divides by points_ / 2. The tables are the Transfer's own
    // (nothing static, so nothing to set up on first use).
    void fft(Half& re, Half& im, bool inverse) const noexcept {
        const int n = points_ / 2;
        for (int i = 1; i < n; ++i) {
            const int j = fft_.reversed[i];
            if (i < j) {
                std::swap(re[i], re[j]);
                std::swap(im[i], im[j]);
            }
        }
        const double sign = inverse ? -1.0 : 1.0;  // the inverse's twiddles are the conjugates
        for (int i = 0; i < n; i += 4) {
            const double r0 = re[i] + re[i + 1], i0 = im[i] + im[i + 1], r1 = re[i] - re[i + 1], i1 = im[i] - im[i + 1];
            const double r2 = re[i + 2] + re[i + 3], i2 = im[i + 2] + im[i + 3];
            const double r3 = sign * (im[i + 2] - im[i + 3]), i3 = sign * (re[i + 3] - re[i + 2]);  // times -i (or i)
            re[i] = r0 + r2;
            im[i] = i0 + i2;
            re[i + 2] = r0 - r2;
            im[i + 2] = i0 - i2;
            re[i + 1] = r1 + r3;
            im[i + 1] = i1 + i3;
            re[i + 3] = r1 - r3;
            im[i + 3] = i1 - i3;
        }
        for (int half = 4, at = 0; half < n; at += half, half <<= 1) {
            for (int i = 0; i < n; i += 2 * half) {
                for (int j = 0; j < half; ++j) {
                    const double wr = fft_.twiddleRe[at + j], wi = sign * fft_.twiddleIm[at + j];
                    const int a = i + j, b = a + half;
                    const double vr = re[b] * wr - im[b] * wi, vi = re[b] * wi + im[b] * wr;
                    re[b] = re[a] - vr;
                    im[b] = im[a] - vi;
                    re[a] += vr;
                    im[a] += vi;
                }
            }
        }
        if (inverse) {
            for (int i = 0; i < n; ++i) {
                re[i] /= n;
                im[i] /= n;
            }
        }
    }

    Voicing v_;
    int points_ = kPoints;
    double sampleRate_ = 48000.0, gainDb_ = 0.0;
    double input_ = 1.0, gIn_[3] = {}, gOut_[3] = {}, offset_[3] = {};
    double drive_ = 1.0, powerOffset_ = 0.0, powerOut_ = 1.0;
    std::complex<double> inputResponse_{}, adaa1_{};  // (phaseDelay())
    Response link_[3] = {};  // V1 to V2, V2 to V3 (the tone section), V3 to the power stage
    Response out_ = {};      // the power stage to the output
    Fft fft_;
};

// Up to 256 samples a period: the editor's curves.
using Transfer = BasicTransfer<256>;

// One point of the curve (Transfer works the parts out once for a curve's worth).
inline double transfer(const Voicing& v, double gain, double bass, double middle, double treble, double presence,
                       double volume, double sagDb, double sampleRate, double x) noexcept {
    return Transfer(v, gain, bass, middle, treble, presence, volume, sampleRate)(x, sagDb);
}

}  // namespace sub::amp
