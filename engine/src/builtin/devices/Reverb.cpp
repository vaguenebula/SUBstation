// Built-in "Reverb" device, after Ableton Live's Reverb: an algorithmic reverb
// with an input filter, early reflections and a diffusion network. Mono in,
// stereo out, as Live's is: the input's two sides are summed before the
// reverb, and Stereo sets how wide the reverb comes out. The design (its
// constants, and the maths its editor shares) is in builtin/ReverbDesign.h.
//
//   mono = (L + R) / 2 -> input filter (a Butterworth high-pass and low-pass a
//   band apart, each switch cross-faded) -> the input line, read by
//     - 12 early-reflection taps after the predelay (Shape's envelope, signs,
//       pans; Spin drifts them in time and swings their pans), times Reflect;
//     - the network's input, Shape's onset later: Schroeder all-passes
//       (Diffusion their gain, Scale their length) into a feedback delay network
//       of 4, 8 or 16 lines (Density). Each line is read through a first-order
//       Thiran all-pass (lossless, so the decay per band is exactly the
//       design's), through two one-pole shelves set per line so each band rings
//       for its share of Decay (or a low-pass), times the loop's gain; High adds
//       an all-pass in each loop. A Walsh-Hadamard transform, rotated by one
//       line, mixes them back in; two orthogonal Hadamard rows of the lines'
//       reads are the left and right of the tail, times Diffuse.
//   -> Stereo (mid/side) -> Dry/Wet.
//
// - Smoothing: what moves a delay (Predelay, Shape's onset, Size, Scale) and the
//   levels applied to the audio (Reflect, Diffuse, Stereo, Dry/Wet, the input
//   filter's switches) glide a sample at a time, through two one-poles in a row,
//   so they have no corners. Everything else glides every 32 samples (a
//   sub-chunk) and what it sets (a coefficient, a gain, Chorus's and Spin's
//   drift) is ramped linearly across the sub-chunk. Each glide lands on its
//   target exactly once within a hair of it, so a settled control is exactly its
//   target and nothing is worked out again. Size glides at Smooth's pace (None:
//   2 ms, a fast pitch sweep but never a step).
// - When a moving delay's whole number of samples changes, its Thiran all-pass's
//   state is worked out again for the new split from the last few samples, so a
//   gliding or chorused line leaves no transient.
// - Freeze glides the tail's decay to practically none (with Cut the input no
//   longer reaches it; without, a minute's, so a held input settles at a level);
//   Flat glides the shelves flat while frozen. A guard eases the loops' gain
//   down while the tail's output peaks above +18 dBFS (a frozen uncut tail fed
//   loudly), and back once it doesn't: ordinary material never gets near it.
// - A change of Density fades each diffuser's input, each loop's, what is
//   written back into the lines and the tail's output to 0 over 12 ms (an
//   S-curve), switches the lines, and fades them back in: the lines that stay
//   keep their sound, lines, all-passes and diffusers that join start from
//   silence and hear nothing abrupt, and every seam falls where the fade is at 0.
// - Silence: once the input has been below -160 dB for longer than the input
//   line reaches and the tail has died below -120 dB (not frozen), it sleeps:
//   the wet is exactly 0 and the network is skipped until sound comes back
//   (the glides and the LFOs go on; what they set is snapped there on waking).
// - On one channel, that channel is the input and the wet's mid the output.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <string>
#include <vector>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/Dsp.h"
#include "builtin/DspBlocks.h"
#include "builtin/ReverbDesign.h"
#include "rt/RtUtils.h"

namespace sub {
namespace {

using reverb::kControl;
using reverb::kMaxDiffusers;
using reverb::kMaxLines;
using reverb::kMaxTaps;
using reverb::kMeterSamples;

constexpr double kPi = std::numbers::pi;
constexpr float kButterworthK = 1.41421356f;  // the input filter's damping (Q 0.707)
constexpr double kFadeSeconds = 0.012;        // Density: each way
constexpr double kGuardLevel = 8.0;           // +18 dBFS: the tail's output peak the guard holds it under
constexpr double kMaxPredelayMs = 250.0;
constexpr float kAwakeLevel = 1e-8f;          // -160 dB: an input this loud wakes it
constexpr float kSleepLevel = 1e-6f;          // -120 dB: a tail this quiet lets it sleep
constexpr float kMeterFloorDb = -90.f;
constexpr double kLandedSamples = 1e-5;       // a delay's glide lands within this of its target
constexpr int kSettleSamples = 4;             // a Thiran's state is worked out again over this many
constexpr float kTailGain = static_cast<float>(reverb::kDiffuseGain * reverb::kOutputScale);

// The sub-chunk glides' time constants (each of the two one-poles), and the
// per-sample ones'.
enum Tau { TauFast = 0, TauSlow, TauCount };
constexpr std::array<double, TauCount> kTaus = {0.02, 0.03};
constexpr double kDelaySeconds = 0.025;  // Predelay and the onset
constexpr double kScaleSeconds = 0.05;
constexpr double kLevelSeconds = 0.005;  // Reflect, Diffuse, Stereo, Dry/Wet, the input filter's switches
constexpr std::array<double, 3> kSizeSeconds = {0.002, 0.35, 0.05};  // Smooth: None, Slow, Fast

// 0 at 0, 1 at 1, flat at both ends: the Density fade's shape.
inline double sCurve(double t) noexcept { return t * t * (3.0 - 2.0 * t); }

// Two one-poles in a row gliding to a target, moved on a sub-chunk at a time: a
// jump eases in and out, and a glide can turn back halfway without a kink. It
// lands on the target exactly once within `landed` of it, so a settled control
// is exactly its target (nothing is recomputed, and none is exactly none).
struct Glide {
    double first = 0.0, value = 0.0;

    void snap(double target) noexcept { first = value = target; }
    bool settled(double target) const noexcept { return first == target && value == target; }
    // Moves on by `c` (the share of the way a one-pole goes in the sub-chunk); true if it moved.
    bool move(double target, double c, double landed) noexcept {
        if (settled(target)) return false;
        first += c * (target - first);
        value += c * (first - value);
        if (std::abs(target - first) < landed && std::abs(target - value) < landed) snap(target);
        return true;
    }
};

// The same moved a sample at a time (a delay, or a level on the audio: no
// corners at all). It lands at a sub-chunk's end, once within `landed`.
struct Smooth {
    double first = 0.0, value = 0.0, target = 0.0, c = 1.0, landed = 0.0;

    void snap(double to) noexcept { first = value = target = to; }
    bool settled() const noexcept { return first == target && value == target; }
    double next() noexcept {
        first += c * (target - first);
        value += c * (first - value);
        return value;
    }
    void land() noexcept {
        if (std::abs(target - first) < landed && std::abs(target - value) < landed) snap(target);
    }
};

// Something worked out from a parameter's value (a log, a gain), again only
// when the value changes: no transcendental every sub-chunk for nothing.
struct Cached {
    float raw = std::numeric_limits<float>::quiet_NaN();
    double value = 0.0;

    template <typename F>
    double of(float v, F&& f) noexcept {
        if (!(v == raw)) {
            raw = v;
            value = f(v);
        }
        return value;
    }
};

// A value ramped linearly across a sub-chunk: next() gives each sample's, the
// last sample's being exactly where it was sent (each sub-chunk starts again
// from that exact value, so rounding never builds up).
struct Ramp {
    float value = 0.f, step = 0.f;
    double end = 0.0;

