// The built-in EQ: its bands' filters (as sub::eq::responseDb draws them), placement, output gain and gain scale.

#include <cmath>
#include <complex>
#include <utility>

#include "Engine.h"
#include "builtin/EqDesign.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

enum { BELL, LOW_SHELF, LOW_CUT, HIGH_SHELF, HIGH_CUT, NOTCH, BAND_PASS, TILT_SHELF };

struct EqEngine {
    sub::Engine engine;
    EqEngine() { engine.setClipFadeMs(0); }
};

uint32_t toneTrack(sub::Engine& engine, double freq, float left = 0.25f, float right = 0.25f, double seconds = 1.0) {
    const Samples wave = sine(freq, seconds, 1.0);
    Samples l(wave.size()), r(wave.size());
    for (size_t i = 0; i < wave.size(); ++i) {
        l[i] = left * wave[i];
        r[i] = right * wave[i];
    }
    const std::string path = makeWav(interleave({l, r}), 2);
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, 0.0, seconds, 0.0, 1.f)});
    return track;
}

// A band's parameters (type, freq, gain, q, slope, place, on), in order.
using Band = std::vector<std::pair<std::string, double>>;
using Values = std::vector<std::pair<std::string, float>>;

double bandValue(const Band& band, const std::string& key, double fallback) {
    for (const auto& [k, v] : band)
        if (k == key) return v;
    return fallback;
}

// An EQ with these bands, after `values` (as the Python dict had them: its own values first).
uint32_t eq(sub::Engine& engine, uint32_t track, const std::vector<Band>& bands, Values values = {}) {
    const uint32_t device = engine.addBuiltinProcessor(engine.trackChain(track), "eq", -1);
    for (size_t i = 0; i < bands.size(); ++i) {
        const std::string prefix = "b" + std::to_string(i + 1) + "_";
        values.emplace_back(prefix + "used", 1.f);
        for (const auto& [k, v] : bands[i]) values.emplace_back(prefix + k, static_cast<float>(v));
    }
    for (const auto& [name, value] : values) setParam(engine, device, name, value);
    return device;
}

// The level of the second half of a channel against the tone's, in dB.
double gainDb(const Samples& out, int c = 0, double reference = 0.25) {
    const Samples samples = channel(out, c);
    const double level = rms(slice(samples, static_cast<int64_t>(samples.size() / 2)));
    return 20 * std::log10(std::max(level, 1e-9) / (reference / std::sqrt(2.0)));
}

double response(int kind, double freq, double gain, double q, int slope, double at) {
    return sub::eq::responseDb(sub::eq::design(kind, freq, gain, q, slope, kSampleRate), at, kSampleRate);
}

}  // namespace

TEST_CASE("the EQ is listed with its parameters") {
    const sub::BuiltinInfo& info = builtinInfo("eq");
    CHECK_EQ(info.name, std::string("EQ"));
    const std::vector<std::string> ids = paramIds(info.params);
    REQUIRE(ids.size() == 24 * 8 + 2);
    CHECK(std::vector<std::string>(ids.begin(), ids.begin() + 8) ==
          (std::vector<std::string>{"b1_used", "b1_on", "b1_type", "b1_freq", "b1_gain", "b1_q", "b1_slope", "b1_place"}));
    CHECK(std::vector<std::string>(ids.end() - 2, ids.end()) == (std::vector<std::string>{"output", "scale"}));
    CHECK(std::find(ids.begin(), ids.end(), "b24_place") != ids.end());
    const sub::ParamInfo& used = info.params[0];
    CHECK(used.hidden);
    CHECK(!used.automatable);
}

TEST_CASE("without bands it passes through") {
    EqEngine e;
    auto& engine = e.engine;
    const uint32_t track = toneTrack(engine, 1000.0);
    eq(engine, track, {{{"type", BELL}, {"freq", 1000}, {"gain", 12}, {"on", 0}}});  // a band switched off does nothing
    CHECK_NEAR(gainDb(engine.renderOffline(0.0, kSampleRate)), 0.0, 0.05);
}

