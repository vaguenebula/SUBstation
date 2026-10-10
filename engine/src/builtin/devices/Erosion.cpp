// Built-in "Erosion" device, after Live 12.4's Erosion: the input read out of a
// 2 ms delay whose read position a sine or band-passed noise wobbles at audio
// rate (builtin/ErosionDesign.h). That phase-modulates every partial, more the
// higher it is: lows survive, highs turn to grit and hiss, and sidebands past
// Nyquist fold back as the "digital" aliasing the device is for. There is no
// dry path (Live's has no Dry/Wet): at Amount 0 it is a clean delay of its
// latency, which the engine compensates, so it is transparent to the sample.
//
// - The delay: centre D = 2 ms in whole samples (the latency), read with 4-point
//   Hermite interpolation at D + E m, m the modulator (RMS 1/sqrt 2) and E the
//   excursion, Amount's square times the most (D - 2 over sqrt 2: a sine never
//   reaches the limit of D - 2, where the read is clamped; noise peaks
//   sometimes do near the top of Amount).
// - The modulators: one quadrature phasor (in double, renormalised each chunk)
//   for the sine, the sides sin(phase ± h), h = Stereo × pi/4 (a quarter cycle
//   apart at 100 %); two white noises (mid and side), each through two
//   band-pass sections (TPT state-variable, k v1) and scaled to RMS 1/sqrt 2
//   exactly (erosion::band), mixed to the sides by Stereo's mid and side gains.
//   Noise Blend crossfades sine and noise at equal power. Live's legacy modes
//   are settings: Sine is Noise Blend 0 %, Noise 100 %, Wide Noise 100 % with
//   Stereo 100 %.
// - Smoothing: the work runs in chunks of 16 samples on a grid that goes on
//   across blocks (so the output never depends on how a block is split). At each
//   chunk's start the controls' glides (two one-poles of 10 ms in a row, so a
//   jump eases in and out; Frequency and Width in log) move on a chunk, and what
//   they make (the excursion, the weights, the spread, the noise's gain and its
//   sections' g and k) ramps linearly over the chunk, the sections' coefficients
//   made from g and k sample by sample while they move. The phasor's rate steps
//   per chunk (its phase stays continuous). Once every glide has landed nothing
//   is worked out again: a device left alone costs only its per-sample work.
// - One channel gets the left modulator, worked out exactly as in stereo: a mono
//   run is the left side of a stereo one. More than two: the first two.
// - Nothing recursive decays (the delay has no feedback, the sections always hear
//   noise, the phasor is renormalised): silence comes out as exact zeros once the
//   line has emptied, 2D samples on.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/Dsp.h"
#include "builtin/DspBlocks.h"
#include "builtin/ErosionDesign.h"

namespace sub {
namespace {

constexpr int kChunk = 16;              // samples per control-rate step
constexpr int kMeterSamples = 256;      // audio per `erosion` value
static_assert(kMeterSamples % kChunk == 0, "a meter's frames end with a chunk");
constexpr double kGlideSeconds = 0.01;  // each of a control's two one-poles
constexpr double kLanded = 1e-6;        // a glide this near its target lands on it
constexpr float kFloorDb = -90.f;       // the `erosion` display's floor
constexpr int kChannels = 2;

// Two one-poles in a row gliding to a target: a jump eases in and out, and a
// glide can turn back halfway without a kink. It lands on the target exactly
// once it is within `landed` of it. (As the Disperser's.)
struct Ease {
    double first = 0.0, value = 0.0;

    void snap(double target) noexcept { first = value = target; }
    bool settled(double target) const noexcept { return first == target && value == target; }
    // One step towards the target; false if it was already there (nothing moved).
    bool step(double target, double coefficient) noexcept {
        if (settled(target)) return false;
        first += coefficient * (target - first);
        value += coefficient * (first - value);
        if (std::abs(target - first) < kLanded && std::abs(target - value) < kLanded) snap(target);
        return true;
    }
};

// A value moving in a straight line over a chunk, sample by sample, from where
// the last chunk was headed to this one's target (floats: the inner loop's type).
// start() puts it back exactly on the last target, so the steps' rounding never
// adds up across chunks.
struct Ramp {
    float value = 0.f, step = 0.f, target = 0.f;

