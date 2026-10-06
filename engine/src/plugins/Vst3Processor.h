#pragma once
// A VST3 plug-in (instrument or effect) as a `Processor`.
//
// The track is stereo. The plug-in's main audio buses get stereo if it accepts
// it; a mono plug-in gets the channels mixed down and its output copied to
// both. Its first aux audio input is its sidechain (Processor::hasSidechain):
// arranged (stereo if it accepts it) and activated with the other buses, on the
// main thread, it hears the device's sidechain, or silence (flagged as such)
// without one. Every other bus gets a buffer of its own (silence in, output ignored).
// Notes go to the first event input bus; raw MIDI controllers are mapped to
// parameters through IMidiMapping.
//
// Parameters are presented as plain values: 0..1 for continuous ones, the step
// index for stepped ones (with the plug-in's value names for lists). Values
// travel to the audio thread through a lock-free queue; the plug-in's own
// output parameters come back through another and reach its controller (and
// the UI) in idle(). Automation goes to the processor sample-accurately with
// the block's other parameter changes; the last automated values reach the
// controller (so the plug-in's editor follows) and the UI in idle().
//
// Threads: process() and reset() on the rendering thread, all else on the main
// thread. The component handler may be called from anywhere by badly behaved
// plug-ins, so what it touches is thread-safe.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Processor.h"
#include "plugins/EditorWindow.h"
#include "plugins/Vst3Support.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/hosting/module.h"

namespace sub::vst3 {

class Vst3Processor final : public Processor {
public:
    // Creates, initialises and connects the plug-in. Throws std::runtime_error.
    Vst3Processor(VST3::Hosting::Module::Ptr module, const VST3::Hosting::ClassInfo& classInfo,
                  Steinberg::FUnknown* hostContext);
    ~Vst3Processor() override;

    std::string typeId() const override { return "vst3:" + classId_.toString(false); }
    std::string name() const override { return name_; }

    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void resetOffline() override;
    void process(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) override;
    int latencySamples() const override { return latency_.load(std::memory_order_relaxed); }
    bool hasSidechain() const override { return auxInput_ >= 0; }
    int tailSamples() const override { return tail_; }

    const std::vector<ParamInfo>& params() const override { return params_; }
    float getParam(int index) const override;
    void setParam(int index, float value) override;
    std::string paramText(int index, float value) const override;

    std::vector<uint8_t> getState() override;
    void setState(const std::vector<uint8_t>& state) override;
    // Another instance of the same class, from the same module, in this one's state.
    std::shared_ptr<Processor> createShadow(double sampleRate, int maxBlockSize) override;

    bool idle() override;
    void takeEvents(std::vector<ProcessorEvent>& out) override;

    bool hasEditor() const override { return controller_ != nullptr; }
    bool openEditor(void* ownerWindow, const std::string& title) override;
    void closeEditor() override;
    bool isEditorOpen() const override;
    bool setEditorVisible(bool visible) override;
    void setEditorTitle(const std::string& title) override;

private:
    class ComponentHandler;
    friend class ComponentHandler;

    struct ParamMeta {
        Steinberg::Vst::ParamID id = 0;
        int steps = 0;
        bool readOnly = false;
    };
    struct Bus {
        std::vector<std::vector<float>> channels;
        std::vector<float*> pointers;
    };
    struct Gesture {
        uint32_t serial = 0;
        float startValue = 0.f;
    };

    // Setup (main thread; the plug-in is not being processed).
    void connect();
    void disconnect();
    void activate();
    void deactivate();
    void setupBuses();
    void allocateBuffers();
    void buildParams();
    void refreshValues();
    void buildMidiMap();

    // Hands parameter changes still queued for the processor to it now, in a
    // process call without audio (the device may not be running).
    void flushParameters();
    void forwardOutputParameters();
    bool applyAutomatedValues();  // main thread; true if there were any

    // Rendering thread.
    void buildEvents(const ProcessContext& ctx);
    void addMidi(const ProcessEvent& event);
    void fillContext(const ProcessContext& ctx);

