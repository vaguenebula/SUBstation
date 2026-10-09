// Built-in "Over The Top" device: multiband upward + downward compression, the
// sound of every drop since 2014. Three bands split by Linkwitz-Riley
// crossovers; each band is squashed hard from above and pulled up hard from
// below, then made up. One knob (Soundgoodize) blends the processed bands in.
//
// The dry signal is the bands summed unprocessed (an allpass of the input), so
// in-between settings don't comb-filter against the crossover's phase shift.

#include <array>
#include <atomic>
#include <cmath>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "builtin/Dsp.h"
#include "rt/RtUtils.h"

namespace sub {
namespace {

class OttProcessor final : public BuiltinProcessor {
public:
    enum Param { Depth = 0, Output, NumParams };

    OttProcessor() : BuiltinProcessor(infos()) {}

    std::string typeId() const override { return "builtin:ott"; }
    std::string name() const override { return "Over The Top"; }

    void prepare(double sampleRate, int) override {
        sampleRate_ = sampleRate;
        lowCross_ = crossover(kLowCrossover, sampleRate);
        highCross_ = crossover(kHighCrossover, sampleRate);
        for (auto& ch : channels_) {
            ch.lowSplit.coeffs = ch.lowLp.coeffs = ch.lowHp.coeffs = lowCross_;
            ch.highSplit.coeffs = ch.highLp.coeffs = ch.highHp.coeffs = ch.lowAllpass.coeffs = highCross_;
        }
        for (int b = 0; b < kBands; ++b) {
            attack_[b] = onePole(kBands_[b].attackMs, sampleRate);
            release_[b] = onePole(kBands_[b].releaseMs, sampleRate);
        }
        depth_.reset(sampleRate, 0.03);
        output_.reset(sampleRate, 0.03);
        depth_.snapTo(param(Depth) / 100.f);
        output_.snapTo(dbToGain(param(Output)));
        reset();
    }

    void reset() override {
        for (auto& ch : channels_) ch.clear();
        env_.fill(0.f);
    }

protected:
    void render(const ProcessContext&, float* const* ch, int numChannels, int numFrames) override {
        depth_.setTarget(param(Depth) / 100.f);
        output_.setTarget(dbToGain(param(Output)));
        const int n = std::min(numChannels, 2);
        if (n <= 0) return;

        for (int i = 0; i < numFrames; ++i) {
            // Split every channel into bands; detect on the loudest channel (linked stereo).
            std::array<std::array<float, kBands>, 2> bands{};
            std::array<float, kBands> peak{};
            for (int c = 0; c < n; ++c) {
                Channel& s = channels_[c];
                const float x = ch[c][i];
                float lp, hp;
                s.lowSplit.split(x, lp, hp);
                const float low = s.lowAllpass.allpass(s.lowLp.lowpass(lp));
                const float rest = s.lowHp.highpass(hp);
                s.highSplit.split(rest, lp, hp);
                bands[c] = {low, s.highLp.lowpass(lp), s.highHp.highpass(hp)};
                for (int b = 0; b < kBands; ++b) peak[b] = std::max(peak[b], std::abs(bands[c][b]));
            }

            const float depth = depth_.next();
            const float out = output_.next();
            std::array<float, kBands> mix;
            for (int b = 0; b < kBands; ++b) {
                const float coeff = peak[b] > env_[b] ? attack_[b] : release_[b];
                env_[b] = peak[b] + coeff * (env_[b] - peak[b]);
                const float gain = bandGain(b, env_[b]);
                mix[b] = 1.f + depth * (gain - 1.f);
            }
            for (int c = 0; c < n; ++c) {
                float sum = 0.f;
                for (int b = 0; b < kBands; ++b) sum += bands[c][b] * mix[b];
                ch[c][i] = sum * out;
            }
        }
    }

private:
    static constexpr int kBands = 3;
    static constexpr float kLowCrossover = 88.3f;   // Hz
    static constexpr float kHighCrossover = 2500.f;
    static constexpr float kDownRatio = 66.f;       // effectively a limiter above the threshold
    static constexpr float kUpRatio = 4.17f;        // and 1:4 below the other one
    static constexpr float kMaxUpwardDb = 30.f;     // how far the quiet stuff may be dragged up
    static constexpr float kFloorDb = -90.f;        // below this, leave silence silent
    static constexpr float kMakeupDb = 6.f;         // on top of the bands' own: a squashed mix lands near -16 dB RMS

