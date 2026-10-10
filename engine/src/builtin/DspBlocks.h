#pragma once
// Building blocks the built-in effects share, beside Dsp.h's: one-pole and DC
// filters, a two-pole glide for controls, a delay line, an envelope follower,
// an LFO with Ableton's shapes and synced rates, RBJ biquads, a Linkwitz-Riley
// crossover, a sliding maximum (for lookahead), white noise, a fast tanh, and
// linear-phase oversampling.
//
// All inline. What allocates says so (prepare()); everything else is real-time
// safe: no locks, no allocation, no exceptions.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "builtin/Dsp.h"

namespace sub::dsp {

inline constexpr double kPi = 3.14159265358979323846;
inline constexpr double kTwoPi = 2.0 * kPi;

// --- One-pole filters ----------------------------------------------------------------------

// A one-pole low-pass (a smoother) and the high-pass it leaves. `coeff` is what
// is left of a step after a sample (onePoleCoefficient() in RtUtils.h).
struct OnePole {
    float z = 0.f;

    void reset(float value = 0.f) noexcept { z = value; }
    float lowpass(float x, float coeff) noexcept {
        z = x + coeff * (z - x);
        return z;
    }
    float highpass(float x, float coeff) noexcept { return x - lowpass(x, coeff); }
};

// The coefficient of a one-pole low-pass at `cutoffHz` (its -3 dB point, near
// enough below a quarter of the rate): what OnePole takes.
inline float onePoleCutoff(double cutoffHz, double sampleRate) noexcept {
    return static_cast<float>(std::exp(-kTwoPi * std::max(0.0, cutoffHz) / sampleRate));
}

// Removes DC: y = x - x[-1] + r y[-1], its corner at `cutoffHz` (5 Hz by default).
class DcBlocker {
public:
    void prepare(double sampleRate, double cutoffHz = 5.0) noexcept { r_ = onePoleCutoff(cutoffHz, sampleRate); }
    void reset() noexcept { x1_ = y1_ = 0.f; }
    float process(float x) noexcept {
        const float y = x - x1_ + r_ * y1_;
        x1_ = x;
        y1_ = std::abs(y) < 1e-20f ? 0.f : y;
        return y;
    }

private:
    float r_ = 0.9993f;
    float x1_ = 0.f, y1_ = 0.f;
};

// --- Glide ---------------------------------------------------------------------------------

// A control's glide towards its target through two one-poles in a row: unlike
// one, it eases in as well as out (no step in its slope when the target jumps,
// which a pitch or a delay time would let you hear). `coefficient` is the share
// of the way each pole moves per step; once both are within `landed` of the
// target it lands there exactly, and settled() is true until the target moves.
struct Glide {
    double first = 0.0, value = 0.0;

    void snap(double target) noexcept { first = value = target; }
    bool settled(double target) const noexcept { return first == target && value == target; }
    double next(double target, double coefficient, double landed) noexcept {
        if (settled(target)) return value;
        first += coefficient * (target - first);
        value += coefficient * (first - value);
        if (std::abs(target - first) < landed && std::abs(target - value) < landed) snap(target);
        return value;
    }
};

// --- Delay line ----------------------------------------------------------------------------

// A ring of floats, a power of two long, written a sample at a time and read
// at a delay in samples (0: the sample just pushed). Fractional reads clamp
// the delay to what the ring holds: linear() to 0..capacity() - 2, hermite()
// to 1..capacity() - 3 (it reads a sample either side).
class DelayLine {
public:
    // Allocates: room for delays up to `maxDelaySamples` (and the reads' neighbours).
    void prepare(int maxDelaySamples) {
        size_t size = 4;
        while (size < static_cast<size_t>(std::max(0, maxDelaySamples)) + 4) size <<= 1;
        buffer_.assign(size, 0.f);
        mask_ = size - 1;
        write_ = 0;
    }
    void reset() noexcept {
        std::fill(buffer_.begin(), buffer_.end(), 0.f);
        write_ = 0;
    }
    int capacity() const noexcept { return static_cast<int>(buffer_.size()); }

