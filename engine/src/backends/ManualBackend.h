#pragma once
// "Manual": a driver type for tests, on every platform (it isn't among
// AudioDevice::driverTypes(), so no one picks it by mistake). No sound card and
// no thread of its own: a test plays the engine a buffer at a time, on its own
// thread, with process(), and reads what came out, as the fake ASIO driver's
// manual mode does where there is ASIO.

#include <atomic>
#include <cstdint>
#include <vector>

#include "AudioDevice.h"

namespace sub {

class ManualBackend final : public AudioBackend {
public:
    static constexpr const char* kName = "Manual";

    // The one open (null: none).
    static ManualBackend* current() noexcept { return current_.load(std::memory_order_acquire); }

    // Calls the engine back `callbacks` times, a buffer each, on this thread.
    void process(int callbacks);
    // What it put out since the last call, interleaved stereo.
    std::vector<float> takeOutput();

    ~ManualBackend() override;
    std::string name() const override { return kName; }
    std::vector<AudioDeviceInfo> devices() override { return {{"Manual", true}}; }
    void open(const DeviceConfig& config, AudioCallback* callback) override;
    void start() override {}
    void close() override;
    bool isOpen() const override { return callback_ != nullptr; }
    DeviceState state() const override;

private:
    static inline std::atomic<ManualBackend*> current_{nullptr};
    AudioCallback* callback_ = nullptr;
    uint32_t rate_ = 48000;
    uint32_t frames_ = 256;
    int64_t sampleTime_ = 0;
    std::vector<float> left_, right_;
    std::vector<float> output_;
};

}  // namespace sub
