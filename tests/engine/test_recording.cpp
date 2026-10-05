// Audio input and recording in the engine, with the fake ASIO driver
// (tests/asio_driver) in manual mode: track inputs, input monitoring, recorded
// takes and where they land on the timeline, count-in, and what ends a
// recording. Skipped without the ASIO SDK.
//
// The loopback tests patch the driver's first output back into an input, as a
// cable would, delayed by the latencies the driver reports: what the engine plays
// comes back as the input it records, so a take can be held sample by sample to
// the arrangement it was recorded against.

#include <cmath>
#include <optional>

#include "Engine.h"
#include "harness/LiveTests.h"
#include "harness/Recording.h"

using namespace subtest;

namespace {

bool fileExists(const std::filesystem::path& path) {
    std::error_code ignored;
    return std::filesystem::exists(path, ignored);
}

}  // namespace

// --- Monitoring ---------------------------------------------------------------------

TEST_CASE("monitoring modes") {
    RecordingTest t;
    auto& driver = t.driver;
    auto& engine = t.engine;
    driver.setInputLevel(1, 0.25);
    driver.setInputLevel(2, -0.5);
    openAsio(engine, kRate, kBuffer, {1, 2});
    const uint32_t track = engine.addTrack();

    const auto heard = [&](int buffers = 2) {
        driver.clearOutput();
        driver.process(buffers);
        return std::make_pair(slice(output(driver, 0), -kBuffer), slice(output(driver, 1), -kBuffer));
    };

    CHECK(allEqual(heard().first, 0.0));  // no input
    engine.setTrackInput(track, {1, 2});  // a stereo pair
    engine.setTrackMonitor(track, sub::MonitorMode::In);
    auto [left, right] = heard();
    CHECK_ALLCLOSE(left, 0.25, 1e-7, 1e-6);
    CHECK_ALLCLOSE(right, -0.5, 1e-7, 1e-6);
    engine.setTrackInput(track, {2});  // mono: both sides
    std::tie(left, right) = heard();
    CHECK_ALLCLOSE(left, -0.5, 1e-7, 1e-6);
    CHECK_ALLCLOSE(right, -0.5, 1e-7, 1e-6);

    engine.setTrackMonitor(track, sub::MonitorMode::Off);
    CHECK(allEqual(heard().first, 0.0));
    engine.setTrackMonitor(track, sub::MonitorMode::Auto);
    CHECK(allEqual(heard().first, 0.0));  // not armed
    engine.setTrackArmed(track, true);
    CHECK_ALLCLOSE(heard().first, -0.5, 1e-7, 1e-6);  // armed, stopped
    engine.play();
    CHECK(allEqual(heard().first, 0.0));  // playing back, not recording: its clips
    engine.stop();
    CHECK_ALLCLOSE(heard().first, -0.5, 1e-7, 1e-6);

    engine.setTrackInput(track, {3});  // not open: silent
    CHECK(allEqual(heard().first, 0.0));
    engine.setTrackInput(track, {});
    CHECK(allEqual(heard().first, 0.0));
    CHECK_THROWS_AS(engine.setTrackInput(track, {0, 1, 2}), std::invalid_argument);
    CHECK_THROWS_AS(engine.setTrackInput(sub::Engine::kMaster, {0}), std::invalid_argument);
}

void subtest::live::monitoringThroughALatentPlugin(RecordingTest& t) {
    auto& driver = t.driver;
    auto& engine = t.engine;
    requireTestPlugins();
    driver.setInputLevel(0, 0.5);
    openAsio(engine, kRate, kBuffer, {0});
    const uint32_t slow = engine.addTrack();
    latentEffect(engine, slow, 300);  // every other track waits for this one...
    const uint32_t monitored = engine.addTrack();
    latentEffect(engine, monitored, 100);
    engine.setTrackInput(monitored, {0});
    engine.setTrackMonitor(monitored, sub::MonitorMode::In);
    driver.clearOutput();
    driver.process(4);
    const std::vector<double> out = output(driver);
    // ...but a monitored track only waits for its own devices.
    CHECK(allEqual(slice(out, 0, 100), 0.0));
    CHECK_ALLCLOSE(slice(out, 100), 0.5, 1e-7, 1e-6);
}

TEST_CASE("monitoring through a latent plug-in is not delayed by compensation") {
    RecordingTest t;
    live::monitoringThroughALatentPlugin(t);
}

// --- Recording ----------------------------------------------------------------------

