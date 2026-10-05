// SUB Test ASIO: a fake ASIO driver for the tests.
//
// It is an in-process COM object like a real driver, loaded through
// SUBSTATION_ASIO_DRIVERS rather than registered. Once started, a thread calls
// the host's bufferSwitch at the pace a sound card would; in manual mode the
// test drives it instead, a buffer at a time (SubTestAsio_Process), so that
// what the host plays can be checked sample by sample.
//
// Functions exported for the tests (test_asio_driver.def;
// tests/engine/harness/AsioDriver.h) configure the next driver instance, set
// what its inputs deliver, send the host driver messages, and read back the
// bytes the host wrote to its outputs, or loop an output back to an input as a
// cable would (SubTestAsio_SetLoopback). Its sample formats are encoded here
// independently of the engine's conversions, which the tests check against
// their own decoding.

#include <windows.h>
#include <objbase.h>
#include <unknwn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "iasiodrv.h"

namespace {

// {5B2E8C1A-7F3D-4E6B-9C0A-1D2F3E4A5B6C}; the tests list the driver with it.
const CLSID kClassId = {0x5b2e8c1a, 0x7f3d, 0x4e6b, {0x9c, 0x0a, 0x1d, 0x2f, 0x3e, 0x4a, 0x5b, 0x6c}};

constexpr long kInputs = 4;
constexpr long kOutputs = 6;
constexpr size_t kMaxCapture = size_t{8} << 20;  // bytes per output channel

// How the next instance behaves (SubTestAsio_Reset restores this).
struct Config {
    long sampleType = ASIOSTInt32LSB;
    long minSize = 32, maxSize = 2048, preferredSize = 256, granularity = -1;
    long inputLatencyExtra = 32, outputLatencyExtra = 64;  // on top of the buffer size
    double rate = 44100.0;                                 // the rate the "hardware" runs at
    bool manual = false;
    std::string initError;  // init() fails with this message
    long panelBufferSize = 0;  // what the control panel changes, if not 0
    double panelRate = 0.0;
    double inputLevels[kInputs] = {0.1, 0.2, 0.3, 0.4};
    long loopOutput = -1, loopInput = -1;  // a cable from that output to that input
};

struct Stats {
    long instances = 0;  // alive
    long inits = 0;
    long controlPanels = 0;
    long starts = 0;
    long running = 0;
    long switches = 0;
    long timeInfo = 0;   // the host takes bufferSwitchTimeInfo
    long bufferSize = 0;
    long inputs = 0;     // channels the host opened
    long outputs = 0;
    void* initHandle = nullptr;
};

std::mutex g_mutex;  // guards the globals against the driver's thread
Config g_config;
Stats g_stats;
std::atomic<long> g_outputReady{0};
std::vector<std::vector<uint8_t>> g_capture(kOutputs);
ASIOCallbacks* g_callbacks = nullptr;  // the host's, while buffers exist

class TestAsio;
std::atomic<TestAsio*> g_driver{nullptr};  // the instance alive (the tests use one at a time)

bool isSupported(double rate) { return rate == 44100.0 || rate == 48000.0 || rate == 96000.0; }

int sampleBytes(long type) {
    switch (type) {
        case ASIOSTInt16LSB: case ASIOSTInt16MSB: return 2;
        case ASIOSTInt24LSB: case ASIOSTInt24MSB: return 3;
        case ASIOSTFloat64LSB: case ASIOSTFloat64MSB: return 8;
        default: return 4;
    }
}

void storeInt(uint8_t* dst, int64_t value, int bytes, bool bigEndian) {
    for (int b = 0; b < bytes; ++b) dst[bigEndian ? bytes - 1 - b : b] = static_cast<uint8_t>(value >> (8 * b));
}

// One sample of `value` in the driver's format.
void encode(long type, double value, uint8_t* dst) {
    const auto scaled = [value](int bits) { return std::llround(value * ((int64_t{1} << (bits - 1)) - 1)); };
    switch (type) {
        case ASIOSTInt16LSB: storeInt(dst, scaled(16), 2, false); break;
        case ASIOSTInt24LSB: storeInt(dst, scaled(24), 3, false); break;
        case ASIOSTInt32LSB: storeInt(dst, scaled(32), 4, false); break;
        case ASIOSTInt32LSB16: storeInt(dst, scaled(16), 4, false); break;
        case ASIOSTInt32LSB18: storeInt(dst, scaled(18), 4, false); break;
        case ASIOSTInt32LSB20: storeInt(dst, scaled(20), 4, false); break;
        case ASIOSTInt32LSB24: storeInt(dst, scaled(24), 4, false); break;
        case ASIOSTInt16MSB: storeInt(dst, scaled(16), 2, true); break;
        case ASIOSTInt24MSB: storeInt(dst, scaled(24), 3, true); break;
        case ASIOSTInt32MSB: storeInt(dst, scaled(32), 4, true); break;
        case ASIOSTFloat32LSB: {
            const auto sample = static_cast<float>(value);
            std::memcpy(dst, &sample, 4);
            break;
        }
        case ASIOSTFloat64LSB: std::memcpy(dst, &value, 8); break;
        case ASIOSTFloat32MSB: {
            const auto sample = static_cast<float>(value);
            uint32_t bits;
            std::memcpy(&bits, &sample, 4);
            storeInt(dst, bits, 4, true);
            break;
        }
        default: std::memset(dst, 0, static_cast<size_t>(sampleBytes(type))); break;
    }
}

int64_t loadInt(const uint8_t* src, int bytes, bool bigEndian) {
    uint64_t value = 0;
    for (int b = 0; b < bytes; ++b) value |= static_cast<uint64_t>(src[bigEndian ? bytes - 1 - b : b]) << (8 * b);
    const int shift = 64 - 8 * bytes;
    return static_cast<int64_t>(value << shift) >> shift;  // sign-extended
}

// The value of one sample in the driver's format (what encode() wrote back).
double decode(long type, const uint8_t* src) {
    const auto scaled = [](int64_t value, int bits) { return static_cast<double>(value) / ((int64_t{1} << (bits - 1)) - 1); };
    switch (type) {
        case ASIOSTInt16LSB: return scaled(loadInt(src, 2, false), 16);
        case ASIOSTInt24LSB: return scaled(loadInt(src, 3, false), 24);
        case ASIOSTInt32LSB: return scaled(loadInt(src, 4, false), 32);
        case ASIOSTInt32LSB16: return scaled(loadInt(src, 4, false), 16);
        case ASIOSTInt32LSB18: return scaled(loadInt(src, 4, false), 18);
        case ASIOSTInt32LSB20: return scaled(loadInt(src, 4, false), 20);
        case ASIOSTInt32LSB24: return scaled(loadInt(src, 4, false), 24);
        case ASIOSTInt16MSB: return scaled(loadInt(src, 2, true), 16);
        case ASIOSTInt24MSB: return scaled(loadInt(src, 3, true), 24);
        case ASIOSTInt32MSB: return scaled(loadInt(src, 4, true), 32);
        case ASIOSTFloat32LSB: {
            float sample;
            std::memcpy(&sample, src, 4);
            return sample;
        }
        case ASIOSTFloat64LSB: {
            double sample;
            std::memcpy(&sample, src, 8);
            return sample;
        }
        case ASIOSTFloat32MSB: {
            const auto bits = static_cast<uint32_t>(loadInt(src, 4, true));
            float sample;
            std::memcpy(&sample, &bits, 4);
            return sample;
        }
        default: return 0.0;
    }
}

class TestAsio final : public IASIO {
public:
    TestAsio() {
        std::lock_guard lock(g_mutex);
        ++g_stats.instances;
    }
    ~TestAsio() {
        stop();
        disposeBuffers();
        TestAsio* self = this;
        g_driver.compare_exchange_strong(self, nullptr);
        std::lock_guard lock(g_mutex);
        --g_stats.instances;
    }

