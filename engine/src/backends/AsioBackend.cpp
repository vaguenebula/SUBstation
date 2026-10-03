// ASIO drivers are in-process COM objects, listed in the registry under
// HKLM\SOFTWARE\ASIO. The host talks to one through the IASIO interface (only
// the SDK's headers are used) and gets its audio in callbacks on the driver's
// thread: each bufferSwitch() hands over one half of a double buffer per open
// channel, in the driver's own sample format.
//
//  * The callbacks carry no context, so one ASIO device can be open per
//    process; they find it through `g_open`.
//  * The driver is created, set up, asked about and released on the thread that
//    opens it (the UI thread). The audio thread calls nothing on it but
//    outputReady(). Drivers are apartment-threaded COM objects, which COM can
//    only create directly in a single-threaded apartment (IASIO can't be
//    marshalled): the backend puts the engine's thread in one before miniaudio
//    would make it multithreaded, as Qt does in the application. In a thread
//    that is multithreaded all the same, the driver is loaded from its DLL.
//  * When the driver's settings change (in its control panel, or its clock) it
//    asks for a reset, and the UI thread closes it and opens it again
//    (Engine::reopenDevice) with the buffer size and sample rate it now has.
//  * SUBSTATION_ASIO_DRIVERS lists drivers to use instead of the installed ones,
//    loaded straight from their DLLs, unregistered (the tests' driver):
//    "Name|{CLSID}|C:\path\driver.dll", several separated by ';'.

#include "AsioBackend.h"

#include <windows.h>
#include <objbase.h>
#include <unknwn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "AsioSupport.h"
#include "iasiodrv.h"

namespace sub {
namespace {

using Clock = std::chrono::steady_clock;

class AsioBackend;
std::atomic<AsioBackend*> g_open{nullptr};  // the open device, for the callbacks
std::atomic<int> g_inCallback{0};           // callbacks running now (see AsioBackend::close)

class CallbackScope {
public:
    CallbackScope() noexcept { g_inCallback.fetch_add(1); }
    ~CallbackScope() { g_inCallback.fetch_sub(1); }
    CallbackScope(const CallbackScope&) = delete;
    CallbackScope& operator=(const CallbackScope&) = delete;
};

void onBufferSwitch(long index, ASIOBool directProcess);
ASIOTime* onBufferSwitchTimeInfo(ASIOTime* time, long index, ASIOBool directProcess);
void onSampleRateChanged(ASIOSampleRate rate);
long onMessage(long selector, long value, void* message, double* opt);

// The rates offered, of those the driver can run at.
constexpr uint32_t kSampleRates[] = {32000, 44100, 48000, 88200, 96000, 176400, 192000, 352800, 384000};

// --- Strings -------------------------------------------------------------------

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                                         nullptr, nullptr);
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr,
                        nullptr);
    return result;
}

// Drivers give their names in the system's ANSI code page.
std::string fromAnsi(const char* text, size_t maxLength) {
    const auto length = static_cast<int>(strnlen(text, maxLength));
    if (length == 0) return {};
    const int size = MultiByteToWideChar(CP_ACP, 0, text, length, nullptr, 0);
    std::wstring wide(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, length, wide.data(), size);
    return utf8(wide);
}

std::vector<std::wstring> split(const std::wstring& text, wchar_t separator) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (size_t at; (at = text.find(separator, start)) != std::wstring::npos; start = at + 1) {
        parts.push_back(text.substr(start, at - start));
    }
    parts.push_back(text.substr(start));
    return parts;
}

std::string hertz(double rate) { return std::to_string(std::llround(rate)) + " Hz"; }

std::string errorText(ASIOError error) {
    switch (error) {
        case ASE_NotPresent: return "the hardware is not present or not available";
        case ASE_HWMalfunction: return "the hardware is malfunctioning";
        case ASE_InvalidParameter: return "invalid parameter";
        case ASE_InvalidMode: return "the hardware is in a mode that doesn't allow it";
        case ASE_SPNotAdvancing: return "the hardware is not running";
        case ASE_NoClock: return "no sample clock";
        case ASE_NoMemory: return "not enough memory";
        default: return "error " + std::to_string(error);
    }
}

int64_t toInt64(const ASIOSamples& samples) {
    return static_cast<int64_t>((static_cast<uint64_t>(samples.hi) << 32) | static_cast<uint32_t>(samples.lo));
}

