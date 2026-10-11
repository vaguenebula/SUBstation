// Built-in "Limiter" device, after Ableton Live 12.1's: a brick-wall lookahead
// limiter. No sample comes out above the ceiling; in True Peak mode no peak
// between samples does either. Gain pushes the input into it, Release (or
// Auto) sets how the gain comes back, Lookahead (1.5, 3 or 6 ms, the device's
// latency) how far ahead peaks are seen. Soft Clip rounds peaks off as they
// near the ceiling instead of turning them all down. Routing limits left and
// right, or mid and side, with Link sharing the gain reduction between the two.
// Maximize turns the ceiling into a threshold: the gain is Output - Threshold
// and the Output is the ceiling.
//
// - Everything runs in a normalized domain where the line (the ceiling, or the
//   threshold with Maximize) is 1 (limiter::scales()): `pre` multiplies the
//   input, `post` the output. Both glide over 20 ms along a smooth curve in dB
//   (its slope never jumps), the same for both, so a moving ceiling leaves
//   material below it untouched (pre · post stays the gain). `post` and Soft
//   Clip's amount travel with their sample through a ring as long as the
//   lookahead, so each sample is detected and scaled with the same values: a
//   change is a smooth level change and never breaks the brick wall. (Input
//   that isn't finite never reaches it: BuiltinProcessor takes it as 0.)
// - The detector runs 12 samples behind the input (kDetectorDelay): it measures
//   each sample's peak (Standard, Soft Clip), or its true peak (True Peak:
//   seven interpolated points between samples, 24 taps each, the largest
//   refined by a parabola), of left, right, mid and side. From those it works
//   out the gain each of four pipelines (L, R, M, S) must have at most, with
//   Link blending each channel's own requirement with the pair's shared one.
// - A pipeline holds the deepest requirement over the attack's length S (a
//   sliding maximum), releases it (Release's one-pole, or Auto's: 50 ms after a
//   lone peak, slowing to 600 ms as the limiting gets dense), and smooths it
//   with two box filters whose lengths add up to S. The gain each sample gets
//   is an average of values that are each at least what that sample needs, so
//   it is never above it, and reaches it exactly when a lone peak comes out,
//   after an S-curve S - 1 samples long. The boxes sum in fixed point
//   (multiples of 2^-30, rounded up): exact, so they never drift and come back
//   to a gain of exactly 1.
// - The output is the delayed input times the routing's gains (both routings
//   are always worked out, and Routing crossfades between them, each obeying
//   the ceiling), through Soft Clip's knee, clamped to the line, times `post`.
// - Mode, Routing, Link and Auto glide (20 ms; the blends ease in and out, so a
//   crossfade has no kink where it starts or stops); a lookahead change dips the
//   output (2 ms out, the line refilled, 2 ms in) and starts again from the new
//   length; idle() then has the engine realign the tracks.
// - Displays, one value per 128 samples, all measured where the sample comes
//   out: the input's peak (in the line's domain), the output's peak (dBFS), the
//   gain reduction of the routing's two channels, and what Soft Clip took off.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/Dsp.h"
#include "builtin/DspBlocks.h"
#include "builtin/LimiterDesign.h"
#include "rt/RtUtils.h"

#if defined(__AVX__)
#include <immintrin.h>
#endif

namespace sub {
namespace {

using limiter::kDetectorDelay;
using limiter::kMeterSamples;
using limiter::kTaps;

constexpr int kCentre = kTaps / 2 - 1;  // where sample m is in the detector's window (x[m - 11 .. m + 12])
constexpr int kLanes = 8;               // the interpolator's points per sample worked out at once: k = 1..8
constexpr float kDepthScale = static_cast<float>(limiter::kDepthScale);

// A gain that glides to its target over `length` samples along a cubic in dB:
// from where it is, at the slope it has, to the target with no slope left (from
// rest, a smoothstep). Its slope doesn't jump where a glide starts or ends, nor
// when a new target further on comes mid-glide (only when one turns it back),
// so a level change has no kink to click on; and it never overshoots. Two of
// them moving together keep their product's own curve (the cubic is linear in
// its ends and slopes): pre · post stays the gain while the ceiling moves.
// Gains here are always above 0, so the logs exist. A cubic's second difference
// is constant, so each sample is three products (the gain by its step's ratio,
// that ratio by its own, and that by a constant); a log and three exp() per new
// target (a render call at most).
struct GainGlide {
    double current = 1.0;
    double step = 1.0, stepRatio = 1.0, stepRatio2 = 1.0;  // the next sample's ratio, its ratio, and that's
    float target = 1.f;
    int remaining = 0, length = 1;

