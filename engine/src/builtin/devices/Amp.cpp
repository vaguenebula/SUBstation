// Built-in "Amp" device, after Ableton Live's Amp: seven guitar amplifier
// models (Clean, Boost, Blues, Rock, Lead, Heavy, Bass), Gain, Bass, Middle,
// Treble, Presence and Volume as an amp's 0..10 dials, the Output switch (Mono
// or Dual, automated as "Dual Mono") and Dry/Wet. No cabinet: as Live's, it
// sounds bright and fizzy alone, as a real amp's line output does, and wants a
// cabinet or a low-pass after it.
//
// Each model is a voicing of one structure (builtin/AmpDesign.h): an input
// high-pass and a bright shelf at the base rate; then, 4x oversampled, three
// triode stages (V1, V2, the tone stack, V3), Presence, the power tubes' input
// low-pass, a power stage whose supply sags under load, and the output
// transformer; then a DC blocker and the model's output trim (level-matched at
// the defaults).
//
// - Control-rate work happens per chunk of up to 16 samples (kChunk), on a grid
//   counted from the meters' 256-sample windows so a window always ends where a
//   chunk does. At each chunk's start the controls are worked out at its end:
//   the dials glide (one-poles of 20 ms), and every gain, bias and filter
//   coefficient (the one-poles', the tone stack's) ramps linearly across the
//   chunk from where the last one ended (per oversampled sample in the
//   oversampled section), so nothing steps: a coefficient's step would be a step
//   in the signal's slope, which a sine played clean shows. While nothing moves
//   the chunk reuses the last one's controls and runs loops without ramps.
// - A change of model morphs (50 ms, an S-curve) from the voicing as it is to
//   the new one: every number of the voicing moves continuously, and the trim
//   keeps the blend in between as loud as the two models (amp::Transfer works
//   their levels out, one per 16-sample cell of the grid at most: the morph
//   sets off once its first four are known, 1 ms at 48 kHz), where it would
//   swell by up to 6 dB.
// - Mono runs one amp on the sum (half the work); Dual one per channel. The
//   switch crossfades over 20 ms; the second amp starts from rest, its input
//   fading in over 5 ms, and stops once Mono is back.
// - Dry/Wet mixes in the input delayed by the latency (37 samples: the
//   oversampler's 36 and one for the stages' anti-aliasing), so at 0 the output
//   is the input delayed, bit for bit.
// - Every stage's anti-aliasing holds its bias at rest, and every recursive state
//   is flushed when tiny at the chunk grid's points (the supply's envelope every
//   sample), so silence rings out to exact zeros, and blocks of any size play
//   alike, bit for bit. An amp whose input has been silent for a while, whose
//   output has been exact zeros for a while too and whose states are all at
//   rest sleeps (costs nothing, puts out the zeros it would) until its input
//   sounds again. Its supply's sag recovers meanwhile, as it would awake; once
//   it has, a woken amp plays as a freshly reset one.
//
// Seven displays, one value per 256 samples: the input's peak, each stage's
// drive (its peak against its clipping point), the power stage's, the supply's
// sag and the output's peak, all in dB.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "builtin/AmpDesign.h"
#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/Dsp.h"
#include "builtin/DspBlocks.h"
#include "rt/RtUtils.h"

namespace sub {
namespace {

constexpr int kOs = amp::kOversampling;
constexpr int kChunk = 16;                // base-rate samples per chunk of control-rate work
constexpr int kMeterSamples = 256;        // audio per display value (a multiple of kChunk)
constexpr double kSmoothSeconds = 0.02;   // the dials' one-poles
constexpr double kMorphSeconds = 0.05;    // a change of model
constexpr double kFadeSeconds = 0.02;     // the Output switch's crossfade
constexpr double kFadeInSeconds = 0.005;  // the second amp's input, as it starts
constexpr double kMixSeconds = 0.02;      // Dry/Wet's ramp
constexpr int kSleepFrames = 4096;        // silence in before an amp at rest may sleep
constexpr int kQuietOut = 256;            // and out (well past what its down-sampler holds)
constexpr int kLevelPoints = 64;          // a period's samples for the morph's levels (amp::Transfer)
constexpr int kLevelSteps = 16;           // points of the morph its levels are worked out at
constexpr float kFloorDb = -90.f;
// Below this the supply's sag leaves the power stage's gain at exactly 1.0f
// (1 + sag env rounds to 1): the envelope is flushed to 0 there, every sample,
// and gets there seconds sooner than 1e-20 would over a 300 ms release.
constexpr float kSagFlush = 1e-8f;

static_assert(kMeterSamples % kChunk == 0);

inline double sCurve(double t) noexcept { return t * t * (3.0 - 2.0 * t); }
inline float flushTiny(float v) noexcept { return static_cast<float>(dsp::flushTiny(v)); }

// A triode stage's controls for a chunk: gains, the bias and the curve's value
// there (ramped), and its one-poles' coefficients.
struct StageControls {
    double gIn = 1.0, gOut = 1.0, bias = 0.0, offset = 0.0;
    float miller = 0.f, coupling = 0.f;
};

// Everything a chunk of both amps needs, at the chunk's end. While anything
// moves, each value ramps across the chunk from the last chunk's.
struct ChunkControls {
    int frames = 0;        // base-rate samples in the chunk (≤ kChunk)
    bool moving = false;   // anything differs from the last chunk's: run the ramped loops
    bool cellEnd = false;  // it ends one of the grid's cells (not cut short by a block's end): flush there
    float inputHighpass = 0.f, bright = 0.f, brightGain = 1.f;  // base rate
    StageControls stage[3];
    amp::ToneCoefficients tone;
    double makeup = 1.0;                       // the stack's make-up gain (linear)
    float presence = 0.f, presenceGain = 1.f;  // the shelf's coefficient and gain
    float grid = 0.f;                          // the power tubes' input low-pass
    // The power stage (its drive without the sag), and the sag's amount and its
    // attack and release (per base-rate sample).
    double drive = 1.0, powerBias = 0.0, powerOffset = 0.0, powerGOut = 1.0;
    float sag = 0.f, sagAttack = 1.f, sagRelease = 1.f;
    float transformer = 0.f;
    // The output's trim: the voicing's own, and in a morph that times the
    // morph's level compensation (worked out afresh each chunk from the
    // voicing's, never from the last chunk's trim, which already carries one).
    float voiceTrim = 1.f, trim = 1.f;
};

// What a chunk of one amp showed: each stage's peak |q| (V1, V2, V3, power;
// 1.0 is its clipping point) and sag × env at the chunk's end.
struct ChunkMeters {
    float drive[4] = {};
    float sag = 0.f;
};

struct StageState {
    dsp::OnePole miller, coupling;
    amp::Adaa adaa;
};

// One amp.
struct Path {
    dsp::Oversampler oversampler;
    dsp::OnePole inputHighpass, bright;  // base rate
    StageState stage[3];
    amp::ToneState tone;
    dsp::OnePole presence, grid, transformer;
    amp::Adaa power;
    float fifo[2] = {};  // two oversampled samples: with the stages' half samples, one base-rate sample
    float env = 0.f, sagGain = 1.f;
    dsp::DcBlocker dc;