TEST_CASE("bands play as their curves show") {
    const std::vector<std::pair<Band, double>> cases{
        {{{"type", BELL}, {"freq", 1000}, {"gain", 12}, {"q", 1}}, 1000},
        {{{"type", BELL}, {"freq", 1000}, {"gain", 12}, {"q", 1}}, 3000},
        {{{"type", BELL}, {"freq", 12000}, {"gain", -9}, {"q", 4}}, 12000},
        {{{"type", LOW_SHELF}, {"freq", 200}, {"gain", 6}, {"q", 0.71}, {"slope", 1}}, 60},
        {{{"type", HIGH_SHELF}, {"freq", 4000}, {"gain", -6}, {"q", 0.71}, {"slope", 3}}, 10000},
        {{{"type", LOW_CUT}, {"freq", 1000}, {"q", 0.71}, {"slope", 3}}, 500},
        {{{"type", HIGH_CUT}, {"freq", 1000}, {"q", 2}, {"slope", 1}}, 1000},
        {{{"type", NOTCH}, {"freq", 2000}, {"q", 4}}, 1500},
        {{{"type", BAND_PASS}, {"freq", 2000}, {"q", 2}}, 1000},
        {{{"type", TILT_SHELF}, {"freq", 1000}, {"gain", 6}, {"slope", 0}}, 10000},
    };
    for (const auto& [band, tone] : cases) {
        INFO("type " + std::to_string(bandValue(band, "type", 0)) + " at " + std::to_string(bandValue(band, "freq", 0)) +
             " Hz, a tone of " + std::to_string(tone) + " Hz");
        EqEngine e;
        auto& engine = e.engine;
        const uint32_t track = toneTrack(engine, tone);
        eq(engine, track, {band});
        const double expected = response(static_cast<int>(bandValue(band, "type", 0)), bandValue(band, "freq", 0),
                                         bandValue(band, "gain", 0.0), bandValue(band, "q", 1.0),
                                         static_cast<int>(bandValue(band, "slope", 1)), tone);
        CHECK_NEAR(gainDb(engine.renderOffline(0.0, kSampleRate)), expected, 0.15);
    }
}

TEST_CASE("the curves match the analog filters") {
    double worst = 0.0;
    const sub::eq::Design bell = sub::eq::design(BELL, 1000.0, 12.0, 1.0, 1, 48000.0);
    for (int i = 0; i < 400; ++i) {
        const double f = 20.0 * std::pow(16000.0 / 20.0, i / 399.0);  // np.geomspace(20, 16000, 400)
        const std::complex<double> s(0.0, f / 1000.0);
        const double k = std::pow(10.0, 12 / 40.0);
        const double analog = 20 * std::log10(std::abs((s * s + s * k + 1.0) / (s * s + s / k + 1.0)));
        worst = std::max(worst, std::fabs(sub::eq::responseDb(bell, f, 48000.0) - analog));
    }
    CHECK(worst < 0.1);
    // Butterworth: 3 dB down at the corner, and 6 dB an octave per order below it (4 octaves here).
    for (const auto& [slope, order] : std::vector<std::pair<int, int>>{{0, 1}, {1, 2}, {3, 4}, {6, 8}}) {
        INFO("order " + std::to_string(order));
        CHECK_NEAR(response(LOW_CUT, 1000.0, 0.0, 0.71, slope, 1000.0), -3.0, 0.1);
        CHECK_NEAR(response(LOW_CUT, 1000.0, 0.0, 0.71, slope, 62.5), -24.1 * order, 0.5 * order);
    }
    CHECK_NEAR(response(BELL, 15000.0, 6.0, 1.0, 1, 15000.0), 6.0, 0.05);  // not squeezed up high
    CHECK(response(NOTCH, 15000.0, 0.0, 4.0, 1, 15000.0) < -40);
}