    void reset(double sampleRate, double seconds) noexcept {
        length = std::max(1, static_cast<int>(sampleRate * seconds));
        snapTo(target);
    }
    void snapTo(float value) noexcept {
        current = target = value;
        step = stepRatio = stepRatio2 = 1.0;
        remaining = 0;
    }
    void setTarget(float value) noexcept {
        if (value == target) return;
        target = value;
        // ln gain over the next n samples, from here: slope k + a2 k² + a3 k³, reaching the target
        // with no slope at k = n. The slope it has is carried on as far as the curve stays between
        // here and the target: up to 3 times the straight line's (Fritsch and Carlson's bound for a
        // monotone cubic), none when the target is the other way. So a ceiling gliding is never
        // above both its old and its new value.
        const double n = length, rise = std::log(static_cast<double>(value) / current), line = rise / n;
        const double slope = std::clamp(std::log(step), std::min(0.0, 3.0 * line), std::max(0.0, 3.0 * line));
        const double a2 = (3.0 * rise - 2.0 * slope * n) / (n * n), a3 = (slope * n - 2.0 * rise) / (n * n * n);
        // Its steps L(k + 1) - L(k): the first is slope + a2 + a3; their differences start at
        // 2 a2 + 6 a3 and grow by 6 a3 each.
        step = std::exp(slope + a2 + a3);
        stepRatio = std::exp(2.0 * a2 + 6.0 * a3);
        stepRatio2 = std::exp(6.0 * a3);
        remaining = length;
    }
    float next() noexcept {
        if (remaining > 0) {
            if (--remaining == 0) {
                current = target;
                step = stepRatio = stepRatio2 = 1.0;
            } else {
                current *= step;
                step *= stepRatio;
                stepRatio *= stepRatio2;
            }
        }
        return static_cast<float>(current);
    }
};

// The interpolator's points k = 1..8 for one or two windows: out[k - 1] = Σ_t
// rows[t][k - 1] window[t]. With AVX, a vector of the eight points per tap
// (compilers turn the plain loop inside out, a shuffle per value, several
// times slower); elsewhere the plain loop. Each point is summed in the same
// order either way, over even and odd taps apart.
template <int kWindows>
inline void interpolateRows(const float (&rows)[kTaps][kLanes], const float* const* windows,
                            float* const* out) noexcept {
#if defined(__AVX__)
    __m256 even[kWindows], odd[kWindows];
    for (int w = 0; w < kWindows; ++w) even[w] = odd[w] = _mm256_setzero_ps();
    for (int t = 0; t < kTaps; t += 2) {
        const __m256 a = _mm256_loadu_ps(rows[t]), b = _mm256_loadu_ps(rows[t + 1]);
        for (int w = 0; w < kWindows; ++w) {
            even[w] = _mm256_add_ps(even[w], _mm256_mul_ps(a, _mm256_set1_ps(windows[w][t])));
            odd[w] = _mm256_add_ps(odd[w], _mm256_mul_ps(b, _mm256_set1_ps(windows[w][t + 1])));
        }
    }
    for (int w = 0; w < kWindows; ++w) _mm256_storeu_ps(out[w], _mm256_add_ps(even[w], odd[w]));
#else
    for (int w = 0; w < kWindows; ++w) {
        float even[kLanes] = {}, odd[kLanes] = {};
        for (int t = 0; t < kTaps; t += 2) {
            for (int k = 0; k < kLanes; ++k) {
                even[k] += rows[t][k] * windows[w][t];
                odd[k] += rows[t + 1][k] * windows[w][t + 1];
            }
        }
        for (int k = 0; k < kLanes; ++k) out[w][k] = even[k] + odd[k];
    }
#endif
}

// The last kTaps samples of a channel, always contiguous (each written twice,
// kTaps apart), for the detector's dot products.
struct History {
    float buffer[2 * kTaps] = {};
    int at = 0;