// --- Finding and loading drivers -------------------------------------------------

struct DriverEntry {
    std::string name;
    CLSID clsid{};
    std::wstring server;  // its DLL (InprocServer32)
    bool viaCom = true;   // false: from SUBSTATION_ASIO_DRIVERS, loaded from `server` rather than through COM
};

std::wstring registryString(HKEY key, const std::wstring& subKey, const wchar_t* value, DWORD types) {
    DWORD size = 0;
    if (RegGetValueW(key, subKey.c_str(), value, types, nullptr, nullptr, &size) != ERROR_SUCCESS || size == 0) {
        return {};
    }
    std::wstring text(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(key, subKey.c_str(), value, types, nullptr, text.data(), &size) != ERROR_SUCCESS) return {};
    text.resize(wcsnlen(text.c_str(), text.size()));
    return text;
}

// The DLL registered for a class; empty if there is none. Uninstalled drivers
// often leave their entry in the ASIO list behind.
std::wstring serverOf(const std::wstring& clsid) {
    // RRF_RT_REG_SZ takes REG_EXPAND_SZ too, expanded (asking for that type itself needs RRF_NOEXPAND).
    const std::wstring server =
        registryString(HKEY_CLASSES_ROOT, L"CLSID\\" + clsid + L"\\InprocServer32", nullptr, RRF_RT_REG_SZ);
    const bool hasFolder = server.find_first_of(L"\\/") != std::wstring::npos;
    if (hasFolder && GetFileAttributesW(server.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    return server;
}

std::vector<DriverEntry> listDrivers() {
    std::vector<DriverEntry> drivers;
    if (const DWORD needed = GetEnvironmentVariableW(L"SUBSTATION_ASIO_DRIVERS", nullptr, 0); needed > 0) {
        std::wstring list(needed, L'\0');
        list.resize(GetEnvironmentVariableW(L"SUBSTATION_ASIO_DRIVERS", list.data(), needed));
        for (const std::wstring& item : split(list, L';')) {
            const std::vector<std::wstring> fields = split(item, L'|');
            DriverEntry driver;
            if (fields.size() != 3 || FAILED(CLSIDFromString(fields[1].c_str(), &driver.clsid))) continue;
            if (GetFileAttributesW(fields[2].c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            driver.name = utf8(fields[0]);
            driver.server = fields[2];
            driver.viaCom = false;
            drivers.push_back(std::move(driver));
        }
        return drivers;
    }

    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root) != ERROR_SUCCESS) return drivers;
    for (DWORD index = 0;; ++index) {
        wchar_t key[256];
        DWORD length = 256;
        const LONG result = RegEnumKeyExW(root, index, key, &length, nullptr, nullptr, nullptr, nullptr);
        if (result == ERROR_NO_MORE_ITEMS) break;
        if (result != ERROR_SUCCESS) continue;
        const std::wstring clsid = registryString(root, key, L"CLSID", RRF_RT_REG_SZ);
        DriverEntry driver;
        if (clsid.empty() || FAILED(CLSIDFromString(clsid.c_str(), &driver.clsid))) continue;
        driver.server = serverOf(clsid);
        if (driver.server.empty()) continue;
        const std::wstring description = registryString(root, key, L"Description", RRF_RT_REG_SZ);
        driver.name = utf8(description.empty() ? std::wstring(key) : description);
        const bool duplicate = std::any_of(drivers.begin(), drivers.end(),
                                           [&](const DriverEntry& other) { return other.name == driver.name; });
        if (!duplicate) drivers.push_back(std::move(driver));
    }
    RegCloseKey(root);
    return drivers;
}

std::string hresultText(HRESULT result) {
    char code[16];
    snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(result));
    wchar_t* text = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                            FORMAT_MESSAGE_IGNORE_INSERTS,
                                        nullptr, static_cast<DWORD>(result), 0, reinterpret_cast<wchar_t*>(&text), 0,
                                        nullptr);
    std::wstring message(text ? text : L"", length);
    if (text) LocalFree(text);
    while (!message.empty() && (message.back() == L'\n' || message.back() == L'\r' || message.back() == L'.')) {
        message.pop_back();
    }
    return message.empty() ? std::string(code) : utf8(message) + ", " + code;
}

