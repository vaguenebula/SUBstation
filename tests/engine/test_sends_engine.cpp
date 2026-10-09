// Sends in the engine: a track's signal also going into return tracks, after
// its fader or before it; delay compensation per edge (a track going to places
// of different latency is delayed differently on each); cycles across outputs
// and sends; and solo and mute across sends. Rendered offline, so no audio
// device is needed.

#include <algorithm>
#include <set>
#include <tuple>

#include "Engine.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"
#include "harness/TaskGraphOrder.h"

using namespace subtest;

namespace {


struct SendsEngine {
    sub::Engine engine;
    SendsEngine() { engine.setClipFadeMs(0); }
};

double level(sub::Engine& engine) { return at(engine.renderOffline(0.0, 4000), -1, 0); }  // past a Utility's ramp

// A click of 0.25 at the start of 1000 samples.
std::vector<int64_t> clicks(const Samples& out) { return above(channel(out, 0), 1e-6); }

std::string levelWav(float value, int seconds = 1) {
    return makeWav(full(static_cast<size_t>(seconds) * kSampleRate * 2, value), 2);
}

}  // namespace

TEST_CASE("post- and pre-fader sends at their levels") {
    SendsEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, dcWav());
    const uint32_t ret = engine.addTrack();  // a return: no clips, fed by sends
    engine.setTrackGain(track, 0.5f);
    engine.setTrackSend(track, ret, 0.5f, false);
    const auto sends = engine.trackSends(track);
    REQUIRE(sends.size() == 1);
    CHECK_EQ(sends[0].trackId, ret);
    CHECK_EQ(sends[0].gain, 0.5f);
    CHECK(!sends[0].preFader);
    CHECK_APPROX(level(engine), 0.25 + 0.125);  // after the fader: 0.5 * 0.5 * 0.5
    engine.setTrackSend(track, ret, 0.5f, true);
    CHECK_APPROX(level(engine), 0.25 + 0.25);  // before it: the track's fader doesn't count
    engine.setTrackGain(track, 0.f);
    CHECK_APPROX(level(engine), 0.25);  // only the return
    engine.setTrackSend(track, ret, 1.f, true);  // a new level alone
    CHECK_APPROX(level(engine), 0.5);
    engine.setTrackGain(ret, 0.5f);  // the return's own fader
    CHECK_APPROX(level(engine), 0.25);
    engine.removeTrackSend(track, ret);
    CHECK(engine.trackSends(track).empty());
    CHECK_APPROX(level(engine), 0.0);
}

TEST_CASE("a return effect processes the sum of what is sent to it") {
    SendsEngine e;
    auto& engine = e.engine;
    const uint32_t a = clipTrack(engine, levelWav(0.5f));
    const uint32_t b = clipTrack(engine, levelWav(0.25f));
    const uint32_t ret = engine.addTrack();
    for (const uint32_t track : {a, b}) {
        engine.setTrackGain(track, 0.f);  // only what is sent (pre-fader) is heard
        engine.setTrackSend(track, ret, 1.f, true);
    }
    CHECK_APPROX(level(engine), 0.75);
    utility(engine, ret, -6.0206f);  // halves the sum
    CHECK_NEAR(level(engine), 0.375, 1e-4);
    // A return sending on into another return, as a track does.
    const uint32_t other = engine.addTrack();
    engine.setTrackGain(ret, 0.f);
    engine.setTrackSend(ret, other, 0.5f, true);
    CHECK_NEAR(level(engine), 0.1875, 1e-4);
}

TEST_CASE("a muted track sends nothing") {
    SendsEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, dcWav());
    const uint32_t post = engine.addTrack(), pre = engine.addTrack();
    engine.setTrackSend(track, post, 1.f, false);
    engine.setTrackSend(track, pre, 1.f, true);
    CHECK_APPROX(level(engine), 1.5);
    engine.setTrackMute(track, true);
    CHECK_APPROX(level(engine), 0.0);  // pre-fader sends too
}

