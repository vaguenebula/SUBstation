// SUBstation's test plug-ins: five tiny VST3 plug-ins whose output the tests
// can predict exactly. They cover the host's code paths rather than sounding
// good:
//
//  * SUB Test Synth (instrument): a processor with a separate edit controller.
//    Each held note adds velocity/127 (DC) or a sine at the note's pitch, times
//    Gain; a note expression "tuning" for a note (by its id) bends its sine.
//    As a synth that reuses its voices can (Serum 2 appears to), a new note keeps
//    the tuning of the last note to end until a tuning of its own comes. It
//    reports the transport it was given in read-only parameters, and has ten
//    Macro parameters that only its controller keeps (in its own state).
//  * SUB Test Effect: a single-component effect (processor and controller in one
//    object, like most JUCE plug-ins). Gain, a Latency parameter that delays the
//    audio and reports it, a bypass parameter, state, and a Win32 editor that
//    can resize itself and edit a parameter on request (on Windows; elsewhere
//    it has no editor).
//  * SUB Test Mono: mono in, mono out, no edit controller.
//  * SUB Test Note Effect: an effect with an event input (as a vocoder or a
//    pitch corrector has), no edit controller: its output is its input plus,
//    for each held note, velocity/127 (DC).
//  * SUB Test Sidechain: a single-component effect with a sidechain (a stereo
//    aux input, inactive until the host activates it): its output is its input
//    plus its sidechain, and a read-only parameter, Key Silent, says whether the
//    host flagged the sidechain silent.
//
// Environment variables make the module misbehave on purpose, for the scanner
// tests: SUB_TEST_PLUGIN_CRASH=1 kills the process as the module loads,
// SUB_TEST_PLUGIN_HANG=1 makes loading hang.

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "base/source/fstreamer.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstnoteexpression.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/main/pluginfactory.h"
#include "public.sdk/source/vst/vstaudioeffect.h"
#include "public.sdk/source/vst/vsteditcontroller.h"
#include "test_plugins.h"

using namespace Steinberg;
using namespace Steinberg::Vst;

#ifdef _WIN32
extern HINSTANCE ghInst;  // this module (dllmain.cpp)
#endif

namespace sub_test {

constexpr double kPi = 3.14159265358979323846;

void writeOutputParam(ProcessData& data, ParamID id, ParamValue value) {
    if (!data.outputParameterChanges) return;
    int32 index = 0;
    if (IParamValueQueue* queue = data.outputParameterChanges->addParameterData(id, index)) {
        queue->addPoint(0, value, index);
    }
}

// ---------------------------------------------------------------------------
// SUB Test Synth

enum SynthParam : ParamID { kSynthGain = 0, kSynthWave, kSynthTempo, kSynthPlaying, kSynthBeat, kSynthLoop, kSynthMacro };
constexpr int kNumMacros = 10;

class SynthProcessor : public AudioEffect {
public:
    SynthProcessor() { setControllerClass(kSynthControllerUID); }
    static FUnknown* createInstance(void*) { return static_cast<IAudioProcessor*>(new SynthProcessor); }

    tresult PLUGIN_API initialize(FUnknown* context) override {
        const tresult result = AudioEffect::initialize(context);
        if (result != kResultOk) return result;
        addEventInput(STR16("Notes"), 16);
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        return kResultOk;
    }

    tresult PLUGIN_API setupProcessing(ProcessSetup& setup) override {
        sampleRate_ = setup.sampleRate;
        return AudioEffect::setupProcessing(setup);
    }

    tresult PLUGIN_API setActive(TBool state) override {
        notes_.clear();
        lastTuning_ = 0.0;
        return AudioEffect::setActive(state);
    }

