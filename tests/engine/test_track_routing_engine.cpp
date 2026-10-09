// A track's output and input as Ableton routes them, in the engine: Sends Only
// (no output), an output into a device's sidechain (summed with the device's
// own sidechain and other tracks' outputs, lined up as a sidechain is), an
// output into a track taking it as its input (Track In: heard only while that
// track monitors), and an input from a track tapped before its devices, before
// its fader or after it. Rendered offline with the test plug-ins (SUB Test
// Sidechain's output: its input plus its sidechain), so no audio device is
// needed; offline, a track monitoring In hears what other tracks bring it.

#include "Engine.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

constexpr double kClick = 0.25;

struct RoutingEngine {
    sub::Engine engine;
    RoutingEngine() {
        requireTestPlugins();
        engine.setClipFadeMs(0);
    }
};

// A click of kClick at the start of 1000 samples.
uint32_t clickTrack(sub::Engine& engine, const std::string& path, double startBeat = 1.0) {
    return clipTrack(engine, path, startBeat, 1000.0 / kSampleRate);
}

// SUB Test Effect: `gain` times its input, `latency` samples late.
uint32_t effect(sub::Engine& engine, uint32_t track, int latency = 0, double gain = 1.0) {
    const uint32_t pid = addTestPlugin(engine, engine.trackChain(track), "SUB Test Effect");
    engine.setProcessorParam(pid, FX_GAIN, static_cast<float>(gain / 2));  // (0..1 is 0..2 times)
    engine.setProcessorParam(pid, FX_LATENCY, static_cast<float>(latency));
    engine.idle();  // the plug-in asked for a restart to change its latency
    return pid;
}

Clicks render(sub::Engine& engine, double beats = 2.0) {
    return clicksOf(channel(engine.renderOffline(0.0, static_cast<int64_t>(beats * kBeat)), 0));
}

}  // namespace

TEST_CASE("sends only: a track without an output is heard through its sends") {
    RoutingEngine e;
    auto& engine = e.engine;
    const uint32_t track = clickTrack(engine, clickWav(kClick));
    const uint32_t ret = engine.addTrack();
    engine.setTrackSend(track, ret, 0.5f, false);
    CHECK_CLICKS(render(engine), {{kBeat, kClick * 1.5}});  // its output and its send
    engine.setTrackOutput(track, sub::Engine::kNoOutput);
    CHECK_EQ(engine.trackOutput(track), sub::Engine::kNoOutput);
    CHECK_CLICKS(render(engine), {{kBeat, kClick * 0.5}});  // its send alone
    engine.setTrackSend(track, ret, 0.5f, true);  // before its fader too
    engine.setTrackGain(track, 0.5f);
    CHECK_CLICKS(render(engine), {{kBeat, kClick * 0.5}});
    engine.setTrackOutput(track, sub::Engine::kMaster);
    CHECK_CLICKS(render(engine), {{kBeat, kClick}});
}

