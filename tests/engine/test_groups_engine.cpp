// Routing in the engine: tracks going into other tracks (group buses), delay
// compensation at every summing point, and solo and mute across levels.
// Rendered offline, so no audio device is needed.

#include <set>

#include "Engine.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

enum { FX_GAIN, FX_LATENCY };  // SUB Test Effect's parameters

struct GroupsEngine {
    sub::Engine engine;
    GroupsEngine() { engine.setClipFadeMs(0); }
};

uint32_t group(sub::Engine& engine, std::optional<uint32_t> output = std::nullopt) {
    const uint32_t track = engine.addTrack();
    if (output) engine.setTrackOutput(track, *output);
    return track;
}

uint32_t utility(sub::Engine& engine, uint32_t track, float gainDb) {
    return utilityOn(engine, engine.trackChain(track), gainDb);
}

uint32_t latentEffect(sub::Engine& engine, uint32_t track, int latency) {
    const uint32_t effect = addTestPlugin(engine, engine.trackChain(track), "SUB Test Effect");
    engine.setProcessorParam(effect, FX_LATENCY, static_cast<float>(latency));
    engine.idle();  // the plug-in asked for a restart to change its latency
    return effect;
}

// A click of 0.25 at the start of 1000 samples.
std::string clickWav() {
    Samples click(1000, 0.f);
    click[0] = 0.25f;
    return makeWav(click);
}

std::vector<int64_t> clicks(const Samples& out) { return above(channel(out, 0), 1e-6); }

std::string level(float value, int seconds = 1) { return makeWav(full(static_cast<size_t>(seconds) * kSampleRate * 2, value), 2); }

}  // namespace

TEST_CASE("a group effect processes the sum of its tracks") {
    GroupsEngine e;
    auto& engine = e.engine;
    const std::string aWav = level(0.5f), bWav = level(0.25f);
    const uint32_t bus = group(engine);
    const uint32_t a = clipTrack(engine, aWav, 0.0, 1.0, bus);
    clipTrack(engine, bWav, 0.0, 1.0, bus);
    CHECK_EQ(engine.trackOutput(a), bus);
    Samples out = engine.renderOffline(0.0, 100);
    CHECK_APPROX(at(out, 50, 0), 0.75);
    CHECK_APPROX(at(out, 50, 1), 0.75);
    utility(engine, bus, -6.0206f);  // halves the sum
    engine.setTrackGain(bus, 0.5f);  // and the group's fader halves it again
    out = engine.renderOffline(0.0, 4000);
    CHECK_NEAR(at(out, -1, 0), 0.1875, 1e-4);
    CHECK_NEAR(at(out, -1, 1), 0.1875, 1e-4);

    // Nested: the group goes into another one, which goes into the master.
    const uint32_t outer = group(engine);
    engine.setTrackOutput(bus, outer);
    engine.setTrackGain(outer, 2.f);
    out = engine.renderOffline(0.0, 4000);
    CHECK_NEAR(at(out, -1, 0), 0.375, 1e-4);
    CHECK_NEAR(at(out, -1, 1), 0.375, 1e-4);

    // Back to the master: the outer group hears nothing.
    engine.setTrackOutput(bus, sub::Engine::kMaster);
    out = engine.renderOffline(0.0, 4000);
    CHECK_NEAR(at(out, -1, 0), 0.1875, 1e-4);
    CHECK_NEAR(at(out, -1, 1), 0.1875, 1e-4);
}

TEST_CASE("routes that would close a cycle are refused") {
    GroupsEngine e;
    auto& engine = e.engine;
    const uint32_t outer = group(engine);
    const uint32_t inner = group(engine, outer);
    const uint32_t track = clipTrack(engine, dcWav(), 0.0, 1.0, inner);
    for (const auto& [from, to] : std::vector<std::pair<uint32_t, uint32_t>>{
             {outer, inner}, {outer, track}, {inner, track}, {track, track}, {outer, outer}}) {
        INFO(std::to_string(from) + " -> " + std::to_string(to));
        CHECK_THROWS_AS(engine.setTrackOutput(from, to), std::invalid_argument);
    }
    CHECK_THROWS_AS(engine.setTrackOutput(track, 999), std::invalid_argument);
    CHECK_EQ(engine.trackOutput(outer), sub::Engine::kMaster);
    const Samples out = engine.renderOffline(0.0, 100);
    CHECK_APPROX(at(out, 50, 0), 0.5);
    CHECK_APPROX(at(out, 50, 1), 0.5);
}

