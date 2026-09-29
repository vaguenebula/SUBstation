#pragma once
// The insert-effect / instrument interface. Built-in devices implement it today;
// VST3 and CLAP plugin adapters will implement it later, so the renderer never
// needs to know where a processor came from.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gil {

// A timestamped event delivered to a processor within a block. Always empty in
// the audio-only v1; VST3/CLAP adapters will translate these to IEventList /
// clap_input_events (notes, MIDI, parameter automation).
struct ProcessEvent {
    enum class Type : uint8_t { NoteOn, NoteOff, Midi, ParamChange };
    Type type = Type::Midi;
    int32_t sampleOffset = 0;
    int32_t param = 0;
    float value = 0.f;
    uint8_t data[4] = {};
};

struct EventList {
    const ProcessEvent* events = nullptr;
    size_t count = 0;
};

struct ProcessContext {
    double sampleRate = 48000.0;
    int64_t samplePos = 0;   // timeline position of the block's first frame
    double beatPos = 0.0;    // the same position in quarter-note beats
    double tempo = 120.0;
    int timeSigNum = 4;
    int timeSigDen = 4;
    bool playing = false;
    EventList inEvents;
};

struct ParamInfo {
    std::string id;
    std::string name;
    std::string unit;
    float minValue = 0.f;
    float maxValue = 1.f;
    float defaultValue = 0.f;
};

class Processor {
public:
    virtual ~Processor() = default;

    // "builtin:utility" today; "vst3:<class id>" / "clap:<plugin id>" later.
    virtual std::string typeId() const = 0;
    virtual std::string name() const = 0;

    // Non-real-time. Called before the processor is first published to the audio
    // thread, and again with audio stopped whenever the sample rate changes.
    virtual void prepare(double sampleRate, int maxBlockSize) = 0;
    virtual void reset() {}

    // Real-time. Processes planar channel buffers in place.
    virtual void process(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) = 0;

    virtual int latencySamples() const { return 0; }  // for future delay compensation
    virtual int tailSamples() const { return 0; }

    // Plain (not normalised) values. setParam is thread-safe.
    virtual const std::vector<ParamInfo>& params() const = 0;
    virtual float getParam(int index) const = 0;
    virtual void setParam(int index, float value) = 0;

    // Plugin editors are embedded in a native window owned by the UI
    // (QWidget::winId() gives an HWND on Windows).
    virtual bool hasEditor() const { return false; }
    virtual bool openEditor(void* /*parentWindowHandle*/) { return false; }
    virtual void closeEditor() {}

    void setEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> enabled_{true};
};

}  // namespace gil
