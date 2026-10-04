#pragma once
// The fake ASIO driver built with the tests (tests/asio_driver) where the
// engine has ASIO (Windows, with Steinberg's SDK): what the Python tests'
// Driver class did through ctypes. The engine finds it through
// SUBSTATION_ASIO_DRIVERS (set here), never the drivers installed on the
// computer; the test drives it through the functions its DLL exports.
//
// In manual mode each process(n) is n buffer switches, made on the test's
// thread, so what the engine plays can be checked sample by sample; output()
// reads back the bytes the engine wrote, which decode() turns into samples.
//
// Elsewhere the tests that need it skip: an AsioDriver can't be made (its
// constructor skips the test), and its functions are never called.

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "Engine.h"
#include "Test.h"

#if defined(_WIN32) && defined(SUBSTATION_TEST_ASIO_DRIVER) && defined(SUBSTATION_HAS_ASIO)
#define SUBTEST_HAVE_ASIO 1
#else
#define SUBTEST_HAVE_ASIO 0
#endif

namespace subtest {

inline constexpr const char* kTestAsioName = "SUB Test ASIO";

// asioMessage selectors (asio.h).
enum AsioSelector : long {
    kSelectorSupported = 1,
    kEngineVersion,
    kResetRequest,
    kBufferSizeChange,
    kResyncRequest,
    kLatenciesChanged,
    kSupportsTimeInfo,
    kOverload = 15,
};

// ASIOSampleType values (asio.h).
enum AsioSampleType : long {
    kInt16Msb = 0,
    kInt24Msb = 1,
    kInt32Msb = 2,
    kFloat32Msb = 3,
    kInt16Lsb = 16,
    kInt24Lsb = 17,
    kInt32Lsb = 18,
    kFloat32Lsb = 19,
    kFloat64Lsb = 20,
    kInt32Lsb16 = 24,
    kInt32Lsb18 = 25,
    kInt32Lsb20 = 26,
    kInt32Lsb24 = 27,
};

// Every sample type the driver can use (the Python FORMATS table's keys, sorted).
inline const std::vector<long>& asioSampleTypes() {
    static const std::vector<long> types{kInt16Msb,  kInt24Msb,   kInt32Msb,   kFloat32Msb, kInt16Lsb,
                                         kInt24Lsb,  kInt32Lsb,   kFloat32Lsb, kFloat64Lsb, kInt32Lsb16,
                                         kInt32Lsb18, kInt32Lsb20, kInt32Lsb24};
    return types;
}

// A sample type's container (bytes, big-endian or not) and significant bits (0: floating point).
struct AsioFormat {
    int bytes = 4;
    bool bigEndian = false;
    int bits = 0;
    bool isFloat = false;
};

inline AsioFormat asioFormat(long type) {
    switch (type) {
        case kInt16Lsb: return {2, false, 16, false};
        case kInt24Lsb: return {3, false, 24, false};
        case kInt32Lsb: return {4, false, 32, false};
        case kFloat32Lsb: return {4, false, 0, true};
        case kFloat64Lsb: return {8, false, 0, true};
        case kInt32Lsb16: return {4, false, 16, false};
        case kInt32Lsb18: return {4, false, 18, false};
        case kInt32Lsb20: return {4, false, 20, false};
        case kInt32Lsb24: return {4, false, 24, false};
        case kInt16Msb: return {2, true, 16, false};
        case kInt24Msb: return {3, true, 24, false};
        case kInt32Msb: return {4, true, 32, false};
        case kFloat32Msb: return {4, true, 0, true};
        default: return {};
    }
}

// A driver buffer's samples as floats (decoded independently of the engine, as numpy did).
inline std::vector<double> decode(const std::vector<uint8_t>& raw, long type) {
    const AsioFormat format = asioFormat(type);
    std::vector<double> out;
    for (size_t at = 0; at + static_cast<size_t>(format.bytes) <= raw.size(); at += static_cast<size_t>(format.bytes)) {
        uint8_t b[8] = {};
        for (int i = 0; i < format.bytes; ++i) b[i] = raw[at + static_cast<size_t>(format.bigEndian ? format.bytes - 1 - i : i)];
        if (format.isFloat && format.bytes == 4) {
            float value;
            std::memcpy(&value, b, 4);
            out.push_back(value);
        } else if (format.isFloat) {
            double value;
            std::memcpy(&value, b, 8);
            out.push_back(value);
        } else {
            int64_t value = 0;
            for (int i = 0; i < format.bytes; ++i) value |= static_cast<int64_t>(b[i]) << (8 * i);
            const int64_t sign = int64_t{1} << (8 * format.bytes - 1);  // the container's sign bit
            if (value >= sign) value -= sign << 1;
            out.push_back(static_cast<double>(value) / static_cast<double>(int64_t{1} << (format.bits - 1)));
        }
    }
    return out;
}

// The engine has ASIO and the fake driver was built.
inline bool haveTestAsio() { return SUBTEST_HAVE_ASIO != 0; }

// Skips the test unless it can use the fake driver.
inline void requireTestAsio() {
    if (!haveTestAsio()) SKIP("built without the ASIO SDK");
}

// Opening the fake driver, with these settings (0 / none: the driver's own).
inline sub::DeviceConfig asioConfig(uint32_t sampleRate = 0, uint32_t bufferFrames = 0, std::vector<int> inputs = {},
                                    std::vector<int> outputs = {}) {
    sub::DeviceConfig config;
    config.driver = "ASIO";
    config.name = kTestAsioName;
    config.sampleRate = sampleRate;
    config.bufferFrames = bufferFrames;
    config.inputChannels = std::move(inputs);
    config.outputChannels = std::move(outputs);
    return config;
}

inline void openAsio(sub::Engine& engine, uint32_t sampleRate = 0, uint32_t bufferFrames = 0, std::vector<int> inputs = {},
                     std::vector<int> outputs = {}) {
    engine.openDevice(asioConfig(sampleRate, bufferFrames, std::move(inputs), std::move(outputs)));
}

// The test driver's hooks (exported from its DLL; see test_asio_driver.cpp).
class AsioDriver {
public:
    // Skips the test where there is no driver; resets its settings for the next instance.
    AsioDriver() {
        requireTestAsio();
#if SUBTEST_HAVE_ASIO
        const std::wstring path = std::filesystem::path(SUBSTATION_TEST_ASIO_DRIVER).make_preferred().wstring();
        const std::wstring drivers = L"SUB Test ASIO|{5B2E8C1A-7F3D-4E6B-9C0A-1D2F3E4A5B6C}|" + path;
        SetEnvironmentVariableW(L"SUBSTATION_ASIO_DRIVERS", drivers.c_str());
        module_ = LoadLibraryW(path.c_str());  // the engine loads the same module (and it stays loaded)
        REQUIRE(module_ != nullptr);
        bind(reset_, "Reset");
        bind(setSampleType_, "SetSampleType");
        bind(setBufferSizes_, "SetBufferSizes");
        bind(setLatencies_, "SetLatencies");
        bind(setManual_, "SetManual");
        bind(setInputLevel_, "SetInputLevel");
        bind(setLoopback_, "SetLoopback");
        bind(failInit_, "FailInit");
        bind(setControlPanelChange_, "SetControlPanelChange");
        bind(process_, "Process");
        bind(readOutput_, "ReadOutput");
        bind(clearOutput_, "ClearOutput");
        bind(sendMessage_, "SendMessage");
        bind(changeSampleRate_, "ChangeSampleRate");
        bind(get_, "Get");
        bind(initHandle_, "InitHandle");
        reset_();
#endif
    }