    void reset() noexcept {
        oversampler.reset();
        inputHighpass.reset();
        bright.reset();
        for (StageState& s : stage) {
            s.miller.reset();
            s.coupling.reset();
            s.adaa.reset();
        }
        tone.reset();
        presence.reset();
        grid.reset();
        transformer.reset();
        power.reset();
        fifo[0] = fifo[1] = 0.f;
        env = 0.f;
        sagGain = 1.f;
        dc.reset();
    }
    // Its stages' anti-aliasing as if their inputs had sat at their biases for ever.
    void prime(const ChunkControls& c) noexcept {
        for (int k = 0; k < 3; ++k) stage[k].adaa.prime(c.stage[k].bias);
        power.prime(c.powerBias);
    }
    // Tiny states to zero (at the grid's points, so blocks of any size flush alike):
    // the base-rate filters', and the oversampled section's.
    void flushInput() noexcept {
        inputHighpass.z = flushTiny(inputHighpass.z);
        bright.z = flushTiny(bright.z);
    }
    void flushStages(const ChunkControls& c) noexcept {
        for (int k = 0; k < 3; ++k) {
            StageState& s = stage[k];
            s.miller.z = flushTiny(s.miller.z);
            s.coupling.z = flushTiny(s.coupling.z);
            if (std::abs(s.adaa.x0 - c.stage[k].bias) < 1e-20) s.adaa.prime(c.stage[k].bias);
        }
        tone.flush();
        presence.z = flushTiny(presence.z);
        grid.z = flushTiny(grid.z);
        transformer.z = flushTiny(transformer.z);
        if (std::abs(power.x0 - c.powerBias) < 1e-20) power.prime(c.powerBias);
        fifo[0] = flushTiny(fifo[0]);
        fifo[1] = flushTiny(fifo[1]);
    }
    // Every state the signal passes through at rest (after the flushes): silence in
    // puts out exact zeros. (The supply's sag may still be recovering: it only
    // scales what comes, and goes on recovering while the amp sleeps.)
    bool atRest(const ChunkControls& c) const noexcept {
        if (inputHighpass.z != 0.f || bright.z != 0.f || !tone.atRest() || presence.z != 0.f || grid.z != 0.f ||
            transformer.z != 0.f || fifo[0] != 0.f || fifo[1] != 0.f || power.x0 != c.powerBias)
            return false;
        for (int k = 0; k < 3; ++k) {
            const StageState& s = stage[k];
            if (s.miller.z != 0.f || s.coupling.z != 0.f || s.adaa.x0 != c.stage[k].bias) return false;
        }
        return true;
    }
};

// A stage run over a stretch of oversampled samples: its states and controls
// in locals (registers), its gains and coefficients ramping per sample while Moving.
template <bool Moving>
struct StageRun {
    double gIn, gOut, bias, offset;
    double dIn = 0.0, dOut = 0.0, dBias = 0.0;
    float cm, cc, dcm = 0.f, dcc = 0.f, zm, zc;
    amp::Adaa adaa;
    double peak = 0.0;

    StageRun(const StageState& s, const StageControls& from, const StageControls& to, int n) noexcept
        : gIn(to.gIn), gOut(to.gOut), bias(to.bias), offset(to.offset), cm(to.miller), cc(to.coupling), zm(s.miller.z),
          zc(s.coupling.z), adaa(s.adaa) {
        if constexpr (Moving) {
            const double step = 1.0 / n;
            dIn = (to.gIn - from.gIn) * step;
            dOut = (to.gOut - from.gOut) * step;
            dBias = (to.bias - from.bias) * step;
            dcm = (to.miller - from.miller) / static_cast<float>(n);
            dcc = (to.coupling - from.coupling) / static_cast<float>(n);
            gIn = from.gIn;
            gOut = from.gOut;
            bias = from.bias;
            cm = from.miller;
            cc = from.coupling;
        }
    }
    float operator()(float x) noexcept {
        if constexpr (Moving) {
            gIn += dIn;
            gOut += dOut;
            bias += dBias;
            // The curve where the anti-aliasing averages it, half a step back as the bias
            // moves (its rest value when it doesn't): a stage at rest stays at 0, where a
            // ramped offset would leave steps of DC at the chunks' joins.
            offset = amp::shape(bias - 0.5 * dBias);
            cm += dcm;
            cc += dcc;
        }
        zm = x + cm * (zm - x);  // the Miller low-pass
        const double q = static_cast<double>(zm) * gIn;
        peak = std::max(peak, std::abs(q));
        const auto y = static_cast<float>(gOut * (adaa.process(q + bias) - offset));
        zc = y + cc * (zc - y);  // the coupling high-pass
        return y - zc;
    }
    void store(StageState& s) const noexcept {
        s.miller.z = zm;
        s.coupling.z = zc;
        s.adaa = adaa;
    }
};

class AmpProcessor final : public BuiltinProcessor {
public:
    enum Param { Type = 0, Gain, Bass, Middle, Treble, Presence, Volume, Dual, Mix, NumParams };
    enum Display { InputLevel = 0, Drive1, Drive2, Drive3, PowerDrive, SagDisplay, OutputLevel };
    enum class Mode { Mono, Dual };

