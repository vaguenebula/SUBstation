// Sidechains in the engine: a device's aux input hearing another track's
// signal, after its fader, before it, before its devices or after one of them; lined up with
// the signal at the device sample for sample, whatever the latency before the tap
// and on the device's own track (the sidechain is delayed, or the track's signal
// is, before the device); cycles refused; the source going; mute and solo.
// Rendered offline with SUB Test Sidechain (its output: its input plus its
// sidechain), so no audio device is needed.

#include <tuple>

#include "Engine.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

enum { FX_GAIN, FX_LATENCY };  // SUB Test Effect's parameters
constexpr int KEY_SILENT = 0;  // SUB Test Sidechain's (read-only)
constexpr double kClick = 0.25;

// Every test here needs the test plug-ins (the Python module was skipped without them).
struct SidechainEngine {
    sub::Engine engine;
    SidechainEngine() {
        requireTestPlugins();
        engine.setClipFadeMs(0);
    }
};

// A click of kClick at the start of 1000 samples.
std::string clickWav() {
    Samples click(1000, 0.f);
    click[0] = static_cast<float>(kClick);
    return makeWav(click);
}

uint32_t clickTrack(sub::Engine& engine, const std::string& path, double startBeat = 1.0,
                    std::optional<uint32_t> output = std::nullopt, double seconds = 1000.0 / kSampleRate) {
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, startBeat, seconds)});
    if (output) engine.setTrackOutput(track, *output);
    return track;
}

// SUB Test Effect: `gain` times its input, `latency` samples late.
uint32_t effect(sub::Engine& engine, uint32_t track, int latency = 0, double gain = 1.0) {
    const uint32_t pid = addTestPlugin(engine, engine.trackChain(track), "SUB Test Effect");
    engine.setProcessorParam(pid, FX_GAIN, static_cast<float>(gain / 2));  // (0..1 is 0..2 times)
    engine.setProcessorParam(pid, FX_LATENCY, static_cast<float>(latency));  // (in steps: samples)
    engine.idle();  // the plug-in asked for a restart to change its latency
    CHECK_EQ(engine.processorInfo(pid).latency, latency);
    return pid;
}

// SUB Test Sidechain on a track (its output: its input plus its sidechain).
uint32_t keyed(sub::Engine& engine, uint32_t track) {
    return addTestPlugin(engine, engine.trackChain(track), "SUB Test Sidechain");
}

Clicks render(sub::Engine& engine, double beats = 2.0) {
    return clicksOf(channel(engine.renderOffline(0.0, static_cast<int64_t>(beats * kBeat)), 0));
}

}  // namespace

TEST_CASE("a device hears its sidechain") {
    SidechainEngine e;
    auto& engine = e.engine;
    const uint32_t source = clickTrack(engine, clickWav());
    const uint32_t track = engine.addTrack();  // no clips: what its device puts out is what it hears on its sidechain
    const uint32_t pid = keyed(engine, track);
    CHECK(engine.processorInfo(pid).hasSidechain);
    CHECK(!engine.processorSidechain(pid).has_value());
    CHECK_CLICKS(render(engine), {{kBeat, kClick}});
    engine.setProcessorSidechain(pid, source);
    const auto info = engine.processorSidechain(pid);
    REQUIRE(info.has_value());
    CHECK_EQ(info->trackId, source);
    CHECK(info->tap == sub::SidechainTap::PostFader);
    CHECK_EQ(info->tapProcessorId, 0u);
    CHECK_CLICKS(render(engine), {{kBeat, 2 * kClick}});  // the source, and the device's output
    engine.clearProcessorSidechain(pid);
    CHECK(!engine.processorSidechain(pid).has_value());
    CHECK_CLICKS(render(engine), {{kBeat, kClick}});
}