    void snap(float v) noexcept {
        value = target = v;
        step = 0.f;
    }
    void start(float next) noexcept {
        value = target;
        target = next;
        step = (target - value) * (1.f / kChunk);
    }
};

class ErosionProcessor final : public BuiltinProcessor {
public:
    enum Param { Frequency = 0, Width, Amount, Blend, Stereo, NumParams };
    enum Display { Input = 0, Output, Erosion, ModLeft, ModRight };
    // What ramps over a chunk: the excursion (samples), Noise Blend's weights, the
    // noise's gain, Stereo's mid and side gains for it and the sine's half-offset
    // (its cos and sin), and the band-pass sections' g and k.
    enum RampId { E = 0, Sine, Noise, Scale, Mid, Side, CosHalf, SinHalf, G, K, NumRamps };

    ErosionProcessor()
        : BuiltinProcessor(infos(), {{"input", 1},
                                     {"output", 1},
                                     {"erosion", kMeterSamples},
                                     {"mod_l", 1},
                                     {"mod_r", 1}}) {}

    std::string typeId() const override { return "builtin:erosion"; }
    std::string name() const override { return "Erosion"; }

    // The delay's centre (2 ms): the engine delays the other tracks by it, so at
    // Amount 0 the device is transparent. No sample stays in it longer than 2D.
    int latencySamples() const override { return centre_; }
    int tailSamples() const override { return 2 * centre_; }

    void prepare(double sampleRate, int) override {
        sampleRate_ = sampleRate;
        centre_ = erosion::centreDelaySamples(sampleRate);
        limit_ = static_cast<float>(erosion::limitSamples(sampleRate));
        maxExcursion_ = erosion::maxExcursionSamples(sampleRate);
        for (dsp::DelayLine& line : lines_) line.prepare(2 * centre_ + 8);
        glide_ = 1.0 - onePoleCoefficient(kGlideSeconds, sampleRate, kChunk);
        reset();
    }

    // Silent, the modulators restarted (the noise reseeded: a render repeats
    // exactly), and every glide and ramp where the parameters are.
    void reset() override {
        for (dsp::DelayLine& line : lines_) line.reset();
        for (dsp::Svf& section : sections_) section.reset();
        noiseMid_.seed(erosion::kMidSeed);
        noiseSide_.seed(erosion::kSideSeed);
        cos_ = 1.0;
        sin_ = 0.0;
        freq_.snap(targetLogFreq());
        width_.snap(targetLogWidth());
        amount_.snap(targetAmount());
        blend_.snap(targetBlend());
        stereo_.snap(targetStereo());
        workOut(true, true, true, true, true);
        for (int r = 0; r < NumRamps; ++r) ramps_[r].snap(next_[r]);
        coefficients_ = dsp::SvfCoefficients(ramps_[G].target, ramps_[K].target);
        chunkLeft_ = 0;
        meterCount_ = 0;
        meterSum_ = 0.0;
    }

protected:
    void render(const ProcessContext&, float* const* ch, int numChannels, int numFrames) override {
        const int n = std::min(numChannels, kChannels);
        if (n <= 0) return;
        if (n != channels_) {  // a channel that wasn't processed has no history to go on from
            for (int c = channels_; c < n; ++c) lines_[c].reset();
            channels_ = n;
        }
        for (int i = 0; i < numFrames;) {
            if (chunkLeft_ == 0) startChunk();
            const int count = std::min(chunkLeft_, numFrames - i);
            const bool still = ramps_[E].value == 0.f && ramps_[E].step == 0.f;  // no excursion over this chunk
            if (n == 2) {
                still ? run<2, false>(ch, i, count) : run<2, true>(ch, i, count);
            } else {
                still ? run<1, false>(ch, i, count) : run<1, true>(ch, i, count);
            }
            i += count;
            chunkLeft_ -= count;
        }
    }

private:
    // A chunk's start: the glides move on a chunk; what moved is worked out
    // again (what didn't keeps its target, and its ramp restarts there with step 0).
    void startChunk() noexcept {
        const bool freq = freq_.step(targetLogFreq(), glide_);
        const bool width = width_.step(targetLogWidth(), glide_);
        const bool amount = amount_.step(targetAmount(), glide_);
        const bool blend = blend_.step(targetBlend(), glide_);
        const bool stereo = stereo_.step(targetStereo(), glide_);
        workOut(freq, width, amount, blend, stereo);
        for (int r = 0; r < NumRamps; ++r) ramps_[r].start(next_[r]);
        bandMoving_ = ramps_[G].step != 0.f || ramps_[K].step != 0.f;
        if (!bandMoving_) coefficients_ = dsp::SvfCoefficients(ramps_[G].target, ramps_[K].target);
        // The phasor's radius back to 1 (its rounding would otherwise drift it).
        const double radius = 1.5 - 0.5 * (cos_ * cos_ + sin_ * sin_);
        cos_ *= radius;
        sin_ *= radius;
        chunkLeft_ = kChunk;
    }