    struct BandSettings {
        float downThresholdDb, upThresholdDb, makeupDb, attackMs, releaseMs;
    };
    static constexpr std::array<BandSettings, kBands> kBands_ = {{
        {-33.8f, -40.8f, 10.3f, 47.8f, 282.f},  // low
        {-30.2f, -41.8f, 5.7f, 22.4f, 282.f},   // mid
        {-35.5f, -40.8f, 10.3f, 13.5f, 132.f},  // high
    }};

    // A Butterworth (k = sqrt 2) state-variable filter section at a crossover.
    // Two in a row make a Linkwitz-Riley crossover half.
    static dsp::SvfCoefficients crossover(float cutoff, double sampleRate) {
        const float g = static_cast<float>(std::tan(3.14159265358979 * std::min<double>(cutoff, 0.49 * sampleRate) / sampleRate));
        return {g, 1.41421356f};
    }
    struct Svf {
        dsp::SvfCoefficients coeffs;
        dsp::Svf state;
        void split(float x, float& lp, float& hp) noexcept {
            const dsp::Svf::Outputs o = state.tick(coeffs, x);
            lp = o.low;
            hp = x - coeffs.k * o.band - o.low;
        }
        float lowpass(float x) noexcept { return state.tick(coeffs, x).low; }
        float highpass(float x) noexcept {
            const dsp::Svf::Outputs o = state.tick(coeffs, x);
            return x - coeffs.k * o.band - o.low;
        }
        // What an LR4 low + high pass at this frequency sum to, so the low band
        // gets the same phase shift from the upper crossover as the others.
        float allpass(float x) noexcept { return x - 2.f * coeffs.k * state.tick(coeffs, x).band; }
        void clear() noexcept { state.reset(); }
    };
    struct Channel {
        Svf lowSplit, lowLp, lowHp, highSplit, highLp, highHp, lowAllpass;
        void clear() noexcept {
            for (Svf* f : {&lowSplit, &lowLp, &lowHp, &highSplit, &highLp, &highHp, &lowAllpass}) f->clear();
        }
    };

    static float onePole(float ms, double sampleRate) {
        return static_cast<float>(onePoleCoefficient(ms * 0.001, sampleRate));
    }

    // The band's linear gain for its envelope: down above one threshold, up
    // below the other, then the make-up.
    static float bandGain(int band, float envelope) noexcept {
        const BandSettings& s = kBands_[band];
        const float level = gainToDb(envelope);
        float db = 0.f;
        if (level > s.downThresholdDb) {
            db = (s.downThresholdDb - level) * (1.f - 1.f / kDownRatio);
        } else if (level < s.upThresholdDb) {
            db = std::min((s.upThresholdDb - level) * (1.f - 1.f / kUpRatio), kMaxUpwardDb);
            db *= std::clamp((level - kFloorDb) / 20.f, 0.f, 1.f);  // fade out towards silence
        }
        return expDbToGain(db + s.makeupDb + kMakeupDb);
    }

    static const std::vector<ParamInfo>& infos() {
        static const std::vector<ParamInfo> kInfos = {
            {"depth", "Soundgoodize", "%", 0.f, 100.f, 100.f},
            {"output", "Output", "dB", -24.f, 24.f, 0.f},
        };
        return kInfos;
    }

    double sampleRate_ = 48000.0;
    dsp::SvfCoefficients lowCross_, highCross_;
    std::array<Channel, 2> channels_{};
    std::array<float, kBands> env_{}, attack_{}, release_{};
    SmoothedValue depth_, output_;
};

}  // namespace

SUB_REGISTER_BUILTIN(OttProcessor, AudioEffect);

}  // namespace sub
