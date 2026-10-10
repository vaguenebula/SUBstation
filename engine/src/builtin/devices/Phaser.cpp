// Built-in "Phaser-Flanger" device, after Ableton's Phaser-Flanger: a phaser,
// a flanger and a doubler in one, swept by two LFOs and an envelope follower
// (the maths in builtin/PhaserDesign.h, shared with its editor's graph).
//
// - Phaser: up to 42 identical second-order all-passes in a row (the
//   Disperser's lattice stages), tuned to Center, their Q the Spread. Blended
//   with the input they cut one notch each; the modulation moves Center (Blend
//   0), Spread (Blend 1) or both. Its feedback is taken one sample late.
// - Flanger and Doubler: one delay line per channel, read with 4-point Hermite
//   interpolation at the delay (exact at whole samples); the modulation moves
//   the flanger's delay two octaves, the doubler's 15 %. The line is written in
//   every mode, so switching to a delay mode reads real history at once.
// - Feedback goes back in with its polarity (Ø), through a soft limit above
//   +6 dBFS so nothing runs away; Warmth gently saturates and darkens the
//   effect inside the loop; Safe Bass keeps the lows dry (a Linkwitz-Riley
//   split); Dry/Wet and Output last.
// - Control works per chunk of 16 frames: the LFOs and the envelope give the
//   modulation at each chunk's end, which is drawn across the chunk and
//   smoothed sample by sample. The Phaser's stages move linearly from one
//   chunk end's coefficients to the next's; the delays follow the smoothed
//   modulation sample by sample (a long delay would turn a chunk's corners
//   into a zipper).
// - Nothing steps: the continuous controls glide (two one-poles in a row, so
//   a jump eases in and out); while Center, Spread, Blend or a delay time
//   glides, what it moves is worked out sample by sample; the gains glide per
//   sample; the right LFO's Phase glides the short way round. A change of Mode
//   crossfades the cores (Flanger and Doubler: two reads of one line), a change
//   of Notches fades as the Disperser's Amount does, an LFO's jump (a new
//   waveform, Sync, the transport) crossfades its value, Safe Bass fades in and
//   out (its crossover's frequency moving sample by sample).
// - Silence rings out to exact zeros without the renderer's denormal flushing:
//   recursive states are flushed per chunk, the cascade's output and the delay
//   line's writes per sample.
// - The delay and the sweep are the effect, not latency: latencySamples() is 0.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/DisperserDesign.h"
#include "builtin/Dsp.h"
#include "builtin/DspBlocks.h"
#include "builtin/PhaserDesign.h"
#include "rt/RtUtils.h"

namespace sub {
namespace {

using phaser::Mode;
using phaser::Wave;

constexpr int kChannels = 2;
constexpr int kChunk = phaser::kChunk;
constexpr int kMaxNotches = phaser::kMaxNotches;
constexpr double kLanded = 1e-6;      // the per-chunk glides land within this of their targets
constexpr double kGainLanded = 1e-7;  // the per-sample ones
constexpr double kTimeLanded = 1e-9;  // the delay times' (in log2 ms: 0.005 samples' jump at 150 ms would show)

// 0 at 0, 1 at 1, flat at both ends: the fades' shape.
inline double sCurve(double t) noexcept { return t * t * (3.0 - 2.0 * t); }

// A float state or write, zero once it is too small to hear (dsp::flushTiny is for doubles).
inline float flushTinyFloat(float v) noexcept { return std::abs(v) < 1e-15f ? 0.f : v; }

// The feedback path's soft limit: as it is up to kSafetyKnee (+6 dBFS), then
// bent so it stays below kSafetyLimit: feedback at 95 % can't run away
// whatever the modulation does.
inline double safety(double v) noexcept {
    const double a = std::abs(v);
    if (a <= phaser::kSafetyKnee) return v;
    constexpr double kSpan = phaser::kSafetyLimit - phaser::kSafetyKnee;
    const double bent =
        phaser::kSafetyKnee + kSpan * dsp::fastTanh(static_cast<float>((a - phaser::kSafetyKnee) / kSpan));
    return v < 0.0 ? -bent : bent;
}

// Warmth: the effect's output blended (by w) with itself saturated and low-passed.
inline double warmed(double y, double w, dsp::OnePole& pole, float coeff) noexcept {
    constexpr auto kDrive = static_cast<float>(phaser::kWarmthDrive);
    const float saturated = dsp::fastTanh(kDrive * static_cast<float>(y)) / kDrive;
    return y + w * (static_cast<double>(pole.lowpass(saturated, coeff)) - y);
}

// Linear interpolation between two lattice stages. Each rotation's (k, c) is a
// point on the unit circle; the chord between two lies inside it, so while the
// coefficients move a stage can only lose energy, never make anything louder.
inline disperser::Stage towards(const disperser::Stage& a, const disperser::Stage& b, double t) noexcept {
    disperser::Stage s;
    s.k1 = a.k1 + t * (b.k1 - a.k1);
    s.c1 = a.c1 + t * (b.c1 - a.c1);
    s.k2 = a.k2 + t * (b.k2 - a.k2);
    s.c2 = a.c2 + t * (b.c2 - a.c2);
    return s;
}

// Two one-poles in a row gliding to a target (as the Disperser's): a jump
// eases in and out, and a glide can turn back halfway without a kink. It
// lands on the target exactly once it is within `landed` of it.
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

// A delay line read `delay` samples before the sample about to be written (≥ 2), with
// DelayLine::hermite's 4-point interpolation, but its fraction taken in double: a float
// delay resolves only 1/2048 of a sample at 150 ms, and a moving read would jitter by that.
inline float readLine(const dsp::DelayLine& line, double delay) noexcept {
    const double back = delay - 1.0;  // (tap(0) is the sample written last)
    const int whole = static_cast<int>(back);
    const auto t = static_cast<float>(back - whole);
    return dsp::hermite(line.tap(whole - 1), line.tap(whole), line.tap(whole + 1), line.tap(whole + 2), t);
}

// Which of the two delays a mode reads (the Phaser none: the flanger's, unused).
inline int delayIndex(Mode mode) noexcept { return mode == Mode::Doubler ? 1 : 0; }

class PhaserProcessor final : public BuiltinProcessor {
public:
    enum Param {
        ModeParam = 0, Notches, Center, Spread, Blend, FlangeTime, DoublerTime, Amount, Feedback, FeedbackInvert,
        Sync, Freq, Rate, Waveform, Duty, SpinOn, Phase, Spin, Lfo2Mix, Sync2, Freq2, Rate2, EnvOn, EnvAmount,
        EnvAttack, EnvRelease, SafeBass, Warmth, Output, Mix, NumParams
    };
    enum Display {
        PhaseDisplay = 0, PhaseRightDisplay, LfoDisplay, ModDisplay, EnvDisplay, SweepLeft, SweepRight, QLeft, QRight,
        InputLevel, OutputLevel
    };