// The engine plays a clip; the cable brings it back; the take, placed where
// the engine says, holds exactly what the timeline played there.
void subtest::live::loopbackTakeLinesUp(RecordingTest& t, const std::string& latency) {
    auto& driver = t.driver;
    auto& engine = t.engine;
    if (latency != "none") requireTestPlugins();
    Rng rng(7);
    Samples signal(kRate);
    for (float& x : signal) x = static_cast<float>(std::round(rng.uniform(-0.5, 0.5) * 32768) / 32768);  // exact in 16 bits
    const std::string wav = makeWav(signal);
    driver.setLoopback(0, 0);
    driver.setLatencies(40, 90);
    openAsio(engine, kRate, kBuffer, {0});
    const uint32_t player = engine.addTrack();
    engine.loadSource(wav);
    engine.setTrackClips(player, {clip(wav, 0.5, 1.0)});
    const uint32_t other = engine.addTrack();  // delayed to line up with a latent player
    engine.setTrackClips(other, {clip(wav, 0.5, 1.0, 0.0, 0.f)});
    if (latency == "track") latentEffect(engine, player, 333);
    else if (latency == "master") latentEffect(engine, sub::Engine::kMaster, 333);
    const uint32_t recorder = engine.addTrack();
    engine.setTrackInput(recorder, {0});
    engine.setTrackMonitor(recorder, sub::MonitorMode::Off);  // or it would play its own input into the cable
    engine.setTrackArmed(recorder, true);

    std::string path = makeWav({0.f});
    path.replace(path.rfind(".wav"), 4, "-take.wav");
    engine.startRecording({{recorder, path}});
    CHECK(engine.isRecording());
    CHECK(engine.isPlaying());
    driver.process(8 * kRate / kBuffer / 4);  // two seconds
    const auto progress = engine.recordingProgress();
    REQUIRE(progress.size() == 1);
    CHECK_EQ(progress[0].trackId, recorder);
    CHECK(progress[0].started);
    const auto takes = engine.stopRecording();
    CHECK(!engine.isRecording());  // recording ends;
    CHECK(engine.isPlaying());  // playing goes on
    REQUIRE(takes.size() == 1);
    const sub::RecordedTake& take = takes[0];
    CHECK_EQ(take.trackId, recorder);
    CHECK_EQ(take.path, path);
    CHECK_EQ(take.channels, 1);
    CHECK_EQ(take.sampleRate, static_cast<double>(kRate));
    CHECK_EQ(take.droppedFrames, int64_t{0});
    CHECK_EQ(take.error, std::string());
    CHECK(take.frames >= 2 * kRate - kBuffer);
    CHECK_EQ(progress[0].startSample, take.startSample);
    const int64_t lag = (2 * kBuffer + 40 + 90) + (latency != "none" ? 333 : 0);
    CHECK_EQ(take.startSample, -lag);  // it began with the playhead at 0, heard that much later

    const std::vector<float> recorded = readTake(engine, take).at(0);
    std::vector<double> timeline(static_cast<size_t>(take.frames + lag + kRate), 0.0);
    for (int i = 0; i < kRate; ++i) timeline[static_cast<size_t>(kRecordSpb / 2 + i)] = signal[static_cast<size_t>(i)];  // what the master played (the other track is silent)
    std::vector<double> expected(static_cast<size_t>(take.frames), 0.0);
    for (int64_t i = 0; i < take.frames; ++i) {
        const int64_t position = i + take.startSample;
        if (position >= 0) expected[static_cast<size_t>(i)] = timeline[static_cast<size_t>(position)];
    }
    CHECK_ALLCLOSE(recorded, expected, 1e-7, 1e-6);
    CHECK(maxAbs(recorded) > 0.4);
}

TEST_CASE("a loopback take lines up with the timeline") {
    requireTestAsio();
    for (const std::string latency : {"none", "track", "master"}) {
        INFO("latency: " + latency);
        RecordingTest t;
        live::loopbackTakeLinesUp(t, latency);
    }
}

