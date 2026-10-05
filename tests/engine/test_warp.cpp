// Warping in the engine: tempo-following clips, pitch shifting and Re-Pitch.
// Rendered offline, so no audio device is needed.

#include <cmath>

#include "Engine.h"
#include "harness/Fixtures.h"
#include "harness/Signal.h"

using namespace subtest;

namespace {

const double kSineRms = 0.5 / std::sqrt(2.0);  // of the 0.5-amplitude test tones

struct WarpEngine {
    sub::Engine engine;
    WarpEngine() { engine.setClipFadeMs(0); }
};

// What a ClipDesc's keyword arguments set beyond the plain clip.
struct Warping {
    bool warp = false;
    double segmentBpm = 0.0;
    sub::WarpMode mode = sub::WarpMode::Standard;
    double transpose = 0.0;
    float pan = 0.f;
};

Samples sineOf(double freq, double seconds = 2.0, double amplitude = 0.5) { return sine(freq, seconds, amplitude); }

// Silence, then a sine burst from `seconds` on: a sharp, easy-to-find onset.
Samples burstAt(double seconds, double total = 2.0, double freq = 1000.0) {
    Samples wave = sineOf(freq, total);
    std::fill(wave.begin(), wave.begin() + static_cast<int64_t>(seconds * kSampleRate), 0.f);
    return wave;
}

sub::ClipDesc warpedClip(const std::string& path, double startBeat, double durationSec, const Warping& w) {
    sub::ClipDesc c = clip(path, startBeat, durationSec, 0.0, 1.f);
    c.warp = w.warp;
    c.segmentBpm = w.segmentBpm;
    c.warpMode = w.mode;
    c.transpose = w.transpose;
    c.pan = w.pan;
    return c;
}

uint32_t addClip(sub::Engine& engine, const std::string& path, double startBeat = 0.0, double durationSec = 2.0,
                 const Warping& w = {}) {
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, {warpedClip(path, startBeat, durationSec, w)});
    return track;
}

// The first and the last audible frame (+1) of the left channel.
std::pair<int64_t, int64_t> audible(const Samples& out, double threshold = 1e-3) {
    const auto loud = above(channel(out, 0), threshold);
    REQUIRE(!loud.empty());
    return {loud.front(), loud.back() + 1};
}

// Where the (1 ms RMS) envelope first reaches half of `level`. Phase-vocoder
// output smears a little before transients, so the raw first sample is early.
int64_t onset(const Samples& out, double level = kSineRms) {
    const std::vector<double> env = envelope(channel(out, 0));
    for (size_t i = 0; i < env.size(); ++i)
        if (env[i] > 0.5 * level) return static_cast<int64_t>(i);
    return 0;  // (np.argmax of all False)
}

Samples leftOf(const Samples& out, int64_t from, int64_t to) { return channel(frames(out, from, to), 0); }

}  // namespace

TEST_CASE("transpose shifts the pitch but not the length") {
    WarpEngine e;
    auto& engine = e.engine;
    addClip(engine, makeWav(sineOf(440.0)), 0.0, 2.0, {.transpose = 12.0});
    const Samples out = engine.renderOffline(0.0, 3 * kSampleRate);
    const auto [first, last] = audible(out);
    CHECK(first < 200);
    CHECK(std::abs(last - 2 * kSampleRate) < 200);
    CHECK_APPROX_REL(dominantFreq(leftOf(out, kSampleRate / 2, kSampleRate / 2 + 16384)), 880.0, 0.005);
}

TEST_CASE("detune in fractional semitones") {
    WarpEngine e;
    auto& engine = e.engine;
    addClip(engine, makeWav(sineOf(440.0)), 0.0, 2.0, {.transpose = -0.5});  // -50 cents
    const Samples out = engine.renderOffline(0.0, 2 * kSampleRate);
    const double freq = dominantFreq(leftOf(out, kSampleRate / 2, kSampleRate / 2 + 32768));
    CHECK_APPROX_REL(freq, 440.0 * std::pow(2.0, -0.5 / 12), 0.002);
}

TEST_CASE("a warped clip follows the tempo and keeps its pitch") {
    for (const sub::WarpMode mode :
         {sub::WarpMode::Transients, sub::WarpMode::Standard, sub::WarpMode::Smooth, sub::WarpMode::Formants}) {
        INFO("warp mode " + std::to_string(static_cast<int>(mode)));
        WarpEngine e;
        auto& engine = e.engine;
        // 2 s of audio at 60 BPM is 2 beats: at 120 BPM that takes 1 s, at the same pitch.
        addClip(engine, makeWav(sineOf(440.0)), 1.0, 2.0, {.warp = true, .segmentBpm = 60.0, .mode = mode});
        const Samples out = engine.renderOffline(0.0, 3 * kSampleRate);
        const auto [first, last] = audible(out);
        CHECK_NEAR(first, kSpb, 2);
        CHECK_EQ(last, static_cast<int64_t>(3 * kSpb));  // ends exactly on beat 3
        const int64_t middle = kBeat + kSampleRate / 4;
        const Samples window = leftOf(out, middle, middle + 16384);
        CHECK_APPROX_REL(dominantFreq(window), 440.0, 0.005);
        CHECK_APPROX_REL(rms(window), kSineRms, 0.15);
    }
}