    // The ramps' next targets (and the phasor's rate) from the glides' values: only
    // what depends on a glide that moved (the rest is as it was: the same numbers).
    void workOut(bool freq, bool width, bool amount, bool blend, bool stereo) noexcept {
        if (freq || width) {
            const erosion::Band band = erosion::band(std::exp(freq_.value), std::exp(width_.value), sampleRate_);
            next_[G] = static_cast<float>(band.g);
            next_[K] = static_cast<float>(band.k);
            next_[Scale] = static_cast<float>(band.scale);
        }
        if (freq) {
            const double delta = 2.0 * erosion::kPi * std::exp(freq_.value) / sampleRate_;
            rotateCos_ = std::cos(delta);
            rotateSin_ = std::sin(delta);
        }
        if (amount) next_[E] = static_cast<float>(maxExcursion_ * amount_.value * amount_.value);
        if (blend) {
            const erosion::Weights weights = erosion::blendWeights(100.0 * blend_.value);
            next_[Sine] = static_cast<float>(weights.sine);
            next_[Noise] = static_cast<float>(weights.noise);
        }
        if (stereo) {
            const erosion::Spread spread = erosion::stereoSpread(100.0 * stereo_.value);
            next_[Mid] = static_cast<float>(spread.mid);
            next_[Side] = static_cast<float>(spread.side);
            next_[CosHalf] = static_cast<float>(std::cos(spread.half));
            next_[SinHalf] = static_cast<float>(std::sin(spread.half));
        }
    }

