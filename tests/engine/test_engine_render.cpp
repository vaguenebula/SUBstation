// Audio engine tests. They render offline, so no audio device is needed.

#include <cmath>
#include <set>

#include "Engine.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

struct RenderEngine {
    sub::Engine engine;
    RenderEngine() { engine.setClipFadeMs(0); }  // exact sample checks; fades have their own test
};

uint32_t addClipTrack(sub::Engine& engine, const std::string& path, double startBeat = 0.0, double durationSec = 1.0,
                      double offsetSec = 0.0, float gain = 1.f) {
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(path, startBeat, durationSec, offsetSec, gain)});
    return track;
}

// AudioSource's raw samples of each channel from `start` (the bindings' samples(), clipped to the file).
std::vector<Samples> sourceSamples(const sub::AudioSource& source, int64_t start, int64_t count) {
    start = std::clamp<int64_t>(start, 0, source.frames());
    count = std::clamp<int64_t>(count, 0, source.frames() - start);
    std::vector<Samples> out;
    for (uint32_t c = 0; c < source.channels(); ++c) {
        const float* data = source.channelData(c) + start;
        out.emplace_back(data, data + count);
    }
    return out;
}

// A 24-bit export's samples, interleaved stereo, as floats.
std::vector<double> readWav24(const std::filesystem::path& path) {
    const Wav wav = readWav(path);
    REQUIRE(wav.format == 1 && wav.bitsPerSample == 24 && wav.channels == 2);
    return wav.samples();
}

}  // namespace

TEST_CASE("a clip lands on its exact sample") {
    RenderEngine e;
    auto& engine = e.engine;
    addClipTrack(engine, dcWav(), 2.0);
    const auto frames = static_cast<int64_t>(5 * kSpb);
    const std::vector<float> out = engine.renderOffline(0.0, frames);
    REQUIRE(out.size() == static_cast<size_t>(frames) * 2);
    const auto start = static_cast<size_t>(2 * kSpb);
    for (size_t i = 0; i < start * 2; ++i) REQUIRE(out[i] == 0.f);
    CHECK_APPROX(out[start * 2], 0.5);
    CHECK_APPROX(out[start * 2 + 1], 0.5);
    CHECK_APPROX(out[(start + kSampleRate - 1) * 2], 0.5);
    CHECK_APPROX(out[(start + kSampleRate - 1) * 2 + 1], 0.5);
    for (size_t i = (start + kSampleRate) * 2; i < out.size(); ++i) REQUIRE(out[i] == 0.f);
}

TEST_CASE("a clip follows the tempo") {
    RenderEngine e;
    auto& engine = e.engine;
    engine.setTempo(60.0);  // one beat per second
    addClipTrack(engine, dcWav(), 1.0);
    const Samples out = engine.renderOffline(0.0, 2 * kSampleRate + 10);
    CHECK_EQ(at(out, kSampleRate - 1, 0), 0.f);
    CHECK_APPROX(at(out, kSampleRate, 0), 0.5);
}

TEST_CASE("offset and duration") {
    RenderEngine e;
    auto& engine = e.engine;
    addClipTrack(engine, rampWav(), 0.0, 0.25, 0.5);
    const Samples out = engine.renderOffline(0.0, kSampleRate);
    const int first = kSampleRate / 2;  // the clip starts playing the source at 0.5 s
    CHECK_APPROX(at(out, 0, 0), (first % 32768) / 32768.0);
    CHECK_APPROX(at(out, 100, 1), ((first + 100) % 32768) / 32768.0);  // mono -> both channels
    CHECK(at(out, kSampleRate / 4 - 1, 0) != 0.f);
    CHECK(allEqual(frames(out, kSampleRate / 4), 0.0));
}

TEST_CASE("render offline from a start position") {
    RenderEngine e;
    auto& engine = e.engine;
    addClipTrack(engine, dcWav(), 4.0);
    const Samples out = engine.renderOffline(4.0, 100);
    CHECK_APPROX(at(out, 0, 0), 0.5);
    CHECK_APPROX(at(out, 0, 1), 0.5);
}