    void to(double target, int n) noexcept {
        value = static_cast<float>(end);
        step = static_cast<float>((target - end) / n);
        end = target;
    }
    void snap(double target) noexcept {
        value = static_cast<float>(target);
        step = 0.f;
        end = target;
    }
    void set(double target, int n, bool snapping) noexcept { snapping ? snap(target) : to(target, n); }
    bool still() const noexcept { return step == 0.f && end == 0.0; }
    float next() noexcept { return value += step; }
};

// Delay lines of one power-of-two length side by side, written in step (one
// write position for all). Each starts a cache line further than a whole number
// of lines' lengths, so their writes (all at one position) don't share a cache set.
class LineBank {
public:
    void prepare(int lines, double maxDelaySamples) {
        size_t size = 4;
        while (size < static_cast<size_t>(std::ceil(maxDelaySamples)) + kSettleSamples + 12) size <<= 1;
        stride_ = size + 16;
        mask_ = size - 1;
        buffer_.assign(stride_ * static_cast<size_t>(lines), 0.f);
        write_ = 0;
    }
    void clear() noexcept {
        std::fill(buffer_.begin(), buffer_.end(), 0.f);
        write_ = 0;
    }
    void clear(int line) noexcept { std::fill_n(this->line(line), mask_ + 1, 0.f); }
    float* line(int q) noexcept { return buffer_.data() + static_cast<size_t>(q) * stride_; }
    size_t stride() const noexcept { return stride_; }
    size_t mask() const noexcept { return mask_; }
    size_t write() const noexcept { return write_; }
    void advance() noexcept { write_ = (write_ + 1) & mask_; }
    bool empty() const noexcept { return buffer_.empty(); }

private:
    std::vector<float> buffer_;
    size_t stride_ = 0, mask_ = 0, write_ = 0;
};

// First-order Thiran (all-pass) interpolation of a delay D (at least 2 samples)
// read before the coming write: M whole samples, then an all-pass of
// d = D - M (0.5..1.5), a = (1 - d) / (1 + d). Lossless (|H| = 1), so a loop's
// decay per band is its design's whatever the fraction; linear or Hermite reads
// would dull the highs a little on every pass.
//
// A moving delay: the all-pass's state (its last output) was worked out for
// the delay a sample ago, so as the delay moves by `step` a sample, the state is
// moved with it (by step times the input's slope there), leaving an error of the
// step's square rather than of the step: a sweep (Size gliding, Chorus) reads
// about 20 times cleaner. And when the whole part changes, the state belongs to
// the old split: it is worked out again for the new one, running the new
// all-pass over the last few samples from an estimate (whose error falls by
// |a| <= 1/3 a sample), so the change leaves no transient.
struct Thiran {
    int whole = 1;
    float a = 0.f;
    float last = 0.f;  // the delay a sample ago
    float step = 0.f;  // how far it moved since (0: the state is for this delay)

    // To a delay (at least 2 samples) that moves: each sample, or each sub-chunk where it holds still.
    void follow(float delay, const float* line, size_t w, size_t mask, float& state) noexcept {
        const int m = static_cast<int>(delay - 0.5f);  // (a floor)
        const float d = delay - static_cast<float>(m);
        a = (1.f - d) / (1.f + d);
        step = delay - last;
        last = delay;
        if (m == whole) return;
        whole = m;
        step = 0.f;
        const auto x = [&](size_t back) { return line[(w - back) & mask]; };  // the input `back` samples ago
        const auto mm = static_cast<size_t>(m);
        // Its output kSettleSamples + 1 samples ago, read between samples; then on to the last.
        size_t back = kSettleSamples + 1 + mm;
        float t = d;
        if (t >= 1.f) {
            ++back;
            t -= 1.f;
        }
        float y = x(back) + t * (x(back + 1) - x(back));
        for (size_t k = kSettleSamples; k >= 1; --k) y = a * (x(k + mm) - y) + x(k + mm + 1);
        state = y;
    }
    // y = a x[n - M] + x[n - M - 1] - a y[n - 1], from `line` before the write at `w`.
    float read(const float* line, size_t w, size_t mask, float& state) const noexcept {
        const auto m = static_cast<size_t>(whole);
        state = a * (line[(w - m) & mask] - state) + line[(w - m - 1) & mask];
        return state;
    }
    // The same while the delay moves: y[n - 1] moved to this sample's delay first.
    float readMoving(const float* line, size_t w, size_t mask, float& state) const noexcept {
        const auto m = static_cast<size_t>(whole);
        const float x0 = line[(w - m) & mask], x1 = line[(w - m - 1) & mask];
        state = a * (x0 - (state + step * (x1 - x0))) + x1;
        return state;
    }
};

class ReverbProcessor final : public BuiltinProcessor {
public:
    enum Param {
        Predelay = 0, LoCut, HiCut, InFreq, InWidth, SpinOn, SpinRate, SpinAmount, Shape, DensityParam, Smoothing,
        Size, Stereo, LoShelf, LoFreq, LoGain, HiFilter, HiType, HiFreq, HiGain, Decay, Freeze, Flat, Cut, Diffusion,
        Scale, ChorusOn, ChorusRate, ChorusAmount, Reflect, Diffuse, Mix, NumParams
    };
    enum Display { InputLevel = 0, EarlyLevel, DiffuseLevel, SpinPhase, ChorusPhase, Signal, Tail };

    ReverbProcessor()
        : BuiltinProcessor(infos(), {{"input", kMeterSamples},
                                     {"early", kMeterSamples},
                                     {"diffuse", kMeterSamples},
                                     {"spin", kMeterSamples},
                                     {"chorus", kMeterSamples},
                                     {"signal", 1},
                                     {"tail", 1}}) {}

    std::string typeId() const override { return "builtin:reverb"; }
    std::string name() const override { return "Reverb"; }

    // Until the tail is 60 dB down: the predelay, onset and diffusers, a pass
    // of the longest loop, and 1.15 times Decay; frozen (or at most) a minute.
    // On the main thread: from the parameters only.
    int tailSamples() const override {
        const double fs = sampleRate_;
        if (isOn(Freeze)) return static_cast<int>(60.0 * fs);
        const double s = reverb::sizeFactor(param(Size)), c = reverb::scaleFactor(param(Scale));
        const reverb::Density d = reverb::densityAt(choiceIndex(DensityParam));
        double samples = (param(Predelay) + reverb::onsetMs(s, param(Shape)) + reverb::kChorusDepthMs) * fs / 1000.0;
        for (int j = 0; j < reverb::layout(d).diffusers; ++j) samples += reverb::diffuserSamples(j, s, c, fs);
        double longest = 0.0;
        for (const int q : reverb::activeLines(d)) longest = std::max(longest, reverb::loopSamples(q, d, s, c, fs));
        samples += longest + (1.15 * param(Decay) / 1000.0 + 0.1) * fs;
        return static_cast<int>(std::min(samples, 60.0 * fs));
    }

    void prepare(double sampleRate, int) override {
        sampleRate_ = sampleRate;
        const double fs = sampleRate;
        const double s = reverb::sizeFactor(reverb::kMaxSize), c = reverb::scaleFactor(100.0);
        // The input line: the predelay, then the last tap at the largest Size with Spin's drift.
        const double inputMs = kMaxPredelayMs + (reverb::kTapMs[kMaxTaps - 1] + reverb::kSpinDepthMs) * s;
        input_.prepare(static_cast<int>(std::ceil(inputMs * fs / 1000.0)) + 8);
        lines_.prepare(kMaxLines, (reverb::kLineMs[kMaxLines - 1] * s + reverb::kChorusDepthMs) * fs / 1000.0 + 2.0);
        allpasses_.prepare(kMaxLines, reverb::loopAllpassSamples(kMaxLines - 1, s, c, fs) + 2.0);
        double longestDiffuser = 0.0;
        for (int j = 0; j < kMaxDiffusers; ++j)
            longestDiffuser = std::max(longestDiffuser, reverb::diffuserSamples(j, s, c, fs));
        diffusers_.prepare(kMaxDiffusers, longestDiffuser + 2.0);
        for (int q = 0; q < kMaxLines; ++q) {
            LineState& line = line_[static_cast<size_t>(q)];
            line.length = reverb::kLineMs[static_cast<size_t>(q)] * fs / 1000.0;
            line.allpassLength = reverb::kLoopAllpassShare * line.length;
        }
        for (int j = 0; j < kMaxDiffusers; ++j)
            diffuser_[static_cast<size_t>(j)].length = reverb::kDiffuserMs[static_cast<size_t>(j)] * fs / 1000.0;
        for (int k = 0; k < kMaxTaps; ++k) {
            TapState& tap = tap_[static_cast<size_t>(k)];
            tap.time = reverb::kTapMs[static_cast<size_t>(k)] * fs / 1000.0;
            tap.angle = reverb::tapAngle(k);
            tap.swing = reverb::tapSwing(k);
            tap.restLeft = std::cos(tap.angle);
            tap.restRight = std::sin(tap.angle);
        }
        for (int t = 0; t < TauCount; ++t)
            glide32_[static_cast<size_t>(t)] = 1.0 - onePoleCoefficient(kTaus[static_cast<size_t>(t)], fs, kControl);
        fadeLength_ = std::max(1, static_cast<int>(std::lround(kFadeSeconds * fs)));

        // The per-sample glides: how fast, and how near their target they land.
        const auto perSample = [&](Smooth& g, double seconds, double landed) {
            g.c = 1.0 - onePoleCoefficient(seconds, fs);
            g.landed = landed;
        };
        perSample(predelay_, kDelaySeconds, kLandedSamples);
        perSample(onset_, kDelaySeconds, kLandedSamples);
        perSample(size_, kSizeSeconds[1], kLandedSamples / line_[kMaxLines - 1].length);
        perSample(scale_, kScaleSeconds, kLandedSamples / (diffuser_[2].length * s));
        for (Smooth* level : {&reflect_, &diffuse_, &stereo_, &mix_, &loCutMix_, &hiCutMix_})
            perSample(*level, kLevelSeconds, 1e-7);
        reset();
    }