    AmpProcessor()
        : BuiltinProcessor(infos(), {{"input", kMeterSamples},
                                     {"drive1", kMeterSamples},
                                     {"drive2", kMeterSamples},
                                     {"drive3", kMeterSamples},
                                     {"power", kMeterSamples},
                                     {"sag", kMeterSamples},
                                     {"output", kMeterSamples}}) {}

    std::string typeId() const override { return "builtin:amp"; }
    std::string name() const override { return "Amp"; }

    // The oversampler's up and down, and one sample for the stages' anti-aliasing
    // (half an oversampled sample each) with the two-sample FIFO: the same at any rate.
    int latencySamples() const override { return latency_; }
    // The slowest pole is the tone stack's: 53 ms at most (the PA stack with Bass
    // and Middle at 10); half a second takes it 80 dB down.
    int tailSamples() const override { return latency_ + static_cast<int>(std::lround(0.5 * sampleRate_)); }

    void prepare(double sampleRate, int maxBlock) override {
        sampleRate_ = sampleRate;
        maxBlock_ = std::max(1, maxBlock);
        for (Path& p : path_) {
            p.oversampler.prepare(maxBlock_, amp::kOversamplingLog2);
            p.oversampler.setFactorLog2(amp::kOversamplingLog2);
            p.dc.prepare(sampleRate, amp::kDcBlockerHz);
        }
        for (dsp::DelayLine& d : dry_) d.prepare(latency_ + 1);
        for (int p = 0; p < 2; ++p) {
            in_[p].assign(static_cast<size_t>(maxBlock_), 0.f);
            wet_[p].assign(static_cast<size_t>(maxBlock_), 0.f);
        }
        share_.assign(static_cast<size_t>(maxBlock_), 0.f);
        const size_t chunks = static_cast<size_t>(maxBlock_ / kChunk + 2);
        controls_.assign(chunks, ChunkControls{});
        for (auto& m : meters_) m.assign(chunks, ChunkMeters{});
        mix_.reset(sampleRate, kMixSeconds);
        fadeLength_ = std::max(1, static_cast<int>(std::lround(kFadeSeconds * sampleRate)));
        fadeInLength_ = std::max(1, static_cast<int>(std::lround(kFadeInSeconds * sampleRate)));
        chunkGlide_ = 1.0 - std::exp(-kChunk / (kSmoothSeconds * sampleRate));
        reset();
    }

    // Silent, the dials and the model where the parameters are, the Output switch as it is.
    void reset() override {
        for (int k = 0; k < 6; ++k) dial_[k] = dialTarget(k);
        model_ = std::clamp(choiceIndex(Type), 0, amp::kModels - 1);
        voice_ = from_ = to_ = amp::voicing(model_);
        morph_ = 1.0;
        morphFrames_ = 0;
        morphMoves_ = false;
        levelComp_ = 0.0;
        levelKnown_ = 0;
        std::fill(std::begin(recentIn_), std::end(recentIn_), 0.f);
        last_ = controlsFor(voice_);
        resetPaths();
        mix_.snapTo(targetMix());
        meterCount_ = 0;
        clearMeters();
    }

protected:
    void render(const ProcessContext&, float* const* ch, int numChannels, int numFrames) override {
        const int n = std::min(numChannels, 2);
        if (n <= 0 || numFrames <= 0) return;
        if (n != channels_) {  // a channel that wasn't processed has no history to go on from
            channels_ = n;
            resetPaths();
        }
        takeSwitches(n);
        mix_.setTarget(targetMix());
        for (int start = 0; start < numFrames; start += maxBlock_) {  // slices: the buffers hold maxBlock_ frames
            const int frames = std::min(maxBlock_, numFrames - start);
            float* part[2] = {ch[0] + start, n == 2 ? ch[1] + start : nullptr};
            renderSlice(part, n, frames);
        }
    }

private:
    void renderSlice(float* const* ch, int n, int frames) {
        const bool fading = fade_ >= 0;
        const bool dualPath = n == 2 && (mode_ == Mode::Dual || fading);
        buildInputs(ch, n, frames);
        start_ = last_;
        int chunks = 0;
        for (int at = 0, phase = meterCount_; at < frames;) {
            const int len = std::min(kChunk - phase % kChunk, frames - at);
            ChunkControls& c = controls_[static_cast<size_t>(chunks++)];
            c = nextControls(len, phase % kChunk == 0);
            c.cellEnd = (phase + len) % kChunk == 0;
            at += len;
            phase = (phase + len) % kMeterSamples;
        }
        runOrSleep(0, frames, chunks);
        if (dualPath) runOrSleep(1, frames, chunks);
        mixOut(ch, n, frames, chunks, dualPath, fading);
        for (int p = 0; p < (dualPath ? 2 : 1); ++p) {
            if (asleep_[p]) continue;
            // Its output exact zeros for a while too: its down-sampler and DC blocker
            // (whose states aren't ours to read) hold nothing more, so sleeping changes
            // nothing at all, wherever a block ends. (Their reset is for certainty.)
            if (quietFrames_[p] >= kSleepFrames && quietOut_[p] >= kQuietOut && path_[p].atRest(last_)) {
                asleep_[p] = true;
                path_[p].oversampler.reset();
                path_[p].dc.reset();
            }
        }
    }

