#pragma once
// Output device wrapper. v1 uses miniaudio's WASAPI backend; the rest of the
// engine only talks to this class, so an ASIO backend can be added behind it.

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ma_context;
struct ma_device;

namespace gil {

struct OutputDeviceInfo {
    std::string name;
    bool isDefault = false;
};

enum class DeviceEvent { None = 0, Stopped, Rerouted };

class AudioCallback {
public:
    virtual ~AudioCallback() = default;
    // Called on the real-time audio thread with interleaved float output.
    virtual void audioCallback(float* out, uint32_t frames, uint32_t channels) noexcept = 0;
    // Called on a backend thread; must only set flags.
    virtual void deviceEvent(DeviceEvent event) noexcept = 0;
};

class AudioDevice {
public:
    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    std::vector<OutputDeviceInfo> outputDevices();

    // Initialises (but does not start) an output device. An empty name selects
    // the system default, sampleRate 0 the device's native rate. Throws on failure.
    void init(const std::string& name, uint32_t sampleRate, uint32_t bufferFrames, bool exclusive,
              AudioCallback* callback);
    void start();
    void close();

    bool isOpen() const { return device_ != nullptr; }
    const std::string& deviceName() const { return name_; }
    std::string backendName() const;
    uint32_t sampleRate() const;
    uint32_t channels() const;
    uint32_t bufferFrames() const;
    double latencySeconds() const;
    bool exclusive() const { return exclusive_; }

private:
    friend struct DeviceCallbacks;

    std::unique_ptr<ma_context> context_;
    bool contextReady_ = false;
    std::unique_ptr<ma_device> device_;
    AudioCallback* callback_ = nullptr;
    std::string name_;
    bool exclusive_ = false;
    std::atomic<bool> closing_{false};
};

}  // namespace gil