    // Silent, every glide and fade where the parameters are, the LFOs at their
    // start: renders after a reset are the same every time.
    void reset() override {
        if (lines_.empty()) return;
        input_.reset();
        lines_.clear();
        allpasses_.clear();
        diffusers_.clear();
        for (int q = 0; q < kMaxLines; ++q) {
            LineState& line = line_[static_cast<size_t>(q)];
            line.mod = line.gain = line.kHi = line.kLo = Ramp{};
            line.read = line.allpassRead = Thiran{};
            line.y = line.allpassY = 0.f;
            line.hi = line.lo = reverb::OnePoleTpt{};
            line.chorusPhase = static_cast<double>(q) / kMaxLines;
            line.chorusRatio = 1.0 + 0.23 * (static_cast<double>((7 * q) % kMaxLines) / (kMaxLines - 1) - 0.5);
        }
        for (DiffuserState& diffuser : diffuser_) {
            diffuser.gain = Ramp{};
            diffuser.read = Thiran{};
            diffuser.y = 0.f;
        }
        for (TapState& tap : tap_) {
            tap.gain = Glide{};
            tap.drift = tap.left = tap.right = Ramp{};
        }
        hp_.reset();
        lp_.reset();
        spinPhase_ = spinStart_ = 0.0;
        gamma_ = 1.0;
        gammaMoved_ = false;
        fade_ = Fade{};
        fading_ = FadePhase::Idle;
        fadeFrom_ = 0;
        loopsRamping_ = false;
        layoutDensity_ = std::clamp(choiceIndex(DensityParam), 0, 3);
        layout_ = reverb::layout(reverb::densityAt(layoutDensity_));
        layoutSwitched_ = false;
        snapGlides();
        snapping_ = true;
        setRamps(kControl, true);
        snapping_ = false;
        sleeping_ = false;
        silent_ = 0;
        meterCount_ = 0;
        meterInput_ = meterEarly_ = 0.f;
        meterDiffuse_ = 0.0;
        netPeak_ = earlyPeak_ = 0.f;
    }

protected:
    void render(const ProcessContext&, float* const* ch, int numChannels, int numFrames) override {
        const int channels = std::min(numChannels, 2);
        if (channels <= 0 || lines_.empty()) return;
        for (int at = 0; at < numFrames;) {
            const int n = std::min(kControl, numFrames - at);
            // How long the input has been silent; any sound wakes it before it is processed.
            int lastLoud = -1;
            for (int i = 0; i < n; ++i) {
                const float x = channels == 2 ? 0.5f * (ch[0][at + i] + ch[1][at + i]) : ch[0][at + i];
                if (std::abs(x) > kAwakeLevel) lastLoud = i;
            }
            const bool waking = sleeping_ && lastLoud >= 0;
            if (lastLoud >= 0) {
                silent_ = n - 1 - lastLoud;
                sleeping_ = false;
            } else {
                silent_ = std::min<int64_t>(silent_ + n, int64_t{1} << 40);
            }
            const bool loopsMoved = moveGlides(n);
            if (!sleeping_) {
                snapping_ = waking;  // (nothing was ramped while it slept)
                setRamps(n, loopsMoved);
                snapping_ = false;
            }
            if (sleeping_) {
                renderAsleep(ch, channels, at, n);
            } else {
                switch (layout_.lines) {
                case 4: renderAwake<4, false>(ch, channels, at, n); break;
                case 8: renderAwake<8, false>(ch, channels, at, n); break;
                default:
                    if (layout_.loopAllpass) {
                        renderAwake<16, true>(ch, channels, at, n);
                    } else {
                        renderAwake<16, false>(ch, channels, at, n);
                    }
                    break;
                }
                afterChunk();
            }
            for (Smooth* g : {&predelay_, &onset_, &size_, &scale_, &reflect_, &diffuse_, &stereo_, &mix_, &loCutMix_,
                              &hiCutMix_})
                g->land();
            at += n;
        }
    }

private:
    // A line of the network: its read, its loop's filters and gain, High's all-pass, its chorus.
    struct LineState {
        double length = 0.0;          // its delay at size factor 1 (samples)
        double allpassLength = 0.0;   // High: its loop all-pass's at size and scale factors 1
        Ramp mod;                     // Chorus's drift (samples)
        Thiran read;                  // the read's split
        float y = 0.f;                // the Thiran's state
        reverb::OnePoleTpt hi, lo;    // the shelves' one-poles
        Ramp gain, kHi, kLo;          // the loop's gain (times the guard and 1 / sqrt(N)); the shelves' far ends
        Thiran allpassRead;           // High: the loop's all-pass
        float allpassY = 0.f;
        double chorusPhase = 0.0, chorusRatio = 1.0;
    };
    struct DiffuserState {
        double length = 0.0;  // its delay at size and scale factors 1 (samples)
        Ramp gain;
        Thiran read;
        float y = 0.f;
    };
    struct TapState {
        double time = 0.0;           // its time after the predelay at size factor 1 (samples)
        double angle = 0.0, swing = 0.0, restLeft = 0.0, restRight = 0.0;  // its pan (reverb::tapAngle(), tapSwing())
        Glide gain;                  // its gain, gliding to Shape's and Density's
        double target = 0.0;         // which
        Ramp drift, left, right;     // Spin's drift of its time; its gain into each side
    };
    enum class FadePhase { Idle, Out, In };
    struct Fade {
        FadePhase phase = FadePhase::Idle;
        int at = 0;       // samples into the phase
        int pending = 3;  // the Density it switches to
    };

    double coefficient(Tau tau, int n) const noexcept {
        return n == kControl ? glide32_[tau] : 1.0 - onePoleCoefficient(kTaus[tau], sampleRate_, n);
    }

    // --- The glides' targets (from the parameters) -----------------------------------------

