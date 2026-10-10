#pragma once
// Audio through miniaudio: on Windows WASAPI, shared or exclusive (the
// "WASAPI" driver); elsewhere miniaudio's own choice of the system's backends
// (PulseAudio, ALSA, Core Audio...), as the "System" driver. Output only for
// now: a capture device (for recording) would open as miniaudio's duplex mode
// and fill AudioIO::inputs.

#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "../AudioDevice.h"

struct ma_context;
struct ma_device;

namespace sub {

class MiniaudioBackend final : public AudioBackend {
public:
    MiniaudioBackend();
    ~MiniaudioBackend() override;

    static constexpr const char* kName = kDefaultDriver;
    std::string name() const override { return kName; }
    std::vector<AudioDeviceInfo> devices() override;
    void open(const DeviceConfig& config, AudioCallback* callback) override;
    void start() override;
    void close() override;
    bool isOpen() const override { return device_ != nullptr; }
    DeviceState state() const override;

private:
    friend struct MiniaudioCallbacks;
    void process(float* interleaved, uint32_t frames, uint32_t channels) noexcept;

    static constexpr uint32_t kChunk = 4096;  // frames per engine callback; longer device buffers take several

    std::unique_ptr<ma_context> context_;
    bool contextReady_ = false;
    std::unique_ptr<ma_device> device_;
    AudioCallback* callback_ = nullptr;
    std::string name_;
    bool exclusive_ = false;
    std::atomic<bool> closing_{false};
    std::vector<float> scratch_;         // the planar output channels, kChunk frames each
    std::array<float*, 2> outputs_{};
    int64_t sampleTime_ = 0;
};

}  // namespace sub