TEST_CASE("devices without a sidechain input") {
    SidechainEngine e;
    auto& engine = e.engine;
    const uint32_t source = engine.addTrack(), track = engine.addTrack();
    const uint32_t utility = engine.addBuiltinProcessor(engine.trackChain(track), "utility", -1);
    const uint32_t fx = effect(engine, track);
    for (const uint32_t pid : {utility, fx}) {
        INFO("processor " + std::to_string(pid));
        CHECK(!engine.processorInfo(pid).hasSidechain);
        CHECK_THROWS_AS(engine.setProcessorSidechain(pid, source), std::invalid_argument);
    }
}

TEST_CASE("taps") {
    // After the source's fader, before it, before its devices, or after one of them.
    SidechainEngine e;
    auto& engine = e.engine;
    const uint32_t source = clickTrack(engine, clickWav());
    const uint32_t first = effect(engine, source, 0, 0.5);
    const uint32_t second = effect(engine, source, 0, 0.5);
    engine.setTrackGain(source, 0.5f);
    const uint32_t pid = keyed(engine, engine.addTrack());
    const double heard = kClick * 0.5 * 0.5 * 0.5;  // the source itself: its devices, then its fader
    const std::vector<std::tuple<sub::SidechainTap, uint32_t, double>> taps{
        {sub::SidechainTap::PreFx, 0, kClick},
        {sub::SidechainTap::PostFader, 0, heard},
        {sub::SidechainTap::PreFader, 0, kClick * 0.25},
        {sub::SidechainTap::AfterDevice, first, kClick * 0.5},
        {sub::SidechainTap::AfterDevice, second, kClick * 0.25},
    };
    for (const auto& [tap, after, key] : taps) {
        INFO("tap " + std::to_string(static_cast<int>(tap)) + " after " + std::to_string(after));
        engine.setProcessorSidechain(pid, source, tap, after);
        CHECK_EQ(engine.processorSidechain(pid)->tapProcessorId, after);
        CHECK_CLICKS(render(engine), {{kBeat, heard + key}});
    }
    CHECK_THROWS_AS(engine.setProcessorSidechain(pid, source, sub::SidechainTap::AfterDevice, pid),
                    std::invalid_argument);  // not the source's device
    // A device switched off passes the signal on as it is.
    engine.setProcessorEnabled(first, false);
    CHECK_CLICKS(render(engine), {{kBeat, kClick * 0.5 * 0.5 + kClick * 0.5}});
}

TEST_CASE("a tap after a device that leaves the source is before the fader") {
    SidechainEngine e;
    auto& engine = e.engine;
    const uint32_t source = clickTrack(engine, clickWav());
    const uint32_t fx = effect(engine, source, 0, 0.5);
    engine.setTrackGain(source, 0.5f);
    const uint32_t pid = keyed(engine, engine.addTrack());
    engine.setProcessorSidechain(pid, source, sub::SidechainTap::AfterDevice, fx);
    const uint32_t other = engine.addTrack();
    engine.moveProcessor(fx, engine.trackChain(other), -1);
    CHECK(engine.processorSidechain(pid)->tap == sub::SidechainTap::AfterDevice);  // as it was set
    CHECK_CLICKS(render(engine), {{kBeat, kClick * 0.5 + kClick}});  // but before the fader
    engine.moveProcessor(fx, engine.trackChain(source), -1);
    CHECK_CLICKS(render(engine), {{kBeat, kClick * 0.25 + kClick * 0.5}});  // after it again
}