    // --- The switches ------------------------------------------------------------------------

    void takeSwitches(int n) noexcept {
        const int model = std::clamp(choiceIndex(Type), 0, amp::kModels - 1);
        if (model != model_) {  // from the voicing as it is (in a morph, the one in between)
            model_ = model;
            from_ = voice_;
            to_ = amp::voicing(model);
            morph_ = 0.0;
            morphFrames_ = 0;
            levelKnown_ = 0;
            levelSteps_[0] = levelComp_;  // (from the compensation as it is)
            morphMoves_ = false;
        }
        if (n == 2 && fade_ < 0) {  // a change during a fade starts when it is done
            const Mode want = wantedMode();
            if (want != mode_) {
                mode_ = want;
                fade_ = 0;
                if (want == Mode::Dual) startPath(1);
            }
        }
    }

    Mode wantedMode() const noexcept { return channels_ == 2 && choiceIndex(Dual) != 0 ? Mode::Dual : Mode::Mono; }

    // An amp starting from rest (at the biases the last chunk ended on).
    void startPath(int p) noexcept {
        path_[p].reset();
        path_[p].prime(last_);
        asleep_[p] = false;
        quietFrames_[p] = 0;
        quietOut_[p] = 0;
    }

    void resetPaths() noexcept {
        for (int p = 0; p < 2; ++p) startPath(p);
        for (dsp::DelayLine& d : dry_) d.reset();
        mode_ = wantedMode();
        fade_ = -1;
    }

    // --- The controls ------------------------------------------------------------------------

    double dialTarget(int k) const noexcept { return std::clamp<double>(param(Gain + k), 0.0, 10.0); }
    float targetMix() const noexcept { return std::clamp(param(Mix), 0.f, 100.f) / 100.f; }

    // The next chunk's controls (at its end), `len` base-rate samples long; `cellStart`: it
    // starts one of the grid's cells.
    ChunkControls nextControls(int len, bool cellStart) noexcept {
        bool dialsMove = false;
        for (int k = 0; k < 6; ++k) dialsMove = dialsMove || dial_[k] != dialTarget(k);
        const bool morphing = morph_ < 1.0;
        if (morphing && cellStart) morphCell();
        const bool morphMoves = morphing && morphMoves_;
        ChunkControls c = last_;
        c.frames = len;
        c.moving = dialsMove || morphMoves;
        if (!c.moving) return c;  // steady: as the last one
        const double glide = len == kChunk ? chunkGlide_ : 1.0 - std::exp(-len / (kSmoothSeconds * sampleRate_));
        bool toneMoves = false;
        for (int k = 0; k < 6; ++k) {
            const double target = dialTarget(k);
            if (dial_[k] == target) continue;
            dial_[k] += glide * (target - dial_[k]);
            if (std::abs(target - dial_[k]) < 1e-4) dial_[k] = target;
            toneMoves = toneMoves || k == Bass - Gain || k == Middle - Gain || k == Treble - Gain;
        }
        if (morphMoves) {
            morphFrames_ += len;
            morph_ = std::min(1.0, static_cast<double>(morphFrames_) / (kMorphSeconds * sampleRate_));
            voice_ = morph_ >= 1.0 ? to_ : amp::blend(from_, to_, sCurve(morph_));
            voicingControls(voice_, c);
        }
        dialControls(voice_, c, morphMoves || toneMoves);
        if (morphing) c.trim = c.voiceTrim * static_cast<float>(std::pow(10.0, morphLevel() / 20.0));
        last_ = c;
        return c;
    }

    // A grid cell's start in a morph: one more of its levels if the curve needs
    // it by the cell's end, and whether the morph moves through the cell (it
    // waits, still, for the levels its curve needs there). Each level is an
    // amp::Transfer, about twice the work the amp does for a cell: one a cell at
    // most, so no block of a morph costs more than about three times a steady
    // one (the start waits three cells, 1 ms at 48 kHz, for the ends' levels and
    // the first two points'). Decided per cell and the morph's place counted in
    // samples, so a morph sets off and moves at the same samples whatever the
    // blocks.
    void morphCell() noexcept {
        const double end = std::min(1.0, static_cast<double>(morphFrames_ + kChunk) / (kMorphSeconds * sampleRate_));
        const int needed = levelsNeeded(end);
        if (levelKnown_ < needed) workOutLevel();
        morphMoves_ = levelKnown_ >= needed;
    }

    // How many of the morph's levels (the ends', then points 1 to kLevelSteps - 1:
    // levelKnown_ counts them) its curve needs at `at` (0..1).
    static int levelsNeeded(double at) noexcept {
        if (at >= 1.0) return 0;  // (done: the new model's own level)
        const int step = std::min(static_cast<int>(at * kLevelSteps), kLevelSteps - 1);
        return 2 + std::min(step + 2, kLevelSteps - 1);  // (point kLevelSteps is 0)
    }

    // The morph's next level: its start's, then its end's (for a tone at the
    // input's recent peak), then each point's compensation.
    void workOutLevel() noexcept {
        const auto level = [&](const amp::Voicing& v) {
            const amp::Transfer t(v, dial_[0], dial_[1], dial_[2], dial_[3], dial_[4], dial_[5], sampleRate_,
                                  kLevelPoints);
            return 20.0 * std::log10(std::max(t.rms(levelInput_, 0.0), 1e-12));
        };
        const int k = levelKnown_++;
        if (k == 0) {  // from the level as it is (in a morph, the one in between)
            double peak = meterIn_;
            for (const float p : recentIn_) peak = std::max(peak, double(p));
            levelInput_ = std::clamp(peak, 1e-3, 1.0);
            levelFrom_ = level(from_) + levelSteps_[0];
        } else if (k == 1) {
            levelTo_ = level(to_);
        } else {
            const int point = k - 1;
            const double s = sCurve(static_cast<double>(point) / kLevelSteps);
            levelSteps_[point] = (1.0 - s) * levelFrom_ + s * levelTo_ - level(amp::blend(from_, to_, s));
        }
    }

