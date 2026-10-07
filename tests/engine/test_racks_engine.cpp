// Racks (device groups) in the engine: chains side by side, summed; their
// faders, mute and solo; delay compensation inside a rack (a latent device in one
// chain doesn't smear the others) and around it; automation of nested devices
// and of chain faders in time; instrument racks layering synths; sidechains into
// devices in racks, and taken after them; moving and removing racks; nesting
// limits; chain meters; and renders bit-identical with and without workers.
// Offline, unless a test needs the fake ASIO driver.

#include <cmath>
#include <functional>
#include <map>
#include <tuple>

#include "Engine.h"
#include "harness/AsioDriver.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

enum { FX_GAIN, FX_LATENCY };  // SUB Test Effect's parameters
constexpr double kClick = 0.25;

struct RackEngine {
    sub::Engine engine;
    RackEngine() { engine.setClipFadeMs(0); }
};

// A click of kClick at the start of 1000 samples.
std::string clickWav() {
    Samples click(1000, 0.f);
    click[0] = static_cast<float>(kClick);
    return makeWav(click);
}

uint32_t rackClipTrack(sub::Engine& engine, const std::string& path, double startBeat = 1.0,
                       double seconds = 1000.0 / kSampleRate) {
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, startBeat, seconds)});
    return track;
}

// A rack in a chain, with `chains` empty chains: (rack id, chain ids).
std::pair<uint32_t, std::vector<uint32_t>> rack(sub::Engine& engine, uint32_t chain, int chains = 2, int index = -1) {
    const uint32_t rid = engine.addRack(chain, index);
    std::vector<uint32_t> ids;
    for (int i = 0; i < chains; ++i) ids.push_back(engine.addRackChain(rid, -1));
    return {rid, ids};
}

// SUB Test Effect in a chain: `gain` times its input, `latency` samples late.
uint32_t effect(sub::Engine& engine, uint32_t chain, int latency = 0, double gain = 1.0) {
    const uint32_t pid = addTestPlugin(engine, chain, "SUB Test Effect");
    engine.setProcessorParam(pid, FX_GAIN, static_cast<float>(gain / 2));  // (0..1 is 0..2 times)
    engine.setProcessorParam(pid, FX_LATENCY, static_cast<float>(latency));
    engine.idle();  // the plug-in asked for a restart to change its latency
    CHECK_EQ(engine.processorInfo(pid).latency, latency);
    return pid;
}

Samples render(sub::Engine& engine, double beats = 2.0) {
    return engine.renderOffline(0.0, static_cast<int64_t>(beats * kBeat));
}

Clicks clicks(const Samples& out) { return clicksOf(channel(out, 0)); }

// A MIDI track playing one note, with a synth in each of the chains `chains` makes for it.
uint32_t synthTrack(sub::Engine& engine, const std::function<std::vector<uint32_t>(uint32_t)>& chains) {
    const uint32_t track = engine.addTrack();
    engine.setTrackNotes(track, {{0.5, 1.0, 60, 100}});
    for (const uint32_t chain : chains(track)) engine.addBuiltinProcessor(chain, "synth", -1);
    return track;
}

}  // namespace

TEST_CASE("an empty rack passes its input on") {
    RackEngine e;
    auto& engine = e.engine;
    const uint32_t track = rackClipTrack(engine, clickWav());
    const uint32_t rid = engine.addRack(engine.trackChain(track), -1);
    const sub::ProcessorInfo info = engine.processorInfo(rid);
    CHECK_EQ(info.typeId, std::string("rack"));
    CHECK_EQ(info.name, std::string("Rack"));
    CHECK_EQ(info.latency, 0);
    CHECK(!info.hasSidechain);
    CHECK(engine.rackChains(rid).empty());
    CHECK_CLICKS(clicks(render(engine)), {{kBeat, kClick}});
    const uint32_t chain = engine.addRackChain(rid, -1);  // an empty chain: its input
    CHECK_EQ(engine.chainRack(chain), rid);
    CHECK_EQ(engine.chainRack(engine.trackChain(track)), 0u);
    CHECK_CLICKS(clicks(render(engine)), {{kBeat, kClick}});
}