    PhaserProcessor() : BuiltinProcessor(infos(), displayInfos()) {}

    std::string typeId() const override { return "builtin:phaser"; }
    std::string name() const override { return "Phaser-Flanger"; }

    // Until the wet path's impulse response is 60 dB down, at the settings'
    // slowest: the feedback's passes round its loop (ln 1e-3 / ln |g|), each as
    // long as the longest the loop delays anything (Phaser: the slowest stages
    // the modulation reaches, their largest group delay, plus their own ring;
    // the delay modes: the longest delay), with room to spare; plus Safe Bass's
    // ring. 0 fully dry; at most 60 s.
    int tailSamples() const override {
        if (param(Mix) <= 0.f) return 0;
        const double sr = sampleRate_;
        const double g = std::abs(phaser::feedbackGain(param(Feedback), false));
        const double passes = g > 1e-6 ? std::log(1e-3) / std::log(g) : 0.0;
        const double envDepth = isOn(EnvOn) ? std::abs(param(EnvAmount)) / 100.0 : 0.0;
        const double depth = std::min(phaser::kModLimit, param(Amount) / 100.0 + envDepth);
        const auto mode = static_cast<Mode>(std::clamp(choiceIndex(ModeParam), 0, 2));
        double tail = 0.0;
        if (mode == Mode::Phaser) {
            const double blend = std::clamp<double>(param(Blend), 0.0, 1.0);
            const double spread = std::clamp(param(Spread) / 100.0, 0.0, 1.0);
            const disperser::Stage slowest = disperser::design(phaser::phaserCenterHz(param(Center), blend, -depth, sr),
                                                               phaser::phaserQ(spread, blend, -depth), sr);
            const double pass = std::clamp(choiceIndex(Notches), 1, kMaxNotches) *
                                    disperser::maxGroupDelaySamples(slowest) + 1.0;
            tail = 1.5 * (7.0 / disperser::decayPerSample(slowest) + 1.25 * pass + passes * pass);
        } else {
            const double ms = mode == Mode::Flanger
                                  ? phaser::flangerMsAt(param(FlangeTime), depth)
                                  : phaser::doublerMsAt(param(DoublerTime), depth);
            tail = (passes + 1.0) * phaser::delaySamples(ms, sr) + 0.01 * sr;
        }
        if (param(SafeBass) > phaser::kSafeBassOff + 1e-3) tail += 3.0 * sr / param(SafeBass);
        return static_cast<int>(std::min(tail, 60.0 * sr));
    }

    void prepare(double sampleRate, int) override {
        sampleRate_ = sampleRate;
        const int longest = static_cast<int>(std::ceil(phaser::kMaxDelayMs * sampleRate / 1000.0)) + 4;
        for (dsp::DelayLine& line : lines_) line.prepare(longest);
        for (int frames = 0; frames <= kChunk; ++frames) {
            glide_[frames] = 1.0 - onePoleCoefficient(phaser::kGlideSeconds, sampleRate, frames);
            offsetGlide_[frames] = 1.0 - onePoleCoefficient(phaser::kOffsetSeconds, sampleRate, frames);
        }
        gainGlide_ = 1.0 - onePoleCoefficient(phaser::kGainSeconds, sampleRate);
        timeGlide_ = 1.0 - onePoleCoefficient(phaser::kTimeGlideSeconds, sampleRate);
        modGlide_ = 1.0 - onePoleCoefficient(phaser::kModSeconds, sampleRate);
        stageGlide_ = 1.0 - onePoleCoefficient(phaser::kGlideSeconds, sampleRate);
        const auto length = [sampleRate](double seconds) {
            return std::max(1, static_cast<int>(std::lround(seconds * sampleRate)));
        };
        lfoFadeLength_ = length(phaser::kLfoFadeSeconds);
        notchFadeLength_ = length(phaser::kNotchFadeSeconds);
        modeFadeLength_ = length(phaser::kModeFadeSeconds);
        safeFadeLength_ = length(phaser::kSafeFadeSeconds);
        warmCoeff_ = dsp::onePoleCutoff(phaser::kWarmthCutoffHz, sampleRate);
        envAttack_ = envRelease_ = -1.f;  // set again at the next render
        reset();
    }

    // Silent, every glide and fade where the parameters are, the LFOs at their start.
    void reset() override {
        clearAudio();
        env_.reset();
        targets_ = readTargets();
        const Targets& t = targets_;
        logCenter_.snap(t.logCenter);
        spread_.snap(t.spread);
        blend_.snap(t.blend);
        logFlange_.snap(t.logFlange);
        logDoubler_.snap(t.logDoubler);
        logSafe_.snap(t.logSafe);
        amount_.snap(t.amount);
        envAmount_.snap(t.envAmount);
        lfo2Mix_.snap(t.lfo2Mix);
        duty_.snap(t.duty);
        gain_.snap(t.feedback);
        warmth_.snap(t.warmth);
        mix_.snap(t.mix);
        out_.snap(t.outGain);
        for (LfoRun* run : {&lfo1_, &lfo2_}) {
            *run = LfoRun{};
            run->offset = t.spinOn ? 0.0 : t.phaseCycles;
        }
        wave_ = t.wave;
        for (Ease& m : mod_) m.snap(0.0);
        mode_ = modeFrom_ = modeTo_ = t.mode;
        modeFading_ = false;
        modeAt_ = 0;
        notches_ = notchFrom_ = notchTo_ = t.notches;
        notchFading_ = false;
        notchAt_ = 0;
        safeRunning_ = t.safeOn;
        safeAt_ = t.safeOn ? safeFadeLength_ : 0;
        safeLog_ = -1.0;  // (the crossover's coefficients are worked out again)
        safeFresh_ = true;
        warmRunning_ = t.warmth != 0.0;
        envRunning_ = t.envOn;
        envNow_ = 0.0;
        primed_ = false;
        stageMoving_ = timeMoving_ = false;
        stale_ = true;
        displayCount_ = 0;
        inPeak_ = outPeak_ = 0.f;
    }

protected:
    void render(const ProcessContext& ctx, float* const* ch, int numChannels, int numFrames) override {
        const int n = std::min(numChannels, kChannels);
        if (n <= 0 || numFrames <= 0) return;
        if (n != channels_) {  // a channel that wasn't processed has no history to go on from
            clearAudio();
            channels_ = n;
            stale_ = true;
        }
        targets_ = readTargets();
        if (targets_.attack != envAttack_ || targets_.release != envRelease_) {
            envAttack_ = targets_.attack;
            envRelease_ = targets_.release;
            env_.setTimes(envAttack_, envRelease_, sampleRate_);
        }
        for (int at = 0; at < numFrames; at += kChunk) {
            const int frames = std::min(kChunk, numFrames - at);
            beginChunk();
            listen(ch, n, at, frames);
            control(ctx, at + frames, frames, n);
            if (n == 2) {
                renderChunk<2>(ch, at, frames);
            } else {
                renderChunk<1>(ch, at, frames);
            }
            endChunk(n, frames);
        }
    }

private:
    // The parameters as the processing uses them, read once a stretch.
    struct Targets {
        Mode mode = Mode::Phaser;
        int notches = 4;
        Wave wave = Wave::Triangle;
        bool sync = false, sync2 = false, spinOn = false, envOn = false, safeOn = false;
        int rate = 15, rate2 = 10;
        double freq = 0.5, freq2 = 2.0;
        double logCenter = 0.0, spread = 0.5, blend = 0.0, logFlange = 0.0, logDoubler = 0.0;
        double flangeMs = 2.5, doublerMs = 30.0;
        double amount = 0.5, envAmount = 0.0, lfo2Mix = 0.0, duty = 0.0, phaseCycles = 0.5, spin = 0.1;
        double feedback = 0.0, warmth = 0.0, outGain = 1.0, mix = 0.5, logSafe = 0.0;
        float attack = 10.f, release = 200.f;
    };