TEST_CASE("the sidechain lines up with the signal at its device") {
    // The source and the device's track click on the same beat: at the device
    // the clicks fall on one sample (whose output is two clicks), and at the
    // master the source's click too.
    requireTestPlugins();
    const std::vector<std::tuple<int, int, int, int>> latencies{
        {300, 0, 0, 0},    // the sidechain comes late: the track's signal waits before the device
        {0, 0, 200, 0},    // the track's signal comes late: the sidechain waits
        {300, 100, 0, 0},  // latency after the tap doesn't count
        {300, 0, 200, 50}, // some of each
        {0, 450, 120, 0},
    };
    for (const auto& [beforeTap, afterTap, beforeDevice, afterDevice] : latencies) {
        for (const std::string tap : {"post", "pre", "device", "pre-fx"}) {
            INFO(tap + " tap; latency before it " + std::to_string(beforeTap) + ", after it " + std::to_string(afterTap) +
                 ", before the device " + std::to_string(beforeDevice) + ", after it " + std::to_string(afterDevice));
            SidechainEngine e;
            auto& engine = e.engine;
            const std::string wav = clickWav();
            const uint32_t source = clickTrack(engine, wav);
            const uint32_t tapped = effect(engine, source, beforeTap);
            effect(engine, source, afterTap);
            const uint32_t track = clickTrack(engine, wav);
            effect(engine, track, beforeDevice);
            const uint32_t pid = keyed(engine, track);
            effect(engine, track, afterDevice);
            if (tap == "device") {
                engine.setProcessorSidechain(pid, source, sub::SidechainTap::AfterDevice, tapped);
            } else {
                const sub::SidechainTap kind = tap == "post"  ? sub::SidechainTap::PostFader
                                               : tap == "pre" ? sub::SidechainTap::PreFader
                                                              : sub::SidechainTap::PreFx;
                engine.setProcessorSidechain(pid, source, kind);
            }
            CHECK_CLICKS(render(engine), {{kBeat, 3 * kClick}});
        }
    }
}

TEST_CASE("a tap before a device waiting for its own sidechain") {
    // The source's own signal waits for a late sidechain before one of its
    // devices: a tap before that wait (before its devices, or after a device
    // before it) leaves the source that much earlier than one after it.
    requireTestPlugins();
    for (const bool waitsFirst : {false, true}) {
        for (const std::string tap : {"pre-fx", "device", "post"}) {
            INFO(tap + (waitsFirst ? " tap, the waiting device first" : " tap, the waiting device second"));
            SidechainEngine e;
            auto& engine = e.engine;
            const std::string wav = clickWav();
            const uint32_t late = clickTrack(engine, wav);
            effect(engine, late, 300);
            const uint32_t source = clickTrack(engine, wav);
            uint32_t waiting = 0, fx = 0;
            if (waitsFirst) {
                waiting = keyed(engine, source);
                fx = effect(engine, source, 40);
            } else {
                fx = effect(engine, source, 40);
                waiting = keyed(engine, source);
            }
            engine.setProcessorSidechain(waiting, late);
            const uint32_t pid = keyed(engine, clickTrack(engine, wav));
            if (tap == "device")
                engine.setProcessorSidechain(pid, source, sub::SidechainTap::AfterDevice, fx);
            else
                engine.setProcessorSidechain(pid, source, tap == "pre-fx" ? sub::SidechainTap::PreFx : sub::SidechainTap::PostFader);
            // The late track, the source with its sidechain, the keyed track with the
            // source's click (and the late one, after the device that waits for it).
            const bool afterTheWait = tap == "post" || (tap == "device" && waitsFirst);
            CHECK_CLICKS(render(engine), {{kBeat, (5 + (afterTheWait ? 1 : 0)) * kClick}});
        }
    }
}

TEST_CASE("a sidechain into a group and into the master") {
    SidechainEngine e;
    auto& engine = e.engine;
    const std::string wav = clickWav();
    const uint32_t source = clickTrack(engine, wav);
    effect(engine, source, 250);
    const uint32_t group = engine.addTrack();
    clickTrack(engine, wav, 1.0, group);
    effect(engine, group, 70);
    const uint32_t inGroup = keyed(engine, group);
    engine.setProcessorSidechain(inGroup, source);
    CHECK_CLICKS(render(engine), {{kBeat, 3 * kClick}});
    effect(engine, sub::Engine::kMaster, 40);
    const uint32_t onMaster = keyed(engine, sub::Engine::kMaster);
    effect(engine, sub::Engine::kMaster, 90);
    engine.setProcessorSidechain(onMaster, source);
    CHECK_CLICKS(render(engine), {{kBeat, 4 * kClick}});
}

