// The built-in Delay: synced and free times, offset, link, feedback, ping pong, freeze, the filter and the modes.

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "Engine.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

constexpr int64_t kClick = 100;                         // where the click is in the clip
constexpr int64_t kSixteenth = kSampleRate * 60 / 120 / 4;  // at the engine's 120 BPM

struct DelayEngine {
    sub::Engine engine;
    DelayEngine() { engine.setClipFadeMs(0); }
};

using Values = std::vector<std::pair<std::string, float>>;

uint32_t clickTrack(sub::Engine& engine, float left = 1.f, float right = 1.f, double seconds = 3.0, int64_t at = kClick) {
    return stereoClickTrack(engine, left, right, at, seconds);
}

// A Delay, fully wet without feedback or filter unless `values` say otherwise
// (set in that order, as a Python dict merges them).
uint32_t delay(sub::Engine& engine, uint32_t track, const Values& values = {}) {
    Values all{{"feedback", 0.f}, {"filter", 0.f}, {"mix", 100.f}};
    for (const auto& [name, value] : values) {
        auto it = std::find_if(all.begin(), all.end(), [&](const auto& v) { return v.first == name; });
        if (it != all.end()) it->second = value;
        else all.emplace_back(name, value);
    }
    const uint32_t device = engine.addBuiltinProcessor(engine.trackChain(track), "delay", -1);
    for (const auto& [name, value] : all) setParam(engine, device, name, value);
    return device;
}

// Where the channel's echoes are (the first sample of each).
std::vector<int64_t> echoes(const Samples& channel, double threshold = 0.05) {
    std::vector<int64_t> found;
    const std::vector<int64_t> loud = above(channel, threshold);
    for (size_t i = 0; i < loud.size(); ++i)
        if (i == 0 || loud[i] - loud[i - 1] > 8) found.push_back(loud[i]);
    return found;
}

int64_t firstEcho(const Samples& channel, double threshold = 0.05) {
    const auto found = echoes(channel, threshold);
    REQUIRE(!found.empty());
    return found[0];
}

}  // namespace

TEST_CASE("the delay is listed with its parameters") {
    const sub::BuiltinInfo info = builtinInfo("delay");
    CHECK_EQ(info.name, std::string("Delay"));
    const std::vector<std::string> ids = paramIds(info.params);
    REQUIRE(ids.size() >= 4);
    CHECK(std::vector<std::string>(ids.begin(), ids.begin() + 4) ==
          (std::vector<std::string>{"l_sync", "l_division", "l_time", "l_offset"}));
    const std::set<std::string> all(ids.begin(), ids.end());
    for (const char* id : {"link", "feedback", "freeze", "filter", "freq", "width", "mode", "ping_pong", "mix"}) {
        INFO(id);
        CHECK(all.count(id) == 1);
    }
}

TEST_CASE("synced times follow the tempo") {
    DelayEngine e;
    auto& engine = e.engine;
    const uint32_t track = clickTrack(engine);
    delay(engine, track, {{"l_division", 2.f}, {"r_division", 3.f}});  // 3 and 4 sixteenths
    const Samples out = engine.renderOffline(0.0, kSampleRate);
    // Fully wet: the click itself is gone, its echo comes 3 (left) and 4 (right) sixteenths later.
    CHECK_NEAR(firstEcho(channel(out, 0)), kClick + 3 * kSixteenth, 2);
    CHECK_NEAR(firstEcho(channel(out, 1)), kClick + 4 * kSixteenth, 2);
    CHECK_NEAR(maxAbs(channel(out, 0)), 1.0, 0.05);
}

TEST_CASE("offset, free time and link") {
    DelayEngine e;
    auto& engine = e.engine;
    const uint32_t track = clickTrack(engine);
    const uint32_t device =
        delay(engine, track, {{"l_division", 3.f}, {"l_offset", 25.f}, {"r_sync", 0.f}, {"r_time", 100.f}});
    Samples out = engine.renderOffline(0.0, kSampleRate);
    CHECK_NEAR(firstEcho(channel(out, 0)), kClick + 1.25 * 4 * kSixteenth, 2);
    CHECK_NEAR(firstEcho(channel(out, 1)), kClick + kSampleRate / 10, 2);

    setParam(engine, device, "link", 1.f);
    setParam(engine, device, "mode", 2.f);  // Jump: no glide
    out = engine.renderOffline(0.0, kSampleRate);
    CHECK_EQ(firstEcho(channel(out, 1)), firstEcho(channel(out, 0)));  // the right side follows the left
}

TEST_CASE("feedback repeats") {
    DelayEngine e;
    auto& engine = e.engine;
    const uint32_t track = clickTrack(engine);
    delay(engine, track, {{"l_sync", 0.f}, {"l_time", 100.f}, {"r_sync", 0.f}, {"r_time", 100.f}, {"feedback", 50.f}});
    const Samples left = channel(engine.renderOffline(0.0, kSampleRate), 0);
    const int64_t step = kSampleRate / 10;
    for (int n = 1; n < 4; ++n) {
        INFO("repeat " + std::to_string(n));
        const int64_t at = kClick + n * step;
        CHECK_APPROX_REL(maxOf(slice(left, at - 2, at + 3)), std::pow(0.5, n - 1), 0.02);
    }
}