    // Component handler callbacks.
    void beginEdit(Steinberg::Vst::ParamID id);
    void performEdit(Steinberg::Vst::ParamID id, Steinberg::Vst::ParamValue value);
    void endEdit(Steinberg::Vst::ParamID id);
    void pushEvent(const ProcessorEvent& event);
    int indexOf(Steinberg::Vst::ParamID id) const;  // hold mutex_
    void dropEditor();

    // The module outlives everything the plug-in made, so it comes first.
    VST3::Hosting::Module::Ptr module_;
    VST3::UID classId_;
    std::string name_;
    std::unique_ptr<ComponentHandler> handler_;

    Steinberg::IPtr<Steinberg::Vst::IComponent> component_;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor_;
    Steinberg::IPtr<Steinberg::Vst::IEditController> controller_;
    Steinberg::IPtr<Steinberg::Vst::IMidiMapping> midiMapping_;
    Steinberg::IPtr<Steinberg::Vst::IConnectionPoint> componentPoint_;
    Steinberg::IPtr<Steinberg::Vst::IConnectionPoint> controllerPoint_;
    bool singleComponent_ = false;  // the component is its own controller
    bool active_ = false;
    bool processing_ = false;

    double sampleRate_ = 48000.0;
    int maxBlock_ = 0;
    std::atomic<int> latency_{0};
    int tail_ = 0;

    // Buses and buffers (rebuilt with the plug-in inactive).
    std::vector<Bus> inputBuses_, outputBuses_;
    std::vector<Steinberg::Vst::AudioBusBuffers> inputs_, outputs_;
    int mainInput_ = -1;   // audio bus indices, -1 = none
    int mainOutput_ = -1;
    int auxInput_ = -1;    // the sidechain
    int eventInput_ = -1;

    // Rendering-thread state.
    ProcessGuard guard_;
    Steinberg::Vst::ProcessData data_{};
    Steinberg::Vst::ProcessContext context_{};
    HostEventList events_;
    HostEventList outputEvents_;
    HostParamChanges inputChanges_;
    HostParamChanges outputChanges_;
    std::vector<Steinberg::Vst::ParamID> midiMap_;  // [channel * kCountCtrlNumber + controller]
    uint8_t held_[16][128] = {};                      // note-ons without a note-off yet
    std::atomic<bool> releaseAll_{false};
    int64_t continuousSamples_ = 0;

    ParamChangeQueue toAudio_;                      // parameter values for the plug-in's processor
    SpscQueue<ParamChange, 4096> fromAudio_;        // its output parameters, for its controller
    std::atomic<int32_t> pendingRestart_{0};        // IComponentHandler::restartComponent flags

    // Automation. The rendering thread's copy of the parameter ids (by index) and
    // the last automated value of each (normalized, 0-1; kNotAutomated: none since
    // the last idle()), set up with the buffers.
    static constexpr float kNotAutomated = -1.f;
    std::vector<Steinberg::Vst::ParamID> automationIds_;
    std::unique_ptr<std::atomic<float>[]> automated_;
    std::atomic<bool> automationPending_{false};
    // While the controller hears of automated values: some plug-ins answer that
    // with performEdit, which is no edit of the user's (it would override the automation).
    std::atomic<bool> syncingAutomation_{false};

    // Parameters. mutex_ guards the lists against a component-handler call
    // from another thread; values_ is read anywhere.
    mutable std::mutex mutex_;
    std::vector<ParamInfo> params_;
    std::vector<ParamMeta> meta_;
    std::unordered_map<Steinberg::Vst::ParamID, int> indices_;
    std::unique_ptr<std::atomic<float>[]> values_;
    std::unordered_map<Steinberg::Vst::ParamID, Gesture> gestures_;
    uint32_t gestureCounter_ = 0;
    std::vector<ProcessorEvent> pending_;

    std::unique_ptr<EditorWindow> editor_;
    std::optional<EditorWindow::Position> editorPosition_;  // where the last editor was: the next opens there
};

}  // namespace sub::vst3
