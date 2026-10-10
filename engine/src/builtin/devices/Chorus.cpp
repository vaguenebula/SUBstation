// Built-in "Chorus-Ensemble" device, after Ableton Live 12's: modulated delays
// added to the sound, from light thickening through flanging to vibrato. Three
// modes: Chorus (one or two delays a side, their time following Amount or held
// at a fixed Time), Ensemble (three a side, their modulation a third of a cycle
// apart) and Vibrato (one a side, its wave from a sine to a triangle, the right
// side Offset behind the left). The voices' delays and how they move are in
// builtin/ChorusDesign.h, which the editor's graph shares.
//
// Per channel (up to two; more pass through untouched), a sample at a time:
//
// - The High-pass splits the input with a Linkwitz-Riley crossover: the highs go
//   into the delays, the lows pass to the wet unmodulated (so a bass stays solid
//   and a fully wet Vibrato keeps its lows). The dry goes through the same
//   crossover (its lows and highs summed: an all-pass, flat in level), so it is
//   in phase with the wet's lows and Dry/Wet blends them without cancelling. Off,
//   the whole input goes in and the dry is the input. The crossover always runs,
//   so switching it on finds it warm.
// - The voices are read before the line is written (an echo of d samples comes
//   d samples later, and feedback adds no sample), 4-point Hermite between
//   samples, and averaged: at Amount 0 the wet is exactly the delayed input.
// - Warmth: a gentle soft-clipping curve with a little bias into the line (a
//   bucket-brigade chip's input stage: echoes recirculating through it saturate
//   and darken instead of growing), anti-aliased through its antiderivative
//   (it sits in the feedback loop, where an oversampler's latency can't go), a
//   one-pole low-pass after the voices, and a 5 Hz DC blocker on the wet while
//   it plays (the bias makes DC). At 0 the wet is untouched.
// - Feedback (Chorus and Ensemble: Vibrato has none, as Live's): the wet,
//   through a second 5 Hz DC blocker (so a DC offset doesn't build up round the
//   loop) and a limiter (the identity within ±1, never beyond ±2), back into
//   the line; Invert flips its sign.
// - Width (Chorus, Ensemble) scales the wet's side; Output its level; Dry/Wet
//   blends it with the dry (fully dry with the High-pass off, the input passes
//   bit for bit).
//
// Smoothing, so no control clicks or zippers:
//
// - The modulation is worked out per chunk of up to 16 samples: Rate (in log),
//   Amount, Shape, Offset, Warmth and the high-pass frequency (in log) glide
//   through two one-poles in a row (a jump eases in and out), solved exactly at
//   the middle and the end of each chunk; each voice's delay is worked out
//   there, and between them it follows the parabola through the chunk's start,
//   middle and end (so its speed, the pitch, has no steps). Warmth's curve and
//   filter and the crossover's coefficients follow their glides the same way,
//   sample by sample. The LFO's phase integrates the rate, so a jump never
//   clicks.
// - Dry/Wet, Output, Width, the feedback's signed gain (Invert ramps it
//   through 0, Vibrato down to it), the high-pass switch and the DC blocker's
//   blend glide per sample, two one-poles in a row each.
// - A change of Mode, Taps or Time cross-fades (30 ms, an S-curve) from the old
//   voices' reading of the lines to the new one's (both read the same lines, so
//   the feedback carries the blend); a change during a fade starts when it is
//   done.
// - After a reset (the device switched on in the middle of a sound, say) the
//   lines are empty: what goes into them fades in over 5 ms (an S-curve), as
//   the renderer fades the device's output in, so each voice's copy of a sound
//   already playing starts as smoothly, one delay later, instead of with a step
//   mid-waveform.
//
// Every recursive state is flushed below 1e-20 after each stretch and the
// feedback is gated below 1e-15, so silence rings out to exact zeros. The
// delay is the effect: latencySamples() is 0. Displays: the LFO's phase and
// the wet's peak, one value per 128 samples each.

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/ChorusDesign.h"
#include "builtin/Dsp.h"
#include "builtin/DspBlocks.h"
#include "rt/RtUtils.h"