TEST_CASE("a warped clip slows down") {
    WarpEngine e;
    auto& engine = e.engine;
    addClip(engine, makeWav(sineOf(440.0)), 0.0, 2.0, {.warp = true, .segmentBpm = 240.0});  // 2 s -> 8 beats... at 120: 4 s
    const Samples out = engine.renderOffline(0.0, 5 * kSampleRate);
    CHECK_EQ(audible(out).second, int64_t{4 * kSampleRate});
    CHECK_APPROX_REL(dominantFreq(leftOf(out, kSampleRate, kSampleRate + 16384)), 440.0, 0.005);
}

TEST_CASE("a warped clip at its own tempo is bit exact") {
    WarpEngine e;
    auto& engine = e.engine;
    addClip(engine, rampWav(), 0.0, 1.0, {.warp = true, .segmentBpm = 120.0});
    const Samples out = engine.renderOffline(0.0, kSampleRate);
    Samples expected(kSampleRate);
    for (int i = 0; i < kSampleRate; ++i) expected[static_cast<size_t>(i)] = static_cast<float>((i % 32768) / 32768.0);
    CHECK_ARRAY_EQUAL(channel(out, 0), expected);
}

TEST_CASE("a tempo change rescales warped clips only") {
    WarpEngine e;
    auto& engine = e.engine;
    const std::string path = makeWav(sineOf(440.0, 1.0));
    addClip(engine, path, 0.0, 1.0, {.warp = true, .segmentBpm = 120.0});  // 2 beats
    addClip(engine, path, 8.0, 1.0);  // unwarped: 1 s
    engine.setTempo(90.0);
    const double spb = kSampleRate * 60 / 90.0;
    const Samples out = engine.renderOffline(0.0, static_cast<int64_t>(12 * spb));
    const Samples warped = frames(out, 0, static_cast<int64_t>(4 * spb));
    const auto end = static_cast<int64_t>(std::llround(2 * spb));  // still 2 beats long, now 1.33 s
    CHECK(audible(warped).second > end - 32);
    CHECK(allEqual(frames(warped, end), 0.0));
    CHECK_NEAR(audible(frames(out, static_cast<int64_t>(8 * spb))).second, kSampleRate, 1);
}

TEST_CASE("a warped clip is sample aligned") {
    // The burst starts 0.5 s into the source; at double speed it must be heard
    // 0.25 s after the clip start, with no stretcher latency.
    WarpEngine e;
    auto& engine = e.engine;
    addClip(engine, makeWav(burstAt(0.5)), 2.0, 2.0,
            {.warp = true, .segmentBpm = 60.0, .mode = sub::WarpMode::Transients});
    const Samples out = engine.renderOffline(0.0, 4 * kSampleRate);
    const auto expected = static_cast<int64_t>(2 * kSpb + 0.25 * kSampleRate);
    CHECK(std::abs(onset(out) - expected) < 0.004 * kSampleRate);
}

TEST_CASE("a transposed clip is sample aligned") {
    WarpEngine e;
    auto& engine = e.engine;
    addClip(engine, makeWav(burstAt(0.5)), 2.0, 2.0, {.transpose = -5.0});
    const Samples out = engine.renderOffline(0.0, 4 * kSampleRate);
    CHECK(std::abs(onset(out) - static_cast<int64_t>(2 * kSpb + 0.5 * kSampleRate)) < 0.004 * kSampleRate);
}

TEST_CASE("starting inside a warped clip matches playing through") {
    // A locate into the middle of a stretched clip re-seeks the stretcher; the
    // bursts must land where a continuous playthrough puts them. (The waveforms
    // differ in phase, as stretched audio does; the timing must not.)
    WarpEngine e;
    auto& engine = e.engine;
    Samples pulses = sineOf(1000.0);
    for (size_t i = 0; i < pulses.size(); ++i)
        if (static_cast<int64_t>(i) % (kSampleRate / 4) >= kSampleRate / 20) pulses[i] = 0.f;  // 50 ms every 250 ms
    addClip(engine, makeWav(pulses), 0.0, 2.0, {.warp = true, .segmentBpm = 100.0});
    const Samples whole = engine.renderOffline(0.0, 2 * kSampleRate);
    const auto start = static_cast<int64_t>(1.5 * kSpb);
    const Samples part = engine.renderOffline(1.5, kSampleRate / 2);
    const std::vector<double> a = envelope(leftOf(whole, start + 2000, start + 22000), 240);
    const std::vector<double> b = envelope(leftOf(part, 2000, 22000), 240);
    CHECK(correlation(a, b) > 0.97);
}