    double targetSize() const noexcept { return reverb::sizeFactor(param(Size)); }
    double targetScale() const noexcept { return reverb::scaleFactor(param(Scale)); }
    double targetPredelay() const noexcept {
        return std::clamp<double>(param(Predelay), 0.5, kMaxPredelayMs) * sampleRate_ / 1000.0;
    }
    double targetOnset(double s) const noexcept { return reverb::onsetMs(s, param(Shape)) * sampleRate_ / 1000.0; }
    static double onOff(bool on) noexcept { return on ? 1.0 : 0.0; }
    double logOf(Cached& cache, int index, float least) const noexcept {
        return cache.of(std::max(least, param(index)), [](float v) { return std::log(static_cast<double>(v)); });
    }
    double logInFreq() noexcept { return logOf(logInFreq_, InFreq, 1.f); }
    double logDecay() noexcept { return logOf(logDecay_, Decay, 1.f); }
    double logLoFreq() noexcept { return logOf(logLoFreq_, LoFreq, 1.f); }
    double logHiFreq() noexcept { return logOf(logHiFreq_, HiFreq, 1.f); }
    double logSpinRate() noexcept { return logOf(logSpinRate_, SpinRate, 0.01f); }
    double logChorusRate() noexcept { return logOf(logChorusRate_, ChorusRate, 0.001f); }
    double gainOf(Cached& cache, int index) const noexcept {
        return cache.of(param(index), [](float db) { return static_cast<double>(dbToGain(db)); });
    }
    double targetSpin() const noexcept {
        return isOn(SpinOn) ? std::clamp(param(SpinAmount), 0.f, 100.f) / 100.0 : 0.0;
    }
    double targetChorus() const noexcept {
        return isOn(ChorusOn) ? std::clamp(param(ChorusAmount), 0.f, 100.f) / 100.0 : 0.0;
    }
    // Sets the per-sample glides' targets.
    void aimSmooths() noexcept {
        predelay_.target = targetPredelay();
        size_.target = targetSize();
        size_.c = sizeCoefficient_.of(static_cast<float>(std::clamp(choiceIndex(Smoothing), 0, 2)), [&](float pace) {
            return 1.0 - onePoleCoefficient(kSizeSeconds[static_cast<size_t>(pace)], sampleRate_);
        });
        onset_.target = targetOnset(size_.value);
        scale_.target = targetScale();
        reflect_.target = gainOf(reflectGain_, Reflect);
        diffuse_.target = gainOf(diffuseGain_, Diffuse);
        stereo_.target = param(Stereo) / 100.0;
        mix_.target = param(Mix) / 100.0;
        loCutMix_.target = onOff(isOn(LoCut));
        hiCutMix_.target = onOff(isOn(HiCut));
    }

    void snapGlides() noexcept {
        aimSmooths();
        for (Smooth* g : {&predelay_, &size_, &scale_, &reflect_, &diffuse_, &stereo_, &mix_, &loCutMix_, &hiCutMix_})
            g->snap(g->target);
        onset_.snap(targetOnset(size_.value));
        inFreq_.snap(logInFreq());
        inWidth_.snap(param(InWidth));
        decay_.snap(logDecay());
        loFreq_.snap(logLoFreq());
        hiFreq_.snap(logHiFreq());
        loShare_.snap(param(LoGain) / 100.0);
        hiShare_.snap(param(HiGain) / 100.0);
        loOn_.snap(onOff(isOn(LoShelf)));
        hiOn_.snap(onOff(isOn(HiFilter)));
        lowpass_.snap(onOff(choiceIndex(HiType) == 1));
        freeze_.snap(onOff(isOn(Freeze)));
        flat_.snap(onOff(isOn(Flat)));
        cut_.snap(onOff(isOn(Cut)));
        diffusion_.snap(param(Diffusion) / 100.0);
        spinAmount_.snap(targetSpin());
        chorusAmount_.snap(targetChorus());
        spinRateLog_.snap(logSpinRate());
        chorusRateLog_.snap(logChorusRate());
        tapShape_ = -1.f;
        updateTapTargets();
        for (TapState& tap : tap_) tap.gain.snap(tap.target);
    }

    void updateTapTargets() noexcept {
        const float shape = param(Shape);
        const int density = std::clamp(choiceIndex(DensityParam), 0, 3);
        if (shape == tapShape_ && density == tapDensity_) return;
        tapShape_ = shape;
        tapDensity_ = density;
        std::array<reverb::Tap, kMaxTaps> taps;
        reverb::earlyTaps(1.0, shape, reverb::densityAt(density), taps);
        for (size_t k = 0; k < kMaxTaps; ++k) tap_[k].target = taps[k].gain;
    }

    // Moves every sub-chunk glide on by `n` samples (and aims the per-sample
    // ones), the Density fade and the LFOs too; true if anything the loops are
    // worked out from moved.
    bool moveGlides(int n) noexcept {
        const double fs = sampleRate_;
        const double fast = coefficient(TauFast, n), slow = coefficient(TauSlow, n);

        // Density: a change fades the network out, switches it, and fades it back in.
        const int density = std::clamp(choiceIndex(DensityParam), 0, 3);
        bool loops = false;
        if (fade_.phase == FadePhase::Out && fade_.at >= fadeLength_) {
            switchLayout(fade_.pending);
            fade_.phase = FadePhase::In;
            fade_.at = 0;
        }
        if (fade_.phase == FadePhase::Idle && density != layoutDensity_) {
            if (sleeping_) {  // nothing sounds: at once
                switchLayout(density);
            } else {
                fade_.phase = FadePhase::Out;
                fade_.at = 0;
            }
        }
        if (fade_.phase == FadePhase::Out) fade_.pending = density;
        fading_ = fade_.phase;  // (this sub-chunk's, from where it is)
        fadeFrom_ = fade_.at;
        if (fade_.phase != FadePhase::Idle) {
            fade_.at += n;
            if (fade_.phase == FadePhase::In && fade_.at >= fadeLength_) fade_.phase = FadePhase::Idle;
        }

        aimSmooths();
        delaysMoving_ = !size_.settled() || !scale_.settled();
        filterMoved_ = inFreq_.move(logInFreq(), fast, 1e-6) | inWidth_.move(param(InWidth), fast, 1e-6);
        loops |= layoutSwitched_ | !size_.settled() | (layout_.loopAllpass && !scale_.settled()) | gammaMoved_;
        loops |= decay_.move(logDecay(), slow, 1e-6);
        loops |= loFreq_.move(logLoFreq(), fast, 1e-6);
        loops |= hiFreq_.move(logHiFreq(), fast, 1e-6);
        loops |= loShare_.move(param(LoGain) / 100.0, fast, 1e-6);
        loops |= hiShare_.move(param(HiGain) / 100.0, fast, 1e-6);
        loops |= loOn_.move(onOff(isOn(LoShelf)), fast, 1e-6);
        loops |= hiOn_.move(onOff(isOn(HiFilter)), fast, 1e-6);
        loops |= lowpass_.move(onOff(choiceIndex(HiType) == 1), fast, 1e-6);
        loops |= freeze_.move(onOff(isOn(Freeze)), slow, 1e-6);
        loops |= flat_.move(onOff(isOn(Flat)), fast, 1e-6);
        loops |= cut_.move(onOff(isOn(Cut)), fast, 1e-6);
        gammaMoved_ = false;
        diffusion_.move(param(Diffusion) / 100.0, slow, 1e-6);
        spinAmount_.move(targetSpin(), fast, 1e-5);
        chorusAmount_.move(targetChorus(), fast, 1e-5);
        updateTapTargets();
        for (TapState& tap : tap_) tap.gain.move(tap.target, fast, 1e-7);

        // The LFOs: their phases at the sub-chunk's start (for the displays) and end.
        // Their rates glide (in log) too: a jump of rate would step the drift's speed.
        spinRateLog_.move(logSpinRate(), slow, 1e-6);
        chorusRateLog_.move(logChorusRate(), slow, 1e-6);
        spinStart_ = spinPhase_;
        spinRate_ = std::exp(spinRateLog_.value);
        spinPhase_ += spinRate_ * n / fs;
        spinPhase_ -= std::floor(spinPhase_);
        chorusRate_ = std::exp(chorusRateLog_.value);
        chorusStart_ = line_[static_cast<size_t>(layout_.firstLine)].chorusPhase;
        for (LineState& line : line_) {
            line.chorusPhase += chorusRate_ * line.chorusRatio * n / fs;
            line.chorusPhase -= std::floor(line.chorusPhase);
        }
        return loops;
    }