TEST_CASE("automation after a device waiting for its sidechain stays in time") {
    // A device after a sidechained one hears the timeline as late as the
    // track's signal waited for the sidechain: its automation is as late.
    SidechainEngine e;
    auto& engine = e.engine;
    const uint32_t source = engine.addTrack();
    effect(engine, source, 300);
    const std::string dc = makeWav(full(4 * kSampleRate * 2, 0.5f), 2);
    const uint32_t track = clickTrack(engine, dc, 0.0, std::nullopt, 4.0);
    engine.setProcessorSidechain(keyed(engine, track), source);
    const uint32_t utility = engine.addBuiltinProcessor(engine.trackChain(track), "utility", -1);
    const sub::ParamInfo info = paramInfo(engine, utility, "gain");
    const int64_t step = kBeat + 100;
    const double stepBeat = static_cast<double>(step) / kBeat;
    const float quiet = info.toNormalized(-60.f), loud = info.toNormalized(0.f);
    engine.setTrackAutomation(track, {{utility, "gain", {{0.0, quiet, 0.f}, {stepBeat, quiet, 0.f}, {stepBeat, loud, 0.f}}}});
    const Samples out = channel(engine.renderOffline(0.0, step + 2000), 0);
    CHECK(maxAbs(slice(out, step - 200, step)) < 0.001);  // settled at -60 dB, right up to the step
    CHECK(out[step - 1] < out[step + 10]);  // rising from it
    CHECK(out[step + 10] < out[step + 1500]);
}

TEST_CASE("sidechain cycles are refused") {
    SidechainEngine e;
    auto& engine = e.engine;
    const uint32_t track = engine.addTrack(), other = engine.addTrack();
    const uint32_t group = engine.addTrack();
    engine.setTrackOutput(track, group);
    const uint32_t pid = keyed(engine, track);
    CHECK_THROWS_AS(engine.setProcessorSidechain(pid, track), std::invalid_argument);  // its own track
    CHECK_THROWS_AS(engine.setProcessorSidechain(pid, group), std::invalid_argument);  // what its track goes into
    CHECK_THROWS_AS(engine.setProcessorSidechain(pid, sub::Engine::kMaster), std::invalid_argument);  // renders after everything
    CHECK_THROWS_AS(engine.setProcessorSidechain(pid, 999), std::invalid_argument);
    const uint32_t onGroup = keyed(engine, group);
    engine.setProcessorSidechain(onGroup, track);  // what goes into it: fine (no cycle)
    engine.setProcessorSidechain(pid, other);
    // The routes it would close a cycle with: outputs, sends and inputs.
    CHECK_THROWS_AS(engine.setTrackOutput(track, other), std::invalid_argument);
    CHECK_THROWS_AS(engine.setTrackSend(track, other, 1.f, false), std::invalid_argument);
    CHECK_THROWS_AS(engine.setTrackInputTrack(other, track), std::invalid_argument);
    // A move that would make it one.
    CHECK_THROWS_AS(engine.moveProcessor(pid, engine.trackChain(other), -1), std::invalid_argument);
    CHECK_EQ(engine.processorChain(pid), engine.trackChain(track));
    const uint32_t masterChain = engine.trackChain(sub::Engine::kMaster);
    engine.moveProcessor(pid, masterChain, -1);  // its sidechain goes with it
    CHECK_EQ(engine.processorChain(pid), masterChain);
    REQUIRE(engine.processorSidechain(pid).has_value());
    CHECK_EQ(engine.processorSidechain(pid)->trackId, other);
}

TEST_CASE("the sidechain goes with its source") {
    SidechainEngine e;
    auto& engine = e.engine;
    const std::string wav = clickWav();
    const uint32_t source = clickTrack(engine, wav);
    const uint32_t track = engine.addTrack();
    const uint32_t pid = keyed(engine, track);
    engine.setProcessorSidechain(pid, source);
    engine.removeTrack(source);
    CHECK(!engine.processorSidechain(pid).has_value());
    CHECK_CLICKS(render(engine), {});
    const uint32_t other = clickTrack(engine, wav);
    engine.setProcessorSidechain(pid, other);
    engine.removeTrack(track);  // and the device with its track
    CHECK_CLICKS(render(engine), {{kBeat, kClick}});
}

