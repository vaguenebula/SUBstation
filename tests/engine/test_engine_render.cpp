// The engine renders offline, so no audio device is needed.

#include <cmath>

#include "Engine.h"
#include "harness/Fixtures.h"

using namespace subtest;

namespace {

uint32_t addClipTrack(sub::Engine& engine, const std::string& path, double startBeat = 0.0, double durationSec = 1.0,
                      double offsetSec = 0.0, float gain = 1.f) {
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    sub::ClipDesc clip;
    clip.path = path;
    clip.startBeat = startBeat;
    clip.durationSec = durationSec;
    clip.offsetSec = offsetSec;
    clip.gain = gain;
    engine.setTrackClips(track, {clip});
    return track;
}

}  // namespace

TEST_CASE("a clip lands on its exact sample") {
    sub::Engine engine;
    engine.setClipFadeMs(0);
    addClipTrack(engine, dcWav(), 2.0);
    const auto frames = static_cast<int64_t>(5 * kSpb);
    const std::vector<float> out = engine.renderOffline(0.0, frames);
    REQUIRE(out.size() == static_cast<size_t>(frames) * 2);
    const auto start = static_cast<size_t>(2 * kSpb);
    for (size_t i = 0; i < start * 2; ++i) REQUIRE(out[i] == 0.f);
    CHECK_NEAR(out[start * 2], 0.5, 1e-6);
    CHECK_NEAR(out[start * 2 + 1], 0.5, 1e-6);
    CHECK_NEAR(out[(start + kSampleRate - 1) * 2], 0.5, 1e-6);
    for (size_t i = (start + kSampleRate) * 2; i < out.size(); ++i) REQUIRE(out[i] == 0.f);
}