    // An LFO, and its right channel's offset (in cycles, unwrapped: whole
    // cycles go into the right side's cycle count for the random shapes).
    struct LfoRun {
        dsp::Lfo lfo;
        double offset = 0.0;
        bool synced = false;    // following the song at the last chunk end
        int division = -1;      // the synced rate it followed
        double position = 0.0;  // the song's position there, in cycles
        bool valued = false;    // has values (to fade from at a jump)
        bool fading = false;    // crossfading from `from` after a jump, fadeAt frames in
        int fadeAt = 0;
        double from[kChannels] = {}, value[kChannels] = {};
    };

    Targets readTargets() const noexcept {
        Targets t;
        t.mode = static_cast<Mode>(std::clamp(choiceIndex(ModeParam), 0, 2));
        t.notches = std::clamp(choiceIndex(Notches), 1, kMaxNotches);
        t.wave = static_cast<Wave>(std::clamp(choiceIndex(Waveform), 0, 9));
        t.sync = isOn(Sync);
        t.sync2 = isOn(Sync2);
        t.spinOn = isOn(SpinOn);
        t.envOn = isOn(EnvOn);
        const int divisions = static_cast<int>(dsp::syncedDivisionLabels().size());
        t.rate = std::clamp(choiceIndex(Rate), 0, divisions - 1);
        t.rate2 = std::clamp(choiceIndex(Rate2), 0, divisions - 1);
        t.freq = std::clamp<double>(param(Freq), phaser::kMinRate, phaser::kMaxRate);
        t.freq2 = std::clamp<double>(param(Freq2), phaser::kMinRate, phaser::kMaxRate);
        t.logCenter = std::log2(std::clamp<double>(param(Center), phaser::kMinCenter, phaser::kMaxCenter));
        t.spread = std::clamp(param(Spread) / 100.0, 0.0, 1.0);
        t.blend = std::clamp<double>(param(Blend), 0.0, 1.0);
        t.flangeMs = std::max<double>(param(FlangeTime), 0.01);
        t.doublerMs = std::max<double>(param(DoublerTime), 0.01);
        t.logFlange = std::log2(t.flangeMs);
        t.logDoubler = std::log2(t.doublerMs);
        t.amount = std::clamp(param(Amount) / 100.0, 0.0, 1.0);
        t.envAmount = t.envOn ? std::clamp(param(EnvAmount) / 100.0, -1.0, 1.0) : 0.0;
        t.lfo2Mix = std::clamp(param(Lfo2Mix) / 100.0, 0.0, 1.0);
        t.duty = std::clamp(param(Duty) / 100.0, -1.0, 1.0);
        t.phaseCycles = param(Phase) / 360.0;
        t.spin = std::clamp(param(Spin) / 100.0, 0.0, 1.0);
        t.feedback = phaser::feedbackGain(param(Feedback), isOn(FeedbackInvert));
        t.warmth = std::clamp(param(Warmth) / 100.0, 0.0, 1.0);
        t.outGain = dbToGain(param(Output));
        t.mix = std::clamp(param(Mix) / 100.0, 0.0, 1.0);
        t.safeOn = param(SafeBass) > phaser::kSafeBassOff + 1e-3;
        t.logSafe = std::log2(std::max<double>(param(SafeBass), phaser::kSafeBassOff));
        t.attack = param(EnvAttack);
        t.release = param(EnvRelease);
        return t;
    }

    // --- Per chunk ------------------------------------------------------------------------

    // Starts what a chunk's parameters call for: a mode's fade, a change of
    // Notches, Safe Bass coming or going, Warmth or the envelope stopping.
    void beginChunk() noexcept {
        const Targets& t = targets_;
        if (!modeFading_ && t.mode != mode_) {
            modeFading_ = true;
            modeFrom_ = mode_;
            modeTo_ = t.mode;
            modeAt_ = 0;
            if (modeTo_ == Mode::Phaser) clearCascade();  // (idle already) it starts from silence
        }
        if (!notchFading_ && t.notches != notches_) {
            const bool heard = modeFading_ ? modeTo_ == Mode::Phaser : mode_ == Mode::Phaser;
            const bool leaving = modeFading_ && modeFrom_ == Mode::Phaser;
            if (heard) {
                notchFading_ = true;
                notchFrom_ = notches_;
                notchTo_ = t.notches;
                notchAt_ = 0;
            } else if (!leaving) {
                notches_ = t.notches;  // the cascade is idle and clear
            }
        }
        if (t.safeOn) {
            if (!safeRunning_) {
                for (dsp::Crossover& split : splits_) split.reset();
                safeRunning_ = true;
                safeFresh_ = true;
                safeLog_ = -1.0;
            }
        } else if (safeRunning_ && safeAt_ == 0) {
            for (dsp::Crossover& split : splits_) split.reset();  // the next switch-on starts from silence
            safeRunning_ = false;
        }
        if (t.warmth == 0.0 && warmth_.settled(0.0)) {
            if (warmRunning_) {
                for (dsp::OnePole& pole : warmPhaser_) pole.reset();
                for (dsp::OnePole& pole : warmDelay_) pole.reset();
                warmRunning_ = false;
            }
        } else {
            warmRunning_ = true;
        }
        const bool envRunning = t.envOn || envAmount_.value != 0.0;
        if (!envRunning) env_.reset();  // off and faded out: it starts from silence when switched on again
        envRunning_ = envRunning;
    }