    // Works out what the sub-chunk glides set at the sub-chunk's end and ramps
    // each value there across it (or snaps it there: on reset, and what a
    // Density switch brings in).
    void setRamps(int n, bool loopsMoved) noexcept {
        const double fs = sampleRate_;
        const bool snap = snapping_ || layoutSwitched_;
        const double s = size_.value, c = scale_.value;
        const reverb::Density density = reverb::densityAt(layoutDensity_);
        const reverb::Layout& layout = layout_;
        const double invSqrtN = 1.0 / std::sqrt(static_cast<double>(layout.lines));
        const double z = freeze_.value;

        // The loops: their gains and the shelves', while anything they come from moves.
        if (loopsMoved || loopsRamping_ || snap) {
            const reverb::Rates rates = reverb::rates(std::exp(decay_.value) / 1000.0, loShare_.value, hiShare_.value,
                                                      z, flat_.value, cut_.value);
            for (int k = 0; k < layout.lines; ++k) {
                const int q = layout.firstLine + layout.lineStride * k;
                LineState& line = line_[static_cast<size_t>(q)];
                if (loopsMoved || snap) {
                    const double loopSamples = reverb::loopSamples(q, density, s, c, fs);
                    const reverb::Loop l = reverb::loop(rates, loopSamples, fs, loOn_.value, hiOn_.value,
                                                        lowpass_.value, z, flat_.value);
                    line.gain.set(l.g * gamma_ * invSqrtN, n, snap);
                    line.kHi.set(l.kHi, n, snap);
                    line.kLo.set(l.kLo, n, snap);
                } else {
                    line.gain.to(line.gain.end, n);
                    line.kHi.to(line.kHi.end, n);
                    line.kLo.to(line.kLo.end, n);
                }
            }
            if (loopsMoved || snap) {
                gHi_.set(reverb::tptG(std::exp(hiFreq_.value), fs), n, snapping_);
                gLo_.set(reverb::tptG(std::exp(loFreq_.value), fs), n, snapping_);
            } else {
                gHi_.to(gHi_.end, n);
                gLo_.to(gLo_.end, n);
            }
            loopsRamping_ = loopsMoved;
        }

        // Chorus's drift of each line (a quarter of the line at most); the
        // lines' reads where nothing moves them.
        const double chorus = chorusAmount_.value * reverb::kChorusDepthMs * fs / 1000.0;
        linesMoving_ = delaysMoving_;
        for (int k = 0; k < layout.lines; ++k) {
            LineState& line = line_[static_cast<size_t>(layout.firstLine + layout.lineStride * k)];
            double drift = 0.0;
            if (chorus > 0.0)
                drift = std::min(chorus, 0.25 * line.length * s) * std::sin(2.0 * kPi * line.chorusPhase);
            line.mod.set(drift, n, snap);
            linesMoving_ = linesMoving_ || !line.mod.still();
        }
        if (!linesMoving_) {
            for (int k = 0; k < layout.lines; ++k) {
                const int q = layout.firstLine + layout.lineStride * k;
                LineState& line = line_[static_cast<size_t>(q)];
                line.read.follow(lineDelay(line, s), lines_.line(q), lines_.write(), lines_.mask(), line.y);
            }
        }
        if (!delaysMoving_ || snap) {
            for (int q = 0; q < kMaxLines; ++q) {
                LineState& line = line_[static_cast<size_t>(q)];
                line.allpassRead.follow(allpassDelay(line, s * c), allpasses_.line(q), allpasses_.write(),
                                        allpasses_.mask(), line.allpassY);
            }
            for (int j = 0; j < kMaxDiffusers; ++j) {
                DiffuserState& diffuser = diffuser_[static_cast<size_t>(j)];
                diffuser.read.follow(diffuserDelay(diffuser, s * c), diffusers_.line(j), diffusers_.write(),
                                     diffusers_.mask(), diffuser.y);
            }
        }

        // The input diffusers' and High's loop all-passes' gains (reverb::diffusionGain()).
        const double d = reverb::diffusionGain(diffusion_.value);
        for (int j = 0; j < layout.diffusers; ++j) {
            DiffuserState& diffuser = diffuser_[static_cast<size_t>(j)];
            diffuser.gain.set(reverb::kDiffuserGain[static_cast<size_t>(j)] * d, n, snap);
        }
        allpassGain_.set(reverb::kLoopAllpassGain * d, n, snap);

        // How much of the input goes into the network (none frozen with Cut).
        inject_.set((1.0 - z * cut_.value) * invSqrtN, n, snap);
        layoutSwitched_ = false;

        // The input filter: its corners' g, per sample while they glide.
        if (filterMoved_ || filterMoving_ || snapping_) {
            const double freq = std::exp(inFreq_.value), width = inWidth_.value;
            hpG_.set(std::tan(kPi * reverb::loCutHz(freq, width, fs) / fs), n, snapping_);
            lpG_.set(std::tan(kPi * reverb::hiCutHz(freq, width, fs) / fs), n, snapping_);
            filterMoving_ = hpG_.step != 0.f || lpG_.step != 0.f;
            if (!filterMoving_) {
                hpCoefficients_ = dsp::SvfCoefficients(hpG_.value, kButterworthK);
                lpCoefficients_ = dsp::SvfCoefficients(lpG_.value, kButterworthK);
            }
        }

        // The early reflections: each tap's gains into each side and Spin's
        // drift of its time (each tap at its own phase) and swing of its pan
        // (in the angle of an equal-power pan: reverb::spinPan()).
        const double spin = spinAmount_.value;
        const double depth = spin * reverb::kSpinDepthMs * std::min(1.0, s) * fs / 1000.0;
        const double spinAngle = 2.0 * kPi * spinPhase_;
        const double spinSin = std::sin(spinAngle), spinCos = std::cos(spinAngle);
        taps_ = 0;
        for (int k = 0; k < kMaxTaps; ++k) {
            TapState& tap = tap_[static_cast<size_t>(k)];
            const double gain = tap.gain.value;
            double left = tap.restLeft, right = tap.restRight, drift = 0.0;
            if (spin > 0.0) {
                // sin and cos of 2 pi (phase + k / 12), turned on from the phase's.
                const double cosStep = kTapCos[static_cast<size_t>(k)], sinStep = kTapSin[static_cast<size_t>(k)];
                const double sinK = spinSin * cosStep + spinCos * sinStep;
                const double cosK = spinCos * cosStep - spinSin * sinStep;
                drift = depth * sinK;
                const double angle = tap.angle + spin * tap.swing * cosK;
                left = std::cos(angle);
                right = std::sin(angle);
            }
            tap.drift.set(drift, n, snapping_);
            const bool silent = gain == 0.0 && tap.left.end == 0.0 && tap.right.end == 0.0;
            tap.left.set(gain * left, n, snapping_);
            tap.right.set(gain * right, n, snapping_);
            if (!silent) tapList_[static_cast<size_t>(taps_++)] = k;
        }

        spinShown_ = isOn(SpinOn) || spinAmount_.value != 0.0;
        chorusShown_ = isOn(ChorusOn) || chorusAmount_.value != 0.0;
    }

    // The delays (samples) at size factor `s` (and scale factor `c`), as the
    // per-sample reads work them out.
    static float lineDelay(const LineState& line, double s) noexcept { return static_cast<float>(line.length * s); }
    static float allpassDelay(const LineState& line, double sc) noexcept {
        return std::max(2.f, static_cast<float>(line.allpassLength * sc));
    }
    static float diffuserDelay(const DiffuserState& diffuser, double sc) noexcept {
        return std::max(2.f, static_cast<float>(diffuser.length * sc));
    }

    // Switches the network to a Density's lines (while its fade is at 0): lines,
    // all-passes and diffusers that join start from silence; the rest keep what
    // they hold.
    void switchLayout(int density) noexcept {
        const reverb::Layout before = layout_;
        const reverb::Layout after = reverb::layout(reverb::densityAt(density));
        const auto active = [](const reverb::Layout& l, int q) {
            return q >= l.firstLine && (q - l.firstLine) % l.lineStride == 0;
        };
        for (int k = 0; k < after.lines; ++k) {
            const int q = after.firstLine + after.lineStride * k;
            LineState& line = line_[static_cast<size_t>(q)];
            if (!active(before, q)) {
                lines_.clear(q);
                line.y = 0.f;
                line.hi.s = line.lo.s = 0.f;
            }
            if (after.loopAllpass && (!before.loopAllpass || !active(before, q))) {
                allpasses_.clear(q);
                line.allpassY = 0.f;
            }
        }
        for (int j = before.diffusers; j < after.diffusers; ++j) {
            diffusers_.clear(j);
            diffuser_[static_cast<size_t>(j)].y = 0.f;
        }
        layout_ = after;
        layoutDensity_ = density;
        layoutSwitched_ = true;
    }