    // IUnknown. ASIO drivers answer to their class id as the interface id.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, kClassId)) {
            AddRef();
            *object = this;
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (left == 0) delete this;
        return left;
    }

    ASIOBool init(void* sysHandle) override {
        std::lock_guard lock(g_mutex);
        ++g_stats.inits;
        g_stats.initHandle = sysHandle;
        error_ = g_config.initError;
        rate_ = g_config.rate;
        sampleType_ = g_config.sampleType;
        return error_.empty() ? ASIOTrue : ASIOFalse;
    }
    void getDriverName(char* name) override { snprintf(name, 32, "SUB Test ASIO"); }
    long getDriverVersion() override { return 1; }
    void getErrorMessage(char* text) override { snprintf(text, 124, "%s", error_.c_str()); }

    ASIOError start() override {
        if (buffers_.empty()) return ASE_InvalidMode;
        if (running_) return ASE_OK;
        running_ = true;
        bool manual;
        {
            std::lock_guard lock(g_mutex);
            ++g_stats.starts;
            g_stats.running = 1;
            manual = g_config.manual;
            // The cable: what leaves the output comes back at the input as late as
            // the latencies the driver reports say.
            const long delay = 2 * bufferSize_ + g_config.inputLatencyExtra + g_config.outputLatencyExtra;
            loop_.assign(static_cast<size_t>(delay), 0.0);
        }
        if (!manual) {
            thread_ = std::thread([this] {
                const auto period = std::chrono::duration<double>(static_cast<double>(bufferSize_) / rate_);
                auto next = std::chrono::steady_clock::now();
                while (running_) {
                    switchBuffers();
                    next += std::chrono::duration_cast<std::chrono::steady_clock::duration>(period);
                    std::this_thread::sleep_until(next);
                }
            });
        }
        return ASE_OK;
    }
    ASIOError stop() override {
        running_ = false;
        if (thread_.joinable()) thread_.join();
        std::lock_guard lock(g_mutex);
        g_stats.running = 0;
        return ASE_OK;
    }