    void push(float x) noexcept {
        buffer_[write_] = x;
        write_ = (write_ + 1) & mask_;
    }
    float tap(int delay) const noexcept { return buffer_[(write_ - 1 - static_cast<size_t>(delay)) & mask_]; }
    float linear(float delay) const noexcept {
        delay = std::clamp(delay, 0.f, static_cast<float>(capacity() - 2));
        const int whole = static_cast<int>(delay);
        const float t = delay - static_cast<float>(whole);
        const float a = tap(whole);
        return a + t * (tap(whole + 1) - a);
    }
    float hermite(float delay) const noexcept {
        delay = std::clamp(delay, 1.f, static_cast<float>(capacity() - 3));
        const int whole = static_cast<int>(delay);
        const float t = delay - static_cast<float>(whole);
        // Reading backwards in time: x0 is `whole` samples ago, x1 one further.
        return hermite4(tap(whole - 1), tap(whole), tap(whole + 1), tap(whole + 2), t);
    }

private:
    static float hermite4(float xm1, float x0, float x1, float x2, float t) noexcept {
        return dsp::hermite(xm1, x0, x1, x2, t);
    }

    std::vector<float> buffer_ = std::vector<float>(4, 0.f);
    size_t mask_ = 3;
    size_t write_ = 0;
};

// --- Envelope follower ---------------------------------------------------------------------

// A level that rises with the attack and falls with the release (one-poles:
// each time is how long a step takes to come 1 - 1/e of the way).
class EnvelopeFollower {
public:
    void setTimes(float attackMs, float releaseMs, double sampleRate) noexcept {
        attack_ = coefficient(attackMs, sampleRate);
        release_ = coefficient(releaseMs, sampleRate);
    }
    void reset(float value = 0.f) noexcept { env_ = value; }

    // On a rectified signal (|x|, or the louder channel's).
    float peak(float rectified) noexcept {
        const float coeff = rectified > env_ ? attack_ : release_;
        env_ = rectified + coeff * (env_ - rectified);
        if (env_ < 1e-20f) env_ = 0.f;
        return env_;
    }
    // The mean square, smoothed the same way; returns its square root (an RMS level).
    float rms(float x) noexcept {
        const float square = x * x;
        const float coeff = square > env_ ? attack_ : release_;
        env_ = square + coeff * (env_ - square);
        if (env_ < 1e-30f) env_ = 0.f;
        return std::sqrt(env_);
    }
    // The state as it is: a peak level after peak(), a mean square after rms().
    float value() const noexcept { return env_; }

private:
    static float coefficient(float ms, double sampleRate) noexcept {
        return ms <= 0.f ? 0.f : static_cast<float>(std::exp(-1.0 / (ms * 0.001 * sampleRate)));
    }