namespace sub {
namespace {

using chorus::Layout;

constexpr int kChannels = 2;
constexpr int kMaxVoices = chorus::kMaxVoices;
constexpr int kChunk = chorus::kChunk;                    // samples per update of the modulation
constexpr int kDisplaySamples = chorus::kDisplaySamples;  // samples per display value
constexpr double kFadeSeconds = 0.03;  // Mode, Taps and Time
constexpr double kFreshSeconds = 0.005;  // what goes into the lines after a reset (Renderer::kSwitchFade's length)

// The glides worked out per chunk: each two one-poles in a row of this time constant (s).
constexpr double kRateGlide = 0.01;
constexpr double kAmountGlide = 0.025;
constexpr double kShapeGlide = 0.01;
constexpr double kOffsetGlide = 0.05;
constexpr double kWarmthGlide = 0.01;
constexpr double kHighPassGlide = 0.01;
constexpr double kGlideLanded = 1e-9;  // a glide this close to its target lands on it
// And per sample (dsp::Glide).
constexpr double kGainRamp = 0.006;      // Dry/Wet, Output, Width, the high-pass switch
constexpr double kFeedbackRamp = 0.01;   // the feedback's gain, the DC blocker's blend
constexpr double kRampLanded = 1e-7;

constexpr float kFeedbackGate = 1e-15f;  // fed back below this: nothing (a dying loop drains to zeros)
constexpr float kLevelFloorDb = -90.f;   // the level display's floor (silence)

// A step of a glide: e^(-h / tau) and h / tau, for h samples and a one-pole time constant of tau samples.
struct Step {
    double decay = 0.0, ratio = 1.0;

    static Step of(double samples, double tauSamples) noexcept {
        const double ratio = samples / std::max(1e-9, tauSamples);
        return {std::exp(-ratio), ratio};
    }
};

// Two one-poles in a row gliding to a target, solved exactly in continuous time
// over a step of any length: a jump eases in and out (its speed starts at 0),
// and the glide sampled at a chunk's middle and end lies on one smooth curve. It
// lands on the target exactly once within `landed` of it. (dsp::Glide, the
// per-sample ramps', moves a whole sample at a time: a chunk's half is often a
// fraction of one, 3.5 samples for a chunk of 7, and the glide must land in the
// same place however a block's end cuts the chunks.)
struct Ease {
    double first = 0.0, value = 0.0;

    void snap(double target) noexcept { first = value = target; }
    bool settled(double target) const noexcept { return first == target && value == target; }
    double step(double target, const Step& s, double landed) noexcept {
        if (settled(target)) return value;
        const double f = first - target, v = value - target;
        value = target + (v + f * s.ratio) * s.decay;
        first = target + f * s.decay;
        if (std::abs(first - target) < landed && std::abs(value - target) < landed) snap(target);
        return value;
    }
};

// A value through a chunk on the parabola through its values at the chunk's
// start, middle and end: next() gives it after each sample (forward
// differences; `h` = 1 / the chunk's length).
struct Quad {
    double v = 0.0, d1 = 0.0, d2 = 0.0;

    void start(double v0, double vMid, double v1, double h) noexcept {
        const double b = 4.0 * vMid - 3.0 * v0 - v1, c = 2.0 * (v0 + v1 - 2.0 * vMid);
        v = v0;
        d1 = (b + c * h) * h;
        d2 = 2.0 * c * h * h;
    }
    double next() noexcept {
        v += d1;
        d1 += d2;
        return v;
    }
};

class ChorusProcessor final : public BuiltinProcessor {
public:
    enum Param {
        Mode = 0, Taps, Time, Rate, Amount, Feedback, FeedbackInvert, Width, Offset, Shape, Warmth,
        HighPass, HighPassFreq, Output, Mix,
        NumParams
    };
    enum Display { PhaseDisplay = 0, LevelDisplay };

    ChorusProcessor() : BuiltinProcessor(infos(), {{"phase", kDisplaySamples}, {"level", kDisplaySamples}}) {}

    std::string typeId() const override { return "builtin:chorus"; }
    std::string name() const override { return "Chorus-Ensemble"; }

