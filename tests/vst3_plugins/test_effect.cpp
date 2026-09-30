// GIL Test Effect: see test_plugins.cpp.

// First, so that its setState renaming applies (see test_plugins.h).
#include "public.sdk/source/vst/vstsinglecomponenteffect.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "public.sdk/source/common/pluginview.h"
#include "test_plugins.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

extern HINSTANCE ghInst;  // this module (dllmain.cpp)

namespace gil_test {

const wchar_t* const kEffectViewClass = L"GILTestEffectView";

enum EffectParam : ParamID { kFxGain = 0, kFxLatency, kFxBypass };
constexpr int32 kMaxLatency = 4096;
constexpr uint32 kEditGainMessage = WM_USER + 1;  // the editor edits Gain like a user dragging a knob
constexpr uint32 kResizeMessage = WM_USER + 2;    // the editor asks the host for a new size
constexpr uint32 kDirtyMessage = WM_USER + 3;     // the controller says its state changed

class EffectView;

class TestEffect : public SingleComponentEffect {
public:
    tresult PLUGIN_API initialize(FUnknown* context) override {
        const tresult result = SingleComponentEffect::initialize(context);
        if (result != kResultOk) return result;
        addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        parameters.addParameter(new RangeParameter(STR16("Gain"), kFxGain, STR16("x"), 0.0, 2.0, 1.0));
        parameters.addParameter(new RangeParameter(STR16("Latency"), kFxLatency, STR16("smp"), 0.0, kMaxLatency,
                                                   0.0, kMaxLatency));
        parameters.addParameter(STR16("Bypass"), nullptr, 1, 0.0,
                                ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass, kFxBypass);
        return kResultOk;
    }

    // The latency follows its parameter at once (the host reads it after the
    // restart it is asked for), so it doesn't wait for the next process call.
    tresult PLUGIN_API setParamNormalized(ParamID tag, ParamValue value) override {
        const tresult result = SingleComponentEffect::setParamNormalized(tag, value);
        if (tag == kFxLatency) {
            const auto samples = static_cast<int32>(std::lround(value * kMaxLatency));
            if (latency_.exchange(samples) != samples && componentHandler) {
                componentHandler->restartComponent(kLatencyChanged);
            }
        }
        return result;
    }

    uint32 PLUGIN_API getLatencySamples() override { return static_cast<uint32>(latency_.load()); }

    tresult PLUGIN_API setActive(TBool) override {
        for (auto& line : delay_) std::fill(line.begin(), line.end(), 0.f);
        write_ = 0;
        return kResultOk;
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        for (auto& line : delay_) line.assign(2 * kMaxLatency, 0.f);
        return SingleComponentEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API setProcessing(TBool) override { return kResultOk; }

    tresult PLUGIN_API process(ProcessData& data) override {
        if (IParameterChanges* changes = data.inputParameterChanges) {
            for (int32 i = 0; i < changes->getParameterCount(); ++i) {
                IParamValueQueue* queue = changes->getParameterData(i);
                ParamValue value = 0.0;
                if (!queue || !lastValue(queue, value)) continue;
                if (queue->getParameterId() == kFxGain) gain_ = 2.0 * value;
                if (queue->getParameterId() == kFxBypass) bypass_ = value >= 0.5;
            }
        }
        if (data.numInputs < 1 || data.numOutputs < 1 || data.numSamples <= 0) return kResultOk;
        const int32 latency = latency_.load();
        const float gain = bypass_ ? 1.f : static_cast<float>(gain_);
        const int32 channels = std::min({data.inputs[0].numChannels, data.outputs[0].numChannels, 2});
        const auto size = static_cast<int32>(delay_[0].size());
        for (int32 i = 0; i < data.numSamples; ++i) {
            for (int32 c = 0; c < channels; ++c) {
                delay_[c][(write_ + i) % size] = data.inputs[0].channelBuffers32[c][i];
            }
            for (int32 c = 0; c < channels; ++c) {
                data.outputs[0].channelBuffers32[c][i] = delay_[c][(write_ + i - latency + size) % size] * gain;
            }
        }
        write_ = (write_ + data.numSamples) % size;
        return kResultOk;
    }

    tresult PLUGIN_API setState(IBStream* state) override {
        IBStreamer s(state, kLittleEndian);
        int32 magic = 0;
        double gain = 0.5, latency = 0.0, bypass = 0.0;
        if (!s.readInt32(magic) || magic != 0x47494C46 || !s.readDouble(gain) || !s.readDouble(latency) ||
            !s.readDouble(bypass)) {
            return kResultFalse;
        }
        gain_ = 2.0 * gain;
        bypass_ = bypass >= 0.5;
        setParamNormalized(kFxGain, gain);
        setParamNormalized(kFxLatency, latency);
        setParamNormalized(kFxBypass, bypass);
        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream* state) override {
        IBStreamer s(state, kLittleEndian);
        s.writeInt32(0x47494C46);  // "GILF"
        s.writeDouble(getParamNormalized(kFxGain));
        s.writeDouble(getParamNormalized(kFxLatency));
        s.writeDouble(getParamNormalized(kFxBypass));
        return kResultOk;
    }

    IPlugView* PLUGIN_API createView(FIDString name) override;

    // Called by the editor, as a real plug-in's knob would.
    void editGain() {
        beginEdit(kFxGain);
        for (const ParamValue value : {0.25, 0.3}) {
            SingleComponentEffect::setParamNormalized(kFxGain, value);
            performEdit(kFxGain, value);
        }
        endEdit(kFxGain);
    }
    void markDirty() { setDirty(true); }

private:
    std::atomic<int32> latency_{0};
    double gain_ = 1.0;
    bool bypass_ = false;
    std::vector<float> delay_[2];
    int32 write_ = 0;
};

class EffectView : public CPluginView {
public:
    explicit EffectView(TestEffect* effect) : effect_(effect) {
        ViewRect initial(0, 0, 400, 300);
        setRect(initial);
    }