TEST_CASE("a stereo take and its live peaks") {
    RecordingTest t;
    auto& driver = t.driver;
    auto& engine = t.engine;
    driver.setInputLevel(2, 0.25);
    driver.setInputLevel(3, -0.75);
    openAsio(engine, kRate, kBuffer, {2, 3});
    const uint32_t track = engine.addTrack();
    engine.setTrackInput(track, {2, 3});
    engine.startRecording({{track, (tempDir() / "rec" / "stereo.wav").string()}});  // makes its folder
    driver.process(10);
    const auto progress = engine.recordingProgress().at(0);
    CHECK_EQ(progress.frames, int64_t{10 * kBuffer});
    CHECK_EQ(progress.peaks.size(), size_t{10 * kBuffer / sub::Engine::kRecordPeakFrames * 2});  // (min, max) pairs
    std::vector<double> expected;
    for (size_t i = 0; i < progress.peaks.size() / 2; ++i) expected.insert(expected.end(), {-0.75, 0.25});
    CHECK_ALLCLOSE(progress.peaks, expected, 1e-7, 1e-6);
    CHECK(engine.recordingProgress().at(0).peaks.empty());  // since the last call
    const sub::RecordedTake take = engine.stopRecording().at(0);
    const auto samples = readTake(engine, take);
    CHECK_EQ(take.channels, 2);
    CHECK_EQ(take.frames, int64_t{10 * kBuffer});
    REQUIRE(samples.size() == 2);
    CHECK_ALLCLOSE(samples[0], 0.25, 1e-7, 1e-6);
    CHECK_ALLCLOSE(samples[1], -0.75, 1e-7, 1e-6);
}

void subtest::live::autoMonitoringWhileRecording(RecordingTest& t) {
    auto& driver = t.driver;
    auto& engine = t.engine;
    driver.setInputLevel(0, 0.5);
    openAsio(engine, kRate, kBuffer, {0});
    const uint32_t track = engine.addTrack();
    engine.setTrackInput(track, {0});
    engine.setTrackArmed(track, true);  // Auto
    engine.play();
    driver.clearOutput();
    driver.process(2);
    CHECK(allEqual(slice(output(driver), -kBuffer), 0.0));  // playing back
    engine.startRecording({{track, (tempDir() / "take.wav").string()}});  // punch in
    driver.clearOutput();
    driver.process(2);
    CHECK_ALLCLOSE(slice(output(driver), -kBuffer), 0.5, 1e-7, 1e-6);
    const sub::RecordedTake take = engine.stopRecording().at(0);
    CHECK_EQ(take.startSample, int64_t{2 * kBuffer - (2 * kBuffer + 32 + 64)});  // where the playhead was, heard
}

TEST_CASE("auto monitoring while recording") {
    RecordingTest t;
    live::autoMonitoringWhileRecording(t);
}

TEST_CASE("a track being recorded plays none of its clips") {
    // Its take replaces them: unmonitored, the track is silent while it records.
    RecordingTest t;
    auto& driver = t.driver;
    auto& engine = t.engine;
    driver.setInputLevel(0, 0.25);
    openAsio(engine, kRate, kBuffer, {0});
    const std::string wav = makeWav(full(4 * kRate, 0.5f));
    engine.loadSource(wav);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(wav, 0.0, 4.0)});
    engine.setTrackInput(track, {0});
    engine.setTrackMonitor(track, sub::MonitorMode::Off);
    engine.setTrackArmed(track, true);

    const auto heard = [&] {
        driver.clearOutput();
        driver.process(3);
        return slice(output(driver), -kBuffer);
    };

    engine.play();
    CHECK_ALLCLOSE(heard(), 0.5, 1e-7, 1e-6);  // playing back: its clip
    engine.startRecording({{track, (tempDir() / "take.wav").string()}});  // punch in
    CHECK(allEqual(heard(), 0.0));  // neither its clip nor its input
    engine.stopRecording();
    CHECK_ALLCLOSE(heard(), 0.5, 1e-7, 1e-6);  // punched out: its clip again

    engine.setTrackMonitor(track, sub::MonitorMode::In);  // monitored: its input, as before
    engine.startRecording({{track, (tempDir() / "take2.wav").string()}});
    CHECK_ALLCLOSE(heard(), 0.25, 1e-7, 1e-6);
    engine.stopRecording();
}

