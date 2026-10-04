// Live playback with workers, through the fake ASIO driver in manual mode: the
// MIDI input, recording and resampling tests again, with silent tracks beside theirs (each
// with a device) so that every buffer's tracks are shared out among threads
// (RecordingTest(true)). Live, held, recorded and preview notes and recorded
// audio behave as on one thread. Skipped without ASIO, like the tests they repeat.

#include <cmath>

#include "Engine.h"
#include "harness/LiveTests.h"
#include "harness/MidiInputTest.h"

using namespace subtest;

namespace {

// The engine on workers, beside silent tracks.
struct WorkersTest : RecordingTest {
    WorkersTest() : RecordingTest(true) {}
};

}  // namespace

// --- The MIDI input tests, again ----------------------------------------------------

TEST_CASE("on workers: live notes reach the instrument at their offsets") {
    WorkersTest t;
    live::liveNotesReachTheInstrument(t);
}

TEST_CASE("on workers: a note released as it is played still sounds") {
    WorkersTest t;
    live::aNoteReleasedAsItIsPlayedStillSounds(t);
}

TEST_CASE("on workers: which tracks hear which input") {
    WorkersTest t;
    live::whichTracksHearWhichInput(t);
}

TEST_CASE("on workers: MIDI monitoring") {
    WorkersTest t;
    live::midiMonitoring(t);
}

TEST_CASE("on workers: held keys are released") {
    WorkersTest t;
    live::heldKeysAreReleased(t);
}

TEST_CASE("on workers: recorded notes land on the beats they were played") {
    requireTestAsio();
    for (const std::string latency : {"none", "track", "master"}) {
        INFO("latency: " + latency);
        WorkersTest t;
        live::recordedNotesLandOnTheirBeats(t, latency);
    }
}

TEST_CASE("on workers: MIDI and audio record together") {
    WorkersTest t;
    live::midiAndAudioRecordTogether(t);
}

// --- The recording tests, again -----------------------------------------------------

TEST_CASE("on workers: auto monitoring while recording") {
    WorkersTest t;
    live::autoMonitoringWhileRecording(t);
}

TEST_CASE("on workers: count-in") {
    WorkersTest t;
    live::countIn(t);
}

TEST_CASE("on workers: a loopback take lines up with the timeline") {
    requireTestAsio();
    for (const std::string latency : {"none", "track", "master"}) {
        INFO("latency: " + latency);
        WorkersTest t;
        live::loopbackTakeLinesUp(t, latency);
    }
}

TEST_CASE("on workers: monitoring through a latent plug-in is not delayed by compensation") {
    WorkersTest t;
    live::monitoringThroughALatentPlugin(t);
}

TEST_CASE("on workers: what ends a recording") {
    WorkersTest t;
    live::whatEndsARecording(t);
}

// --- The resampling tests, again ----------------------------------------------------

TEST_CASE("on workers: a resampled take equals its source's render") {
    requireTestAsio();
    for (const std::string kind : {"track", "latent track", "group", "master"}) {
        INFO("from a " + kind);
        WorkersTest t;
        live::aResampledTakeEqualsItsSourcesRender(t, kind);
    }
}

TEST_CASE("on workers: monitoring a track is not delayed") {
    WorkersTest t;
    live::monitoringATrackIsNotDelayed(t);
}

TEST_CASE("on workers: solo across a monitored input") {
    WorkersTest t;
    live::soloAcrossAMonitoredInput(t);
}

// --- Its own ------------------------------------------------------------------------

TEST_CASE("the workers render live") {
    WorkersTest t;
    auto& driver = t.driver;
    auto& engine = t.engine;
    keys(engine, driver);
    send(engine, {kNoteOn, 60, 127}, 0);
    CHECK_ALLCLOSE(slice(heard(driver, 50), -kBuffer), held({{0, kBuffer, 127}}), 1e-7, 1e-6);
    CHECK(engine.nodesOnWorkers() > 0);
}

