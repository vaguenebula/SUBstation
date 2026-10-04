// MIDI in the engine: note scheduling and the built-in Synth.
// Rendered offline, so no audio device is needed (but for one test that plays live).

#include <chrono>
#include <cmath>
#include <map>
#include <thread>

#include "Engine.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

enum { WAVE, ATTACK, DECAY, SUSTAIN, RELEASE, CUTOFF, RESONANCE, VOLUME };
const double kSineRms = 0.25 * 100 / 127 / std::sqrt(2.0);  // one voice at the default velocity (100)

// Opens an output device (the default, else any that opens) or skips the test.
void openAnyOutput(sub::Engine& engine) {
    std::vector<std::string> names{""};
    try {
        for (const auto& d : engine.devices(sub::kDefaultDriver)) names.push_back(d.name);
    } catch (const std::exception&) {
    }
    for (const std::string& name : names) {
        sub::DeviceConfig config;
        config.driver = sub::kDefaultDriver;
        config.name = name;
        config.bufferFrames = 256;
        try {
            engine.openDevice(config);
            return;
        } catch (const std::runtime_error&) {
        }
    }
    SKIP("no audio output device");
}

// Waits until the playing transport has advanced `beats`.
void waitBeats(sub::Engine& engine, double beats, double timeout = 5.0) {
    const double target = engine.positionBeats() + beats;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
    while (engine.positionBeats() < target) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);  // (or the audio device stopped)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// A track with a Synth (sine, no filtering, short release unless `params` say
// otherwise) playing `notes`: (track, synth).
std::pair<uint32_t, uint32_t> synthTrack(sub::Engine& engine, const std::vector<sub::NoteDesc>& notes,
                                         const std::map<int, float>& params = {}) {
    const uint32_t track = engine.addTrack();
    const uint32_t synth = engine.addBuiltinProcessor(engine.trackChain(track), "synth", -1);
    std::map<int, float> settings{{WAVE, 0.f}, {CUTOFF, 20000.f}, {RELEASE, 5.f}, {SUSTAIN, 100.f}};
    for (const auto& [index, value] : params) settings[index] = value;
    for (const auto& [index, value] : settings) engine.setProcessorParam(synth, index, value);
    engine.setTrackNotes(track, notes);
    return {track, synth};
}

float trackMeter(sub::Engine& engine, uint32_t track) {
    float level = 0.f;
    for (const auto& m : engine.takeMeters())
        if (m.trackId == track) level = std::max(level, m.left);
    return level;
}

}  // namespace

TEST_CASE("the synth plays notes on their beats") {
    sub::Engine engine;
    synthTrack(engine, {{1.0, 2.0, 69}});  // A3 (440 Hz) from beat 1 to beat 3
    const Samples out = engine.renderOffline(0.0, 5 * kBeat);
    CHECK(allEqual(frames(out, 0, kBeat), 0.0));
    CHECK(maxAbs(frames(out, kBeat, kBeat + 480)) > 0.05);  // within 10 ms (3 ms attack)
    CHECK_ARRAY_EQUAL(channel(out, 0), channel(out, 1));  // mono on both channels
    const Samples left = channel(out, 0);
    CHECK_APPROX_REL(dominantFreq(slice(left, kBeat + 4800, kBeat + 4800 + 16384)), 440.0, 1e-3);
    const Samples held = slice(left, 2 * kBeat, 3 * kBeat - 100);
    CHECK_APPROX_REL(rms(held), kSineRms, 0.02);  // sustain 100 %
    CHECK(maxAbs(frames(out, 3 * kBeat + kSampleRate / 50)) < 1e-4);  // released: 5 ms to -60 dB
}

TEST_CASE("notes are in beats and follow the tempo") {
    sub::Engine engine;
    engine.setTempo(60.0);
    synthTrack(engine, {{2.0, 1.0, 60}});
    const Samples out = engine.renderOffline(0.0, 4 * kSampleRate);
    const auto sounding = above(channel(out, 0), 1e-6);
    REQUIRE(!sounding.empty());
    CHECK(2 * kSampleRate <= sounding[0]);
    CHECK(sounding[0] < 2 * kSampleRate + 10);
    CHECK(maxAbs(frames(out, 3 * kSampleRate + kSampleRate / 50)) < 1e-4);
}