TEST_CASE("an output into a device's sidechain is heard only there") {
    RoutingEngine e;
    auto& engine = e.engine;
    const std::string wav = clickWav(kClick);
    const uint32_t source = clickTrack(engine, wav);
    engine.setTrackGain(source, 0.5f);  // after its fader
    const uint32_t track = engine.addTrack();  // no clips: its device puts out what its sidechain hears
    const uint32_t pid = keyed(engine, track);
    engine.setTrackOutputSidechain(source, pid);
    CHECK_EQ(engine.trackOutputSidechain(source), pid);
    CHECK_EQ(engine.trackOutput(source), sub::Engine::kNoOutput);
    CHECK_CLICKS(render(engine), {{kBeat, kClick * 0.5}});  // through the device, not on its own

    // Summed with the device's own sidechain and another track's output.
    const uint32_t other = clickTrack(engine, wav);
    engine.setTrackOutputSidechain(other, pid);
    CHECK_CLICKS(render(engine), {{kBeat, kClick * 1.5}});
    const uint32_t keyer = clickTrack(engine, wav);
    engine.setProcessorSidechain(pid, keyer, sub::SidechainTap::PreFader);
    engine.setTrackMute(keyer, true);  // (its own output silent: what it keys is before its fader)
    CHECK_CLICKS(render(engine), {{kBeat, kClick * 2.5}});

    // Muted, the output keys nothing (it is after the fader).
    engine.setTrackMute(other, true);
    CHECK_CLICKS(render(engine), {{kBeat, kClick * 1.5}});
    engine.setTrackMute(other, false);

    // Back into the master: heard, and no longer keying.
    engine.setTrackOutput(source, sub::Engine::kMaster);
    CHECK_EQ(engine.trackOutputSidechain(source), 0u);
    CHECK_CLICKS(render(engine), {{kBeat, kClick * 2.5}});

    // When the device goes, what went into it goes into the master.
    engine.removeProcessor(pid);
    CHECK_EQ(engine.trackOutputSidechain(other), 0u);
    CHECK_EQ(engine.trackOutput(other), sub::Engine::kMaster);
    CHECK_CLICKS(render(engine), {{kBeat, kClick * 1.5}});  // the source and the other track
}

TEST_CASE("outputs into a sidechain line up with the signal at the device") {
    for (const auto& [sourceLatency, trackLatency] : std::vector<std::pair<int, int>>{{300, 0}, {0, 200}, {120, 450}}) {
        INFO("source " + std::to_string(sourceLatency) + ", track " + std::to_string(trackLatency));
        RoutingEngine e;
        auto& engine = e.engine;
        const std::string wav = clickWav(kClick);
        const uint32_t source = clickTrack(engine, wav);
        effect(engine, source, sourceLatency);
        const uint32_t other = clickTrack(engine, wav);  // a second output into it, with no latency
        const uint32_t track = clickTrack(engine, wav);
        effect(engine, track, trackLatency);
        const uint32_t pid = keyed(engine, track);
        engine.setTrackOutputSidechain(source, pid);
        engine.setTrackOutputSidechain(other, pid);
        CHECK_CLICKS(render(engine), {{kBeat, 3 * kClick}});  // on one sample, at the device and the master
    }
}

TEST_CASE("outputs into a sidechain that would close a cycle are refused") {
    RoutingEngine e;
    auto& engine = e.engine;
    const uint32_t group = engine.addTrack();
    const uint32_t track = clickTrack(engine, clickWav(kClick));
    engine.setTrackOutput(track, group);
    const uint32_t own = keyed(engine, track);
    const uint32_t onGroup = keyed(engine, group);
    CHECK_THROWS_AS(engine.setTrackOutputSidechain(track, own), std::invalid_argument);  // its own device
    CHECK_THROWS_AS(engine.setTrackOutputSidechain(group, own), std::invalid_argument);  // a device on what feeds it
    CHECK_THROWS_AS(engine.setTrackOutputSidechain(track, 9999), std::invalid_argument);
    const uint32_t plain = engine.addBuiltinProcessor(engine.trackChain(group), "utility", -1);
    CHECK_THROWS_AS(engine.setTrackOutputSidechain(track, plain), std::invalid_argument);  // no sidechain input
    // A device moving onto a track that feeds what goes into it: refused too.
    const uint32_t other = clickTrack(engine, clickWav(kClick));
    engine.setTrackOutputSidechain(other, onGroup);
    engine.setTrackOutput(track, other);  // (into the other's bus: the other track feeds the group no more)
    CHECK_THROWS_AS(engine.moveProcessor(onGroup, engine.trackChain(track), -1), std::invalid_argument);
    CHECK_EQ(engine.trackOutputSidechain(other), onGroup);
}