    void reset() noexcept {
        std::fill(std::begin(buffer), std::end(buffer), 0.f);
        at = 0;
    }
    // Writes x; returns the last kTaps samples, oldest first (x last).
    const float* push(float x) noexcept {
        buffer[at] = x;
        buffer[at + kTaps] = x;
        const float* window = buffer + at + 1;
        at = at + 1 == kTaps ? 0 : at + 1;
        return window;
    }
};

// Auto release's coefficient by how dense the limiting has been (0..1), in steps
// worked out by prepare() (limiter::autoReleaseMs), read in between linearly.
constexpr int kAutoSteps = 32;

// What every pipeline shares between reconfigurations (and the release, per render call).
struct PipelineShape {
    int window = 1;        // S: the hold's window and the boxes' combined support
    int64_t a = 1, b = 1;  // the boxes' lengths (a + b - 1 = S)
    int64_t bScaled = static_cast<int64_t>(limiter::kDepthScale);  // b · 2^30
    double inverse = 1.0 / limiter::kDepthScale;                   // 1 / (b · 2^30)
    size_t mask = 1;
    float release = 0.f, density = 0.f;           // one-pole coefficients: Release's; Auto's density's
    std::array<float, kAutoSteps + 1> automatic{};  // Auto's release coefficient at densities k / kAutoSteps

    float autoRelease(float dense) const noexcept {
        const float at = dense * static_cast<float>(kAutoSteps);
        const int k = std::min(static_cast<int>(at), kAutoSteps - 1);
        const auto i = static_cast<size_t>(k);
        return automatic[i] + (at - static_cast<float>(k)) * (automatic[i + 1] - automatic[i]);
    }
};

// One channel's gain computer (L, R, M or S): the required depth (1 - gain) in, the gain out.
struct Pipeline {
    std::array<dsp::SlidingMax, 3> holds;  // one per lookahead, each exactly its window long
    dsp::SlidingMax* hold = &holds[1];
    float manual = 0.f, automatic = 0.f;  // the release stages, on the depth: Release's and Auto's
    float density = 0.f;                  // how much of the recent time it was limiting (0..1)
    std::vector<int32_t> ring1, ring2;    // the boxes' last values, in units of 2^-30
    int64_t sum1 = 0, sum2 = 0;

    void clear(int choice) noexcept {
        hold = &holds[static_cast<size_t>(choice)];
        hold->reset();
        manual = automatic = density = 0.f;
        std::fill(ring1.begin(), ring1.end(), 0);
        std::fill(ring2.begin(), ring2.end(), 0);
        sum1 = sum2 = 0;
    }

    float next(float required, const PipelineShape& s, float autoShare, size_t pos) noexcept {
        const float held = hold->push(required, s.window);
        manual = dsp::followPeak(manual, held, s.release);
        // Auto: released as slowly as the limiting has lately been dense. A lone peak's comes
        // back in about 50 ms; under sustained limiting (a bass note: its crests limited one
        // after another) it slows to 600 ms, so the gain doesn't follow each cycle.
        const float limiting = held > 0.f ? 1.f : 0.f;
        density = limiting + s.density * (density - limiting);
        automatic = dsp::followPeak(automatic, held, s.autoRelease(density));
        if (manual < limiter::kTinyDepth) manual = 0.f;
        if (automatic < limiter::kTinyDepth) automatic = 0.f;
        if (density < limiter::kTinyDepth) density = 0.f;
        // Both stages are at least `held`, so any blend of them is too (the max only
        // guards against rounding an ulp under it).
        const float depth = std::max(held, manual + autoShare * (automatic - manual));
        // Rounded up to a multiple of 2^-30 (exact: depth · 2^30 is at most 2^30, and
        // a float that large is a whole number already).
        const float scaled = depth * kDepthScale;
        auto q1 = static_cast<int32_t>(scaled);
        q1 += static_cast<float>(q1) < scaled ? 1 : 0;
        sum1 += q1 - ring1[(pos - static_cast<size_t>(s.a)) & s.mask];
        ring1[pos] = q1;
        // The first box's average, rounded up (never below what it averages).
        const auto q2 = static_cast<int32_t>((sum1 + s.a - 1) / s.a);
        sum2 += q2 - ring2[(pos - static_cast<size_t>(s.b)) & s.mask];
        ring2[pos] = q2;
        // 1 - the second box's average, from what is left (no cancellation near full depth).
        return static_cast<float>(static_cast<double>(s.bScaled - sum2) * s.inverse);
    }
};

// The output's dip around a lookahead change: out over kDipSeconds, silent while
// the delay line refills, back in. The fades are smootherstep curves (flat in
// value, slope and curvature at both ends).
struct Dip {
    enum class Phase { None, Out, Wait, In };
    Phase phase = Phase::None;
    int remaining = 0, length = 1;

