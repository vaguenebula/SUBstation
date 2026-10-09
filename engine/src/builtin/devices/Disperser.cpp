// Built-in "Disperser" device, after Kilohearts' Disperser: up to 64 identical
// second-order all-pass filters in a row (builtin/DisperserDesign.h). Every
// frequency comes out at the level it went in, but what is around the stages'
// frequency comes out later than the rest: a transient smears into a chirp, a
// kick's click into a zap. Amount is the number of stages (0 passes the input
// through untouched), Frequency where they turn the phase (kept below
// Nyquist), Pinch their Q: how narrow the band they delay, and how long they
// delay it. Dry/Wet blends the input with the stages' output; Bypass passes the
// input through with the device left on.
//
// - Frequency and Pinch glide (in log, two one-poles of 20 ms in a row, so a
//   jump eases in and out), and while they move the stages' coefficients follow
//   them sample by sample. The stages are normalized lattices, which hold the
//   energy they were given however their coefficients move: a glide can't make
//   anything louder than what went in. (Through many narrow stages a glide is
//   still heard: what they held back catches up as the delay shrinks, as a
//   tape's pitch moves while its speed does.)
// - A change of Amount fades (20 ms, an S-curve) to the output after the new
//   number of stages. Fewer: from the output after the old number to the output
//   after the new, both tapped from one cascade. More: the stages added start
//   from silence and their input fades in, while the old output fades out, so
//   they never hear the signal start abruptly (which they would smear into a
//   chirp longer than any fade). Stages no longer heard are cleared. A change
//   that comes during a fade starts when it is done.
// - Dry/Wet and Bypass glide (two one-poles of 5 ms in a row: most of the way in
//   20 ms, and they can turn back halfway without a kink); what is heard of the
//   stages' output is the one times the other. In between, the input and its
//   dispersed copy add up as a phaser's do: notches where they are out of phase,
//   so the level is flat only fully wet (or dry). The stages go on running while
//   bypassed (or dry), so it comes back without a seam; switch the device off to
//   save their work.
// - Each channel has its own states, in double, flushed below 1e-20 after each
//   stretch so silence rings out to exact zeros. On one channel, that one.
// - The dispersion is the effect, not latency: latencySamples() is 0, so the
//   engine doesn't delay the other tracks to line them up with it.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/DisperserDesign.h"
#include "builtin/Dsp.h"

namespace sub {
namespace {

constexpr double kGlideSeconds = 0.02;  // Frequency and Pinch: each of the two one-poles
constexpr double kFadeSeconds = 0.02;   // Amount
constexpr double kMixSeconds = 0.005;   // Dry/Wet and Bypass: each of the two one-poles
constexpr int kChannels = 2;
constexpr int kMaxStages = disperser::kMaxStages;

// 0 at 0, 1 at 1, flat at both ends: Amount's fades' shape.
inline double sCurve(double t) noexcept { return t * t * (3.0 - 2.0 * t); }

// Two one-poles in a row gliding to a target: a jump eases in and out, and a
// glide can turn back halfway without a kink. It lands on the target exactly
// once it is within `landed` of it.
struct Ease {
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

class DisperserProcessor final : public BuiltinProcessor {
public:
    enum Param { Amount = 0, Frequency, Pinch, Mix, Bypass, NumParams };

    DisperserProcessor() : BuiltinProcessor(infos()) {}

    std::string typeId() const override { return "builtin:disperser"; }
    std::string name() const override { return "Disperser"; }

    // Until what is left of its impulse response is 60 dB down: a quarter longer
    // than its largest group delay, and 7 time constants of its slowest pole (a
    // bound measured over the parameters' range at 44.1 and 192 kHz); at most 60 s.
    int tailSamples() const override {
        const int stages = targetStages();
        if (stages == 0 || isOn(Bypass) || param(Mix) <= 0.f) return 0;
        const disperser::Stage stage = disperser::design(param(Frequency), param(Pinch), sampleRate_);
        const double samples =
            1.25 * stages * disperser::maxGroupDelaySamples(stage) + 7.0 / disperser::decayPerSample(stage);
        return static_cast<int>(std::min(samples, 60.0 * sampleRate_));
    }