    // The input's level over the chunk: the input meter's peak, and the
    // envelope follower (linked: the louder channel).
    void listen(float* const* ch, int n, int at, int frames) noexcept {
        float peak = inPeak_;
        for (int i = at; i < at + frames; ++i) {
            float level = std::abs(ch[0][i]);
            if (n > 1) level = std::max(level, std::abs(ch[1][i]));
            peak = std::max(peak, level);
            if (envRunning_) env_.peak(level);
        }
        inPeak_ = peak;
    }

    // The chunk end's controls: the glides moved on, the LFOs, the envelope, and from
    // them each channel's modulation, the stages' coefficients and the delays there.
    void control(const ProcessContext& ctx, int endFrame, int frames, int n) noexcept {
        const Targets& t = targets_;
        const double glide = glide_[frames];
        // (Center, Spread, Blend and the delay times glide per sample: renderFrames)
        stageMoving_ = !(logCenter_.settled(t.logCenter) && spread_.settled(t.spread) && blend_.settled(t.blend));
        timeMoving_ = !(logFlange_.settled(t.logFlange) && logDoubler_.settled(t.logDoubler));
        logSafe_.next(t.logSafe, glide, kLanded);
        amount_.next(t.amount, glide, kLanded);
        envAmount_.next(t.envAmount, glide, kLanded);
        lfo2Mix_.next(t.lfo2Mix, glide, kLanded);
        duty_.next(t.duty, glide, kLanded);

        // LFO 1: Ableton's shapes, bent by Duty; a new waveform crossfades as a jump does.
        double rate1 = 0.0;
        const bool jump1 = stepLfo(lfo1_, ctx, t.sync, t.rate, t.freq, endFrame, frames, rate1) || t.wave != wave_;
        wave_ = t.wave;
        const double duty = duty_.value;
        if (t.wave == Wave::TriangleAnalog) {
            // Its shape follows the rate: glided (in log), so a jump of Freq doesn't jump its level.
            const double logRate = std::log2(std::max(rate1, 1e-3));
            if (primed_) {
                analogRate_.next(logRate, glide, kLanded);
            } else {
                analogRate_.snap(logRate);
            }
            if (analogRate_.value != analogShapeRate_ || duty != analogShapeDuty_) {
                analog_ = phaser::analogShape(duty, std::exp2(analogRate_.value));
                analogShapeRate_ = analogRate_.value;
                analogShapeDuty_ = duty;
            }
        } else {
            analogRate_.snap(std::log2(std::max(rate1, 1e-3)));
        }
        setValues(lfo1_, jump1, n, frames, [&](double phase, uint32_t cycle) {
            if (t.wave == Wave::TriangleAnalog) return phaser::analogValue(analog_, phase);  // (its shape kept)
            return static_cast<double>(phaser::waveValue(t.wave, phase, cycle, duty, rate1));
        });
        // LFO 2: a triangle.
        double rate2 = 0.0;
        const bool jump2 = stepLfo(lfo2_, ctx, t.sync2, t.rate2, t.freq2, endFrame, frames, rate2);
        setValues(lfo2_, jump2, n, frames, [](double phase, uint32_t cycle) {
            return static_cast<double>(dsp::Lfo::shape(dsp::LfoShape::Triangle, phase, cycle));
        });

        envNow_ = envRunning_ ? phaser::envelopeAmount(env_.value()) : 0.0;

        // Each channel's modulation: the LFOs' blend and the envelope at the chunk's end, drawn in a
        // straight line from the last chunk end's and smoothed sample by sample (two one-poles of a
        // millisecond), so what the sweep follows is smooth rather than drawn in 16-sample pieces (a
        // long delay would turn the pieces' corners into a zipper as it swept).
        const double mix2 = lfo2Mix_.value, amount = amount_.value, envAmount = envAmount_.value;
        const double flangeMs = flangeMsNow(), doublerMs = doublerMsNow();
        const double perFrame = 1.0 / frames;
        for (int c = 0; c < n; ++c) {
            const double lfo = (1.0 - mix2) * lfo1_.value[c] + mix2 * lfo2_.value[c];
            const double raw = std::clamp(amount * lfo + envAmount * envNow_, -phaser::kModLimit, phaser::kModLimit);
            if (!primed_) {
                rawFrom_[c] = raw;
                mod_[c].snap(raw);
            }
            const double from = rawFrom_[c];
            Ease& mod = mod_[c];
            const bool moving = !(from == raw && mod.settled(raw));
            if (moving) {
                const double k = modGlide_, step = (raw - from) * perFrame;
                double first = mod.first, value = mod.value;
                for (int i = 0; i < frames; ++i) {
                    first += k * (from + (i + 1) * step - first);
                    value += k * (first - value);
                    modAt_[c][i] = value;
                }
                mod.first = first;
                mod.value = value;
                if (from == raw && std::abs(raw - first) < 1e-9 && std::abs(raw - value) < 1e-9) mod.snap(raw);
            } else if (stageMoving_ || timeMoving_) {
                std::fill_n(modAt_[c], frames, mod.value);  // (what the glides' sample-by-sample work reads)
            }
            rawFrom_[c] = raw;
            // The chunk end's stages and delays, unless nothing they come from has moved.
            const double m = mod.value;
            const bool changed = moving || stale_;
            modMoving_[c] = moving;
            if (!stageMoving_ && changed) setStage(c, m);
            if (!timeMoving_ && changed) {
                delayTo_[0][c] = phaser::delaySamples(phaser::flangerMsAt(flangeMs, m), sampleRate_);
                delayTo_[1][c] = phaser::delaySamples(phaser::doublerMsAt(doublerMs, m), sampleRate_);
            }
        }
        stale_ = false;
        // Safe Bass's crossover: its frequency's tan at the chunk end, moved to sample by sample
        // (stepping its coefficients a chunk at a time would step what its states put out).
        safeFrom_ = safeTo_;
        if (safeRunning_ && logSafe_.value != safeLog_) {
            safeLog_ = logSafe_.value;
            safeTo_ = dsp::CrossoverCoefficients::at(std::exp2(safeLog_), sampleRate_).svf;
            if (safeFresh_) safeFrom_ = safeTo_;
        }
        safeFresh_ = false;
        if (!primed_) {  // the first chunk after a reset: nothing to move from
            safeFrom_ = safeTo_;
            for (int c = 0; c < kChannels; ++c) stageFrom_[c] = stageTo_[c];
            primed_ = true;
        }
    }

    // The delay times now: as set once their glides have landed (exactly: no round trip through
    // log2 and exp2), else where the glides are.
    double flangeMsNow() const noexcept {
        return logFlange_.settled(targets_.logFlange) ? targets_.flangeMs : std::exp2(logFlange_.value);
    }
    double doublerMsNow() const noexcept {
        return logDoubler_.settled(targets_.logDoubler) ? targets_.doublerMs : std::exp2(logDoubler_.value);
    }

