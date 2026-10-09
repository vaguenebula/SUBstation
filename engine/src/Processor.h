#pragma once
// The insert-effect / instrument interface. Built-in devices and hosted plug-ins
// (plugins/Vst3Processor) implement it, so the renderer never needs to know
// where a processor came from.
//
// Threads: process() and reset() run on the rendering thread (the audio thread,
// or the thread rendering offline). Everything else is called from the main
// (UI) thread, as plug-in formats require, unless noted.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sub {

// A timestamped event delivered to a processor within a block. The renderer
// sends each track's notes to every processor on the track (audio effects
// ignore them), sorted by sample offset, note-offs before note-ons at the same
// offset. Plug-in adapters translate these to their format's events.
// (Automation reaches each processor separately: see Processor::automate().)
struct ProcessEvent {
    enum class Type : uint8_t { NoteOn, NoteOff, Midi };
    Type type = Type::Midi;
    int32_t sampleOffset = 0;
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
    // A note starts; a note ends (a note-on at velocity 0 ends one, as in MIDI).
    bool startsNote() const noexcept { return type == Type::NoteOn && velocity() > 0; }
    bool endsNote() const noexcept { return type == Type::NoteOff || (type == Type::NoteOn && velocity() == 0); }
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

    // Samples a beat lasts at the block's tempo (0 without one).
    double samplesPerBeat() const noexcept { return tempo > 0.0 ? sampleRate * 60.0 / tempo : 0.0; }
    // Beats (quarter notes) in a bar of the time signature.
    double beatsPerBar() const noexcept { return std::max(1, timeSigNum) * 4.0 / std::max(1, timeSigDen); }
};

// A parameter, whatever kind of processor it belongs to. Its values are plain
// (in its own units, from minValue to maxValue). Automation, and anything else
// that treats all parameters alike, works on normalized values (0..1), which
// toNormalized()/fromNormalized() map to and from plain ones: evenly, or in
// log(value) for logScale parameters, or in whole steps for discrete ones (the
// way VST3 maps its stepped parameters, so plug-in values round-trip).
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

    // The number of steps between the lowest and highest value of a discrete
    // parameter (a list, or whole values); 0 for a continuous one.
    int stepCount() const noexcept {
        if (steps > 0) return steps;
        if (!valueLabels.empty()) return std::max(1, static_cast<int>(valueLabels.size()) - 1);
        return 0;
    }
    bool isLog() const noexcept { return logScale && minValue > 0.f && maxValue > minValue; }

    float toNormalized(float plain) const noexcept {
        const float range = maxValue - minValue;
        if (!(range > 0.f)) return 0.f;
        if (const int count = stepCount(); count > 0) {
            const auto last = static_cast<float>(count);
            return std::clamp(std::round(plain - minValue), 0.f, last) / last;
        }
        plain = std::clamp(plain, minValue, maxValue);
        if (isLog()) return std::log(plain / minValue) / std::log(maxValue / minValue);
        return (plain - minValue) / range;
    }
    float fromNormalized(float normalized) const noexcept {
        normalized = std::clamp(normalized, 0.f, 1.f);
        if (const int count = stepCount(); count > 0) {
            const auto last = static_cast<float>(count);
            return minValue + std::min(last, std::floor(normalized * (last + 1.f)));
        }
        if (isLog()) return minValue * std::pow(maxValue / minValue, normalized);
        return minValue + normalized * (maxValue - minValue);
    }
};

// A stream of values a device's own editor draws besides its parameters: a
// meter's readings, a curve, samples for an analyser (Processor::displays()).
struct DisplayInfo {
    std::string id;           // what the editor asks for it by ("reduction")
    int samplesPerValue = 1;  // the audio each value stands for: 1 for samples, more for a meter
};

