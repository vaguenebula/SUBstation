// GIL Test Sidechain: see test_plugins.cpp.

// First, so that its setState renaming applies (see test_plugins.h).
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include <algorithm>

#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "test_plugins.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace gil_test {

enum SidechainParam : ParamID { kKeySilent = 0 };

class TestSidechain : public SingleComponentEffect {
public:
    tresult PLUGIN_API initialize(FUnknown* context) override {
        const tresult result = SingleComponentEffect::initialize(context);
        if (result != kResultOk) return result;
        addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        addAudioInput(STR16("Sidechain"), SpeakerArr::kStereo, kAux, 0);  // inactive until the host activates it
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        parameters.addParameter(STR16("Key Silent"), nullptr, 1, 1.0, ParameterInfo::kIsReadOnly, kKeySilent);
        return kResultOk;
    }

    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }
    tresult PLUGIN_API setState(IBStream*) override { return kResultOk; }
    tresult PLUGIN_API getState(IBStream*) override { return kResultOk; }

    // Its input plus its sidechain, sample for sample (while the host has the
    // sidechain bus active), and whether the host flagged the sidechain silent.
    tresult PLUGIN_API process(ProcessData& data) override {
        if (data.numInputs < 1 || data.numOutputs < 1 || data.numSamples <= 0) return kResultOk;
        const bool active = data.numInputs > 1 && audioInputs.size() > 1 && audioInputs[1]->isActive();
        const AudioBusBuffers* key = active && data.inputs[1].numChannels > 0 ? &data.inputs[1] : nullptr;
        const int32 channels = std::min({data.inputs[0].numChannels, data.outputs[0].numChannels, 2});
        for (int32 c = 0; c < channels; ++c) {
            const float* in = data.inputs[0].channelBuffers32[c];
            const float* k = key ? key->channelBuffers32[std::min(c, key->numChannels - 1)] : nullptr;
            float* out = data.outputs[0].channelBuffers32[c];
            for (int32 i = 0; i < data.numSamples; ++i) out[i] = in[i] + (k ? k[i] : 0.f);
        }
        const uint64 all = key ? (uint64(1) << std::min(key->numChannels, 63)) - 1 : 0;
        const bool silent = !key || (key->silenceFlags & all) == all;
        int32 index = 0;
        if (data.outputParameterChanges) {
            if (IParamValueQueue* queue = data.outputParameterChanges->addParameterData(kKeySilent, index)) {
                queue->addPoint(0, silent ? 1.0 : 0.0, index);
            }
        }
        return kResultOk;
    }
};

FUnknown* createTestSidechain(void*) { return static_cast<IAudioProcessor*>(new TestSidechain); }

}  // namespace gil_test