// ASIO drivers answer to their class id as the interface id. `singleThreaded`:
// whether the calling thread is in a single-threaded COM apartment.
IASIO* createDriver(const DriverEntry& entry, bool singleThreaded, HMODULE& module) {
    void* object = nullptr;
    if (entry.viaCom && singleThreaded) {
        const HRESULT result = CoCreateInstance(entry.clsid, nullptr, CLSCTX_INPROC_SERVER, entry.clsid, &object);
        if (FAILED(result)) throw std::runtime_error(entry.name + " could not be loaded (" + hresultText(result) + ")");
        return static_cast<IASIO*>(object);
    }
    module = LoadLibraryW(entry.server.c_str());
    if (!module) throw std::runtime_error(entry.name + " could not be loaded from " + utf8(entry.server));
    using GetClassObject = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    const auto getClassObject =
        reinterpret_cast<GetClassObject>(reinterpret_cast<void*>(GetProcAddress(module, "DllGetClassObject")));
    IClassFactory* factory = nullptr;
    HRESULT result = getClassObject ? getClassObject(entry.clsid, IID_IClassFactory, reinterpret_cast<void**>(&factory))
                                    : E_NOINTERFACE;
    if (SUCCEEDED(result)) {
        result = factory->CreateInstance(nullptr, entry.clsid, &object);
        factory->Release();
    }
    if (FAILED(result)) throw std::runtime_error(entry.name + " could not be loaded (" + hresultText(result) + ")");
    return static_cast<IASIO*>(object);
}

// --- The backend ---------------------------------------------------------------------

// Whether the calling thread is in a single-threaded COM apartment (and COM is initialised).
bool inSingleThreadedApartment() {
    APTTYPE type;
    APTTYPEQUALIFIER qualifier;
    return SUCCEEDED(CoGetApartmentType(&type, &qualifier)) && (type == APTTYPE_STA || type == APTTYPE_MAINSTA);
}

class AsioBackend final : public AudioBackend {
public:
    // Before anything else on this thread initialises COM (see the top of the file).
    AsioBackend() : threadCom_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {}

    ~AsioBackend() override {
        close();
        if (threadCom_) CoUninitialize();
    }

    std::string name() const override { return "ASIO"; }

    std::vector<AudioDeviceInfo> devices() override {
        std::vector<AudioDeviceInfo> result;
        for (const DriverEntry& driver : listDrivers()) result.push_back({driver.name, false});
        return result;
    }

