#pragma once
// Built-in "Utility" device: gain, pan and stereo width.

#include "processors/BuiltinProcessor.h"
#include "rt/RtUtils.h"

namespace gil {

class UtilityProcessor final : public BuiltinProcessor {
public:
    enum Param { Gain = 0, Pan, Width, NumParams };

    UtilityProcessor() : BuiltinProcessor(infos()) {}

    std::string typeId() const override { return "builtin:utility"; }
    std::string name() const override { return "Utility"; }

    void prepare(double sampleRate, int) override {
        gainL_.reset(sampleRate, 0.02);
        gainR_.reset(sampleRate, 0.02);
        width_.reset(sampleRate, 0.02);
        float l, r;
        targets(l, r);
        gainL_.snapTo(l);
        gainR_.snapTo(r);
        width_.snapTo(param(Width) / 100.f);
    }

protected:
    void render(const ProcessContext&, float* const* ch, int numChannels, int numFrames) override {
        float l, r;
        targets(l, r);
        gainL_.setTarget(l);
        gainR_.setTarget(r);
        width_.setTarget(param(Width) / 100.f);

        if (numChannels < 2) {
            for (int i = 0; i < numFrames; ++i) {
                ch[0][i] *= gainL_.next();
                gainR_.next();
                width_.next();
            }
            return;
        }
        float* left = ch[0];
        float* right = ch[1];
        for (int i = 0; i < numFrames; ++i) {
            const float w = width_.next();
            const float mid = 0.5f * (left[i] + right[i]);
            const float side = 0.5f * (left[i] - right[i]) * w;
            left[i] = (mid + side) * gainL_.next();
            right[i] = (mid - side) * gainR_.next();
        }
    }

private:
    static const std::vector<ParamInfo>& infos() {
        static const std::vector<ParamInfo> kInfos = {
            {"gain", "Gain", "dB", -60.f, 24.f, 0.f},
            {"pan", "Pan", "", -1.f, 1.f, 0.f},
            {"width", "Width", "%", 0.f, 200.f, 100.f},
        };
        return kInfos;
    }

    void targets(float& l, float& r) const {
        const float g = dbToGain(param(Gain));
        balanceGains(param(Pan), l, r);
        l *= g;
        r *= g;
    }

    SmoothedValue gainL_, gainR_, width_;
};

}  // namespace gil