TEST_CASE("clip gain, track gain and master") {
    RenderEngine e;
    auto& engine = e.engine;
    const uint32_t track = addClipTrack(engine, dcWav(), 0.0, 1.0, 0.0, 0.5f);
    engine.setTrackGain(track, 0.5f);
    engine.setMasterGain(0.5f);
    const Samples out = engine.renderOffline(0.0, 1000);
    CHECK_APPROX(at(out, 500, 0), 0.0625);
    CHECK_APPROX(at(out, 500, 1), 0.0625);
}

TEST_CASE("pan is balance with a unity centre") {
    RenderEngine e;
    auto& engine = e.engine;
    const uint32_t track = addClipTrack(engine, dcWav());
    engine.setTrackPan(track, 1.f);
    Samples out = engine.renderOffline(0.0, 1000);
    CHECK_NEAR(at(out, 500, 0), 0.0, 1e-6);
    CHECK_APPROX(at(out, 500, 1), 0.5);

    engine.setTrackPan(track, -0.5f);
    out = engine.renderOffline(0.0, 4000);  // past the 20 ms smoothing ramp
    CHECK_APPROX(at(out, -1, 0), 0.5);
    CHECK_APPROX_REL(at(out, -1, 1), 0.5 * std::cos(0.5 * kPi / 2), 1e-4);
}

TEST_CASE("mute and solo") {
    RenderEngine e;
    auto& engine = e.engine;
    const std::string other = makeWav(full(kSampleRate * 2, 0.25f), 2);
    const uint32_t a = addClipTrack(engine, dcWav());
    const uint32_t b = addClipTrack(engine, other);
    CHECK_APPROX(at(engine.renderOffline(0.0, 100), 50, 0), 0.75);

    engine.setTrackMute(a, true);
    CHECK_APPROX(at(engine.renderOffline(0.0, 4000), -1, 0), 0.25);

    engine.setTrackMute(a, false);
    engine.setTrackSolo(a, true);
    CHECK_APPROX(at(engine.renderOffline(0.0, 4000), -1, 0), 0.5);

    engine.setTrackSolo(b, true);
    CHECK_APPROX(at(engine.renderOffline(0.0, 4000), -1, 0), 0.75);
}

TEST_CASE("the loop wraps") {
    RenderEngine e;
    auto& engine = e.engine;
    addClipTrack(engine, rampWav());
    engine.setLoop(true, 0.0, 1.0);  // one beat = 24000 samples
    const auto loopLen = static_cast<int64_t>(kSpb);
    const Samples out = engine.renderOffline(0.0, loopLen * 2 + 10, true);
    CHECK_APPROX(at(out, loopLen - 1, 0), ((loopLen - 1) % 32768) / 32768.0);
    CHECK_APPROX(at(out, loopLen + 5, 0), 5 / 32768.0);  // jumped back to the loop start
    CHECK_ARRAY_EQUAL(frames(out, 0, loopLen), frames(out, loopLen, 2 * loopLen));
    // Without the loop flag (as for exports) the timeline plays straight through.
    const Samples straight = engine.renderOffline(0.0, loopLen * 2);
    CHECK(frames(straight, 0, loopLen) != frames(straight, loopLen));
}

TEST_CASE("a tempo change keeps the playhead on its beat") {
    RenderEngine e;
    auto& engine = e.engine;
    engine.setPositionBeats(8.0);
    engine.setTempo(60.0);
    CHECK_APPROX(engine.positionBeats(), 8.0);
    engine.setTempo(174.0);
    CHECK_NEAR(engine.positionBeats(), 8.0, 1e-4);
}

TEST_CASE("transport state without a device") {
    RenderEngine e;
    auto& engine = e.engine;
    CHECK(!engine.isPlaying());
    engine.play();
    CHECK(engine.isPlaying());
    engine.stop();
    CHECK(!engine.isPlaying());
}

