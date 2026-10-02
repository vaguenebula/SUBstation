// Built-in "Compressor" device: a feed-forward compressor with a soft knee,
// keyed by its own input or by its sidechain (a ducker, with another track
// there). Linked stereo: both channels get the gain the louder one calls for.
//
// The key's level follows its peaks at once and falls at the release time (so
// it holds across a low note's cycles); the gain computer works in dB on that,
// and the gain reduction it asks for is smoothed with the attack as it grows.
//
// Its editor draws three displays, one value per kMeterSamples: the key's
// level in dB, the gain reduction in dB (positive), and the output level in dB.

#include <algorithm>
#include <cmath>

#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "rt/RtUtils.h"

namespace gil {
namespace {

class CompressorProcessor final : public BuiltinProcessor {
public:
    enum Param { Threshold = 0, Ratio, Attack, Release, Knee, Makeup, Mix, NumParams };
    enum Display { InputLevel = 0, Reduction, OutputLevel };

    CompressorProcessor()
        : BuiltinProcessor(infos(), {{"input", kMeterSamples}, {"reduction", kMeterSamples},
                                     {"output", kMeterSamples}}) {}

    std::string typeId() const override { return "builtin:compressor"; }
    std::string name() const override { return "Compressor"; }
    bool hasSidechain() const override { return true; }

    void prepare(double sampleRate, int) override {
        sampleRate_ = sampleRate;
        makeup_.reset(sampleRate, 0.02);
        mix_.reset(sampleRate, 0.02);
        makeup_.snapTo(param(Makeup));
        mix_.snapTo(param(Mix) / 100.f);
        reset();
    }

    void reset() override {
        envelope_ = reductionDb_ = 0.f;
        meterCount_ = 0;
        peakIn_ = peakOut_ = maxReduction_ = 0.f;
    }

protected:
    void render(const ProcessContext&, float* const* ch, int numChannels, int numFrames) override {
        const int n = std::min(numChannels, 2);
        if (n <= 0) return;
        const float threshold = param(Threshold);
        const float slope = 1.f / std::max(1.f, param(Ratio)) - 1.f;  // dB out per dB in above the knee, minus 1
        const float knee = param(Knee);
        const float attack = onePole(param(Attack));
        const float release = onePole(param(Release));
        makeup_.setTarget(param(Makeup));
        mix_.setTarget(param(Mix) / 100.f);
        // With a sidechain the key is what it hears (silence while solo leaves its
        // source out); without one, the device's own input.
        const bool keyed = sidechainConnected();
        const float* keyL = keyed ? sidechain(0) : nullptr;
        const float* keyR = keyed ? sidechain(1) : nullptr;

        for (int i = 0; i < numFrames; ++i) {
            float key = 0.f;
            if (!keyed) {
                for (int c = 0; c < n; ++c) key = std::max(key, std::abs(ch[c][i]));
            } else if (keyL != nullptr) {
                key = std::max(std::abs(keyL[i]), std::abs(keyR[i]));
            }
            envelope_ = key >= envelope_ ? key : key + release * (envelope_ - key);
            const float level = 20.f * std::log10(envelope_ + 1e-9f);
            const float target = level - curve(level, threshold, slope, knee);
            reductionDb_ = target > reductionDb_ ? target + attack * (reductionDb_ - target) : target;

            const float mix = mix_.next();
            const float gain = 1.f + mix * (std::exp((makeup_.next() - reductionDb_) * kDbToLog) - 1.f);
            float out = 0.f;
            for (int c = 0; c < n; ++c) {
                ch[c][i] *= gain;
                out = std::max(out, std::abs(ch[c][i]));
            }

            peakIn_ = std::max(peakIn_, key);
            peakOut_ = std::max(peakOut_, out);
            maxReduction_ = std::max(maxReduction_, reductionDb_);
            if (++meterCount_ == kMeterSamples) {
                publish(InputLevel, toDb(peakIn_));
                publish(Reduction, maxReduction_);
                publish(OutputLevel, toDb(peakOut_));
                meterCount_ = 0;
                peakIn_ = peakOut_ = maxReduction_ = 0.f;
            }
        }
    }

private:
    static constexpr int kMeterSamples = 256;  // audio per meter value
    static constexpr float kFloorDb = -90.f;
    static constexpr float kDbToLog = 0.11512925f;  // ln(10) / 20

    // The level out for `level` in (dB): unchanged below the knee, the ratio's
    // slope above it, and a quadratic blend across it.
    static float curve(float level, float threshold, float slope, float knee) noexcept {
        const float over = level - threshold;
        if (2.f * over <= -knee) return level;
        if (2.f * over < knee) {
            const float x = over + 0.5f * knee;
            return level + slope * x * x / (2.f * knee);
        }
        return level + slope * over;
    }

    static float toDb(float peak) noexcept { return std::max(kFloorDb, 20.f * std::log10(peak + 1e-9f)); }

    float onePole(float ms) const noexcept {
        return static_cast<float>(std::exp(-1.0 / (std::max(0.01f, ms) * 0.001 * sampleRate_)));
    }

    static const std::vector<ParamInfo>& infos() {
        static const std::vector<ParamInfo> kInfos = {
            {"threshold", "Threshold", "dB", -60.f, 0.f, -18.f},
            {"ratio", "Ratio", ":1", 1.f, 20.f, 4.f, true},
            {"attack", "Attack", "ms", 0.1f, 200.f, 10.f, true},
            {"release", "Release", "ms", 5.f, 2000.f, 150.f, true},
            {"knee", "Knee", "dB", 0.f, 24.f, 6.f},
            {"makeup", "Makeup", "dB", -12.f, 24.f, 0.f},
            {"mix", "Dry/Wet", "%", 0.f, 100.f, 100.f},
        };
        return kInfos;
    }

    double sampleRate_ = 48000.0;
    float envelope_ = 0.f;  // the key's level (linear)
    float reductionDb_ = 0.f;
    SmoothedValue makeup_, mix_;
    int meterCount_ = 0;
    float peakIn_ = 0.f, peakOut_ = 0.f, maxReduction_ = 0.f;
};

}  // namespace

GIL_REGISTER_BUILTIN(CompressorProcessor, AudioEffect);

}  // namespace gil