    void open(const DeviceConfig& config, AudioCallback* callback) override {
        close();
        const std::vector<DriverEntry> drivers = listDrivers();
        const auto entry = std::find_if(drivers.begin(), drivers.end(), [&](const DriverEntry& driver) {
            return config.name.empty() || driver.name == config.name;
        });
        if (entry == drivers.end()) {
            throw std::runtime_error(config.name.empty() ? "No ASIO driver is installed"
                                                         : "ASIO driver not found: " + config.name);
        }
        AsioBackend* none = nullptr;
        if (!g_open.compare_exchange_strong(none, this)) {
            throw std::runtime_error("Another ASIO device is open (only one can be, in one program)");
        }
        callback_ = callback;
        try {
            comInitialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED));
            driver_ = createDriver(*entry, inSingleThreadedApartment(), module_);
            name_ = entry->name;
            const HWND window = config.window ? reinterpret_cast<HWND>(config.window) : GetDesktopWindow();
            if (!driver_->init(window)) {
                char message[256] = {};
                driver_->getErrorMessage(message);
                const std::string reason = fromAnsi(message, sizeof(message) - 1);
                throw std::runtime_error(name_ + " could not be started" + (reason.empty() ? "" : ": " + reason));
            }
            setUp(config);
        } catch (...) {
            close();
            throw;
        }
    }

    void start() override {
        if (!driver_ || started_) return;
        running_.store(true);
        const ASIOError error = driver_->start();
        if (error != ASE_OK) {
            const std::string message = failure("could not start", error);
            close();
            throw std::runtime_error(message);
        }
        started_ = true;
    }

    void close() override {
        if (driver_ && started_) driver_->stop();
        started_ = false;
        running_.store(false);
        if (g_open.load() == this) {
            g_open.store(nullptr);
            // A callback that found this device before may still be running.
            const auto deadline = Clock::now() + std::chrono::seconds(2);
            while (g_inCallback.load() > 0 && Clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        if (driver_) {
            if (buffersCreated_) driver_->disposeBuffers();
            driver_->Release();
            driver_ = nullptr;
        }
        buffersCreated_ = false;
        if (module_) {
            FreeLibrary(module_);
            module_ = nullptr;
        }
        if (comInitialized_) {
            CoUninitialize();
            comInitialized_ = false;
        }
        callback_ = nullptr;
        name_.clear();
        buffers_.clear();
        scratch_.clear();
        inputPointers_.clear();
        outputPointers_.clear();
    }

    bool isOpen() const override { return driver_ != nullptr; }

    DeviceState state() const override {
        DeviceState state;
        if (!driver_) return state;
        state.driver = name();
        state.name = name_;
        state.sampleRate = static_cast<uint32_t>(std::llround(sampleRate_));
        state.bufferFrames = static_cast<uint32_t>(bufferFrames_);
        state.inputLatency = static_cast<uint32_t>(std::max(0L, inputLatency_));
        state.outputLatency = static_cast<uint32_t>(std::max(0L, outputLatency_));
        state.inputChannels = inputs_;
        state.outputChannels = outputs_;
        state.capabilities = capabilities_;
        return state;
    }

    bool showControlPanel() override {
        if (!driver_) return false;
        inControlPanel_.store(true);
        const ASIOError error = driver_->controlPanel();
        inControlPanel_.store(false);
        // Not every driver asks for a reset after its settings changed.
        if (settingsChanged()) notify(DeviceEvent::ResetRequest);
        return error == ASE_OK;
    }

    bool inControlPanel() const override { return inControlPanel_.load(); }

    void refreshLatencies() override {
        long input = 0, output = 0;
        if (driver_ && driver_->getLatencies(&input, &output) == ASE_OK) {
            inputLatency_ = input;
            outputLatency_ = output;
        }
    }

    void adjustForReset(DeviceConfig& config) override {
        if (!driver_) return;
        long frames = requestedBufferFrames_.exchange(0);
        long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
        if (frames <= 0 && driver_->getBufferSize(&minSize, &maxSize, &preferred, &granularity) == ASE_OK &&
            (preferred != preferredSize_ || minSize != minSize_ || maxSize != maxSize_)) {
            frames = preferred;  // changed in the driver's own settings
        }
        if (frames > 0) config.bufferFrames = static_cast<uint32_t>(frames);
        double rate = requestedRate_.exchange(0.0);
        ASIOSampleRate current = 0.0;
        if (rate <= 0.0 && driver_->getSampleRate(&current) == ASE_OK && current > 0.0 &&
            std::abs(current - sampleRate_) >= 0.5) {
            rate = current;
        }
        if (rate > 0.0) config.sampleRate = static_cast<uint32_t>(std::llround(rate));
    }

    // --- Driver callbacks (any thread) ----------------------------------------------

    void process(long index, const ASIOTime* time) noexcept {
        if (!running_.load(std::memory_order_acquire) || (index != 0 && index != 1)) return;
        const int64_t hostTime =
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
        if (time && (time->timeInfo.flags & kSamplePositionValid)) sampleTime_ = toInt64(time->timeInfo.samplePosition);
        const auto frames = static_cast<int>(bufferFrames_);
        const size_t numInputs = inputs_.size();
        for (size_t c = 0; c < numInputs; ++c) {
            asio::fromDevice(inputTypes_[c], buffers_[c].buffers[index], inputBuffers_[c], frames);
        }
        AudioIO io;
        io.inputs = inputPointers_.data();
        io.numInputs = static_cast<uint32_t>(numInputs);
        io.outputs = outputPointers_.data();
        io.numOutputs = static_cast<uint32_t>(outputs_.size());
        io.frames = static_cast<uint32_t>(frames);
        io.sampleTime = sampleTime_;
        io.hostTimeNs = hostTime;
        callback_->audioCallback(io);
        for (size_t c = 0; c < outputs_.size(); ++c) {
            asio::toDevice(outputTypes_[c], outputPointers_[c], buffers_[numInputs + c].buffers[index], frames);
        }
        if (postOutput_) driver_->outputReady();  // lets the driver play the buffer as soon as it's filled
        sampleTime_ += frames;
    }

    void rateChanged(double rate) noexcept {
        if (rate <= 0.0 || std::abs(rate - sampleRate_) < 0.5) return;  // the rate we set
        requestedRate_.store(rate);
        notify(DeviceEvent::ResetRequest);
    }

    long request(long selector, long value) noexcept {
        switch (selector) {
            case kAsioBufferSizeChange:
                if (value > 0) requestedBufferFrames_.store(value);
                notify(DeviceEvent::ResetRequest);
                return 1;
            case kAsioResetRequest: notify(DeviceEvent::ResetRequest); return 1;
            case kAsioLatenciesChanged: notify(DeviceEvent::LatencyChanged); return 1;
            default: return 0;
        }
    }

private:
    void notify(DeviceEvent event) noexcept {
        if (callback_) callback_->deviceEvent(event);
    }

    std::string failure(const std::string& what, ASIOError error) const {
        return name_ + " " + what + " (" + errorText(error) + ")";
    }

    void check(ASIOError error, const char* what) const {
        if (error != ASE_OK) throw std::runtime_error(failure(what, error));
    }

    std::string channelName(long channel, bool input, long& type) {
        ASIOChannelInfo info{};
        info.channel = channel;
        info.isInput = input ? ASIOTrue : ASIOFalse;
        std::string name;
        if (driver_->getChannelInfo(&info) == ASE_OK) {
            type = info.type;
            name = fromAnsi(info.name, sizeof(info.name));
        } else {
            type = -1;
        }
        return name.empty() ? (input ? "Input " : "Output ") + std::to_string(channel + 1) : name;
    }

    void checkChannels(const std::vector<int>& channels, long available, const char* kind) const {
        for (size_t i = 0; i < channels.size(); ++i) {
            if (channels[i] < 0 || channels[i] >= available) {
                throw std::runtime_error(name_ + " has no " + kind + " " + std::to_string(channels[i] + 1));
            }
            if (std::find(channels.begin(), channels.begin() + static_cast<ptrdiff_t>(i), channels[i]) !=
                channels.begin() + static_cast<ptrdiff_t>(i)) {
                throw std::runtime_error(std::string("The same ") + kind + " is listed twice");
            }
        }
    }

    std::vector<long> channelTypes(const std::vector<int>& channels, const std::vector<long>& types) const {
        std::vector<long> result;
        for (const int channel : channels) {
            const long type = types[static_cast<size_t>(channel)];
            if (asio::sampleFormat(type).bytes == 0) {
                throw std::runtime_error(name_ + " uses a sample format SUBstation can't play (type " +
                                         std::to_string(type) + ")");
            }
            result.push_back(type);
        }
        return result;
    }

    // Chooses the channels, sample rate and buffer size and creates the buffers.
    void setUp(const DeviceConfig& config) {
        long numInputs = 0, numOutputs = 0;
        check(driver_->getChannels(&numInputs, &numOutputs), "could not tell its channels");
        capabilities_ = {};
        capabilities_.hasControlPanel = true;
        std::vector<long> inputTypes(static_cast<size_t>(std::max(0L, numInputs)));
        std::vector<long> outputTypes(static_cast<size_t>(std::max(0L, numOutputs)));
        for (long c = 0; c < numInputs; ++c) {
            capabilities_.inputNames.push_back(channelName(c, true, inputTypes[static_cast<size_t>(c)]));
        }
        for (long c = 0; c < numOutputs; ++c) {
            capabilities_.outputNames.push_back(channelName(c, false, outputTypes[static_cast<size_t>(c)]));
        }

        inputs_ = config.inputChannels;
        outputs_ = config.outputChannels;
        if (outputs_.empty()) {
            for (long c = 0; c < std::min(2L, numOutputs); ++c) outputs_.push_back(static_cast<int>(c));
        }
        checkChannels(inputs_, numInputs, "input");
        checkChannels(outputs_, numOutputs, "output");
        if (inputs_.empty() && outputs_.empty()) throw std::runtime_error(name_ + " has no channels to open");
        inputTypes_ = channelTypes(inputs_, inputTypes);
        outputTypes_ = channelTypes(outputs_, outputTypes);

        // Sample rate: the one asked for, else the one the driver runs at.
        for (const uint32_t rate : kSampleRates) {
            if (driver_->canSampleRate(rate) == ASE_OK) capabilities_.sampleRates.push_back(rate);
        }
        ASIOSampleRate current = 0.0;
        if (driver_->getSampleRate(&current) != ASE_OK) current = 0.0;
        double rate = config.sampleRate > 0 ? config.sampleRate : current;
        if (rate <= 0.0) {  // some drivers have no rate until one is set
            const auto& rates = capabilities_.sampleRates;
            for (const uint32_t candidate : {48000u, 44100u}) {
                if (rate <= 0.0 && std::find(rates.begin(), rates.end(), candidate) != rates.end()) rate = candidate;
            }
            if (rate <= 0.0 && !rates.empty()) rate = rates.front();
        }
        if (rate <= 0.0) throw std::runtime_error(name_ + " has no sample rate");
        if (std::abs(rate - current) >= 0.5) {
            if (driver_->canSampleRate(rate) != ASE_OK) throw std::runtime_error(name_ + " can't run at " + hertz(rate));
            check(driver_->setSampleRate(rate), ("could not switch to " + hertz(rate)).c_str());
            ASIOSampleRate now = 0.0;
            if (driver_->getSampleRate(&now) == ASE_OK && now > 0.0) rate = now;
        }
        sampleRate_ = rate;
        const auto running = static_cast<uint32_t>(std::llround(rate));
        auto& rates = capabilities_.sampleRates;
        if (std::find(rates.begin(), rates.end(), running) == rates.end()) {
            rates.insert(std::upper_bound(rates.begin(), rates.end(), running), running);
        }

        // Buffer size: the one asked for if the driver takes it, else the nearest it offers.
        check(driver_->getBufferSize(&minSize_, &maxSize_, &preferredSize_, &granularity_),
              "could not tell its buffer sizes");
        capabilities_.bufferSizes = asio::bufferSizeOptions(minSize_, maxSize_, preferredSize_, granularity_);
        capabilities_.preferredBufferFrames = static_cast<uint32_t>(std::max(0L, preferredSize_));
        long frames = asio::chooseBufferSize(config.bufferFrames, minSize_, maxSize_, preferredSize_, granularity_);

        callbacks_.bufferSwitch = &onBufferSwitch;
        callbacks_.sampleRateDidChange = &onSampleRateChanged;
        callbacks_.asioMessage = &onMessage;
        callbacks_.bufferSwitchTimeInfo = &onBufferSwitchTimeInfo;
        ASIOError error = createBuffers(frames);
        if (error != ASE_OK && preferredSize_ > 0 && frames != preferredSize_) {
            frames = preferredSize_;
            error = createBuffers(frames);
        }
        check(error, "could not open its channels");
        bufferFrames_ = frames;

        if (driver_->getLatencies(&inputLatency_, &outputLatency_) != ASE_OK) inputLatency_ = outputLatency_ = frames;
        postOutput_ = driver_->outputReady() == ASE_OK;

        // The engine's side: a float buffer per channel. The driver starts from silence.
        const size_t length = static_cast<size_t>(frames);
        scratch_.assign((inputs_.size() + outputs_.size()) * length, 0.f);
        inputBuffers_.clear();
        inputPointers_.clear();
        outputPointers_.clear();
        for (size_t c = 0; c < inputs_.size(); ++c) {
            inputBuffers_.push_back(scratch_.data() + c * length);
            inputPointers_.push_back(inputBuffers_.back());
        }
        for (size_t c = 0; c < outputs_.size(); ++c) {
            outputPointers_.push_back(scratch_.data() + (inputs_.size() + c) * length);
            const ASIOBufferInfo& info = buffers_[inputs_.size() + c];
            const auto bytes = static_cast<size_t>(asio::sampleFormat(outputTypes_[c]).bytes);
            for (void* half : info.buffers) std::memset(half, 0, length * bytes);
        }
        sampleTime_ = 0;
        requestedBufferFrames_.store(0);
        requestedRate_.store(0.0);
    }

    ASIOError createBuffers(long frames) {
        if (buffersCreated_) {
            driver_->disposeBuffers();
            buffersCreated_ = false;
        }
        buffers_.clear();
        for (const int channel : inputs_) buffers_.push_back({ASIOTrue, channel, {nullptr, nullptr}});
        for (const int channel : outputs_) buffers_.push_back({ASIOFalse, channel, {nullptr, nullptr}});
        const ASIOError error =
            driver_->createBuffers(buffers_.data(), static_cast<long>(buffers_.size()), frames, &callbacks_);
        if (error != ASE_OK) return error;
        buffersCreated_ = true;
        for (const ASIOBufferInfo& info : buffers_) {
            if (!info.buffers[0] || !info.buffers[1]) return ASE_NoMemory;
        }
        return ASE_OK;
    }

    // After the control panel: whether the driver now runs differently.
    bool settingsChanged() {
        if (!driver_) return false;
        long minSize = 0, maxSize = 0, preferred = 0, granularity = 0;
        if (driver_->getBufferSize(&minSize, &maxSize, &preferred, &granularity) == ASE_OK &&
            (minSize != minSize_ || maxSize != maxSize_ || preferred != preferredSize_ || granularity != granularity_)) {
            return true;
        }
        ASIOSampleRate rate = 0.0;
        return driver_->getSampleRate(&rate) == ASE_OK && rate > 0.0 && std::abs(rate - sampleRate_) >= 0.5;
    }

    const bool threadCom_;         // COM initialised by the constructor
    IASIO* driver_ = nullptr;
    HMODULE module_ = nullptr;     // the driver's DLL, when loaded without COM
    bool comInitialized_ = false;  // by open()
    bool buffersCreated_ = false;
    bool started_ = false;
    bool postOutput_ = false;
    std::atomic<bool> running_{false};
    std::atomic<bool> inControlPanel_{false};
    AudioCallback* callback_ = nullptr;

    std::string name_;
    double sampleRate_ = 0.0;
    long bufferFrames_ = 0;
    long inputLatency_ = 0;
    long outputLatency_ = 0;
    long minSize_ = 0, maxSize_ = 0, preferredSize_ = 0, granularity_ = 0;  // as the driver said when opened
    DeviceCaps capabilities_;
    std::vector<int> inputs_, outputs_;
    std::vector<long> inputTypes_, outputTypes_;
    std::vector<ASIOBufferInfo> buffers_;  // the open inputs, then the open outputs
    ASIOCallbacks callbacks_{};
    std::vector<float> scratch_;
    std::vector<float*> inputBuffers_;
    std::vector<const float*> inputPointers_;
    std::vector<float*> outputPointers_;
    int64_t sampleTime_ = 0;  // audio thread

    // What the driver asked for, for the next reset.
    std::atomic<long> requestedBufferFrames_{0};
    std::atomic<double> requestedRate_{0.0};
};

void onBufferSwitch(long index, ASIOBool) {
    CallbackScope scope;
    if (AsioBackend* self = g_open.load()) self->process(index, nullptr);
}

ASIOTime* onBufferSwitchTimeInfo(ASIOTime* time, long index, ASIOBool) {
    CallbackScope scope;
    if (AsioBackend* self = g_open.load()) self->process(index, time);
    return nullptr;
}

void onSampleRateChanged(ASIOSampleRate rate) {
    CallbackScope scope;
    if (AsioBackend* self = g_open.load()) self->rateChanged(rate);
}

long onMessage(long selector, long value, void*, double*) {
    CallbackScope scope;
    switch (selector) {
        case kAsioSelectorSupported:
            return value == kAsioEngineVersion || value == kAsioResetRequest || value == kAsioBufferSizeChange ||
                           value == kAsioResyncRequest || value == kAsioLatenciesChanged ||
                           value == kAsioSupportsTimeInfo
                       ? 1
                       : 0;
        case kAsioEngineVersion: return 2;
        case kAsioSupportsTimeInfo: return 1;
        // The driver lost samples. Each callback brings its own timing, so there is nothing to redo.
        case kAsioResyncRequest: return 1;
        case kAsioResetRequest:
        case kAsioBufferSizeChange:
        case kAsioLatenciesChanged:
            if (AsioBackend* self = g_open.load()) return self->request(selector, value);
            return 1;
        default: return 0;
    }
}

}  // namespace

std::unique_ptr<AudioBackend> createAsioBackend() { return std::make_unique<AsioBackend>(); }

}  // namespace sub