    // How much the blend in a morph is to be turned down (dB, on its trim) to
    // sound as loud as the morph's ends put it, their levels in between: a
    // blend of two voicings clips where neither does, so it can come out up to
    // 6 dB louder than both. The levels are worked out at kLevelSteps + 1 points
    // of the morph as it gets near them (morphCell()) and joined by a smooth
    // curve (Catmull-Rom: straight lines would bend at the points, which the
    // sound would show). While the morph waits for its first ones, it holds the
    // compensation it had.
    double morphLevel() noexcept {
        if (morph_ >= 1.0) return levelComp_ = 0.0;
        if (morphFrames_ == 0) return levelComp_ = levelSteps_[0];
        const double at = morph_ * kLevelSteps;
        const int step = std::min(static_cast<int>(at), kLevelSteps - 1);
        // Mirrored past the ends: the morph's S-curve starts and ends still, so does the level.
        const auto point = [&](int k) { return levelSteps_[k < 0 ? -k : (k > kLevelSteps ? 2 * kLevelSteps - k : k)]; };
        const double p0 = point(step - 1), p1 = point(step), p2 = point(step + 1), p3 = point(step + 2);
        const double t = at - step;
        const double a = p2 - p0, b = 2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3, c = 3.0 * (p1 - p2) + p3 - p0;
        return levelComp_ = p1 + 0.5 * t * (a + t * (b + t * c));
    }

    ChunkControls controlsFor(const amp::Voicing& v) const noexcept {
        ChunkControls c;
        voicingControls(v, c);
        dialControls(v, c, true);
        return c;
    }

    // What the voicing alone sets: filters' coefficients, biases, fixed gains, the sag.
    void voicingControls(const amp::Voicing& v, ChunkControls& c) const noexcept {
        const double rate = sampleRate_, overRate = kOs * sampleRate_;
        c.inputHighpass = dsp::onePoleCutoff(v.inputHighpassHz, rate);
        c.bright = dsp::onePoleCutoff(v.brightHz, rate);
        c.brightGain = static_cast<float>(std::pow(10.0, v.brightDb / 20.0));
        for (int k = 0; k < 3; ++k) {
            const amp::Stage& s = v.stages[k];
            StageControls& sc = c.stage[k];
            sc.gOut = std::pow(10.0, s.headDb / 20.0) / amp::shapeSlope(s.bias);
            sc.bias = s.bias;
            sc.offset = amp::restValue(s.bias);
            sc.miller = dsp::onePoleCutoff(s.lowpassHz, overRate);
            sc.coupling = dsp::onePoleCutoff(s.highpassHz, overRate);
        }
        c.makeup = std::pow(10.0, v.toneMakeupDb / 20.0);
        c.presence = dsp::onePoleCutoff(v.presenceHz, overRate);
        c.grid = dsp::onePoleCutoff(amp::kGridHz, overRate);
        c.powerBias = v.powerBias;
        c.powerOffset = amp::restValue(v.powerBias);
        c.powerGOut = 1.0 / amp::shapeSlope(v.powerBias);
        c.sag = static_cast<float>(v.sag);
        c.sagAttack = static_cast<float>(1.0 - std::exp(-1.0 / (v.sagAttackMs * 1e-3 * rate)));
        c.sagRelease = static_cast<float>(1.0 - std::exp(-1.0 / (v.sagReleaseMs * 1e-3 * rate)));
        c.transformer = dsp::onePoleCutoff(v.transformerHz, overRate);
        c.voiceTrim = c.trim = static_cast<float>(std::pow(10.0, v.trimDb / 20.0));
    }

    // What the dials set with the voicing: the stages' input gains (Gain), the
    // tone stack (Bass, Middle, Treble: made again only when `tone`, it costs
    // the most), Presence's shelf and the power stage's drive (Volume).
    void dialControls(const amp::Voicing& v, ChunkControls& c, bool tone) const noexcept {
        const double g = amp::gainDb(v, dial_[0]);
        for (int k = 0; k < 3; ++k) {
            const amp::Stage& s = v.stages[k];
            const double knob = k == 0 ? v.gainSplit * g : (k == 2 ? (1.0 - v.gainSplit) * g : 0.0);
            c.stage[k].gIn = std::pow(10.0, (s.levelDb + knob - s.headDb) / 20.0);
        }
        if (tone) c.tone = amp::toneStack(v.tone, dial_[1], dial_[2], dial_[3], kOs * sampleRate_);
        c.presenceGain = static_cast<float>(std::pow(10.0, amp::presenceDb(v, dial_[4]) / 20.0));
        c.drive = std::pow(10.0, amp::volumeDb(v, dial_[5]) / 20.0);
    }

    // --- Mono and Dual -----------------------------------------------------------------------

    // The amps' inputs: Mono the sum, Dual each channel, a fade between them
    // (its share per sample in share_, for mixOut()'s crossfade of the right channel).
    void buildInputs(float* const* ch, int n, int frames) noexcept {
        float* a = in_[0].data();
        float* b = in_[1].data();
        if (n == 1) {
            std::copy_n(ch[0], frames, a);
            return;
        }
        const float* l = ch[0];
        const float* r = ch[1];
        if (fade_ < 0) {
            if (mode_ == Mode::Dual) {
                std::copy_n(l, frames, a);
                std::copy_n(r, frames, b);
            } else {
                for (int i = 0; i < frames; ++i) a[i] = 0.5f * (l[i] + r[i]);
            }
            return;
        }
        const bool toDual = mode_ == Mode::Dual;
        for (int i = 0; i < frames; ++i) {
            const float mono = 0.5f * (l[i] + r[i]);
            float s = 1.f, in = 1.f;
            if (fade_ >= 0) {
                s = static_cast<float>(sCurve(static_cast<double>(fade_) / fadeLength_));
                in = static_cast<float>(sCurve(std::min(1.0, static_cast<double>(fade_) / fadeInLength_)));
                if (++fade_ > fadeLength_) fade_ = -1;
            }
            share_[static_cast<size_t>(i)] = s;
            if (toDual) {
                a[i] = (1.f - s) * mono + s * l[i];
                b[i] = r[i] * in;
            } else {  // the second amp goes on with the right channel until it fades out
                a[i] = (1.f - s) * l[i] + s * mono;
                b[i] = r[i];
            }
        }
    }

