// The built-in Sidechain device: hits found sample-accurately in the key, the curve played from each,
// depth, lookahead, ducking only the lows, hits on the beat, and its displays.

#include <algorithm>
#include <cmath>
#include <tuple>

#include "Engine.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

constexpr int64_t kHit = 12000;  // where the kick starts, in samples
constexpr double kLevel = 0.5;   // dcWav()'s

struct SidechainEngine {
    sub::Engine engine;
    SidechainEngine() { engine.setClipFadeMs(0); }
};

using Point = std::tuple<double, double, double>;  // x, y, curve
using Values = std::vector<std::pair<std::string, double>>;

uint32_t clipTrackOf(sub::Engine& engine, const std::string& path, double seconds = 1.0) {
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, 0.0, seconds, 0.0, 1.f)});
    return track;
}

// Short bursts of a 60 Hz sine (a kick, as far as the key goes) starting at each of `hits`.
std::string kickWav(const std::vector<int64_t>& hits = {kHit}, double seconds = 1.0, int64_t length = 2400) {
    Samples data(static_cast<size_t>(seconds * kSampleRate) * 2, 0.f);
    std::vector<float> burst(static_cast<size_t>(length));
    for (int64_t n = 0; n < length; ++n)
        burst[static_cast<size_t>(n)] = static_cast<float>(0.8 * std::sin(2 * kPi * 60.0 * static_cast<double>(n) / kSampleRate) *
                                                           std::exp(-static_cast<double>(n) / 600.0));
    burst[0] = 0.8f;  // starts at full level: the hit is its first sample
    for (const int64_t at : hits)
        for (int64_t n = 0; n < length && (at + n) * 2 + 1 < static_cast<int64_t>(data.size()); ++n)
            data[static_cast<size_t>((at + n) * 2)] = data[static_cast<size_t>((at + n) * 2 + 1)] = burst[static_cast<size_t>(n)];
    return makeWav(data, 2);
}

// Parameters for a curve of (x, y, curve) points; the other slots unused.
Values curveValues(const std::vector<Point>& points) {
    Values values;
    for (size_t i = 0; i < 16; ++i) {
        const bool used = i < points.size();
        const auto [x, y, c] = used ? points[i] : Point{1.0, 1.0, 0.0};
        const std::string p = "p" + std::to_string(i + 1) + "_";
        values.insert(values.end(), {{p + "used", used ? 1.0 : 0.0}, {p + "x", x}, {p + "y", y}, {p + "curve", c}});
    }
    return values;
}

const std::vector<Point> kLinear{{0.0, 0.0, 0.0}, {1.0, 1.0, 0.0}};  // ducked at the hit, straight back up over the length

// Where the hits came (since the render started), from the phase display: its 0s.
std::vector<int64_t> hits(sub::Engine& engine, uint32_t device) {
    std::vector<float> phase;
    const auto position = static_cast<int64_t>(engine.readProcessorDisplay(device, 2, 0, phase));
    std::vector<int64_t> found;
    for (size_t i = 0; i < phase.size(); ++i)
        if (phase[i] == 0.f) found.push_back(static_cast<int64_t>(i) + position - static_cast<int64_t>(phase.size()));
    return found;
}

// A Sidechain device (no smoothing, 100 ms, the curve's points, then `values`,
// as a Python dict merges them), keyed by `key` before its fader (heard only so).
uint32_t sidechain(sub::Engine& engine, uint32_t track, std::optional<uint32_t> key = std::nullopt,
                   const std::vector<Point>& points = kLinear, const Values& values = {}) {
    const uint32_t device = engine.addBuiltinProcessor(engine.trackChain(track), "sidechain", -1);
    Values all{{"smooth", 0.0}, {"length", 100.0}};
    for (const auto& v : curveValues(points)) all.push_back(v);
    for (const auto& [name, value] : values) {
        auto it = std::find_if(all.begin(), all.end(), [&](const auto& v) { return v.first == name; });
        if (it != all.end()) it->second = value;
        else all.emplace_back(name, value);
    }
    for (const auto& [name, value] : all) setParam(engine, device, name, static_cast<float>(value));
    if (key) {
        engine.setTrackGain(*key, 0.f);  // heard only through the sidechain
        engine.setProcessorSidechain(device, *key, sub::SidechainTap::PreFader);
    }
    return device;
}

}  // namespace