    static float curve(float t) noexcept { return t * t * t * (10.f + t * (6.f * t - 15.f)); }
    // The next sample's gain; Out ends (at 0) when `remaining` reaches 0, and the
    // device starts the wait (it reconfigures in between).
    float next() noexcept {
        switch (phase) {
        case Phase::Out:
            --remaining;
            return curve(static_cast<float>(remaining) / static_cast<float>(length));
        case Phase::Wait:
            if (--remaining <= 0) {
                phase = Phase::In;
                remaining = length;
            }
            return 0.f;
        case Phase::In: {
            --remaining;
            const float gain = curve(1.f - static_cast<float>(remaining) / static_cast<float>(length));
            if (remaining <= 0) phase = Phase::None;
            return gain;
        }
        case Phase::None: break;
        }
        return 1.f;
    }
};

class LimiterProcessor final : public BuiltinProcessor {
public:
    enum Param { Gain = 0, Ceiling, Release, AutoRelease, Lookahead, Mode, Routing, Link, Maximize, Threshold, Output,
                 NumParams };
    enum Display { InputL = 0, InputR, OutputL, OutputR, ReductionA, ReductionB, ClipDisplay };
    enum ModeChoice { Standard = 0, SoftClip, TruePeak };

    LimiterProcessor()
        : BuiltinProcessor(infos(), {{"input_l", kMeterSamples},
                                     {"input_r", kMeterSamples},
                                     {"output_l", kMeterSamples},
                                     {"output_r", kMeterSamples},
                                     {"reduction_a", kMeterSamples},
                                     {"reduction_b", kMeterSamples},
                                     {"clip", kMeterSamples}}) {}

    std::string typeId() const override { return "builtin:limiter"; }
    std::string name() const override { return "Limiter"; }

    int latencySamples() const override { return limiter::lookaheadSamples(choiceIndex(Lookahead), sampleRate_); }
    // The delay line still holds the lookahead's samples when the input stops.
    int tailSamples() const override { return latencySamples(); }

    // The lookahead changed: the engine must realign the tracks.
    bool idle() override {
        const int latency = latencySamples();
        if (latency == reportedLatency_) return false;
        reportedLatency_ = latency;
        return true;
    }

    void prepare(double sampleRate, int) override {
        sampleRate_ = sampleRate;
        const int most = limiter::lookaheadSamples(2, sampleRate);
        for (auto& line : delay_) line.prepare(most + 1);
        postRing_.prepare(most + 1);
        softRing_.prepare(most + 1);
        size_t ringSize = 2;
        while (ringSize < static_cast<size_t>(most) + 2) ringSize <<= 1;
        shape_.mask = ringSize - 1;
        for (Pipeline& pipe : pipes_) {
            pipe.ring1.assign(ringSize, 0);
            pipe.ring2.assign(ringSize, 0);
            for (int choice = 0; choice < 3; ++choice) {
                const int window = windowFor(limiter::lookaheadSamples(choice, sampleRate));
                pipe.holds[static_cast<size_t>(choice)].prepare(window);
            }
        }
        // The interpolator's phases as rows: tap t's coefficient for each point k = 1..7,
        // and for k = 8 the next sample itself.
        const limiter::Phases& phases = limiter::truePeakPhases();
        for (int t = 0; t < kTaps; ++t) {
            for (int k = 0; k < kLanes - 1; ++k) rows_[t][k] = phases[static_cast<size_t>(k)][static_cast<size_t>(t)];
            rows_[t][kLanes - 1] = t == kCentre + 1 ? 1.f : 0.f;
        }
        shape_.density = coefficient(limiter::kAutoDensityMs);
        for (int k = 0; k <= kAutoSteps; ++k)
            shape_.automatic[static_cast<size_t>(k)] =
                coefficient(limiter::autoReleaseMs(static_cast<double>(k) / kAutoSteps));
        dip_.length = std::max(1, static_cast<int>(std::lround(limiter::kDipSeconds * sampleRate)));
        pre_.reset(sampleRate, limiter::kRampSeconds);
        post_.reset(sampleRate, limiter::kRampSeconds);
        for (SmoothedValue* s : {&softness_, &routing_, &link_, &autoShare_})
            s->reset(sampleRate, limiter::kRampSeconds);
        reportedLatency_ = latencySamples();
        reset();
    }