TEST_CASE("warped offline renders are repeatable") {
    WarpEngine e;
    auto& engine = e.engine;
    Rng rng(3);
    addClip(engine, makeWav(rng.uniformSamples(kSampleRate, -0.5, 0.5)), 0.0, 1.0,
            {.warp = true, .segmentBpm = 77.0, .mode = sub::WarpMode::Smooth, .transpose = 3.0});
    const Samples first = engine.renderOffline(0.0, kSampleRate);
    const Samples second = engine.renderOffline(0.0, kSampleRate);
    CHECK_ARRAY_EQUAL(first, second);
}

TEST_CASE("Re-Pitch changes speed and pitch together") {
    WarpEngine e;
    auto& engine = e.engine;
    addClip(engine, makeWav(sineOf(440.0)), 0.0, 2.0,
            {.warp = true, .segmentBpm = 60.0, .mode = sub::WarpMode::RePitch, .transpose = 7.0});  // transpose is ignored in Re-Pitch
    const Samples out = engine.renderOffline(0.0, 2 * kSampleRate);
    CHECK_EQ(audible(out).second, int64_t{kSampleRate});
    CHECK_APPROX_REL(dominantFreq(leftOf(out, kSampleRate / 4, kSampleRate / 4 + 16384)), 880.0, 0.002);
}

TEST_CASE("Re-Pitch filters instead of aliasing") {
    // 15 kHz at double speed would be 30 kHz, above Nyquist: it must be filtered
    // out, not fold back down to 18 kHz.
    WarpEngine e;
    auto& engine = e.engine;
    addClip(engine, makeWav(sineOf(15000.0)), 0.0, 2.0, {.warp = true, .segmentBpm = 60.0, .mode = sub::WarpMode::RePitch});
    const Samples out = engine.renderOffline(0.0, kSampleRate);
    CHECK(maxAbs(leftOf(out, kSampleRate / 4, kSampleRate / 4 + 8192)) < 0.02);
}

TEST_CASE("stretching stereo and mono sources") {
    WarpEngine e;
    auto& engine = e.engine;
    addClip(engine, makeWav(interleave({sineOf(440.0), sineOf(660.0)}), 2), 0.0, 2.0, {.warp = true, .segmentBpm = 90.0});
    const Samples out = engine.renderOffline(0.0, kSampleRate);
    const Samples window = frames(out, kSampleRate / 4, kSampleRate / 4 + 16384);
    CHECK_APPROX_REL(dominantFreq(channel(window, 0)), 440.0, 0.005);
    CHECK_APPROX_REL(dominantFreq(channel(window, 1)), 660.0, 0.005);
}

TEST_CASE("clip pan") {
    WarpEngine e;
    auto& engine = e.engine;
    addClip(engine, dcWav(), 0.0, 1.0, {.pan = -1.f});
    const Samples out = engine.renderOffline(0.0, 1000);
    CHECK_NEAR(at(out, 500, 0), 0.5, 1e-6);
    CHECK_NEAR(at(out, 500, 1), 0.0, 1e-6);
}

TEST_CASE("many warped clips in sequence") {
    // Back-to-back warped clips share a few stretch voices; each must still play.
    WarpEngine e;
    auto& engine = e.engine;
    const std::string path = makeWav(sineOf(440.0, 1.0));
    engine.loadSource(path);
    const uint32_t track = engine.addTrack();
    std::vector<sub::ClipDesc> clips;
    for (int beat = 0; beat < 16; beat += 2) {
        sub::ClipDesc c = warpedClip(path, beat, 0.25, {.warp = true, .segmentBpm = 60.0});
        c.id = "c" + std::to_string(beat);
        clips.push_back(c);
    }  // each: 0.25 s of audio at 60 BPM = 0.25 beats
    engine.setTrackClips(track, clips);
    const Samples out = channel(engine.renderOffline(0.0, static_cast<int64_t>(16 * kSpb)), 0);
    for (int beat = 0; beat < 16; beat += 2) {
        INFO("beat " + std::to_string(beat));
        const Samples piece = slice(out, static_cast<int64_t>(beat * kSpb) + 200, static_cast<int64_t>((beat + 0.25) * kSpb) - 200);
        CHECK(rms(piece) > 0.2);
        CHECK(allEqual(slice(out, static_cast<int64_t>((beat + 0.25) * kSpb) + 1, static_cast<int64_t>((beat + 1) * kSpb)), 0.0));
    }
}

TEST_CASE("an export includes warped audio") {
    WarpEngine e;
    auto& engine = e.engine;
    addClip(engine, makeWav(sineOf(440.0)), 0.0, 2.0, {.warp = true, .segmentBpm = 60.0, .transpose = 12.0});
    const auto path = tempDir() / "out.wav";
    engine.exportWav(path.string(), 0.0, 2.0, 16);
    const Wav wav = readWav(path);
    REQUIRE(wav.format == 1 && wav.bitsPerSample == 16 && wav.channels == 2);
    const std::vector<double> left = channel(wav.samples(), 0);
    CHECK_EQ(left.size(), size_t{kSampleRate});
    CHECK_APPROX_REL(dominantFreq(slice(left, kSampleRate / 4, kSampleRate / 4 + 16384)), 880.0, 0.005);
}