TEST_CASE("removing a group sends what went into it to the master") {
    GroupsEngine e;
    auto& engine = e.engine;
    const uint32_t bus = group(engine);
    const uint32_t track = clipTrack(engine, dcWav(), 0.0, 1.0, bus);
    engine.setTrackMute(bus, true);
    Samples out = engine.renderOffline(0.0, 4000);
    CHECK_APPROX(at(out, -1, 0), 0.0);
    CHECK_APPROX(at(out, -1, 1), 0.0);
    engine.removeTrack(bus);
    CHECK_EQ(engine.trackOutput(track), sub::Engine::kMaster);
    out = engine.renderOffline(0.0, 4000);
    CHECK_APPROX(at(out, -1, 0), 0.5);
    CHECK_APPROX(at(out, -1, 1), 0.5);
}

TEST_CASE("the meters of a group") {
    GroupsEngine e;
    auto& engine = e.engine;
    const uint32_t bus = group(engine);
    clipTrack(engine, dcWav(), 0.0, 1.0, bus);
    std::set<uint32_t> ids;
    for (const auto& m : engine.takeMeters()) ids.insert(m.trackId);
    CHECK(ids.count(sub::Engine::kMaster) == 1);
    CHECK(ids.count(bus) == 1);
}

TEST_CASE("solo and mute across levels") {
    GroupsEngine e;
    auto& engine = e.engine;
    const std::vector<std::string> wavs{level(0.5f), level(0.25f), level(0.125f), level(0.0625f)};
    const uint32_t outer = group(engine);
    const uint32_t inner = group(engine, outer);
    const uint32_t a = clipTrack(engine, wavs[0], 0.0, 1.0, inner);  // 0.5, in the inner group
    const uint32_t b = clipTrack(engine, wavs[1], 0.0, 1.0, inner);  // 0.25, in the inner group
    const uint32_t c = clipTrack(engine, wavs[2], 0.0, 1.0, outer);  // 0.125, in the outer group
    const uint32_t d = clipTrack(engine, wavs[3]);  // 0.0625, on its own

    const auto heard = [&] { return at(engine.renderOffline(0.0, 4000), -1, 0); };  // past the 20 ms smoothing

    CHECK_APPROX(heard(), 0.9375);
    // Soloing a group solos what is in it.
    engine.setTrackSolo(inner, true);
    CHECK_APPROX(heard(), 0.75);
    engine.setTrackSolo(outer, true);
    CHECK_APPROX(heard(), 0.875);
    engine.setTrackSolo(inner, false);
    engine.setTrackSolo(outer, false);
    // Soloing a track keeps its groups heard, but not the other tracks in them.
    engine.setTrackSolo(b, true);
    CHECK_APPROX(heard(), 0.25);
    engine.setTrackSolo(d, true);
    CHECK_APPROX(heard(), 0.3125);
    // A muted group silences what is in it, soloed or not.
    engine.setTrackMute(inner, true);
    CHECK_APPROX(heard(), 0.0625);
    engine.setTrackMute(inner, false);
    engine.setTrackSolo(b, false);
    engine.setTrackSolo(d, false);
    engine.setTrackMute(outer, true);
    CHECK_APPROX(heard(), 0.0625);
    engine.setTrackMute(outer, false);
    // Muting a track in a soloed group leaves the rest of the group.
    engine.setTrackSolo(outer, true);
    engine.setTrackMute(a, true);
    CHECK_APPROX(heard(), 0.375);
    engine.setTrackSolo(c, true);  // one soloed inside a soloed group: the group still plays whole
    CHECK_APPROX(heard(), 0.375);
}