    tresult PLUGIN_API process(ProcessData& data) override {
        if (IParameterChanges* changes = data.inputParameterChanges) {
            for (int32 i = 0; i < changes->getParameterCount(); ++i) {
                IParamValueQueue* queue = changes->getParameterData(i);
                ParamValue value = 0.0;
                if (!queue || !lastValue(queue, value)) continue;
                if (queue->getParameterId() == kSynthGain) gain_ = value;
                if (queue->getParameterId() == kSynthWave) sine_ = value >= 0.5;
            }
        }
        if (const ProcessContext* context = data.processContext) {
            writeOutputParam(data, kSynthTempo, context->tempo / 1000.0);
            writeOutputParam(data, kSynthPlaying, (context->state & ProcessContext::kPlaying) ? 1.0 : 0.0);
            writeOutputParam(data, kSynthBeat, context->projectTimeMusic / 1000.0);
            writeOutputParam(data, kSynthLoop, (context->state & ProcessContext::kCycleActive) ? 1.0 : 0.0);
        }
        if (data.numOutputs < 1 || data.outputs[0].numChannels < 1 || data.numSamples <= 0) return kResultOk;

        AudioBusBuffers& out = data.outputs[0];
        int32 position = 0;
        IEventList* events = data.inputEvents;
        const int32 numEvents = events ? events->getEventCount() : 0;
        for (int32 e = 0; e <= numEvents; ++e) {
            Event event{};
            int32 until = data.numSamples;
            if (e < numEvents) {
                if (events->getEvent(e, event) != kResultOk) continue;
                until = std::clamp(event.sampleOffset, position, data.numSamples);
            }
            render(out, position, until);
            position = until;
            if (e == numEvents) break;
            if (event.type == Event::kNoteOnEvent && event.noteOn.velocity > 0.f) {
                notes_.push_back({event.noteOn.pitch, event.noteOn.velocity, 0.0, event.noteOn.noteId, lastTuning_});
            } else if (event.type == Event::kNoteOnEvent || event.type == Event::kNoteOffEvent) {
                const int16 pitch = event.type == Event::kNoteOnEvent ? event.noteOn.pitch : event.noteOff.pitch;
                const int32 id = event.type == Event::kNoteOffEvent ? event.noteOff.noteId : -1;
                auto it = std::find_if(notes_.begin(), notes_.end(), [&](const Note& n) {
                    return id >= 0 ? n.noteId == id : n.pitch == pitch;
                });
                if (it != notes_.end()) {
                    lastTuning_ = it->tuning;  // (what its voice is left with)
                    notes_.erase(it);
                }
            } else if (event.type == Event::kNoteExpressionValueEvent &&
                       event.noteExpressionValue.typeId == kTuningTypeID) {
                for (Note& note : notes_) {
                    if (note.noteId == event.noteExpressionValue.noteId) {
                        note.tuning = 240.0 * (event.noteExpressionValue.value - 0.5);  // semitones
                    }
                }
            }
        }
        out.silenceFlags = notes_.empty() ? ((uint64(1) << out.numChannels) - 1) : 0;
        return kResultOk;
    }

    tresult PLUGIN_API setState(IBStream* state) override {
        IBStreamer s(state, kLittleEndian);
        double gain = 1.0;
        int32 sine = 1;
        if (!s.readDouble(gain) || !s.readInt32(sine)) return kResultFalse;
        gain_ = gain;
        sine_ = sine != 0;
        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream* state) override {
        IBStreamer s(state, kLittleEndian);
        s.writeDouble(gain_);
        s.writeInt32(sine_ ? 1 : 0);
        return kResultOk;
    }

private:
    struct Note {
        int16 pitch;
        float velocity;
        double phase;
        int32 noteId;
        double tuning;  // semitones (its note expression)
    };

    void render(AudioBusBuffers& out, int32 from, int32 to) {
        for (int32 i = from; i < to; ++i) {
            double sample = 0.0;
            for (Note& note : notes_) {
                if (sine_) {
                    sample += note.velocity * std::sin(2.0 * kPi * note.phase);
                    note.phase += 440.0 * std::pow(2.0, (note.pitch + note.tuning - 69) / 12.0) / sampleRate_;
                    note.phase -= std::floor(note.phase);
                } else {
                    sample += note.velocity;
                }
            }
            for (int32 c = 0; c < out.numChannels; ++c) out.channelBuffers32[c][i] = static_cast<float>(sample * gain_);
        }
    }

    std::vector<Note> notes_;
    double lastTuning_ = 0.0;  // the tuning of the last note to end, which the next one starts with
    double sampleRate_ = 48000.0;
    double gain_ = 1.0;
    bool sine_ = true;
};

class SynthController : public EditController {
public:
    static FUnknown* createInstance(void*) { return static_cast<IEditController*>(new SynthController); }