    // The longest delay (at any Amount) times the repeats until the feedback has
    // taken an echo down 60 dB (none in Vibrato), and 50 ms for the filters; at
    // most 60 s. Fully dry, nothing (or with the high-pass on, the dry's
    // crossover: 50 ms).
    int tailSamples() const override {
        if (param(Mix) <= 0.f) return isOn(HighPass) ? static_cast<int>(0.05 * sampleRate_) : 0;
        const Targets t = targets();
        const double longest = chorus::highestMs(t.layout) * sampleRate_ / 1000.0;
        const double gain = std::abs(t.feedback);
        const double repeats = gain > 0.001 ? std::min(1000.0, -3.0 / std::log10(gain)) : 0.0;
        const double samples = longest * (1.0 + repeats) + 0.05 * sampleRate_;
        return static_cast<int>(std::min(samples, 60.0 * sampleRate_));
    }

    void prepare(double sampleRate, int) override {
        sampleRate_ = sampleRate;
        const int longest = static_cast<int>(std::ceil(chorus::kMaxDelayMs * sampleRate / 1000.0)) + 8;
        for (auto& line : lines_) line.prepare(longest);
        maxRead_ = static_cast<double>(lines_[0].capacity() - 3);
        for (auto& dc : wetDc_) dc.prepare(sampleRate, 5.0);
        for (auto& dc : loopDc_) dc.prepare(sampleRate, 5.0);
        for (int len = 1; len <= kChunk; ++len) {
            const double half = 0.5 * len;  // the glides are solved at a chunk's middle and end
            rateSteps_[len] = Step::of(half, kRateGlide * sampleRate);
            amountSteps_[len] = Step::of(half, kAmountGlide * sampleRate);
            shapeSteps_[len] = Step::of(half, kShapeGlide * sampleRate);
            offsetSteps_[len] = Step::of(half, kOffsetGlide * sampleRate);
            warmthSteps_[len] = Step::of(half, kWarmthGlide * sampleRate);
            highPassSteps_[len] = Step::of(half, kHighPassGlide * sampleRate);
        }
        gainShare_ = 1.0 - onePoleCoefficient(kGainRamp, sampleRate);
        feedbackShare_ = 1.0 - onePoleCoefficient(kFeedbackRamp, sampleRate);
        fadeLength_ = std::max(1, static_cast<int>(std::lround(kFadeSeconds * sampleRate)));
        freshLength_ = std::max(1, static_cast<int>(std::lround(kFreshSeconds * sampleRate)));
        reset();
    }

    // Silent, the LFO back at its start (offline renders repeat exactly), every
    // glide where the parameters are, and what goes into the lines fading in.
    void reset() override {
        clearState();
        lfo_.reset();
        const Targets t = targets();
        logRate_.snap(t.logRate);
        rate_ = std::exp(t.logRate);
        amount_.snap(t.amount);
        shape_.snap(t.shape);
        offset_.snap(t.offset);
        warmth_.snap(t.warmth);
        logHighPass_.snap(t.logHighPass);
        mix_.snap(t.mix);
        gain_.snap(t.gain);
        width_.snap(t.width);
        feedback_.snap(t.feedback);
        highPass_.snap(t.highPass);
        dcBlend_.snap(t.warmth > 0.0 ? 1.0 : 0.0);
        settleWarmth(t.warmth);
        settleHighPass(t.logHighPass);
        current_ = 0;
        fading_ = false;
        fadeAt_ = 0;
        readings_[0].layout = t.layout;
        startDelays(readings_[0]);
        readings_[1] = readings_[0];
        displayCount_ = 0;
        peak_ = 0.f;
    }

protected:
    void render(const ProcessContext&, float* const* ch, int numChannels, int numFrames) override {
        const int n = std::min(numChannels, kChannels);
        if (n <= 0 || numFrames <= 0) return;
        if (n != channels_) {  // a channel that wasn't processed has no history to go on from
            clearState();
            for (Reading& r : readings_) startDelays(r);
            channels_ = n;
        }
        const Targets t = targets();
        for (int done = 0; done < numFrames;) {
            // A chunk never crosses a display value's end, so values come every 128 samples across renders.
            const int len = std::min({kChunk, numFrames - done, kDisplaySamples - displayCount_});
            if (n == 2) {
                renderChunk<2>(ch, done, len, t);
            } else {
                renderChunk<1>(ch, done, len, t);
            }
            done += len;
            displayCount_ += len;
            if (displayCount_ >= kDisplaySamples) {
                const auto phase = static_cast<float>(lfo_.phase());
                publish(PhaseDisplay, phase < 1.f ? phase : 0.f);  // (a phase a hair under 1 rounds to it)
                publish(LevelDisplay, std::max(kLevelFloorDb, gainToDb(peak_)));
                displayCount_ = 0;
                peak_ = 0.f;
            }
        }
        flushStates();
    }

private:
    // The voices of one layout reading the lines: their delays (samples) at the
    // end of the last chunk, and their paths through this one.
    struct Reading {
        Layout layout;
        double delay[kChannels][kMaxVoices] = {};
        Quad path[kChannels][kMaxVoices];
    };

