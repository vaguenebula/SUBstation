#pragma once
// Click generator. The renderer schedules clicks at sample offsets inside a
// block; the metronome then renders the current click voice across the block.

#include <cstdint>
#include <vector>

namespace gil {

class Metronome {
public:
    void prepare(double sampleRate);  // non-RT: synthesises the click samples

    // Real-time safe. Renders the active click into [from, to) of the buffers;
    // start() replaces the active click, so callers render up to each click's
    // offset before starting it.
    void renderUntil(float* left, float* right, int from, int to);
    void start(bool accent);

private:
    std::vector<float> accentClick_;
    std::vector<float> normalClick_;
    const std::vector<float>* voice_ = nullptr;
    size_t voicePos_ = 0;
};

}  // namespace gil