    tresult PLUGIN_API initialize(FUnknown* context) override {
        const tresult result = EditController::initialize(context);
        if (result != kResultOk) return result;
        parameters.addParameter(STR16("Gain"), nullptr, 0, 1.0, ParameterInfo::kCanAutomate, kSynthGain);
        auto* wave = new StringListParameter(STR16("Wave"), kSynthWave);
        wave->appendString(STR16("DC"));
        wave->appendString(STR16("Sine"));
        wave->setNormalized(1.0);
        wave->getInfo().defaultNormalizedValue = 1.0;
        parameters.addParameter(wave);
        parameters.addParameter(new RangeParameter(STR16("Tempo"), kSynthTempo, STR16("BPM"), 0.0, 1000.0, 0.0,
                                                   0, ParameterInfo::kIsReadOnly));
        parameters.addParameter(STR16("Playing"), nullptr, 1, 0.0, ParameterInfo::kIsReadOnly, kSynthPlaying);
        parameters.addParameter(new RangeParameter(STR16("Beat"), kSynthBeat, nullptr, 0.0, 1000.0, 0.0, 0,
                                                   ParameterInfo::kIsReadOnly));
        parameters.addParameter(STR16("Loop"), nullptr, 1, 0.0, ParameterInfo::kIsReadOnly, kSynthLoop);
        for (int i = 0; i < kNumMacros; ++i) {
            const std::string title = "Macro " + std::to_string(i + 1);
            const std::u16string wide(title.begin(), title.end());
            parameters.addParameter(reinterpret_cast<const TChar*>(wide.c_str()), nullptr, 0, 0.0,
                                    ParameterInfo::kCanAutomate, kSynthMacro + i);
        }
        return kResultOk;
    }

    // The controller's own state: the macros.
    tresult PLUGIN_API setState(IBStream* state) override {
        IBStreamer s(state, kLittleEndian);
        for (int i = 0; i < kNumMacros; ++i) {
            double value = 0.0;
            if (!s.readDouble(value)) return kResultFalse;
            setParamNormalized(kSynthMacro + i, value);
        }
        return kResultOk;
    }

    tresult PLUGIN_API getState(IBStream* state) override {
        IBStreamer s(state, kLittleEndian);
        for (int i = 0; i < kNumMacros; ++i) s.writeDouble(getParamNormalized(kSynthMacro + i));
        return kResultOk;
    }

    tresult PLUGIN_API setComponentState(IBStream* state) override {
        IBStreamer s(state, kLittleEndian);
        double gain = 1.0;
        int32 sine = 1;
        if (!s.readDouble(gain) || !s.readInt32(sine)) return kResultFalse;
        setParamNormalized(kSynthGain, gain);
        setParamNormalized(kSynthWave, sine ? 1.0 : 0.0);
        return kResultOk;
    }
};

// ---------------------------------------------------------------------------
// SUB Test Mono

class MonoEffect : public AudioEffect {
public:
    static FUnknown* createInstance(void*) { return static_cast<IAudioProcessor*>(new MonoEffect); }

    tresult PLUGIN_API initialize(FUnknown* context) override {
        const tresult result = AudioEffect::initialize(context);
        if (result != kResultOk) return result;
        addAudioInput(STR16("Input"), SpeakerArr::kMono);
        addAudioOutput(STR16("Output"), SpeakerArr::kMono);
        return kResultOk;
    }

    tresult PLUGIN_API setBusArrangements(SpeakerArrangement* inputs, int32 numIns, SpeakerArrangement* outputs,
                                          int32 numOuts) override {
        if (numIns != 1 || numOuts != 1 || inputs[0] != SpeakerArr::kMono || outputs[0] != SpeakerArr::kMono) {
            return kResultFalse;
        }
        return AudioEffect::setBusArrangements(inputs, numIns, outputs, numOuts);
    }

    tresult PLUGIN_API process(ProcessData& data) override {
        if (data.numInputs < 1 || data.numOutputs < 1 || data.numSamples <= 0) return kResultOk;
        const float* in = data.inputs[0].channelBuffers32[0];
        float* out = data.outputs[0].channelBuffers32[0];
        for (int32 i = 0; i < data.numSamples; ++i) out[i] = in[i] * 0.5f;
        return kResultOk;
    }
};

// ---------------------------------------------------------------------------
// SUB Test Note Effect

class NoteEffect : public AudioEffect {
public:
    static FUnknown* createInstance(void*) { return static_cast<IAudioProcessor*>(new NoteEffect); }

    tresult PLUGIN_API initialize(FUnknown* context) override {
        const tresult result = AudioEffect::initialize(context);
        if (result != kResultOk) return result;
        addAudioInput(STR16("Input"), SpeakerArr::kStereo);
        addAudioOutput(STR16("Output"), SpeakerArr::kStereo);
        addEventInput(STR16("Notes"), 16);
        return kResultOk;
    }

    tresult PLUGIN_API setActive(TBool state) override {
        held_.clear();
        return AudioEffect::setActive(state);
    }

