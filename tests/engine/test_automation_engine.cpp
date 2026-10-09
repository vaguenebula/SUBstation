// Automation in the engine: envelopes of mixer controls (tracks and master) and
// of device parameters, rendered offline.

#include <cmath>

#include "Engine.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

struct AutomationEngine {
    sub::Engine engine;
    AutomationEngine() { engine.setClipFadeMs(0); }
};

// Four seconds of constant 0.5 in both channels.
std::string longDcWav() { return makeWav(full(4 * kSampleRate * 2, 0.5f), 2); }

uint32_t dcTrack(sub::Engine& engine, const std::string& path, double seconds = 4.0) {
    return clipTrack(engine, path, 0.0, seconds);
}

using Points = std::vector<sub::AutomationPoint>;

double volumeGain(double value) { return value * value * value * sub::kMaxVolumeGain; }

int utilityGainIndex(sub::Engine& engine, uint32_t pid) { return engine.processorParamIndex(pid, "gain"); }

}  // namespace

TEST_CASE("a volume ramp follows the envelope sample by sample") {
    AutomationEngine e;
    auto& engine = e.engine;
    const uint32_t track = dcTrack(engine, longDcWav());
    engine.setTrackAutomation(track, {{0, "volume", Points{{0.0, 0.f, 0.f}, {2.0, 1.f, 0.f}}}});
    const Samples out = engine.renderOffline(0.0, static_cast<int64_t>(3 * kSpb));
    for (const double beat : {0.0, 0.25, 1.0, 1.5}) {
        INFO("beat " + std::to_string(beat));
        const auto i = static_cast<int64_t>(beat * kSpb);
        CHECK_APPROX_TOL(at(out, i, 0), 0.5 * volumeGain(beat / 2.0), 1e-4, 1e-6);
    }
    CHECK_APPROX_REL(at(out, static_cast<int64_t>(2.5 * kSpb), 1), 0.5 * sub::kMaxVolumeGain, 1e-5);  // holds the last value
}

TEST_CASE("a volume lane replaces the fader until it is removed") {
    AutomationEngine e;
    auto& engine = e.engine;
    const uint32_t track = dcTrack(engine, longDcWav());
    engine.setTrackGain(track, 0.25f);
    engine.setTrackAutomation(track, {{0, "volume", Points{{0.0, 1.f, 0.f}}}});
    Samples out = engine.renderOffline(0.0, 1000);
    CHECK_APPROX_REL(at(out, 500, 0), 0.5 * sub::kMaxVolumeGain, 1e-5);
    engine.setTrackAutomation(track, {});
    out = engine.renderOffline(0.0, 1000);
    CHECK_APPROX(at(out, 500, 0), 0.125);
}

TEST_CASE("mute silences an automated track") {
    AutomationEngine e;
    auto& engine = e.engine;
    const uint32_t track = dcTrack(engine, longDcWav());
    engine.setTrackAutomation(track, {{0, "volume", Points{{0.0, 1.f, 0.f}}}});
    engine.setTrackMute(track, true);
    CHECK(allEqual(engine.renderOffline(0.0, 1000), 0.0));
}

TEST_CASE("pan automation") {
    AutomationEngine e;
    auto& engine = e.engine;
    const uint32_t track = dcTrack(engine, longDcWav());
    // Hard left at beat 0, centre from beat 1, hard right at beat 2: a step, then a ramp.
    engine.setTrackAutomation(
        track, {{0, "pan", Points{{0.0, 0.f, 0.f}, {1.0, 0.f, 0.f}, {1.0, 0.5f, 0.f}, {2.0, 1.f, 0.f}}}});
    const Samples out = engine.renderOffline(0.0, static_cast<int64_t>(3 * kSpb));
    const auto left = static_cast<int64_t>(0.5 * kSpb);
    CHECK_APPROX(at(out, left, 0), 0.5);
    CHECK_NEAR(at(out, left, 1), 0.0, 1e-7);
    const auto centre = static_cast<int64_t>(kSpb);
    CHECK_APPROX(at(out, centre, 0), 0.5);
    CHECK_APPROX(at(out, centre, 1), 0.5);
    const auto right = static_cast<int64_t>(2.5 * kSpb);
    CHECK_NEAR(at(out, right, 0), 0.0, 1e-7);
    CHECK_APPROX(at(out, right, 1), 0.5);
}