    float env_ = 0.f;
    float attack_ = 0.f, release_ = 0.f;
};

// --- Noise ---------------------------------------------------------------------------------

// A 32-bit integer hash (lowbias32): neighbouring inputs give unrelated outputs.
inline uint32_t hash32(uint32_t x) noexcept {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

// A hash as a value in -1..1.
inline float hashToBipolar(uint32_t x) noexcept {
    return static_cast<float>(hash32(x)) * (2.f / 4294967296.f) - 1.f;
}

// White noise in -1..1 (xorshift32): the same sequence from the same seed.
struct Noise {
    uint32_t state = 0x9e3779b9U;

    void seed(uint32_t value) noexcept { state = value != 0 ? value : 0x9e3779b9U; }
    float next() noexcept {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<float>(state) * (2.f / 4294967296.f) - 1.f;
    }
};

// --- Fast tanh -----------------------------------------------------------------------------

// tanh(x) as a 7/6 Padé approximant, within 1e-6 for |x| ≤ 3 and 1e-4 up to
// 4.97, then ±1: what saturation runs per (oversampled) sample, a few times
// cheaper than std::tanh.
inline float fastTanh(float x) noexcept {
    if (x > 4.97f) return 1.f;
    if (x < -4.97f) return -1.f;
    const float x2 = x * x;
    const float num = x * (135135.f + x2 * (17325.f + x2 * (378.f + x2)));
    const float den = 135135.f + x2 * (62370.f + x2 * (3150.f + 28.f * x2));
    return std::clamp(num / den, -1.f, 1.f);
}

// --- LFO -----------------------------------------------------------------------------------

// Ableton's LFO shapes. Every shape is -1..1; Sine and Triangle start at 0
// rising, the saws at their ends, Square high for the first half. Random holds
// a new value each cycle, RandomSmooth glides (cosine) from one cycle's value
// to the next's.
enum class LfoShape { Sine, Triangle, SawUp, SawDown, Square, Random, RandomSmooth };

// A phase in 0..1, counting whole cycles too (the random shapes' values come
// from the cycle's number, so a render from the same start repeats exactly).
class Lfo {
public:
    static float shape(LfoShape s, double phase, uint32_t cycle) noexcept {
        const auto p = static_cast<float>(phase);
        switch (s) {
        case LfoShape::Sine: return static_cast<float>(std::sin(kTwoPi * phase));
        case LfoShape::Triangle: return p < 0.25f ? 4.f * p : (p < 0.75f ? 2.f - 4.f * p : 4.f * p - 4.f);
        case LfoShape::SawUp: return 2.f * p - 1.f;
        case LfoShape::SawDown: return 1.f - 2.f * p;
        case LfoShape::Square: return p < 0.5f ? 1.f : -1.f;
        case LfoShape::Random: return hashToBipolar(cycle);
        case LfoShape::RandomSmooth: {
            const float from = hashToBipolar(cycle), to = hashToBipolar(cycle + 1);
            const float t = 0.5f - 0.5f * static_cast<float>(std::cos(kPi * phase));
            return from + t * (to - from);
        }
        }
        return 0.f;
    }

    void reset() noexcept {
        phase_ = 0.0;
        cycle_ = 0;
    }
    void setPhase(double phase) noexcept {
        const double whole = std::floor(phase);
        phase_ = phase - whole;
    }
    double phase() const noexcept { return phase_; }
    uint32_t cycle() const noexcept { return cycle_; }

    // Moves on by `cycles` (≥ 0), counting the cycles it completes.
    void advance(double cycles) noexcept {
        phase_ += cycles;
        if (phase_ >= 1.0) {
            const double whole = std::floor(phase_);
            phase_ -= whole;
            cycle_ += static_cast<uint32_t>(whole);
        }
    }
    // The shape's value at the phase plus `offset` cycles (a stereo offset, a voice's place).
    float value(LfoShape s, double offset = 0.0) const noexcept {
        const double at = phase_ + offset;
        const double whole = std::floor(at);
        return shape(s, at - whole, cycle_ + static_cast<uint32_t>(static_cast<int64_t>(whole)));
    }

private:
    double phase_ = 0.0;
    uint32_t cycle_ = 0;
};

// Synced LFO rates as note lengths (a bar is a whole note, 4 beats), straight,
// triplet (T, 2/3 as long) and dotted (D, 1.5 times): labels for a list
// parameter, and each one's length in beats.
inline const std::vector<std::string>& syncedDivisionLabels() {
    static const std::vector<std::string> kLabels = {
        "1/64", "1/32T", "1/32", "1/16T", "1/16", "1/16D", "1/8T", "1/8", "1/8D", "1/4T",
        "1/4",  "1/4D",  "1/2T", "1/2",   "1/2D", "1 Bar", "2 Bars", "4 Bars", "8 Bars",
    };
    return kLabels;
}
inline double syncedCycleBeats(int divisionIndex) noexcept {
    static constexpr std::array<double, 19> kBeats = {
        4.0 / 64,       4.0 / 32 * 2 / 3, 4.0 / 32, 4.0 / 16 * 2 / 3, 4.0 / 16, 4.0 / 16 * 1.5, 4.0 / 8 * 2 / 3,
        4.0 / 8,        4.0 / 8 * 1.5,    1.0 * 2 / 3, 1.0,            1.5,                2.0 * 2 / 3,
        2.0,            3.0,              4.0,         8.0,            16.0,               32.0,
    };
    return kBeats[static_cast<size_t>(std::clamp(divisionIndex, 0, static_cast<int>(kBeats.size()) - 1))];
}

// --- Biquads -------------------------------------------------------------------------------

// A second-order section's coefficients from Robert Bristow-Johnson's Audio EQ
// Cookbook, normalized (a0 = 1). Frequencies are kept below 0.49 of the rate.
// Shelves take Q as the cookbook's (0.7071: no overshoot).
struct BiquadCoefficients {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    static BiquadCoefficients lowpass(double f, double q, double sr) noexcept {
        const Pre p(f, q, sr);
        return normalize((1 - p.cosw) / 2, 1 - p.cosw, (1 - p.cosw) / 2, 1 + p.alpha, -2 * p.cosw, 1 - p.alpha);
    }
    static BiquadCoefficients highpass(double f, double q, double sr) noexcept {
        const Pre p(f, q, sr);
        return normalize((1 + p.cosw) / 2, -(1 + p.cosw), (1 + p.cosw) / 2, 1 + p.alpha, -2 * p.cosw, 1 - p.alpha);
    }
    // A band-pass whose peak is at 0 dB.
    static BiquadCoefficients bandpass(double f, double q, double sr) noexcept {
        const Pre p(f, q, sr);
        return normalize(p.alpha, 0, -p.alpha, 1 + p.alpha, -2 * p.cosw, 1 - p.alpha);
    }
    static BiquadCoefficients notch(double f, double q, double sr) noexcept {
        const Pre p(f, q, sr);
        return normalize(1, -2 * p.cosw, 1, 1 + p.alpha, -2 * p.cosw, 1 - p.alpha);
    }
    static BiquadCoefficients allpass(double f, double q, double sr) noexcept {
        const Pre p(f, q, sr);
        return normalize(1 - p.alpha, -2 * p.cosw, 1 + p.alpha, 1 + p.alpha, -2 * p.cosw, 1 - p.alpha);
    }
    static BiquadCoefficients peak(double f, double q, double gainDb, double sr) noexcept {
        const Pre p(f, q, sr);
        const double a = std::pow(10.0, gainDb / 40.0);
        return normalize(1 + p.alpha * a, -2 * p.cosw, 1 - p.alpha * a, 1 + p.alpha / a, -2 * p.cosw,
                         1 - p.alpha / a);
    }
    static BiquadCoefficients lowShelf(double f, double q, double gainDb, double sr) noexcept {
        const Pre p(f, q, sr);
        const double a = std::pow(10.0, gainDb / 40.0);
        const double s = 2 * std::sqrt(a) * p.alpha;
        return normalize(a * ((a + 1) - (a - 1) * p.cosw + s), 2 * a * ((a - 1) - (a + 1) * p.cosw),
                         a * ((a + 1) - (a - 1) * p.cosw - s), (a + 1) + (a - 1) * p.cosw + s,
                         -2 * ((a - 1) + (a + 1) * p.cosw), (a + 1) + (a - 1) * p.cosw - s);
    }
    static BiquadCoefficients highShelf(double f, double q, double gainDb, double sr) noexcept {
        const Pre p(f, q, sr);
        const double a = std::pow(10.0, gainDb / 40.0);
        const double s = 2 * std::sqrt(a) * p.alpha;
        return normalize(a * ((a + 1) + (a - 1) * p.cosw + s), -2 * a * ((a - 1) + (a + 1) * p.cosw),
                         a * ((a + 1) + (a - 1) * p.cosw - s), (a + 1) - (a - 1) * p.cosw + s,
                         2 * ((a - 1) - (a + 1) * p.cosw), (a + 1) - (a - 1) * p.cosw - s);
    }

    // The section's gain at `f` Hz, in dB (an editor's curve, through the app layer).
    double magnitudeDb(double f, double sr) const noexcept {
        const double w = kTwoPi * f / sr;
        const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2 * w), s2 = std::sin(2 * w);
        const double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
        const double dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
        const double num = nr * nr + ni * ni, den = dr * dr + di * di;
        return 10.0 * std::log10(std::max(num, 1e-30) / std::max(den, 1e-30));
    }

private:
    struct Pre {
        double cosw, alpha;
        Pre(double f, double q, double sr) noexcept {
            const double w = kTwoPi * std::clamp(f, 1.0, 0.49 * sr) / sr;
            cosw = std::cos(w);
            alpha = std::sin(w) / (2 * std::max(q, 1e-3));
        }
    };
    static BiquadCoefficients normalize(double b0, double b1, double b2, double a0, double a1, double a2) noexcept {
        return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
    }
};

// A section's state: transposed direct form II, in double (a shelf at 20 Hz
// keeps its precision), flushed to 0 once both states are tiny. Together: the
// two states of a section with poles near z = 1 (low or narrow) nearly cancel,
// and zeroing one alone kicks the other back up, round and round for minutes.
struct Biquad {
    double s1 = 0.0, s2 = 0.0;

