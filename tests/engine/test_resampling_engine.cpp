// Resampling in the engine: a track taking its input from another track's
// output (or a group's, or a return's), or from the master's, with the fake ASIO
// driver in manual mode. A take of a track equals that track's render sample for
// sample, wherever latent plug-ins are; a monitored track hears its source
// without delay compensation, never the master; cycles are refused. Skipped
// without ASIO, like the recording tests (but for the cycles, which need no driver).

#include <cmath>
#include <map>

#include "Engine.h"
#include "harness/LiveTests.h"
#include "harness/Recording.h"

using namespace subtest;

namespace {

// The left output of the next buffers.
std::vector<double> heardLeft(AsioDriver& driver, int buffers = 1) {
    driver.clearOutput();
    driver.process(buffers);
    return output(driver, 0);
}

constexpr int kRamp = static_cast<int>(0.02 * kRate) / kBuffer + 2;  // buffers for a live fader's (or solo's) 20 ms ramp, and then some

void openAsioPlain(sub::Engine& engine) { openAsio(engine, kRate, kBuffer); }

}  // namespace

// Recorded from a track (after its fader), a group or the master, a take
// holds what that source renders, placed where it was heard: as late as it
// leaves its source, not by any device latency, nor delayed with the edges
// that line up at the master.
void subtest::live::aResampledTakeEqualsItsSourcesRender(RecordingTest& t, const std::string& kind) {
    auto& driver = t.driver;
    auto& engine = t.engine;
    requireTestPlugins();
    Rng rng(11);
    Samples signal(static_cast<size_t>(kRate) * 2);
    for (float& x : signal) x = static_cast<float>(std::round(rng.uniform(-0.5, 0.5) * 32768) / 32768);
    const std::string wav = makeWav(signal, 2);
    openAsioPlain(engine);
    engine.loadSource(wav);
    uint32_t source = engine.addTrack();
    engine.setTrackClips(source, {clip(wav, 0.5, 1.0)});
    engine.setTrackGain(source, 0.5f);  // after the fader
    const uint32_t slow = engine.addTrack();  // everything else is delayed to line up with it
    latentEffect(engine, slow, 100);
    const uint32_t recorder = engine.addTrack();
    int64_t arrival = 0;
    if (kind == "latent track") {
        latentEffect(engine, source, 333);
        arrival = 333;
    } else if (kind == "group") {
        const uint32_t group = engine.addTrack();
        engine.setTrackOutput(source, group);
        latentEffect(engine, source, 50);
        latentEffect(engine, group, 333);
        engine.setTrackGain(group, 0.8f);
        source = group;
        arrival = 50 + 333;
    } else if (kind == "master") {
        latentEffect(engine, sub::Engine::kMaster, 333);
        engine.setTrackGain(sub::Engine::kMaster, 0.8f);
        source = sub::Engine::kMaster;
        arrival = 100 + 333;  // the master hears the tracks 100 late
    }
    engine.setTrackInputTrack(recorder, source);
    // The master can't be heard (it would feed back); a track could, but would go into the master too.
    engine.setTrackMonitor(recorder, kind == "master" ? sub::MonitorMode::In : sub::MonitorMode::Off);

    engine.startRecording({{recorder, (tempDir() / "take.wav").string()}});
    driver.process(8 * kRate / kBuffer / 4);  // two seconds
    const auto progress = engine.recordingProgress();
    const sub::RecordedTake take = engine.stopRecording().at(0);
    CHECK_EQ(take.trackId, recorder);
    CHECK_EQ(take.channels, 2);
    CHECK_EQ(take.droppedFrames, int64_t{0});
    CHECK_EQ(take.error, std::string());
    CHECK_EQ(take.startSample, -arrival);
    REQUIRE(!progress.empty());
    CHECK_EQ(progress[0].startSample, -arrival);
    const auto recorded = readTake(engine, take);

    engine.stop();
    if (source != sub::Engine::kMaster) engine.setTrackSolo(source, true);  // the source alone (a group: with what is in it)
    const Samples rendered = engine.renderOffline(0.0, take.frames + take.startSample);
    REQUIRE(recorded.size() == 2);
    for (int c = 0; c < 2; ++c) {
        INFO("channel " + std::to_string(c));
        std::vector<double> expected(static_cast<size_t>(take.frames), 0.0);
        const Samples renderedChannel = channel(rendered, c);
        for (size_t i = 0; i < renderedChannel.size(); ++i)
            expected[static_cast<size_t>(-take.startSample) + i] = renderedChannel[i];
        CHECK_ALLCLOSE(recorded[static_cast<size_t>(c)], expected, 1e-7, 1e-6);
    }
    CHECK(std::max(maxAbs(recorded[0]), maxAbs(recorded[1])) > 0.15);
}

