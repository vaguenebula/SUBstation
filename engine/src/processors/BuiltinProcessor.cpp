#include "processors/BuiltinProcessor.h"

#include <algorithm>

namespace gil {

BuiltinProcessor::BuiltinProcessor(const std::vector<ParamInfo>& infos)
    : infos_(infos), values_(std::make_unique<std::atomic<float>[]>(infos.size())) {
    for (size_t i = 0; i < infos.size(); ++i) values_[i].store(infos[i].defaultValue);
}

float BuiltinProcessor::getParam(int index) const {
    return index >= 0 && index < static_cast<int>(infos_.size()) ? values_[index].load() : 0.f;
}

void BuiltinProcessor::setParam(int index, float value) {
    if (index < 0 || index >= static_cast<int>(infos_.size())) return;
    const ParamInfo& info = infos_[index];
    values_[index].store(std::clamp(value, info.minValue, info.maxValue));
}

void BuiltinProcessor::process(const ProcessContext& ctx, float* const* channels, int numChannels, int numFrames) {
    const size_t count = numAutomation();
    if (count == 0) {
        render(ctx, channels, numChannels, numFrames);
        return;
    }
    ParamAutomation* changes = automation();
    std::sort(changes, changes + count, [](const ParamAutomation& a, const ParamAutomation& b) {
        return a.sampleOffset != b.sampleOffset ? a.sampleOffset < b.sampleOffset : a.order < b.order;
    });
    const auto apply = [this](const ParamAutomation& change) {
        if (change.index >= 0 && change.index < static_cast<int>(infos_.size())) {
            values_[change.index].store(infos_[change.index].fromNormalized(change.value), std::memory_order_relaxed);
        }
    };

    const int numOut = std::clamp(numChannels, 0, kMaxChannels);
    float* part[kMaxChannels] = {};
    const double samplesPerBeat = ctx.tempo > 0.0 ? ctx.sampleRate * 60.0 / ctx.tempo : 0.0;
    size_t next = 0;
    size_t event = 0;
    int position = 0;
    while (position < numFrames) {
        while (next < count && changes[next].sampleOffset <= position) apply(changes[next++]);
        const int end = next < count ? std::min(numFrames, static_cast<int>(changes[next].sampleOffset)) : numFrames;
        // The events in [position, end), relative to its start; the last stretch takes the rest.
        size_t numEvents = 0;
        while (event < ctx.inEvents.count && (end == numFrames || ctx.inEvents.events[event].sampleOffset < end)) {
            if (numEvents < events_.size()) {
                ProcessEvent& copy = events_[numEvents++];
                copy = ctx.inEvents.events[event];
                copy.sampleOffset = std::max(0, copy.sampleOffset - position);
            }
            ++event;
        }
        ProcessContext stretch = ctx;
        stretch.samplePos = ctx.samplePos + position;
        if (samplesPerBeat > 0.0) stretch.beatPos = ctx.beatPos + position / samplesPerBeat;
        stretch.inEvents = {events_.data(), numEvents};
        for (int c = 0; c < numOut; ++c) part[c] = channels[c] + position;
        render(stretch, part, numOut, end - position);
        position = end;
    }
    while (next < count) apply(changes[next++]);  // at or after the block's end
}

}  // namespace gil