TEST_CASE("the sidechain device is listed with its parameters") {
    const sub::BuiltinInfo info = builtinInfo("sidechain");
    CHECK_EQ(info.name, std::string("Sidechain"));
    const std::vector<std::string> ids = paramIds(info.params);
    REQUIRE(ids.size() == 12 + 16 * 4);
    CHECK(std::vector<std::string>(ids.begin(), ids.begin() + 12) ==
          (std::vector<std::string>{"trigger", "threshold", "depth", "sync", "length", "rate", "smooth", "lookahead",
                                    "range", "crossover", "autofit", "character"}));
    CHECK_EQ(ids.back(), std::string("p16_curve"));
    std::map<std::string, float> defaults;
    for (const auto& p : info.params) {
        defaults[p.id] = p.defaultValue;
        if (p.id[0] == 'p') {
            INFO(p.id);
            CHECK(p.hidden);
            CHECK(!p.automatable);
        }
    }
    CHECK(std::vector<float>({defaults["p1_used"], defaults["p2_used"], defaults["p3_used"], defaults["p4_used"]}) ==
          std::vector<float>({1.f, 1.f, 1.f, 0.f}));  // the default curve: 3 points
}

TEST_CASE("without a sidechain nothing ducks") {
    SidechainEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrackOf(engine, dcWav());
    sidechain(engine, track);
    const Samples out = engine.renderOffline(0.0, kSampleRate / 2);
    CHECK(allclose(channel(out, 0), kLevel));
}

TEST_CASE("the curve starts at the hit's first sample") {
    SidechainEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrackOf(engine, dcWav());
    const uint32_t key = clipTrackOf(engine, kickWav());
    sidechain(engine, track, key);
    const Samples out = channel(engine.renderOffline(0.0, kSampleRate / 2), 0);
    const auto length = static_cast<int64_t>(0.1 * kSampleRate);
    CHECK_APPROX(out[kHit - 1], kLevel);
    CHECK_NEAR(out[kHit], 0.0, 1e-6);
    // Straight back up over its length, sample by sample, then untouched.
    std::vector<double> expected(static_cast<size_t>(length));
    for (int64_t i = 0; i < length; ++i) expected[static_cast<size_t>(i)] = kLevel * static_cast<double>(i) / static_cast<double>(length);
    CHECK(allclose(slice(out, kHit, kHit + length), expected, 1e-5, 2e-5));
    CHECK(allclose(slice(out, kHit + length + 1), kLevel));
}

TEST_CASE("curve shape, depth and smoothing") {
    SidechainEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrackOf(engine, dcWav());
    const uint32_t key = clipTrackOf(engine, kickWav());
    const std::vector<Point> hold{{0.0, 0.2, 0.0}, {0.5, 0.2, 0.0}, {0.5001, 1.0, 0.0}, {1.0, 1.0, 0.0}};
    sidechain(engine, track, key, hold, {{"depth", 50.0}});
    const Samples out = channel(engine.renderOffline(0.0, kSampleRate / 2), 0);
    const double gain = 1 - 0.5 * (1 - 0.2);  // depth 50 %: halfway to the curve
    CHECK_APPROX_REL(out[kHit + 100], kLevel * gain, 1e-5);
    CHECK_APPROX(out[kHit + 2600], kLevel);  // past half its length: back up at once
}

TEST_CASE("smoothing softens the jump") {
    SidechainEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrackOf(engine, dcWav());
    const uint32_t key = clipTrackOf(engine, kickWav());
    sidechain(engine, track, key, kLinear, {{"smooth", 5.0}});
    const Samples out = channel(engine.renderOffline(0.0, kSampleRate / 2), 0);
    CHECK(0.3 < out[kHit + 10] / kLevel);  // on its way down,
    CHECK(out[kHit + 10] / kLevel < 1.0);  // not there yet
    CHECK(out[kHit + 48 * 30] < 0.5 * kLevel);  // (and still well ducked a few ms on)
}

