#pragma once
// A rack (a device group): a device whose chains each process its input, side
// by side, and whose output is what they put out, summed. Its structure lives
// in the engine's chains and in the render snapshot (RackRender), not in the
// processor: the renderer runs its chains (Renderer::processRack). The
// processor is only its place in a chain, its on/off switch and its id.

#include <atomic>
#include <string>
#include <vector>

#include "Processor.h"

namespace gil {

class RackProcessor final : public Processor {
public:
    std::string typeId() const override { return "rack"; }
    std::string name() const override { return "Rack"; }
    void prepare(double /*sampleRate*/, int /*maxBlockSize*/) override {}
    // Never called: the renderer runs the rack's chains instead.
    void process(const ProcessContext&, float* const*, int, int) override {}
    // As the last snapshot worked it out: its slowest chain (for the UI).
    int latencySamples() const override { return latency_.load(std::memory_order_relaxed); }
    void setLatency(int samples) noexcept { latency_.store(samples, std::memory_order_relaxed); }

    const std::vector<ParamInfo>& params() const override {
        static const std::vector<ParamInfo> none;
        return none;
    }
    float getParam(int /*index*/) const override { return 0.f; }
    void setParam(int /*index*/, float /*value*/) override {}

private:
    std::atomic<int> latency_{0};
};

}  // namespace gil