    ASIOError getChannels(long* numInputs, long* numOutputs) override {
        *numInputs = kInputs;
        *numOutputs = kOutputs;
        return ASE_OK;
    }
    ASIOError getLatencies(long* input, long* output) override {
        std::lock_guard lock(g_mutex);
        *input = bufferSize_ + g_config.inputLatencyExtra;
        *output = bufferSize_ + g_config.outputLatencyExtra;
        return ASE_OK;
    }
    ASIOError getBufferSize(long* minSize, long* maxSize, long* preferred, long* granularity) override {
        std::lock_guard lock(g_mutex);
        *minSize = g_config.minSize;
        *maxSize = g_config.maxSize;
        *preferred = g_config.preferredSize;
        *granularity = g_config.granularity;
        return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate rate) override { return isSupported(rate) ? ASE_OK : ASE_NoClock; }
    ASIOError getSampleRate(ASIOSampleRate* rate) override {
        *rate = rate_;
        return ASE_OK;
    }
    ASIOError setSampleRate(ASIOSampleRate rate) override {
        if (!isSupported(rate)) return ASE_NoClock;
        rate_ = rate;
        std::lock_guard lock(g_mutex);
        g_config.rate = rate;
        return ASE_OK;
    }
    ASIOError getClockSources(ASIOClockSource* clocks, long* count) override {
        clocks[0] = {};
        clocks[0].associatedChannel = -1;
        clocks[0].isCurrentSource = ASIOTrue;
        snprintf(clocks[0].name, sizeof(clocks[0].name), "Internal");
        *count = 1;
        return ASE_OK;
    }
    ASIOError setClockSource(long reference) override { return reference == 0 ? ASE_OK : ASE_InvalidParameter; }
    ASIOError getSamplePosition(ASIOSamples* position, ASIOTimeStamp* time) override {
        const auto samples = static_cast<uint64_t>(samplePosition_.load());
        position->hi = static_cast<unsigned long>(samples >> 32);
        position->lo = static_cast<unsigned long>(samples & 0xffffffffu);
        time->hi = time->lo = 0;
        return ASE_OK;
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        const long count = info->isInput ? kInputs : kOutputs;
        if (info->channel < 0 || info->channel >= count) return ASE_InvalidParameter;
        info->isActive = ASIOFalse;
        for (const Buffer& buffer : buffers_) {
            if (buffer.isInput == (info->isInput != ASIOFalse) && buffer.channel == info->channel) info->isActive = ASIOTrue;
        }
        info->channelGroup = 0;
        info->type = sampleType_;
        snprintf(info->name, sizeof(info->name), "Test %s %ld", info->isInput ? "In" : "Out", info->channel + 1);
        return ASE_OK;
    }