    void prepare(double sampleRate, int) override {
        sampleRate_ = sampleRate;
        glide_ = 1.0 - onePoleCoefficient(kGlideSeconds, sampleRate);
        mixGlide_ = 1.0 - onePoleCoefficient(kMixSeconds, sampleRate);
        fadeLength_ = std::max(1, static_cast<int>(std::lround(kFadeSeconds * sampleRate)));
        reset();
    }

    // Silent, and every glide and fade where the parameters are.
    void reset() override {
        clearStages(0, kMaxStages);
        snapToParams();
    }

protected:
    void render(const ProcessContext&, float* const* ch, int numChannels, int numFrames) override {
        const int n = std::min(numChannels, kChannels);
        if (n <= 0) return;
        if (n != channels_) {  // a channel that wasn't processed has no history to go on from
            clearStages(0, kMaxStages);
            channels_ = n;
        }
        const int target = targetStages();
        if (stages_ == 0 && target == 0 && !fading_) {
            // No stages, none coming: the input passes untouched, and stages added
            // later start from silence where the parameters are.
            snapToParams();
            return;
        }
        if (n == 2) {
            renderFrames<2>(ch, numFrames, target);
        } else {
            renderFrames<1>(ch, numFrames, target);
        }
        flushStages();
    }

private:
    using State = disperser::State;

    // Stages [from, to) on N channels in step (independent chains, which the CPU overlaps).
    template <int N>
    void runStages(const disperser::Stage& stage, int from, int to, double* y) noexcept {
        for (int s = from; s < to; ++s) {
            for (int c = 0; c < N; ++c) y[c] = disperser::process(stage, states_[c][s], y[c]);
        }
    }

    template <int N>
    void renderFrames(float* const* ch, int numFrames, int target) noexcept {
        const double logFreq = targetLogFreq(), logPinch = targetLogPinch();
        const double wetTarget = targetWet(), bypassTarget = targetBypass();
        const double fadeLength = fadeLength_;
        for (int i = 0; i < numFrames; ++i) {
            if (gliding(logFreq, logPinch)) stage_ = designNow();
            if (!fading_ && target != stages_) {
                fading_ = true;
                from_ = stages_;
                to_ = target;
                fadeAt_ = 0;
            }
            // What is heard of the stages' output: Dry/Wet's share, unless bypassed.
            const double wet = wet_.next(wetTarget, mixGlide_, 1e-7) * on_.next(bypassTarget, mixGlide_, 1e-7);
            const int from = fading_ ? from_ : stages_;
            const int to = fading_ ? to_ : stages_;
            const bool adding = to > from;
            const double toShare = fading_ ? sCurve((fadeAt_ + 1) / fadeLength) : 1.0;
            const disperser::Stage stage = stage_;  // (a copy the compiler keeps in registers)

            double y[N], tap[N];
            for (int c = 0; c < N; ++c) y[c] = ch[c][i];
            runStages<N>(stage, 0, std::min(from, to), y);  // what every tap goes through
            for (int c = 0; c < N; ++c) {
                tap[c] = y[c];
                if (adding) y[c] *= toShare;  // stages added hear it fade in
            }
            runStages<N>(stage, std::min(from, to), std::max(from, to), y);
            for (int c = 0; c < N; ++c) {
                // Adding, the old output fades out as the new stages' (already faded in) comes;
                // removing (or steady), from the longer cascade's output to the tap.
                const double out = adding ? (1.0 - toShare) * tap[c] + y[c] : (1.0 - toShare) * y[c] + toShare * tap[c];
                const double dry = ch[c][i];
                ch[c][i] = static_cast<float>((1.0 - wet) * dry + wet * out);
            }

            if (fading_ && ++fadeAt_ >= fadeLength_) {
                fading_ = false;
                stages_ = to_;
                if (from_ > to_) clearStages(to_, from_);  // no longer heard: silent when next added
            }
        }
    }