TEST_CASE("the metronome clicks on beats") {
    RenderEngine e;
    auto& engine = e.engine;
    const Samples level = channel(engine.renderOffline(0.0, static_cast<int64_t>(4 * kSpb), false, true), 0);
    for (int beat = 0; beat < 4; ++beat) {
        INFO("beat " + std::to_string(beat));
        const auto onset = static_cast<int64_t>(beat * kSpb);
        CHECK(maxAbs(slice(level, onset, onset + 200)) > 0.1);  // a click
        CHECK(maxAbs(slice(level, onset + 4000, onset + static_cast<int64_t>(kSpb) - 10)) < 1e-3);  // that doesn't spill
    }
    // The downbeat is accented (higher pitch and level).
    CHECK(maxAbs(slice(level, 0, 2000)) > maxAbs(slice(level, kBeat, kBeat + 2000)));
}

TEST_CASE("clip fades: a short one where a clip cuts into its file") {
    sub::Engine engine;  // default 4 ms fades
    addClipTrack(engine, dcWav(), 0.0, 0.5, 0.25);  // 0.25 s to 0.75 s of a 1 s file: both edges cut into it
    const Samples out = engine.renderOffline(0.0, kSampleRate);
    const auto fade = static_cast<int64_t>(std::round(0.004 * kSampleRate));
    CHECK_EQ(at(out, 0, 0), 0.f);
    CHECK(0.f < at(out, fade / 2, 0));
    CHECK(at(out, fade / 2, 0) < 0.5f);
    CHECK_APPROX(at(out, fade, 0), 0.5);
    CHECK(0.f < at(out, kSampleRate / 2 - 1, 0));
    CHECK(at(out, kSampleRate / 2 - 1, 0) < 0.01f);
}

TEST_CASE("clip fades: none at the file's own start and end (a one-shot keeps its attack)") {
    sub::Engine engine;  // default 4 ms fades
    const std::string wav = dcWav();  // 1 s
    addClipTrack(engine, wav, 0.0, 1.0);  // all of it
    Samples out = engine.renderOffline(0.0, kSampleRate);
    CHECK_APPROX(at(out, 0, 0), 0.5);
    CHECK_APPROX(at(out, kSampleRate - 1, 0), 0.5);
    // From the file's start to a cut: faded at the cut only.
    sub::Engine cut;
    addClipTrack(cut, wav, 0.0, 0.5);
    out = cut.renderOffline(0.0, kSampleRate);
    CHECK_APPROX(at(out, 0, 0), 0.5);
    CHECK(at(out, kSampleRate / 2 - 1, 0) < 0.01f);
    // From a cut to the file's end: faded at the cut only.
    sub::Engine tail;
    addClipTrack(tail, wav, 0.0, 0.5, 0.5);
    out = tail.renderOffline(0.0, kSampleRate);
    CHECK_EQ(at(out, 0, 0), 0.f);
    CHECK_APPROX(at(out, kSampleRate / 2 - 1, 0), 0.5);
}