TEST_CASE("velocity sets the level") {
    sub::Engine engine;
    const auto [track, synth] = synthTrack(engine, {{0.0, 2.0, 60, 127}});
    const double loud = rms(channel(frames(engine.renderOffline(0.0, kBeat), kBeat / 2), 0));
    engine.setTrackNotes(track, {{0.0, 2.0, 60, 64}});
    const double soft = rms(channel(frames(engine.renderOffline(0.0, kBeat), kBeat / 2), 0));
    CHECK_APPROX_REL(soft / loud, 64 / 127.0, 0.01);
}

TEST_CASE("chords and repeated keys") {
    sub::Engine engine;
    // A note ending where the next one on the same key starts doesn't cut it short.
    const auto [track, synth] = synthTrack(engine, {{0.0, 1.0, 60}, {1.0, 1.0, 60}});
    const Samples out = engine.renderOffline(0.0, 3 * kBeat);
    CHECK_APPROX_REL(rms(channel(frames(out, kBeat + 1000, 2 * kBeat - 100), 0)), kSineRms, 0.02);
    CHECK(maxAbs(frames(out, 2 * kBeat + kSampleRate / 50)) < 1e-4);
    // Two keys at once sound together: the level adds up.
    engine.setTrackNotes(track, {{0.0, 1.0, 60}, {0.0, 1.0, 67}});
    const Samples chord = channel(frames(engine.renderOffline(0.0, kBeat), kBeat / 2), 0);
    CHECK_APPROX_REL(rms(chord), std::sqrt(2.0) * kSineRms, 0.05);
}

TEST_CASE("offline renders leave no hanging notes") {
    sub::Engine engine;
    synthTrack(engine, {{0.0, 16.0, 60}});
    const Samples whole = engine.renderOffline(0.0, 2 * kBeat);
    engine.renderOffline(0.0, kBeat / 2);  // stops in the middle of the note...
    // ...which must not keep sounding: rendering from later on (past the note-on) is silent.
    CHECK_EQ(maxAbs(engine.renderOffline(4.0, kBeat)), 0.0);
    CHECK_ARRAY_EQUAL(engine.renderOffline(0.0, 2 * kBeat), whole);
}

TEST_CASE("a loop's wrap releases and retriggers notes") {
    sub::Engine engine;
    synthTrack(engine, {{1.0, 4.0, 60}}, {{RELEASE, 1.f}});
    engine.setLoop(true, 1.0, 2.0);  // shorter than the note
    const Samples out = channel(engine.renderOffline(1.0, 3 * kBeat, true), 0);
    // Each pass restarts the note (fresh attack) and ends it at the wrap.
    for (const int64_t wrap : {kBeat, 2 * kBeat}) {
        INFO("wrap at " + std::to_string(wrap));
        CHECK(maxAbs(slice(out, wrap - 40, wrap)) > 0.1);  // still sounding right before the wrap
        CHECK(maxAbs(slice(out, wrap + 60, wrap + 70)) < maxAbs(slice(out, wrap + 2000, wrap + 3000)) / 2);
    }
    CHECK_ALLCLOSE(slice(out, kBeat, 2 * kBeat), slice(out, 2 * kBeat), 1e-7, 1e-4);
}

TEST_CASE("an instrument's output goes through the chain") {
    sub::Engine engine;
    const auto [track, synth] = synthTrack(engine, {{0.0, 2.0, 60}});
    const uint32_t utility = engine.addBuiltinProcessor(engine.trackChain(track), "utility", -1);
    engine.setProcessorParam(utility, 0, -6.0206f);  // half the level
    CHECK_APPROX_REL(rms(channel(frames(engine.renderOffline(0.0, kBeat), kBeat / 2), 0)), kSineRms / 2, 0.02);
    engine.setProcessorEnabled(synth, false);
    CHECK_EQ(maxAbs(engine.renderOffline(0.0, kBeat)), 0.0);
    engine.setProcessorEnabled(synth, true);
    CHECK_APPROX_REL(rms(channel(frames(engine.renderOffline(0.0, kBeat), kBeat / 2), 0)), kSineRms / 2, 0.02);
}