    tresult PLUGIN_API isPlatformTypeSupported(FIDString type) override {
        return strcmp(type, kPlatformTypeHWND) == 0 ? kResultTrue : kResultFalse;
    }
    tresult PLUGIN_API canResize() override { return kResultTrue; }
    tresult PLUGIN_API checkSizeConstraint(ViewRect* rect) override {
        rect->right = rect->left + std::max<int32>(rect->getWidth(), 200);
        rect->bottom = rect->top + std::max<int32>(rect->getHeight(), 150);
        return kResultTrue;
    }
    tresult PLUGIN_API onSize(ViewRect* newSize) override {
        CPluginView::onSize(newSize);
        if (hwnd_) SetWindowPos(hwnd_, nullptr, 0, 0, rect.getWidth(), rect.getHeight(), SWP_NOZORDER | SWP_NOMOVE);
        return kResultTrue;
    }

    void attachedToParent() override {
        WNDCLASSW wc{};
        if (!GetClassInfoW(ghInst, kEffectViewClass, &wc)) {
            wc.lpfnWndProc = &EffectView::windowProc;
            wc.hInstance = ghInst;
            wc.lpszClassName = kEffectViewClass;
            wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(GRAY_BRUSH));
            RegisterClassW(&wc);
        }
        hwnd_ = CreateWindowExW(0, kEffectViewClass, L"GIL Test Effect", WS_CHILD | WS_VISIBLE, 0, 0, rect.getWidth(),
                                rect.getHeight(), static_cast<HWND>(systemWindow), nullptr, ghInst, nullptr);
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    }
    void removedFromParent() override {
        if (hwnd_) DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }

private:
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* view = reinterpret_cast<EffectView*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (view) {
            if (message == kEditGainMessage) {
                view->effect_->editGain();
                return 1;
            }
            if (message == kResizeMessage) {
                ViewRect wanted(0, 0, static_cast<int32>(wParam), static_cast<int32>(lParam));
                return view->plugFrame && view->plugFrame->resizeView(view, &wanted) == kResultTrue;
            }
            if (message == kDirtyMessage) {
                view->effect_->markDirty();
                return 1;
            }
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    TestEffect* effect_;
    HWND hwnd_ = nullptr;
};

IPlugView* PLUGIN_API TestEffect::createView(FIDString name) {
    return strcmp(name, ViewType::kEditor) == 0 ? new EffectView(this) : nullptr;
}

FUnknown* createTestEffect(void*) { return static_cast<IAudioProcessor*>(new TestEffect); }

}  // namespace gil_test