    // `count` frames from `from`, within one chunk. Erode false: the excursion is
    // 0 throughout, so the output is the line's sample D ago exactly (the same as
    // Hermite at a whole delay, and cheaper); the modulators run on all the same,
    // so the next chunk is as it would have been.
    template <int N, bool Erode>
    void run(float* const* ch, int from, int count) noexcept {
        double c = cos_, s = sin_;
        const double rc = rotateCos_, rs = rotateSin_;
        float e = ramps_[E].value, sine = ramps_[Sine].value, noise = ramps_[Noise].value,
              scale = ramps_[Scale].value, mid = ramps_[Mid].value, side = ramps_[Side].value,
              cosHalf = ramps_[CosHalf].value, sinHalf = ramps_[SinHalf].value, g = ramps_[G].value,
              k = ramps_[K].value;
        const float eStep = ramps_[E].step, sineStep = ramps_[Sine].step, noiseStep = ramps_[Noise].step,
                    scaleStep = ramps_[Scale].step, midStep = ramps_[Mid].step, sideStep = ramps_[Side].step,
                    cosStep = ramps_[CosHalf].step, sinStep = ramps_[SinHalf].step, gStep = ramps_[G].step,
                    kStep = ramps_[K].step;
        dsp::SvfCoefficients coefficients = coefficients_;
        dsp::Svf m1 = sections_[0], m2 = sections_[1], s1 = sections_[2], s2 = sections_[3];
        dsp::Noise noiseMid = noiseMid_, noiseSide = noiseSide_;
        const bool bandMoving = bandMoving_;
        const float centre = static_cast<float>(centre_), limit = limit_;
        double sum = meterSum_;  // (summed in the samples' order, however the block was split)

        // What the displays get, published after the loop (out of the way of the
        // arithmetic, whose state stays in registers).
        float inputs[kChunk], outputs[kChunk], modLeft[kChunk], modRight[kChunk];

        for (int j = 0; j < count; ++j) {
            const int i = from + j;
            const double c1 = c * rc - s * rs;
            s = s * rc + c * rs;
            c = c1;
            cosHalf += cosStep;
            sinHalf += sinStep;
            const auto fs = static_cast<float>(s), fc = static_cast<float>(c);
            const float sineL = fs * cosHalf + fc * sinHalf;  // sin(phase + h)
            const float sineR = fs * cosHalf - fc * sinHalf;  // sin(phase - h)

            if (bandMoving) {
                g += gStep;
                k += kStep;
                coefficients = dsp::SvfCoefficients(g, k);
            }
            const float bandK = coefficients.k;
            float midNoise = bandK * m1.tick(coefficients, noiseMid.next()).band;
            float sideNoise = bandK * s1.tick(coefficients, noiseSide.next()).band;
            midNoise = bandK * m2.tick(coefficients, midNoise).band;
            sideNoise = bandK * s2.tick(coefficients, sideNoise).band;
            scale += scaleStep;
            midNoise *= scale;
            sideNoise *= scale;
            mid += midStep;
            side += sideStep;
            const float noiseL = mid * midNoise + side * sideNoise;
            const float noiseR = mid * midNoise - side * sideNoise;

            sine += sineStep;
            noise += noiseStep;
            const float modL = sine * sineL + noise * noiseL;
            const float modR = sine * sineR + noise * noiseR;
            e += eStep;

            const float mod[2] = {modL, modR};
            float x[N], y[N];
            for (int ci = 0; ci < N; ++ci) x[ci] = ch[ci][i];  // (both read before either is written)
            for (int ci = 0; ci < N; ++ci) {
                dsp::DelayLine& line = lines_[ci];
                line.push(x[ci]);
                if constexpr (Erode) {
                    float d = e * mod[ci];
                    d = d < limit ? d : limit;  // (a NaN would land on the limit, never reach the read)
                    d = d > -limit ? d : -limit;
                    y[ci] = line.hermite(centre + d);
                    const double change = static_cast<double>(y[ci]) - static_cast<double>(line.tap(centre_));
                    sum += change * change;
                } else {
                    y[ci] = line.tap(centre_);
                }
                ch[ci][i] = y[ci];
            }
            if constexpr (N == 2) {
                inputs[j] = 0.5f * (x[0] + x[1]);
                outputs[j] = 0.5f * (y[0] + y[1]);
                modRight[j] = modR;
            } else {
                inputs[j] = x[0];
                outputs[j] = y[0];
                modRight[j] = modL;
            }
            modLeft[j] = modL;
        }

        cos_ = c;
        sin_ = s;
        ramps_[E].value = e;
        ramps_[Sine].value = sine;
        ramps_[Noise].value = noise;
        ramps_[Scale].value = scale;
        ramps_[Mid].value = mid;
        ramps_[Side].value = side;
        ramps_[CosHalf].value = cosHalf;
        ramps_[SinHalf].value = sinHalf;
        ramps_[G].value = g;
        ramps_[K].value = k;
        sections_[0] = m1;
        sections_[1] = m2;
        sections_[2] = s1;
        sections_[3] = s2;
        noiseMid_ = noiseMid;
        noiseSide_ = noiseSide;
        meterSum_ = sum;

        for (int j = 0; j < count; ++j) {
            publish(Input, inputs[j]);
            publish(Output, outputs[j]);
            publish(ModLeft, modLeft[j]);
            publish(ModRight, modRight[j]);
        }
        meter(count, N);
    }