    // What the parameters ask for, as the glides and ramps take them.
    struct Targets {
        Layout layout;
        double logRate = 0.0, amount = 0.0, shape = 0.0, offset = 0.0, warmth = 0.0, logHighPass = 0.0;
        double mix = 0.0, gain = 1.0, width = 1.0, feedback = 0.0, highPass = 0.0;
    };

    // The modulation at a point of a chunk (its middle or end).
    struct Point {
        double phase = 0.0, rate = 0.0, amount = 0.0, shape = 0.0, offset = 0.0, warmth = 0.0, logHighPass = 0.0;
    };

    Layout targetLayout() const noexcept {
        return chorus::layout(choiceIndex(Mode), choiceIndex(Taps), choiceIndex(Time));
    }

    Targets targets() const noexcept {
        Targets t;
        t.layout = targetLayout();
        const bool vibrato = t.layout.mode == chorus::Mode::Vibrato;
        t.logRate = std::log(std::clamp<double>(param(Rate), chorus::kMinRate, chorus::kMaxRate));
        t.amount = std::clamp(param(Amount) / 100.0, 0.0, 1.0);
        t.shape = std::clamp(param(Shape) / 100.0, 0.0, 1.0);
        t.offset = std::clamp(param(Offset) / 360.0, 0.0, 0.5);
        t.warmth = std::clamp(param(Warmth) / 100.0, 0.0, 1.0);
        t.logHighPass = std::log(chorus::highPassFrequency(param(HighPassFreq), sampleRate_));
        t.mix = std::clamp(param(Mix) / 100.0, 0.0, 1.0);
        t.gain = dbToGain(param(Output));
        t.width = vibrato ? 1.0 : std::clamp(param(Width) / 100.0, 0.0, 2.0);
        const double sign = isOn(FeedbackInvert) ? -1.0 : 1.0;
        t.feedback = vibrato ? 0.0 : sign * chorus::kFeedbackScale * std::clamp(param(Feedback) / 100.0, 0.0, 1.0);
        t.highPass = isOn(HighPass) ? 1.0 : 0.0;
        return t;
    }

    double delaySamples(const Layout& l, int channel, int voice, const Point& at) const noexcept {
        const double phase = at.phase + chorus::voicePhase(l, channel, voice, at.offset);
        const double lfo = chorus::lfoValue(l.mode, phase, at.shape);
        return chorus::delayMs(l, at.amount, lfo) * sampleRate_ / 1000.0;
    }

    // A reading's delays where the modulation is now (a layout faded to starts there).
    void startDelays(Reading& r) const noexcept {
        Point now;
        now.phase = lfo_.phase();
        now.amount = amount_.value;
        now.shape = shape_.value;
        now.offset = offset_.value;
        for (int c = 0; c < kChannels; ++c) {
            for (int v = 0; v < chorus::voices(r.layout); ++v) r.delay[c][v] = delaySamples(r.layout, c, v, now);
        }
    }

    // The glides half a chunk on. The phase moves by the rate's mean over the step
    // (from `fromRate`, at its start), counted in `cycles` from the chunk's start.
    Point stepGlides(const Targets& t, int len, double fromRate, double& cycles) noexcept {
        Point p;
        if (!logRate_.settled(t.logRate)) {
            logRate_.step(t.logRate, rateSteps_[len], kGlideLanded);
            rate_ = std::exp(logRate_.value);
        }
        p.rate = rate_;
        cycles += 0.5 * (fromRate + p.rate) * (0.5 * len) / sampleRate_;
        p.phase = lfo_.phase() + cycles;
        p.amount = amount_.step(t.amount, amountSteps_[len], kGlideLanded);
        p.shape = shape_.step(t.shape, shapeSteps_[len], kGlideLanded);
        p.offset = offset_.step(t.offset, offsetSteps_[len], kGlideLanded);
        p.warmth = warmth_.step(t.warmth, warmthSteps_[len], kGlideLanded);
        p.logHighPass = logHighPass_.step(t.logHighPass, highPassSteps_[len], kGlideLanded);
        return p;
    }