TEST_CASE("clip fades: a clip's own, curved, instead of the short ones") {
    sub::Engine engine;  // default 4 ms fades
    const std::string wav = dcWav();
    engine.loadSource(wav);
    const uint32_t track = engine.addTrack();
    sub::ClipDesc c = clip(wav, 0.0, 1.0);  // all of the file: its own fades all the same
    c.fadeInSec = 0.1;  // a straight line
    c.fadeOutSec = 0.2;
    c.fadeOutCurve = 1.f;  // bulging up: loud until late
    engine.setTrackClips(track, {c});
    const Samples out = engine.renderOffline(0.0, kSampleRate);
    CHECK_EQ(at(out, 0, 0), 0.f);
    CHECK_APPROX_REL(at(out, kSampleRate / 20, 0), 0.25, 1e-4);  // halfway in
    CHECK_APPROX(at(out, kSampleRate / 10, 0), 0.5);
    CHECK_APPROX(at(out, kSampleRate * 8 / 10 - 1, 0), 0.5);  // the fade out starts at 0.8 s
    const double bulge = std::expm1(-6.0 * 0.5) / std::expm1(-6.0);  // automationShape(0.5, 1)
    CHECK_APPROX_REL(at(out, kSampleRate * 9 / 10, 0), 0.5 * bulge, 1e-4);
    CHECK(at(out, kSampleRate - 1, 0) < 0.01f);

    // Longer together than the clip, they meet, shortened in proportion.
    c = clip(wav, 0.0, 1.0);
    c.fadeInSec = 1.5;
    c.fadeOutSec = 0.5;
    engine.setTrackClips(track, {c});
    const Samples met = engine.renderOffline(0.0, kSampleRate);
    CHECK_EQ(at(met, 0, 0), 0.f);
    CHECK_APPROX_REL(at(met, kSampleRate * 3 / 8, 0), 0.25, 1e-4);  // in for 0.75 s, then out for 0.25 s
    CHECK_APPROX_REL(at(met, kSampleRate * 7 / 8, 0), 0.25, 1e-4);
}

TEST_CASE("clip fades: a warped clip's are in its audio's time, stretched with it") {
    sub::Engine engine;
    engine.setClipFadeMs(0);
    const std::string wav = dcWav();
    engine.loadSource(wav);
    const uint32_t track = engine.addTrack();
    sub::ClipDesc c = clip(wav, 0.0, 1.0);
    c.warp = true;
    c.segmentBpm = 60.0;  // at 120 BPM: twice as fast
    c.warpMode = sub::WarpMode::RePitch;
    c.fadeInSec = 0.2;  // 0.1 s on the timeline
    engine.setTrackClips(track, {c});
    const Samples out = engine.renderOffline(0.0, kSampleRate / 2);
    CHECK_APPROX_REL(at(out, kSampleRate / 20, 0), 0.25, 1e-3);
    CHECK_APPROX_REL(at(out, kSampleRate / 10, 0), 0.5, 1e-3);
}

TEST_CASE("a clip waits for its source") {
    RenderEngine e;
    auto& engine = e.engine;
    const std::string wav = dcWav();
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {clip(wav, 0.0, 1.0)});
    CHECK(allEqual(engine.renderOffline(0.0, 1000), 0.0));
    engine.loadSource(wav);
    CHECK_APPROX(at(engine.renderOffline(0.0, 1000), 500, 0), 0.5);
}

TEST_CASE("remove a track") {
    RenderEngine e;
    auto& engine = e.engine;
    const uint32_t track = addClipTrack(engine, dcWav());
    engine.removeTrack(track);
    CHECK(allEqual(engine.renderOffline(0.0, 1000), 0.0));
    CHECK_THROWS_AS(engine.setTrackGain(track, 1.f), std::invalid_argument);
}

TEST_CASE("the utility device") {
    RenderEngine e;
    auto& engine = e.engine;
    const uint32_t track = addClipTrack(engine, dcWav());
    const uint32_t device = engine.addBuiltinProcessor(engine.trackChain(track), "utility", -1);
    CHECK(paramIds(engine.processorParams(device)) == (std::vector<std::string>{"gain", "pan", "width"}));
    engine.setProcessorParam(device, 0, static_cast<float>(20 * std::log10(0.5)));
    const Samples out = engine.renderOffline(0.0, 4000);
    CHECK_APPROX_REL(at(out, -1, 0), 0.25, 1e-4);

    engine.setProcessorEnabled(device, false);
    CHECK_APPROX(at(engine.renderOffline(0.0, 100), 50, 0), 0.5);

    engine.removeProcessor(device);
    CHECK_THROWS_AS(engine.processorParams(device), std::invalid_argument);
}

