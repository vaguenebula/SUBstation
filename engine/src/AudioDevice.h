#pragma once
// Audio devices. Each driver type is an AudioBackend: WASAPI (through
// miniaudio), and ASIO when the engine is built with Steinberg's ASIO SDK. The
// engine only talks to AudioDevice, which opens a device of either.
//
// Devices run duplex: each callback gets the open input channels and fills the
// open output channels, as separate (non-interleaved) float buffers of the same
// length. Each callback also says where it is on the device's sample clock and
// when it began, which is what recording and MIDI input need to line their
// material up with the audio.
//
// Threads: a backend is opened, queried and closed on one thread (the UI
// thread: ASIO drivers are COM objects in its apartment). The audio callback
// runs on the backend's real-time thread; device events may come on any thread.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sub {

// The driver type every build has: WASAPI on Windows, miniaudio's default
// backend ("System") elsewhere (backends/WasapiBackend.h).
#ifdef _WIN32
inline constexpr const char* kDefaultDriver = "WASAPI";
#else
inline constexpr const char* kDefaultDriver = "System";
#endif

struct AudioDeviceInfo {
    std::string name;
    bool isDefault = false;
};

// A device to open.
struct DeviceConfig {
    std::string driver = kDefaultDriver;  // one of AudioDevice::driverTypes()
    std::string name;                  // empty: the system default (WASAPI), the first driver (ASIO)
    uint32_t sampleRate = 0;           // 0: the rate the device runs at
    uint32_t bufferFrames = 0;         // 0: the device's preferred size
    bool exclusive = false;            // WASAPI exclusive mode
    std::vector<int> inputChannels;    // device channels to open (0-based); none by default
    std::vector<int> outputChannels;   // empty: the first two (or the only one)
    uintptr_t window = 0;              // the application's main window (HWND); ASIO drivers own their dialogs by it
};

// What the open device offers.
struct DeviceCaps {
    std::vector<std::string> inputNames;   // all of the device's channels, open or not
    std::vector<std::string> outputNames;
    std::vector<uint32_t> sampleRates;     // the rates it can run at; empty: any
    std::vector<uint32_t> bufferSizes;     // the sizes it offers; empty: any
    uint32_t preferredBufferFrames = 0;
    bool hasControlPanel = false;
};

// The open device.
struct DeviceState {
    std::string driver;
    std::string name;
    uint32_t sampleRate = 0;
    uint32_t bufferFrames = 0;
    uint32_t inputLatency = 0;         // frames, as the driver reports them
    uint32_t outputLatency = 0;
    std::vector<int> inputChannels;    // the open channels, in the callback's order
    std::vector<int> outputChannels;
    bool exclusive = false;
    DeviceCaps capabilities;
};

// One callback's audio. The callback must write every output sample.
struct AudioIO {
    const float* const* inputs = nullptr;
    uint32_t numInputs = 0;
    float* const* outputs = nullptr;
    uint32_t numOutputs = 0;
    uint32_t frames = 0;
    int64_t sampleTime = 0;  // the device's sample clock at the first frame
    int64_t hostTimeNs = 0;  // std::chrono::steady_clock (QueryPerformanceCounter) when the callback began
};

// Flags, so that several can be pending at once.
enum class DeviceEvent : uint32_t {
    None = 0,
    Stopped = 1u << 0,         // the device went away
    Rerouted = 1u << 1,        // the system moved the output to another device
    ResetRequest = 1u << 2,    // the driver's settings changed: close it and open it again
    LatencyChanged = 1u << 3,  // the driver reports new latencies
};

class AudioCallback {
public:
    virtual ~AudioCallback() = default;
    // The real-time thread. Never locks, allocates or waits.
    virtual void audioCallback(const AudioIO& io) noexcept = 0;
    // Any thread; must only set flags.
    virtual void deviceEvent(DeviceEvent event) noexcept = 0;
};

// A driver type.
class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    virtual std::string name() const = 0;
    virtual std::vector<AudioDeviceInfo> devices() = 0;

    // Opens (but doesn't start) a device. Throws std::runtime_error with a
    // message for the user; nothing is open then.
    virtual void open(const DeviceConfig& config, AudioCallback* callback) = 0;
    virtual void start() = 0;
    virtual void close() = 0;  // returns once the callback can no longer run
    virtual bool isOpen() const = 0;
    virtual DeviceState state() const = 0;

    // The driver's own settings dialog, if it has one (ASIO). It may run a
    // modal message loop; inControlPanel() is true meanwhile.
    virtual bool showControlPanel() { return false; }
    virtual bool inControlPanel() const { return false; }
    // Main-thread work after DeviceEvent::LatencyChanged.
    virtual void refreshLatencies() {}
    // Before a reset (DeviceEvent::ResetRequest): what to open instead, if the
    // driver asked for another buffer size or changed its sample rate.
    virtual void adjustForReset(DeviceConfig&) {}
};

// The device the engine plays through, of any driver type.
class AudioDevice {
public:
    AudioDevice();
    ~AudioDevice();
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // The driver types this engine was built with: kDefaultDriver ("WASAPI" on
    // Windows), and "ASIO".
    static std::vector<std::string> driverTypes();
    std::vector<AudioDeviceInfo> devices(const std::string& driver);

    // Closes the open device (of any driver type), then opens this one.
    void open(const DeviceConfig& config, AudioCallback* callback);
    void start();
    void close();
    bool isOpen() const;
    DeviceState state() const;

    // The last device opened, with what its driver asked to change since (for a reset).
    bool hasConfig() const { return hasConfig_; }
    DeviceConfig resetConfig();

    bool showControlPanel();
    bool inControlPanel() const;
    void refreshLatencies();

private:
    AudioBackend& backend(const std::string& driver);

    std::vector<std::unique_ptr<AudioBackend>> backends_;
    AudioBackend* current_ = nullptr;
    DeviceConfig config_;
    bool hasConfig_ = false;
};

}  // namespace sub