    // Silent, and every glide where the parameters are; the lookahead's new length at once.
    void reset() override {
        setTargets();
        pre_.snapTo(pre_.target);
        post_.snapTo(post_.target);
        for (SmoothedValue* s : {&softness_, &routing_, &link_, &autoShare_}) s->snapTo(s->target());
        configure(choiceIndex(Lookahead));
        dip_.phase = Dip::Phase::None;
        truePeak_ = choiceIndex(Mode) == TruePeak;
        clearMeters();
        meterCount_ = 0;
    }

protected:
    void render(const ProcessContext&, float* const* ch, int numChannels, int numFrames) override {
        const int n = std::min(numChannels, 2);
        if (n <= 0) return;
        if (n != channels_) {  // a channel that wasn't processed has no history to go on from
            channels_ = n;
            configure(choice_);
        }
        setTargets();
        shape_.release = coefficient(std::max(0.1f, param(Release)));
        if (dip_.phase == Dip::Phase::None && latencySamples() != lookahead_) {
            dip_.phase = Dip::Phase::Out;
            dip_.remaining = dip_.length;
        }
        const bool truePeak = choiceIndex(Mode) == TruePeak;
        if (truePeak && !truePeak_) prev7_[0] = prev7_[1] = 0.f;  // (a neighbour of 0 reads high, never low)
        truePeak_ = truePeak;

        int done = 0;
        while (done < numFrames) {
            int end = numFrames;
            if (dip_.phase == Dip::Phase::Out) end = std::min(numFrames, done + dip_.remaining);
            if (n == 2) {
                if (truePeak) renderFrames<true, true>(ch, done, end);
                else renderFrames<true, false>(ch, done, end);
            } else {
                if (truePeak) renderFrames<false, true>(ch, done, end);
                else renderFrames<false, false>(ch, done, end);
            }
            done = end;
            if (dip_.phase == Dip::Phase::Out && dip_.remaining <= 0) {
                // Faded out: start again from the new length, and wait for the line to refill.
                configure(choiceIndex(Lookahead));
                dip_.phase = Dip::Phase::Wait;
                dip_.remaining = lookahead_;
            }
        }
    }

private:
    static int windowFor(int lookahead) noexcept { return lookahead + 1 - kDetectorDelay; }

    float coefficient(double ms) const noexcept {
        return static_cast<float>(onePoleCoefficient(ms * 0.001, sampleRate_));
    }

    // The glides' targets from the parameters (each render call: automation splits blocks).
    void setTargets() noexcept {
        const limiter::Scales s =
            limiter::scales(isOn(Maximize), param(Gain), param(Ceiling), param(Threshold), param(Output));
        pre_.setTarget(s.pre);
        post_.setTarget(s.post);
        lineDb_ = s.lineDb;
        softness_.setTarget(choiceIndex(Mode) == SoftClip ? 1.f : 0.f);
        routing_.setTarget(choiceIndex(Routing) == 1 ? 1.f : 0.f);
        link_.setTarget(std::clamp(param(Link), 0.f, 100.f) / 100.f);
        autoShare_.setTarget(isOn(AutoRelease) ? 1.f : 0.f);
    }

    // The lookahead's lengths for a choice, and everything that runs through them
    // cleared (fills of a few thousand values at most: real-time safe).
    void configure(int choice) noexcept {
        choice_ = std::clamp(choice, 0, 2);
        lookahead_ = limiter::lookaheadSamples(choice_, sampleRate_);
        shape_.window = windowFor(lookahead_);
        shape_.a = (shape_.window + 2) / 2;
        shape_.b = shape_.window + 1 - shape_.a;
        shape_.bScaled = shape_.b * static_cast<int64_t>(limiter::kDepthScale);
        shape_.inverse = 1.0 / static_cast<double>(shape_.bScaled);
        for (Pipeline& pipe : pipes_) pipe.clear(choice_);
        pos_ = 0;
        for (auto& line : delay_) line.reset();
        for (auto& history : history_) history.reset();
        prev7_[0] = prev7_[1] = 0.f;
        fill(postRing_, static_cast<float>(post_.current));
        fill(softRing_, dsp::sCurve(softness_.current()));
    }

    static void fill(dsp::DelayLine& line, float value) noexcept {
        for (int i = 0; i < line.capacity(); ++i) line.push(value);
    }

    void clearMeters() noexcept {
        inPeak_[0] = inPeak_[1] = outPeak_[0] = outPeak_[1] = 0.f;
        minGain_.fill(1.f);
        clipRatio_ = 1.f;
    }