TEST_CASE("a resampled take equals its source's render") {
    requireTestAsio();
    for (const std::string kind : {"track", "latent track", "group", "master"}) {
        INFO("from a " + kind);
        RecordingTest t;
        live::aResampledTakeEqualsItsSourcesRender(t, kind);
    }
}

// A monitored track hears its source's output instead of its clips, as soon
// as the source has it: not delayed to line up with a slower track.
void subtest::live::monitoringATrackIsNotDelayed(RecordingTest& t) {
    auto& driver = t.driver;
    auto& engine = t.engine;
    requireTestPlugins();
    openAsioPlain(engine);
    const std::string dc = dcWav();
    engine.loadSource(dc);
    const uint32_t source = engine.addTrack();
    engine.setTrackClips(source, {clip(dc, 0.0, 1.0)});
    const uint32_t slow = engine.addTrack();
    latentEffect(engine, slow, 300);  // the source reaches the master 300 late...
    const uint32_t listener = engine.addTrack();
    engine.setTrackClips(listener, {clip(dc, 0.0, 1.0, 0.0, 0.5f)});  // (0.25)
    engine.setTrackInputTrack(listener, source);
    engine.setTrackMonitor(listener, sub::MonitorMode::Off);
    engine.play();
    std::vector<double> out = heardLeft(driver, 4);
    CHECK_ALLCLOSE(slice(out, 0, 300), 0.0, 1e-7, 1e-6);
    CHECK_ALLCLOSE(slice(out, 300), 0.75, 1e-7, 1e-6);  // its clip, lined up

    engine.stop();
    engine.setPositionBeats(0.0);
    engine.setTrackMonitor(listener, sub::MonitorMode::In);
    driver.process(2);
    engine.play();
    out = heardLeft(driver, 4);  // ...but the listener hears it at once
    CHECK_ALLCLOSE(slice(out, 0, 300), 0.5, 1e-7, 1e-6);
    CHECK_ALLCLOSE(slice(out, 300), 1.0, 1e-7, 1e-6);

    engine.setTrackMonitor(listener, sub::MonitorMode::Auto);
    engine.setTrackArmed(listener, true);
    CHECK_APPROX(heardLeft(driver, 3).back(), 0.75);  // playing back without recording: its clip
    engine.stop();
    CHECK_APPROX(heardLeft(driver, 3).back(), 0.0);  // stopped: hears the source (silent now)
}

TEST_CASE("monitoring a track is not delayed") {
    RecordingTest t;
    live::monitoringATrackIsNotDelayed(t);
}

TEST_CASE("monitoring the master is impossible") {
    RecordingTest t;
    auto& driver = t.driver;
    auto& engine = t.engine;
    openAsioPlain(engine);
    const std::string dc = dcWav();
    engine.loadSource(dc);
    const uint32_t source = engine.addTrack();
    engine.setTrackClips(source, {clip(dc, 0.0, 1.0)});
    const uint32_t resampler = engine.addTrack();
    engine.setTrackInputTrack(resampler, sub::Engine::kMaster);
    engine.setTrackMonitor(resampler, sub::MonitorMode::In);
    engine.play();
    CHECK_ALLCLOSE(heardLeft(driver, 4), 0.5, 1e-7, 1e-6);  // no feedback
}

