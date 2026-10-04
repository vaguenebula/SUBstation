// Renders in the background (RenderJob): an export or a track's render on a
// thread of its own, with its progress, cancelled (its file gone), one at a
// time, rendering the project as it was when it started.

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

#include "Engine.h"
#include "RenderJob.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

struct JobEngine {
    sub::Engine engine;
    JobEngine() { engine.setClipFadeMs(0); }
};

constexpr double kTwoHours = 2 * 60 * 60 * 2.0;  // in beats

// The progress seen until the job is done.
std::vector<double> waitDone(sub::RenderJob& job, double timeout = 20.0) {
    std::vector<double> seen;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(timeout);
    while (!job.done()) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);  // (the render never ended)
        seen.push_back(job.progress());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return seen;
}

bool fileExists(const std::filesystem::path& path) {
    std::error_code ignored;
    return std::filesystem::exists(path, ignored);
}

}  // namespace

TEST_CASE("an export in the background is the same export") {
    JobEngine e;
    auto& engine = e.engine;
    clipTrack(engine, rampWav());
    const auto now = tempDir() / "now.wav", later = tempDir() / "later.wav";
    engine.exportWav(now.string(), 0.0, 2.0, 24);

    const auto job = engine.startExport(later.string(), 0.0, 2.0, 24);
    CHECK_EQ(job->path(), later.string());
    CHECK(engine.isRendering());
    const std::vector<double> seen = waitDone(*job);
    CHECK(std::is_sorted(seen.begin(), seen.end()));
    CHECK_EQ(job->progress(), 1.0);
    CHECK(engine.isRendering());  // until it is finished
    CHECK(job->finish() == std::optional<int64_t>(2 * kBeat));
    CHECK(!engine.isRendering());
    CHECK(readBytes(later) == readBytes(now));
    CHECK(job->finish() == std::optional<int64_t>(2 * kBeat));  // (again: the same)
}

TEST_CASE("a track rendered in the background is the same render") {
    JobEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, dcWav(), 0.0, 0.5);
    const auto now = tempDir() / "now.wav", later = tempDir() / "later.wav";
    const int64_t written = engine.renderTrackToWav(track, now.string(), 0.0, 2.0, 1.0);
    const auto job = engine.startTrackRender(track, later.string(), 0.0, 2.0, 1.0);
    waitDone(*job);
    CHECK(job->finish() == std::optional<int64_t>(written));
    CHECK_EQ(written, 2 * kBeat);  // (no tail: silent after the clip)
    CHECK(readBytes(later) == readBytes(now));
}

TEST_CASE("a cancelled render leaves no file") {
    JobEngine e;
    auto& engine = e.engine;
    clipTrack(engine, dcWav());
    const auto target = tempDir() / "long.wav";
    auto job = engine.startExport(target.string(), 0.0, kTwoHours, 16);
    CHECK(fileExists(target));  // created at once
    job->cancel();
    CHECK(job->cancelled());
    CHECK(!job->finish().has_value());
    CHECK(!fileExists(target));
    CHECK(!engine.isRendering());
    // The engine renders again as before.
    CHECK_APPROX(at(engine.renderOffline(0.0, kBeat), 100, 0), 0.5);

    const uint32_t track = engine.addTrack();
    job = engine.startTrackRender(track, (tempDir() / "track.wav").string(), 0.0, kTwoHours, 0.0);
    job->cancel();
    CHECK(!job->finish().has_value());
    CHECK(!fileExists(tempDir() / "track.wav"));
}

TEST_CASE("one render at a time, and what waits for it") {
    JobEngine e;
    auto& engine = e.engine;
    clipTrack(engine, dcWav());
    const auto job = engine.startExport((tempDir() / "a.wav").string(), 0.0, 2.0, 24);
    CHECK_THROWS_MATCHING(engine.startExport((tempDir() / "b.wav").string(), 0.0, 2.0, 24), std::runtime_error,
                          "Wait for the render");
    CHECK(!fileExists(tempDir() / "b.wav"));
    CHECK_THROWS_MATCHING(engine.renderOffline(0.0, 100), std::runtime_error, "Wait for the render");
    CHECK_THROWS_MATCHING(engine.setAudioThreads(engine.audioThreads() != 1 ? 1 : 2), std::runtime_error,
                          "Wait for the render");
    engine.takeMeters();  // what the UI polls doesn't wait for it
    engine.idle();
    CHECK(job->finish() == std::optional<int64_t>(2 * kBeat));
    engine.setAudioThreads(1);
    CHECK_APPROX(at(engine.renderOffline(0.0, 100), 50, 0), 0.5);
}

TEST_CASE("a render hears the project as it was when it started") {
    JobEngine e;
    auto& engine = e.engine;
    const uint32_t track = clipTrack(engine, dcWav());
    const uint32_t utility = engine.addBuiltinProcessor(engine.trackChain(track), "utility", -1);
    const auto mix = tempDir() / "mix.wav";
    const auto job = engine.startExport(mix.string(), 0.0, 2.0, 32);
    engine.setTrackClips(track, {});  // changes meanwhile are taken...
    engine.removeProcessor(utility);
    engine.removeTrack(track);
    engine.idle();
    CHECK(job->finish() == std::optional<int64_t>(2 * kBeat));  // ...but not heard in it
    sub::Engine reader;
    const auto exported = reader.loadSource(mix.string());
    CHECK_EQ(exported->channels(), 2u);
    CHECK_EQ(exported->frames(), 2 * kBeat);
    for (uint32_t c = 0; c < exported->channels(); ++c) {
        INFO("channel " + std::to_string(c));
        const Samples samples(exported->channelData(c), exported->channelData(c) + exported->frames());
        CHECK_ALLCLOSE(samples, 0.5, 1e-6, 1e-12);  // (pytest.approx)
    }
    CHECK_EQ(at(engine.renderOffline(0.0, 100), 50, 0), 0.f);
}

TEST_CASE("a render that can't start says why") {
    JobEngine e;
    auto& engine = e.engine;
    CHECK_THROWS_MATCHING(engine.startExport((tempDir() / "missing folder" / "mix.wav").string(), 0.0, 1.0, 24),
                          std::runtime_error, "Could not create");
    CHECK_THROWS_AS(engine.startExport((tempDir() / "mix.wav").string(), 1.0, 1.0, 24), std::invalid_argument);
    CHECK_THROWS_AS(engine.startTrackRender(12345, (tempDir() / "track.wav").string(), 0.0, 1.0, 0.0),
                    std::invalid_argument);
    CHECK(!engine.isRendering());
    CHECK(std::filesystem::is_empty(tempDir()));
}

TEST_CASE("a render let go of unfinished gives the engine back") {
    JobEngine e;
    auto& engine = e.engine;
    auto job = engine.startExport((tempDir() / "mix.wav").string(), 0.0, kTwoHours, 24);
    job.reset();
    CHECK(!engine.isRendering());
    CHECK(!fileExists(tempDir() / "mix.wav"));
    // And an engine going while its render runs stops it first.
    auto other = std::make_unique<sub::Engine>();
    job = other->startExport((tempDir() / "other.wav").string(), 0.0, kTwoHours, 24);
    other.reset();
    CHECK(!job->finish().has_value());
    CHECK(!fileExists(tempDir() / "other.wav"));
}
