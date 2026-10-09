#pragma once
// What every built-in device shares: its parameters, described by a fixed
// ParamInfo list and kept as atomics (the UI sets them, the rendering thread
// reads them), and automation, applied sample-accurately by rendering the block
// in stretches between the points where automation changes a value.
//
// A new built-in device lists its parameters, passes them to the constructor,
// and implements render(), reading its parameters with param(). Automation,
// the UI's knobs and saving then work for it as for any other device.
//
// A device whose own editor draws more than its parameters (meters, curves)
// also lists its displays and publish()es their values from render().
//
// A device with state besides its parameters (a sampler's sample) keeps it as
// named text values: it implements stateValues()/setStateValues(), and the
// state saves, loads and undoes as a plug-in's does (the UI keeps it in the
// model's Device.state). Audio files it uses come through loadSource().

#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "AudioSource.h"
#include "Automation.h"
#include "Processor.h"
#include "rt/RtUtils.h"

namespace sub {

class BuiltinProcessor : public Processor {
public:
    const std::vector<ParamInfo>& params() const final { return infos_; }
    float getParam(int index) const final;
    void setParam(int index, float value) final;

    // Real-time. Calls render() for each stretch of the block with no automation
    // change in it, applying the changes in between.
    void process(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) final;

    std::vector<DisplayInfo> displays() const final { return displayInfos_; }

    // A switch parameter's value labels, for devices' ParamInfo lists.
    static const std::vector<std::string>& offOnLabels();
    uint64_t readDisplay(int index, uint64_t position, std::vector<float>& out) const final;

    // State besides the parameters, as lines "name=value" (UTF-8; a backslash
    // escapes a backslash or a newline: "\\", "\n"). Empty: none, the defaults.
    using StateValues = std::map<std::string, std::string>;
    std::vector<uint8_t> getState() final { return encodeState(stateValues()); }
    void setState(const std::vector<uint8_t>& state) final { setStateValues(decodeState(state)); }
    static std::vector<uint8_t> encodeState(const StateValues& values);
    static StateValues decodeState(const std::vector<uint8_t>& state);

    // How the device gets audio files (the engine shares its decoded files
    // with it). Without one, it decodes them itself.
    using SourceLoader = std::function<std::shared_ptr<const AudioSource>(const std::string& path)>;
    void setSourceLoader(SourceLoader loader) { loader_ = std::move(loader); }

protected:
    // `infos` must outlive the processor (a static list).
    explicit BuiltinProcessor(const std::vector<ParamInfo>& infos, std::vector<DisplayInfo> displays = {});

    // Real-time: processes a stretch of the block, in place. `ctx` describes the
    // stretch (its position, and its events, relative to its start).
    virtual void render(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) = 0;

    // The device's state besides its parameters (none by default). Called on
    // the main thread, or on the thread the UI loads states on: setStateValues
    // may take its time (loading files), and throws if it can't do it all.
    virtual StateValues stateValues() const { return {}; }
    virtual void setStateValues(const StateValues& /*values*/) {}

    // Not real-time: an audio file, decoded (at the engine's sample rate when
    // the engine loads it, else at the file's own). Throws if it can't be read.
    std::shared_ptr<const AudioSource> loadSource(const std::string& path) const;

    // A parameter's plain value; thread-safe.
    float param(int index) const noexcept { return values_[index].load(std::memory_order_relaxed); }
    // A switch's state: on from 0.5, as automation plays it.
    bool isOn(int index) const noexcept { return automationSwitchOn(param(index)); }
    // A choice's index (or a stepped parameter's value), rounded.
    int choiceIndex(int index) const noexcept { return static_cast<int>(std::lround(param(index))); }
    template <typename E>
    E choice(int index) const noexcept {
        return static_cast<E>(choiceIndex(index));
    }

    // In render(): publishes a value of display `index` (one per its samplesPerValue).
    void publish(int index, float value) noexcept { displayStreams_[static_cast<size_t>(index)]->push(value); }

    // In render(): the sidechain over the stretch being rendered (render() gets
    // part of a block when automation splits it); null: silence.
    const float* sidechain(int channel) const noexcept {
        const float* key = Processor::sidechain(channel);
        return key ? key + stretchStart_ : nullptr;
    }

private:
    static constexpr int kMaxChannels = 8;
    static constexpr size_t kMaxEvents = 2048;
    static constexpr size_t kDisplayCapacity = 8192;  // values kept for readers of each display

    const std::vector<ParamInfo>& infos_;
    std::unique_ptr<std::atomic<float>[]> values_;
    std::vector<ProcessEvent> events_ = std::vector<ProcessEvent>(kMaxEvents);  // one stretch's events
    int stretchStart_ = 0;  // where the stretch render() is given starts in the block
    std::vector<DisplayInfo> displayInfos_;
    std::vector<std::unique_ptr<DisplayStream>> displayStreams_;
    SourceLoader loader_;
};

}  // namespace sub
