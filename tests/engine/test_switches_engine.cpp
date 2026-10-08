// Switches in the engine: a track's activator and a device's (or a rack's)
// on/off, automated. Off, a track is silent (its pre-fader sends too) whatever
// its mute; a device is passed by, as late as it would make its signal. Each
// switch fades over Renderer::switchFade() samples. Rendered offline.

#include <cmath>

#include "Engine.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

struct SwitchEngine {
    sub::Engine engine;
    SwitchEngine() { engine.setClipFadeMs(0); }
};

// Four seconds of constant 0.5 in both channels.
std::string longDcWav() { return makeWav(full(4 * kSampleRate * 2, 0.5f), 2); }

uint32_t dcTrack(sub::Engine& engine, const std::string& path, double startBeat = 0.0) {
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, startBeat, 4.0, 0.0, 1.f)});
    return track;
}

using Points = std::vector<sub::AutomationPoint>;

// On for the first beat, off from then on; and the other way round.
const Points kOnThenOff{{0.0, 1.f, 0.f}, {1.0, 1.f, 0.f}, {1.0, 0.f, 0.f}};
const Points kOffThenOn{{0.0, 0.f, 0.f}, {1.0, 0.f, 0.f}, {1.0, 1.f, 0.f}};

const int kFade = sub::Renderer::switchFade(kSampleRate);

double dbGain(double db) { return std::pow(10.0, db / 20.0); }

}  // namespace

TEST_CASE("a switch fades over about 5 ms") {
    CHECK_EQ(kFade, 240);
    CHECK_EQ(sub::Renderer::switchFade(1.0), 1);
    CHECK_EQ(sub::Renderer::switchFade(1e9), sub::Renderer::kMaxSwitchFade);
    CHECK(sub::automationSwitchOn(0.5f));
    CHECK(!sub::automationSwitchOn(0.49f));
}

TEST_CASE("a track's switch silences it where it is off, fading") {
    SwitchEngine e;
    auto& engine = e.engine;
    const uint32_t track = dcTrack(engine, longDcWav());
    engine.setTrackAutomation(track, {{0, sub::kTrackOnLane, kOnThenOff}});
    const Samples out = engine.renderOffline(0.0, 2 * kBeat);
    CHECK_EQ(at(out, kBeat / 2, 0), 0.5f);
    CHECK_EQ(at(out, kBeat - 1, 1), 0.5f);
    // Each sample's gain: the share of the fade's samples up to it that were on.
    for (const int i : {0, kFade / 2 - 1, kFade - 2}) {
        INFO("sample " + std::to_string(i) + " after the switch");
        CHECK_APPROX_REL(at(out, kBeat + i, 0), 0.5 * (kFade - 1 - i) / kFade, 1e-6);
    }
    CHECK(allEqual(frames(out, kBeat + kFade - 1), 0.0));
}

TEST_CASE("a switch fades across a loop's wrap") {
    SwitchEngine e;
    auto& engine = e.engine;
    const uint32_t track = dcTrack(engine, longDcWav());
    engine.setTrackAutomation(track, {{0, sub::kTrackOnLane, kOnThenOff}});  // off at the loop's end, on at its start
    engine.setLoop(true, 0.0, 2.0);
    const Samples out = engine.renderOffline(0.0, 3 * kBeat, true);
    // Just after the wrap the window still holds the silence played before it.
    CHECK_APPROX_REL(at(out, 2 * kBeat, 0), 0.5 / kFade, 1e-4);
    CHECK_APPROX_REL(at(out, 2 * kBeat + kFade / 2 - 1, 0), 0.25, 1e-4);
    CHECK_EQ(at(out, 2 * kBeat + kFade, 0), 0.5f);
}

