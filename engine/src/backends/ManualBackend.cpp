#include "ManualBackend.h"

namespace sub {

ManualBackend::~ManualBackend() { close(); }

void ManualBackend::open(const DeviceConfig& config, AudioCallback* callback) {
    close();
    rate_ = config.sampleRate ? config.sampleRate : 48000;
    frames_ = config.bufferFrames ? config.bufferFrames : 256;
    left_.assign(frames_, 0.f);
    right_.assign(frames_, 0.f);
    output_.clear();
    sampleTime_ = 0;
    callback_ = callback;
    current_.store(this, std::memory_order_release);
}

void ManualBackend::close() {
    ManualBackend* self = this;
    current_.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
    callback_ = nullptr;
}

void ManualBackend::process(int callbacks) {
    float* outputs[2] = {left_.data(), right_.data()};
    for (int c = 0; c < callbacks && callback_; ++c) {
        AudioIO io;
        io.outputs = outputs;
        io.numOutputs = 2;
        io.frames = frames_;
        io.sampleTime = sampleTime_;
        callback_->audioCallback(io);
        for (uint32_t i = 0; i < frames_; ++i) {
            output_.push_back(left_[i]);
            output_.push_back(right_[i]);
        }
        sampleTime_ += frames_;
    }
}

std::vector<float> ManualBackend::takeOutput() {
    std::vector<float> out;
    out.swap(output_);
    return out;
}

DeviceState ManualBackend::state() const {
    DeviceState state;
    if (!callback_) return state;
    state.driver = kName;
    state.name = "Manual";
    state.sampleRate = rate_;
    state.bufferFrames = frames_;
    state.outputChannels = {0, 1};
    state.capabilities.outputNames = {"Left", "Right"};
    return state;
}

}  // namespace sub