    // Channel c's stages for its modulation `m`, at where Center, Spread and Blend are.
    void setStage(int c, double m) noexcept {
        center_[c] = phaser::centerHzAt(logCenter_.value, blend_.value, m, sampleRate_);
        q_[c] = phaser::phaserQ(spread_.value, blend_.value, m);
        stageTo_[c] = disperser::design(center_[c], q_[c], sampleRate_);
    }

    // Moves an LFO on to the chunk's end, free (at its rate) or, synced while
    // the song plays, to the song's position; and its right side's offset (Spin's
    // faster cycles, or gliding to Phase the short way round). True where its
    // phase jumped: it started following the song, its division changed, or the
    // song's position moved other than by the chunk (a loop, a locate).
    bool stepLfo(LfoRun& r, const ProcessContext& ctx, bool sync, int division, double freeHz, int endFrame,
                 int frames, double& rateHz) noexcept {
        const Targets& t = targets_;
        const bool synced = sync && ctx.playing && ctx.tempo > 0.0;
        bool jump = false;
        if (synced) {
            const double samplesPerBeat = ctx.samplesPerBeat();
            const double beats = dsp::syncedCycleBeats(division);
            const double position = std::max(0.0, (ctx.beatPos + endFrame / samplesPerBeat) / beats);
            const double cycles = frames / samplesPerBeat / beats;
            jump = !r.synced || division != r.division || std::abs(position - (r.position + cycles)) > 1e-6;
            r.lfo.reset();
            r.lfo.advance(position);
            if (t.spinOn) r.offset = jump ? t.spin * position : r.offset + t.spin * (position - r.position);
            r.position = position;
            r.division = division;
            rateHz = ctx.tempo / 60.0 / beats;
        } else {
            // Free, or synced with the song stopped (at the tempo's rate): it carries on from where it was.
            rateHz = sync ? phaser::syncedRateHz(division, ctx.tempo) : freeHz;
            const double cycles = rateHz * frames / sampleRate_;
            r.lfo.advance(cycles);
            if (t.spinOn) r.offset += t.spin * cycles;
        }
        r.synced = synced;
        if (!t.spinOn) {
            double d = t.phaseCycles - r.offset;
            d -= std::floor(d + 0.5);  // the short way round: -0.5..0.5 cycles
            r.offset += std::abs(d) < 1e-9 ? d : offsetGlide_[frames] * d;
        }
        return jump;
    }

    // An LFO's values at the chunk end, the right channel's at its offset; at a
    // jump, a crossfade from the values reached to the new ones.
    template <typename Shape>
    void setValues(LfoRun& r, bool jump, int n, int frames, Shape&& shape) noexcept {
        double v[kChannels] = {};
        v[0] = shape(r.lfo.phase(), r.lfo.cycle());
        if (n > 1) {
            const double at = r.lfo.phase() + r.offset;
            const double whole = std::floor(at);
            v[1] = shape(at - whole, r.lfo.cycle() + static_cast<uint32_t>(static_cast<int64_t>(whole)));
        }
        if (jump && r.valued) {
            r.fading = true;
            r.fadeAt = 0;
            for (int c = 0; c < kChannels; ++c) r.from[c] = r.value[c];
        }
        if (r.fading) {
            r.fadeAt += frames;
            if (r.fadeAt >= lfoFadeLength_) {
                r.fading = false;
            } else {
                const double s = sCurve(static_cast<double>(r.fadeAt) / lfoFadeLength_);
                for (int c = 0; c < n; ++c) v[c] = r.from[c] + s * (v[c] - r.from[c]);
            }
        }
        for (int c = 0; c < n; ++c) r.value[c] = v[c];
        if (n == 1) r.value[1] = v[0];
        r.valued = true;
    }

    // --- Per sample -----------------------------------------------------------------------

    // A chunk, in runs between the ends of the fades.
    template <int N>
    void renderChunk(float* const* ch, int offset, int frames) noexcept {
        for (int i = 0; i < frames;) {
            int end = frames;
            if (modeFading_) end = std::min(end, i + modeFadeLength_ - modeAt_);
            if (notchFading_) end = std::min(end, i + notchFadeLength_ - notchAt_);
            renderFrames<N>(ch, offset, frames, i, end);
            i = end;
            if (modeFading_ && modeAt_ >= modeFadeLength_) endModeFade();
            if (notchFading_ && notchAt_ >= notchFadeLength_) endNotchFade();
        }
    }

    // Stages [from, to) on N channels in step (independent chains, which the CPU overlaps).
    template <int N>
    void runStages(const disperser::Stage* stage, int from, int to, double* y) noexcept {
        for (int s = from; s < to; ++s) {
            for (int c = 0; c < N; ++c) y[c] = disperser::process(stage[c], stages_[s][c], y[c]);
        }
    }