    void setSampleType(long type) { setSampleType_(type); }
    void setBufferSizes(long minSize, long maxSize, long preferred, long granularity) {
        setBufferSizes_(minSize, maxSize, preferred, granularity);
    }
    void setLatencies(long inputExtra, long outputExtra) { setLatencies_(inputExtra, outputExtra); }
    void setManual(bool manual) { setManual_(manual ? 1 : 0); }
    void setInputLevel(int channel, double level) { setInputLevel_(channel, level); }
    void setLoopback(int output, int input) { setLoopback_(output, input); }
    void failInit(const char* message) { failInit_(message); }  // null: it doesn't
    void setControlPanelChange(long bufferSize, double rate) { setControlPanelChange_(bufferSize, rate); }
    int process(int buffers) { return process_(buffers); }
    // The bytes the engine wrote to an output channel (none if it never opened it).
    std::vector<uint8_t> output(int channel) {
        const long size = readOutput_(channel, nullptr, 0);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (size > 0) readOutput_(channel, bytes.data(), size);
        return bytes;
    }
    void clearOutput() { clearOutput_(); }
    long sendMessage(long selector, long value) { return sendMessage_(selector, value); }
    void changeSampleRate(double rate) { changeSampleRate_(rate); }
    double get(const char* key) { return get_(key); }
    uintptr_t initHandle() { return reinterpret_cast<uintptr_t>(initHandle_()); }

private:
#if SUBTEST_HAVE_ASIO
    template <typename F>
    void bind(F& function, const char* name) {
        function = reinterpret_cast<F>(GetProcAddress(module_, ("SubTestAsio_" + std::string(name)).c_str()));
        REQUIRE(function != nullptr);
    }
    HMODULE module_ = nullptr;
#endif
    void (*reset_)() = nullptr;
    void (*setSampleType_)(long) = nullptr;
    void (*setBufferSizes_)(long, long, long, long) = nullptr;
    void (*setLatencies_)(long, long) = nullptr;
    void (*setManual_)(int) = nullptr;
    void (*setInputLevel_)(int, double) = nullptr;
    void (*setLoopback_)(int, int) = nullptr;
    void (*failInit_)(const char*) = nullptr;
    void (*setControlPanelChange_)(long, double) = nullptr;
    int (*process_)(int) = nullptr;
    long (*readOutput_)(int, void*, long) = nullptr;
    void (*clearOutput_)() = nullptr;
    long (*sendMessage_)(long, long) = nullptr;
    void (*changeSampleRate_)(double) = nullptr;
    double (*get_)(const char*) = nullptr;
    void* (*initHandle_)() = nullptr;
};

}  // namespace subtest
