// Background freezing, phase 1 (docs/engine/background-freeze.md): what a strip
// plays live, unchanged, is kept, and played again instead of running its
// devices. Each test plays one script twice, a buffer at a time through the
// "Manual" driver: on an engine with background freezing (no wait before a
// strip plays from its cache, a short warm-up) and on one without, and compares
// what came out. The devices here forget what they heard within the warm-up (a
// 50 ms delay without feedback, Utilities), so wherever the cache plays, or
// hands over to devices it warmed up first, the two are the same sample for
// sample. Where an edit lands on what is playing, the devices start cold: the
// two are the same again once the delay has forgotten.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "Engine.h"
#include "backends/ManualBackend.h"
#include "harness/Fixtures.h"

using namespace subtest;

namespace {

constexpr int kBuffer = 256;
constexpr double kWarm = 0.25;              // seconds
constexpr int64_t kCold = kSampleRate / 10;  // a cold start's difference is gone by then (50 ms delay, 20 ms fade)

sub::BackgroundFreezingSettings freezing(bool enabled = true, double budgetMB = 1024.0) {
    sub::BackgroundFreezingSettings settings;
    settings.enabled = enabled;
    settings.idleSeconds = 0.0;  // a strip plays from its cache as soon as it is good
    settings.warmSeconds = kWarm;
    settings.budgetMB = budgetMB;
    return settings;
}

// An engine on the Manual driver, and what it played.
struct Session {
    sub::Engine engine;
    const bool freezing;
    std::vector<float> out;      // interleaved stereo
    std::vector<int64_t> marks;  // frames played at moments the script marked