TEST_CASE("a track's switch stands in for its mute while it has one") {
    SwitchEngine e;
    auto& engine = e.engine;
    const uint32_t track = dcTrack(engine, longDcWav());
    engine.setTrackMute(track, true);
    engine.setTrackAutomation(track, {{0, sub::kTrackOnLane, kOffThenOn}});
    Samples out = engine.renderOffline(0.0, 2 * kBeat);
    CHECK(allEqual(frames(out, 0, kBeat), 0.0));
    CHECK_EQ(at(out, kBeat + kFade, 0), 0.5f);  // on, though muted
    engine.setTrackAutomation(track, {});  // its mute counts again
    CHECK(allEqual(engine.renderOffline(0.0, 2 * kBeat), 0.0));
}

TEST_CASE("a track switched off sends nothing before its fader either") {
    SwitchEngine e;
    auto& engine = e.engine;
    const uint32_t track = dcTrack(engine, longDcWav());
    const uint32_t ret = engine.addTrack();
    engine.setTrackGain(track, 0.f);  // heard only through the send
    engine.setTrackSend(track, ret, 1.f, true);
    engine.setTrackAutomation(track, {{0, sub::kTrackOnLane, kOnThenOff}});
    const Samples out = engine.renderOffline(0.0, 2 * kBeat);
    CHECK_EQ(at(out, kBeat / 2, 0), 0.5f);
    CHECK(allEqual(frames(out, kBeat + kFade), 0.0));
}

TEST_CASE("the master has no switch") {
    SwitchEngine e;
    auto& engine = e.engine;
    dcTrack(engine, longDcWav());
    engine.setTrackAutomation(0, {{0, sub::kTrackOnLane, Points{{0.0, 0.f, 0.f}}}});
    CHECK_EQ(at(engine.renderOffline(0.0, 1000), 500, 0), 0.5f);
}

TEST_CASE("a device switched off passes its input on") {
    SwitchEngine e;
    auto& engine = e.engine;
    const uint32_t track = dcTrack(engine, longDcWav());
    const uint32_t utility = utilityOn(engine, engine.trackChain(track), -12.f);
    engine.setTrackAutomation(track, {{utility, sub::kDeviceOnLane, kOffThenOn}});
    const Samples out = engine.renderOffline(0.0, 2 * kBeat);
    CHECK_EQ(at(out, kBeat / 2, 0), 0.5f);
    // Fading in (from its input to what it makes of it: it was reset, and its gain smooths in).
    const Samples fading = channel(frames(out, kBeat, kBeat + kFade), 0);
    CHECK(maxOf(fading) <= 0.5f);
    CHECK(fading.back() < 0.45f);
    CHECK_APPROX_REL(at(out, kBeat + kBeat / 2, 1), 0.5 * dbGain(-12.0), 1e-3);
    // Its own parameters' automation plays as usual where it is on.
    const sub::ParamInfo gain = paramInfo(engine, utility, "gain");
    engine.setTrackAutomation(track, {{utility, sub::kDeviceOnLane, kOffThenOn},
                                      {utility, "gain", Points{{0.0, gain.toNormalized(-6.f), 0.f}}}});
    CHECK_APPROX_REL(at(engine.renderOffline(0.0, 2 * kBeat), kBeat + kBeat / 2, 0), 0.5 * dbGain(-6.0), 1e-3);
}

TEST_CASE("a device switched off by hand stays off whatever its switch's lane says") {
    SwitchEngine e;
    auto& engine = e.engine;
    const uint32_t track = dcTrack(engine, longDcWav());
    const uint32_t utility = utilityOn(engine, engine.trackChain(track), -12.f);
    engine.setTrackAutomation(track, {{utility, sub::kDeviceOnLane, Points{{0.0, 1.f, 0.f}}}});
    engine.setProcessorEnabled(utility, false);  // (the application switches it on while its lane plays)
    CHECK_EQ(at(engine.renderOffline(0.0, 1000), 500, 0), 0.5f);
}