TEST_CASE("placement") {
    EqEngine e;
    auto& engine = e.engine;
    const uint32_t track = toneTrack(engine, 1000.0, 0.25f, 0.25f);
    const uint32_t device = eq(engine, track, {{{"type", BELL}, {"freq", 1000}, {"gain", -12}, {"place", 1}}});  // left only
    Samples out = engine.renderOffline(0.0, kSampleRate);
    CHECK_NEAR(gainDb(out, 0), -12.0, 0.15);
    CHECK_NEAR(gainDb(out, 1), 0.0, 0.05);

    const int index = engine.processorParamIndex(device, "b1_place");
    engine.setProcessorParam(device, index, 4.f);  // side: the same on both channels has none
    out = engine.renderOffline(0.0, kSampleRate);
    CHECK_NEAR(gainDb(out, 0), 0.0, 0.05);
    CHECK_NEAR(gainDb(out, 1), 0.0, 0.05);
    engine.setProcessorParam(device, index, 3.f);  // mid: all of it
    out = engine.renderOffline(0.0, kSampleRate);
    CHECK_NEAR(gainDb(out, 0), -12.0, 0.15);
    CHECK_NEAR(gainDb(out, 1), -12.0, 0.15);
}

TEST_CASE("side only takes the difference") {
    EqEngine e;
    auto& engine = e.engine;
    const uint32_t track = toneTrack(engine, 1000.0, 0.25f, -0.25f);  // all side
    eq(engine, track, {{{"type", BELL}, {"freq", 1000}, {"gain", -12}, {"place", 3}}});  // mid: none of it
    CHECK_NEAR(gainDb(engine.renderOffline(0.0, kSampleRate), 0), 0.0, 0.05);
}

TEST_CASE("output gain and gain scale") {
    EqEngine e;
    auto& engine = e.engine;
    const uint32_t track = toneTrack(engine, 1000.0);
    const uint32_t device = eq(engine, track, {{{"type", BELL}, {"freq", 1000}, {"gain", 12}}}, {{"output", -6.f}});
    CHECK_NEAR(gainDb(engine.renderOffline(0.0, kSampleRate)), 6.0, 0.15);
    setParam(engine, device, "scale", 50.f);  // half the gain
    CHECK_NEAR(gainDb(engine.renderOffline(0.0, kSampleRate)), 0.0, 0.15);
}

TEST_CASE("the EQ's extremes stay finite") {
    EqEngine e;
    auto& engine = e.engine;
    const uint32_t track = toneTrack(engine, 30.0);
    eq(engine, track,
       {{{"type", BELL}, {"freq", 10}, {"gain", 30}, {"q", 40}},
        {{"type", LOW_CUT}, {"freq", 22000}, {"q", 40}, {"slope", 8}},
        {{"type", HIGH_SHELF}, {"freq", 22000}, {"gain", 30}, {"q", 40}, {"slope", 8}},
        {{"type", NOTCH}, {"freq", 10}, {"q", 0.025}}});
    CHECK(allFinite(engine.renderOffline(0.0, kSampleRate)));
}

TEST_CASE("the EQ's displays are the input and output") {
    EqEngine e;
    auto& engine = e.engine;
    const uint32_t track = toneTrack(engine, 1000.0, 0.5f, 0.25f);
    const uint32_t device = eq(engine, track, {{{"type", BELL}, {"freq", 1000}, {"gain", -6}}});
    std::vector<std::string> ids;
    for (const auto& d : engine.processorDisplays(device)) ids.push_back(d.id);
    CHECK(ids == (std::vector<std::string>{"input", "output"}));
    engine.renderOffline(0.0, 4096);
    std::vector<float> before, after;
    const uint64_t position = engine.readProcessorDisplay(device, 0, 0, before);
    engine.readProcessorDisplay(device, 1, 0, after);
    CHECK_EQ(position, uint64_t{4096});
    CHECK_NEAR(maxAbs(before), 0.375, 0.01);  // in mono
    CHECK_NEAR(maxAbs(slice(after, 2048)), 0.375 * std::pow(10.0, -6 / 20.0), 0.01);
}
