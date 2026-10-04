#pragma once
// What the engine tests share: the sample rate the engine runs at without a
// device, WAV files to play, and where the test plug-ins are.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "Test.h"

namespace subtest {

inline constexpr int kSampleRate = 48000;  // the engine's rate when no device is open
inline constexpr double kSpb = kSampleRate * 60.0 / 120.0;  // samples per beat at 120 BPM

// Writes float samples in [-1, 1) as 16-bit PCM (rounded, clipped), interleaved
// when `channels` > 1. Returns the path as UTF-8.
inline std::string writeWav(const std::filesystem::path& path, const std::vector<float>& interleaved, int channels = 1,
                            int sampleRate = kSampleRate) {
    std::ofstream out(path, std::ios::binary);
    const auto frames = static_cast<uint32_t>(interleaved.size() / static_cast<size_t>(channels));
    const uint32_t dataBytes = frames * static_cast<uint32_t>(channels) * 2;
    auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
    auto u16 = [&](uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
    out.write("RIFF", 4);
    u32(36 + dataBytes);
    out.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(static_cast<uint16_t>(channels));
    u32(static_cast<uint32_t>(sampleRate));
    u32(static_cast<uint32_t>(sampleRate * channels * 2));
    u16(static_cast<uint16_t>(channels * 2));
    u16(16);
    out.write("data", 4);
    u32(dataBytes);
    for (const float s : interleaved) {
        double v = std::round(static_cast<double>(s) * 32768.0);
        v = v < -32768.0 ? -32768.0 : (v > 32767.0 ? 32767.0 : v);
        u16(static_cast<uint16_t>(static_cast<int16_t>(v)));
    }
    const std::u8string text = path.u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

// A fresh WAV in the test's folder.
inline std::string makeWav(const std::vector<float>& interleaved, int channels = 1, int sampleRate = kSampleRate) {
    static int counter = 0;
    return writeWav(tempDir() / ("clip" + std::to_string(counter++) + ".wav"), interleaved, channels, sampleRate);
}

// One second of constant 0.5 in both channels.
inline std::string dcWav() { return makeWav(std::vector<float>(kSampleRate * 2, 0.5f), 2); }

// Mono, sample i = (i % 32768) / 32768 (exact in 16-bit PCM).
inline std::string rampWav() {
    std::vector<float> samples(kSampleRate);
    for (int i = 0; i < kSampleRate; ++i) samples[i] = static_cast<float>(i % 32768) / 32768.f;
    return makeWav(samples, 1);
}

// The test VST3 bundle (tests/vst3_plugins), built with the tests; empty if it wasn't.
inline std::string testPluginsBundle() {
#ifdef SUBSTATION_TEST_PLUGINS_BUNDLE
    return SUBSTATION_TEST_PLUGINS_BUNDLE;
#else
    return {};
#endif
}

}  // namespace subtest