    template <int N, bool LoopAllpass>
    void renderAwake(float* const* ch, int channels, int at, int n) noexcept {
        constexpr int kFirst = N == 4 ? 2 : (N == 8 ? 1 : 0);
        constexpr int kStride = kMaxLines / N;
        constexpr int kDiffusers = N == 4 ? 2 : (N == 8 ? 3 : 4);
        const size_t lineMask = lines_.mask(), apMask = allpasses_.mask(), diffMask = diffusers_.mask();
        float* const lineBase = lines_.line(0);
        float* const apBase = allpasses_.line(0);
        float* const diffBase = diffusers_.line(0);
        const size_t lineStride = lines_.stride(), apStride = allpasses_.stride(), diffStride = diffusers_.stride();
        const bool linesMoving = linesMoving_, delaysMoving = delaysMoving_;
        const bool loActive = !loCutMix_.settled() || loCutMix_.target > 0.0;
        const bool hiActive = !hiCutMix_.settled() || hiCutMix_.target > 0.0;
        const bool filterMoving = filterMoving_;
        const FadePhase fading = fading_;
        const double fadeStep = 1.0 / fadeLength_;
        if (!loActive) hp_.reset();
        if (!hiActive) lp_.reset();

        // The sub-chunk works on copies of the states, written back at its end:
        // the compiler then knows the buffers' writes can't touch them and keeps
        // them in registers.
        std::array<LineState, N> lines;
        for (int k = 0; k < N; ++k) lines[static_cast<size_t>(k)] = line_[static_cast<size_t>(kFirst + kStride * k)];
        std::array<DiffuserState, kDiffusers> diffusers;
        for (int j = 0; j < kDiffusers; ++j) diffusers[static_cast<size_t>(j)] = diffuser_[static_cast<size_t>(j)];
        const int tapCount = taps_;
        std::array<TapState, kMaxTaps> taps;
        for (int t = 0; t < tapCount; ++t)
            taps[static_cast<size_t>(t)] = tap_[static_cast<size_t>(tapList_[static_cast<size_t>(t)])];
        Smooth size = size_, scale = scale_, predelay = predelay_, onset = onset_;
        Smooth reflect = reflect_, diffuse = diffuse_, stereo = stereo_, mix = mix_;
        Smooth loCut = loCutMix_, hiCut = hiCutMix_;
        const bool delaysGliding = !size.settled() || !scale.settled() || !predelay.settled() || !onset.settled();
        const bool levelsGliding = !reflect.settled() || !diffuse.settled() || !stereo.settled() || !mix.settled() ||
                                   !loCut.settled() || !hiCut.settled();
        Ramp gHiRamp = gHi_, gLoRamp = gLo_, apGain = allpassGain_, inject = inject_, hpG = hpG_, lpG = lpG_;
        dsp::Svf hp = hp_, lp = lp_;
        dsp::SvfCoefficients hpCoefficients = hpCoefficients_, lpCoefficients = lpCoefficients_;
        float meterInput = meterInput_, meterEarly = meterEarly_;
        double meterDiffuse = meterDiffuse_;
        float netPeak = 0.f, earlyPeak = 0.f;

        for (int i = 0; i < n; ++i) {
            const float dryL = ch[0][at + i];
            const float dryR = channels == 2 ? ch[1][at + i] : dryL;
            const float x = channels == 2 ? 0.5f * (dryL + dryR) : dryL;
            publish(Signal, x);
            meterInput = std::max(meterInput, std::abs(x));
            double s = size.value, sc = s * scale.value, before = predelay.value, later = onset.value;
            if (delaysGliding) {
                s = size.next();
                sc = s * scale.next();
                before = predelay.next();
                later = onset.next();
            }
            float reflectGain = static_cast<float>(reflect.value), diffuseGain = static_cast<float>(diffuse.value);
            float width = static_cast<float>(stereo.value), wet = static_cast<float>(mix.value);
            float loMix = static_cast<float>(loCut.value), hiMix = static_cast<float>(hiCut.value);
            if (levelsGliding) {
                reflectGain = static_cast<float>(reflect.next());
                diffuseGain = static_cast<float>(diffuse.next());
                width = static_cast<float>(stereo.next());
                wet = static_cast<float>(mix.next());
                loMix = static_cast<float>(loCut.next());
                hiMix = static_cast<float>(hiCut.next());
            }
            // Density's fade (an S-curve, 0 where the lines switch): each diffuser's input, each
            // loop's, what is written back, the tail.
            float fade = 1.f;
            if (fading != FadePhase::Idle) {
                const double t = std::min(1.0, (fadeFrom_ + i + 1) * fadeStep);
                fade = static_cast<float>(fading == FadePhase::Out ? sCurve(1.0 - t) : sCurve(t));
            }

            // The input filter: each section cross-faded with its switch.
            float filtered = x;
            if (filterMoving) {
                hpCoefficients = dsp::SvfCoefficients(hpG.next(), kButterworthK);
                lpCoefficients = dsp::SvfCoefficients(lpG.next(), kButterworthK);
            }
            if (loActive) {
                const dsp::Svf::Outputs o = hp.tick(hpCoefficients, filtered);
                const float high = filtered - kButterworthK * o.band - o.low;
                filtered += loMix * (high - filtered);
            }
            if (hiActive) {
                const float low = lp.tick(lpCoefficients, filtered).low;
                filtered += hiMix * (low - filtered);
            }
            input_.push(filtered);

            // Early reflections.
            float earlyL = 0.f, earlyR = 0.f;
            for (int t = 0; t < tapCount; ++t) {
                TapState& tap = taps[static_cast<size_t>(t)];
                const float v = input_.hermite(static_cast<float>(before + tap.time * s) + tap.drift.next());
                earlyL += v * tap.left.next();
                earlyR += v * tap.right.next();
            }
            earlyL *= reflectGain;
            earlyR *= reflectGain;
            earlyPeak = std::max(earlyPeak, std::max(std::abs(earlyL), std::abs(earlyR)));

            // The network's input, through the diffusers.
            float u = input_.hermite(static_cast<float>(before + later));
            const size_t wd = diffusers_.write();
            for (int j = 0; j < kDiffusers; ++j) {
                DiffuserState& diffuser = diffusers[static_cast<size_t>(j)];
                float* const line = diffBase + static_cast<size_t>(j) * diffStride;
                float zj;
                if (delaysMoving) {
                    diffuser.read.follow(diffuserDelay(diffuser, sc), line, wd, diffMask, diffuser.y);
                    zj = diffuser.read.readMoving(line, wd, diffMask, diffuser.y);
                } else {
                    zj = diffuser.read.read(line, wd, diffMask, diffuser.y);
                }
                const float g = diffuser.gain.next();
                const float w = fade * u + g * zj;  // (faded in: a diffuser that joins hears nothing abrupt)
                line[wd] = w;
                u = zj - g * w;
            }
            diffusers_.advance();
            u *= inject.next();

            // The lines: read, filtered, scaled, (High: all-passed), mixed, written back.
            const size_t w = lines_.write(), wa = allpasses_.write();
            const float gHi = gHiRamp.next(), gLo = gLoRamp.next();
            const float gAp = LoopAllpass ? apGain.next() : 0.f;
            float v[N];
            float tailL = 0.f, tailR = 0.f;
            for (int k = 0; k < N; ++k) {
                const int q = kFirst + kStride * k;
                LineState& line = lines[static_cast<size_t>(k)];
                const float* const read = lineBase + static_cast<size_t>(q) * lineStride;
                float r;
                if (linesMoving) {
                    line.read.follow(lineDelay(line, s) + line.mod.next(), read, w, lineMask, line.y);
                    r = line.read.readMoving(read, w, lineMask, line.y);
                } else {
                    r = line.read.read(read, w, lineMask, line.y);
                }
                tailL += (k & 1) ? -r : r;  // Hadamard row 1
                tailR += (k & 2) ? -r : r;  // and row 2: uncorrelated sums of the same lines
                const float h = line.hi.lowpass(r, gHi);
                float y = h + line.kHi.next() * (r - h);
                const float l = line.lo.lowpass(y, gLo);
                y += (line.kLo.next() - 1.f) * l;
                y *= line.gain.next() * fade;
                if constexpr (LoopAllpass) {
                    float* const ap = apBase + static_cast<size_t>(q) * apStride;
                    float za;
                    if (delaysMoving) {
                        line.allpassRead.follow(allpassDelay(line, sc), ap, wa, apMask, line.allpassY);
                        za = line.allpassRead.readMoving(ap, wa, apMask, line.allpassY);
                    } else {
                        za = line.allpassRead.read(ap, wa, apMask, line.allpassY);
                    }
                    const float wv = y + gAp * za;
                    ap[wa] = wv;
                    y = za - gAp * wv;
                }
                v[k] = y;
            }
            hadamard<N>(v);
            for (int k = 0; k < N; ++k) {
                const int q = kFirst + kStride * k;
                lineBase[static_cast<size_t>(q) * lineStride + w] =
                    fade * (v[(k + 1) & (N - 1)] + static_cast<float>(reverb::kInputSign[static_cast<size_t>(k)]) * u);
            }
            lines_.advance();
            if constexpr (LoopAllpass) allpasses_.advance();
            const float out = kTailGain * fade;
            tailL *= out;
            tailR *= out;
            netPeak = std::max(netPeak, std::max(std::abs(tailL), std::abs(tailR)));
            tailL *= diffuseGain;
            tailR *= diffuseGain;
            publish(Tail, 0.5f * (tailL + tailR));
            meterEarly = std::max(meterEarly, std::max(std::abs(earlyL), std::abs(earlyR)));
            meterDiffuse += 0.5 * (static_cast<double>(tailL) * tailL + static_cast<double>(tailR) * tailR);

            // Stereo, then Dry/Wet.
            const float wetL = earlyL + tailL, wetR = earlyR + tailR;
            const float mid = 0.5f * (wetL + wetR);
            const float side = 0.5f * (wetL - wetR) * width;
            if (channels == 2) {
                ch[0][at + i] = dryL * (1.f - wet) + (mid + side) * wet;
                ch[1][at + i] = dryR * (1.f - wet) + (mid - side) * wet;
            } else {
                ch[0][at + i] = dryL * (1.f - wet) + mid * wet;
            }
            if (++meterCount_ == kMeterSamples) {
                publishMeters(i, meterInput, meterEarly, std::sqrt(meterDiffuse / kMeterSamples));
                meterInput = meterEarly = 0.f;
                meterDiffuse = 0.0;
            }
        }

        for (int k = 0; k < N; ++k) line_[static_cast<size_t>(kFirst + kStride * k)] = lines[static_cast<size_t>(k)];
        for (int j = 0; j < kDiffusers; ++j) diffuser_[static_cast<size_t>(j)] = diffusers[static_cast<size_t>(j)];
        for (int t = 0; t < tapCount; ++t)
            tap_[static_cast<size_t>(tapList_[static_cast<size_t>(t)])] = taps[static_cast<size_t>(t)];
        size_ = size;
        scale_ = scale;
        predelay_ = predelay;
        onset_ = onset;
        reflect_ = reflect;
        diffuse_ = diffuse;
        stereo_ = stereo;
        mix_ = mix;
        loCutMix_ = loCut;
        hiCutMix_ = hiCut;
        gHi_ = gHiRamp;
        gLo_ = gLoRamp;
        allpassGain_ = apGain;
        inject_ = inject;
        hpG_ = hpG;
        lpG_ = lpG;
        hp_ = hp;
        lp_ = lp;
        hpCoefficients_ = hpCoefficients;
        lpCoefficients_ = lpCoefficients;
        meterInput_ = meterInput;
        meterEarly_ = meterEarly;
        meterDiffuse_ = meterDiffuse;
        netPeak_ = netPeak;
        earlyPeak_ = earlyPeak;
    }