    ASIOError createBuffers(ASIOBufferInfo* infos, long count, long size, ASIOCallbacks* callbacks) override {
        long minSize, maxSize, preferred, granularity;
        getBufferSize(&minSize, &maxSize, &preferred, &granularity);
        const bool power = (size & (size - 1)) == 0;
        if (size < minSize || size > maxSize || (granularity == -1 && !power) || (granularity == 0 && size != preferred)) {
            return ASE_InvalidMode;
        }
        disposeBuffers();
        const auto bytes = static_cast<size_t>(size) * sampleBytes(sampleType_);
        long inputs = 0, outputs = 0;
        for (long i = 0; i < count; ++i) {
            ASIOBufferInfo& info = infos[i];
            const bool input = info.isInput != ASIOFalse;
            if (info.channelNum < 0 || info.channelNum >= (input ? kInputs : kOutputs)) {
                buffers_.clear();
                return ASE_InvalidParameter;
            }
            buffers_.push_back({input, info.channelNum, std::vector<uint8_t>(2 * bytes, 0xAB)});  // not silence
            (input ? inputs : outputs) += 1;
        }
        for (long i = 0; i < count; ++i) {
            infos[i].buffers[0] = buffers_[static_cast<size_t>(i)].memory.data();
            infos[i].buffers[1] = buffers_[static_cast<size_t>(i)].memory.data() + bytes;
        }
        bufferSize_ = size;
        index_ = 0;
        samplePosition_ = 0;
        timeInfo_ = callbacks->asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1;
        std::lock_guard lock(g_mutex);
        g_callbacks = callbacks;
        g_stats.bufferSize = size;
        g_stats.inputs = inputs;
        g_stats.outputs = outputs;
        g_stats.timeInfo = timeInfo_ ? 1 : 0;
        return ASE_OK;
    }
    ASIOError disposeBuffers() override {
        stop();
        buffers_.clear();
        std::lock_guard lock(g_mutex);
        g_callbacks = nullptr;
        g_stats.inputs = g_stats.outputs = 0;
        return ASE_OK;
    }

    ASIOError controlPanel() override {
        std::lock_guard lock(g_mutex);
        ++g_stats.controlPanels;
        if (g_config.panelBufferSize > 0) {
            g_config.minSize = g_config.maxSize = g_config.preferredSize = g_config.panelBufferSize;
            g_config.granularity = 0;
        }
        if (g_config.panelRate > 0.0) rate_ = g_config.rate = g_config.panelRate;
        return ASE_OK;
    }
    ASIOError future(long selector, void*) override {
        return selector == kAsioCanTimeInfo ? ASE_SUCCESS : ASE_NotPresent;
    }
    ASIOError outputReady() override {
        ++g_outputReady;
        return ASE_OK;
    }

