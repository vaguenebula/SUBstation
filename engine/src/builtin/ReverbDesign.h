#pragma once
// The Reverb's design: its constants, and the maths the device
// (builtin/devices/Reverb.cpp) and its editor share, so the curves the editor
// draws are worked out from the very numbers the engine plays.
//
// The signal: the input summed to mono goes through a band (a Butterworth
// high-pass and low-pass), then into a delay line. The early reflections are 12
// taps of that line from the predelay on (the first at the predelay itself),
// each with a gain (Shape's envelope), a sign and a pan; the diffuse tail is read
// from it later (Shape's onset), blurred by Schroeder all-passes and fed into a
// feedback delay network: 4, 8 or 16 lines (Density) mixed by a Walsh-Hadamard
// matrix, each line's loop with two one-pole shelves (or a low-pass) set so
// every band rings for its own time. Size scales every delay by sqrt(size / 100).
//
// Shared with the application layer's ReverbResponse (app/src/audio), which
// wraps decaySeconds(), inputFilterDb(), earlyTaps() and spinPan() for the editor.

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <limits>
#include <numbers>
#include <span>

namespace sub::reverb {

inline constexpr int kMaxLines = 16;
inline constexpr int kMaxTaps = 12;
inline constexpr int kMaxDiffusers = 4;
inline constexpr int kControl = 32;             // samples per control sub-chunk
inline constexpr int kMeterSamples = 256;       // audio per meter value
inline constexpr double kMinLineSamples = 4.0;  // a line is never shorter
inline constexpr double kMinSize = 0.22;        // the Size parameter's range
inline constexpr double kMaxSize = 500.0;

// The network's lines at Size 100, in ms: spread about geometrically (ratio
// ~1.084) and nudged off exact ratios, so their echoes never line up.
inline constexpr std::array<double, kMaxLines> kLineMs = {29.13, 31.71, 34.07, 37.29, 39.97, 43.69, 46.93, 51.37,
                                                          55.11, 60.29, 64.79, 70.87, 76.03, 83.27, 89.41, 97.61};
// The input's signs into the active lines (the k-th active line takes kInputSign[k]).
inline constexpr std::array<int, kMaxLines> kInputSign = {+1, -1, +1, -1, +1, +1, -1, -1,
                                                          +1, -1, -1, +1, -1, +1, +1, -1};

// The input diffusers (Dattorro's 142, 107, 379, 277 samples at 29761 Hz), in ms
// at Size 100 and Scale 50 %, and their gains at Diffusion 100 %.
inline constexpr std::array<double, kMaxDiffusers> kDiffuserMs = {4.771, 3.595, 12.735, 9.307};
inline constexpr std::array<double, kMaxDiffusers> kDiffuserGain = {0.75, 0.75, 0.625, 0.625};
inline constexpr double kLoopAllpassShare = 0.11;  // High: each loop's all-pass, a share of its line (times Scale's c)
inline constexpr double kLoopAllpassGain = 0.5;    // its gain at Diffusion 100 %

// The early reflections at Size 100: time after the predelay (ms; the first comes
// at the predelay itself, which is the time to the first reflection, as Live's
// is), pan (-1..1), sign.
inline constexpr std::array<double, kMaxTaps> kTapMs = {0.0,  2.8,  5.7,  8.7,  12.1, 16.0,
                                                        20.4, 25.5, 31.3, 37.9, 45.2, 53.6};
inline constexpr std::array<double, kMaxTaps> kTapPan = {-0.83, 0.71, -0.32, 0.94, -0.61, 0.17,
                                                         0.88,  -0.95, 0.42, -0.24, 0.66, -0.54};
inline constexpr std::array<int, kMaxTaps> kTapSign = {+1, -1, +1, +1, -1, +1, -1, +1, -1, -1, +1, -1};

inline constexpr double kEarlyGain = 0.7;    // the reflections' total gain (their energy, 0.49)
inline constexpr double kDiffuseGain = 1.3;  // the tail's trim: at the defaults it is about as loud as the reflections
// The tail's two outputs are sums of all the lines' reads (signs +-1) times
// this, whatever the Density: a network's energy is shared out over its lines,
// so the sum of their reads carries all of it however many there are, and the
// tail is as loud at every Density (1 / sqrt(N) would make Sparse 6 dB louder).
inline constexpr double kOutputScale = 0.25;
inline constexpr double kSpinDepthMs = 1.0;          // Spin Amount 100 %: taps drift 0..2 ms later (times min(1, s))
inline constexpr double kSpinPanSwing = 0.7;         // and +-0.7 of pan swing in the middle (see spinPan())
inline constexpr double kChorusDepthMs = 2.0;        // Chorus Amount 100 %: +-2 ms on each line (a quarter at most)
inline constexpr double kOnsetShare = 0.8;           // Shape 100 %: the diffuse onset 0.8 x the last tap's time later
inline constexpr double kFreezeSeconds = 1000.0;     // frozen with Cut: the tail's decay time (practically endless)
inline constexpr double kFreezeUncutSeconds = 60.0;  // frozen without Cut (the input keeps adding: kept bounded)
inline constexpr double kMaxStereo = 120.0;          // Stereo Image's top: the two sides independent

// --- Density: which lines, diffusers and taps play ------------------------------------------

enum class Density { Sparse = 0, Low, Mid, High };

// A Density's network: how many lines (the active ones are firstLine,
// firstLine + lineStride, ...), input diffusers (the first ones) and early taps,
// and whether each loop has an all-pass of its own.
struct Layout {
    int lines;
    int diffusers;
    int taps;
    bool loopAllpass;
    int firstLine;
    int lineStride;
};

constexpr Layout layout(Density d) noexcept {
    switch (d) {
    case Density::Sparse: return {4, 2, 6, false, 2, 4};
    case Density::Low: return {8, 3, 12, false, 1, 2};
    case Density::Mid: return {16, 4, 12, false, 0, 1};
    case Density::High: break;
    }
    return {16, 4, 12, true, 0, 1};
}

inline Density densityAt(int index) noexcept { return static_cast<Density>(std::clamp(index, 0, 3)); }

// The lines a Density runs, in order (indices into kLineMs).
inline std::span<const int> activeLines(Density d) noexcept {
    static constexpr std::array<int, 4> kSparse = {2, 6, 10, 14};
    static constexpr std::array<int, 8> kLow = {1, 3, 5, 7, 9, 11, 13, 15};
    static constexpr std::array<int, 16> kAll = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    switch (d) {
    case Density::Sparse: return kSparse;
    case Density::Low: return kLow;
    default: return kAll;
    }
}

// The early taps a Density uses (indices into kTapMs).
inline std::span<const int> activeTaps(Density d) noexcept {
    static constexpr std::array<int, 6> kSparse = {0, 2, 4, 6, 8, 10};
    static constexpr std::array<int, 12> kAll = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    return d == Density::Sparse ? std::span<const int>(kSparse) : std::span<const int>(kAll);
}

// --- Sizes ---------------------------------------------------------------------------------

// Size's factor on every delay: 1 at 100, 0.047 at 0.22 (tiny and metallic),
// 2.24 at 500 (a huge, grainy space).
inline double sizeFactor(double size) noexcept { return std::sqrt(std::clamp(size, kMinSize, kMaxSize) / 100.0); }
// Scale's factor on the diffusers' and loop all-passes' lengths: 0.25..1.75, 1 at 50 %.
inline double scaleFactor(double scalePercent) noexcept {
    return 0.25 + 1.5 * std::clamp(scalePercent, 0.0, 100.0) / 100.0;
}
// Line `line`'s delay in samples at size factor `s`.
inline double lineSamples(int line, double s, double fs) noexcept {
    return std::max(kMinLineSamples, kLineMs[static_cast<size_t>(line)] * s * fs / 1000.0);
}
// High: the all-pass in line `line`'s loop, its delay in samples.
inline double loopAllpassSamples(int line, double s, double c, double fs) noexcept {
    return std::max(2.0, kLoopAllpassShare * kLineMs[static_cast<size_t>(line)] * s * c * fs / 1000.0);
}
// Input diffuser `j`'s delay in samples.
inline double diffuserSamples(int j, double s, double c, double fs) noexcept {
    return std::max(2.0, kDiffuserMs[static_cast<size_t>(j)] * s * c * fs / 1000.0);
}
// Stereo Image's width on the wet's side (mid/side): 0 mono, 1 at its top (120),
// where the two sides are the network's own two independent sums, as Live's 120
// degrees give each ear a channel independent of the other's.
inline double stereoWidth(double stereo) noexcept { return std::clamp(stereo, 0.0, kMaxStereo) / kMaxStereo; }
// How much later than the predelay the network hears the input, in ms: Shape's onset.
inline double onsetMs(double s, double shapePercent) noexcept {
    return std::clamp(shapePercent, 0.0, 100.0) / 100.0 * kOnsetShare * kTapMs[kMaxTaps - 1] * s;
}
// Diffusion's share (0..1) of the all-passes' full gains (kDiffuserGain,
// kLoopAllpassGain): its square root, so the default 70 % already blurs the
// echoes into a smooth tail by 150 ms while low settings stay grainy.
inline double diffusionGain(double share) noexcept { return std::sqrt(std::clamp(share, 0.0, 1.0)); }
// A line's whole loop, in samples: the line, and High's all-pass (its delay on average over frequency).
inline double loopSamples(int line, Density d, double s, double c, double fs) noexcept {
    return lineSamples(line, s, fs) + (layout(d).loopAllpass ? loopAllpassSamples(line, s, c, fs) : 0.0);
}
// The active lines' mean loop: what the editor's decay curve is worked out for.
inline double meanLoopSamples(Density d, double s, double c, double fs) noexcept {
    double sum = 0.0;
    const std::span<const int> lines = activeLines(d);
    for (const int q : lines) sum += loopSamples(q, d, s, c, fs);
    return sum / static_cast<double>(lines.size());
}

// --- The loops' filters --------------------------------------------------------------------

// The TPT one-pole (bilinear, prewarped): G = g / (1 + g), g = tan(pi f / fs),
// f held to 1 Hz..0.45 fs.
inline double tptG(double f, double fs) noexcept {
    const double g = std::tan(std::numbers::pi * std::clamp(f, 1.0, 0.45 * fs) / fs);
    return g / (1.0 + g);
}
// Its low-pass (the high-pass is x minus it). At Nyquist it is exactly 0, so a
// shelf built on it reaches its gain there exactly.
struct OnePoleTpt {
    float s = 0.f;
    float lowpass(float x, float G) noexcept {
        const float v = (x - s) * G;
        const float y = v + s;
        s = y + v;
        return y;
    }
};
// Its low-pass response at f Hz: H(z) = g (1 + z^-1) / ((1 + g) - (1 - g) z^-1).
inline std::complex<double> tptLowpass(double cutoff, double f, double fs) noexcept {
    const double g = std::tan(std::numbers::pi * std::clamp(cutoff, 1.0, 0.45 * fs) / fs);
    const std::complex<double> z1 = std::polar(1.0, -2.0 * std::numbers::pi * f / fs);
    return g * (1.0 + z1) / ((1.0 + g) - (1.0 - g) * z1);
}

// How fast each band decays, in dB per second (negative). hiDiff and loDiff are
// the band's rate less the middle's (0 or below).
struct Rates {
    double mid = 0.0, hiDiff = 0.0, loDiff = 0.0;
};
// From the decay time (s), the shelves' shares of it (0..1, how long their band
// rings), and the switches' blends (0..1): Freeze glides the middle to the
// frozen rate (with Cut, practically none; without, a minute's, so a held input
// settles at a level); Flat glides the shelves' extra rates to none while frozen.
inline Rates rates(double decaySeconds, double loShare, double hiShare, double freezeBlend, double flatBlend,
                   double cutBlend) noexcept {
    const double t = std::max(decaySeconds, 0.01);
    const double frozen = (1.0 - cutBlend) * (-60.0 / kFreezeUncutSeconds) + cutBlend * (-60.0 / kFreezeSeconds);
    const double keep = 1.0 - freezeBlend * flatBlend;
    Rates r;
    r.mid = (1.0 - freezeBlend) * (-60.0 / t) + freezeBlend * frozen;
    r.hiDiff = (-60.0 / (t * std::max(hiShare, 0.01)) + 60.0 / t) * keep;
    r.loDiff = (-60.0 / (t * std::max(loShare, 0.01)) + 60.0 / t) * keep;
    return r;
}

// A line's loop: its gain per pass, and its shelves' gains at the band's far
// end (Nyquist for the high one, DC for the low one), for a loop of
// `loopSamples`. The switches are blends (0..1) as they glide: loOn, hiOn the
// shelves on, lowpassBlend the high filter a low-pass (its far end 0, whatever
// the rate), freezeBlend and flatBlend Freeze with Flat (every band flat).
struct Loop {
    double g = 1.0, kLo = 1.0, kHi = 1.0;
};
inline Loop loop(const Rates& r, double loopSamples, double fs, double loOn, double hiOn, double lowpassBlend,
                 double freezeBlend, double flatBlend) noexcept {
    constexpr double kNepersPerDb = std::numbers::ln10 / 20.0;
    const double seconds = loopSamples / fs;
    const double flat = freezeBlend * flatBlend;
    Loop l;
    l.g = std::exp(r.mid * seconds * kNepersPerDb);
    double kHi = std::exp(r.hiDiff * seconds * kNepersPerDb);
    kHi -= lowpassBlend * kHi;
    kHi = 1.0 + hiOn * (kHi - 1.0);
    l.kHi = kHi + flat * (1.0 - kHi);
    const double kLo = 1.0 + loOn * (std::exp(r.loDiff * seconds * kNepersPerDb) - 1.0);
    l.kLo = kLo + flat * (1.0 - kLo);
    return l;
}

// A loop's gain per pass at f (Hz): its gain through the high shelf
// (H + kHi (1 - H), H the one-pole low-pass at hiFreq) and the low one
// (kLo H + (1 - H) at loFreq); and the decay time that gives there (s, to -60
// dB): +inf where nothing is lost.
inline double loopGainAt(const Loop& l, double hiFreq, double loFreq, double f, double fs) noexcept {
    const std::complex<double> hHi = tptLowpass(hiFreq, f, fs), hLo = tptLowpass(loFreq, f, fs);
    return std::abs(l.g * (hHi + l.kHi * (1.0 - hHi)) * (l.kLo * hLo + (1.0 - hLo)));
}
inline double decaySecondsAt(const Loop& l, double loopSamples, double hiFreq, double loFreq, double f,
                             double fs) noexcept {
    const double gain = loopGainAt(l, hiFreq, loFreq, f, fs);
    if (!(gain < 1.0)) return std::numeric_limits<double>::infinity();
    return -60.0 * loopSamples / (fs * 20.0 * std::log10(std::max(gain, 1e-300)));
}

// What the decay curve depends on, as the device's parameters hold them.
struct DecaySettings {
    double decayMs = 1200.0, size = 100.0, scale = 50.0;
    int density = 3;  // 0 Sparse .. 3 High
    bool loShelf = true, hiFilter = true, hiLowpass = false, freeze = false, flat = true, cut = true;
    double loFreq = 90.0, loGain = 75.0, hiFreq = 4500.0, hiGain = 70.0;  // Hz, % of the decay
};
// The tail's decay time (s) at f Hz, as the network plays it: the loop of a line
// of the active lines' mean length (in a network that mixes every line into
// every other each pass, energy decays at the lines' mean loss over their mean
// length). About 1000 s frozen with Cut and Flat; +inf where nothing is lost.
inline double decaySeconds(const DecaySettings& p, double f, double fs) noexcept {
    const Density d = densityAt(p.density);
    const double loops = meanLoopSamples(d, sizeFactor(p.size), scaleFactor(p.scale), fs);
    const double z = p.freeze ? 1.0 : 0.0;
    const Rates r = rates(p.decayMs / 1000.0, p.loGain / 100.0, p.hiGain / 100.0, z, p.flat ? 1.0 : 0.0,
                          p.cut ? 1.0 : 0.0);
    const Loop l = loop(r, loops, fs, p.loShelf ? 1.0 : 0.0, p.hiFilter ? 1.0 : 0.0, p.hiLowpass ? 1.0 : 0.0, z,
                        p.flat ? 1.0 : 0.0);
    return decaySecondsAt(l, loops, p.hiFreq, p.loFreq, f, fs);
}

// --- The input filter ----------------------------------------------------------------------

// The band's corners: the low cut at freq / 2^(width/2), the high cut at
// freq * 2^(width/2), each held to 10 Hz..0.45 fs.
inline double loCutHz(double freq, double width, double fs) noexcept {
    return std::clamp(freq / std::exp2(0.5 * width), 10.0, 0.45 * fs);
}
inline double hiCutHz(double freq, double width, double fs) noexcept {
    return std::clamp(freq * std::exp2(0.5 * width), 10.0, 0.45 * fs);
}
// Its gain (dB) at f: the bilinear Butterworth pair's exact magnitude,
// |LP|^2 = 1 / (1 + W^4), |HP|^2 = W^4 / (1 + W^4), W = tan(pi f / fs) / tan(pi fc / fs).
inline double inputFilterDb(double freq, double width, bool loCut, bool hiCut, double f, double fs) noexcept {
    const double t = std::tan(std::numbers::pi * std::clamp(f, 0.0, 0.4999 * fs) / fs);
    double power = 1.0;
    if (loCut) {
        const double w = t / std::tan(std::numbers::pi * loCutHz(freq, width, fs) / fs);
        const double w4 = w * w * w * w;
        power *= w4 / (1.0 + w4);
    }
    if (hiCut) {
        const double w = t / std::tan(std::numbers::pi * hiCutHz(freq, width, fs) / fs);
        power /= 1.0 + w * w * w * w;
    }
    return 10.0 * std::log10(std::max(power, 1e-30));
}

// --- The early reflections -----------------------------------------------------------------

// A reflection at rest (Spin aside): its time after the predelay (ms), gain (signed), pan.
struct Tap {
    double ms = 0.0, gain = 0.0, pan = 0.0;
};
// All 12 (out[k] is tap k, whatever the Density: one the Density doesn't use
// has gain 0); returns how many the Density uses. Shape sets their envelope:
// e_k = exp(-a u_k), u_k their time over the last's, a = 0.4 + 5.6 shape; the
// gains share kEarlyGain's energy.
inline int earlyTaps(double s, double shapePercent, Density d, std::array<Tap, kMaxTaps>& out) noexcept {
    const double a = 0.4 + 5.6 * std::clamp(shapePercent, 0.0, 100.0) / 100.0;
    const std::span<const int> taps = activeTaps(d);
    std::array<double, kMaxTaps> e{};
    double sum = 0.0;
    for (const int k : taps) {
        e[static_cast<size_t>(k)] = std::exp(-a * kTapMs[static_cast<size_t>(k)] / kTapMs[kMaxTaps - 1]);
        sum += e[static_cast<size_t>(k)] * e[static_cast<size_t>(k)];
    }
    const double norm = kEarlyGain / std::sqrt(sum);
    for (size_t k = 0; k < kMaxTaps; ++k) out[k] = {kTapMs[k] * s, kTapSign[k] * e[k] * norm, kTapPan[k]};
    return static_cast<int>(taps.size());
}

// Spin swings each reflection around the stereo field, in the angle of an
// equal-power pan (left cos t, right sin t). At rest t = acos(-pan) / 2, which is
// the pan law sqrt((1 -+ pan) / 2); Spin moves it by up to asin(kSpinPanSwing)
// / 2 radians (a reflection in the middle swings +-0.7), less near the edges, so
// a reflection never swings past one and its gains never turn a corner (one at
// +-0.95 swings about 0.2 in from its edge).
inline double tapAngle(int k) noexcept { return 0.5 * std::acos(-kTapPan[static_cast<size_t>(k)]); }
inline double tapSwing(int k) noexcept {
    const double t = tapAngle(k);
    return std::min({0.5 * std::asin(kSpinPanSwing), t, 0.5 * std::numbers::pi - t});
}
// Spin's angle offset for tap k, its amount 0..1 and the LFO's phase in cycles
// (each tap a twelfth of a cycle on from the one before), and the pan that gives.
inline double spinAngle(int k, double amount, double phase) noexcept {
    return amount * tapSwing(k) * std::cos(2.0 * std::numbers::pi * (phase + k / static_cast<double>(kMaxTaps)));
}
inline double spinPan(int k, double amount, double phase) noexcept {
    return -std::cos(2.0 * (tapAngle(k) + spinAngle(k, amount, phase)));
}
// Spin's drift of tap k's time, in ms (at size factor s): later only (0 to twice
// the depth), so no reflection ever comes before the predelay. Tap 0's drift
// also moves where the network hears the input, so Spin reaches the tail too.
inline double spinDriftMs(int k, double amount, double phase, double s) noexcept {
    return amount * kSpinDepthMs * std::min(1.0, s) *
           (1.0 + std::sin(2.0 * std::numbers::pi * (phase + k / static_cast<double>(kMaxTaps))));
}

}  // namespace sub::reverb