TEST_CASE("preview notes") {
    // Preview notes (the piano roll's keys) reach the instrument at the start of
    // the next buffer, and every one of them ends, also several in one buffer.
    WorkersTest t;
    auto& driver = t.driver;
    auto& engine = t.engine;
    openAsio(engine, kRate, kBuffer);
    const uint32_t track = engine.addTrack();
    dcSynth(engine, track);  // DC: each note adds its velocity
    driver.process(2);
    engine.previewNote(track, 60, 127);
    CHECK_ALLCLOSE(heard(driver), held({{0, kBuffer, 127}}), 1e-7, 1e-6);
    for (int key = 61; key < 73; ++key) {  // a note dragged across keys: release one, play the next
        engine.previewNote(track, key - 1, 0);
        engine.previewNote(track, key, 64);
    }
    CHECK_ALLCLOSE(heard(driver), held({{0, kBuffer, 64}}), 1e-7, 1e-6);
    engine.previewNote(track, 72, 0);
    CHECK(allEqual(heard(driver, 2), 0.0));
    engine.previewNote(track, 50, 127);
    engine.play();  // notes started by hand go on when the transport starts
    CHECK_ALLCLOSE(heard(driver), held({{0, kBuffer, 127}}), 1e-7, 1e-6);
    engine.stop();
    engine.previewNote(track, 50, 0);
    CHECK(allEqual(heard(driver), 0.0));
}

TEST_CASE("sends ramp live") {
    // Live, a send's level and solo change in a ramp (as a fader's), whichever
    // threads render the tracks.
    WorkersTest t;
    auto& driver = t.driver;
    auto& engine = t.engine;
    openAsio(engine, kRate, kBuffer);
    const std::string dc = dcWav();
    engine.loadSource(dc);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(dc, 0.0, 1.0)});
    const uint32_t ret = engine.addTrack();
    engine.setTrackSend(track, ret, 0.5f, false);
    engine.play();
    const int ramp = static_cast<int>(0.02 * kRate) / kBuffer + 2;  // the 20 ms ramp, and then some
    CHECK_APPROX(heard(driver, ramp).back(), 0.75);

    const auto glidesTo = [&](double value, const char* what) {
        INFO(what);
        const std::vector<double> out = heard(driver, ramp);
        CHECK_APPROX(out.back(), value);
        double jump = 0.0;
        for (size_t i = 1; i < out.size(); ++i) jump = std::max(jump, std::fabs(out[i] - out[i - 1]));
        CHECK(jump < 0.01);  // no jump
    };
    engine.setTrackSend(track, ret, 1.f, false);
    glidesTo(1.0, "a new send level");
    engine.setTrackSolo(ret, true);  // the track goes on sending, but not into the master
    glidesTo(0.5, "the return soloed");
    engine.setTrackMute(track, true);
    glidesTo(0.0, "the track muted");
    CHECK(engine.nodesOnWorkers() > 0);
}

TEST_CASE("a sidechain lines up live") {
    // Live, with the source on one thread and the device's track perhaps on
    // another, a sidechain still lines up at its device: the source's click and
    // the track's (and its device's copy of the source's) fall on one sample.
    WorkersTest t;
    auto& driver = t.driver;
    auto& engine = t.engine;
    requireTestPlugins();
    openAsio(engine, kRate, kBuffer);
    Samples click(1000, 0.f);
    click[0] = 0.25f;
    const std::string path = makeWav(click);
    engine.loadSource(path);
    const uint32_t source = engine.addTrack(), track = engine.addTrack();
    for (const uint32_t tr : {source, track}) engine.setTrackClips(tr, {clip(path, 0.25, 1000.0 / kRate)});
    const uint32_t latent = addTestPlugin(engine, engine.trackChain(source), "SUB Test Effect");
    engine.setProcessorParam(latent, 1, 300);  // Latency
    engine.idle();
    const uint32_t keyed = addTestPlugin(engine, engine.trackChain(track), "SUB Test Sidechain");
    engine.setProcessorSidechain(keyed, source);  // it comes 300 samples late: the track waits for it
    engine.play();
    const std::vector<double> out = heard(driver, (kRate / 8 + 300) / kBuffer + 8);
    const auto loud = above(out, 1e-7);
    REQUIRE(loud.size() == 1);
    CHECK_APPROX(out[static_cast<size_t>(loud[0])], 0.75);
    CHECK(engine.nodesOnWorkers() > 0);
}