    // Frames [from, to) of a chunk `frames` long, no fade starting or ending among them.
    template <int N>
    void renderFrames(float* const* ch, int offset, int frames, int from, int to) noexcept {
        const Targets& tg = targets_;
        const Mode outgoing = modeFading_ ? modeFrom_ : mode_;
        const Mode incoming = modeFading_ ? modeTo_ : mode_;
        const bool modeFade = modeFading_;
        const bool phaserRuns = outgoing == Mode::Phaser || incoming == Mode::Phaser;
        const bool delayRuns = outgoing != Mode::Phaser || incoming != Mode::Phaser;
        const bool phaserIn = modeFade && incoming == Mode::Phaser;
        const bool phaserOut = modeFade && outgoing == Mode::Phaser;
        const bool twoReads = modeFade && !phaserRuns;  // Flanger <-> Doubler
        const int readA = delayIndex(outgoing != Mode::Phaser ? outgoing : incoming);
        const int readB = delayIndex(incoming);
        const bool notchFade = notchFading_;
        const int shortRun = notchFade ? std::min(notchFrom_, notchTo_) : notches_;
        const int longRun = notchFade ? std::max(notchFrom_, notchTo_) : notches_;
        const bool adding = notchFade && notchTo_ > notchFrom_;
        const bool warm = warmRunning_;
        const bool safe = safeRunning_;
        const int safeTarget = tg.safeOn ? safeFadeLength_ : 0;
        const bool safeGliding = safe && safeFrom_.a2 != safeTo_.a2;
        const float safeFromG = safeFrom_.a2 / safeFrom_.a1, safeToG = safeTo_.a2 / safeTo_.a1;  // (a2 = g a1)
        dsp::SvfCoefficients safeCoeffs = safeTo_;
        const bool gainsMove = !(gain_.settled(tg.feedback) && warmth_.settled(tg.warmth) && mix_.settled(tg.mix) &&
                                 out_.settled(tg.outGain));
        const bool timeMoving = timeMoving_, stageMoving = stageMoving_;
        const double flangeMsChunk = flangeMsNow(), doublerMsChunk = doublerMsNow();
        const double perFrame = 1.0 / frames;
        const double perModeFade = 1.0 / modeFadeLength_, perNotchFade = 1.0 / notchFadeLength_;
        const double perSafeFade = 1.0 / safeFadeLength_;
        const float warmCoeff = warmCoeff_;
        int modeAt = modeAt_, notchAt = notchAt_, safeAt = safeAt_;
        float outPeak = outPeak_;

        for (int i = from; i < to; ++i) {
            const double t = (i + 1) * perFrame;
            if (gainsMove) {
                gain_.next(tg.feedback, gainGlide_, kGainLanded);
                warmth_.next(tg.warmth, gainGlide_, kGainLanded);
                mix_.next(tg.mix, gainGlide_, kGainLanded);
                out_.next(tg.outGain, gainGlide_, kGainLanded);
            }
            const double g = gain_.value, w = warmth_.value, mix = mix_.value, outGain = out_.value;
            const double share = modeFade ? sCurve((modeAt + 1) * perModeFade) : 1.0;  // the incoming mode's
            // The glides of the stages' and the delays' controls, in every mode (so a mode coming in finds
            // them where they would be in it).
            if (stageMoving) {
                logCenter_.next(tg.logCenter, stageGlide_, kLanded);
                spread_.next(tg.spread, stageGlide_, kLanded);
                blend_.next(tg.blend, stageGlide_, kLanded);
            }
            if (timeMoving) {
                logFlange_.next(tg.logFlange, timeGlide_, kTimeLanded);
                logDoubler_.next(tg.logDoubler, timeGlide_, kTimeLanded);
            }

            // Safe Bass: the lows split off and kept dry, faded in and out by s.
            double x[N], dry[N], coreIn[N], low[N];
            double s = 0.0;
            if (safe) {
                safeAt += safeAt < safeTarget ? 1 : (safeAt > safeTarget ? -1 : 0);
                s = sCurve(safeAt * perSafeFade);
                if (safeGliding) {
                    const auto g = static_cast<float>(safeFromG + t * (safeToG - safeFromG));
                    safeCoeffs = dsp::SvfCoefficients(g, safeTo_.k);
                }
            }
            for (int c = 0; c < N; ++c) {
                x[c] = ch[c][offset + i];
                if (safe) {
                    float lo = 0.f, hi = 0.f;
                    splits_[c].process(dsp::CrossoverCoefficients{safeCoeffs}, static_cast<float>(x[c]), lo, hi);
                    low[c] = lo;
                    dry[c] = x[c] + s * (lo + hi - x[c]);
                    coreIn[c] = x[c] + s * (hi - x[c]);
                } else {
                    low[c] = 0.0;
                    dry[c] = coreIn[c] = x[c];
                }
            }

            // The Phaser: the cascade, its feedback a sample late.
            double wetPhaser[N] = {};
            if (phaserRuns) {
                disperser::Stage stage[N];
                double y[N];
                const double inFade = phaserIn ? share : 1.0;
                for (int c = 0; c < N; ++c) {
                    // The stages: moving linearly across the chunk with the modulation, or, while
                    // Center, Spread or Blend glides, designed sample by sample (as the Disperser's).
                    if (stageMoving) {
                        const double m = modAt_[c][i];
                        stage[c] = disperser::design(phaser::centerHzAt(logCenter_.value, blend_.value, m, sampleRate_),
                                                     phaser::phaserQ(spread_.value, blend_.value, m), sampleRate_);
                    } else {
                        stage[c] = towards(stageFrom_[c], stageTo_[c], t);
                    }
                    y[c] = coreIn[c] * inFade + g * feedback_[c];
                }
                if (!notchFade) {
                    runStages<N>(stage, 0, longRun, y);
                } else {
                    // Fewer: from the longer cascade's output to the tap after the stages kept. More: the
                    // stages added hear their input fade in, while the old output fades out.
                    const double toShare = sCurve((notchAt + 1) * perNotchFade);
                    double tap[N];
                    runStages<N>(stage, 0, shortRun, y);
                    for (int c = 0; c < N; ++c) {
                        tap[c] = y[c];
                        if (adding) y[c] *= toShare;
                    }
                    runStages<N>(stage, shortRun, longRun, y);
                    for (int c = 0; c < N; ++c) {
                        y[c] = adding ? (1.0 - toShare) * tap[c] + y[c] : (1.0 - toShare) * y[c] + toShare * tap[c];
                    }
                }
                for (int c = 0; c < N; ++c) {
                    double v = std::abs(y[c]) < 1e-20 ? 0.0 : y[c];  // (its float never denormal)
                    if (warm) v = warmed(v, w, warmPhaser_[c], warmCoeff);
                    feedback_[c] = safety(v);
                    wetPhaser[c] = v;
                }
            }

            // The Flanger and the Doubler: the line read at the delay (two reads crossfading between
            // them), written with the input and the feedback; in Phaser mode with the input alone.
            double wetDelay[N] = {};
            if (delayRuns) {
                const double delayGain = phaserOut ? share * g : (phaserIn ? (1.0 - share) * g : g);
                const double flangeMs = timeMoving ? flangeMsNow() : flangeMsChunk;
                const double doublerMs = timeMoving ? doublerMsNow() : doublerMsChunk;
                for (int c = 0; c < N; ++c) {
                    // The delays, sample by sample from the smoothed modulation and the times' glides
                    // (as they stand still, the chunk end's).
                    const auto delay = [&](int kind) {
                        if (!timeMoving && !modMoving_[c]) return delayTo_[kind][c];
                        const double m = modAt_[c][i];
                        return phaser::delaySamples(kind == 0 ? phaser::flangerMsAt(flangeMs, m)
                                                              : phaser::doublerMsAt(doublerMs, m),
                                                    sampleRate_);
                    };
                    double y = readLine(lines_[c], delay(readA));
                    if (twoReads) y = (1.0 - share) * y + share * readLine(lines_[c], delay(readB));
                    const double v = warm ? warmed(y, w, warmDelay_[c], warmCoeff) : y;
                    wetDelay[c] = v;
                    lines_[c].push(flushTinyFloat(static_cast<float>(coreIn[c] + delayGain * safety(v))));
                }
            } else {
                for (int c = 0; c < N; ++c) lines_[c].push(flushTinyFloat(static_cast<float>(coreIn[c])));
            }

            for (int c = 0; c < N; ++c) {
                double wet;
                if (phaserOut) {
                    wet = (1.0 - share) * wetPhaser[c] + share * wetDelay[c];
                } else if (phaserIn) {
                    wet = (1.0 - share) * wetDelay[c] + share * wetPhaser[c];
                } else {
                    wet = phaserRuns ? wetPhaser[c] : wetDelay[c];
                }
                const auto out = static_cast<float>(outGain * ((1.0 - mix) * dry[c] + mix * (s * low[c] + wet)));
                ch[c][offset + i] = out;
                outPeak = std::max(outPeak, std::abs(out));
            }
            if (modeFade) ++modeAt;
            if (notchFade) ++notchAt;
        }
        modeAt_ = modeAt;
        notchAt_ = notchAt;
        safeAt_ = safeAt;
        outPeak_ = outPeak;
    }