    // --- An amp ------------------------------------------------------------------------------

    void runOrSleep(int p, int frames, int chunks) noexcept {
        const float* in = in_[p].data();
        int last = frames - 1;
        while (last >= 0 && in[last] == 0.f) --last;
        quietFrames_[p] = last < 0 ? std::min(kSleepFrames, quietFrames_[p] + frames) : frames - 1 - last;
        Path& path = path_[p];
        if (asleep_[p]) {
            if (last < 0) {  // at rest, given silence: the zeros it would put out
                std::fill_n(wet_[p].data(), frames, 0.f);
                for (int k = 0; k < chunks; ++k) {  // the sag recovers over its release, as it would awake
                    const ChunkControls& c = controls_[static_cast<size_t>(k)];
                    for (int i = 0; i < c.frames && path.env > 0.f; ++i) {  // as powerChunk() does in silence
                        path.env += c.sagRelease * (0.f - path.env);
                        if (path.env < kSagFlush) path.env = 0.f;
                    }
                    meters_[p][static_cast<size_t>(k)] = ChunkMeters{};
                    meters_[p][static_cast<size_t>(k)].sag = c.sag * path.env;
                }
                return;
            }
            // Its states are at rest, its biases where the controls are: once the sag
            // has recovered, it plays as a fresh one.
            asleep_[p] = false;
            path.prime(start_);
            path.sagGain = 1.f / (1.f + start_.sag * path.env);
        }
        runPath(path, in, wet_[p].data(), meters_[p].data(), frames, chunks);
        const float* wet = wet_[p].data();
        last = frames - 1;
        while (last >= 0 && wet[last] == 0.f) --last;
        quietOut_[p] = last < 0 ? std::min(kQuietOut, quietOut_[p] + frames) : frames - 1 - last;
    }

    void runPath(Path& p, const float* in, float* wet, ChunkMeters* meters, int frames, int chunks) noexcept {
        // Base rate: the input's coupling high-pass and the bright shelf.
        const ChunkControls* from = &start_;
        for (int k = 0, at = 0; k < chunks; ++k) {
            const ChunkControls& c = controls_[static_cast<size_t>(k)];
            float hp = c.inputHighpass, br = c.bright, g = c.brightGain, dhp = 0.f, dbr = 0.f, dg = 0.f;
            if (c.moving) {  // gains and coefficients glide across the chunk
                const auto frames = static_cast<float>(c.frames);
                dhp = (c.inputHighpass - from->inputHighpass) / frames;
                dbr = (c.bright - from->bright) / frames;
                dg = (c.brightGain - from->brightGain) / frames;
                hp = from->inputHighpass;
                br = from->bright;
                g = from->brightGain;
            }
            dsp::OnePole input = p.inputHighpass, bright = p.bright;
            for (int i = at; i < at + c.frames; ++i) {
                hp += dhp;
                br += dbr;
                g += dg;
                const float x = input.highpass(in[i], hp);
                const float lp = bright.lowpass(x, br);
                wet[i] = x + (g - 1.f) * (x - lp);
            }
            p.inputHighpass = input;
            p.bright = bright;
            if (c.cellEnd) p.flushInput();
            at += c.frames;
            from = &c;
        }

        float* os = p.oversampler.up(wet, frames);
        from = &start_;
        for (int k = 0, at = 0; k < chunks; ++k) {
            const ChunkControls& c = controls_[static_cast<size_t>(k)];
            float* x = os + kOs * at;
            if (c.moving) {
                stagesChunk<true>(p, *from, c, x, kOs * c.frames, meters[k]);
            } else {
                stagesChunk<false>(p, *from, c, x, kOs * c.frames, meters[k]);
            }
            if (c.cellEnd) p.flushStages(c);
            at += c.frames;
            from = &c;
        }
        p.oversampler.down(os, frames, wet);

        // Base rate: the DC blocker and the trim.
        from = &start_;
        for (int k = 0, at = 0; k < chunks; ++k) {
            const ChunkControls& c = controls_[static_cast<size_t>(k)];
            float t = c.trim, dt = 0.f;
            if (c.moving) {
                t = from->trim;
                dt = (c.trim - from->trim) / static_cast<float>(c.frames);
            }
            for (int i = at; i < at + c.frames; ++i) {
                t += dt;
                wet[i] = p.dc.process(wet[i]) * t;
            }
            at += c.frames;
            from = &c;
        }
    }