void subtest::live::countIn(RecordingTest& t) {
    auto& driver = t.driver;
    auto& engine = t.engine;
    driver.setInputLevel(0, 0.5);
    openAsio(engine, kRate, kBuffer, {0});
    const uint32_t track = engine.addTrack();
    engine.setTrackInput(track, {0});
    engine.setTrackMonitor(track, sub::MonitorMode::Off);
    engine.setPositionBeats(2.0);
    engine.startRecording({{track, (tempDir() / "take.wav").string()}}, 4.0);
    driver.clearOutput();
    driver.process(1);
    CHECK(engine.isCountingIn());
    CHECK_EQ(engine.positionBeats(), 2.0);
    CHECK_EQ(engine.recordingProgress().at(0).frames, int64_t{0});
    const int64_t countIn = 4 * kRecordSpb;
    driver.process(static_cast<int>(countIn / kBuffer + 4));
    CHECK(!engine.isCountingIn());
    const int64_t moved = (countIn / kBuffer + 5) * kBuffer - countIn;  // samples played since it ended
    CHECK_APPROX(engine.positionBeats(), 2.0 + static_cast<double>(moved) / kRecordSpb);
    const sub::RecordedTake take = engine.stopRecording().at(0);
    CHECK_EQ(take.frames, moved);
    CHECK_EQ(take.startSample, 2 * kRecordSpb - (2 * kBuffer + 32 + 64));
    // It clicks on every beat of the count-in (the metronome is off), then stops.
    const std::vector<double> out = output(driver);
    for (int beat = 0; beat < 4; ++beat) {
        INFO("beat " + std::to_string(beat));
        CHECK(maxAbs(slice(out, beat * kRecordSpb, beat * kRecordSpb + 200)) > 0.05);
        if (beat) CHECK(maxAbs(slice(out, beat * kRecordSpb - 2000, beat * kRecordSpb)) < 1e-3);
    }
    driver.clearOutput();
    driver.process(20);
    CHECK(maxAbs(slice(output(driver), 3000)) < 1e-3);  // no fifth click
}

TEST_CASE("count-in") {
    RecordingTest t;
    live::countIn(t);
}

void subtest::live::whatEndsARecording(RecordingTest& t) {
    auto& driver = t.driver;
    auto& engine = t.engine;
    openAsio(engine, kRate, kBuffer, {0, 1});
    const uint32_t a = engine.addTrack(), b = engine.addTrack();
    engine.setTrackInput(a, {0});
    engine.setTrackInput(b, {1});

    // A locate: the takes end where the playhead jumped.
    engine.startRecording({{a, (tempDir() / "a1.wav").string()}, {b, (tempDir() / "b1.wav").string()}});
    driver.process(4);
    engine.setPositionBeats(8.0);
    driver.process(4);
    CHECK(!engine.isRecording());
    auto takes = engine.stopRecording();
    REQUIRE(takes.size() == 2);
    CHECK_EQ(takes[0].trackId, a);
    CHECK_EQ(takes[0].frames, int64_t{4 * kBuffer});
    CHECK_EQ(takes[1].trackId, b);
    CHECK_EQ(takes[1].frames, int64_t{4 * kBuffer});

    // Opening the device again (another buffer size or sample rate): the takes so far are kept.
    engine.startRecording({{a, (tempDir() / "a2.wav").string()}});
    driver.process(3);
    openAsio(engine, 96000, kBuffer, {0, 1});
    CHECK(!engine.isRecording());
    const sub::RecordedTake kept = engine.stopRecording().at(0);
    CHECK_EQ(kept.frames, int64_t{3 * kBuffer});
    CHECK_EQ(kept.sampleRate, static_cast<double>(kRate));
    CHECK(engine.stopRecording().empty());

    // Stopped before anything came in: no take, no file.
    engine.stop();
    engine.startRecording({{a, (tempDir() / "a3.wav").string()}}, 4.0);
    engine.stop();
    const sub::RecordedTake none = engine.stopRecording().at(0);
    CHECK_EQ(none.frames, int64_t{0});
    CHECK(!fileExists(tempDir() / "a3.wav"));
}

TEST_CASE("what ends a recording") {
    RecordingTest t;
    live::whatEndsARecording(t);
}

TEST_CASE("recording needs an open input") {
    RecordingTest t;
    auto& engine = t.engine;
    const uint32_t track = engine.addTrack();
    engine.setTrackInput(track, {1});
    const std::string x = (tempDir() / "x.wav").string();
    CHECK_THROWS_MATCHING(engine.startRecording({{track, x}}), std::runtime_error, "No audio device");
    openAsio(engine, kRate, kBuffer, {0});
    CHECK_THROWS_MATCHING(engine.startRecording({{track, x}}), std::runtime_error, "not open");
    const uint32_t other = engine.addTrack();
    CHECK_THROWS_MATCHING(engine.startRecording({{other, x}}), std::invalid_argument, "no input");
    engine.setTrackInput(track, {0});
    CHECK_THROWS_MATCHING(engine.startRecording({{track, tempDir().string()}}), std::runtime_error,
                          "Could not create");  // a folder
    CHECK(!engine.isRecording());
    CHECK(!engine.isPlaying());
}
