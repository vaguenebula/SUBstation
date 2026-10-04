#pragma once
// What the engine tests share: the sample rate the engine runs at without a
// device, WAV files to play, where the test plug-ins are, and the calls the
// Python tests' helpers made on the engine.

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "Engine.h"
#include "Test.h"
#include "builtin/BuiltinRegistry.h"
#include "plugins/Vst3Format.h"

namespace subtest {

inline constexpr int kSampleRate = 48000;  // the engine's rate when no device is open
inline constexpr double kSpb = kSampleRate * 60.0 / 120.0;  // samples per beat at 120 BPM
inline constexpr int64_t kBeat = kSampleRate / 2;            // the same, as a whole number of samples

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

inline bool haveTestPlugins() {
    const std::string bundle = testPluginsBundle();
    std::error_code ignored;
    return !bundle.empty() && std::filesystem::exists(bundle, ignored);
}

// Skips the test unless the test plug-ins were built.
inline void requireTestPlugins() {
    if (!haveTestPlugins()) SKIP("test plug-ins not built");
}

// The test plug-ins' class ids by name (scanned once). Skips the test without them.
inline const std::map<std::string, std::string>& testPluginUids() {
    requireTestPlugins();
    static const std::map<std::string, std::string> uids = [] {
        std::map<std::string, std::string> found;
        for (const sub::PluginDescription& d : sub::vst3::Vst3Format::instance().scanFile(testPluginsBundle()))
            found[d.name] = d.uid;
        return found;
    }();
    return uids;
}

// One of the test plug-ins ("SUB Test Effect"...) in a chain.
inline uint32_t addTestPlugin(sub::Engine& engine, uint32_t chain, const std::string& name, int index = -1) {
    return engine.addPluginProcessor(chain, "VST3", testPluginsBundle(), testPluginUids().at(name), index);
}

// --- What the Python tests' helpers did ----------------------------------------------

inline sub::ClipDesc clip(const std::string& path, double startBeat, double durationSec, double offsetSec = 0.0,
                          float gain = 1.f) {
    sub::ClipDesc c;
    c.path = path;
    c.startBeat = startBeat;
    c.durationSec = durationSec;
    c.offsetSec = offsetSec;
    c.gain = gain;
    return c;
}

// A track playing one clip of `path` (loaded first), going into `output` if given.
inline uint32_t clipTrack(sub::Engine& engine, const std::string& path, double startBeat = 0.0,
                          double durationSec = 1.0, std::optional<uint32_t> output = std::nullopt) {
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, startBeat, durationSec)});
    if (output) engine.setTrackOutput(track, *output);
    return track;
}

// Sets a device's parameter by its id.
inline void setParam(sub::Engine& engine, uint32_t processor, const std::string& id, float value) {
    const int index = engine.processorParamIndex(processor, id);
    REQUIRE(index >= 0);
    engine.setProcessorParam(processor, index, value);
}

inline sub::ParamInfo paramInfo(sub::Engine& engine, uint32_t processor, const std::string& id) {
    const int index = engine.processorParamIndex(processor, id);
    REQUIRE(index >= 0);
    return engine.processorParams(processor).at(static_cast<size_t>(index));
}

// A Utility on a chain, at `gainDb`.
inline uint32_t utilityOn(sub::Engine& engine, uint32_t chain, float gainDb = 0.f) {
    const uint32_t processor = engine.addBuiltinProcessor(chain, "utility", -1);
    setParam(engine, processor, "gain", gainDb);
    return processor;
}

// A built-in device type as the registry lists it.
inline const sub::BuiltinInfo& builtinInfo(const std::string& id) {
    for (const sub::BuiltinInfo& info : sub::BuiltinRegistry::instance().devices())
        if (info.id == id) return info;
    throw std::invalid_argument("no built-in device " + id);
}

// The ids of a list of parameters.
inline std::vector<std::string> paramIds(const std::vector<sub::ParamInfo>& params) {
    std::vector<std::string> ids;
    for (const sub::ParamInfo& p : params) ids.push_back(p.id);
    return ids;
}

}  // namespace subtest