TEST_CASE("chains sum their devices' outputs") {
    requireTestPlugins();
    RackEngine e;
    auto& engine = e.engine;
    const uint32_t track = rackClipTrack(engine, clickWav());
    const auto [rid, chains] = rack(engine, engine.trackChain(track));
    const uint32_t a = chains[0], b = chains[1];
    effect(engine, a, 0, 0.5);
    effect(engine, b, 0, 0.25);
    CHECK_CLICKS(clicks(render(engine)), {{kBeat, kClick * 0.75}});
    const uint32_t third = engine.addRackChain(rid, 0);  // an empty one, first
    CHECK(engine.rackChains(rid) == (std::vector<uint32_t>{third, a, b}));
    CHECK_CLICKS(clicks(render(engine)), {{kBeat, kClick * 1.75}});
    engine.setRackChainOrder(rid, {b, a, third});
    CHECK(engine.rackChains(rid) == (std::vector<uint32_t>{b, a, third}));
    CHECK_THROWS_AS(engine.setRackChainOrder(rid, {a, b}), std::invalid_argument);
    engine.removeRackChain(third);
    CHECK_CLICKS(clicks(render(engine)), {{kBeat, kClick * 0.75}});
    engine.setProcessorEnabled(rid, false);  // switched off: its input as it is
    CHECK_CLICKS(clicks(render(engine)), {{kBeat, kClick}});
    engine.setProcessorEnabled(rid, true);
    // A device after the rack hears the sum.
    effect(engine, engine.trackChain(track), 0, 2.0);
    CHECK_CLICKS(clicks(render(engine)), {{kBeat, kClick * 1.5}});
}

TEST_CASE("chain faders, mute and solo") {
    RackEngine e;
    auto& engine = e.engine;
    const uint32_t track = rackClipTrack(engine, clickWav());
    const auto [rid, chains] = rack(engine, engine.trackChain(track));
    const uint32_t a = chains[0], b = chains[1];

    const auto heard = [&](double left, double right, const char* what) {
        INFO(what);
        const Samples out = render(engine);
        CHECK_APPROX(std::round(at(out, kBeat, 0) * 1e6) / 1e6, left);
        CHECK_APPROX(std::round(at(out, kBeat, 1) * 1e6) / 1e6, right);
    };

    heard(2 * kClick, 2 * kClick, "both chains");
    engine.setChainGain(a, 0.5f);
    heard(1.5 * kClick, 1.5 * kClick, "chain a at half");
    engine.setChainPan(b, 1.f);  // hard right
    heard(0.5 * kClick, 1.5 * kClick, "chain b hard right");
    engine.setChainPan(b, 0.f);
    engine.setChainMute(a, true);
    heard(kClick, kClick, "chain a muted");
    engine.setChainMute(a, false);
    engine.setChainSolo(a, true);  // only the soloed chains of the rack
    heard(0.5 * kClick, 0.5 * kClick, "chain a soloed");
    engine.setChainSolo(b, true);
    heard(1.5 * kClick, 1.5 * kClick, "both soloed");
    engine.setChainMute(b, true);  // muted wins
    heard(0.5 * kClick, 0.5 * kClick, "chain b soloed and muted");
    CHECK_THROWS_AS(engine.setChainGain(engine.trackChain(track), 0.5f), std::invalid_argument);  // a track's own chain has no fader of its own
}