    // One period: the inputs deliver, the host processes, the outputs are recorded.
    void switchBuffers() {
        ASIOCallbacks* callbacks;
        const long index = index_;
        const auto bytes = static_cast<size_t>(bufferSize_) * sampleBytes(sampleType_);
        {
            std::lock_guard lock(g_mutex);
            callbacks = g_callbacks;
            for (Buffer& buffer : buffers_) {
                if (!buffer.isInput) continue;
                uint8_t* half = buffer.memory.data() + index * bytes;
                const bool looped = g_config.loopOutput >= 0 && buffer.channel == g_config.loopInput;
                const double level = g_config.inputLevels[buffer.channel];
                for (long i = 0; i < bufferSize_; ++i) {
                    const double value = looped ? loop_[static_cast<size_t>(i)] : level;
                    encode(sampleType_, value, half + i * sampleBytes(sampleType_));
                }
            }
        }
        if (!callbacks) return;
        if (timeInfo_) {
            ASIOTime time{};
            const auto position = static_cast<uint64_t>(samplePosition_.load());
            time.timeInfo.samplePosition.hi = static_cast<unsigned long>(position >> 32);
            time.timeInfo.samplePosition.lo = static_cast<unsigned long>(position & 0xffffffffu);
            time.timeInfo.sampleRate = rate_;
            time.timeInfo.speed = 1.0;
            time.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid;
            callbacks->bufferSwitchTimeInfo(&time, index, ASIOTrue);
        } else {
            callbacks->bufferSwitch(index, ASIOTrue);
        }
        std::lock_guard lock(g_mutex);
        loop_.erase(loop_.begin(), loop_.begin() + bufferSize_);
        const Buffer* cable = nullptr;
        for (const Buffer& buffer : buffers_) {
            if (!buffer.isInput && buffer.channel == g_config.loopOutput) cable = &buffer;
        }
        for (long i = 0; i < bufferSize_; ++i) {  // silence if no cable (or its output isn't open)
            loop_.push_back(cable ? decode(sampleType_, cable->memory.data() + index * bytes + i * sampleBytes(sampleType_))
                                  : 0.0);
        }
        for (const Buffer& buffer : buffers_) {
            if (buffer.isInput) continue;
            auto& capture = g_capture[static_cast<size_t>(buffer.channel)];
            if (capture.size() + bytes > kMaxCapture) continue;
            const uint8_t* half = buffer.memory.data() + index * bytes;
            capture.insert(capture.end(), half, half + bytes);
        }
        index_ = 1 - index;
        samplePosition_ += bufferSize_;
        ++g_stats.switches;
    }

    bool running() const { return running_; }
    bool manual() const {
        std::lock_guard lock(g_mutex);
        return g_config.manual;
    }
    void setRate(double rate) { rate_ = rate; }

private:
    struct Buffer {
        bool isInput;
        long channel;
        std::vector<uint8_t> memory;  // both halves
    };

    std::atomic<ULONG> references_{1};
    std::string error_;
    double rate_ = 44100.0;
    long sampleType_ = ASIOSTInt32LSB;
    long bufferSize_ = 0;
    long index_ = 0;
    bool timeInfo_ = false;
    std::atomic<int64_t> samplePosition_{0};
    std::vector<Buffer> buffers_;
    std::deque<double> loop_;  // the cable's samples on their way (SubTestAsio_SetLoopback)
    std::atomic<bool> running_{false};
    std::thread thread_;
};

class Factory final : public IClassFactory {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IClassFactory)) {
            *object = this;
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** object) override {
        if (outer) return CLASS_E_NOAGGREGATION;
        auto* driver = new TestAsio();
        const HRESULT result = driver->QueryInterface(iid, object);
        if (SUCCEEDED(result)) g_driver = driver;
        driver->Release();
        return result;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL) override { return S_OK; }
};

Factory g_factory;

}  // namespace

STDAPI DllGetClassObject(REFCLSID clsid, REFIID iid, LPVOID* object) {
    if (!IsEqualCLSID(clsid, kClassId)) return CLASS_E_CLASSNOTAVAILABLE;
    return g_factory.QueryInterface(iid, object);
}

STDAPI DllCanUnloadNow() { return S_FALSE; }

// --- Test hooks (ctypes) ---------------------------------------------------------