TEST_CASE("ping pong bounces") {
    DelayEngine e;
    auto& engine = e.engine;
    const uint32_t track = clickTrack(engine, 1.f, 0.f);
    delay(engine, track,
          {{"l_sync", 0.f}, {"l_time", 100.f}, {"r_sync", 0.f}, {"r_time", 50.f}, {"feedback", 50.f}, {"ping_pong", 1.f}});
    const Samples out = engine.renderOffline(0.0, kSampleRate / 2);
    const auto left = echoes(channel(out, 0), 0.01), right = echoes(channel(out, 1), 0.01);
    const int64_t stepL = kSampleRate / 10, stepR = kSampleRate / 20;
    REQUIRE(left.size() >= 2);
    REQUIRE(!right.empty());
    // The input, summed to mono, goes left first, then bounces right, then left again.
    CHECK_NEAR(left[0], kClick + stepL, 2);
    CHECK_NEAR(right[0], kClick + stepL + stepR, 2);
    CHECK_NEAR(left[1], kClick + 2 * stepL + stepR, 2);
    CHECK_APPROX_REL(maxAbs(channel(out, 0)), 0.5, 0.02);  // half of the click, on each side
}

TEST_CASE("freeze holds the loop and ignores new input") {
    DelayEngine e;
    auto& engine = e.engine;
    const uint32_t track = clickTrack(engine);
    const uint32_t device = delay(engine, track, {{"l_sync", 0.f}, {"l_time", 100.f}, {"r_sync", 0.f}, {"r_time", 100.f}});
    setParam(engine, device, "freeze", 1.f);
    const Samples out = engine.renderOffline(0.0, kSampleRate);
    CHECK(maxAbs(out) <= 0.01);  // frozen before the click came: it never went in

    setParam(engine, device, "freeze", 0.f);
    setParam(engine, device, "feedback", 10.f);
    const Samples first = engine.renderOffline(0.0, kSampleRate / 4);  // the click goes in, and its first echo comes
    CHECK_NEAR(maxAbs(channel(first, 0)), 1.0, 0.05);
}

TEST_CASE("the filter takes out what is outside its band") {
    DelayEngine e;
    auto& engine = e.engine;
    const Samples low = sine(60.0, 2.0, 0.5);
    const std::string path = makeWav(stereo(low), 2);
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, 0.0, 2.0, 0.0, 1.f)});
    const uint32_t device = delay(engine, track, {{"l_sync", 0.f}, {"l_time", 10.f}, {"r_sync", 0.f}, {"r_time", 10.f}});
    const double unfiltered = maxAbs(channel(frames(engine.renderOffline(0.0, kSampleRate), kSampleRate / 2), 0));
    for (const auto& [name, value] : Values{{"filter", 1.f}, {"freq", 4000.f}, {"width", 2.f}})  // 2 to 8 kHz
        setParam(engine, device, name, value);
    const double filtered = maxAbs(channel(frames(engine.renderOffline(0.0, kSampleRate), kSampleRate / 2), 0));
    CHECK_APPROX_REL(unfiltered, 0.5, 0.02);
    CHECK(filtered < 0.01);
}

TEST_CASE("time changes glide only in Repitch") {
    // The time jumps from 10 to 200 ms at beat 0.5; a click comes 0.375 s later. Fade (done fading) and Jump
    // echo it at 200 ms; Repitch is still gliding there.
    for (const auto& [mode, exact] : std::vector<std::pair<float, bool>>{{0.f, false}, {1.f, true}, {2.f, true}}) {
        INFO("mode " + std::to_string(mode));  // Repitch, Fade, Jump
        DelayEngine e;
        auto& engine = e.engine;
        const int64_t click = 30000;
        const uint32_t track = clickTrack(engine, 1.f, 1.f, 2.0, click);
        const uint32_t device =
            delay(engine, track, {{"l_sync", 0.f}, {"l_time", 10.f}, {"r_sync", 0.f}, {"r_time", 10.f}, {"mode", mode}});
        const sub::ParamInfo info = paramInfo(engine, device, "l_time");
        const float before = info.toNormalized(10.f), after = info.toNormalized(200.f);
        engine.setTrackAutomation(track, {{device, "l_time", {{0.0, before, 0.f}, {0.5, before, 0.f}, {0.5, after, 0.f}}}});
        const Samples out = engine.renderOffline(0.0, kSampleRate);
        const int64_t at = firstEcho(channel(out, 0));
        CHECK_EQ(std::abs(at - (click + kSampleRate / 5)) <= 2, exact);
    }
}

TEST_CASE("mono and silence stay finite") {
    DelayEngine e;
    auto& engine = e.engine;
    const uint32_t track = clickTrack(engine);
    delay(engine, track, {{"feedback", 95.f}, {"filter", 1.f}, {"width", 0.5f}});
    const Samples out = engine.renderOffline(0.0, 2 * kSampleRate);
    CHECK(allFinite(out));
    CHECK(maxAbs(out) < 2.0);
}

TEST_CASE("the input display is the input in mono") {
    DelayEngine e;
    auto& engine = e.engine;
    const uint32_t track = clickTrack(engine, 1.f, 0.5f);
    const uint32_t device = delay(engine, track);
    const auto displays = engine.processorDisplays(device);
    REQUIRE(displays.size() == 1);
    CHECK_EQ(displays[0].id, std::string("input"));
    CHECK_EQ(displays[0].samplesPerValue, 1);
    engine.renderOffline(0.0, 4096);
    std::vector<float> values;
    const uint64_t position = engine.readProcessorDisplay(device, 0, 0, values);
    CHECK_EQ(position, uint64_t{4096});
    REQUIRE(values.size() > static_cast<size_t>(kClick));
    CHECK_NEAR(values[kClick], 0.75, 1e-3);
    CHECK_EQ(countNonzero(values), size_t{1});
}