    tresult PLUGIN_API process(ProcessData& data) override {
        if (data.numInputs < 1 || data.numOutputs < 1 || data.numSamples <= 0) return kResultOk;
        AudioBusBuffers& in = data.inputs[0];
        AudioBusBuffers& out = data.outputs[0];
        int32 position = 0;
        IEventList* events = data.inputEvents;
        const int32 numEvents = events ? events->getEventCount() : 0;
        for (int32 e = 0; e <= numEvents; ++e) {
            Event event{};
            int32 until = data.numSamples;
            if (e < numEvents) {
                if (events->getEvent(e, event) != kResultOk) continue;
                until = std::clamp(event.sampleOffset, position, data.numSamples);
            }
            double level = 0.0;
            for (const auto& [pitch, velocity] : held_) level += velocity;
            for (int32 c = 0; c < out.numChannels; ++c) {
                const float* from = in.channelBuffers32[std::min(c, in.numChannels - 1)];
                for (int32 i = position; i < until; ++i) out.channelBuffers32[c][i] = from[i] + static_cast<float>(level);
            }
            position = until;
            if (e == numEvents) break;
            if (event.type == Event::kNoteOnEvent && event.noteOn.velocity > 0.f) {
                held_.push_back({event.noteOn.pitch, event.noteOn.velocity});
            } else if (event.type == Event::kNoteOnEvent || event.type == Event::kNoteOffEvent) {
                const int16 pitch = event.type == Event::kNoteOnEvent ? event.noteOn.pitch : event.noteOff.pitch;
                auto it = std::find_if(held_.begin(), held_.end(), [pitch](const auto& n) { return n.first == pitch; });
                if (it != held_.end()) held_.erase(it);
            }
        }
        out.silenceFlags = 0;
        return kResultOk;
    }

private:
    std::vector<std::pair<int16, float>> held_;  // (pitch, velocity)
};

}  // namespace sub_test

// ---------------------------------------------------------------------------
// Module entry points

#ifdef _WIN32
bool InitModule() {
    char value[8] = {};
    if (GetEnvironmentVariableA("SUB_TEST_PLUGIN_CRASH", value, sizeof(value)) && value[0] == '1') {
        TerminateProcess(GetCurrentProcess(), 3);
    }
    if (GetEnvironmentVariableA("SUB_TEST_PLUGIN_HANG", value, sizeof(value)) && value[0] == '1') {
        Sleep(INFINITE);
    }
    return true;
}

bool DeinitModule() {
    UnregisterClassW(sub_test::kEffectViewClass, ghInst);  // its window procedure goes with the module
    return true;
}
#else
bool InitModule() {
    const auto set = [](const char* name) {
        const char* value = std::getenv(name);
        return value && value[0] == '1';
    };
    if (set("SUB_TEST_PLUGIN_CRASH")) std::_Exit(3);
    if (set("SUB_TEST_PLUGIN_HANG")) {
        for (;;) std::this_thread::sleep_for(std::chrono::hours(1));
    }
    return true;
}

bool DeinitModule() { return true; }
#endif

BEGIN_FACTORY_DEF("SUBstation", "https://example.invalid/substation", "mailto:none@example.invalid")

DEF_CLASS2(INLINE_UID_FROM_FUID(sub_test::kSynthProcessorUID), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "SUB Test Synth", Vst::kDistributable, "Instrument|Synth", "1.0.0", kVstVersionString,
           sub_test::SynthProcessor::createInstance)

DEF_CLASS2(INLINE_UID_FROM_FUID(sub_test::kSynthControllerUID), PClassInfo::kManyInstances,
           kVstComponentControllerClass, "SUB Test Synth Controller", 0, "", "1.0.0", kVstVersionString,
           sub_test::SynthController::createInstance)

DEF_CLASS2(INLINE_UID_FROM_FUID(sub_test::kEffectUID), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "SUB Test Effect", 0, "Fx|Delay", "1.0.0", kVstVersionString, sub_test::createTestEffect)

DEF_CLASS2(INLINE_UID_FROM_FUID(sub_test::kMonoUID), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "SUB Test Mono", Vst::kDistributable, "Fx", "1.0.0", kVstVersionString,
           sub_test::MonoEffect::createInstance)

DEF_CLASS2(INLINE_UID_FROM_FUID(sub_test::kSidechainUID), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "SUB Test Sidechain", 0, "Fx|Dynamics", "1.0.0", kVstVersionString, sub_test::createTestSidechain)

DEF_CLASS2(INLINE_UID_FROM_FUID(sub_test::kNoteEffectUID), PClassInfo::kManyInstances, kVstAudioEffectClass,
           "SUB Test Note Effect", Vst::kDistributable, "Fx|Pitch Shift", "1.0.0", kVstVersionString,
           sub_test::NoteEffect::createInstance)

END_FACTORY