    void endModeFade() noexcept {
        modeFading_ = false;
        mode_ = modeTo_;
        if (mode_ != Mode::Phaser) {  // the cascade goes idle, silent (a waiting change of Notches lands)
            clearCascade();
            if (notchFading_) {
                notchFading_ = false;
                notches_ = notchTo_;
            }
        } else {
            for (dsp::OnePole& pole : warmDelay_) pole.reset();  // the line goes on being written
        }
    }

    void endNotchFade() noexcept {
        notchFading_ = false;
        const int from = notchFrom_;
        notches_ = notchTo_;
        clearStages(notches_, from);  // no longer heard: silent when next added
    }

    // After a chunk: its end's coefficients are the next one's start; flushes; the displays.
    void endChunk(int n, int frames) noexcept {
        if (stageMoving_ || timeMoving_) stale_ = true;  // (the next chunk works out where they landed)
        if (stageMoving_) {
            for (int c = 0; c < n; ++c) setStage(c, mod_[c].value);
        }
        if (timeMoving_) {
            const double flangeMs = flangeMsNow(), doublerMs = doublerMsNow();
            for (int c = 0; c < n; ++c) {
                delayTo_[0][c] = phaser::delaySamples(phaser::flangerMsAt(flangeMs, mod_[c].value), sampleRate_);
                delayTo_[1][c] = phaser::delaySamples(phaser::doublerMsAt(doublerMs, mod_[c].value), sampleRate_);
            }
        }
        for (int c = 0; c < kChannels; ++c) stageFrom_[c] = stageTo_[c];
        flushStates();
        displayCount_ += frames;
        if (displayCount_ < phaser::kDisplaySamples) return;
        displayCount_ -= phaser::kDisplaySamples;
        const double phase = lfo1_.lfo.phase();
        double phaseRight = phase;
        if (n > 1) {
            const double at = phase + lfo1_.offset;
            phaseRight = at - std::floor(at);
        }
        publish(PhaseDisplay, static_cast<float>(phase));
        publish(PhaseRightDisplay, static_cast<float>(phaseRight));
        publish(LfoDisplay, static_cast<float>(lfo1_.value[0]));
        publish(ModDisplay, static_cast<float>(mod_[0].value));
        publish(EnvDisplay, targets_.envOn ? static_cast<float>(envNow_) : 0.f);
        const Mode shown = modeFading_ ? modeTo_ : mode_;  // during a fade, the incoming mode's
        for (int c = 0; c < kChannels; ++c) {
            const int from = std::min(c, n - 1);
            const double sweep = shown == Mode::Phaser ? center_[from]
                                                       : delayTo_[delayIndex(shown)][from] * 1000.0 / sampleRate_;
            publish(c == 0 ? SweepLeft : SweepRight, static_cast<float>(sweep));
        }
        for (int c = 0; c < kChannels; ++c) {
            const int from = std::min(c, n - 1);
            publish(c == 0 ? QLeft : QRight, shown == Mode::Phaser ? static_cast<float>(q_[from]) : 0.f);
        }
        publish(InputLevel, std::max(-90.f, gainToDb(inPeak_)));
        publish(OutputLevel, std::max(-90.f, gainToDb(outPeak_)));
        inPeak_ = outPeak_ = 0.f;
    }

    // --- State ----------------------------------------------------------------------------

    void clearStages(int from, int to) noexcept {
        for (int s = std::max(0, from); s < std::min(to, kMaxNotches); ++s) {
            for (disperser::State& state : stages_[s]) state = disperser::State{};
        }
    }
    void clearCascade() noexcept {
        clearStages(0, kMaxNotches);
        for (double& fb : feedback_) fb = 0.0;
        for (dsp::OnePole& pole : warmPhaser_) pole.reset();
    }
    // Every audio state silent (the LFOs and glides go on).
    void clearAudio() noexcept {
        clearCascade();
        for (dsp::DelayLine& line : lines_) line.reset();
        for (dsp::OnePole& pole : warmDelay_) pole.reset();
        for (dsp::Crossover& split : splits_) split.reset();
    }

    // What has died away, to zero: within a chunk none of it can reach the denormal range.
    void flushStates() noexcept {
        const int used = std::max(notches_, notchFading_ ? std::max(notchFrom_, notchTo_) : 0);
        for (int s = 0; s < used; ++s) {
            for (disperser::State& state : stages_[s]) {
                state.d1 = dsp::flushTiny(state.d1);
                state.d2 = dsp::flushTiny(state.d2);
            }
        }
        for (int c = 0; c < kChannels; ++c) {
            feedback_[c] = dsp::flushTiny(feedback_[c]);
            warmPhaser_[c].z = flushTinyFloat(warmPhaser_[c].z);
            warmDelay_[c].z = flushTinyFloat(warmDelay_[c].z);
            for (dsp::Svf* svf : {&splits_[c].split, &splits_[c].low, &splits_[c].high}) {
                svf->ic1 = flushTinyFloat(svf->ic1);
                svf->ic2 = flushTinyFloat(svf->ic2);
            }
        }
    }