// Monitored, an input edge carries solo like any edge: soloing the source
// keeps the track hearing it; soloing the track keeps its source going into
// it (but not into the master). Unmonitored, it carries nothing.
void subtest::live::soloAcrossAMonitoredInput(RecordingTest& t) {
    auto& driver = t.driver;
    auto& engine = t.engine;
    openAsioPlain(engine);
    const std::string dc = dcWav();
    engine.loadSource(dc);
    const uint32_t source = engine.addTrack();
    engine.setTrackClips(source, {clip(dc, 0.0, 1.0)});  // 0.5
    const uint32_t listener = engine.addTrack();
    engine.setTrackClips(listener, {clip(dc, 0.0, 1.0, 0.0, 0.5f)});  // 0.25
    engine.setTrackInputTrack(listener, source);
    engine.play();

    const auto level = [&] { return heardLeft(driver, kRamp).back(); };

    engine.setTrackMonitor(listener, sub::MonitorMode::Off);
    CHECK_APPROX(level(), 0.75);
    engine.setTrackSolo(source, true);
    CHECK_APPROX(level(), 0.5);
    engine.setTrackMonitor(listener, sub::MonitorMode::In);
    CHECK_APPROX(level(), 1.0);  // it hears the soloed source
    engine.setTrackSolo(source, false);
    engine.setTrackSolo(listener, true);
    CHECK_APPROX(level(), 0.5);  // only through the listener
    engine.setTrackMonitor(listener, sub::MonitorMode::Off);
    CHECK_APPROX(level(), 0.25);  // its clip; the source is silenced
}

TEST_CASE("solo across a monitored input") {
    RecordingTest t;
    live::soloAcrossAMonitoredInput(t);
}

TEST_CASE("input cycles are refused") {
    sub::Engine engine;
    const uint32_t a = engine.addTrack(), b = engine.addTrack(), group = engine.addTrack(), ret = engine.addTrack();
    CHECK_THROWS_AS(engine.setTrackInputTrack(a, a), std::invalid_argument);
    engine.setTrackOutput(a, group);
    CHECK_THROWS_MATCHING(engine.setTrackInputTrack(a, group), std::invalid_argument, "feeds");  // its own group
    engine.setTrackInputTrack(b, a);
    CHECK_THROWS_AS(engine.setTrackInputTrack(a, b), std::invalid_argument);
    CHECK_THROWS_AS(engine.setTrackOutput(b, a), std::invalid_argument);  // b takes its input from a
    engine.setTrackInputTrack(b, ret);
    CHECK_THROWS_AS(engine.setTrackSend(b, ret, 1.f, false), std::invalid_argument);
    engine.setTrackSend(a, ret, 1.f, false);  // b takes ret's output; a feeding ret is fine
    engine.setTrackInputTrack(b, sub::Engine::kMaster);  // the master: always (it renders after every track)
    CHECK(engine.trackInputTrack(b) == std::optional<uint32_t>(sub::Engine::kMaster));
    engine.setTrackInputTrack(group, sub::Engine::kMaster);
    CHECK_THROWS_AS(engine.setTrackInputTrack(sub::Engine::kMaster, a), std::invalid_argument);
    CHECK_THROWS_AS(engine.setTrackInputTrack(a, 999), std::invalid_argument);
    engine.setTrackInput(b, {0});  // back to the device: the edge from the master goes (it was none)
    engine.setTrackInputTrack(b, ret);
    engine.setTrackInput(b, {0});  // and the one from ret
    CHECK(!engine.trackInputTrack(b).has_value());
    engine.setTrackSend(b, ret, 1.f, false);
}