    Session(bool withCache, const sub::BackgroundFreezingSettings& settings, int threads) : freezing(withCache) {
        engine.setClipFadeMs(0);
        if (threads > 1) engine.setAudioThreads(threads);
        if (withCache) engine.setBackgroundFreezing(settings);
        sub::DeviceConfig config;
        config.driver = sub::ManualBackend::kName;
        config.sampleRate = kSampleRate;
        config.bufferFrames = kBuffer;
        engine.openDevice(config);
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() { engine.closeDevice(); }

    int64_t frames() const { return static_cast<int64_t>(out.size() / 2); }
    void mark() { marks.push_back(frames()); }
    // Plays on for about `seconds` (whole buffers), with the UI's idle() between,
    // and a round of the cache store's work after each buffer, as in real time
    // (its thread does one every 5 ms; this plays far faster).
    void run(double seconds) {
        sub::ManualBackend* device = sub::ManualBackend::current();
        REQUIRE(device != nullptr);
        const auto buffers = static_cast<int>(std::llround(seconds * kSampleRate / kBuffer));
        for (int b = 0; b < buffers; ++b) {
            device->process(1);
            engine.serviceBackgroundFreezing();
            if (b % 16 == 15) engine.idle();
        }
        const std::vector<float> played = device->takeOutput();
        out.insert(out.end(), played.begin(), played.end());
    }
    // What was captured is published (the store's thread does it every few ms anyway).
    void settle() { engine.serviceBackgroundFreezing(); }
    void locate(double seconds) { engine.setPositionBeats(seconds * 2.0); }  // (120 BPM)
    uint64_t fromCache(uint32_t strip) { return engine.backgroundFreezingStats(strip).framesFromCache; }
};

struct Outcome {
    std::vector<float> with, without;
    std::vector<int64_t> marks;
    int64_t frames() const { return static_cast<int64_t>(with.size() / 2); }
};

// The script with background freezing, then without.
template <typename Script>
Outcome both(Script&& script, int threads = 1, const sub::BackgroundFreezingSettings& settings = freezing()) {
    Outcome o;
    {
        Session s(true, settings, threads);
        script(s);
        o.with = std::move(s.out);
        o.marks = s.marks;
    }
    {
        Session s(false, settings, threads);
        script(s);
        o.without = std::move(s.out);
        CHECK(s.marks == o.marks);
    }
    REQUIRE(o.with.size() == o.without.size());
    return o;
}

// The first frame in [from, to) where the two differ; -1 if none.
int64_t firstDifference(const Outcome& o, int64_t from = 0, int64_t to = std::numeric_limits<int64_t>::max()) {
    to = std::min(to, o.frames());
    for (int64_t f = std::max<int64_t>(0, from); f < to; ++f) {
        const auto i = static_cast<size_t>(f) * 2;
        if (o.with[i] != o.without[i] || o.with[i + 1] != o.without[i + 1]) return f;
    }
    return -1;
}

float peak(const std::vector<float>& samples, int64_t from, int64_t to) {
    float most = 0.f;
    for (int64_t f = from; f < to && static_cast<size_t>(f) * 2 + 1 < samples.size(); ++f) {
        const auto i = static_cast<size_t>(f) * 2;
        most = std::max({most, std::fabs(samples[i]), std::fabs(samples[i + 1])});
    }
    return most;
}

// Frames `run(seconds)` plays.
uint64_t framesOf(double seconds) {
    return static_cast<uint64_t>(std::llround(seconds * kSampleRate / kBuffer)) * kBuffer;
}

// Six seconds of stereo noise (a seed for each).
std::string noiseWav(uint32_t seed) {
    std::vector<float> samples(static_cast<size_t>(6 * kSampleRate) * 2);
    uint32_t x = seed * 2654435761u + 1u;
    for (float& v : samples) {
        x = x * 1664525u + 1013904223u;
        v = static_cast<float>(static_cast<int32_t>(x >> 8) - (1 << 23)) / static_cast<float>(1 << 25);
    }
    return makeWav(samples, 2);
}

struct Wavs {
    std::string drums = noiseWav(1), bass = noiseWav(2);
};

// The test song. "drums": two clips of noise (0 to `split` seconds, then on to
// 6 s) through a Utility and a Delay (50 ms, no feedback, no filter) to the
// master; "bass": the same through a Utility into "group" (a Utility); a
// Utility on the master.
struct Song {
    uint32_t drums = 0, bass = 0, group = 0;
    uint32_t drumsGain = 0, delay = 0, bassGain = 0, groupGain = 0, masterGain = 0;
    double split = 3.0;
    std::string drumsWav, bassWav;
};

std::vector<sub::ClipDesc> twoClips(const std::string& path, double split, float secondGain = 1.f) {
    return {clip(path, 0.0, split), clip(path, split * 2.0, 6.0 - split, split, secondGain)};
}

Song makeSong(sub::Engine& engine, const Wavs& wavs, double split = 3.0) {
    Song song;
    song.split = split;
    song.drumsWav = wavs.drums;
    song.bassWav = wavs.bass;
    engine.loadSource(wavs.drums);
    engine.loadSource(wavs.bass);
    song.drums = engine.addTrack();
    song.bass = engine.addTrack();
    song.group = engine.addTrack();
    engine.setTrackClips(song.drums, twoClips(wavs.drums, split));
    engine.setTrackClips(song.bass, twoClips(wavs.bass, split));
    engine.setTrackOutput(song.bass, song.group);
    song.drumsGain = utilityOn(engine, engine.trackChain(song.drums), -3.f);
    song.delay = engine.addBuiltinProcessor(engine.trackChain(song.drums), "delay", -1);
    setParam(engine, song.delay, "mode", 2.f);  // Jump: a new time applies at once
    setParam(engine, song.delay, "l_sync", 0.f);
    setParam(engine, song.delay, "r_sync", 0.f);
    setParam(engine, song.delay, "l_time", 50.f);
    setParam(engine, song.delay, "r_time", 50.f);
    setParam(engine, song.delay, "feedback", 0.f);
    setParam(engine, song.delay, "filter", 0.f);
    setParam(engine, song.delay, "mix", 50.f);
    song.bassGain = utilityOn(engine, engine.trackChain(song.bass), 2.f);
    song.groupGain = utilityOn(engine, engine.trackChain(song.group), -2.f);
    song.masterGain = utilityOn(engine, engine.trackChain(sub::Engine::kMaster), -1.f);
    return song;
}

// Plays the song through once from the start (what is kept), and again from the start.
Song twoPasses(Session& s, const Wavs& wavs) {
    const Song song = makeSong(s.engine, wavs);
    s.engine.play();
    s.run(6.0);
    s.settle();
    s.locate(0.0);
    return song;
}

void checkSame(const Outcome& o) {
    CHECK_EQ(firstDifference(o), -1);
    CHECK(peak(o.without, 0, o.frames()) > 0.05f);  // (something played)
}

// The same but just after each mark (an edit where it plays, a locate where the
// devices stood idle): there the devices start cold, and the two are the same
// again once the delay has forgotten.
void checkSameButCold(const Outcome& o, size_t marks) {
    REQUIRE(o.marks.size() == marks);
    int64_t from = 0;
    for (const int64_t mark : o.marks) {
        CHECK_EQ(firstDifference(o, from, mark), -1);
        from = mark + kCold;
    }
    CHECK_EQ(firstDifference(o, from), -1);
    CHECK(peak(o.without, 0, o.frames()) > 0.05f);
}

// --- Scripts that run on one thread and on several ---------------------------------

void secondPassFromTheCache(Session& s, const Wavs& wavs) {
    const Song song = twoPasses(s, wavs);
    const uint64_t drums = s.fromCache(song.drums), group = s.fromCache(song.group);
    const uint64_t master = s.fromCache(sub::Engine::kMaster);
    s.run(6.0);
    if (!s.freezing) return;
    // All of it but the warm-ups after the locate (one more for each strip on
    // the way: the group's input, the master's, warm up first), and before the
    // end of what was kept (where the devices start again, to be warm there).
    CHECK(s.fromCache(song.drums) - drums >= framesOf(6.0 - 2 * kWarm) - 4 * kBuffer);
    CHECK(s.fromCache(song.group) - group >= framesOf(6.0 - 3 * kWarm) - 4 * kBuffer);
    CHECK(s.fromCache(sub::Engine::kMaster) - master >= framesOf(6.0 - 4 * kWarm) - 4 * kBuffer);
    const sub::BackgroundFreezingStats stats = s.engine.backgroundFreezingStats();
    CHECK(stats.blocks > 0);
    CHECK(stats.bytes > 0);
}

void editAhead(Session& s, const Wavs& wavs) {
    const Song song = twoPasses(s, wavs);
    s.run(1.0);
    // The second clip (from 3 s) changes, 2 s ahead.
    s.engine.setTrackClips(song.drums, twoClips(wavs.drums, song.split, 0.5f));
    s.engine.setTrackClips(song.bass, twoClips(wavs.bass, song.split, 0.5f));
    const uint64_t drums = s.fromCache(song.drums), group = s.fromCache(song.group);
    s.run(1.5);
    if (s.freezing) {  // up to a warm-up before it, the cache still plays
        CHECK_EQ(s.fromCache(song.drums) - drums, framesOf(1.5));
        CHECK_EQ(s.fromCache(song.group) - group, framesOf(1.5));
    }
    s.run(3.0);  // past it: the devices, warmed up while the cache played
}

}  // namespace

// --- Playing from the cache -------------------------------------------------------------

TEST_CASE("background freezing: a second pass plays from the cache, sample for sample") {
    const Wavs wavs;
    checkSame(both([&](Session& s) { secondPassFromTheCache(s, wavs); }));
}

TEST_CASE("background freezing: on workers, a second pass plays from the cache") {
    const Wavs wavs;
    checkSame(both([&](Session& s) { secondPassFromTheCache(s, wavs); }, 4));
}

TEST_CASE("background freezing: what plays after a jump is kept only once warm") {
    // The first pass starts at 2 s: the delay starts empty there, as it doesn't
    // when playing through from the start. What it put out in its first 50 ms
    // isn't kept, so the second pass, through from the start, hears the echo.
    const Wavs wavs;
    checkSame(both([&](Session& s) {
        const Song song = makeSong(s.engine, wavs);
        s.locate(2.0);
        s.engine.play();
        s.run(4.0);
        s.settle();
        s.locate(0.0);
        const uint64_t drums = s.fromCache(song.drums);
        s.run(6.0);
        if (s.freezing) CHECK(s.fromCache(song.drums) - drums > framesOf(3.0));
    }));
}

TEST_CASE("background freezing: a track the UI shows plays live") {
    const Wavs wavs;
    checkSame(both([&](Session& s) {
        const Song song = twoPasses(s, wavs);
        s.engine.setTrackObserved(song.drums, true);
        const uint64_t drums = s.fromCache(song.drums), group = s.fromCache(song.group);
        s.run(6.0);
        if (!s.freezing) return;
        CHECK_EQ(s.fromCache(song.drums), drums);
        CHECK(s.fromCache(song.group) - group > framesOf(5.0));
    }));
}

TEST_CASE("background freezing: without memory for it, everything plays live") {
    const Wavs wavs;
    checkSame(both(
        [&](Session& s) {
            const Song song = twoPasses(s, wavs);
            s.run(6.0);
            if (!s.freezing) return;
            CHECK_EQ(s.fromCache(song.drums), uint64_t{0});
            CHECK_EQ(s.engine.backgroundFreezingStats().blocks, size_t{0});
        },
        1, freezing(true, 0.0)));
}

TEST_CASE("background freezing: switched off it drops the cache, on again it keeps anew") {
    const Wavs wavs;
    checkSame(both([&](Session& s) {
        const Song song = twoPasses(s, wavs);
        if (s.freezing) {
            CHECK(s.engine.backgroundFreezingStats().blocks > 0);
            s.engine.setBackgroundFreezing(freezing(false));
            s.settle();
            CHECK_EQ(s.engine.backgroundFreezingStats().blocks, size_t{0});
        }
        const uint64_t off = s.fromCache(song.drums);
        s.run(6.0);  // off: live, kept nowhere
        if (s.freezing) {
            CHECK_EQ(s.fromCache(song.drums), off);
            s.engine.setBackgroundFreezing(freezing(true));
        }
        s.locate(0.0);
        s.run(6.0);  // on: kept
        s.settle();
        s.locate(0.0);
        const uint64_t on = s.fromCache(song.drums);
        s.run(6.0);  // from the cache
        if (s.freezing) CHECK(s.fromCache(song.drums) - on > framesOf(5.0));
    }));
}

// --- Edits --------------------------------------------------------------------------

TEST_CASE("background freezing: an edit ahead plays where it is, warmed up while the cache played") {
    const Wavs wavs;
    checkSame(both([&](Session& s) { editAhead(s, wavs); }));
}

TEST_CASE("background freezing: on workers, an edit ahead plays where it is") {
    const Wavs wavs;
    checkSame(both([&](Session& s) { editAhead(s, wavs); }, 4));
}

TEST_CASE("background freezing: an edit behind the playhead doesn't stop the cache") {
    const Wavs wavs;
    checkSameButCold(both([&](Session& s) {
        const Song song = twoPasses(s, wavs);
        s.run(4.0);
        // The first clip (to 3 s) changes, behind the playhead.
        std::vector<sub::ClipDesc> clips = twoClips(wavs.drums, song.split);
        clips[0].gain = 0.5f;
        s.engine.setTrackClips(song.drums, clips);
        const uint64_t drums = s.fromCache(song.drums), master = s.fromCache(sub::Engine::kMaster);
        s.run(1.0);
        if (s.freezing) {
            CHECK_EQ(s.fromCache(song.drums) - drums, framesOf(1.0));
            CHECK_EQ(s.fromCache(sub::Engine::kMaster) - master, framesOf(1.0));
        }
        s.mark();
        s.locate(0.0);
        s.run(6.0);  // the edit plays; after it, the cache again
    }), 1);
}

TEST_CASE("background freezing: an envelope's change plays where it changes") {
    const Wavs wavs;
    // Utility gain, normalized: -3 dB is (60 - 3) / 84.
    const float level = 57.f / 84.f;
    const auto lane = [&](const Song& song, bool dip) {
        sub::AutomationLaneDesc l;
        l.processorId = song.drumsGain;
        l.param = "gain";
        l.points = {{0.0, level}, {6.0, level}, {10.0, level}, {12.0, level}};
        if (dip) l.points.insert(l.points.begin() + 2, {8.0, 0.5f});  // 3 s to 5 s
        return std::vector<sub::AutomationLaneDesc>{l};
    };
    checkSame(both([&](Session& s) {
        const Song song = makeSong(s.engine, wavs);
        s.engine.setTrackAutomation(song.drums, lane(song, false));
        s.engine.play();
        s.run(6.0);
        s.settle();
        s.locate(0.0);
        s.run(1.0);
        s.engine.setTrackAutomation(song.drums, lane(song, true));
        const uint64_t drums = s.fromCache(song.drums);
        s.run(1.5);
        if (s.freezing) CHECK_EQ(s.fromCache(song.drums) - drums, framesOf(1.5));
        s.run(3.5);
    }));
}

TEST_CASE("background freezing: an edit at the loop's end changes what plays after the wrap") {
    // Looping 1 s to 3 s, the second clips starting at 2.9 s: the delay carries
    // their last 50 ms over the wrap. Changed, what was kept after the wrap is
    // no good, though nothing changed there.
    const Wavs wavs;
    checkSame(both([&](Session& s) {
        const Song song = makeSong(s.engine, wavs, 2.9);
        s.engine.setLoop(true, 2.0, 6.0);
        s.locate(1.0);
        s.engine.play();
        s.run(6.0);  // three times round
        s.settle();
        const uint64_t drums = s.fromCache(song.drums);
        s.run(4.0);
        if (s.freezing) CHECK(s.fromCache(song.drums) - drums > framesOf(3.0));
        s.run(0.5);
        s.engine.setTrackClips(song.drums, twoClips(wavs.drums, song.split, 0.5f));
        s.run(4.0);
    }));
}

TEST_CASE("background freezing: a change where it plays plays at once") {
    const Wavs wavs;
    const std::vector<std::string> edits = {"parameter", "state",  "switched off", "removed", "added",
                                            "rerouted",  "fader upstream", "tempo"};
    for (const std::string& edit : edits) {
        INFO("edit: " + edit);
        checkSameButCold(both([&](Session& s) {
            const Song song = makeSong(s.engine, wavs);
            setParam(s.engine, song.drumsGain, "gain", -12.f);
            const std::vector<uint8_t> quieter = s.engine.processorState(song.drumsGain);
            setParam(s.engine, song.drumsGain, "gain", -3.f);
            s.engine.play();
            s.run(6.0);
            s.settle();
            s.locate(0.0);
            s.run(2.0);
            s.mark();
            if (edit == "parameter") setParam(s.engine, song.drumsGain, "gain", -9.f);
            if (edit == "state") s.engine.setProcessorState(song.drumsGain, quieter);
            if (edit == "switched off") s.engine.setProcessorEnabled(song.delay, false);
            if (edit == "removed") s.engine.removeProcessor(song.delay);
            if (edit == "added") utilityOn(s.engine, s.engine.trackChain(song.drums), 4.f);
            if (edit == "rerouted") s.engine.setTrackOutput(song.drums, song.group);
            if (edit == "fader upstream") s.engine.setTrackGain(song.bass, 0.5f);
            if (edit == "tempo") s.engine.setTempo(100.0);
            s.run(2.0);
            s.settle();
            s.mark();
            s.locate(0.0);
            s.run(4.0);  // what plays now is kept anew
        }), 2);
    }
}

// --- Instruments ---------------------------------------------------------------------

namespace {

enum { SYNTH_GAIN, SYNTH_WAVE };  // SUB Test Synth's parameters

// SUB Test Synth (DC or sines) playing `notes` into the master.
uint32_t synthTrack(sub::Engine& engine, bool sine, const std::vector<sub::NoteDesc>& notes) {
    const uint32_t track = engine.addTrack();
    const uint32_t synth = addTestPlugin(engine, engine.trackChain(track), "SUB Test Synth");
    engine.setProcessorParam(synth, SYNTH_WAVE, sine ? 1.f : 0.f);
    engine.setProcessorParam(synth, SYNTH_GAIN, 0.25f);
    engine.setTrackNotes(track, notes);
    return track;
}

}  // namespace

TEST_CASE("background freezing: notes held where the instrument plays again sound on") {
    // A long note (0 to 4 s) and short ones; ahead, one short note changes, so
    // the instrument starts again while the long note is held.
    requireTestPlugins();
    const auto notes = [](int changedVelocity) {
        std::vector<sub::NoteDesc> n = {{0.0, 8.0, 48, 100}};
        for (int i = 0; i < 12; ++i) n.push_back({i * 1.0, 0.5, 60 + i, i == 7 ? changedVelocity : 80});
        return n;
    };
    checkSame(both([&](Session& s) {
        const uint32_t track = synthTrack(s.engine, false, notes(80));
        s.engine.play();
        s.run(6.0);
        s.settle();
        s.locate(0.0);
        s.run(1.0);
        s.engine.setTrackNotes(track, notes(40));  // the note at 3.5 s
        const uint64_t cached = s.fromCache(track);
        s.run(1.5);
        if (s.freezing) CHECK_EQ(s.fromCache(track) - cached, framesOf(1.5));
        s.run(3.5);
    }));
}

TEST_CASE("background freezing: a note held where playing starts is kept only once it ends") {
    // Sines: a note started late (where playing starts, in the middle of it) is
    // out of phase with the arrangement's, so nothing is kept until it ends.
    requireTestPlugins();
    checkSame(both([&](Session& s) {
        const uint32_t track = synthTrack(s.engine, true, {{0.0, 8.0, 57, 100}, {9.0, 1.0, 64, 100}});
        s.locate(2.0);
        s.engine.play();  // in the middle of the long note
        s.run(4.0);
        s.settle();
        s.locate(0.0);
        const uint64_t cached = s.fromCache(track);
        s.run(6.0);
        if (s.freezing) CHECK(s.fromCache(track) - cached > framesOf(1.0));  // (from 4.25 s)
    }));
}