extern "C" {

void SubTestAsio_Reset() {
    std::lock_guard lock(g_mutex);
    g_config = {};
    const long instances = g_stats.instances;
    g_stats = {};
    g_stats.instances = instances;
    g_outputReady = 0;
    for (auto& capture : g_capture) capture.clear();
}

void SubTestAsio_SetSampleType(long type) {
    std::lock_guard lock(g_mutex);
    g_config.sampleType = type;
}

void SubTestAsio_SetBufferSizes(long minSize, long maxSize, long preferred, long granularity) {
    std::lock_guard lock(g_mutex);
    g_config.minSize = minSize;
    g_config.maxSize = maxSize;
    g_config.preferredSize = preferred;
    g_config.granularity = granularity;
}

void SubTestAsio_SetLatencies(long inputExtra, long outputExtra) {
    std::lock_guard lock(g_mutex);
    g_config.inputLatencyExtra = inputExtra;
    g_config.outputLatencyExtra = outputExtra;
}

void SubTestAsio_SetManual(int manual) {
    std::lock_guard lock(g_mutex);
    g_config.manual = manual != 0;
}

void SubTestAsio_SetInputLevel(int channel, double level) {
    std::lock_guard lock(g_mutex);
    if (channel >= 0 && channel < kInputs) g_config.inputLevels[channel] = level;
}

// A cable from an output to an input (-1, -1: none), delayed by the input and
// output latencies the driver reports, as a real loopback would be.
void SubTestAsio_SetLoopback(int output, int input) {
    std::lock_guard lock(g_mutex);
    g_config.loopOutput = output;
    g_config.loopInput = input;
}

void SubTestAsio_FailInit(const char* message) {
    std::lock_guard lock(g_mutex);
    g_config.initError = message ? message : "";
}

void SubTestAsio_SetControlPanelChange(long bufferSize, double rate) {
    std::lock_guard lock(g_mutex);
    g_config.panelBufferSize = bufferSize;
    g_config.panelRate = rate;
}

// Manual mode: runs `buffers` periods now, on the caller's thread. Returns how many ran.
int SubTestAsio_Process(int buffers) {
    TestAsio* driver = g_driver;
    if (!driver || !driver->running() || !driver->manual()) return 0;
    for (int i = 0; i < buffers; ++i) driver->switchBuffers();
    return buffers;
}

// Copies what the host wrote to an output so far (raw, in the driver's format).
// With no destination, returns how many bytes there are.
long SubTestAsio_ReadOutput(int channel, void* destination, long maxBytes) {
    std::lock_guard lock(g_mutex);
    if (channel < 0 || channel >= kOutputs) return 0;
    const auto& capture = g_capture[static_cast<size_t>(channel)];
    if (!destination) return static_cast<long>(capture.size());
    const size_t count = std::min(capture.size(), static_cast<size_t>(std::max(0L, maxBytes)));
    std::memcpy(destination, capture.data(), count);
    return static_cast<long>(count);
}

void SubTestAsio_ClearOutput() {
    std::lock_guard lock(g_mutex);
    for (auto& capture : g_capture) capture.clear();
}

// Sends the host a driver message now, from the caller's thread. -1 if no host listens.
long SubTestAsio_SendMessage(long selector, long value) {
    ASIOCallbacks* callbacks;
    {
        std::lock_guard lock(g_mutex);
        callbacks = g_callbacks;
    }
    return callbacks ? callbacks->asioMessage(selector, value, nullptr, nullptr) : -1;
}

// The "hardware" changes its rate (an external clock), and tells the host.
void SubTestAsio_ChangeSampleRate(double rate) {
    ASIOCallbacks* callbacks;
    {
        std::lock_guard lock(g_mutex);
        g_config.rate = rate;
        callbacks = g_callbacks;
    }
    if (TestAsio* driver = g_driver) driver->setRate(rate);
    if (callbacks) callbacks->sampleRateDidChange(rate);
}

double SubTestAsio_Get(const char* key) {
    std::lock_guard lock(g_mutex);
    const std::string name = key;
    if (name == "instances") return g_stats.instances;
    if (name == "inits") return g_stats.inits;
    if (name == "control_panels") return g_stats.controlPanels;
    if (name == "starts") return g_stats.starts;
    if (name == "running") return g_stats.running;
    if (name == "switches") return g_stats.switches;
    if (name == "time_info") return g_stats.timeInfo;
    if (name == "buffer_size") return g_stats.bufferSize;
    if (name == "inputs") return g_stats.inputs;
    if (name == "outputs") return g_stats.outputs;
    if (name == "output_ready") return static_cast<double>(g_outputReady.load());
    if (name == "rate") return g_config.rate;
    return -1.0;
}

void* SubTestAsio_InitHandle() {
    std::lock_guard lock(g_mutex);
    return g_stats.initHandle;
}

}  // extern "C"
