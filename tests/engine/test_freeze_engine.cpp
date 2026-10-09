// Freezing in the engine: one track's signal before its fader rendered offline
// (lined up with the timeline, solo ignored, into a WAV file with its tail), and
// frozen tracks playing their clips through their faders without their devices,
// notes or what goes into them; what goes only into frozen tracks isn't rendered.

#include <cmath>

#include "Engine.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {


struct FreezeEngine {
    sub::Engine engine;
    FreezeEngine() { engine.setClipFadeMs(0); }
};

// Renders the track to `path` and plays that instead, frozen (as the app does).
int64_t freeze(sub::Engine& engine, uint32_t track, const std::filesystem::path& path, double endBeat,
               double tailSeconds = 0.0) {
    const int64_t written = engine.renderTrackToWav(track, path.string(), 0.0, endBeat, tailSeconds);
    engine.loadSource(path.string());
    engine.setTrackClips(track, {clip(path.string(), 0.0, static_cast<double>(written) / kSampleRate)});
    engine.setTrackNotes(track, {});
    engine.setTrackFrozen(track, true);
    return written;
}

std::vector<int64_t> clicks(const Samples& out) { return above(channel(out, 0), 1e-6); }

// A click of 0.25 at sample 1000 of a second.
std::string level(float value, int seconds = 1) { return makeWav(full(static_cast<size_t>(seconds) * kSampleRate * 2, value), 2); }

}  // namespace

// --- Rendering one track ------------------------------------------------------------

TEST_CASE("a track renders after its devices, before its fader") {
    FreezeEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, level(0.5f));
    utility(engine, track, -6.0206f);  // halves it
    engine.setTrackGain(track, 0.1f);
    engine.setTrackPan(track, 1.f);
    engine.setTrackMute(track, true);
    const Samples out = engine.renderTrackOffline(track, 0.0, 4000);
    CHECK_EQ(out.size(), size_t{4000 * 2});
    CHECK_ALLCLOSE(frames(out, -100), 0.25, 1e-7, 1e-4);  // (once the utility's gain has settled)
}

TEST_CASE("a group renders its bus") {
    FreezeEngine e;
    auto& engine = e.engine;
    const uint32_t bus = engine.addTrack();
    clipTrack(engine, level(0.5f), 0.0, 1.0, bus);
    const uint32_t quiet = clipTrack(engine, level(0.25f), 0.0, 1.0, bus);
    engine.setTrackGain(quiet, 0.5f);  // what goes into the bus: after the tracks' faders
    utility(engine, bus, -6.0206f);
    CHECK_ALLCLOSE(frames(engine.renderTrackOffline(bus, 0.0, 4000), -100), (0.5 + 0.125) / 2, 1e-7, 1e-4);
}

TEST_CASE("solo is ignored") {
    FreezeEngine e;
    auto& engine = e.engine;
    const uint32_t bus = engine.addTrack();
    clipTrack(engine, level(0.5f), 0.0, 1.0, bus);
    const uint32_t other = clipTrack(engine, level(0.25f));
    engine.setTrackSolo(other, true);  // would leave the group silent
    CHECK_ALLCLOSE(engine.renderTrackOffline(bus, 0.0, 100), 0.5, 1e-7, 1e-4);
    CHECK_ALLCLOSE(engine.renderOffline(0.0, 100), 0.25, 1e-7, 1e-4);  // (playing, it counts)
}

TEST_CASE("an unknown track is refused") {
    FreezeEngine e;
    auto& engine = e.engine;
    CHECK_THROWS_AS(engine.renderTrackOffline(99, 0.0, 10), std::invalid_argument);
    CHECK_THROWS_AS(engine.renderTrackToWav(engine.addTrack(), (tempDir() / "x.wav").string(), 1.0, 1.0, 0.0),
                    std::invalid_argument);
}

TEST_CASE("the render lines up with the timeline") {
    requireTestPlugins();
    FreezeEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, clickWav(0.25, kSampleRate, 1000));
    latentEffect(engine, track, 300);
    CHECK(clicks(engine.renderTrackOffline(track, 0.0, 4000)) == std::vector<int64_t>{1000});
}