    // Asleep: the wet is exactly 0 (the network skipped), the dry still goes
    // through Dry/Wet; the glides go on.
    void renderAsleep(float* const* ch, int channels, int at, int n) noexcept {
        if (!mix_.settled() || (mix_.value != 0.0 && mix_.value != 1.0)) {
            for (int i = 0; i < n; ++i) {
                const auto dry = static_cast<float>(1.0 - mix_.next());
                for (int c = 0; c < channels; ++c) ch[c][at + i] *= dry;
            }
        } else if (mix_.value == 1.0) {
            for (int c = 0; c < channels; ++c) std::fill_n(ch[c] + at, n, 0.f);
        }  // (all dry: the input as it is)
        for (Smooth* g : {&predelay_, &onset_, &size_, &scale_, &reflect_, &diffuse_, &stereo_, &loCutMix_, &hiCutMix_}) {
            if (g->settled()) continue;
            for (int i = 0; i < n; ++i) g->next();
        }
        for (int i = 0; i < n; ++i)
            if (++meterCount_ == kMeterSamples) publishMeters(i, -1.f, -1.f, -1.0);
    }

    // The 256-sample displays, at sample `i` of the sub-chunk: the input's and
    // the reflections' peaks and the tail's RMS (below 0: asleep, the floor), and
    // the phases as they are after it (so they step by exactly rate * 256 / fs
    // however blocks fall).
    void publishMeters(int i, float input, float early, double diffuse) noexcept {
        const auto db = [](double level) {
            return level < 0.0 ? kMeterFloorDb : std::max(kMeterFloorDb, gainToDb(static_cast<float>(level)));
        };
        publish(InputLevel, db(input));
        publish(EarlyLevel, db(early));
        publish(DiffuseLevel, db(diffuse));
        const double after = (i + 1) / sampleRate_;
        const auto phase = [](double p) { return static_cast<float>(p - std::floor(p)); };
        publish(SpinPhase, spinShown_ ? phase(spinStart_ + spinRate_ * after) : -1.f);
        const double ratio = line_[static_cast<size_t>(layout_.firstLine)].chorusRatio;
        publish(ChorusPhase, chorusShown_ ? phase(chorusStart_ + chorusRate_ * ratio * after) : -1.f);
        meterCount_ = 0;
    }

    // After an awake sub-chunk: what has died away flushed to 0, the guard, and whether to sleep.
    void afterChunk() noexcept {
        for (LineState& line : line_) {
            line.y = static_cast<float>(dsp::flushTiny(line.y));
            line.hi.s = static_cast<float>(dsp::flushTiny(line.hi.s));
            line.lo.s = static_cast<float>(dsp::flushTiny(line.lo.s));
            line.allpassY = static_cast<float>(dsp::flushTiny(line.allpassY));
        }
        for (DiffuserState& diffuser : diffuser_) diffuser.y = static_cast<float>(dsp::flushTiny(diffuser.y));
        for (dsp::Svf* svf : {&hp_, &lp_}) {
            svf->ic1 = static_cast<float>(dsp::flushTiny(svf->ic1));
            svf->ic2 = static_cast<float>(dsp::flushTiny(svf->ic2));
        }

        // The guard: the loops' gain eases down while the tail's output is too loud, and back.
        if (netPeak_ > kGuardLevel) {
            gamma_ = std::max(0.5, gamma_ * 0.97);
            gammaMoved_ = true;
        } else if (gamma_ != 1.0) {
            gamma_ += 0.002 * (1.0 - gamma_);
            if (1.0 - gamma_ < 1e-6) gamma_ = 1.0;
            gammaMoved_ = true;
        }

        // Sleep once the input line holds nothing but silence and the tail has died away.
        const double s = size_.value, sc = s * scale_.value;
        double reach = predelay_.value + onset_.value +
                       (reverb::kTapMs[kMaxTaps - 1] + reverb::kSpinDepthMs) * s * sampleRate_ / 1000.0 + 4.0;
        for (int j = 0; j < layout_.diffusers; ++j) reach += diffuserDelay(diffuser_[static_cast<size_t>(j)], sc);
        if (static_cast<double>(silent_) > reach && netPeak_ < kSleepLevel && earlyPeak_ < kSleepLevel &&
            freeze_.settled(0.0) && !isOn(Freeze) && fade_.phase == FadePhase::Idle) {
            sleeping_ = true;
        }
    }