    // One sample of Frequency's and Pinch's glides towards their targets; true while they move.
    bool gliding(double logFreq, double logPinch) noexcept {
        if (freq_.settled(logFreq) && pinch_.settled(logPinch)) return false;
        freq_.next(logFreq, glide_, 1e-6);
        pinch_.next(logPinch, glide_, 1e-6);
        return true;
    }

    void clearStages(int from, int to) noexcept {
        for (auto& channel : states_) {
            for (int s = from; s < to; ++s) channel[s] = State{};
        }
    }

    // Flushes what has died away to zero, so silence never runs into denormals.
    void flushStages() noexcept {
        const int used = std::max(stages_, fading_ ? std::max(from_, to_) : 0);
        for (auto& channel : states_) {
            for (int s = 0; s < used; ++s) {
                State& state = channel[s];
                state.d1 = dsp::flushTiny(state.d1);
                state.d2 = dsp::flushTiny(state.d2);
            }
        }
    }

    void snapToParams() noexcept {
        freq_.snap(targetLogFreq());
        pinch_.snap(targetLogPinch());
        stage_ = designNow();
        stages_ = from_ = to_ = targetStages();
        fading_ = false;
        fadeAt_ = 0;
        wet_.snap(targetWet());
        on_.snap(targetBypass());
    }

    int targetStages() const noexcept {
        return std::clamp(choiceIndex(Amount), 0, kMaxStages);
    }
    // The glides' targets: the frequency as the stages are tuned to it (below Nyquist), in log.
    double targetLogFreq() const noexcept {
        return std::log(disperser::stageFrequency(param(Frequency), sampleRate_));
    }
    double targetLogPinch() const noexcept {
        return std::log(std::clamp<double>(param(Pinch), disperser::kMinPinch, disperser::kMaxPinch));
    }
    double targetWet() const noexcept { return std::clamp(param(Mix), 0.f, 100.f) / 100.0; }
    double targetBypass() const noexcept { return isOn(Bypass) ? 0.0 : 1.0; }
    disperser::Stage designNow() const noexcept {
        return disperser::design(std::exp(freq_.value), std::exp(pinch_.value), sampleRate_);
    }

    static const std::vector<ParamInfo>& infos() {
        static const std::vector<ParamInfo> kInfos = {
            {"amount", "Amount", "stages", 0.f, static_cast<float>(kMaxStages), 16.f, false, {}, kMaxStages},
            {"freq", "Frequency", "Hz", static_cast<float>(disperser::kMinFrequency),
             static_cast<float>(disperser::kMaxFrequency), 1000.f, true},
            {"pinch", "Pinch", "Q", static_cast<float>(disperser::kMinPinch), static_cast<float>(disperser::kMaxPinch),
             1.f, true},
            {"mix", "Dry/Wet", "%", 0.f, 100.f, 100.f},
            {"bypass", "Bypass", "", 0.f, 1.f, 0.f, false, offOnLabels()},
        };
        return kInfos;
    }

    double sampleRate_ = 48000.0;
    double glide_ = 0.001;
    double mixGlide_ = 0.004;
    int fadeLength_ = 960;
    int channels_ = kChannels;

    State states_[kChannels][kMaxStages] = {};
    disperser::Stage stage_;  // the coefficients now, as Frequency and Pinch glide
    Ease freq_, pinch_;        // their glides, in log
    int stages_ = 0;           // the stages heard (when not fading)
    bool fading_ = false;      // from from_ stages to to_, fadeAt_ samples in
    int from_ = 0, to_ = 0, fadeAt_ = 0;
    Ease wet_, on_;            // Dry/Wet's glide (0..1) and Bypass's (0 bypassed, 1 on)
};

}  // namespace

SUB_REGISTER_BUILTIN(DisperserProcessor, AudioEffect);

}  // namespace sub