    // The `erosion` display: what the device changed (its output against the
    // input D samples ago) as a power in dB, per kMeterSamples frames. The count
    // runs on across calls, as the chunks' grid does (each meter's frames end
    // where a chunk does: kMeterSamples is a multiple of kChunk).
    void meter(int frames, int channels) noexcept {
        meterCount_ += frames;
        if (meterCount_ < kMeterSamples) return;
        const double power = meterSum_ / (static_cast<double>(kMeterSamples) * channels);
        publish(Erosion, power > 1e-9 ? std::max(kFloorDb, static_cast<float>(10.0 * std::log10(power))) : kFloorDb);
        meterCount_ -= kMeterSamples;
        meterSum_ = 0.0;
    }

    double targetLogFreq() const noexcept {
        return std::log(erosion::modFrequency(param(Frequency), sampleRate_));
    }
    double targetLogWidth() const noexcept {
        return std::log(std::clamp<double>(param(Width), erosion::kMinWidth, erosion::kMaxWidth));
    }
    double targetAmount() const noexcept { return std::clamp(param(Amount), 0.f, 100.f) / 100.0; }
    double targetBlend() const noexcept { return std::clamp(param(Blend), 0.f, 100.f) / 100.0; }
    double targetStereo() const noexcept { return std::clamp(param(Stereo), 0.f, 100.f) / 100.0; }

    static const std::vector<ParamInfo>& infos() {
        static const std::vector<ParamInfo> kInfos = {
            {"freq", "Frequency", "Hz", static_cast<float>(erosion::kMinFrequency),
             static_cast<float>(erosion::kMaxFrequency), 1000.f, true},
            {"width", "Filter Width", "oct", static_cast<float>(erosion::kMinWidth),
             static_cast<float>(erosion::kMaxWidth), 2.5f, true},
            {"amount", "Amount", "%", 0.f, 100.f, 25.f},
            {"blend", "Noise Blend", "%", 0.f, 100.f, 100.f},
            {"stereo", "Stereo Width", "%", 0.f, 100.f, 0.f},
        };
        return kInfos;
    }

    double sampleRate_ = 48000.0;
    int centre_ = erosion::centreDelaySamples(48000.0);  // D: the latency
    float limit_ = static_cast<float>(erosion::limitSamples(48000.0));
    double maxExcursion_ = erosion::maxExcursionSamples(48000.0);
    double glide_ = 0.03;
    int channels_ = kChannels;

    dsp::DelayLine lines_[kChannels];
    dsp::Svf sections_[4];  // the mid noise's two band-pass sections, then the side's
    dsp::Noise noiseMid_, noiseSide_;
    double cos_ = 1.0, sin_ = 0.0;              // the sine's phasor
    double rotateCos_ = 1.0, rotateSin_ = 0.0;  // its turn per sample

    Ease freq_, width_, amount_, blend_, stereo_;  // the controls' glides (Frequency and Width in log)
    // What the glides make, ramped over each chunk (RampId), and where each is
    // headed next: workOut() sets those, only for what moved.
    Ramp ramps_[NumRamps];
    float next_[NumRamps] = {0.f, 0.f, 1.f, 1.f, 1.f, 0.f, 1.f, 0.f, 0.f, 1.f};
    dsp::SvfCoefficients coefficients_;  // the sections' (made per sample while g or k ramps)
    bool bandMoving_ = false;
    int chunkLeft_ = 0;  // samples left in the chunk under way

    int meterCount_ = 0;
    double meterSum_ = 0.0;
};

}  // namespace

SUB_REGISTER_BUILTIN(ErosionProcessor, AudioEffect);

}  // namespace sub
