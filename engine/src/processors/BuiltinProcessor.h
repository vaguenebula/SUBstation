#pragma once
// What every built-in device shares: its parameters, described by a fixed
// ParamInfo list and kept as atomics (the UI sets them, the rendering thread
// reads them), and automation, applied sample-accurately by rendering the block
// in stretches between the points where automation changes a value.
//
// A new built-in device lists its parameters, passes them to the constructor,
// and implements render(), reading its parameters with param(). Automation,
// the UI's knobs and saving then work for it as for any other device.

#include <atomic>
#include <memory>
#include <vector>

#include "Processor.h"

namespace gil {

class BuiltinProcessor : public Processor {
public:
    const std::vector<ParamInfo>& params() const final { return infos_; }
    float getParam(int index) const final;
    void setParam(int index, float value) final;

    // Real-time. Calls render() for each stretch of the block with no automation
    // change in it, applying the changes in between.
    void process(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) final;

protected:
    // `infos` must outlive the processor (a static list).
    explicit BuiltinProcessor(const std::vector<ParamInfo>& infos);

    // Real-time: processes a stretch of the block, in place. `ctx` describes the
    // stretch (its position, and its events, relative to its start).
    virtual void render(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) = 0;

    // A parameter's plain value; thread-safe.
    float param(int index) const noexcept { return values_[index].load(std::memory_order_relaxed); }

private:
    static constexpr int kMaxChannels = 8;
    static constexpr size_t kMaxEvents = 2048;

    const std::vector<ParamInfo>& infos_;
    std::unique_ptr<std::atomic<float>[]> values_;
    std::vector<ProcessEvent> events_ = std::vector<ProcessEvent>(kMaxEvents);  // one stretch's events
};

}  // namespace gil