    // The warmth's filter while it holds still.
    void settleWarmth(double w) noexcept {
        lowpass_ = static_cast<float>(chorus::warmLowpassCoefficient(w, sampleRate_));
        settledWarmth_ = w;
    }
    void settleHighPass(double logFreq) noexcept {
        crossover_ = dsp::CrossoverCoefficients::at(std::exp(logFreq), sampleRate_);
        settledHighPass_ = logFreq;
    }
    // The crossover's g = tan(pi f / sampleRate), at a frequency in log.
    double crossoverG(double logFreq) const noexcept {
        return std::tan(chorus::kPi * std::exp(logFreq) / sampleRate_);
    }

    // The sum of a reading's voices on a channel, each on its path through the
    // chunk (a delay in samples, at least 2: read before this sample is written).
    float readVoices(Reading& r, int c, int count) const noexcept {
        const dsp::DelayLine& line = lines_[c];
        float sum = 0.f;
        for (int v = 0; v < count; ++v) {
            const double d = std::clamp(r.path[c][v].next(), 2.0, maxRead_);
            const int whole = static_cast<int>(d);
            const auto t = static_cast<float>(d - whole);
            sum += dsp::hermite(line.tap(whole - 2), line.tap(whole - 1), line.tap(whole), line.tap(whole + 1), t);
        }
        return sum;
    }