    // A chunk's oversampled samples, stage by stage: each loop's only loop-carried
    // dependencies are a couple of one-poles, so successive samples' divisions overlap.
    template <bool Moving>
    static void stagesChunk(Path& p, const ChunkControls& from, const ChunkControls& c, float* x, int n,
                            ChunkMeters& m) noexcept {
        for (int k = 0; k < 2; ++k) {
            StageRun<Moving> run(p.stage[k], from.stage[k], c.stage[k], n);
            for (int i = 0; i < n; ++i) x[i] = run(x[i]);
            run.store(p.stage[k]);
            m.drive[k] = static_cast<float>(run.peak);
        }
        {  // the tone stack, then V3
            StageRun<Moving> run(p.stage[2], from.stage[2], c.stage[2], n);
            amp::ToneCoefficients tc = c.tone, dc;
            amp::ToneState tone = p.tone;
            double makeup = c.makeup, dMakeup = 0.0;
            if constexpr (Moving) {  // the coefficients glide too: a step would click
                const double step = 1.0 / n;
                const amp::ToneCoefficients& a = from.tone;
                dc = {(tc.b0 - a.b0) * step, (tc.b1 - a.b1) * step, (tc.b2 - a.b2) * step, (tc.b3 - a.b3) * step,
                      (tc.a1 - a.a1) * step, (tc.a2 - a.a2) * step, (tc.a3 - a.a3) * step};
                tc = a;
                makeup = from.makeup;
                dMakeup = (c.makeup - from.makeup) * step;
            }
            for (int i = 0; i < n; ++i) {
                if constexpr (Moving) {
                    tc.b0 += dc.b0;
                    tc.b1 += dc.b1;
                    tc.b2 += dc.b2;
                    tc.b3 += dc.b3;
                    tc.a1 += dc.a1;
                    tc.a2 += dc.a2;
                    tc.a3 += dc.a3;
                    makeup += dMakeup;
                }
                x[i] = run(static_cast<float>(tone.process(tc, x[i]) * makeup));
            }
            p.tone = tone;
            run.store(p.stage[2]);
            m.drive[2] = static_cast<float>(run.peak);
        }
        m.drive[3] = powerChunk<Moving>(p, from, c, x, n);
        m.sag = c.sag * p.env;
    }

    // Presence, the power stage with its sag, the transformer and the FIFO. The
    // sag follows the power stage's output per base-rate sample (four oversampled).
    template <bool Moving>
    static float powerChunk(Path& p, const ChunkControls& from, const ChunkControls& c, float* x, int n) noexcept {
        const float attack = c.sagAttack, release = c.sagRelease;
        float sag = c.sag, dSag = 0.f;  // per base-rate sample
        float cp = c.presence, cg = c.grid, cx = c.transformer, dcp = 0.f, dcg = 0.f, dcx = 0.f;
        float shelf = c.presenceGain - 1.f, dShelf = 0.f;
        double drive = c.drive, bias = c.powerBias, offset = c.powerOffset, gOut = c.powerGOut;
        double dDrive = 0.0, dBias = 0.0, dGOut = 0.0;
        if constexpr (Moving) {
            const double step = 1.0 / n;
            shelf = from.presenceGain - 1.f;
            dShelf = (c.presenceGain - from.presenceGain) / static_cast<float>(n);
            sag = from.sag;
            dSag = (c.sag - from.sag) * static_cast<float>(kOs) / static_cast<float>(n);
            cp = from.presence;
            cg = from.grid;
            cx = from.transformer;
            dcp = (c.presence - from.presence) / static_cast<float>(n);
            dcg = (c.grid - from.grid) / static_cast<float>(n);
            dcx = (c.transformer - from.transformer) / static_cast<float>(n);
            dDrive = (c.drive - from.drive) * step;
            dBias = (c.powerBias - from.powerBias) * step;
            dGOut = (c.powerGOut - from.powerGOut) * step;
            drive = from.drive;
            bias = from.powerBias;
            gOut = from.powerGOut;
        }
        float zp = p.presence.z, zg = p.grid.z, zx = p.transformer.z, f0 = p.fifo[0], f1 = p.fifo[1];
        float env = p.env, sagGain = p.sagGain;
        amp::Adaa adaa = p.power;
        double peak = 0.0;
        for (int i = 0; i < n; i += kOs) {
            float most = 0.f;
            for (int j = i; j < i + kOs; ++j) {
                if constexpr (Moving) {
                    shelf += dShelf;
                    cp += dcp;
                    cg += dcg;
                    cx += dcx;
                    drive += dDrive;
                    bias += dBias;
                    offset = amp::shape(bias - 0.5 * dBias);  // (as StageRun's)
                    gOut += dGOut;
                }
                const float v = x[j];
                zp = v + cp * (zp - v);
                const float s = v + shelf * (v - zp);  // Presence: a high shelf
                zg = s + cg * (zg - s);                // the power tubes' input (Miller) low-pass
                const double q = static_cast<double>(zg) * (drive * sagGain);
                peak = std::max(peak, std::abs(q));
                const double y = adaa.process(q + bias) - offset;
                most = std::max(most, static_cast<float>(std::abs(y)));
                const auto out = static_cast<float>(y * gOut);
                zx = out + cx * (zx - out);  // the transformer
                x[j] = f1;
                f1 = f0;
                f0 = zx;
            }
            env += (most > env ? attack : release) * (most - env);
            if (env < kSagFlush) env = 0.f;  // (every sample, so blocks of any size play alike)
            if constexpr (Moving) sag += dSag;
            sagGain = 1.f / (1.f + sag * env);
        }
        p.presence.z = zp;
        p.grid.z = zg;
        p.transformer.z = zx;
        p.fifo[0] = f0;
        p.fifo[1] = f1;
        p.env = env;
        p.sagGain = sagGain;
        p.power = adaa;
        return static_cast<float>(peak);
    }

    // --- Out ---------------------------------------------------------------------------------

