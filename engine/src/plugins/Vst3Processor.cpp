#include "plugins/Vst3Processor.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

#include "plugins/EditorWindow.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "public.sdk/source/vst/vstpresetfile.h"

namespace sub::vst3 {

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

constexpr int kMaxListSteps = 128;  // longer lists show as a knob with the plug-in's value text
constexpr int32 kMaxEvents = 2048;

float toPlain(ParamValue normalized, int steps) {
    normalized = std::clamp(normalized, 0.0, 1.0);
    if (steps <= 0) return static_cast<float>(normalized);
    return static_cast<float>(std::min<int>(steps, static_cast<int>(normalized * (steps + 1))));
}

ParamValue toNormalized(float plain, int steps) {
    if (steps <= 0) return std::clamp<double>(plain, 0.0, 1.0);
    return std::clamp<double>(std::round(plain), 0.0, steps) / steps;
}

std::string utf8(const TChar* text) { return StringConvert::convert(text); }

int channelCount(SpeakerArrangement arrangement) { return SpeakerArr::getChannelCount(arrangement); }

bool isSilent(const float* samples, int count) {
    for (int i = 0; i < count; ++i) {
        if (samples[i] != 0.f) return false;
    }
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// The plug-in's calls to us.

class Vst3Processor::ComponentHandler final : public IComponentHandler,
                                              public IComponentHandler2,
                                              public IUnitHandler {
public:
    explicit ComponentHandler(Vst3Processor& owner) : owner_(owner) {}

    tresult PLUGIN_API beginEdit(ParamID id) override {
        owner_.beginEdit(id);
        return kResultOk;
    }
    tresult PLUGIN_API performEdit(ParamID id, ParamValue valueNormalized) override {
        owner_.performEdit(id, valueNormalized);
        return kResultOk;
    }
    tresult PLUGIN_API endEdit(ParamID id) override {
        owner_.endEdit(id);
        return kResultOk;
    }
    // Restarts deactivate the plug-in, which it may not expect during this call:
    // they wait for the next idle().
    tresult PLUGIN_API restartComponent(int32 flags) override {
        owner_.pendingRestart_.fetch_or(flags);
        // Values or the plug-in itself changed: what it puts out may have too.
        if (flags & (kReloadComponent | kParamValuesChanged | kParamTitlesChanged | kIoChanged)) owner_.noteChange();
        return kResultOk;
    }

    tresult PLUGIN_API setDirty(TBool state) override {
        if (state) {
            owner_.noteChange();
            owner_.pushEvent({ProcessorEvent::Type::StateDirty});
        }
        return kResultOk;
    }
    tresult PLUGIN_API requestOpenEditor(FIDString) override {
        owner_.pushEvent({ProcessorEvent::Type::EditorRequested});
        return kResultOk;
    }
    tresult PLUGIN_API startGroupEdit() override { return kResultOk; }
    tresult PLUGIN_API finishGroupEdit() override { return kResultOk; }

    tresult PLUGIN_API notifyUnitSelection(UnitID) override { return kResultOk; }
    tresult PLUGIN_API notifyProgramListChange(ProgramListID, int32) override {
        owner_.pendingRestart_.fetch_or(kParamTitlesChanged);  // program names show in list parameters
        return kResultOk;
    }

    tresult PLUGIN_API queryInterface(const TUID queried, void** obj) override {
        if (FUnknownPrivate::iidEqual(queried, IComponentHandler::iid) ||
            FUnknownPrivate::iidEqual(queried, FUnknown::iid)) {
            *obj = static_cast<IComponentHandler*>(this);
        } else if (FUnknownPrivate::iidEqual(queried, IComponentHandler2::iid)) {
            *obj = static_cast<IComponentHandler2*>(this);
        } else if (FUnknownPrivate::iidEqual(queried, IUnitHandler::iid)) {
            *obj = static_cast<IUnitHandler*>(this);
        } else {
            *obj = nullptr;
            return kNoInterface;
        }
        return kResultOk;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

private:
    Vst3Processor& owner_;
};

// ---------------------------------------------------------------------------
// Life cycle

Vst3Processor::Vst3Processor(VST3::Hosting::Module::Ptr module, const VST3::Hosting::ClassInfo& classInfo,
                             FUnknown* hostContext)
    : module_(std::move(module)),
      classId_(classInfo.ID()),
      name_(classInfo.name()),
      handler_(std::make_unique<ComponentHandler>(*this)) {
    const auto& factory = module_->getFactory();
    component_ = factory.createInstance<IComponent>(classId_);
    if (!component_) throw std::runtime_error(name_ + " could not be created.");
    if (component_->initialize(hostContext) != kResultOk) {
        component_ = nullptr;
        throw std::runtime_error(name_ + " could not be initialized.");
    }
    processor_ = FUnknownPtr<IAudioProcessor>(component_);
    if (!processor_) {
        component_->terminate();
        component_ = nullptr;
        throw std::runtime_error(name_ + " does not process audio.");
    }

    // The controller: the component itself (single-component plug-ins, like
    // most JUCE ones), or a class of its own.
    controller_ = FUnknownPtr<IEditController>(component_);
    singleComponent_ = controller_ != nullptr;
    if (!controller_) {
        TUID controllerId{};
        if (component_->getControllerClassId(controllerId) == kResultTrue) {
            controller_ = factory.createInstance<IEditController>(VST3::UID::fromTUID(controllerId));
            if (controller_ && controller_->initialize(hostContext) != kResultOk) controller_ = nullptr;
        }
    }
    if (controller_) {
        controller_->setComponentHandler(handler_.get());
        if (!singleComponent_) {
            connect();
            // The controller starts from the component's state.
            auto state = owned(new MemoryStream);
            if (component_->getState(state) == kResultOk) {
                state->seek(0, IBStream::kIBSeekSet, nullptr);
                controller_->setComponentState(state);
            }
        }
        midiMapping_ = FUnknownPtr<IMidiMapping>(controller_);
    }

    eventInput_ = component_->getBusCount(kEvent, kInput) > 0 ? 0 : -1;
    buildParams();
    buildMidiMap();
}

Vst3Processor::~Vst3Processor() {
    editor_.reset();
    deactivate();
    if (controller_) controller_->setComponentHandler(nullptr);
    disconnect();
    if (controller_ && !singleComponent_) controller_->terminate();
    midiMapping_ = nullptr;
    controller_ = nullptr;
    processor_ = nullptr;
    if (component_) component_->terminate();
    component_ = nullptr;
}

void Vst3Processor::connect() {
    FUnknownPtr<IConnectionPoint> componentPoint(component_);
    FUnknownPtr<IConnectionPoint> controllerPoint(controller_);
    if (!componentPoint || !controllerPoint) return;
    // Directly, not through the SDK's ConnectionProxy: it drops messages sent from
    // any thread but the main thread, and plug-ins send them from their own threads
    // too (FabFilter's editors ask for their analyzer data from their drawing thread).
    componentPoint_ = componentPoint;
    controllerPoint_ = controllerPoint;
    componentPoint_->connect(controllerPoint_);
    controllerPoint_->connect(componentPoint_);
}

void Vst3Processor::disconnect() {
    if (componentPoint_ && controllerPoint_) {
        componentPoint_->disconnect(controllerPoint_);
        controllerPoint_->disconnect(componentPoint_);
    }
    componentPoint_ = nullptr;
    controllerPoint_ = nullptr;
}

void Vst3Processor::activate() {
    component_->setActive(true);
    active_ = true;
    latency_.store(static_cast<int>(std::min<uint32>(processor_->getLatencySamples(), 1u << 22)));
    const uint32 tail = processor_->getTailSamples();
    tail_ = static_cast<int>(std::min<uint32>(tail, static_cast<uint32>(sampleRate_ * 60)));
    processor_->setProcessing(true);
    processing_ = true;
}

void Vst3Processor::deactivate() {
    if (processing_) processor_->setProcessing(false);
    if (active_) component_->setActive(false);
    processing_ = active_ = false;
}

void Vst3Processor::prepare(double sampleRate, int maxBlockSize) {
    ScopedSuspend suspend(guard_);
    deactivate();
    sampleRate_ = sampleRate;
    maxBlock_ = maxBlockSize;
    setupBuses();
    ProcessSetup setup{kRealtime, kSample32, maxBlockSize, sampleRate};
    processor_->setupProcessing(setup);
    allocateBuffers();
    activate();
}

void Vst3Processor::setupBuses() {
    const int32 numInputs = component_->getBusCount(kAudio, kInput);
    const int32 numOutputs = component_->getBusCount(kAudio, kOutput);
    const auto mainBus = [this](BusDirection direction, int32 count) {
        for (int32 i = 0; i < count; ++i) {
            BusInfo info{};
            if (component_->getBusInfo(kAudio, direction, i, info) == kResultOk && info.busType == kMain) return i;
        }
        return count > 0 ? 0 : -1;
    };
    mainInput_ = mainBus(kInput, numInputs);
    mainOutput_ = mainBus(kOutput, numOutputs);
    auxInput_ = -1;
    for (int32 i = 0; i < numInputs && auxInput_ < 0; ++i) {
        BusInfo info{};
        if (i != mainInput_ && component_->getBusInfo(kAudio, kInput, i, info) == kResultOk && info.busType == kAux) {
            auxInput_ = i;
        }
    }

    // Stereo main buses (and sidechain) if the plug-in takes them; else stereo
    // main buses and its own sidechain; else its own layout, as it was. (A
    // refused arrangement may leave any bus changed: each try asks for every one.)
    std::vector<SpeakerArrangement> ownInputs(numInputs, SpeakerArr::kStereo);
    std::vector<SpeakerArrangement> ownOutputs(numOutputs, SpeakerArr::kStereo);
    for (int32 i = 0; i < numInputs; ++i) processor_->getBusArrangement(kInput, i, ownInputs[i]);
    for (int32 i = 0; i < numOutputs; ++i) processor_->getBusArrangement(kOutput, i, ownOutputs[i]);
    const auto arrange = [&](bool stereoMain, bool stereoAux) {
        auto inputs = ownInputs;
        auto outputs = ownOutputs;
        if (stereoMain && mainInput_ >= 0) inputs[mainInput_] = SpeakerArr::kStereo;
        if (stereoMain && mainOutput_ >= 0) outputs[mainOutput_] = SpeakerArr::kStereo;
        if (stereoAux && auxInput_ >= 0) inputs[auxInput_] = SpeakerArr::kStereo;
        return processor_->setBusArrangements(inputs.data(), numInputs, outputs.data(), numOutputs) == kResultTrue;
    };
    if (!arrange(true, true) && !(auxInput_ >= 0 && arrange(true, false))) arrange(false, false);

    if (mainInput_ >= 0) component_->activateBus(kAudio, kInput, mainInput_, true);
    if (auxInput_ >= 0 && component_->activateBus(kAudio, kInput, auxInput_, true) != kResultTrue) {
        auxInput_ = -1;  // it won't process that bus: no sidechain (the bus gets silence, as other extras)
    }
    if (mainOutput_ >= 0) component_->activateBus(kAudio, kOutput, mainOutput_, true);
    if (eventInput_ >= 0) component_->activateBus(kEvent, kInput, eventInput_, true);

    // What it settled on.
    const auto readBuses = [this](BusDirection direction, int32 count, std::vector<Bus>& buses) {
        buses.assign(static_cast<size_t>(count), {});
        for (int32 i = 0; i < count; ++i) {
            SpeakerArrangement arrangement = 0;
            int channels = 0;
            if (processor_->getBusArrangement(direction, i, arrangement) == kResultOk) {
                channels = channelCount(arrangement);
            } else {
                BusInfo info{};
                if (component_->getBusInfo(kAudio, direction, i, info) == kResultOk) channels = info.channelCount;
            }
            buses[i].channels.resize(static_cast<size_t>(std::max(0, channels)));
        }
    };
    readBuses(kInput, numInputs, inputBuses_);
    readBuses(kOutput, numOutputs, outputBuses_);
    if (auxInput_ >= 0 && inputBuses_[static_cast<size_t>(auxInput_)].channels.empty()) {
        auxInput_ = -1;  // it settled on no channels there: no sidechain
    }
}

void Vst3Processor::allocateBuffers() {
    const auto allocate = [this](std::vector<Bus>& buses, std::vector<AudioBusBuffers>& buffers) {
        buffers.assign(buses.size(), AudioBusBuffers{});
        for (size_t b = 0; b < buses.size(); ++b) {
            Bus& bus = buses[b];
            bus.pointers.clear();
            for (auto& channel : bus.channels) {
                channel.assign(static_cast<size_t>(maxBlock_), 0.f);
                bus.pointers.push_back(channel.data());
            }
            buffers[b].numChannels = static_cast<int32>(bus.channels.size());
            buffers[b].channelBuffers32 = bus.pointers.data();
        }
    };
    allocate(inputBuses_, inputs_);
    allocate(outputBuses_, outputs_);

    size_t queues = 64;
    {
        std::lock_guard lock(mutex_);
        queues = std::clamp<size_t>(params_.size() + 64, 128, 4096);
        automationIds_.clear();
        for (const ParamMeta& meta : meta_) automationIds_.push_back(meta.id);
    }
    automated_ = std::make_unique<std::atomic<float>[]>(automationIds_.size());
    for (size_t i = 0; i < automationIds_.size(); ++i) automated_[i].store(kNotAutomated);
    automationPending_.store(false);
    events_.setCapacity(kMaxEvents);
    outputEvents_.setCapacity(kMaxEvents);
    inputChanges_.setCapacity(queues);
    outputChanges_.setCapacity(queues);

    data_ = ProcessData{};
    data_.processMode = kRealtime;
    data_.symbolicSampleSize = kSample32;
    data_.numInputs = static_cast<int32>(inputs_.size());
    data_.numOutputs = static_cast<int32>(outputs_.size());
    data_.inputs = inputs_.empty() ? nullptr : inputs_.data();
    data_.outputs = outputs_.empty() ? nullptr : outputs_.data();
    data_.inputParameterChanges = &inputChanges_;
    data_.outputParameterChanges = &outputChanges_;
    data_.inputEvents = eventInput_ >= 0 ? &events_ : nullptr;
    data_.outputEvents = &outputEvents_;
    data_.processContext = &context_;
}

void Vst3Processor::resetOffline() {
    ScopedSuspend suspend(guard_);
    deactivate();
    activate();
    std::memset(held_, 0, sizeof(held_));
    releaseAll_.store(false);
}

void Vst3Processor::reset() { releaseAll_.store(true, std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// Processing

void Vst3Processor::process(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) {
    if (numFrames <= 0 || numFrames > maxBlock_ || numChannels < 1) return;
    if (!guard_.tryEnter()) return;  // the main thread has the plug-in: the audio passes through

    buildEvents(ctx);
    ParamChange change;
    while (toAudio_.pop(change)) inputChanges_.add(change.id, 0, change.value);
    const ParamAutomation* changes = automation();
    const size_t numChanges = numAutomation();
    for (size_t i = 0; i < numChanges; ++i) {
        const auto index = static_cast<size_t>(changes[i].index);
        if (index >= automationIds_.size()) continue;
        inputChanges_.add(automationIds_[index], std::clamp<int32>(changes[i].sampleOffset, 0, numFrames - 1),
                          changes[i].value);
        automated_[index].store(changes[i].value, std::memory_order_relaxed);
    }
    if (numChanges > 0) automationPending_.store(true, std::memory_order_release);
    fillContext(ctx);

    float* left = channels[0];
    float* right = numChannels > 1 ? channels[1] : channels[0];
    const float* keyLeft = sidechain(0);
    const float* keyRight = sidechain(1) ? sidechain(1) : keyLeft;
    for (size_t b = 0; b < inputBuses_.size(); ++b) {
        Bus& bus = inputBuses_[b];
        const int count = static_cast<int>(bus.channels.size());
        // The track's signal into the main input, the sidechain into the aux one.
        const float* fromLeft = static_cast<int>(b) == mainInput_ ? left : nullptr;
        const float* fromRight = right;
        if (static_cast<int>(b) == auxInput_ && keyLeft) {
            fromLeft = keyLeft;
            fromRight = keyRight;
        }
        if (fromLeft && count > 0) {
            if (count == 1) {
                float* mono = bus.pointers[0];
                for (int i = 0; i < numFrames; ++i) mono[i] = 0.5f * (fromLeft[i] + fromRight[i]);
            } else {
                std::copy_n(fromLeft, numFrames, bus.pointers[0]);
                std::copy_n(fromRight, numFrames, bus.pointers[1]);
                for (int c = 2; c < count; ++c) std::fill_n(bus.pointers[c], numFrames, 0.f);
            }
            uint64 silence = 0;
            for (int c = 0; c < count && c < 64; ++c) {
                if (isSilent(bus.pointers[c], numFrames)) silence |= uint64(1) << c;
            }
            inputs_[b].silenceFlags = silence;
        } else {
            for (int c = 0; c < count; ++c) std::fill_n(bus.pointers[c], numFrames, 0.f);
            inputs_[b].silenceFlags = count >= 64 ? ~uint64(0) : (uint64(1) << count) - 1;
        }
    }
    for (auto& output : outputs_) output.silenceFlags = 0;

    data_.numSamples = numFrames;
    const bool processed = processor_->process(data_) == kResultOk;

    if (processed && mainOutput_ >= 0 && !outputBuses_[mainOutput_].channels.empty()) {
        const Bus& bus = outputBuses_[mainOutput_];
        std::copy_n(bus.pointers[0], numFrames, left);
        if (right != left) std::copy_n(bus.pointers[bus.pointers.size() > 1 ? 1 : 0], numFrames, right);
    }

    forwardOutputParameters();
    inputChanges_.clear();
    events_.clear();
    outputEvents_.clear();
    continuousSamples_ += numFrames;
    guard_.leave();
}

// The plug-in's output parameters (meters, values it changed itself) go to
// its controller in idle().
void Vst3Processor::forwardOutputParameters() {
    for (int32 i = 0; i < outputChanges_.getParameterCount(); ++i) {
        HostParamQueue& queue = outputChanges_.queue(i);
        if (queue.getPointCount() > 0) fromAudio_.push({queue.getParameterId(), queue.lastValue()});
    }
    outputChanges_.clear();
}

void Vst3Processor::flushParameters() {
    if (toAudio_.empty() || !processing_) return;
    ScopedSuspend suspend(guard_);  // the rendering thread keeps off; we are the processor's only caller
    inputChanges_.clear();
    ParamChange change;
    while (toAudio_.pop(change)) inputChanges_.add(change.id, 0, change.value);
    if (inputChanges_.getParameterCount() == 0) return;
    events_.clear();
    data_.numSamples = 0;
    processor_->process(data_);
    forwardOutputParameters();
    inputChanges_.clear();
    outputEvents_.clear();
}

void Vst3Processor::buildEvents(const ProcessContext& ctx) {
    events_.clear();
    if (eventInput_ < 0) {
        releaseAll_.store(false, std::memory_order_relaxed);
        return;
    }
    Event event{};
    event.busIndex = eventInput_;
    if (releaseAll_.exchange(false, std::memory_order_relaxed)) {
        for (int16 channel = 0; channel < 16; ++channel) {
            for (int16 key = 0; key < 128; ++key) {
                if (!held_[channel][key]) continue;
                held_[channel][key] = 0;
                event.sampleOffset = 0;
                event.type = Event::kNoteOffEvent;
                event.noteOff = {channel, key, 0.f, -1, 0.f};
                events_.add(event);
            }
        }
    }
    for (size_t e = 0; e < ctx.inEvents.count; ++e) {
        const ProcessEvent& in = ctx.inEvents.events[e];
        const auto channel = static_cast<int16>(in.channel() & 0x0F);
        const auto key = static_cast<int16>(in.key() & 0x7F);
        event.sampleOffset = in.sampleOffset;
        switch (in.type) {
            case ProcessEvent::Type::NoteOn:
                if (in.velocity() > 0) {
                    event.type = Event::kNoteOnEvent;
                    event.noteOn = {channel, key, 0.f, in.velocity() / 127.f, 0, -1};
                    if (events_.add(event) && held_[channel][key] < 255) ++held_[channel][key];
                    break;
                }
                [[fallthrough]];  // velocity 0 releases
            case ProcessEvent::Type::NoteOff:
                event.type = Event::kNoteOffEvent;
                event.noteOff = {channel, key, 0.f, -1, 0.f};
                if (events_.add(event) && held_[channel][key] > 0) --held_[channel][key];
                break;
            case ProcessEvent::Type::Midi:
                addMidi(in);
                break;
        }
    }
}

void Vst3Processor::addMidi(const ProcessEvent& in) {
    const uint8_t status = in.data[0] & 0xF0;
    const int channel = in.data[0] & 0x0F;
    const auto mapped = [&](int controller) {
        return midiMap_.empty() ? kNoParamId : midiMap_[channel * kCountCtrlNumber + controller];
    };
    ParamID param = kNoParamId;
    ParamValue value = 0.0;
    switch (status) {
        case 0xB0:  // control change
            param = mapped(in.data[1] & 0x7F);
            value = (in.data[2] & 0x7F) / 127.0;
            break;
        case 0xD0:  // channel pressure
            param = mapped(kAfterTouch);
            value = (in.data[1] & 0x7F) / 127.0;
            break;
        case 0xE0:  // pitch bend
            param = mapped(kPitchBend);
            value = (((in.data[2] & 0x7F) << 7) | (in.data[1] & 0x7F)) / 16383.0;
            break;
        case 0xA0: {  // polyphonic key pressure is an event of its own
            Event event{};
            event.busIndex = eventInput_;
            event.sampleOffset = in.sampleOffset;
            event.type = Event::kPolyPressureEvent;
            event.polyPressure = {static_cast<int16>(channel), static_cast<int16>(in.data[1] & 0x7F),
                                  (in.data[2] & 0x7F) / 127.f, -1};
            events_.add(event);
            return;
        }
        default:
            return;
    }
    if (param != kNoParamId) inputChanges_.add(param, in.sampleOffset, value);
}

void Vst3Processor::fillContext(const ProcessContext& ctx) {
    Vst::ProcessContext& c = context_;
    c = {};
    c.state = Vst::ProcessContext::kTempoValid | Vst::ProcessContext::kTimeSigValid |
              Vst::ProcessContext::kProjectTimeMusicValid | Vst::ProcessContext::kBarPositionValid |
              Vst::ProcessContext::kContTimeValid | Vst::ProcessContext::kSystemTimeValid;
    if (ctx.playing) c.state |= Vst::ProcessContext::kPlaying;
    if (ctx.looping) {
        c.state |= Vst::ProcessContext::kCycleActive | Vst::ProcessContext::kCycleValid;
        c.cycleStartMusic = ctx.loopStartBeat;
        c.cycleEndMusic = ctx.loopEndBeat;
    }
    c.sampleRate = ctx.sampleRate;
    c.projectTimeSamples = ctx.samplePos;
    c.continousTimeSamples = continuousSamples_;
    c.systemTime = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
    c.projectTimeMusic = ctx.beatPos;
    const double beatsPerBar = ctx.timeSigNum * 4.0 / ctx.timeSigDen;
    c.barPositionMusic = std::floor(ctx.beatPos / beatsPerBar + 1e-9) * beatsPerBar;
    c.tempo = ctx.tempo;
    c.timeSigNumerator = ctx.timeSigNum;
    c.timeSigDenominator = ctx.timeSigDen;
}

// ---------------------------------------------------------------------------
// Parameters

void Vst3Processor::buildParams() {
    std::vector<ParamInfo> params;
    std::vector<ParamMeta> meta;
    std::unordered_map<ParamID, int> indices;
    const int32 count = controller_ ? controller_->getParameterCount() : 0;
    for (int32 i = 0; i < count; ++i) {
        Vst::ParameterInfo info{};
        if (controller_->getParameterInfo(i, info) != kResultOk) continue;
        ParamInfo param;
        param.id = std::to_string(info.id);
        param.name = utf8(info.title);
        if (param.name.empty()) param.name = utf8(info.shortTitle);
        if (param.name.empty()) param.name = "Parameter " + std::to_string(i + 1);
        param.unit = utf8(info.units);
        const int steps = std::max<int32>(0, info.stepCount);
        param.steps = steps;
        param.minValue = 0.f;
        param.maxValue = steps > 0 ? static_cast<float>(steps) : 1.f;
        param.defaultValue = toPlain(info.defaultNormalizedValue, steps);
        param.automatable = (info.flags & Vst::ParameterInfo::kCanAutomate) != 0;
        param.readOnly = (info.flags & Vst::ParameterInfo::kIsReadOnly) != 0;
        // The device's own on/off switch stands for the plug-in's bypass.
        param.hidden = (info.flags & (Vst::ParameterInfo::kIsHidden | Vst::ParameterInfo::kIsBypass)) != 0;
        const bool list = (info.flags & Vst::ParameterInfo::kIsList) != 0 || steps == 1;
        if (list && steps > 0 && steps <= kMaxListSteps) {
            for (int s = 0; s <= steps; ++s) {
                String128 text{};
                std::string label;
                if (controller_->getParamStringByValue(info.id, static_cast<ParamValue>(s) / steps, text) == kResultOk) {
                    label = utf8(text);
                }
                param.valueLabels.push_back(label.empty() ? std::to_string(s) : label);
            }
            // Some plug-ins name only the current value, whatever value they are asked about.
            auto sorted = param.valueLabels;
            std::sort(sorted.begin(), sorted.end());
            if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
                param.valueLabels.clear();
                if (steps == 1) param.valueLabels = {"Off", "On"};
            }
        }
        indices[info.id] = static_cast<int>(params.size());
        meta.push_back({info.id, steps, param.readOnly});
        params.push_back(std::move(param));
    }

    auto values = std::make_unique<std::atomic<float>[]>(params.size());
    for (size_t i = 0; i < params.size(); ++i) {
        values[i].store(toPlain(controller_->getParamNormalized(meta[i].id), meta[i].steps));
    }
    std::lock_guard lock(mutex_);
    params_ = std::move(params);
    meta_ = std::move(meta);
    indices_ = std::move(indices);
    values_ = std::move(values);
    gestures_.clear();
}

void Vst3Processor::refreshValues() {
    std::vector<ParamMeta> meta;
    {
        std::lock_guard lock(mutex_);
        meta = meta_;
    }
    // Asked without holding the lock: the plug-in may call us back.
    std::vector<float> values(meta.size());
    for (size_t i = 0; i < meta.size(); ++i) values[i] = toPlain(controller_->getParamNormalized(meta[i].id), meta[i].steps);
    std::lock_guard lock(mutex_);
    if (meta_.size() != meta.size()) return;  // the list changed meanwhile; buildParams read fresh values
    for (size_t i = 0; i < meta.size(); ++i) values_[i].store(values[i]);
}

void Vst3Processor::buildMidiMap() {
    std::vector<ParamID> map;
    if (midiMapping_ && eventInput_ >= 0) {
        map.assign(16 * kCountCtrlNumber, kNoParamId);
        for (int16 channel = 0; channel < 16; ++channel) {
            for (int32 controller = 0; controller < kCountCtrlNumber; ++controller) {
                ParamID id = kNoParamId;
                if (midiMapping_->getMidiControllerAssignment(eventInput_, channel,
                                                              static_cast<CtrlNumber>(controller), id) == kResultTrue) {
                    map[channel * kCountCtrlNumber + controller] = id;
                }
            }
        }
    }
    midiMap_ = std::move(map);
}

int Vst3Processor::indexOf(ParamID id) const {
    const auto it = indices_.find(id);
    return it == indices_.end() ? -1 : it->second;
}

float Vst3Processor::getParam(int index) const {
    std::lock_guard lock(mutex_);
    return index >= 0 && index < static_cast<int>(meta_.size()) ? values_[index].load() : 0.f;
}

void Vst3Processor::setParam(int index, float value) {
    ParamMeta meta;
    {
        std::lock_guard lock(mutex_);
        if (index < 0 || index >= static_cast<int>(meta_.size()) || meta_[index].readOnly) return;
        meta = meta_[index];
        values_[index].store(toPlain(toNormalized(value, meta.steps), meta.steps));
    }
    const ParamValue normalized = toNormalized(value, meta.steps);
    controller_->setParamNormalized(meta.id, normalized);
    toAudio_.push(meta.id, normalized);
    noteChange();
}

std::string Vst3Processor::paramText(int index, float value) const {
    ParamMeta meta;
    {
        std::lock_guard lock(mutex_);
        if (index < 0 || index >= static_cast<int>(meta_.size())) return {};
        meta = meta_[index];
    }
    String128 text{};
    if (controller_->getParamStringByValue(meta.id, toNormalized(value, meta.steps), text) != kResultOk) return {};
    return utf8(text);
}

// The plug-in's editor changed a parameter: the processor must hear of it, and
// the UI records it as an edit.
void Vst3Processor::beginEdit(ParamID id) {
    std::lock_guard lock(mutex_);
    const int index = indexOf(id);
    if (index < 0) return;
    gestures_[id] = {++gestureCounter_, values_[index].load()};
    ProcessorEvent event;  // clicking a control, before it changes anything: its automation shows
    event.type = ProcessorEvent::Type::ParamTouched;
    event.paramIndex = index;
    pending_.push_back(event);
}

void Vst3Processor::performEdit(ParamID id, ParamValue value) {
    if (syncingAutomation_.load()) return;  // an echo of the automated value we just gave it
    toAudio_.push(id, value);
    noteChange();
    std::lock_guard lock(mutex_);
    const int index = indexOf(id);
    if (index < 0) return;
    const float plain = toPlain(value, meta_[index].steps);
    const float old = values_[index].exchange(plain);
    ProcessorEvent event;
    event.type = ProcessorEvent::Type::ParamEdited;
    event.paramIndex = index;
    event.value = plain;
    event.oldValue = old;
    if (const auto gesture = gestures_.find(id); gesture != gestures_.end()) {
        event.gesture = gesture->second.serial;
        event.oldValue = gesture->second.startValue;
    }
    pending_.push_back(event);
}

void Vst3Processor::endEdit(ParamID id) {
    std::lock_guard lock(mutex_);
    gestures_.erase(id);
}

void Vst3Processor::pushEvent(const ProcessorEvent& event) {
    std::lock_guard lock(mutex_);
    pending_.push_back(event);
}

void Vst3Processor::takeEvents(std::vector<ProcessorEvent>& out) {
    std::lock_guard lock(mutex_);
    out.insert(out.end(), pending_.begin(), pending_.end());
    pending_.clear();
}

// ---------------------------------------------------------------------------
// Main-thread housekeeping

bool Vst3Processor::idle() {
    if (editor_ && editor_->wasClosed()) {
        dropEditor();
        pushEvent({ProcessorEvent::Type::EditorClosed});
    }

    // Output parameters: the controller shows what the processor reports; and
    // what automation did.
    bool changed = applyAutomatedValues();
    ParamChange change;
    while (fromAudio_.pop(change)) {
        {
            std::lock_guard lock(mutex_);
            const int index = indexOf(change.id);
            if (index >= 0) values_[index].store(toPlain(change.value, meta_[index].steps));
        }
        if (controller_) controller_->setParamNormalized(change.id, change.value);
        changed = true;
    }

    bool latencyChanged = false;
    const int32 flags = pendingRestart_.exchange(0);
    if (flags & (kReloadComponent | kIoChanged | kLatencyChanged | kPrefetchableSupportChanged)) {
        const int before = latency_.load();
        ScopedSuspend suspend(guard_);
        deactivate();
        if (flags & (kReloadComponent | kIoChanged)) {
            setupBuses();
            ProcessSetup setup{kRealtime, kSample32, maxBlock_, sampleRate_};
            processor_->setupProcessing(setup);
            allocateBuffers();
        }
        activate();
        if (latency_.load() != before) {
            latencyChanged = true;
            pushEvent({ProcessorEvent::Type::LatencyChanged});
        }
    }
    if (flags & (kParamTitlesChanged | kReloadComponent)) {
        buildParams();
        ScopedSuspend suspend(guard_);  // the parameter queues are sized for the list
        allocateBuffers();
        pushEvent({ProcessorEvent::Type::ParamInfoChanged});
    } else if (flags & kParamValuesChanged) {
        if (controller_) refreshValues();
        changed = true;
    }
    if (flags & kMidiCCAssignmentChanged) {
        ScopedSuspend suspend(guard_);
        buildMidiMap();
    }
    if (changed) pushEvent({ProcessorEvent::Type::ParamsChanged});
    return latencyChanged;
}

bool Vst3Processor::applyAutomatedValues() {
    if (!automationPending_.exchange(false, std::memory_order_acquire)) return false;
    std::vector<std::pair<ParamID, ParamValue>> updates;
    {
        std::lock_guard lock(mutex_);
        // automated_ and meta_ index the same list: the buffers are set up after every parameter rebuild.
        const size_t count = std::min(automationIds_.size(), meta_.size());
        for (size_t i = 0; i < count; ++i) {
            const float value =
                automated_[i].exchange(kNotAutomated, std::memory_order_relaxed);
            if (value < 0.f) continue;
            values_[i].store(toPlain(value, meta_[i].steps));
            updates.emplace_back(meta_[i].id, value);
        }
    }
    // Without the lock: the plug-in may call us back.
    if (controller_) {
        syncingAutomation_.store(true);
        for (const auto& [id, value] : updates) controller_->setParamNormalized(id, value);
        syncingAutomation_.store(false);
    }
    return !updates.empty();
}

// ---------------------------------------------------------------------------
// State: a .vstpreset in memory (component state, plus the controller's own
// state for plug-ins with a separate controller).

std::vector<uint8_t> Vst3Processor::getState() {
    flushParameters();  // the processor's state must include the latest changes
    auto componentState = owned(new MemoryStream);
    component_->getState(componentState);
    componentState->seek(0, IBStream::kIBSeekSet, nullptr);
    IPtr<MemoryStream> controllerState;
    if (controller_ && !singleComponent_) {
        controllerState = owned(new MemoryStream);
        if (controller_->getState(controllerState) == kResultOk) {
            controllerState->seek(0, IBStream::kIBSeekSet, nullptr);
        } else {
            controllerState = nullptr;
        }
    }
    auto preset = owned(new MemoryStream);
    if (!PresetFile::savePreset(preset, FUID::fromTUID(classId_.data()), componentState, controllerState)) {
        throw std::runtime_error(name_ + " could not save its settings.");
    }
    const auto* bytes = reinterpret_cast<const uint8_t*>(preset->getData());
    return {bytes, bytes + preset->getSize()};
}

void Vst3Processor::setState(const std::vector<uint8_t>& state) {
    auto stream = owned(new MemoryStream);
    int32 written = 0;
    stream->write(const_cast<uint8_t*>(state.data()), static_cast<int32>(state.size()), &written);
    stream->seek(0, IBStream::kIBSeekSet, nullptr);
    bool loaded = false;
    {
        ScopedSuspend suspend(guard_);
        ParamChange stale;
        while (toAudio_.pop(stale)) {
        }  // changes from before must not undo the state
        loaded = PresetFile::loadPreset(stream, FUID::fromTUID(classId_.data()), component_,
                                        singleComponent_ ? nullptr : controller_.get());
    }
    if (!loaded) throw std::runtime_error("These settings are not for " + name_ + ".");
    noteChange();
    if (controller_) refreshValues();
    pushEvent({ProcessorEvent::Type::ParamsChanged});
}

// ---------------------------------------------------------------------------
// Editor

bool Vst3Processor::openEditor(void* ownerWindow, const std::string& title) {
    if (editor_ && editor_->isOpen()) {
        editor_->setTitle(title);
        editor_->bringToFront();
        return true;
    }
    dropEditor();
    if (!controller_) return false;
    IPtr<IPlugView> view = owned(controller_->createView(ViewType::kEditor));
    if (!view || view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue) return false;
    editor_ = std::make_unique<EditorWindow>(view, ownerWindow, title,
                                             editorPosition_ ? &*editorPosition_ : nullptr);
    if (!editor_->isOpen()) {
        editor_.reset();
        return false;
    }
    return true;
}

void Vst3Processor::closeEditor() { dropEditor(); }

void Vst3Processor::dropEditor() {
    if (!editor_) return;
    editorPosition_ = editor_->position();
    editor_.reset();
}

bool Vst3Processor::isEditorOpen() const { return editor_ && editor_->isOpen() && editor_->isVisible(); }

bool Vst3Processor::setEditorVisible(bool visible) {
    if (!editor_ || !editor_->isOpen()) return false;
    editor_->setVisible(visible);
    return true;
}

void Vst3Processor::setEditorTitle(const std::string& title) {
    if (editor_) editor_->setTitle(title);
}

}  // namespace sub::vst3