TEST_CASE("cycles are refused across sends, outputs and returns") {
    SendsEngine e;
    auto& engine = e.engine;
    const uint32_t group = engine.addTrack();
    const uint32_t track = clipTrack(engine, dcWav(), 0.0, 1.0, group);
    const uint32_t a = engine.addTrack(), b = engine.addTrack();  // returns
    engine.setTrackSend(group, a, 1.f, false);
    engine.setTrackSend(a, b, 1.f, false);
    for (const auto& [from, to] :
         std::vector<std::pair<uint32_t, uint32_t>>{{b, a}, {a, a}, {b, group}, {a, track}, {b, track}, {track, track}}) {
        INFO(std::to_string(from) + " -> " + std::to_string(to));
        CHECK_THROWS_AS(engine.setTrackSend(from, to, 1.f, false), std::invalid_argument);
    }
    CHECK_THROWS_AS(engine.setTrackOutput(b, a), std::invalid_argument);  // a sends to b
    CHECK_THROWS_AS(engine.setTrackOutput(a, track), std::invalid_argument);  // the track goes (through its group) into a
    CHECK_THROWS_AS(engine.setTrackSend(track, sub::Engine::kMaster, 1.f, false), std::invalid_argument);
    CHECK_THROWS_AS(engine.setTrackSend(track, 999, 1.f, false), std::invalid_argument);
    engine.setTrackSend(track, b, 0.5f, false);  // a second way into b is no cycle
    CHECK_APPROX(level(engine), 0.5 + 0.5 + 0.5 + 0.25);  // group, a, b (from a and the track)
}

TEST_CASE("removing a return removes the sends to it") {
    SendsEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, dcWav());
    const uint32_t ret = engine.addTrack(), other = engine.addTrack();
    engine.setTrackSend(track, ret, 1.f, false);
    engine.setTrackSend(track, other, 0.5f, false);
    engine.removeTrack(ret);
    const auto sends = engine.trackSends(track);
    REQUIRE(sends.size() == 1);
    CHECK_EQ(sends[0].trackId, other);
    CHECK_APPROX(level(engine), 0.75);
}

TEST_CASE("solo and mute across sends") {
    SendsEngine e;
    auto& engine = e.engine;
    const std::vector<std::string> wavs{levelWav(0.5f), levelWav(0.25f), levelWav(0.0625f)};
    const uint32_t group = engine.addTrack();
    const uint32_t a = clipTrack(engine, wavs[0], 0.0, 1.0, group);  // 0.5, in the group
    const uint32_t b = clipTrack(engine, wavs[1]);  // 0.25
    const uint32_t d = clipTrack(engine, wavs[2]);  // 0.0625, sends nothing
    const uint32_t ret = engine.addTrack(), other = engine.addTrack();
    engine.setTrackSend(group, ret, 1.f, false);
    engine.setTrackSend(b, ret, 1.f, false);
    engine.setTrackSend(b, other, 0.5f, false);
    engine.setTrackSend(ret, other, 0.5f, false);
    // group 0.5, b 0.25, d 0.0625, ret 0.75, other 0.125 (from b) + 0.375 (from ret)
    CHECK_APPROX(level(engine), 2.0625);

    const auto solo = [&](std::vector<uint32_t> soloed) {
        for (const uint32_t track : {group, a, b, d, ret, other})
            engine.setTrackSolo(track, std::find(soloed.begin(), soloed.end(), track) != soloed.end());
        return level(engine);
    };

    // Solo in place: a soloed track is heard with everything it goes into.
    CHECK_APPROX(solo({a}), 0.5 + 0.5 + 0.25);  // its group, ret (from the group), other (from ret)
    CHECK_APPROX(solo({b}), 0.25 + 0.25 + 0.125 + 0.125);  // itself, ret, other (from it and from ret)
    CHECK_APPROX(solo({d}), 0.0625);
    // A soloed return keeps what sends to it sending, but nothing else of theirs.
    CHECK_APPROX(solo({ret}), 0.75 + 0.375);  // ret, and other (from ret)
    CHECK_APPROX(solo({other}), 0.5);  // from b and from ret (which hears the group and b)
    CHECK_APPROX(solo({ret, d}), 0.75 + 0.375 + 0.0625);
    // Mute: a muted track sends nothing, even into a soloed return.
    engine.setTrackMute(a, true);
    CHECK_APPROX(solo({ret}), 0.25 + 0.125);
    engine.setTrackMute(a, false);
    engine.setTrackMute(ret, true);
    CHECK_APPROX(solo({a}), 0.5);  // into the group; the return it reaches is muted
    CHECK_APPROX(solo({}), 0.5 + 0.25 + 0.0625 + 0.125);
}