TEST_CASE("notes without an instrument are silent") {
    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    engine.setTrackNotes(track, {{0.0, 1.0, 60}});
    CHECK_EQ(maxAbs(engine.renderOffline(0.0, kBeat)), 0.0);
    engine.addBuiltinProcessor(engine.trackChain(track), "synth", -1);
    CHECK(maxAbs(engine.renderOffline(0.0, kBeat)) > 0.05);
}

TEST_CASE("the cutoff filters the saw") {
    sub::Engine engine;
    const auto [track, synth] = synthTrack(engine, {{0.0, 2.0, 45}}, {{WAVE, 2.f}});  // saw, 110 Hz

    const auto highBandEnergy = [&] {
        const Samples out = channel(frames(engine.renderOffline(0.0, kBeat), kBeat / 2), 0);
        const std::vector<double> s = spectrum(out);
        double high = 0.0, all = 0.0;
        for (size_t k = 0; k < s.size(); ++k) {
            const double power = s[k] * s[k];
            all += power;
            if (static_cast<double>(k) * kSampleRate / static_cast<double>(out.size()) > 2000) high += power;
        }
        return high / all;
    };

    const double open = highBandEnergy();
    engine.setProcessorParam(synth, CUTOFF, 300.f);
    CHECK(highBandEnergy() < open / 20);  // 12 dB/octave
}

TEST_CASE("the synth's parameters") {
    sub::Engine engine;
    const uint32_t track = engine.addTrack();
    const uint32_t synth = engine.addBuiltinProcessor(engine.trackChain(track), "synth", -1);
    const auto params = engine.processorParams(synth);
    CHECK(paramIds(params) == (std::vector<std::string>{"wave", "attack", "decay", "sustain", "release", "cutoff",
                                                        "resonance", "volume"}));
    REQUIRE(params.size() == 8);
    CHECK(params[WAVE].valueLabels == (std::vector<std::string>{"Sine", "Triangle", "Saw", "Square"}));
    std::vector<bool> logScale;
    for (const auto& p : params) logScale.push_back(p.logScale);
    CHECK(logScale == (std::vector<bool>{false, true, true, false, true, true, false, false}));
    engine.setProcessorParam(synth, CUTOFF, 1e9f);
    CHECK_EQ(engine.processorParam(synth, CUTOFF), 20000.f);  // clamped
}

TEST_CASE("preview notes need a running device") {
    sub::Engine engine;
    const auto [track, synth] = synthTrack(engine, {});
    engine.previewNote(track, 60, 100);  // no device: dropped, so it can't hang later
    CHECK_EQ(maxAbs(engine.renderOffline(0.0, kBeat)), 0.0);
    CHECK_THROWS_AS(engine.previewNote(track + 100, 60, 100), std::invalid_argument);
}

TEST_CASE("quick preview notes all end") {
    // Dragging a note across keys in the piano roll plays and releases notes
    // faster than audio blocks go by. Several arrive in one block; each must end.
    sub::Engine engine;
    openAnyOutput(engine);
    const auto [track, synth] = synthTrack(engine, {}, {{RELEASE, 1.f}});
    engine.play();  // the transport measures audio time
    engine.previewNote(track, 60, 100);
    waitBeats(engine, 0.25);
    engine.takeMeters();
    waitBeats(engine, 0.25);
    CHECK(trackMeter(engine, track) > 0.05f);  // heard

    for (int key = 61; key < 73; ++key) {  // as the dragged note moves up: release the last key, play the next
        engine.previewNote(track, key - 1, 0);
        engine.previewNote(track, key, 100);
    }
    engine.previewNote(track, 72, 0);  // mouse up
    waitBeats(engine, 0.25);
    engine.takeMeters();
    waitBeats(engine, 0.5);
    CHECK(trackMeter(engine, track) < 1e-4f);  // nothing left sounding
    engine.closeDevice();
}
