#pragma once
// The insert-effect / instrument interface. Built-in devices and hosted plug-ins
// (plugins/Vst3Processor) implement it, so the renderer never needs to know
// where a processor came from.
//
// Threads: process() and reset() run on the rendering thread (the audio thread,
// or the thread rendering offline). Everything else is called from the main
// (UI) thread, as plug-in formats require, unless noted.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace gil {

// A timestamped event delivered to a processor within a block. The renderer
// sends each track's notes to every processor on the track (audio effects
// ignore them), sorted by sample offset, note-offs before note-ons at the same
// offset. Plug-in adapters translate these to their format's events.
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
    uint8_t channel() const noexcept { return data[2]; }
};

struct EventList {
    const ProcessEvent* events = nullptr;
    size_t count = 0;
};

// Where the block sits on the timeline. The renderer splits blocks where the
// playhead jumps (loop wrap-around), so a block is always one continuous stretch.
struct ProcessContext {
    double sampleRate = 48000.0;
    int64_t samplePos = 0;   // timeline position of the block's first frame
    double beatPos = 0.0;    // the same position in quarter-note beats
    double tempo = 120.0;
    int timeSigNum = 4;
    int timeSigDen = 4;
    bool playing = false;
    bool looping = false;    // playback wraps from loopEndBeat to loopStartBeat
    double loopStartBeat = 0.0;
    double loopEndBeat = 0.0;
    bool offline = false;    // rendering an export (or a test), not live
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
    int steps = 0;                         // > 0: only whole values from minValue to minValue + steps
    bool automatable = true;
    bool readOnly = false;                 // set by the processor itself (a meter or a display)
    bool hidden = false;                   // not for a generic editor (e.g. a plug-in's own bypass)
};

// Something a processor reports to the UI. Collected on the main thread.
struct ProcessorEvent {
    enum class Type : uint8_t {
        ParamEdited,       // the plug-in's editor changed a parameter, as a user edit
        ParamsChanged,     // values changed without user edits (preset, meters): show them
        ParamInfoChanged,  // the parameter list, names or ranges changed
        EditorClosed,      // the user closed the editor window
        EditorRequested,   // the plug-in asks for its editor to be opened
        StateDirty,        // the plug-in's state changed in a way no parameter shows
        LatencyChanged,
    };
    Type type = Type::ParamsChanged;
    int paramIndex = -1;  // ParamEdited: which parameter,
    float value = 0.f;    // its new value
    float oldValue = 0.f; // and its value before the edit (or before the gesture started)
    uint32_t gesture = 0; // edits of one gesture (a knob drag) share this; 0: a single edit
};

class Processor {
public:
    virtual ~Processor() = default;

    // "builtin:utility", "vst3:<class id>".
    virtual std::string typeId() const = 0;
    virtual std::string name() const = 0;

    // Called before the processor is first published to the audio thread, and
    // again with audio stopped whenever the sample rate changes.
    virtual void prepare(double sampleRate, int maxBlockSize) = 0;
    // Real-time. Silences the processor: sounding notes, tails, filter state.
    // Only the renderer calls it, before a block, when a reset was requested.
    virtual void reset() {}
    // Clears everything a real-time reset can't (a plug-in's reverb tail), with
    // the processor not being processed. The engine calls it around offline renders.
    virtual void resetOffline() {}

    // Real-time. Processes planar channel buffers in place.
    virtual void process(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) = 0;

    virtual int latencySamples() const { return 0; }  // compensated by delaying the other tracks
    virtual int tailSamples() const { return 0; }

    // Plain (not normalised) values. getParam is thread-safe.
    virtual const std::vector<ParamInfo>& params() const = 0;
    virtual float getParam(int index) const = 0;
    virtual void setParam(int index, float value) = 0;
    // How the processor itself shows a value ("" if it leaves that to the UI).
    virtual std::string paramText(int /*index*/, float /*value*/) const { return {}; }

    // Everything needed to restore the processor later (a plug-in's own state).
    // Empty for processors whose parameters are their whole state.
    virtual std::vector<uint8_t> getState() { return {}; }
    virtual void setState(const std::vector<uint8_t>& /*state*/) {}

    // Main-thread housekeeping, called regularly. Returns true if the processor's
    // latency changed, so the engine must realign the tracks.
    virtual bool idle() { return false; }
    virtual void takeEvents(std::vector<ProcessorEvent>& /*out*/) {}

    // A plug-in's editor, in a window of its own owned by `ownerWindow` (an HWND
    // on Windows; may be null).
    virtual bool hasEditor() const { return false; }
    virtual bool openEditor(void* /*ownerWindow*/, const std::string& /*title*/) { return false; }
    virtual void closeEditor() {}
    virtual bool isEditorOpen() const { return false; }  // open and not hidden
    // Hides an open editor or shows it again; false if none is open.
    virtual bool setEditorVisible(bool /*visible*/) { return false; }
    virtual void setEditorTitle(const std::string& /*title*/) {}

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