TEST_CASE("compensation across sends") {
    // Every click lands on beat 1 at the master, whatever latency sits on the
    // returns, the tracks sending, or what they send through.
    requireTestPlugins();
    SendsEngine e;
    auto& engine = e.engine;
    const std::string wav = clickWav(0.25);
    const uint32_t track = clipTrack(engine, wav, 1.0);
    clipTrack(engine, wav, 1.0);  // beside it, straight into the master
    const uint32_t nearReturn = engine.addTrack(), farReturn = engine.addTrack();  // returns
    engine.setTrackSend(track, nearReturn, 1.f, false);
    engine.setTrackSend(track, farReturn, 1.f, false);

    const auto check = [&](double expected, const char* what) {
        INFO(what);
        const Samples out = engine.renderOffline(0.0, 2 * kBeat);
        CHECK(clicks(out) == std::vector<int64_t>{kBeat});
        CHECK_NEAR(at(out, kBeat, 0), expected, 1e-4);
    };

    check(1.0, "no latency");
    latentEffect(engine, farReturn, 300);  // a latent return: the rest wait for it
    check(1.0, "a latent return");
    latentEffect(engine, nearReturn, 50);  // one track into two returns of different latency
    check(1.0, "two returns of different latency");
    latentEffect(engine, track, 120);  // a latent track sending
    check(1.0, "a latent track sending");
    engine.setTrackSend(nearReturn, farReturn, 1.f, false);  // a return into another (with more latency)
    check(1.25, "a return into another");
    latentEffect(engine, sub::Engine::kMaster, 40);
    check(1.25, "a latent master");
    const uint32_t group = engine.addTrack();  // the track goes on through a group of its own
    latentEffect(engine, group, 70);
    engine.setTrackOutput(track, group);
    check(1.25, "through a group");
}

TEST_CASE("a pre-fader tap after a latent device") {
    requireTestPlugins();
    SendsEngine e;
    auto& engine = e.engine;
    const std::string wav = clickWav(0.25);
    const uint32_t track = clipTrack(engine, wav, 1.0);
    const uint32_t ret = engine.addTrack();
    latentEffect(engine, track, 200);
    engine.setTrackSend(track, ret, 1.f, true);
    engine.setTrackGain(track, 0.5f);
    clipTrack(engine, wav, 1.0);  // beside them, delayed to line up
    const Samples out = engine.renderOffline(0.0, 2 * kBeat);
    CHECK(clicks(out) == std::vector<int64_t>{kBeat});
    CHECK_NEAR(at(out, kBeat, 0), 0.125 + 0.25 + 0.25, 1e-4);
}

TEST_CASE("send automation plays in time") {
    // A send's level is applied where the return hears it: as late as the
    // return hears its inputs.
    requireTestPlugins();
    SendsEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, levelWav(0.5f, 2), 0.0, 2.0);
    const uint32_t ret = engine.addTrack();
    latentEffect(engine, track, 100);
    latentEffect(engine, ret, 50);
    const uint32_t late = clipTrack(engine, clickWav(0.25));
    latentEffect(engine, late, 400);  // so that the return's edge to the master is delayed too
    engine.setTrackGain(track, 0.f);  // only the send is heard
    engine.setTrackSend(track, ret, 1.f, true);
    const int64_t step = kBeat + 400;
    const double stepBeat = static_cast<double>(step) / kBeat;
    engine.setTrackAutomation(
        track, {{0, "send:" + std::to_string(ret), {{0.0, 0.f, 0.f}, {stepBeat, 0.f, 0.f}, {stepBeat, 1.f, 0.f}}}});
    const Samples out = channel(engine.renderOffline(0.0, step + 2000), 0);
    CHECK(maxAbs(slice(out, step - 300, step)) < 1e-6);  // silent right up to the step
    CHECK(out[step + 40] > 0.3);  // and loud from it (+6 dB at the top of the lane)
    // Without its envelope, the send's own level counts again.
    engine.setTrackAutomation(track, {});
    CHECK_APPROX(at(engine.renderOffline(0.0, 4000), -1, 0), 0.5);
}

TEST_CASE("the meters of a return") {
    SendsEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, dcWav());
    const uint32_t ret = engine.addTrack();
    engine.setTrackSend(track, ret, 1.f, false);
    std::set<uint32_t> ids;
    for (const auto& m : engine.takeMeters()) ids.insert(m.trackId);
    CHECK(ids.count(sub::Engine::kMaster) == 1);
    CHECK(ids.count(ret) == 1);
}

TEST_CASE("ranks follow the longest path through sends") {
    // A node going to several places ranks by the heaviest of them.
    // 0 -> 2 (a group) and 3 (a return); 1 -> 3; 2 -> master; 3 -> 4 (a return) -> master
    const std::vector<std::vector<int>> destinations{{2, 3}, {3}, {}, {4}, {}};
    const std::vector<float> costs{1.f, 1.f, 1.f, 2.f, 5.f};
    const auto [order, ranks] = taskGraphOrder(destinations, costs);
    const std::vector<double> expected{8.0, 8.0, 1.0, 7.0, 5.0};
    REQUIRE(ranks.size() == expected.size());
    for (size_t i = 0; i < ranks.size(); ++i) CHECK_APPROX(ranks[i], expected[i]);
    CHECK(order == (std::vector<int>{0, 1}));
    CHECK_THROWS_AS(taskGraphOrder(std::vector<std::vector<int>>{{1}, {0}}, {1.f, 1.f}), std::invalid_argument);
}
