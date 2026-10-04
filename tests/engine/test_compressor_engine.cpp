// The built-in Compressor: its gain curve, its displays, and keying from a sidechain.

#include <cmath>
#include <utility>

#include "Engine.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

struct CompressorEngine {
    sub::Engine engine;
    CompressorEngine() { engine.setClipFadeMs(0); }
};

uint32_t addClipTrack(sub::Engine& engine, const std::string& path) {
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, 0.0, 1.0, 0.0, 1.f)});
    return track;
}

uint32_t compressor(sub::Engine& engine, uint32_t track, const std::vector<std::pair<std::string, float>>& values) {
    const uint32_t device = engine.addBuiltinProcessor(engine.trackChain(track), "compressor", -1);
    for (const auto& [name, value] : values) setParam(engine, device, name, value);
    return device;
}

const double kLevelDb = 20 * std::log10(0.5);  // dcWav()'s

// read_processor_display(device, index, position): (values, next position).
std::pair<std::vector<float>, uint64_t> readDisplay(sub::Engine& engine, uint32_t device, int index,
                                                    uint64_t position = 0) {
    std::vector<float> values;
    const uint64_t next = engine.readProcessorDisplay(device, index, position, values);
    return {values, next};
}

}  // namespace

TEST_CASE("a hard knee's reduction follows the ratio") {
    CompressorEngine e;
    auto& engine = e.engine;
    const uint32_t track = addClipTrack(engine, dcWav());
    compressor(engine, track, {{"threshold", -18.f}, {"ratio", 4.f}, {"knee", 0.f}, {"attack", 1.f}});
    const Samples out = engine.renderOffline(0.0, kSampleRate / 2);
    const double reduction = (kLevelDb + 18.0) * (1 - 1 / 4.0);
    CHECK_APPROX_REL(at(out, -1, 0), 0.5 * std::pow(10.0, -reduction / 20), 1e-3);
}

TEST_CASE("below the knee nothing changes") {
    CompressorEngine e;
    auto& engine = e.engine;
    const uint32_t track = addClipTrack(engine, dcWav());
    compressor(engine, track, {{"threshold", 0.f}, {"knee", 6.f}});  // -6 dB is just below the knee's start (-3 dB)
    const Samples out = engine.renderOffline(0.0, 4800);
    CHECK_APPROX_REL(at(out, -1, 0), 0.5, 1e-5);
}

TEST_CASE("displays report levels and reduction") {
    CompressorEngine e;
    auto& engine = e.engine;
    const uint32_t track = addClipTrack(engine, dcWav());
    const uint32_t device = compressor(engine, track, {{"threshold", -18.f}, {"ratio", 4.f}, {"knee", 0.f}, {"attack", 1.f}});
    std::vector<std::pair<std::string, int>> displays;
    for (const auto& d : engine.processorDisplays(device)) displays.emplace_back(d.id, d.samplesPerValue);
    CHECK(displays == (std::vector<std::pair<std::string, int>>{{"input", 256}, {"reduction", 256}, {"output", 256}}));

    engine.renderOffline(0.0, 256 * 100);
    auto [reduction, position] = readDisplay(engine, device, 1);
    CHECK_EQ(position, uint64_t{100});
    REQUIRE(reduction.size() == 100);  // (float32 values)
    CHECK_NEAR(reduction.back(), (kLevelDb + 18.0) * 0.75, 0.01);
    const auto level = readDisplay(engine, device, 0).first;
    REQUIRE(!level.empty());
    CHECK_NEAR(level.back(), kLevelDb, 0.01);

    // A reader picks up where it left off; one that fell behind gets the latest 8192.
    engine.renderOffline(0.0, 256 * 10);
    const auto [more, next] = readDisplay(engine, device, 1, position);
    CHECK_EQ(more.size(), size_t{10});
    CHECK_EQ(next, uint64_t{110});
    CHECK(readDisplay(engine, device, 1, next).first.empty());
    engine.renderOffline(0.0, 256 * 9000);
    CHECK_EQ(readDisplay(engine, device, 1, next).first.size(), size_t{8192});
}

TEST_CASE("a sidechain keys the compressor") {
    CompressorEngine e;
    auto& engine = e.engine;
    const uint32_t track = addClipTrack(engine, dcWav());
    const uint32_t key = addClipTrack(engine, makeWav(full(kSampleRate * 2, 1.f), 2));
    engine.setTrackGain(key, 0.f);  // heard only through the sidechain, taken before its fader
    const uint32_t device = compressor(engine, track, {{"threshold", -18.f}, {"ratio", 4.f}, {"knee", 0.f}, {"attack", 1.f}});
    engine.setProcessorSidechain(device, key, sub::SidechainTap::PreFader);
    Samples out = engine.renderOffline(0.0, kSampleRate / 2);
    CHECK_APPROX_REL(at(out, -1, 0), 0.5 * std::pow(10.0, -18.0 * 0.75 / 20), 1e-3);  // keyed at 0 dB

    // A silent sidechain keys nothing: the compressor doesn't fall back to its own input.
    engine.setProcessorSidechain(device, engine.addTrack(), sub::SidechainTap::PreFader);
    out = engine.renderOffline(0.0, kSampleRate / 2);
    CHECK_APPROX_REL(at(out, -1, 0), 0.5, 1e-4);

    engine.clearProcessorSidechain(device);  // without one, its own input keys it
    out = engine.renderOffline(0.0, kSampleRate / 2);
    CHECK_APPROX_REL(at(out, -1, 0), 0.5 * std::pow(10.0, -(kLevelDb + 18.0) * 0.75 / 20), 1e-3);
}

TEST_CASE("attack holds across a low note's cycles") {
    // The reduction reaches its level within a few attack times on a low sine too, not only on DC.
    CompressorEngine e;
    auto& engine = e.engine;
    const Samples tone = sine(55.0, 1.0, 0.9);
    const uint32_t track = addClipTrack(engine, makeWav(stereo(tone), 2));
    const uint32_t device = compressor(engine, track, {{"threshold", -24.f}, {"ratio", 4.f}, {"knee", 0.f}, {"attack", 10.f}});
    engine.renderOffline(0.0, kSampleRate / 2);
    const auto reduction = readDisplay(engine, device, 1).first;
    const double steady = (20 * std::log10(0.9) + 24.0) * 0.75;
    REQUIRE(reduction.size() > static_cast<size_t>(50 * kSampleRate / 1000 / 256));
    const float after50ms = reduction[50 * kSampleRate / 1000 / 256];
    CHECK(after50ms > 0.9 * steady);
    CHECK_NEAR(reduction.back(), steady, 0.1);
}