TEST_CASE("over the top") {
    // Soundgoodize at 100 % brings loud and quiet audio close together; at 0 % it only passes it through.
    RenderEngine e;
    auto& engine = e.engine;
    Rng rng(7);
    std::vector<double> noise(kSampleRate);
    for (double& x : noise) x = rng.normal();
    const double scale = 1.0 / rms(noise);
    for (double& x : noise) x *= scale;

    const auto rmsDb = [](const Samples& x) { return 20 * std::log10(rms(x)); };
    const auto render = [&](double levelDb, float depth) {
        Samples samples(noise.size());
        for (size_t i = 0; i < noise.size(); ++i) samples[i] = static_cast<float>(noise[i] * std::pow(10.0, levelDb / 20));
        const std::string path = makeWav(samples);
        engine.loadSource(path);
        const uint32_t track = engine.addTrack();
        engine.setTrackClips(track, {clip(path, 0.0, 1.0, 0.0, 1.f)});
        const uint32_t device = engine.addBuiltinProcessor(engine.trackChain(track), "ott", -1);
        CHECK(paramIds(engine.processorParams(device)) == (std::vector<std::string>{"depth", "output"}));
        engine.setProcessorParam(device, 0, depth);
        const Samples out = channel(frames(engine.renderOffline(0.0, kSampleRate), kSampleRate / 4), 0);
        engine.removeTrack(track);
        return rmsDb(out);
    };

    const double loud = -8.0, quiet = -45.0;
    CHECK_NEAR(render(loud, 0.f), loud, 0.5);  // the crossovers sum flat
    CHECK_NEAR(render(quiet, 0.f), quiet, 0.5);
    const double squashedLoud = render(loud, 100.f), squashedQuiet = render(quiet, 100.f);
    CHECK(squashedLoud < loud - 3);  // down from above,
    CHECK(squashedQuiet > quiet + 15);  // up from below
    CHECK(squashedLoud - squashedQuiet < 12);  // 37 dB apart went in
}

TEST_CASE("source peaks and samples") {
    RenderEngine e;
    auto& engine = e.engine;
    const std::string ramp = rampWav();
    const auto source = engine.loadSource(ramp);
    CHECK_EQ(source->channels(), 1u);
    CHECK_EQ(source->frames(), int64_t{kSampleRate});
    CHECK_APPROX(source->duration(), 1.0);
    const int spp = sub::AudioSource::samplesPerPeak(0);
    CHECK_EQ(source->numPeaks(0), static_cast<int64_t>(std::ceil(static_cast<double>(kSampleRate) / spp)));
    const float* peaks = source->peaks(0);  // [peak][channel][min, max]
    CHECK_APPROX(peaks[1 * 2 + 0], spp / 32768.0);
    CHECK_APPROX(peaks[1 * 2 + 1], (2 * spp - 1) / 32768.0);
    const int coarsest = source->numPeakLevels() - 1;
    const float* coarse = source->peaks(coarsest);
    double lowestMax = 1e9;
    for (int64_t i = 0; i < std::min(source->numPeaks(0), source->numPeaks(coarsest)); ++i)
        lowestMax = std::min(lowestMax, static_cast<double>(peaks[i * 2 + 1]));
    CHECK(coarse[1] >= lowestMax);
    const auto raw = sourceSamples(*source, 10, 5);
    REQUIRE(raw.size() == 1);
    REQUIRE(raw[0].size() == 5);
    CHECK_APPROX(raw[0][0], 10 / 32768.0);
    CHECK(engine.loadSource(ramp) != nullptr);  // cached
    CHECK_EQ(engine.cachedSource(ramp)->frames(), int64_t{kSampleRate});
}

TEST_CASE("resamples to the engine's rate") {
    RenderEngine e;
    auto& engine = e.engine;
    const std::string path = makeWav(full(22050, 0.5f), 1, 22050);
    const auto source = engine.loadSource(path);
    CHECK_EQ(source->sampleRate(), uint32_t{kSampleRate});
    CHECK_EQ(source->fileSampleRate(), 22050u);
    CHECK_NEAR(source->frames(), kSampleRate, 4);
}