    template <int N>
    void renderChunk(float* const* ch, int at, int len, const Targets& t) noexcept {
        // A new layout fades in from where the modulation is now (one asked for during a fade waits).
        if (!fading_ && !(t.layout == readings_[current_].layout)) {
            current_ ^= 1;
            readings_[current_].layout = t.layout;
            startDelays(readings_[current_]);
            fading_ = true;
            fadeAt_ = 0;
        }

        // The glides at the chunk's middle and end.
        const double w0 = warmth_.value, hp0 = logHighPass_.value;
        double cycles = 0.0;
        const Point mid = stepGlides(t, len, rate_, cycles);
        const Point end = stepGlides(t, len, mid.rate, cycles);
        const double h = 1.0 / len;

        // Each voice's path through the chunk.
        Reading* heard[2] = {&readings_[current_], fading_ ? &readings_[current_ ^ 1] : nullptr};
        int voices[2] = {chorus::voices(heard[0]->layout), heard[1] ? chorus::voices(heard[1]->layout) : 0};
        for (int r = 0; r < 2; ++r) {
            if (!heard[r]) continue;
            Reading& reading = *heard[r];
            for (int c = 0; c < N; ++c) {
                for (int v = 0; v < voices[r]; ++v) {
                    const double dEnd = delaySamples(reading.layout, c, v, end);
                    reading.path[c][v].start(reading.delay[c][v], delaySamples(reading.layout, c, v, mid), dEnd, h);
                    reading.delay[c][v] = dEnd;
                }
            }
        }
        const float scale[2] = {1.f / static_cast<float>(voices[0]),
                                voices[1] > 0 ? 1.f / static_cast<float>(voices[1]) : 0.f};

        // Warmth's amount and filter, and the crossover, follow their glides sample by sample.
        const bool warmthMoving = !(w0 == mid.warmth && mid.warmth == end.warmth);
        Quad warmthPath, lowpassPath;
        if (warmthMoving) {
            warmthPath.start(w0, mid.warmth, end.warmth, h);
            lowpassPath.start(chorus::warmLowpassCoefficient(w0, sampleRate_),
                              chorus::warmLowpassCoefficient(mid.warmth, sampleRate_),
                              chorus::warmLowpassCoefficient(end.warmth, sampleRate_), h);
        } else if (end.warmth != settledWarmth_) {
            settleWarmth(end.warmth);
        }
        const bool highPassMoving = !(hp0 == mid.logHighPass && mid.logHighPass == end.logHighPass);
        Quad crossoverPath;
        if (highPassMoving) {
            crossoverPath.start(crossoverG(hp0), crossoverG(mid.logHighPass), crossoverG(end.logHighPass), h);
        } else if (end.logHighPass != settledHighPass_) {
            settleHighPass(end.logHighPass);
        }
        // The DC blocker on the wet while the warmth plays (or is still gliding out).
        const double dcTarget = t.warmth > 0.0 || w0 > 0.0 || end.warmth > 0.0 ? 1.0 : 0.0;

        const bool fading = fading_;
        const double fadeLength = fadeLength_;
        double warmth = settledWarmth_;
        float lowpass = lowpass_;
        dsp::CrossoverCoefficients crossover = crossover_;
        float peak = peak_;
        for (int i = at; i < at + len; ++i) {
            const auto mix = static_cast<float>(mix_.next(t.mix, gainShare_, kRampLanded));
            const auto gain = static_cast<float>(gain_.next(t.gain, gainShare_, kRampLanded));
            const auto width = static_cast<float>(width_.next(t.width, gainShare_, kRampLanded));
            const auto highPass = static_cast<float>(highPass_.next(t.highPass, gainShare_, kRampLanded));
            const auto feedback = static_cast<float>(feedback_.next(t.feedback, feedbackShare_, kRampLanded));
            const auto dcBlend = static_cast<float>(dcBlend_.next(dcTarget, feedbackShare_, kRampLanded));
            float fadeIn = 1.f;
            if (fading) {
                fadeIn = static_cast<float>(dsp::sCurve(std::min(1.0, (fadeAt_ + 1) / fadeLength)));
                ++fadeAt_;
            }
            float fresh = 1.f;  // what goes into the lines, after a reset
            if (freshAt_ < freshLength_) {
                fresh = static_cast<float>(dsp::sCurve(static_cast<double>(freshAt_ + 1) / freshLength_));
                ++freshAt_;
            }
            if (warmthMoving) {
                warmth = std::clamp(warmthPath.next(), 0.0, 1.0);
                lowpass = static_cast<float>(std::clamp(lowpassPath.next(), 0.0, 0.999));
            }
            if (highPassMoving) {  // (with the settled coefficients' damping: the crossover's Butterworth k)
                crossover.svf = dsp::SvfCoefficients(static_cast<float>(crossoverPath.next()), crossover_.svf.k);
            }

            float dry[N], wet[N];
            for (int c = 0; c < N; ++c) {
                const float x = ch[c][i];
                float low = 0.f, high = 0.f;
                crossovers_[c].process(crossover, x, low, high);
                // The high-pass on, the dry is the crossover's all-pass (in phase with the wet's lows).
                dry[c] = x + highPass * ((low + high) - x);
                const float lineSource = fresh * ((1.f - highPass) * x + highPass * high);

                float raw = readVoices(*heard[0], c, voices[0]) * scale[0];
                if (fading) raw = fadeIn * raw + (1.f - fadeIn) * (readVoices(*heard[1], c, voices[1]) * scale[1]);
                const float lowpassed = lowpasses_[c].lowpass(raw, lowpass);
                const float wetHere = lowpassed + dcBlend * (wetDc_[c].process(lowpassed) - lowpassed);

                float fedBack = chorus::limitFeedback(feedback * loopDc_[c].process(wetHere));
                if (std::abs(fedBack) < kFeedbackGate) fedBack = 0.f;
                const float lineIn = lineSource + fedBack;
                if (warmth > 0.0) {
                    lines_[c].push(static_cast<float>(warmers_[c].process(lineIn, warmth)));
                } else {
                    warmers_[c].pass(lineIn);
                    lines_[c].push(lineIn);
                }
                wet[c] = wetHere + highPass * low;
            }
            if constexpr (N == 2) {
                if (width != 1.f) {
                    const float m = 0.5f * (wet[0] + wet[1]), s = 0.5f * (wet[0] - wet[1]) * width;
                    wet[0] = m + s;
                    wet[1] = m - s;
                }
            }
            for (int c = 0; c < N; ++c) {
                const float y = gain * wet[c];
                peak = std::max(peak, std::abs(y));
                ch[c][i] = dry[c] + mix * (y - dry[c]);
            }
        }
        peak_ = peak;

        lfo_.advance(cycles);
        if (fading_ && fadeAt_ >= fadeLength_) fading_ = false;
    }

    // Every line and filter silent; what goes into the lines fades in again (they hold none of the sound).
    void clearState() noexcept {
        freshAt_ = 0;
        for (auto& line : lines_) line.reset();
        for (auto& x : crossovers_) x.reset();
        for (auto& lp : lowpasses_) lp.reset();
        for (auto& dc : wetDc_) dc.reset();
        for (auto& dc : loopDc_) dc.reset();
        for (auto& warmer : warmers_) warmer.reset();
    }

