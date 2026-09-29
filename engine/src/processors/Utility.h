#pragma once
// Built-in "Utility" device: gain, pan and stereo width.

#include <array>
#include <atomic>

#include "Processor.h"
#include "rt/RtUtils.h"

namespace gil {

class UtilityProcessor final : public Processor {
public:
    enum Param { Gain = 0, Pan, Width, NumParams };

    UtilityProcessor() {
        for (int i = 0; i < NumParams; ++i) values_[i].store(infos()[i].defaultValue);
    }

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
        width_.snapTo(values_[Width].load() / 100.f);
    }

    void process(const ProcessContext&, float* const* ch, int numChannels, int numFrames) override {
        float l, r;
        targets(l, r);
        gainL_.setTarget(l);
        gainR_.setTarget(r);
        width_.setTarget(values_[Width].load(std::memory_order_relaxed) / 100.f);

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

    const std::vector<ParamInfo>& params() const override { return infos(); }
    float getParam(int index) const override {
        return index >= 0 && index < NumParams ? values_[index].load() : 0.f;
    }
    void setParam(int index, float value) override {
        if (index < 0 || index >= NumParams) return;
        const auto& info = infos()[index];
        values_[index].store(std::clamp(value, info.minValue, info.maxValue));
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
        const float g = dbToGain(values_[Gain].load(std::memory_order_relaxed));
        balanceGains(values_[Pan].load(std::memory_order_relaxed), l, r);
        l *= g;
        r *= g;
    }

    std::array<std::atomic<float>, NumParams> values_{};
    SmoothedValue gainL_, gainR_, width_;
};

}  // namespace gil