    void reset() noexcept { s1 = s2 = 0.0; }
    float process(const BiquadCoefficients& c, float in) noexcept {
        const double x = in;
        const double y = c.b0 * x + s1;
        s1 = c.b1 * x - c.a1 * y + s2;
        s2 = c.b2 * x - c.a2 * y;
        if (std::abs(s1) < 1e-20 && std::abs(s2) < 1e-20) s1 = s2 = 0.0;
        return static_cast<float>(y);
    }
};

// --- Linkwitz-Riley crossover --------------------------------------------------------------

// A 4th-order Linkwitz-Riley crossover (each side two Butterworth TPT
// state-variable sections in a row, as Over The Top's): the low and high
// bands add up to an all-pass, flat in level. CrossoverAllpass is that
// all-pass alone, for the bands that don't go through this crossover, so every
// band has the same phase shift (a 3-band split puts the low band through the
// upper crossover's all-pass).
struct CrossoverCoefficients {
    SvfCoefficients svf;

    static CrossoverCoefficients at(double freq, double sampleRate) noexcept {
        const double f = std::clamp(freq, 1.0, 0.49 * sampleRate);
        return {SvfCoefficients(static_cast<float>(std::tan(kPi * f / sampleRate)), 1.41421356f)};
    }
};

struct Crossover {
    Svf split, low, high;