TEST_CASE("a latent device in one chain doesn't smear the others") {
    // Every click lands on beat 1 as one: the other chains are delayed to line
    // up with the slowest, and the rack's latency is compensated like any device's.
    requireTestPlugins();
    RackEngine e;
    auto& engine = e.engine;
    const std::string wav = clickWav();
    const uint32_t track = rackClipTrack(engine, wav);
    const auto [rid, chains] = rack(engine, engine.trackChain(track));
    const uint32_t a = chains[0], b = chains[1];
    rackClipTrack(engine, wav);  // beside it, delayed to line up with it
    effect(engine, a, 300);
    CHECK_EQ(engine.processorInfo(rid).latency, 300);
    CHECK_CLICKS(clicks(render(engine)), {{kBeat, 3 * kClick}});
    effect(engine, b, 120);
    effect(engine, b, 250);  // now the slowest
    CHECK_EQ(engine.processorInfo(rid).latency, 370);
    CHECK_CLICKS(clicks(render(engine)), {{kBeat, 3 * kClick}});
    // Before and after the rack on its track, and a rack in a rack.
    effect(engine, engine.trackChain(track), 50);
    engine.moveProcessor(engine.chainProcessors(engine.trackChain(track)).back(), engine.trackChain(track), 0);
    const auto [inner, innerChains] = rack(engine, a);
    effect(engine, innerChains[0], 500);
    CHECK_EQ(engine.processorInfo(inner).latency, 500);
    CHECK_EQ(engine.processorInfo(rid).latency, 800);
    effect(engine, engine.trackChain(track), 30);
    CHECK_CLICKS(clicks(render(engine)), {{kBeat, 4 * kClick}});  // (the inner rack doubles chain a)
}

TEST_CASE("nested device automation plays in time") {
    // A device in a rack hears the timeline as late as the devices before the
    // rack and before it in its chain: its automation is as late.
    requireTestPlugins();
    RackEngine e;
    auto& engine = e.engine;
    const uint32_t track = rackClipTrack(engine, makeWav(full(2 * kSampleRate * 2, 0.5f), 2), 0.0, 2.0);
    effect(engine, engine.trackChain(track), 100);
    const auto [rid, chains] = rack(engine, engine.trackChain(track));
    const uint32_t a = chains[0], b = chains[1];
    engine.setChainMute(b, true);
    effect(engine, a, 50);
    effect(engine, b, 400);  // the slowest chain: chain a is delayed after it
    const uint32_t gain = utilityOn(engine, a);
    const sub::ParamInfo info = paramInfo(engine, gain, "gain");
    const float quiet = info.toNormalized(-60.f), loud = info.toNormalized(0.f);
    const int64_t step = kBeat + 400;
    const double stepBeat = static_cast<double>(step) / kBeat;
    engine.setTrackAutomation(track, {{gain, "gain", {{0.0, quiet, 0.f}, {stepBeat, quiet, 0.f}, {stepBeat, loud, 0.f}}}});
    const Samples out = channel(render(engine, 3.0), 0);
    CHECK(maxAbs(slice(out, step - 300, step)) < 0.001);  // still at -60 dB right up to the step
    CHECK(out[step + 40] > 0.01);  // and rising from it at once (smoothed)
}

TEST_CASE("chain volume automation plays in time") {
    // A chain's fader comes after the devices before the rack and in its chain.
    requireTestPlugins();
    RackEngine e;
    auto& engine = e.engine;
    const uint32_t track = rackClipTrack(engine, makeWav(full(2 * kSampleRate * 2, 0.5f), 2), 0.0, 2.0);
    effect(engine, engine.trackChain(track), 100);
    const auto [rid, chains] = rack(engine, engine.trackChain(track));
    const uint32_t a = chains[0], b = chains[1];
    effect(engine, a, 50);
    effect(engine, b, 400);
    engine.setChainMute(b, true);
    const int64_t step = kBeat + 400;
    const double stepBeat = static_cast<double>(step) / kBeat;
    const std::string chainParam = "chain:" + std::to_string(a);
    engine.setTrackAutomation(track,
                              {{rid, chainParam + ":volume", {{0.0, 0.f, 0.f}, {stepBeat, 0.f, 0.f}, {stepBeat, 1.f, 0.f}}}});
    Samples out = channel(render(engine, 3.0), 0);
    CHECK(maxAbs(slice(out, step - 300, step)) < 1e-6);  // silent right up to the step
    CHECK_APPROX_REL(out[step + 1], 0.5 * sub::kMaxVolumeGain, 1e-4);  // a fader isn't smoothed offline
    engine.setTrackAutomation(track, {{rid, chainParam + ":pan", {{0.0, 1.f, 0.f}}}});
    out = render(engine, 3.0);
    CHECK_NEAR(at(out, step, 0), 0.0, 1e-7);
    CHECK_APPROX_REL(at(out, step, 1), 0.5, 1e-4);
}