TEST_CASE("the input goes with its source") {
    RecordingTest t;
    auto& driver = t.driver;
    auto& engine = t.engine;
    openAsioPlain(engine);
    const uint32_t source = engine.addTrack(), listener = engine.addTrack();
    engine.setTrackInputTrack(listener, source);
    CHECK(engine.trackInputTrack(listener) == std::optional<uint32_t>(source));
    engine.removeTrack(source);
    CHECK(!engine.trackInputTrack(listener).has_value());
    const std::string take = (tempDir() / "take.wav").string();
    CHECK_THROWS_MATCHING(engine.startRecording({{listener, take}}), std::invalid_argument, "no input");
    engine.setTrackInputTrack(listener, sub::Engine::kMaster);
    engine.startRecording({{listener, take}});
    driver.process(2);
    const sub::RecordedTake recorded = engine.stopRecording().at(0);
    CHECK_EQ(recorded.frames, int64_t{2 * kBuffer});
    CHECK_EQ(recorded.channels, 2);
}

TEST_CASE("a source removed while recording leaves silence") {
    RecordingTest t;
    auto& driver = t.driver;
    auto& engine = t.engine;
    openAsioPlain(engine);
    const std::string dc = dcWav();
    engine.loadSource(dc);
    const uint32_t source = engine.addTrack(), listener = engine.addTrack();
    engine.setTrackClips(source, {clip(dc, 0.0, 1.0)});
    engine.setTrackInputTrack(listener, source);
    engine.startRecording({{listener, (tempDir() / "take.wav").string()}});
    driver.process(2);
    engine.removeTrack(source);
    driver.process(2);
    const sub::RecordedTake take = engine.stopRecording().at(0);
    const auto samples = readTake(engine, take);
    CHECK_EQ(take.frames, int64_t{4 * kBuffer});
    for (const auto& c : samples) {
        CHECK_ALLCLOSE(slice(c, 0, 2 * kBuffer), 0.5, 1e-7, 1e-6);
        CHECK_ALLCLOSE(slice(c, 2 * kBuffer), 0.0, 1e-7, 1e-6);
    }
}

TEST_CASE("device and resampled takes together") {
    // Each take is placed by its own input: device input by the device's
    // latencies and the output's lag, a track's output by its arrival.
    RecordingTest t;
    auto& driver = t.driver;
    auto& engine = t.engine;
    requireTestPlugins();
    driver.setInputLevel(0, 0.25);
    openAsio(engine, kRate, kBuffer, {0});
    const std::string dc = dcWav();
    engine.loadSource(dc);
    const uint32_t source = engine.addTrack();
    engine.setTrackClips(source, {clip(dc, 0.0, 1.0)});
    latentEffect(engine, source, 200);
    const uint32_t mic = engine.addTrack(), resampler = engine.addTrack();
    engine.setTrackInput(mic, {0});
    engine.setTrackInputTrack(resampler, source);
    for (const uint32_t track : {mic, resampler}) engine.setTrackMonitor(track, sub::MonitorMode::Off);
    engine.setPositionBeats(1.0);
    engine.startRecording({{mic, (tempDir() / "mic.wav").string()}, {resampler, (tempDir() / "res.wav").string()}});
    driver.process(4);
    std::map<uint32_t, sub::RecordedTake> takes;
    for (const auto& take : engine.stopRecording()) takes[take.trackId] = take;
    CHECK_EQ(takes[mic].startSample, kRecordSpb - (2 * kBuffer + 32 + 64 + 200));  // the driver's latencies, the lag
    CHECK_EQ(takes[resampler].startSample, kRecordSpb - 200);
    CHECK_EQ(takes[mic].channels, 1);
    CHECK_EQ(takes[resampler].channels, 2);
    CHECK_EQ(takes[mic].frames, int64_t{4 * kBuffer});
    CHECK_EQ(takes[resampler].frames, int64_t{4 * kBuffer});
}