TEST_CASE("a group's render lines up with the timeline") {
    requireTestPlugins();
    FreezeEngine e;
    auto& engine = e.engine;
    const uint32_t bus = engine.addTrack();
    const uint32_t late = clipTrack(engine, clickWav(0.25, kSampleRate, 1000), 0.0, 1.0, bus);
    latentEffect(engine, late, 200);  // the bus hears its tracks 200 late
    latentEffect(engine, bus, 100);
    CHECK(clicks(engine.renderTrackOffline(bus, 0.0, 4000)) == std::vector<int64_t>{1000});
}

TEST_CASE("the WAV has the range and its tail") {
    FreezeEngine e;
    auto& engine = e.engine;
    Samples tone(kSampleRate * 2, 0.f);
    std::fill(tone.begin(), tone.begin() + kSampleRate, 0.5f);  // half a second of sound in a one-second clip
    const uint32_t track = clipTrack(engine, makeWav(tone, 2));
    const auto path = tempDir() / "frozen.wav";
    const int64_t written = engine.renderTrackToWav(track, path.string(), 0.0, 0.5, 2.0);  // a quarter second, then on
    CHECK_EQ(written, int64_t{kSampleRate / 2});  // the tail, until it fell silent
    const auto source = engine.loadSource(path.string());
    CHECK_EQ(source->frames(), written);
    CHECK_EQ(source->channels(), 2u);
    for (uint32_t c = 0; c < source->channels(); ++c) {
        const Samples samples(source->channelData(c), source->channelData(c) + written);
        CHECK_ALLCLOSE(samples, 0.5, 1e-7, 1e-4);
    }
}

TEST_CASE("the WAV keeps what is louder than full scale") {
    FreezeEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, level(0.5f));
    utility(engine, track, 12.f);  // about 2.0
    const auto path = tempDir() / "loud.wav";
    engine.renderTrackToWav(track, path.string(), 0.0, 1.0, 0.0);
    const auto source = engine.loadSource(path.string());
    float peak = 0.f;
    for (uint32_t c = 0; c < source->channels(); ++c)
        for (int64_t i = 4000; i < 4100; ++i) peak = std::max(peak, source->channelData(c)[i]);
    CHECK(peak > 1.5f);  // 32-bit float: not clipped
}

// --- Frozen tracks -----------------------------------------------------------------

TEST_CASE("a frozen track plays its render through its fader") {
    FreezeEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, level(0.5f));
    const uint32_t effect = utility(engine, track, -6.0206f);
    engine.setTrackGain(track, 0.5f);
    engine.renderOffline(0.0, 4000);  // (a new utility glides to its gain once)
    const Samples before = engine.renderOffline(0.0, kSampleRate / 2);
    freeze(engine, track, tempDir() / "f.wav", 1.0);
    CHECK(engine.trackFrozen(track));
    CHECK_ALLCLOSE(engine.renderOffline(0.0, kSampleRate / 2), before, 1e-7, 1e-6);
    // Its devices are left out (it plays what they made); its fader stays live.
    setParam(engine, effect, "gain", 0.f);
    CHECK_ALLCLOSE(frames(engine.renderOffline(0.0, 4000), -100), 0.125, 1e-7, 1e-4);
    engine.setTrackGain(track, 1.f);
    CHECK_ALLCLOSE(frames(engine.renderOffline(0.0, 4000), -100), 0.25, 1e-7, 1e-4);
    engine.setTrackFrozen(track, false);  // its devices again (on what it plays now)
    CHECK_ALLCLOSE(frames(engine.renderOffline(0.0, 4000), -100), 0.25, 1e-7, 1e-4);
}

TEST_CASE("a frozen MIDI track plays no notes") {
    FreezeEngine e;
    auto& engine = e.engine;
    const uint32_t track = engine.addTrack();
    engine.addBuiltinProcessor(engine.trackChain(track), "synth", -1);
    engine.setTrackNotes(track, {{0.0, 1.0, 60, 100}});
    CHECK(maxAbs(engine.renderOffline(0.0, kBeat)) > 0.01);
    engine.setTrackClips(track, {});
    engine.setTrackFrozen(track, true);  // without a render: nothing at all
    CHECK_EQ(maxAbs(engine.renderOffline(0.0, kBeat)), 0.0);
}

