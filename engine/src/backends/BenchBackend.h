#pragma once
// "Bench": a driver type for the performance experiments (perf-experiments
// branch only). No sound card: a thread of its own calls the engine back once
// per buffer, on the clock, as a driver's real-time thread would (SCHED_FIFO
// where the system lets it), and records how long each callback took and how
// late it started. Outputs go nowhere.

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "AudioDevice.h"

namespace sub {

class BenchBackend final : public AudioBackend {
public:
    static constexpr const char* kName = "Bench";

    struct Stats {
        std::vector<float> callbackUs;  // how long each callback took
        std::vector<float> lateUs;      // how late each started, against its deadline
        bool realtime = false;          // the thread got SCHED_FIFO
    };
    // Any thread, with the device closed or running: what was recorded since the last call.
    static Stats takeStats();

    ~BenchBackend() override;
    std::string name() const override { return kName; }
    std::vector<AudioDeviceInfo> devices() override { return {{"Bench", true}}; }
    void open(const DeviceConfig& config, AudioCallback* callback) override;
    void start() override;
    void close() override;
    bool isOpen() const override { return callback_ != nullptr; }
    DeviceState state() const override;

private:
    void run() noexcept;

    AudioCallback* callback_ = nullptr;
    uint32_t rate_ = 48000;
    uint32_t frames_ = 128;
    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::vector<float> left_, right_;
};

}  // namespace sub