    void reset() noexcept {
        split.reset();
        low.reset();
        high.reset();
    }
    void flush() noexcept {
        split.flush();
        low.flush();
        high.flush();
    }
    void process(const CrossoverCoefficients& c, float x, float& lowOut, float& highOut) noexcept {
        const Svf::Outputs s = split.tick(c.svf, x);
        const float hp = x - c.svf.k * s.band - s.low;
        lowOut = low.tick(c.svf, s.low).low;
        const Svf::Outputs h = high.tick(c.svf, hp);
        highOut = hp - c.svf.k * h.band - h.low;
    }
};

struct CrossoverAllpass {
    Svf state;

    void reset() noexcept { state.reset(); }
    void flush() noexcept { state.flush(); }
    float process(const CrossoverCoefficients& c, float x) noexcept {
        return x - 2.f * c.svf.k * state.tick(c.svf, x).band;
    }
};

// --- Sliding maximum -----------------------------------------------------------------------

// The largest of the last `window` values pushed (a lookahead limiter's or
// gate's peak ahead): a queue of the values that may still be the largest
// within the longest window (each smaller than the one before it), O(1) per
// value on average. The window may change from one push to the next (a
// lookahead turned): the largest within it is the first value queued that is
// recent enough, found by bisection.
class SlidingMax {
public:
    // Allocates: windows up to `maxWindow` values.
    void prepare(int maxWindow) {
        size_t size = 2;
        while (size < static_cast<size_t>(std::max(1, maxWindow)) + 1) size <<= 1;
        values_.assign(size, 0.f);
        indices_.assign(size, 0);
        mask_ = size - 1;
        maxWindow_ = std::max(1, maxWindow);
        reset();
    }
    void reset() noexcept {
        head_ = tail_ = 0;
        count_ = 0;
    }