    static const std::vector<ParamInfo>& infos() {
        static const std::vector<ParamInfo> kInfos = [] {
            const std::vector<std::string>& divisions = dsp::syncedDivisionLabels();
            const auto indexOf = [&divisions](const char* label, int fallback) {
                const auto at = std::find(divisions.begin(), divisions.end(), label);
                return static_cast<float>(at == divisions.end() ? fallback : at - divisions.begin());
            };
            const auto lastDivision = static_cast<float>(divisions.size() - 1);
            const auto& offOn = offOnLabels();
            return std::vector<ParamInfo>{
                {"mode", "Mode", "", 0.f, 2.f, 0.f, false, phaser::modeLabels()},
                {"notches", "Notches", "#", 1.f, static_cast<float>(kMaxNotches), 4.f, false, {}, kMaxNotches - 1},
                {"center", "Center", "Hz", static_cast<float>(phaser::kMinCenter),
                 static_cast<float>(phaser::kMaxCenter), 1000.f, true},
                {"spread", "Spread", "%", 0.f, 100.f, 50.f},
                {"blend", "Blend", "", 0.f, 1.f, 0.f},
                {"flange_time", "Flanger Time", "ms", 0.1f, 20.f, 2.5f, true},
                {"doubler_time", "Doubler Time", "ms", 20.f, 150.f, 30.f, true},
                {"amount", "Amount", "%", 0.f, 100.f, 50.f},
                {"feedback", "Feedback", "%", 0.f, 100.f, 50.f},
                {"fb_invert", "Feedback Invert", "", 0.f, 1.f, 0.f, false, offOn},
                {"sync", "LFO Sync", "", 0.f, 1.f, 0.f, false, offOn},
                {"freq", "LFO Freq", "Hz", static_cast<float>(phaser::kMinRate),
                 static_cast<float>(phaser::kMaxRate), 0.5f, true},
                {"rate", "LFO Rate", "", 0.f, lastDivision, indexOf("1 Bar", 15), false, divisions},
                {"wave", "LFO Waveform", "", 0.f, 9.f, 1.f, false, phaser::waveLabels()},
                {"duty", "Duty Cycle", "%", -100.f, 100.f, 0.f},
                {"spin_on", "Spin On", "", 0.f, 1.f, 0.f, false, offOn},
                {"phase", "Phase", "°", 0.f, 360.f, 180.f},
                {"spin", "Spin", "%", 0.f, 100.f, 10.f},
                {"lfo2_mix", "LFO 2 Mix", "%", 0.f, 100.f, 0.f},
                {"sync2", "LFO 2 Sync", "", 0.f, 1.f, 0.f, false, offOn},
                {"freq2", "LFO 2 Freq", "Hz", static_cast<float>(phaser::kMinRate),
                 static_cast<float>(phaser::kMaxRate), 2.f, true},
                {"rate2", "LFO 2 Rate", "", 0.f, lastDivision, indexOf("1/4", 10), false, divisions},
                {"env_on", "Env Follow", "", 0.f, 1.f, 0.f, false, offOn},
                {"env_amount", "Env Amount", "%", -100.f, 100.f, 50.f},
                {"env_attack", "Env Attack", "ms", 0.1f, 300.f, 10.f, true},
                {"env_release", "Env Release", "ms", 1.f, 3000.f, 200.f, true},
                {"safe_bass", "Safe Bass", "Hz", static_cast<float>(phaser::kSafeBassOff), 3000.f,
                 static_cast<float>(phaser::kSafeBassOff), true},
                {"warmth", "Warmth", "%", 0.f, 100.f, 0.f},
                {"output", "Output", "dB", -24.f, 24.f, 0.f},
                {"mix", "Dry/Wet", "%", 0.f, 100.f, 50.f},
            };
        }();
        return kInfos;
    }

    static std::vector<DisplayInfo> displayInfos() {
        std::vector<DisplayInfo> out;
        for (const char* id : {"phase", "phase_r", "lfo", "mod", "env", "sweep_l", "sweep_r", "q_l", "q_r", "input",
                               "output"}) {
            out.push_back({id, phaser::kDisplaySamples});
        }
        return out;
    }

    double sampleRate_ = 48000.0;
    int channels_ = kChannels;

    // Glide coefficients for a chunk of 0..kChunk frames (the shorter ones at a stretch's end), and per sample.
    std::array<double, kChunk + 1> glide_{}, offsetGlide_{};
    double timeGlide_ = 4e-4, stageGlide_ = 1e-3, modGlide_ = 0.02;
    double gainGlide_ = 0.004;
    int lfoFadeLength_ = 960, notchFadeLength_ = 960, modeFadeLength_ = 1440, safeFadeLength_ = 960;
    float warmCoeff_ = 0.52f;

    Targets targets_;
    // The continuous controls' glides: per chunk, and per sample the gains', the stages' controls'
    // (Center in log2 Hz, Spread, Blend: stageMoving_ while they glide) and the delay times' (log2 ms:
    // timeMoving_).
    Ease logSafe_, amount_, envAmount_, lfo2Mix_, duty_;
    Ease gain_, warmth_, mix_, out_, logCenter_, spread_, blend_, logFlange_, logDoubler_;
    bool stageMoving_ = false, timeMoving_ = false;

    LfoRun lfo1_, lfo2_;
    Wave wave_ = Wave::Triangle;
    Ease analogRate_;  // Triangle Analog's rate (log2 Hz), and its shape at the rate and duty it was worked out for
    phaser::AnalogShape analog_;
    double analogShapeRate_ = -1e9, analogShapeDuty_ = -1e9;

    dsp::EnvelopeFollower env_;
    float envAttack_ = -1.f, envRelease_ = -1.f;
    bool envRunning_ = false;
    double envNow_ = 0.0;  // its 0..1 at the last chunk end

    // Each channel's modulation, and what it gives at the chunk ends: the stages, the two delays (samples).
    Ease mod_[kChannels];
    bool primed_ = false;
    double center_[kChannels] = {}, q_[kChannels] = {};
    disperser::Stage stageFrom_[kChannels], stageTo_[kChannels];
    double delayTo_[2][kChannels] = {};
    double rawFrom_[kChannels] = {};        // the modulation before smoothing, at the last chunk end
    double modAt_[kChannels][kChunk] = {};  // the smoothed modulation at each frame of the chunk
    bool modMoving_[kChannels] = {};
    bool stale_ = true;  // the chunk end's stages and delays must be worked out even if the modulation stood still

    // The Phaser's cascade (stage by stage, the channels side by side), its feedback, Warmth's filters.
    disperser::State stages_[kMaxNotches][kChannels] = {};
    double feedback_[kChannels] = {};
    dsp::OnePole warmPhaser_[kChannels], warmDelay_[kChannels];
    bool warmRunning_ = false;
    int notches_ = 4;            // the stages heard (when not fading)
    bool notchFading_ = false;   // from notchFrom_ stages to notchTo_, notchAt_ samples in
    int notchFrom_ = 4, notchTo_ = 4, notchAt_ = 0;

    // The Flanger's and Doubler's lines.
    std::array<dsp::DelayLine, kChannels> lines_;

    Mode mode_ = Mode::Phaser;   // the mode heard (when not fading)
    bool modeFading_ = false;    // from modeFrom_ to modeTo_, modeAt_ samples in
    Mode modeFrom_ = Mode::Phaser, modeTo_ = Mode::Phaser;
    int modeAt_ = 0;

    // Safe Bass: the crossovers, their coefficients at the last chunk's start and end (the end's at
    // safeLog_), and the fade in (safeAt_ of safeFadeLength_).
    dsp::Crossover splits_[kChannels];
    dsp::SvfCoefficients safeFrom_, safeTo_;
    double safeLog_ = -1.0;
    bool safeFresh_ = true;
    bool safeRunning_ = false;
    int safeAt_ = 0;

    int displayCount_ = 0;
    float inPeak_ = 0.f, outPeak_ = 0.f;
};

}  // namespace

SUB_REGISTER_BUILTIN(PhaserProcessor, AudioEffect);

}  // namespace sub