TEST_CASE("a frozen group doesn't hear its tracks") {
    FreezeEngine e;
    auto& engine = e.engine;
    const uint32_t bus = engine.addTrack();
    const uint32_t child = clipTrack(engine, level(0.5f), 0.0, 1.0, bus);
    utility(engine, bus, -6.0206f);
    freeze(engine, bus, tempDir() / "bus.wav", 1.0);
    CHECK_ALLCLOSE(frames(engine.renderOffline(0.0, 4000), -100), 0.25, 1e-7, 1e-4);
    engine.setTrackGain(child, 0.f);  // inside the frozen audio
    CHECK_ALLCLOSE(frames(engine.renderOffline(0.0, 4000), -100), 0.25, 1e-7, 1e-4);
    engine.setTrackGain(bus, 0.5f);  // live
    CHECK_ALLCLOSE(frames(engine.renderOffline(0.0, 4000), -100), 0.125, 1e-7, 1e-4);
}

TEST_CASE("what goes only into frozen tracks isn't rendered") {
    FreezeEngine e;
    auto& engine = e.engine;
    const uint32_t bus = engine.addTrack();
    const uint32_t inner = engine.addTrack();
    engine.setTrackOutput(inner, bus);
    const uint32_t child = clipTrack(engine, level(0.5f), 0.0, 1.0, inner);
    utility(engine, child, 0.f);
    const uint32_t ret = engine.addTrack();
    const uint32_t sender = clipTrack(engine, level(0.25f), 0.0, 1.0, bus);
    engine.setTrackSend(sender, ret, 1.f, false);  // also heard elsewhere: rendered
    freeze(engine, bus, tempDir() / "bus.wav", 1.0);
    // The bus (both tracks, frozen), and the send into the return, live.
    CHECK_ALLCLOSE(engine.renderOffline(0.0, 100), 0.75 + 0.25, 1e-7, 1e-4);
    engine.setTrackGain(sender, 0.f);
    CHECK_ALLCLOSE(engine.renderOffline(0.0, 100), 0.75, 1e-7, 1e-4);
    engine.setTrackFrozen(bus, false);  // all of it again
    CHECK_ALLCLOSE(engine.renderOffline(0.0, 100), 0.5 + 0.75, 1e-7, 1e-4);
}

TEST_CASE("sends tap the frozen audio") {
    FreezeEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, level(0.5f));
    utility(engine, track, -6.0206f);
    const uint32_t ret = engine.addTrack();
    engine.setTrackOutput(track, ret);  // (all of it through the return)
    const uint32_t pre = engine.addTrack();
    engine.setTrackSend(track, pre, 1.f, true);
    freeze(engine, track, tempDir() / "t.wav", 1.0);
    engine.setTrackGain(track, 0.5f);
    // Into the return after the fader (0.125), the pre-fader send before it (0.25).
    CHECK_ALLCLOSE(frames(engine.renderOffline(0.0, 4000), -100), 0.125 + 0.25, 1e-7, 1e-4);
}

TEST_CASE("a frozen track adds no latency") {
    // Were its devices' latency still counted, everything else would wait for
    // it, and its frozen audio (on time) would come that much early.
    requireTestPlugins();
    FreezeEngine e;
    auto& engine = e.engine;
    const std::string wav = clickWav(0.25, kSampleRate, 1000);
    const uint32_t track = clipTrack(engine, wav);
    latentEffect(engine, track, 300);
    clipTrack(engine, wav, 1.0);
    CHECK(clicks(engine.renderOffline(0.0, kBeat + 4000)) == (std::vector<int64_t>{1000, kBeat + 1000}));
    freeze(engine, track, tempDir() / "t.wav", 1.0);
    CHECK(clicks(engine.renderOffline(0.0, kBeat + 4000)) == (std::vector<int64_t>{1000, kBeat + 1000}));
}

TEST_CASE("a frozen group lines up with nothing inside it") {
    // Its bus doesn't hear its tracks, so it isn't as late as they are.
    requireTestPlugins();
    FreezeEngine e;
    auto& engine = e.engine;
    const std::string wav = clickWav(0.25, kSampleRate, 1000);
    const uint32_t bus = engine.addTrack();
    const uint32_t child = clipTrack(engine, wav, 0.0, 1.0, bus);
    latentEffect(engine, child, 500);  // would make everything else 500 late
    clipTrack(engine, wav, 1.0);
    freeze(engine, bus, tempDir() / "bus.wav", 1.0);
    const Samples out = engine.renderOffline(0.0, kBeat + 4000);
    CHECK(clicks(out) == (std::vector<int64_t>{1000, kBeat + 1000}));
}