    void publishMeters(bool stereo) noexcept {
        const auto level = [](float peak, float offsetDb) {
            return std::max(limiter::kFloorDb, gainToDb(peak) + offsetDb);
        };
        const int right = stereo ? 1 : 0;
        publish(InputL, level(inPeak_[0], lineDb_));
        publish(InputR, level(inPeak_[right], lineDb_));
        publish(OutputL, level(outPeak_[0], 0.f));
        publish(OutputR, level(outPeak_[right], 0.f));
        // The routing's two channels: L and R, or M and S (where Routing is heading); mono: the one.
        const size_t first = stereo && routing_.target() >= 0.5f ? 2 : 0;
        publish(ReductionA, limiter::reductionDb(minGain_[first]));
        publish(ReductionB, limiter::reductionDb(minGain_[stereo ? first + 1 : first]));
        publish(ClipDisplay, clipRatio_ > 1.f ? 20.f * std::log10(clipRatio_) : 0.f);
        clearMeters();
    }

    // The largest magnitude among the 8x grid's points around sample m (v[0..9]: k = -1..8).
    static float gridPeak(const float* v) noexcept {
        float most = std::abs(v[0]);
        for (int k = 1; k < kLanes + 2; ++k) most = std::max(most, std::abs(v[k]));
        return most;
    }

    // The peak over [m, m + 1) from the grid's points v[0..9] (k = -1..8): the
    // largest refined by a parabola through it and its neighbours.
    static float truePeakOf(const float* v) noexcept {
        int best = 1;
        float most = std::abs(v[1]);
        for (int k = 2; k <= kLanes; ++k) {
            const float a = std::abs(v[k]);
            const bool more = a > most;
            most = more ? a : most;
            best = more ? k : best;
        }
        const float sign = v[best] < 0.f ? -1.f : 1.f;
        return limiter::refinedPeak(sign * v[best - 1], sign * v[best], sign * v[best + 1]);
    }

    // The parabola raises the largest point y0 by (B - A)² / 8 (A + B) for
    // A = y0 - ym, B = y0 - yp: at most max(A, B) / 8, and A and B are at most
    // twice the largest magnitude M of the points k = -1..8, so the peak found is
    // at most 1.25 M. Where that is under the target, no gain reduction can
    // follow: the refinement is skipped.
    static constexpr float kRefinedAtMost = 1.25f;

    // The 8x grid's points k = -1..8 around sample m (v[0..9]) of one or two
    // channels' windows; each channel's k = 7 kept for the next sample's k = -1.
    template <int kChannels>
    void gridPoints(const float* const* windows, float (*v)[kLanes + 2]) noexcept {
        float* out[kChannels];
        for (int c = 0; c < kChannels; ++c) out[c] = v[c] + 2;
        interpolateRows<kChannels>(rows_, windows, out);
        for (int c = 0; c < kChannels; ++c) {
            v[c][0] = prev7_[c];
            v[c][1] = windows[c][kCentre];
            prev7_[c] = v[c][kLanes];
        }
    }