TEST_CASE("a latent device switched off keeps what comes after it in time") {
    SwitchEngine e;
    auto& engine = e.engine;
    // A step at beat 1 through a Sidechain device that ducks nothing but looks 10 ms ahead.
    const uint32_t track = dcTrack(engine, longDcWav(), 1.0);
    const uint32_t device = engine.addBuiltinProcessor(engine.trackChain(track), "sidechain", -1);
    setParam(engine, device, "depth", 0.f);
    setParam(engine, device, "lookahead", 10.f);
    engine.idle();  // its latency changed: the tracks realign
    const int latency = engine.processorInfo(device).latency;
    REQUIRE(latency > kFade);
    const auto stepAt = [&](const Points& points) {
        engine.setTrackAutomation(track, {{device, sub::kDeviceOnLane, points}});
        const std::vector<int64_t> found = nonzero(channel(engine.renderOffline(0.0, 2 * kBeat), 0));
        return found.empty() ? int64_t{-1} : found.front();
    };
    CHECK_EQ(stepAt(Points{{0.0, 1.f, 0.f}}), kBeat);
    CHECK_EQ(stepAt(Points{{0.0, 0.f, 0.f}}), kBeat);
    // Switched off just after the step goes into it (before it comes out): the
    // signal goes on, in time, and the fade is between two of the same.
    const double off = static_cast<double>(kBeat + 100) / kSpb;
    engine.setTrackAutomation(track, {{device, sub::kDeviceOnLane, Points{{0.0, 1.f, 0.f}, {off, 1.f, 0.f}, {off, 0.f, 0.f}}}});
    const Samples switched = channel(engine.renderOffline(0.0, 2 * kBeat), 0);
    CHECK(allEqual(slice(switched, 0, kBeat), 0.0));
    CHECK(allclose(slice(switched, kBeat), 0.5, 1e-6));
}

TEST_CASE("a rack switched off passes its input on") {
    SwitchEngine e;
    auto& engine = e.engine;
    const uint32_t track = dcTrack(engine, longDcWav());
    const uint32_t rack = engine.addRack(engine.trackChain(track), -1);
    const uint32_t chain = engine.addRackChain(rack, -1);
    const uint32_t utility = utilityOn(engine, chain, -12.f);
    engine.setTrackAutomation(track, {{rack, sub::kDeviceOnLane, kOnThenOff}});
    Samples out = engine.renderOffline(0.0, 2 * kBeat);
    CHECK_APPROX_REL(at(out, kBeat / 2, 0), 0.5 * dbGain(-12.0), 1e-4);
    CHECK_EQ(at(out, kBeat + kFade + 100, 0), 0.5f);
    // A device in it switches as one on the track's own chain does.
    engine.setTrackAutomation(track, {{utility, sub::kDeviceOnLane, kOnThenOff}});
    out = engine.renderOffline(0.0, 2 * kBeat);
    CHECK_APPROX_REL(at(out, kBeat / 2, 0), 0.5 * dbGain(-12.0), 1e-4);
    CHECK_EQ(at(out, kBeat + kFade + 100, 0), 0.5f);
}

TEST_CASE("an instrument switched off is silent, and starts again from silence") {
    SwitchEngine e;
    auto& engine = e.engine;
    const uint32_t track = engine.addTrack();
    const uint32_t synth = engine.addBuiltinProcessor(engine.trackChain(track), "synth", -1);
    // A note held over all three beats; the synth off for the middle one.
    engine.setTrackNotes(track, {{0.0, 3.0, 60, 100}});
    engine.setTrackAutomation(
        track, {{synth, sub::kDeviceOnLane,
                 Points{{0.0, 1.f, 0.f}, {1.0, 1.f, 0.f}, {1.0, 0.f, 0.f}, {2.0, 0.f, 0.f}, {2.0, 1.f, 0.f}}}});
    const Samples out = channel(engine.renderOffline(0.0, 3 * kBeat), 0);
    CHECK(maxAbs(slice(out, kBeat / 2, kBeat)) > 0.01);
    CHECK(allEqual(slice(out, kBeat + kFade, 2 * kBeat), 0.0));
    // Back on, it was reset: the note it missed the start of doesn't sound again.
    CHECK(maxAbs(slice(out, 2 * kBeat + kFade, 3 * kBeat)) < 1e-6);
}