// A change the host's automation makes to a parameter within a process() call.
struct ParamAutomation {
    int32_t sampleOffset = 0;
    int32_t index = 0;   // the parameter
    float value = 0.f;   // normalized (ParamInfo::fromNormalized gives its plain value)
    uint32_t order = 0;  // when it was handed over; sorting by time keeps each parameter's changes in order
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
        ParamTouched,      // the user took hold of a parameter in the plug-in's editor (paramIndex)
    };
    Type type = Type::ParamsChanged;
    int paramIndex = -1;  // ParamEdited, ParamTouched: which parameter,
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

    virtual int latencySamples() const { return 0; }  // compensated by delaying the other tracks (and sidechains)
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

    // Rendering thread: its automation switched it off for a whole chunk, so
    // the renderer didn't process it (it resets it as it comes back on).
    bool switchedOff() const noexcept { return switchedOff_; }
    void setSwitchedOff(bool off) noexcept { switchedOff_ = off; }
    // Rendering thread: samples left of its fade back in from its input, after
    // its switch's lane went while it was off (0: none).
    int switchFadeIn() const noexcept { return switchFadeIn_; }
    void setSwitchFadeIn(int samples) noexcept { switchFadeIn_ = samples; }

    // --- Automation (rendering thread) -------------------------------------
    // Before each process() call the renderer hands the processor what its
    // automation does during that call: parameter `index` goes to the normalized
    // `value` at `sampleOffset`, in time order for each parameter (at least the
    // value at offset 0 for every automated parameter). After the call it clears
    // them. Applying them is up to the processor: built-in devices split their
    // blocks where values change (BuiltinProcessor), plug-ins hand them on to the
    // plug-in (sample-accurately in VST3). Processors nested in others (the
    // devices in a rack) get theirs directly, wherever they sit.
    static constexpr size_t kMaxAutomation = 2048;  // per call; the rest is dropped
    bool automate(int index, float value, int32_t sampleOffset) noexcept {
        if (numAutomation_ >= automation_.size()) return false;
        automation_[numAutomation_] = {sampleOffset, index, value, static_cast<uint32_t>(numAutomation_)};
        ++numAutomation_;
        return true;
    }
    void clearAutomation() noexcept { numAutomation_ = 0; }

    // --- Sidechain -------------------------------------------------------------
    // A processor with a sidechain (aux) input says so (main thread). Before each
    // process() call the renderer hands it what goes into that input over the
    // call's frames (two channels, as long as the call), or null for nothing
    // (silence: no source, or solo leaves it out); after the call, null again.
    virtual bool hasSidechain() const { return false; }
    void setSidechain(const float* left, const float* right) noexcept {
        sidechain_[0] = left;
        sidechain_[1] = right;
    }
    // Rendering thread: whether the device has a sidechain at all (a source was
    // chosen), so one can key from its own input without one, yet hear silence
    // when solo leaves its source out. Set before each process() call.
    void setSidechainConnected(bool connected) noexcept { sidechainConnected_ = connected; }

    // --- Display ---------------------------------------------------------------
    // What a device's own editor draws besides its parameters, as streams of
    // values that process() publishes. readDisplay() appends stream `index`'s
    // values since `position` (0 at first) to `out` and returns where to read
    // from next; any thread may call it, any number of readers may follow a stream.
    virtual std::vector<DisplayInfo> displays() const { return {}; }
    virtual uint64_t readDisplay(int /*index*/, uint64_t position, std::vector<float>& /*out*/) const {
        return position;
    }

protected:
    // In process(): the automation for this call (processors may sort it).
    ParamAutomation* automation() noexcept { return automation_.data(); }
    size_t numAutomation() const noexcept { return numAutomation_; }
    // In process(): the sidechain's channel (0 or 1) for this call; null: silence.
    const float* sidechain(int channel) const noexcept { return sidechain_[channel & 1]; }
    bool sidechainConnected() const noexcept { return sidechainConnected_; }

private:
    const float* sidechain_[2] = {nullptr, nullptr};
    bool sidechainConnected_ = false;
    std::atomic<bool> enabled_{true};
    std::atomic<bool> resetRequested_{false};
    bool switchedOff_ = false;
    int switchFadeIn_ = 0;
    std::vector<ParamAutomation> automation_ = std::vector<ParamAutomation>(kMaxAutomation);
    size_t numAutomation_ = 0;
};

}  // namespace sub