TEST_CASE("probe a file") {
    const std::string path = makeWav(full(12345 * 2, 0.f), 2, 44100);
    const sub::AudioFileInfo info = sub::AudioSource::probe(path);
    CHECK_EQ(info.frames, int64_t{12345});
    CHECK_EQ(info.channels, 2u);
    CHECK_EQ(info.sampleRate, 44100u);
    CHECK_APPROX(info.duration, 12345 / 44100.0);
    CHECK_THROWS_AS(sub::AudioSource::probe(path + ".missing"), std::runtime_error);
}

TEST_CASE("an export matches the render") {
    RenderEngine e;
    auto& engine = e.engine;
    addClipTrack(engine, rampWav(), 0.0, 1.0, 0.0, 0.9f);
    const auto target = tempDir() / "mix.wav";
    engine.exportWav(target.string(), 0.0, 1.0, 24);
    const Wav wav = readWav(target);
    CHECK_EQ(wav.channels, 2);
    CHECK_EQ(wav.bitsPerSample, 24);
    CHECK_EQ(wav.sampleRate, kSampleRate);
    const std::vector<double> exported = wav.samples();
    const Samples rendered = engine.renderOffline(0.0, static_cast<int64_t>(kSpb));
    CHECK_EQ(exported.size(), rendered.size());
    CHECK_ALLCLOSE(exported, rendered, 1e-7, 2.0 / (1 << 23));
}

TEST_CASE("meters are empty offline") {
    RenderEngine e;
    auto& engine = e.engine;
    const uint32_t track = addClipTrack(engine, dcWav());
    engine.renderOffline(0.0, 1000);
    std::map<uint32_t, std::pair<float, float>> meters;
    for (const auto& m : engine.takeMeters()) meters[m.trackId] = {m.left, m.right};
    std::set<uint32_t> ids;
    for (const auto& [id, level] : meters) ids.insert(id);
    CHECK(ids == (std::set<uint32_t>{0, track}));
    CHECK(meters[track] == std::make_pair(0.f, 0.f));
}

TEST_CASE("every strip has its own chain") {
    RenderEngine e;
    auto& engine = e.engine;
    const uint32_t a = engine.addTrack(), b = engine.addTrack();
    const std::vector<uint32_t> chains{engine.trackChain(sub::Engine::kMaster), engine.trackChain(a), engine.trackChain(b)};
    CHECK_EQ(std::set<uint32_t>(chains.begin(), chains.end()).size(), size_t{3});
    const uint32_t device = engine.addBuiltinProcessor(chains[1], "utility", -1);
    CHECK_EQ(engine.processorChain(device), chains[1]);
    CHECK_THROWS_AS(engine.addBuiltinProcessor(*std::max_element(chains.begin(), chains.end()) + 1, "utility", -1),
                    std::invalid_argument);
    engine.removeTrack(a);
    CHECK_THROWS_AS(engine.addBuiltinProcessor(chains[1], "utility", -1), std::invalid_argument);  // its chain went with it
    CHECK_THROWS_AS(engine.processorChain(device), std::invalid_argument);
}

