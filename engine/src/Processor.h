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

// A timestamped event delivered to a processor within a block. The renderer
// sends each track's notes to every processor on the track (audio effects
// ignore them), sorted by sample offset, note-offs before note-ons at the same
// offset. VST3/CLAP adapters will translate these to IEventList /
// clap_input_events (notes, MIDI, parameter automation).
struct ProcessEvent {
    enum class Type : uint8_t { NoteOn, NoteOff, Midi, ParamChange };
    Type type = Type::Midi;
    int32_t sampleOffset = 0;
    int32_t param = 0;
    float value = 0.f;
    // NoteOn/NoteOff: data[0] is the key (0-127, 60 = C3), data[1] the velocity
    // (1-127; 0 for note-offs), data[2] the MIDI channel. Midi: the raw bytes.
    uint8_t data[4] = {};

    static ProcessEvent noteOn(int32_t offset, uint8_t key, uint8_t velocity) noexcept {
        ProcessEvent event;
        event.type = Type::NoteOn;
        event.sampleOffset = offset;
        event.data[0] = key;
        event.data[1] = velocity;
        return event;
    }
    static ProcessEvent noteOff(int32_t offset, uint8_t key) noexcept {
        ProcessEvent event;
        event.type = Type::NoteOff;
        event.sampleOffset = offset;
        event.data[0] = key;
        return event;
    }
    uint8_t key() const noexcept { return data[0]; }
    uint8_t velocity() const noexcept { return data[1]; }
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
    bool logScale = false;                 // knobs move evenly in log(value) (frequencies, times)
    std::vector<std::string> valueLabels;  // non-empty: a choice; values 0..n-1 name these
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
    // Real-time. Silences the processor: sounding notes, tails, filter state.
    // Only the renderer calls it, before a block, when a reset was requested.
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

    // Switching a processor off also resets it, so when it comes back on it
    // can't resume notes whose note-offs it missed while it was off.
    void setEnabled(bool enabled) noexcept {
        if (!enabled) requestReset();
        enabled_.store(enabled, std::memory_order_relaxed);
    }
    bool isEnabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    // Thread-safe: asks whichever renderer processes this next to reset() it
    // first (the engine does this around offline renders).
    void requestReset() noexcept { resetRequested_.store(true, std::memory_order_release); }
    bool takeResetRequest() noexcept { return resetRequested_.exchange(false, std::memory_order_acquire); }

private:
    std::atomic<bool> enabled_{true};
    std::atomic<bool> resetRequested_{false};
};

}  // namespace gil