TEST_CASE("an instrument rack layers two synths") {
    RackEngine e;
    auto& engine = e.engine;
    const uint32_t layered = synthTrack(engine, [&](uint32_t t) { return rack(engine, engine.trackChain(t)).second; });
    const Samples both = render(engine);
    CHECK(maxAbs(both) > 0.01);
    engine.removeTrack(layered);
    synthTrack(engine, [&](uint32_t t) { return std::vector<uint32_t>{engine.trackChain(t)}; });
    synthTrack(engine, [&](uint32_t t) { return std::vector<uint32_t>{engine.trackChain(t)}; });
    CHECK_ALLCLOSE(both, render(engine), 1e-7, 1e-6);  // two tracks of one synth each
}

TEST_CASE("a sidechain into a device in a rack lines up") {
    // SUB Test Sidechain puts out its input plus its sidechain: both clicks land
    // on beat 1 together, whatever the latency before the rack and in it.
    requireTestPlugins();
    const std::vector<std::tuple<int, int, int>> cases{
        {0, 0, 0},
        {200, 0, 0},  // the sidechain comes early: it waits
        {0, 300, 0},
        {0, 0, 500},  // the device's chain is delayed after it, not before
        {100, 50, 400},
    };
    for (const auto& [beforeRack, beforeDevice, otherChain] : cases) {
        INFO("before the rack " + std::to_string(beforeRack) + ", before the device " + std::to_string(beforeDevice) +
             ", in the other chain " + std::to_string(otherChain));
        RackEngine e;
        auto& engine = e.engine;
        const std::string wav = clickWav();
        const uint32_t source = rackClipTrack(engine, wav);
        const uint32_t track = rackClipTrack(engine, wav);
        if (beforeRack) effect(engine, engine.trackChain(track), beforeRack);
        const auto [rid, chains] = rack(engine, engine.trackChain(track));
        const uint32_t a = chains[0], b = chains[1];
        engine.setChainMute(b, true);
        if (beforeDevice) effect(engine, a, beforeDevice);
        const uint32_t keyed = addTestPlugin(engine, a, "SUB Test Sidechain");
        if (otherChain) effect(engine, b, otherChain);
        engine.setTrackGain(source, 0.f);  // heard only through the sidechain (taken before the fader)
        engine.setProcessorSidechain(keyed, source, sub::SidechainTap::PreFader);
        CHECK_CLICKS(clicks(render(engine)), {{kBeat, 2 * kClick}});
        CHECK_THROWS_AS(engine.setProcessorSidechain(keyed, track), std::invalid_argument);  // its own track: a cycle
    }
}