TEST_CASE("master automation") {
    AutomationEngine e;
    auto& engine = e.engine;
    dcTrack(engine, longDcWav());
    engine.setMasterGain(0.5f);
    engine.setTrackAutomation(0, {{0, "volume", Points{{0.0, 0.f, 0.f}, {1.0, 0.f, 0.f}, {1.0, 1.f, 0.f}}}});
    const Samples out = engine.renderOffline(0.0, static_cast<int64_t>(2 * kSpb));
    CHECK(allEqual(frames(out, 0, kBeat), 0.0));
    CHECK_APPROX_REL(at(out, kBeat + 10, 0), 0.5 * sub::kMaxVolumeGain, 1e-5);
}

TEST_CASE("master pan") {
    AutomationEngine e;
    auto& engine = e.engine;
    dcTrack(engine, longDcWav());
    engine.setMasterPan(-1.f);
    const Samples out = engine.renderOffline(0.0, 1000);
    CHECK_APPROX(at(out, 500, 0), 0.5);
    CHECK_NEAR(at(out, 500, 1), 0.0, 1e-7);
}

TEST_CASE("a curved segment") {
    // A bent segment follows automationShape: bulging upward when the curve is positive.
    AutomationEngine e;
    auto& engine = e.engine;
    const uint32_t track = dcTrack(engine, longDcWav());
    const float curve = 0.5f;
    engine.setTrackAutomation(track, {{0, "pan", Points{{0.0, 0.5f, curve}, {2.0, 1.f, 0.f}}}});
    const Samples out = engine.renderOffline(0.0, static_cast<int64_t>(2 * kSpb));
    const auto i = static_cast<int64_t>(kSpb);  // halfway
    const double a = -curve * static_cast<double>(sub::kAutomationCurvature);
    const double shaped = 0.5 + 0.5 * std::expm1(a * 0.5) / std::expm1(a);
    CHECK(shaped > 0.75);  // above the straight line
    const double pan = shaped * 2 - 1;
    CHECK_APPROX_REL(at(out, i, 0), 0.5 * std::cos(pan * kPi / 2), 1e-4);
}

TEST_CASE("device parameter automation") {
    AutomationEngine e;
    auto& engine = e.engine;
    const uint32_t track = dcTrack(engine, longDcWav());
    const uint32_t pid = engine.addBuiltinProcessor(engine.trackChain(track), "utility", -1);
    const sub::ParamInfo info = engine.processorParams(pid).at(static_cast<size_t>(utilityGainIndex(engine, pid)));
    const float minus12 = info.toNormalized(-12.f);
    CHECK_NEAR(info.fromNormalized(minus12), -12.0, 1e-4);
    // 0 dB for the first beat, then -12 dB.
    const float zero = info.toNormalized(0.f);
    engine.setTrackAutomation(track, {{pid, "gain", Points{{0.0, zero, 0.f}, {1.0, zero, 0.f}, {1.0, minus12, 0.f}}}});
    const Samples out = engine.renderOffline(0.0, static_cast<int64_t>(2 * kSpb));
    CHECK_APPROX_REL(at(out, static_cast<int64_t>(0.5 * kSpb), 0), 0.5, 1e-4);
    CHECK_APPROX_REL(at(out, static_cast<int64_t>(1.5 * kSpb), 0), 0.5 * std::pow(10.0, -12 / 20.0), 1e-3);  // after the device's smoothing
    // The device holds the automated value, which the UI shows.
    CHECK_NEAR(engine.processorParam(pid, utilityGainIndex(engine, pid)), -12.0, 1e-3);
}