TEST_CASE("track in: what goes into a track is heard while it monitors") {
    RoutingEngine e;
    auto& engine = e.engine;
    const std::string wav = clickWav(kClick);
    const uint32_t source = clickTrack(engine, wav);
    engine.setTrackGain(source, 0.5f);
    const uint32_t track = clickTrack(engine, wav, 0.0);  // its own clip, a beat earlier
    engine.setTrackInMonitored(track, true);
    engine.setTrackOutput(source, track);
    effect(engine, track, 0, 2.0);  // what the track hears goes through its devices
    engine.setTrackMonitor(track, sub::MonitorMode::Off);
    CHECK_CLICKS(render(engine), {{0, kClick * 2}});  // its clip: the source isn't heard
    engine.setTrackMonitor(track, sub::MonitorMode::Auto);  // (offline: not armed, playing back)
    CHECK_CLICKS(render(engine), {{0, kClick * 2}});
    engine.setTrackMonitor(track, sub::MonitorMode::In);
    CHECK_CLICKS(render(engine), {{kBeat, kClick}});  // the source, through the track, instead of its clip
    // Soloing the source keeps the track that hears it heard.
    engine.setTrackSolo(source, true);
    CHECK_CLICKS(render(engine), {{kBeat, kClick}});
    engine.setTrackSolo(source, false);

    // A bus (the default) sums what goes into it always, with its clips.
    engine.setTrackInMonitored(track, false);
    engine.setTrackMonitor(track, sub::MonitorMode::Off);
    CHECK_CLICKS(render(engine), {{0, kClick * 2}, {kBeat, kClick}});
}

TEST_CASE("track in lines nothing up: the track hears its inputs as they come") {
    RoutingEngine e;
    auto& engine = e.engine;
    const std::string wav = clickWav(kClick);
    const uint32_t source = clickTrack(engine, wav);
    effect(engine, source, 300);
    const uint32_t track = engine.addTrack();
    engine.setTrackInMonitored(track, true);
    engine.setTrackMonitor(track, sub::MonitorMode::In);
    engine.setTrackOutput(source, track);
    const uint32_t other = clickTrack(engine, wav);  // straight into the master
    // The source comes 300 late into the track; the master lines the track up with the other one.
    CHECK_CLICKS(render(engine), {{kBeat, kClick}, {kBeat + 300, kClick}});
    (void)other;
}

TEST_CASE("an input from a track tapped before its devices, before its fader or after it") {
    RoutingEngine e;
    auto& engine = e.engine;
    const uint32_t source = clickTrack(engine, clickWav(kClick));
    const uint32_t fx = effect(engine, source, 0, 0.5);
    engine.setTrackGain(source, 0.5f);
    engine.setTrackOutput(source, sub::Engine::kNoOutput);  // heard only through the track taking it
    const uint32_t track = engine.addTrack();
    engine.setTrackMonitor(track, sub::MonitorMode::In);
    const std::vector<std::tuple<sub::SidechainTap, uint32_t, double>> taps{
        {sub::SidechainTap::PostFader, 0, kClick * 0.25},
        {sub::SidechainTap::PreFader, 0, kClick * 0.5},
        {sub::SidechainTap::PreFx, 0, kClick},
        {sub::SidechainTap::AfterDevice, fx, kClick * 0.5},
    };
    for (const auto& [tap, after, heard] : taps) {
        INFO("tap " + std::to_string(static_cast<int>(tap)));
        engine.setTrackInputTrack(track, source, tap, after);
        CHECK_CLICKS(render(engine), {{kBeat, heard}});
    }
    CHECK_THROWS_AS(engine.setTrackInputTrack(track, source, sub::SidechainTap::AfterDevice, 9999),
                    std::invalid_argument);
    // Muted, the source's taps before its fader still sound (its fader mutes what is after it).
    engine.setTrackInputTrack(track, source, sub::SidechainTap::PreFx);
    engine.setTrackMute(source, true);
    CHECK_CLICKS(render(engine), {{kBeat, kClick}});
    // Not monitored, the track plays its clips (none).
    engine.setTrackMonitor(track, sub::MonitorMode::Off);
    CHECK_CLICKS(render(engine), {});
}