TEST_CASE("a sidechain taken after a device in a rack") {
    // The key is the chain's signal after that device: before the chain's fader,
    // and without the other chains.
    requireTestPlugins();
    RackEngine e;
    auto& engine = e.engine;
    const uint32_t source = rackClipTrack(engine, clickWav());
    const auto [rid, chains] = rack(engine, engine.trackChain(source));
    const uint32_t a = chains[0], b = chains[1];
    const uint32_t half = effect(engine, a, 0, 0.5);
    effect(engine, a, 0, 0.5);
    const uint32_t quarter = effect(engine, b, 0, 0.25);
    engine.setChainGain(a, 0.5f);
    engine.setTrackGain(source, 0.f);  // heard only through the sidechain
    const uint32_t keyed = addTestPlugin(engine, engine.trackChain(engine.addTrack()), "SUB Test Sidechain");
    const std::vector<std::tuple<uint32_t, double>> taps{
        {half, kClick * 0.5},
        {quarter, kClick * 0.25},
        {rid, kClick * (0.25 * 0.5 + 0.25)},  // after the rack: its chains summed after their faders
    };
    for (const auto& [after, key] : taps) {
        INFO("after " + std::to_string(after));
        engine.setProcessorSidechain(keyed, source, sub::SidechainTap::AfterDevice, after);
        CHECK_CLICKS(clicks(render(engine)), {{kBeat, key}});
    }
    engine.setProcessorSidechain(keyed, source, sub::SidechainTap::AfterDevice, half);
    engine.setProcessorEnabled(half, false);  // switched off, it passes its input on
    CHECK_CLICKS(clicks(render(engine)), {{kBeat, kClick}});
    engine.setProcessorEnabled(half, true);
    engine.setProcessorEnabled(rid, false);  // a rack switched off passes its input on: after it
    CHECK_CLICKS(clicks(render(engine)), {{kBeat, kClick}});
    engine.setProcessorEnabled(rid, true);
    const auto [inner, innerChains] = rack(engine, b, 1);
    const uint32_t deep = effect(engine, innerChains[0], 0, 0.5);
    engine.setProcessorSidechain(keyed, source, sub::SidechainTap::AfterDevice, deep);
    CHECK_CLICKS(clicks(render(engine)), {{kBeat, kClick * 0.25 * 0.5}});  // after the chain's first device, then this one
    engine.setProcessorEnabled(inner, false);
    CHECK_CLICKS(clicks(render(engine)), {{kBeat, kClick * 0.25}});
    engine.setProcessorEnabled(inner, true);
    engine.moveProcessor(deep, engine.trackChain(engine.addTrack()), -1);  // it left the source: before the fader
    CHECK_CLICKS(clicks(render(engine)), {{kBeat, kClick * (0.25 * 0.5 + 0.25)}});
    CHECK_THROWS_AS(engine.setProcessorSidechain(keyed, source, sub::SidechainTap::AfterDevice, deep),
                    std::invalid_argument);  // not on the source
}

TEST_CASE("a sidechain taken after a device in a rack lines up") {
    // The source and the keyed device's track click on the same beat: at the
    // device both clicks fall on one sample, whatever the latency before the
    // rack, before and after the tap in its chain, in the other chain, and
    // before the device.
    requireTestPlugins();
    const std::vector<std::tuple<int, int, int, int, int>> cases{
        {0, 0, 0, 0, 0},
        {200, 0, 0, 0, 0},
        {0, 300, 0, 0, 0},
        {0, 0, 250, 0, 0},  // after the tap: doesn't count
        {0, 0, 0, 400, 0},  // the other chain: lines its chain up after the tap, doesn't count
        {0, 0, 0, 0, 350},  // the sidechain comes early: it waits
        {100, 50, 70, 400, 120},
    };
    for (const auto& [beforeRack, beforeTap, afterTap, otherChain, beforeDevice] : cases) {
        INFO("before the rack " + std::to_string(beforeRack) + ", before the tap " + std::to_string(beforeTap) +
             ", after it " + std::to_string(afterTap) + ", in the other chain " + std::to_string(otherChain) +
             ", before the device " + std::to_string(beforeDevice));
        RackEngine e;
        auto& engine = e.engine;
        const std::string wav = clickWav();
        const uint32_t source = rackClipTrack(engine, wav);
        if (beforeRack) effect(engine, engine.trackChain(source), beforeRack);
        const auto [rid, chains] = rack(engine, engine.trackChain(source));
        if (beforeTap) effect(engine, chains[0], beforeTap);
        const uint32_t tapped = effect(engine, chains[0]);
        if (afterTap) effect(engine, chains[0], afterTap);
        if (otherChain) effect(engine, chains[1], otherChain);
        engine.setTrackGain(source, 0.f);  // heard only through the sidechain
        const uint32_t track = rackClipTrack(engine, wav);
        if (beforeDevice) effect(engine, engine.trackChain(track), beforeDevice);
        const uint32_t keyed = addTestPlugin(engine, engine.trackChain(track), "SUB Test Sidechain");
        engine.setProcessorSidechain(keyed, source, sub::SidechainTap::AfterDevice, tapped);
        CHECK_CLICKS(clicks(render(engine)), {{kBeat, 2 * kClick}});
    }
}