TEST_CASE("it bends as automation does") {
    SidechainEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrackOf(engine, dcWav());
    const uint32_t key = clipTrackOf(engine, kickWav());
    sidechain(engine, track, key, {{0.0, 0.0, 0.5}, {1.0, 1.0, 0.0}});
    const Samples out = channel(engine.renderOffline(0.0, kSampleRate / 2), 0);
    const double a = -0.5 * 6.0;
    const double middle = std::expm1(a * 0.5) / std::expm1(a);  // bent up: past halfway at half the length
    CHECK_NEAR(out[kHit + 2400] / kLevel, middle, 1e-3);
    CHECK(middle > 0.6);
}

TEST_CASE("threshold and rearming") {
    SidechainEngine e;
    auto& engine = e.engine;
    const std::string dc = dcWav();
    const uint32_t track = clipTrackOf(engine, dc);
    const std::vector<int64_t> times{kHit, kHit + 4800, kHit + 4800 + 600};  // the third comes 12.5 ms after the second: too soon
    const uint32_t key = clipTrackOf(engine, kickWav(times, 1.0, 480));
    const uint32_t device = sidechain(engine, track, key, kLinear, {{"length", 20.0}});
    engine.renderOffline(0.0, 20000);
    CHECK(hits(engine, device) == (std::vector<int64_t>{kHit, kHit + 4800}));

    // Above the key's level: no hits at all.
    const uint32_t track2 = clipTrackOf(engine, dc);
    sidechain(engine, track2, key, kLinear, {{"threshold", -1.0}});
    engine.removeTrack(track);
    const Samples out = engine.renderOffline(0.0, kSampleRate / 2);
    CHECK(allclose(channel(out, 0), kLevel));
}

TEST_CASE("lookahead ducks before the kick") {
    SidechainEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrackOf(engine, dcWav());
    const uint32_t key = clipTrackOf(engine, kickWav());
    const uint32_t device = sidechain(engine, track, key, kLinear, {{"lookahead", 5.0}});
    CHECK_EQ(engine.processorInfo(device).latency, 240);
    const Samples out = channel(engine.renderOffline(0.0, kSampleRate / 2), 0);
    // The mix is delayed to match: the duck starts 5 ms before the kick.
    CHECK_APPROX(out[kHit - 241], kLevel);
    CHECK_NEAR(out[kHit - 240], 0.0, 1e-6);
}

TEST_CASE("lows only keeps the highs") {
    SidechainEngine e;
    auto& engine = e.engine;
    const Samples low = sine(50, 1.0, 0.3), high = sine(5000, 1.0, 0.3);
    Samples both(low.size());
    for (size_t i = 0; i < both.size(); ++i) both[i] = low[i] + high[i];
    const uint32_t track = clipTrackOf(engine, makeWav(stereo(both), 2));
    const uint32_t key = clipTrackOf(engine, kickWav());
    const std::vector<Point> hold{{0.0, 0.0, 0.0}, {0.99, 0.0, 0.0}, {1.0, 1.0, 0.0}};  // ducked all the way through
    sidechain(engine, track, key, hold, {{"length", 500.0}, {"range", 1.0}, {"crossover", 300.0}});
    const Samples out = channel(engine.renderOffline(0.0, kSampleRate), 0);

    const auto level = [](const Samples& segment, double freq) {
        const std::vector<double> window = hanning(segment.size());
        double sum = 0.0;
        for (const double w : window) sum += w;
        const std::vector<double> s = spectrum(segment, window);
        return s[static_cast<size_t>(std::llround(freq * static_cast<double>(segment.size()) / kSampleRate))] / sum * 2;
    };

    const Samples ducked = slice(out, kHit + 4800, kHit + 4800 + 9600);
    CHECK(level(ducked, 50) < 0.3 * 0.02);  // the lows gone
    CHECK_APPROX_REL(level(ducked, 5000), 0.3, 0.05);  // the highs kept
    const Samples before = slice(out, kHit - 9600, kHit);  // while the curve is at 1: both as they were (the bands add up flat)
    CHECK_APPROX_REL(level(before, 50), 0.3, 0.02);
    CHECK_APPROX_REL(level(before, 5000), 0.3, 0.02);
}