    template <bool kStereo, bool kTruePeak>
    void renderFrames(float* const* ch, int from, int to) noexcept {
        float* left = ch[0];
        [[maybe_unused]] float* right = kStereo ? ch[1] : nullptr;
        const PipelineShape shape = shape_;
        const int lookahead = lookahead_;
        constexpr float kMargin = kTruePeak ? limiter::kTruePeakMargin : 1.f;
        size_t pos = pos_;
        for (int i = from; i < to; ++i) {
            // In: scaled to the line, into the lookahead and the detector's window.
            const float pre = pre_.next();
            const float xl = left[i] * pre;
            delay_[0].push(xl);
            const float* wl = history_[0].push(xl);
            [[maybe_unused]] const float* wr = wl;
            if constexpr (kStereo) {
                const float xr = right[i] * pre;
                delay_[1].push(xr);
                wr = history_[1].push(xr);
            }
            postRing_.push(post_.next());
            softRing_.push(dsp::sCurve(softness_.next()));

            // The detector, for sample m = n - kDetectorDelay, with the knee it was given:
            // the most gain each pipeline may have; then the pipelines' gains.
            const float target = (1.f + (1.f - limiter::kSoftKnee) * softRing_.tap(kDetectorDelay)) * kMargin;
            const float autoShare = dsp::sCurve(autoShare_.next());
            float gainL;
            [[maybe_unused]] float gainR = 1.f, gainM = 1.f, gainS = 1.f;
            if constexpr (kStereo) {
                float pL, pR, pM, pS;
                if constexpr (kTruePeak) {
                    float v[2][kLanes + 2], vm[kLanes + 2], vs[kLanes + 2];
                    const float* windows[2] = {wl, wr};
                    gridPoints<2>(windows, v);
                    const float* vl = v[0];
                    const float* vr = v[1];
                    for (int k = 0; k < kLanes + 2; ++k) {
                        vm[k] = 0.5f * (vl[k] + vr[k]);
                        vs[k] = 0.5f * (vl[k] - vr[k]);
                    }
                    const float near = target / kRefinedAtMost;
                    pL = gridPeak(vl);
                    pR = gridPeak(vr);
                    pM = gridPeak(vm);
                    pS = gridPeak(vs);
                    if (pL > near) pL = truePeakOf(vl);
                    if (pR > near) pR = truePeakOf(vr);
                    if (pM + pS > near) {
                        pM = truePeakOf(vm);
                        pS = truePeakOf(vs);
                    }
                } else {
                    const float a = wl[kCentre], b = wr[kCentre];
                    pL = std::abs(a);
                    pR = std::abs(b);
                    pM = 0.5f * std::abs(a + b);
                    pS = 0.5f * std::abs(a - b);
                }
                // L/R: each channel's own gain blended by Link with the pair's (the lower).
                const float link = link_.next();
                const float rL = pL > target ? target / pL : 1.f;
                const float rR = pR > target ? target / pR : 1.f;
                const float rLR = std::min(rL, rR);
                const float gL = rL + link * (rLR - rL);
                const float gR = rR + link * (rLR - rR);
                // M/S: |left| and |right| are at most |mid| + |side|, so their sum must fit;
                // own gains share the room out. The linked gain turns both down alike, by what
                // left and right need: |mid| + |side| is the louder of |left| and |right| at every
                // moment, between samples too, so that is exactly enough (the sum of the mid's and
                // the side's true peaks, found at different moments, would be more than enough).
                float gM = 1.f, gS = 1.f;
                if (const float linked = std::max(pL, pR); pM + pS > target || linked > target) {
                    const float rMS = linked > target ? target / linked : 1.f;
                    float ownM, ownS;
                    limiter::sharedCeiling(pM, pS, target, ownM, ownS);
                    gM = ownM + link * (rMS - ownM);
                    gS = ownS + link * (rMS - ownS);
                }
                gainL = pipes_[0].next(1.f - gL, shape, autoShare, pos);
                gainR = pipes_[1].next(1.f - gR, shape, autoShare, pos);
                gainM = pipes_[2].next(1.f - gM, shape, autoShare, pos);
                gainS = pipes_[3].next(1.f - gS, shape, autoShare, pos);
            } else {
                float pL;
                if constexpr (kTruePeak) {
                    float v[1][kLanes + 2];
                    const float* windows[1] = {wl};
                    gridPoints<1>(windows, v);
                    pL = gridPeak(v[0]);
                    if (pL > target / kRefinedAtMost) pL = truePeakOf(v[0]);
                } else {
                    pL = std::abs(wl[kCentre]);
                }
                gainL = pipes_[0].next(pL > target ? 1.f - target / pL : 0.f, shape, autoShare, pos);
            }
            pos = (pos + 1) & shape.mask;

            // Out: sample n - L, with the values it came in with.
            const float dl = delay_[0].tap(lookahead);
            const float post = postRing_.tap(lookahead);
            const float softness = softRing_.tap(lookahead);
            if (softness != kneeSoftness_) {
                kneeSoftness_ = softness;
                knee_ = limiter::knee(softness);
            }
            const float dip = dip_.phase == Dip::Phase::None ? 1.f : dip_.next();
            float yl = gainL * dl;
            if constexpr (kStereo) {
                // Both routings' outputs obey the ceiling, so Routing's crossfade between them does.
                const float routing = dsp::sCurve(routing_.next());
                const float dr = delay_[1].tap(lookahead);
                float yr = gainR * dr;
                if (routing > 0.f) {
                    const float mid = 0.5f * (dl + dr), side = 0.5f * (dl - dr);
                    const float ml = gainM * mid + gainS * side, mr = gainM * mid - gainS * side;
                    yl = routing >= 1.f ? ml : yl + routing * (ml - yl);
                    yr = routing >= 1.f ? mr : yr + routing * (mr - yr);
                }
                const float outR = throughKnee(yr) * post * dip;
                right[i] = outR;
                inPeak_[1] = std::max(inPeak_[1], std::abs(dr));
                outPeak_[1] = std::max(outPeak_[1], std::abs(outR));
                minGain_[1] = std::min(minGain_[1], gainR);
                minGain_[2] = std::min(minGain_[2], gainM);
                minGain_[3] = std::min(minGain_[3], gainS);
            }
            const float outL = throughKnee(yl) * post * dip;
            left[i] = outL;
            inPeak_[0] = std::max(inPeak_[0], std::abs(dl));
            outPeak_[0] = std::max(outPeak_[0], std::abs(outL));
            minGain_[0] = std::min(minGain_[0], gainL);
            if (++meterCount_ == kMeterSamples) {
                meterCount_ = 0;
                publishMeters(kStereo);
            }
        }
        pos_ = pos;
    }