TEST_CASE("moving devices into and out of racks") {
    requireTestPlugins();
    RackEngine e;
    auto& engine = e.engine;
    const std::string wav = clickWav();
    const uint32_t track = rackClipTrack(engine, wav);
    const uint32_t other = rackClipTrack(engine, wav, 0.5);
    const uint32_t fx = effect(engine, engine.trackChain(track), 0, 0.5);
    const auto [rid, chains] = rack(engine, engine.trackChain(track));
    const uint32_t a = chains[0], b = chains[1];
    engine.moveProcessor(fx, a, -1);  // in: one chain halves, the other passes
    CHECK_EQ(engine.processorChain(fx), a);
    CHECK(engine.chainProcessors(a) == std::vector<uint32_t>{fx});
    CHECK_CLICKS(clicks(render(engine)), {{kBeat / 2, kClick}, {kBeat, 1.5 * kClick}});
    engine.moveProcessor(rid, engine.trackChain(other), -1);  // the rack moves with its chains and devices
    CHECK(engine.rackChains(rid) == (std::vector<uint32_t>{a, b}));
    CHECK_EQ(engine.processorChain(fx), a);
    CHECK_CLICKS(clicks(render(engine)), {{kBeat / 2, 1.5 * kClick}, {kBeat, kClick}});
    CHECK_THROWS_AS(engine.moveProcessor(rid, a, -1), std::invalid_argument);  // into itself
    const auto [inner, innerChains] = rack(engine, b);
    const uint32_t c = innerChains[0];
    CHECK_THROWS_AS(engine.moveProcessor(rid, c, -1), std::invalid_argument);  // into a rack in it
    engine.moveProcessor(fx, engine.trackChain(track), -1);  // out again
    CHECK_CLICKS(clicks(render(engine)), {{kBeat / 2, 3 * kClick}, {kBeat, 0.5 * kClick}});
    engine.removeProcessor(rid);  // with everything in it
    CHECK_THROWS_AS(engine.processorInfo(inner), std::invalid_argument);
    CHECK_THROWS_AS(engine.chainProcessors(c), std::invalid_argument);
    CHECK_CLICKS(clicks(render(engine)), {{kBeat / 2, kClick}, {kBeat, 0.5 * kClick}});
    engine.removeTrack(track);
    CHECK_THROWS_AS(engine.processorInfo(fx), std::invalid_argument);
}

TEST_CASE("racks nest at most kMaxRackDepth deep") {
    RackEngine e;
    auto& engine = e.engine;
    const uint32_t track = engine.addTrack();
    uint32_t chain = engine.trackChain(track);
    std::vector<uint32_t> racks;
    for (int i = 0; i < sub::Engine::kMaxRackDepth; ++i) {
        const auto [rid, chains] = rack(engine, chain, 1);
        chain = chains[0];
        racks.push_back(rid);
    }
    CHECK_THROWS_AS(engine.addRack(chain, -1), std::invalid_argument);
    engine.addBuiltinProcessor(chain, "utility", -1);  // devices may go there
    CHECK_THROWS_AS(engine.moveProcessor(racks[1], engine.rackChains(racks.back())[0], -1),
                    std::invalid_argument);  // nor may a rack move deeper
    CHECK_ARRAY_EQUAL(render(engine), 0.0);
    engine.removeTrack(track);  // everything in it goes
}