TEST_CASE("sidechains, mute and solo") {
    // A sidechain isn't heard on its own: soloing its source doesn't make the
    // device's track heard, soloing that track keeps the source keying it; mute
    // and solo silence it only after the source's fader.
    SidechainEngine e;
    auto& engine = e.engine;
    const std::string wav = clickWav();
    const uint32_t source = clickTrack(engine, wav);
    const uint32_t track = engine.addTrack();
    const uint32_t pid = keyed(engine, track);
    engine.setProcessorSidechain(pid, source);
    engine.setTrackSolo(source, true);
    CHECK_CLICKS(render(engine), {{kBeat, kClick}});  // the source alone
    engine.setTrackSolo(source, false);
    engine.setTrackSolo(track, true);
    CHECK_CLICKS(render(engine), {{kBeat, kClick}});  // the track alone, keyed by the source
    engine.setTrackSolo(track, false);
    engine.setTrackMute(source, true);
    CHECK_CLICKS(render(engine), {});
    engine.setProcessorSidechain(pid, source, sub::SidechainTap::PreFader);
    CHECK_CLICKS(render(engine), {{kBeat, kClick}});  // before the fader: muting doesn't change it
    // A keyed device on a group stays keyed while a track in the group is soloed (the group is heard).
    engine.setProcessorSidechain(pid, source);
    const uint32_t group = engine.addTrack();
    const uint32_t inside = clickTrack(engine, wav, 1.0, group);
    engine.setProcessorSidechain(keyed(engine, group), source);
    engine.setTrackMute(source, false);
    engine.setTrackSolo(inside, true);
    CHECK_CLICKS(render(engine), {{kBeat, 2 * kClick}});  // the track and the group's key, not the source
    engine.setTrackSolo(inside, false);
    // Into the master's devices, the sidechain plays whatever is soloed.
    engine.setTrackMute(source, false);
    engine.setProcessorSidechain(pid, source);
    engine.moveProcessor(pid, engine.trackChain(sub::Engine::kMaster), -1);
    engine.setTrackSolo(engine.addTrack(), true);  // something silent
    CHECK_CLICKS(render(engine), {{kBeat, kClick}});  // the source isn't heard, but keys the master's device
}

TEST_CASE("silence is flagged") {
    SidechainEngine e;
    auto& engine = e.engine;
    const uint32_t pid = keyed(engine, engine.addTrack());
    engine.renderOffline(0.0, 4096);
    engine.idle();  // the plug-in's output parameters
    CHECK_EQ(engine.processorParam(pid, KEY_SILENT), 1.f);  // no source
    const uint32_t source = clickTrack(engine, dcWav(), 0.0, std::nullopt, 1.0);
    engine.setProcessorSidechain(pid, source);
    engine.renderOffline(0.0, 4096);
    engine.idle();
    CHECK_EQ(engine.processorParam(pid, KEY_SILENT), 0.f);
    engine.setTrackMute(source, true);
    engine.renderOffline(0.0, 4096);
    engine.idle();
    CHECK_EQ(engine.processorParam(pid, KEY_SILENT), 1.f);  // silent after the source's fader
}

TEST_CASE("the source renders before the device on any threads") {
    // Many tracks keyed by one source, rendered on several threads: each waits for it.
    SidechainEngine e;
    auto& engine = e.engine;
    const std::string wav = clickWav();
    const uint32_t source = clickTrack(engine, wav);
    effect(engine, source, 33);
    for (int i = 0; i < 12; ++i) {
        const uint32_t track = clickTrack(engine, wav);
        engine.addBuiltinProcessor(engine.trackChain(track), "utility", -1);
        engine.setProcessorSidechain(keyed(engine, track), source);
    }
    for (const int threads : {1, 4}) {
        INFO(std::to_string(threads) + " threads");
        engine.setAudioThreads(threads);
        CHECK_CLICKS(render(engine), {{kBeat, 25 * kClick}});
    }
}