TEST_CASE("hits on the beat") {
    SidechainEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrackOf(engine, makeWav(full(2 * kSampleRate * 2, static_cast<float>(kLevel)), 2), 2.0);
    engine.setTempo(120.0);
    const uint32_t device = sidechain(engine, track, std::nullopt, kLinear, {{"trigger", 3.0}, {"length", 50.0}});  // every 1/4: every 24000 samples
    const Samples out = channel(engine.renderOffline(0.0, 80000), 0);
    for (int beat = 0; beat < 4; ++beat) {
        INFO("beat " + std::to_string(beat));
        const int64_t at = beat * kSampleRate / 2;
        CHECK_NEAR(out[at], 0.0, 1e-6);
        if (at) CHECK_APPROX(out[at - 1], kLevel);
    }
    CHECK(hits(engine, device) == std::vector<int64_t>{72000});  // (the display keeps the latest 8192 samples)
}

TEST_CASE("a synced length") {
    SidechainEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrackOf(engine, dcWav());
    const uint32_t key = clipTrackOf(engine, kickWav());
    engine.setTempo(120.0);
    sidechain(engine, track, key, kLinear, {{"sync", 1.0}, {"rate", 2.0}});  // 1/8 at 120: 250 ms
    const Samples out = channel(engine.renderOffline(0.0, kSampleRate / 2), 0);
    CHECK_NEAR(out[kHit + 6000] / kLevel, 0.5, 1e-3);
}

TEST_CASE("the sidechain device's displays") {
    SidechainEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrackOf(engine, dcWav());
    const uint32_t key = clipTrackOf(engine, kickWav());
    const uint32_t device = sidechain(engine, track, key);
    std::vector<std::pair<std::string, int>> displays;
    for (const auto& d : engine.processorDisplays(device)) displays.emplace_back(d.id, d.samplesPerValue);
    CHECK(displays == (std::vector<std::pair<std::string, int>>{{"key", 1}, {"input", 1}, {"phase", 1}}));
    engine.renderOffline(0.0, 16384);
    std::vector<float> keyValues, inputs, phase;
    engine.readProcessorDisplay(device, 0, 0, keyValues);
    engine.readProcessorDisplay(device, 1, 0, inputs);
    engine.readProcessorDisplay(device, 2, 0, phase);
    CHECK_EQ(keyValues.size(), size_t{8192});  // (the latest)
    CHECK_EQ(inputs.size(), size_t{8192});
    CHECK_EQ(phase.size(), size_t{8192});
    REQUIRE(keyValues.size() == 8192 && phase.size() == 8192);
    const int64_t start = 16384 - 8192;
    CHECK_NEAR(keyValues[kHit - start], 0.8, 1e-3);
    CHECK_EQ(keyValues[kHit - start - 1], 0.f);
    CHECK(allclose(inputs, kLevel));
    CHECK_EQ(phase[kHit - start - 1], -1.f);
    CHECK_EQ(phase[kHit - start], 0.f);
    CHECK_EQ(phase[kHit - start + 10], 10.f);
}

TEST_CASE("the sidechain device's extremes stay finite") {
    SidechainEngine e;
    auto& engine = e.engine;
    Rng rng(1);
    const std::string noise = makeWav(rng.uniformSamples(kSampleRate * 2, -1.0, 1.0), 2);
    const uint32_t track = clipTrackOf(engine, noise);
    const uint32_t key = clipTrackOf(engine, noise);
    sidechain(engine, track, key, kLinear,
              {{"threshold", -60.0}, {"length", 10.0}, {"range", 1.0}, {"crossover", 30.0}, {"lookahead", 20.0}, {"smooth", 30.0}});
    const Samples out = engine.renderOffline(0.0, kSampleRate);
    CHECK(allFinite(out));
    CHECK(maxAbs(out) < 2.0);
}