TEST_CASE("a sidechain moving with a rack can't close a cycle") {
    requireTestPlugins();
    RackEngine e;
    auto& engine = e.engine;
    const uint32_t source = rackClipTrack(engine, clickWav());
    const uint32_t track = engine.addTrack();
    const auto [rid, chains] = rack(engine, engine.trackChain(track), 1);
    const uint32_t keyed = addTestPlugin(engine, chains[0], "SUB Test Sidechain");
    engine.setProcessorSidechain(keyed, source);
    CHECK_THROWS_AS(engine.moveProcessor(rid, engine.trackChain(source), -1), std::invalid_argument);  // onto its own source
    CHECK_EQ(engine.processorChain(rid), engine.trackChain(track));
    engine.removeTrack(source);  // its sidechain goes with its source
    CHECK(!engine.processorSidechain(keyed).has_value());
}

TEST_CASE("racks render the same with and without workers") {
    requireTestPlugins();
    RackEngine e;
    auto& engine = e.engine;
    Rng rng(9);
    const std::string noise = makeWav(rng.uniformSamples(kSampleRate * 2, -0.5, 0.5), 2);
    for (int t = 0; t < 6; ++t) {
        const uint32_t track = rackClipTrack(engine, noise, 0.0, 1.0);
        const auto [rid, chains] = rack(engine, engine.trackChain(track), 3);
        for (size_t i = 0; i < chains.size(); ++i)
            effect(engine, chains[i], 37 * ((t + static_cast<int>(i)) % 4), 0.5 + 0.1 * static_cast<double>(i));
        const auto [inner, innerChains] = rack(engine, chains[0]);
        utilityOn(engine, innerChains[0], -3.f);
        effect(engine, innerChains[1], 11 * t);
        engine.setChainGain(chains[1], 0.7f);
        engine.addBuiltinProcessor(chains[2], "synth", -1);
        engine.setTrackNotes(track, {{0.25 * t, 0.5, 60 + t, 100}});
    }
    engine.setAudioThreads(1);
    render(engine, 4.0);  // (the utilities' gains glide to where they were set, once)
    const Samples serial = render(engine, 4.0);
    engine.setAudioThreads(4);
    engine.setCostOrdering(false);
    const Samples parallel = render(engine, 4.0);
    CHECK_ARRAY_EQUAL(serial, parallel);
}

TEST_CASE("chain meters") {
    AsioDriver driver;  // (skips the test without ASIO)
    driver.setManual(true);
    sub::Engine engine;
    engine.setClipFadeMs(0);
    const uint32_t track = rackClipTrack(engine, makeWav(full(kSampleRate * 2, 0.5f), 2), 0.0, 1.0);
    const auto [rid, chains] = rack(engine, engine.trackChain(track));
    const uint32_t a = chains[0], b = chains[1];
    engine.setChainGain(b, 0.5f);
    openAsio(engine, kSampleRate, 256);
    engine.play();
    engine.takeMeters();
    driver.process(20);
    std::map<std::pair<uint32_t, uint32_t>, float> meters;
    for (const auto& m : engine.takeMeters()) meters[{m.trackId, m.chainId}] = m.left;
    const auto meter = [&](uint32_t chain) { return meters[std::make_pair(track, chain)]; };
    CHECK_APPROX_REL(meter(a), 0.5, 1e-3);
    CHECK_APPROX_REL(meter(b), 0.25, 1e-3);
    CHECK_APPROX_REL(meter(0), 0.75, 1e-3);  // the track's own
    engine.closeDevice();
}