TEST_CASE("compensation in nested groups") {
    // Every click lands on beat 1, whatever latency sits where in the tree.
    requireTestPlugins();
    GroupsEngine e;
    auto& engine = e.engine;
    const std::string wav = clickWav();
    const uint32_t outer = group(engine);
    const uint32_t inner = group(engine, outer);
    const uint32_t deep = clipTrack(engine, wav, 1.0, 1.0, inner);  // in a group in a group
    const uint32_t shallow = clipTrack(engine, wav, 1.0, 1.0, outer);
    const uint32_t alone = clipTrack(engine, wav, 1.0);  // straight into the master

    const auto check = [&](const char* what) {
        INFO(what);
        const Samples out = engine.renderOffline(0.0, 2 * kBeat);
        CHECK(clicks(out) == std::vector<int64_t>{kBeat});
        CHECK_NEAR(at(out, kBeat, 0), 0.75, 1e-4);
    };

    check("no latency");
    latentEffect(engine, deep, 100);  // an unbalanced tree: deep in one branch
    check("deep in one branch");
    latentEffect(engine, inner, 30);  // on a group
    check("on a group");
    latentEffect(engine, shallow, 200);  // more than the other branch
    check("more than the other branch");
    latentEffect(engine, outer, 50);
    latentEffect(engine, alone, 70);
    check("everywhere");
    engine.setTrackOutput(inner, sub::Engine::kMaster);  // the tree changes shape
    check("the tree changed shape");
}

TEST_CASE("group device automation plays in time") {
    // A group's device hears the timeline as late as its slowest input: its
    // automation is as late.
    requireTestPlugins();
    GroupsEngine e;
    auto& engine = e.engine;
    const uint32_t bus = group(engine);
    const uint32_t track = clipTrack(engine, level(0.5f, 2), 0.0, 2.0, bus);
    latentEffect(engine, track, 100);
    latentEffect(engine, bus, 50);
    const uint32_t gain = utility(engine, bus, 0.f);
    const sub::ParamInfo info = paramInfo(engine, gain, "gain");
    const float quiet = info.toNormalized(-60.f), loud = info.toNormalized(0.f);
    const int64_t step = kBeat + 400;  // not on a block boundary of the render (which starts 150 samples early)
    const double stepBeat = static_cast<double>(step) / kBeat;
    engine.setTrackAutomation(bus, {{gain, "gain", {{0.0, quiet, 0.f}, {stepBeat, quiet, 0.f}, {stepBeat, loud, 0.f}}}});
    const Samples out = channel(engine.renderOffline(0.0, step + 2000), 0);
    CHECK(maxAbs(slice(out, step - 300, step)) < 0.001);  // still at -60 dB right up to the step
    CHECK(out[step + 40] > 0.01);  // and rising from it at once (smoothed)
}

TEST_CASE("group volume automation plays in time") {
    // A group's fader comes after the latency of what feeds it and of its own devices.
    requireTestPlugins();
    GroupsEngine e;
    auto& engine = e.engine;
    const uint32_t bus = group(engine);
    const uint32_t track = clipTrack(engine, level(0.5f, 2), 0.0, 2.0, bus);
    latentEffect(engine, track, 100);
    latentEffect(engine, bus, 50);
    clipTrack(engine, dcWav());  // beside it, delayed to line up with the group
    const int64_t step = kBeat + 400;
    const double stepBeat = static_cast<double>(step) / kBeat;
    engine.setTrackAutomation(bus, {{0, "volume", {{0.0, 0.f, 0.f}, {stepBeat, 0.f, 0.f}, {stepBeat, 1.f, 0.f}}}});
    const Samples out = channel(engine.renderOffline(0.0, step + 2000), 0);
    Samples before = slice(out, step - 300, step);
    for (float& x : before) x -= 0.5f;
    CHECK(maxAbs(before) < 1e-4);  // only the other track up to the step
    CHECK(out[step + 40] > 0.6f);
}