    // The Walsh-Hadamard transform in place (unnormalized: 1 / sqrt(N) is in the loops' gains).
    template <int N>
    static void hadamard(float* v) noexcept {
        for (int h = 1; h < N; h *= 2) {
            for (int i = 0; i < N; i += 2 * h) {
                for (int j = i; j < i + h; ++j) {
                    const float a = v[j], b = v[j + h];
                    v[j] = a + b;
                    v[j + h] = a - b;
                }
            }
        }
    }

    static const std::vector<ParamInfo>& infos() {
        const std::vector<std::string>& kOnOff = offOnLabels();
        static const std::vector<ParamInfo> kInfos = {
            {"predelay", "Predelay", "ms", 0.5f, 250.f, 2.5f, true},
            {"lo_cut", "Lo Cut", "", 0.f, 1.f, 1.f, false, kOnOff},
            {"hi_cut", "Hi Cut", "", 0.f, 1.f, 1.f, false, kOnOff},
            {"in_freq", "In Filter Freq", "Hz", 50.f, 18000.f, 830.f, true},
            {"in_width", "In Filter Width", "oct", 0.5f, 9.f, 7.5f},
            {"spin", "ER Spin", "", 0.f, 1.f, 1.f, false, kOnOff},
            {"spin_rate", "ER Spin Rate", "Hz", 0.07f, 1.3f, 0.3f, true},
            {"spin_amount", "ER Spin Amount", "%", 0.f, 100.f, 25.f},
            {"shape", "ER Shape", "%", 0.f, 100.f, 50.f},
            {"density", "Density", "", 0.f, 3.f, 3.f, false, {"Sparse", "Low", "Mid", "High"}},
            {"smooth", "Size Smoothing", "", 0.f, 2.f, 1.f, false, {"None", "Slow", "Fast"}},
            {"size", "Room Size", "size", 0.22f, 500.f, 100.f, true},
            {"stereo", "Stereo Image", "%", 0.f, 120.f, 100.f},
            {"lo_shelf", "Lo Shelf", "", 0.f, 1.f, 1.f, false, kOnOff},
            {"lo_freq", "Lo Shelf Freq", "Hz", 20.f, 15000.f, 90.f, true},
            {"lo_gain", "Lo Shelf Gain", "%", 20.f, 100.f, 75.f},
            {"hi_filter", "Hi Filter", "", 0.f, 1.f, 1.f, false, kOnOff},
            {"hi_type", "Hi Filter Type", "", 0.f, 1.f, 0.f, false, {"Shelf", "Low-pass"}},
            {"hi_freq", "Hi Filter Freq", "Hz", 20.f, 16000.f, 4500.f, true},
            {"hi_gain", "Hi Shelf Gain", "%", 20.f, 100.f, 70.f},
            {"decay", "Decay Time", "ms", 200.f, 60000.f, 1200.f, true},
            {"freeze", "Freeze", "", 0.f, 1.f, 0.f, false, kOnOff},
            {"flat", "Flat", "", 0.f, 1.f, 1.f, false, kOnOff},
            {"cut", "Cut", "", 0.f, 1.f, 1.f, false, kOnOff},
            {"diffusion", "Diffusion", "%", 0.f, 100.f, 70.f},
            {"scale", "Scale", "%", 0.f, 100.f, 50.f},
            {"chorus", "Chorus", "", 0.f, 1.f, 1.f, false, kOnOff},
            {"chorus_rate", "Chorus Rate", "Hz", 0.01f, 8.f, 0.8f, true},
            {"chorus_amount", "Chorus Amount", "%", 0.f, 100.f, 20.f},
            {"reflect", "Reflect Level", "dB", -30.f, 6.f, 0.f},
            {"diffuse", "Diffuse Level", "dB", -30.f, 6.f, 0.f},
            {"mix", "Dry/Wet", "%", 0.f, 100.f, 40.f},
        };
        return kInfos;
    }

    // sin and cos of 2 pi k / 12: each tap's place on Spin's circle.
    static constexpr std::array<double, kMaxTaps> kTapSin = {
        0.0, 0.5, 0.86602540378443865, 1.0, 0.86602540378443865, 0.5,
        0.0, -0.5, -0.86602540378443865, -1.0, -0.86602540378443865, -0.5};
    static constexpr std::array<double, kMaxTaps> kTapCos = {
        1.0, 0.86602540378443865, 0.5, 0.0, -0.5, -0.86602540378443865,
        -1.0, -0.86602540378443865, -0.5, 0.0, 0.5, 0.86602540378443865};

    double sampleRate_ = 48000.0;
    std::array<double, TauCount> glide32_{};
    int fadeLength_ = 576;

    // Buffers (prepare() sizes them).
    dsp::DelayLine input_;
    LineBank lines_, allpasses_, diffusers_;

    // The network.
    std::array<LineState, kMaxLines> line_{};
    std::array<DiffuserState, kMaxDiffusers> diffuser_{};
    reverb::Layout layout_ = reverb::layout(reverb::Density::High);
    int layoutDensity_ = 3;
    bool layoutSwitched_ = false;
    Fade fade_;
    FadePhase fading_ = FadePhase::Idle;  // this sub-chunk's fade, from fadeFrom_ samples into it
    int fadeFrom_ = 0;
    Ramp gHi_, gLo_, allpassGain_, inject_;
    bool delaysMoving_ = false, linesMoving_ = false, loopsRamping_ = false;
    double gamma_ = 1.0;  // the guard's share of the loops' gain
    bool gammaMoved_ = false;

    // The input filter.
    dsp::Svf hp_, lp_;
    dsp::SvfCoefficients hpCoefficients_, lpCoefficients_;
    Ramp hpG_, lpG_;
    bool filterMoved_ = false, filterMoving_ = false;

    // The early reflections.
    std::array<TapState, kMaxTaps> tap_{};
    std::array<int, kMaxTaps> tapList_{};  // the taps heard now
    int taps_ = 0;
    float tapShape_ = -1.f;
    int tapDensity_ = -1;

    // The glides: a sample at a time (delays, levels) and a sub-chunk at a time.
    Smooth predelay_, onset_, size_, scale_, reflect_, diffuse_, stereo_, mix_, loCutMix_, hiCutMix_;
    Glide inFreq_, inWidth_, decay_, loFreq_, hiFreq_, loShare_, hiShare_, loOn_, hiOn_, lowpass_, freeze_, flat_, cut_,
        diffusion_, spinAmount_, chorusAmount_, spinRateLog_, chorusRateLog_;
    bool snapping_ = false;
    Cached logInFreq_, logDecay_, logLoFreq_, logHiFreq_, logSpinRate_, logChorusRate_, reflectGain_, diffuseGain_,
        sizeCoefficient_;

    // The LFOs.
    double spinPhase_ = 0.0, spinStart_ = 0.0, chorusStart_ = 0.0;
    double spinRate_ = 0.3, chorusRate_ = 0.8;
    bool spinShown_ = true, chorusShown_ = true;

    // Sleeping, and the meters.
    bool sleeping_ = false;
    int64_t silent_ = 0;  // samples since the input was last above kAwakeLevel
    float netPeak_ = 0.f, earlyPeak_ = 0.f;
    int meterCount_ = 0;
    float meterInput_ = 0.f, meterEarly_ = 0.f;
    double meterDiffuse_ = 0.0;
};

}  // namespace

SUB_REGISTER_BUILTIN(ReverbProcessor, AudioEffect);

}  // namespace sub