TEST_CASE("move a processor between chains") {
    RenderEngine e;
    auto& engine = e.engine;
    const std::string wav = dcWav();
    const uint32_t a = addClipTrack(engine, wav);
    const uint32_t b = addClipTrack(engine, wav);
    const uint32_t chainA = engine.trackChain(a), chainB = engine.trackChain(b);
    const uint32_t half = engine.addBuiltinProcessor(chainA, "utility", -1);
    engine.setProcessorParam(half, 0, static_cast<float>(20 * std::log10(0.5)));
    CHECK_APPROX_REL(at(engine.renderOffline(0.0, 4000), -1, 0), 0.75, 1e-4);

    // To the other track: its gain goes along.
    engine.moveProcessor(half, chainB, -1);
    CHECK_EQ(engine.processorChain(half), chainB);
    engine.setTrackClips(a, {});
    CHECK_APPROX_REL(at(engine.renderOffline(0.0, 4000), -1, 0), 0.25, 1e-4);

    // To the master, and to a position in a chain.
    engine.moveProcessor(half, engine.trackChain(sub::Engine::kMaster), -1);
    CHECK_APPROX_REL(at(engine.renderOffline(0.0, 4000), -1, 0), 0.25, 1e-4);
    const uint32_t other = engine.addBuiltinProcessor(chainB, "utility", -1);
    engine.moveProcessor(half, chainB, 0);
    engine.setChainOrder(chainB, {half, other});  // the order it has
    CHECK_THROWS_AS(engine.setChainOrder(chainB, {other}), std::invalid_argument);
    engine.moveProcessor(half, chainB, 5);  // past the end: last
    engine.setChainOrder(chainB, {other, half});
    CHECK_THROWS_AS(engine.moveProcessor(half, chainB + 99, -1), std::invalid_argument);
    CHECK_THROWS_AS(engine.moveProcessor(999, chainB, -1), std::invalid_argument);
}

TEST_CASE("an effect on the master changes the export") {
    RenderEngine e;
    auto& engine = e.engine;
    const std::string wav = dcWav();
    addClipTrack(engine, wav, 0.0, 0.5);
    addClipTrack(engine, wav, 0.0, 0.5);
    const uint32_t utility = engine.addBuiltinProcessor(engine.trackChain(sub::Engine::kMaster), "utility", -1);
    setParam(engine, utility, "gain", static_cast<float>(20 * std::log10(0.25)));
    const auto target = tempDir() / "mix.wav";
    engine.exportWav(target.string(), 0.0, 1.0, 24);
    const std::vector<double> exported = readWav24(target);
    // The master's effect works on the tracks' sum (1.0), after which its fader applies.
    CHECK_ALLCLOSE(slice(exported, 4000 * 2), 0.25, 1e-7, 1e-5);
    const Samples rendered = engine.renderOffline(0.0, static_cast<int64_t>(kSpb));
    CHECK_ALLCLOSE(slice(exported, 4000 * 2), frames(rendered, 4000), 1e-7, 2.0 / (1 << 23));  // past the gain's smoothing
    engine.setMasterGain(0.5f);
    CHECK_ALLCLOSE(frames(engine.renderOffline(0.0, 8000), 4000), 0.125, 1e-4, 0.0);
    engine.setProcessorEnabled(utility, false);
    CHECK_ALLCLOSE(engine.renderOffline(0.0, 100), 0.5, 1e-7, 0.0);
    engine.removeProcessor(utility);
    CHECK_ALLCLOSE(engine.renderOffline(0.0, 100), 0.5, 1e-7, 0.0);
}

TEST_CASE("the master is a track without clips") {
    RenderEngine e;
    auto& engine = e.engine;
    CHECK_THROWS_AS(engine.setTrackClips(sub::Engine::kMaster, {}), std::invalid_argument);
    CHECK_THROWS_AS(engine.setTrackNotes(sub::Engine::kMaster, {}), std::invalid_argument);
    CHECK_THROWS_AS(engine.removeTrack(sub::Engine::kMaster), std::invalid_argument);
    addClipTrack(engine, dcWav());
    engine.setTrackGain(sub::Engine::kMaster, 0.5f);  // its mixer, as the tracks'
    engine.setTrackPan(sub::Engine::kMaster, 1.f);
    const Samples out = engine.renderOffline(0.0, 100);
    CHECK_NEAR(at(out, 50, 0), 0.0, 1e-7);
    CHECK_APPROX(at(out, 50, 1), 0.25);
    const auto meters = engine.takeMeters();
    REQUIRE(!meters.empty());
    CHECK_EQ(meters[0].trackId, sub::Engine::kMaster);
}