    // A limited sample (normalized) through Soft Clip's knee and clamped to the
    // line, noting what the knee took off.
    float throughKnee(float y) noexcept {
        const float a = std::abs(y);
        if (a <= knee_.start) return y;
        if (knee_.k > 0.f) {
            const float shaped = limiter::shape(y, knee_);
            clipRatio_ = std::max(clipRatio_, a / std::abs(shaped));
            return shaped;
        }
        return std::clamp(y, -1.f, 1.f);
    }

    static const std::vector<ParamInfo>& infos() {
        static const std::vector<ParamInfo> kInfos = [] {
            static const std::vector<std::string> kLookaheads = {"1.5 ms", "3 ms", "6 ms"};
            static const std::vector<std::string> kModes = {"Standard", "Soft Clip", "True Peak"};
            static const std::vector<std::string> kRoutings = {"L/R", "M/S"};
            std::vector<ParamInfo> list = {
                {"gain", "Gain", "dB", -24.f, 24.f, 0.f},
                {"ceiling", "Ceiling", "dB", -24.f, 0.f, -0.3f},
                {"release", "Release", "ms", 0.1f, 3000.f, 300.f, true},
                {"auto_release", "Auto Release", "", 0.f, 1.f, 1.f, false, offOnLabels()},
                {"lookahead", "Lookahead", "", 0.f, 2.f, 1.f, false, kLookaheads},
                {"mode", "Mode", "", 0.f, 2.f, 0.f, false, kModes},
                {"routing", "Routing", "", 0.f, 1.f, 0.f, false, kRoutings},
                {"link", "Link", "%", 0.f, 100.f, 100.f},
                {"maximize", "Maximize", "", 0.f, 1.f, 0.f, false, offOnLabels()},
                {"threshold", "Threshold", "dB", -24.f, 0.f, -0.3f},
                {"output", "Output", "dB", -24.f, 0.f, -0.3f},
            };
            list[Lookahead].automatable = false;  // (it is the device's latency)
            return list;
        }();
        return kInfos;
    }

    double sampleRate_ = 48000.0;
    int reportedLatency_ = 0;
    int channels_ = 2;

    // The lookahead now (it changes through the dip) and the pipelines' shape for it.
    int choice_ = 1;
    int lookahead_ = 144;
    PipelineShape shape_;
    Dip dip_;

    // Glides: pre and post in dB; Link linear; Soft Clip's amount, Routing (0 L/R, 1 M/S) and Auto's share
    // linear ramps, eased where they are used.
    GainGlide pre_, post_;
    SmoothedValue softness_, routing_, link_, autoShare_;
    float lineDb_ = 0.f;
    bool truePeak_ = false;

    std::array<dsp::DelayLine, 2> delay_;  // the normalized input, L samples long
    dsp::DelayLine postRing_, softRing_;   // post and Soft Clip's amount, as each sample came in
    std::array<History, 2> history_;       // the detector's window per channel
    float prev7_[2] = {};                  // the previous sample's last interpolated point, per channel
    float rows_[kTaps][kLanes] = {};       // the interpolator: tap t's coefficient for points k = 1..8
    std::array<Pipeline, 4> pipes_;        // L, R, M, S
    size_t pos_ = 0;                       // where the boxes write
    limiter::Knee knee_ = limiter::knee(0.f);
    float kneeSoftness_ = 0.f;

    // The displays' window.
    int meterCount_ = 0;
    float inPeak_[2] = {}, outPeak_[2] = {};
    std::array<float, 4> minGain_{1.f, 1.f, 1.f, 1.f};
    float clipRatio_ = 1.f;
};

}  // namespace

SUB_REGISTER_BUILTIN(LimiterProcessor, AudioEffect);

}  // namespace sub