    // Dry/Wet with the input delayed by the latency, the right channel's
    // crossfade while the Output switch fades, and the displays.
    void mixOut(float* const* ch, int n, int frames, int chunks, bool dualPath, bool fading) noexcept {
        const float* wet0 = wet_[0].data();
        const float* wet1 = wet_[1].data();
        const bool toDual = mode_ == Mode::Dual;
        for (int k = 0, at = 0; k < chunks; ++k) {
            const int len = controls_[static_cast<size_t>(k)].frames;
            for (int i = at; i < at + len; ++i) {
                const float mix = mix_.next(), dryShare = 1.f - mix;
                const float wetL = wet0[i];
                float wetR = wetL;
                if (dualPath) {
                    if (!fading) {
                        wetR = wet1[i];
                    } else {
                        const float s = share_[static_cast<size_t>(i)];
                        wetR = toDual ? (1.f - s) * wet0[i] + s * wet1[i] : (1.f - s) * wet1[i] + s * wet0[i];
                    }
                }
                for (int c = 0; c < n; ++c) {
                    const float x = ch[c][i];
                    meterIn_ = std::max(meterIn_, std::abs(x));
                    dry_[c].push(x);
                    const float y = dry_[c].tap(latency_) * dryShare + (c == 0 ? wetL : wetR) * mix;
                    ch[c][i] = y;
                    meterOut_ = std::max(meterOut_, std::abs(y));
                }
            }
            float sag = 0.f;
            for (int p = 0; p < (dualPath ? 2 : 1); ++p) {
                const ChunkMeters& m = meters_[p][static_cast<size_t>(k)];
                for (int d = 0; d < 4; ++d) meterDrive_[d] = std::max(meterDrive_[d], m.drive[d]);
                sag = std::max(sag, m.sag);
            }
            meterSag_ = sag;
            meterCount_ += len;
            if (meterCount_ >= kMeterSamples) publishMeters();
            at += len;
        }
    }

    static float toDb(float level) noexcept { return std::max(kFloorDb, gainToDb(level)); }

    void publishMeters() noexcept {
        publish(InputLevel, toDb(meterIn_));
        publish(Drive1, toDb(meterDrive_[0]));
        publish(Drive2, toDb(meterDrive_[1]));
        publish(Drive3, toDb(meterDrive_[2]));
        publish(PowerDrive, toDb(meterDrive_[3]));
        publish(SagDisplay, 20.f * std::log10(1.f + meterSag_));
        publish(OutputLevel, toDb(meterOut_));
        std::rotate(std::begin(recentIn_), std::begin(recentIn_) + 1, std::end(recentIn_));
        recentIn_[3] = meterIn_;
        meterCount_ = 0;
        clearMeters();
    }

    void clearMeters() noexcept {
        meterIn_ = meterOut_ = meterSag_ = 0.f;
        for (float& d : meterDrive_) d = 0.f;
    }

    static const std::vector<std::string>& dualLabels() {
        static const std::vector<std::string> kLabels = {"Mono", "Dual"};
        return kLabels;
    }

    static const std::vector<ParamInfo>& infos() {
        static const std::vector<ParamInfo> kInfos = {
            {"type", "Amp Type", "", 0.f, 6.f, 0.f, false, amp::modelLabels()},
            {"gain", "Gain", "dial", 0.f, 10.f, 5.f},
            {"bass", "Bass", "dial", 0.f, 10.f, 5.f},
            {"middle", "Middle", "dial", 0.f, 10.f, 5.f},
            {"treble", "Treble", "dial", 0.f, 10.f, 5.f},
            {"presence", "Presence", "dial", 0.f, 10.f, 5.f},
            {"volume", "Volume", "dial", 0.f, 10.f, 5.f},
            {"dual", "Dual Mono", "", 0.f, 1.f, 0.f, false, dualLabels()},
            {"mix", "Dry/Wet", "%", 0.f, 100.f, 100.f},
        };
        return kInfos;
    }

    const int latency_ = dsp::Oversampler::latencyFor(amp::kOversamplingLog2) + 1;  // 37
    double sampleRate_ = 48000.0;
    int maxBlock_ = 1;
    int channels_ = 2;

    Path path_[2];                               // Mono: the first; Dual: one per channel
    dsp::DelayLine dry_[2];                      // each channel's input, for Dry/Wet
    std::vector<float> in_[2], wet_[2], share_;  // maxBlock_ each
    std::vector<ChunkControls> controls_;        // a slice's chunks
    std::vector<ChunkMeters> meters_[2];         // what each amp showed in them
    ChunkControls start_;                        // the controls the slice's first chunk ramps from
    ChunkControls last_;                         // the controls at the end of the last chunk

    double dial_[6] = {5, 5, 5, 5, 5, 5};  // Gain..Volume as they glide
    double chunkGlide_ = 0.3;              // their one-poles' step over a whole chunk
    int model_ = 0;
    amp::Voicing voice_ = amp::voicing(0), from_ = voice_, to_ = voice_;  // the morph
    double morph_ = 1.0;                                                  // 1: done
    int morphFrames_ = 0;                                                 // how far it has moved
    bool morphMoves_ = false;  // through this cell (it waits for its levels: morphCell())
    // The morph's levels (morphCell(), morphLevel()): the input's peak they are for, the ends', its points (the
    // compensation at each) and how many of them are known (the ends' first), and the compensation now.
    double levelInput_ = 1e-3, levelFrom_ = 0.0, levelTo_ = 0.0, levelComp_ = 0.0;
    double levelSteps_[kLevelSteps + 1] = {};  // (the last, the morph's end, stays 0)
    int levelKnown_ = 0;
    float recentIn_[4] = {};  // the input's peak in the last four display windows

    Mode mode_ = Mode::Mono;
    int fade_ = -1;  // samples into the Output switch's fade (-1: none)
    int fadeLength_ = 960, fadeInLength_ = 240;
    SmoothedValue mix_;

    int quietFrames_[2] = {};  // per amp: frames since its input was last non-zero (up to kSleepFrames)
    int quietOut_[2] = {};     // and since its output was (up to kQuietOut)
    bool asleep_[2] = {};

    int meterCount_ = 0;
    float meterIn_ = 0.f, meterOut_ = 0.f, meterSag_ = 0.f, meterDrive_[4] = {};
};

}  // namespace

SUB_REGISTER_BUILTIN(AmpProcessor, AudioEffect);

}  // namespace sub