    float push(float x, int window) noexcept {
        window = std::clamp(window, 1, maxWindow_);
        while (tail_ != head_ && values_[(tail_ - 1) & mask_] <= x) --tail_;
        values_[tail_ & mask_] = x;
        indices_[tail_ & mask_] = count_;
        ++tail_;
        while (indices_[head_ & mask_] + static_cast<uint64_t>(maxWindow_) <= count_) ++head_;
        const uint64_t oldest = count_ + 1 >= static_cast<uint64_t>(window) ? count_ + 1 - static_cast<uint64_t>(window) : 0;
        ++count_;
        if (indices_[head_ & mask_] >= oldest) return values_[head_ & mask_];
        size_t low = head_, high = tail_ - 1;  // (the value just pushed is always recent enough)
        while (low < high) {
            const size_t mid = low + (high - low) / 2;
            if (indices_[mid & mask_] >= oldest)
                high = mid;
            else
                low = mid + 1;
        }
        return values_[low & mask_];
    }

private:
    std::vector<float> values_ = std::vector<float>(2, 0.f);
    std::vector<uint64_t> indices_ = std::vector<uint64_t>(2, 0);
    size_t mask_ = 1;
    size_t head_ = 0, tail_ = 0;
    uint64_t count_ = 0;
    int maxWindow_ = 1;
};

// --- Kaiser windows ------------------------------------------------------------------------

// The modified Bessel function of the first kind of order 0, by its series (to 1e-12 of the
// sum): a Kaiser window of β is I0(β √(1 - r²)) / I0(β) for r from -1 to 1. For designing
// filters (oversampling's, the Limiter's true-peak interpolator), not per sample.
inline double besselI0(double x) noexcept {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 50; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

// --- Oversampling --------------------------------------------------------------------------

// Linear-phase oversampling by 2, 4 or 8 (stages of 2, each a half-band FIR,
// Kaiser-windowed sinc, run polyphase so the zero taps cost nothing), or none.
// One per channel. The first stage is the steep one: flat within 0.005 dB to
// 0.42 of the base rate (20 kHz at 48 kHz), images and what would fold back
// more than 80 dB down from 0.58 of it; the later stages only need to keep what
// would fold back into that band, so they are short.
// Up and down together delay the signal by a whole number of base-rate samples
// (latencySamples(): 31, 36, 38 for 2x, 4x, 8x); a dry path mixed back in is
// delayed by as much.
class Oversampler {
public:
    static constexpr int kMaxFactorLog2 = 3;

    // Allocates: blocks up to `maxBlock` base-rate samples, factors up to 2^maxFactorLog2.
    void prepare(int maxBlock, int maxFactorLog2 = kMaxFactorLog2) {
        maxBlock_ = std::max(1, maxBlock);
        maxFactorLog2_ = std::clamp(maxFactorLog2, 0, kMaxFactorLog2);
        for (int s = 0; s < maxFactorLog2_; ++s) stages_[static_cast<size_t>(s)].design(kStageTaps[s], kStageBeta[s], maxBlock_ << s);
        const size_t most = static_cast<size_t>(maxBlock_) << maxFactorLog2_;
        work_[0].assign(most, 0.f);
        work_[1].assign(most, 0.f);
        factorLog2_ = std::min(factorLog2_, maxFactorLog2_);
        reset();
    }
    void reset() noexcept {
        for (Stage& s : stages_) s.reset();
    }
    // 0..maxFactorLog2: 1x, 2x, 4x, 8x. Real-time safe (no allocation); the stages start from silence.
    void setFactorLog2(int factorLog2) noexcept {
        factorLog2 = std::clamp(factorLog2, 0, maxFactorLog2_);
        if (factorLog2 == factorLog2_) return;
        factorLog2_ = factorLog2;
        reset();
    }
    int factorLog2() const noexcept { return factorLog2_; }
    int factor() const noexcept { return 1 << factorLog2_; }
    int latencySamples() const noexcept { return latencyFor(factorLog2_); }
    static int latencyFor(int factorLog2) noexcept {
        int latency = 0;
        for (int s = 0; s < std::clamp(factorLog2, 0, kMaxFactorLog2); ++s) latency += ((kStageTaps[s] - 1) / 2) >> s;
        return latency;
    }

    // `n` base-rate samples (n ≤ maxBlock) up to n * factor(): the result is the
    // oversampler's own buffer, to process in place and hand to down().
    float* up(const float* in, int n) noexcept {
        float* from = work_[0].data();
        std::copy_n(in, n, from);
        int length = n;
        for (int s = 0; s < factorLog2_; ++s) {
            float* to = work_[(s + 1) & 1].data();
            stages_[static_cast<size_t>(s)].up(from, length, to);
            from = to;
            length *= 2;
        }
        return from;
    }
    // n * factor() oversampled samples (up()'s buffer) down to `n` at the base rate.
    void down(const float* oversampled, int n, float* out) noexcept {
        if (factorLog2_ == 0) {
            std::copy_n(oversampled, n, out);
            return;
        }
        int length = n << factorLog2_;
        const float* from = oversampled;
        for (int s = factorLog2_ - 1; s >= 0; --s) {
            length /= 2;
            float* to = s == 0 ? out : work_[(from == work_[0].data()) ? 1 : 0].data();
            stages_[static_cast<size_t>(s)].down(from, length, to);
            from = to;
        }
    }

private:
    // Taps per stage: centres at 31, 10 and 8, so up and down delay a whole number of base samples.
    static constexpr std::array<int, kMaxFactorLog2> kStageTaps = {63, 21, 17};
    static constexpr std::array<double, kMaxFactorLog2> kStageBeta = {7.8, 7.9, 7.9};

    // One 2x stage: the half-band filter h (unity gain at DC) split into its
    // phases' non-zero taps, and the histories of both directions. Both run a
    // tap at a time over the whole block (contiguous, so the compiler
    // vectorises it), adding the taps in the same order a sample at a time
    // would.
    struct Stage {
        int taps = 0;
        // Up: output 2n + p = 2 * sum over taps j = p (mod 2) of h[j] x[n - (j - p) / 2].
        std::array<std::vector<int>, 2> upDelay;
        std::array<std::vector<float>, 2> upCoeff;
        int upHistory = 0;
        std::vector<float> upBuffer;
        std::array<std::vector<float>, 2> upSums;
        // Down: output n = sum over taps j of h[j] v[2n - j], read from v's even and odd samples
        // apart: an even j reads even[n - j / 2], an odd one odd[n - (j + 1) / 2].
        std::vector<int> downOffset;
        std::vector<float> downCoeff;
        int downHistory = 0;
        std::vector<float> downEven, downOdd, downSums;

        void design(int count, double beta, int maxIn) {
            taps = count;
            const int centre = (count - 1) / 2;
            std::vector<double> h(static_cast<size_t>(count));
            const double i0Beta = besselI0(beta);
            double oddSum = 0.0;
            for (int j = 0; j < count; ++j) {
                const int m = j - centre;
                const double sinc = m == 0 ? 0.5 : std::sin(kPi * m / 2.0) / (kPi * m);
                const double r = static_cast<double>(m) / centre;
                const double window = besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0Beta;
                h[static_cast<size_t>(j)] = (m % 2 == 0 && m != 0) ? 0.0 : sinc * window;
                if (m % 2 != 0) oddSum += h[static_cast<size_t>(j)];
            }
            // Both phases of the up filter pass DC at unity: no image of DC at the old Nyquist.
            for (int j = 0; j < count; ++j) {
                if ((j - centre) % 2 != 0) h[static_cast<size_t>(j)] *= 0.5 / oddSum;
            }
            for (int p = 0; p < 2; ++p) {
                upDelay[static_cast<size_t>(p)].clear();
                upCoeff[static_cast<size_t>(p)].clear();
            }
            downOffset.clear();
            downCoeff.clear();
            for (int j = 0; j < count; ++j) {
                const double c = h[static_cast<size_t>(j)];
                if (c == 0.0) continue;
                const int p = j % 2;
                upDelay[static_cast<size_t>(p)].push_back((j - p) / 2);
                upCoeff[static_cast<size_t>(p)].push_back(static_cast<float>(2.0 * c));
                downOffset.push_back(j);
                downCoeff.push_back(static_cast<float>(c));
            }
            upHistory = (count - 1) / 2 + 1;
            downHistory = count / 2 + 1;
            upBuffer.assign(static_cast<size_t>(upHistory + maxIn), 0.f);
            for (std::vector<float>& sums : upSums) sums.assign(static_cast<size_t>(maxIn), 0.f);
            downEven.assign(static_cast<size_t>(downHistory + maxIn), 0.f);
            downOdd.assign(static_cast<size_t>(downHistory + maxIn), 0.f);
            downSums.assign(static_cast<size_t>(maxIn), 0.f);
        }
        void reset() noexcept {
            std::fill(upBuffer.begin(), upBuffer.end(), 0.f);
            std::fill(downEven.begin(), downEven.end(), 0.f);
            std::fill(downOdd.begin(), downOdd.end(), 0.f);
        }
        void up(const float* in, int n, float* out) noexcept {
            float* buffer = upBuffer.data();
            std::copy_n(in, n, buffer + upHistory);
            for (size_t p = 0; p < 2; ++p) {
                float* sums = upSums[p].data();
                std::fill_n(sums, n, 0.f);
                for (size_t k = 0; k < upDelay[p].size(); ++k) {
                    const float c = upCoeff[p][k];
                    const float* x = buffer + upHistory - upDelay[p][k];
                    for (int i = 0; i < n; ++i) sums[i] += c * x[i];
                }
            }
            const float* even = upSums[0].data();
            const float* odd = upSums[1].data();
            for (int i = 0; i < n; ++i) {
                out[2 * i] = even[i];
                out[2 * i + 1] = odd[i];
            }
            std::copy_n(buffer + n, upHistory, buffer);
        }
        void down(const float* in, int n, float* out) noexcept {
            float* even = downEven.data();
            float* odd = downOdd.data();
            for (int i = 0; i < n; ++i) {
                even[downHistory + i] = in[2 * i];
                odd[downHistory + i] = in[2 * i + 1];
            }
            float* sums = downSums.data();
            std::fill_n(sums, n, 0.f);
            for (size_t k = 0; k < downOffset.size(); ++k) {
                const int j = downOffset[k];
                const float c = downCoeff[k];
                const float* v = j % 2 == 0 ? even + downHistory - j / 2 : odd + downHistory - (j + 1) / 2;
                for (int i = 0; i < n; ++i) sums[i] += c * v[i];
            }
            std::copy_n(sums, n, out);
            std::copy_n(even + n, downHistory, even);
            std::copy_n(odd + n, downHistory, odd);
        }
    };

    std::array<Stage, kMaxFactorLog2> stages_;
    std::array<std::vector<float>, 2> work_;
    int maxBlock_ = 1;
    int maxFactorLog2_ = 0;
    int factorLog2_ = 0;
};

}  // namespace sub::dsp
