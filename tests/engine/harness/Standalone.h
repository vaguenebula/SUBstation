#pragma once
// A built-in device on its own, outside an engine, at any sample rate, run as
// the renderer runs it: in blocks, its parameters' changes handed over as
// automation (so its blocks split there, sample-accurately), or, for one that
// isn't automatable, set between blocks (a block starting there). The devices'
// own tests build on it.
//
//   Standalone saturator("saturator", 48000, {{"drive", 12.f}});
//   Samples out = saturator.play(sine(1000, 1), {{24000, "drive", 0.f}});

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Processor.h"
#include "builtin/BuiltinRegistry.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"
#include "harness/Test.h"

namespace subtest {

using ParamValues = std::vector<std::pair<std::string, float>>;

// A parameter's change at a frame: as automation hands it over, or (`direct`, for one that
// isn't automatable) set between blocks, a block starting there.
struct ParamChange {
    int64_t frame;
    std::string id;
    float value;
    bool direct = false;
};

class Standalone {
public:
    static constexpr int kMaxBlock = 1024;  // the renderer's largest block (Renderer::kMaxBlock)

    explicit Standalone(const std::string& kind, double rate = kSampleRate, const ParamValues& values = {})
        : processor_(sub::BuiltinRegistry::instance().create(kind)), rate_(rate) {
        set(values);
        processor_->prepare(rate, kMaxBlock);
        context_.sampleRate = rate;
        context_.offline = true;
    }

    sub::Processor& processor() { return *processor_; }
    double rate() const { return rate_; }
    // What each block is processed with (tempo, playing, ...); its position is set per block.
    sub::ProcessContext& context() { return context_; }

    int index(const std::string& id) const {
        const auto& params = processor_->params();
        for (size_t i = 0; i < params.size(); ++i)
            if (params[i].id == id) return static_cast<int>(i);
        INFO(id);
        REQUIRE(false);
        return -1;
    }
    void set(const std::string& id, float value) { processor_->setParam(index(id), value); }
    void set(const ParamValues& values) {
        for (const auto& [id, value] : values) set(id, value);
    }

    // Processes channels of equal length in place (one, two, ...), `block` frames at a time.
    void run(const std::vector<Samples*>& channels, const std::vector<ParamChange>& changes = {}, int block = 256) {
        const auto frames = static_cast<int64_t>(channels[0]->size());
        std::vector<float*> pointers(channels.size());
        size_t next = 0;
        for (int64_t start = 0; start < frames;) {
            while (next < changes.size() && changes[next].direct && changes[next].frame <= start) {
                set(changes[next].id, changes[next].value);
                ++next;
            }
            int64_t end = std::min<int64_t>(start + block, frames);
            for (size_t c = next; c < changes.size(); ++c) {  // a block ends where a direct change comes
                if (changes[c].direct && changes[c].frame > start) {
                    end = std::min(end, changes[c].frame);
                    break;
                }
            }
            const int n = static_cast<int>(end - start);
            while (next < changes.size() && !changes[next].direct && changes[next].frame < end) {
                const ParamChange& change = changes[next++];
                const int i = index(change.id);
                processor_->automate(i, processor_->params()[static_cast<size_t>(i)].toNormalized(change.value),
                                     static_cast<int32_t>(std::max<int64_t>(0, change.frame - start)));
            }
            for (size_t c = 0; c < channels.size(); ++c) pointers[c] = channels[c]->data() + start;
            context_.samplePos = start;
            const double samplesPerBeat = context_.samplesPerBeat();
            context_.beatPos = samplesPerBeat > 0.0 ? double(start) / samplesPerBeat : 0.0;
            processor_->process(context_, pointers.data(), static_cast<int>(channels.size()), n);
            processor_->clearAutomation();
            start = end;
        }
    }
    // One channel: what comes out.
    Samples play(Samples mono, const std::vector<ParamChange>& changes = {}, int block = 256) {
        run({&mono}, changes, block);
        return mono;
    }

private:
    std::shared_ptr<sub::Processor> processor_;
    double rate_;
    sub::ProcessContext context_;
};

}  // namespace subtest