    // Flushes what has died away to zero, so silence never runs into denormals (the DC blockers flush their own).
    void flushStates() noexcept {
        for (auto& x : crossovers_) x.flush();
        for (auto& lp : lowpasses_) lp.z = dsp::flushTiny(lp.z);
    }

    static const std::vector<ParamInfo>& infos() {
        static const std::vector<ParamInfo> kInfos = {
            {"mode", "Mode", "", 0.f, 2.f, 0.f, false, {"Chorus", "Ensemble", "Vibrato"}},
            {"taps", "Tap Count", "", 0.f, 1.f, 1.f, false, {"1", "2"}},
            {"time", "Delay Time", "", 0.f, 5.f, 0.f, false, {"Auto", "7 ms", "10 ms", "20 ms", "35 ms", "50 ms"}},
            {"rate", "Rate", "Hz", static_cast<float>(chorus::kMinRate), static_cast<float>(chorus::kMaxRate), 0.8f,
             true},
            {"amount", "Amount", "%", 0.f, 100.f, 50.f},
            {"feedback", "Feedback", "%", 0.f, 100.f, 0.f},
            {"fb_invert", "Feedback Invert", "", 0.f, 1.f, 0.f, false, offOnLabels()},
            {"width", "Width", "%", 0.f, 200.f, 100.f},
            {"offset", "Offset", "\xc2\xb0", 0.f, 180.f, 0.f},  // (a degree sign, in UTF-8)
            {"shape", "Shape", "%", 0.f, 100.f, 0.f},
            {"warmth", "Warmth", "%", 0.f, 100.f, 0.f},
            {"hp", "High-pass", "", 0.f, 1.f, 0.f, false, offOnLabels()},
            {"hp_freq", "High-pass Freq", "Hz", static_cast<float>(chorus::kMinHighPass),
             static_cast<float>(chorus::kMaxHighPass), 100.f, true},
            {"output", "Output", "dB", -36.f, 6.f, 0.f},
            {"mix", "Dry/Wet", "%", 0.f, 100.f, 50.f},
        };
        return kInfos;
    }

    double sampleRate_ = 48000.0;
    int channels_ = kChannels;

    dsp::DelayLine lines_[kChannels];
    double maxRead_ = 2.0;  // the longest delay a line can be read at
    dsp::Crossover crossovers_[kChannels];
    dsp::OnePole lowpasses_[kChannels];  // the warmth's
    dsp::DcBlocker wetDc_[kChannels];    // the warmth's DC, on the wet
    dsp::DcBlocker loopDc_[kChannels];   // DC round the feedback loop
    chorus::WarmStage warmers_[kChannels];  // the warmth's curve, into the lines

    // The modulation, glided per chunk.
    dsp::Lfo lfo_;  // (its phase: the voices' shapes are chorus::lfoValue's)
    Ease logRate_, amount_, shape_, offset_, warmth_, logHighPass_;
    double rate_ = 1.0;  // exp(logRate_)
    std::array<Step, kChunk + 1> rateSteps_{}, amountSteps_{}, shapeSteps_{}, offsetSteps_{}, warmthSteps_{},
        highPassSteps_{};

    // Glided per sample: each pole's share of the way a sample.
    dsp::Glide mix_, gain_, width_, feedback_, highPass_, dcBlend_;
    double gainShare_ = 0.0035, feedbackShare_ = 0.0021;

    // Warmth and its filter, and the crossover, while their glides hold still.
    float lowpass_ = 0.f;
    double settledWarmth_ = 0.0;
    dsp::CrossoverCoefficients crossover_;
    double settledHighPass_ = 0.0;

    // The layout heard (and, fading, the one before it).
    Reading readings_[2];
    int current_ = 0;
    bool fading_ = false;
    int fadeAt_ = 0, fadeLength_ = 1440;
    int freshAt_ = 0, freshLength_ = 240;  // the lines' input fading in after a reset

    int displayCount_ = 0;  // samples into the display value
    float peak_ = 0.f;      // the wet's peak over it
};

}  // namespace

SUB_REGISTER_BUILTIN(ChorusProcessor, AudioEffect);

}  // namespace sub