TEST_CASE("automation splits a built-in device's block where values change") {
    // A step in the middle of a block takes effect there, not at the block's start.
    AutomationEngine e;
    auto& engine = e.engine;
    const uint32_t track = dcTrack(engine, longDcWav());
    const uint32_t pid = engine.addBuiltinProcessor(engine.trackChain(track), "utility", -1);
    const sub::ParamInfo info = engine.processorParams(pid).at(static_cast<size_t>(utilityGainIndex(engine, pid)));
    constexpr int64_t step = kBeat + 100;  // well inside a block (blocks are kMaxBlock long from 0)
    static_assert(step % sub::Renderer::kMaxBlock > 200);
    const double stepBeat = static_cast<double>(step) / kSpb;
    engine.setTrackAutomation(track, {{pid, "gain", Points{{0.0, info.toNormalized(-60.f), 0.f},
                                                           {stepBeat, info.toNormalized(-60.f), 0.f},
                                                           {stepBeat, info.toNormalized(0.f), 0.f}}}});
    const Samples out = channel(engine.renderOffline(0.0, step + 2000), 0);
    CHECK(maxAbs(slice(out, step - 200, step)) < 0.001);  // settled at -60 dB, right up to the step
    CHECK(out[step - 1] < out[step + 10]);  // rising from it (smoothed)
    CHECK(out[step + 10] < out[step + 1500]);
}

TEST_CASE("discrete parameters take whole steps") {
    AutomationEngine e;
    auto& engine = e.engine;
    const uint32_t track = engine.addTrack();
    const uint32_t pid = engine.addBuiltinProcessor(engine.trackChain(track), "synth", -1);
    const int wave = engine.processorParamIndex(pid, "wave");
    const sub::ParamInfo info = engine.processorParams(pid).at(static_cast<size_t>(wave));
    CHECK_EQ(info.stepCount(), 3);
    engine.setTrackAutomation(track, {{pid, "wave", Points{{0.0, 0.55f, 0.f}}}});
    engine.renderOffline(0.0, 256);
    CHECK_EQ(engine.processorParam(pid, wave), 2.f);  // Saw
    engine.setTrackAutomation(track, {{pid, "wave", Points{{0.0, 0.f, 0.f}}}});
    engine.renderOffline(0.0, 256);
    CHECK_EQ(engine.processorParam(pid, wave), 0.f);  // Sine
}

TEST_CASE("envelopes of missing devices or parameters are ignored") {
    AutomationEngine e;
    auto& engine = e.engine;
    const uint32_t track = dcTrack(engine, longDcWav());
    const uint32_t pid = engine.addBuiltinProcessor(engine.trackChain(track), "utility", -1);
    engine.setTrackAutomation(track, {{pid, "nonsense", Points{{0.0, 0.f, 0.f}}},
                                      {pid + 100, "gain", Points{{0.0, 0.f, 0.f}}},
                                      {0, "nonsense", Points{{0.0, 0.f, 0.f}}}});
    const Samples out = engine.renderOffline(0.0, 1000);
    CHECK_APPROX(at(out, 500, 0), 0.5);
    engine.removeProcessor(pid);  // its envelope stays, unplayed
    CHECK_APPROX(at(engine.renderOffline(0.0, 1000), 500, 0), 0.5);
}

TEST_CASE("automation follows the tempo") {
    AutomationEngine e;
    auto& engine = e.engine;
    const uint32_t track = dcTrack(engine, longDcWav());
    engine.setTrackAutomation(track, {{0, "volume", Points{{0.0, 0.f, 0.f}, {1.0, 0.f, 0.f}, {1.0, 1.f, 0.f}}}});
    engine.setTempo(60.0);  // beat 1 is now at one second
    const Samples out = engine.renderOffline(0.0, static_cast<int64_t>(1.5 * kSampleRate));
    CHECK_EQ(at(out, kSampleRate - 1, 0), 0.f);
    CHECK(at(out, kSampleRate, 0) > 0.5f);
}

TEST_CASE("the normalized mapping") {
    AutomationEngine e;
    auto& engine = e.engine;
    const uint32_t track = engine.addTrack();
    const uint32_t synth = engine.addBuiltinProcessor(engine.trackChain(track), "synth", -1);
    const sub::ParamInfo cutoff = paramInfo(engine, synth, "cutoff");
    CHECK(cutoff.logScale);
    const double middle = std::sqrt(20.0 * 20000.0);  // log scale: the geometric mean is halfway
    CHECK_NEAR(cutoff.toNormalized(static_cast<float>(middle)), 0.5, 1e-5);
    CHECK_APPROX_REL(cutoff.fromNormalized(0.5f), middle, 1e-4);
    CHECK_APPROX(cutoff.fromNormalized(1.5f), 20000.0);
}
