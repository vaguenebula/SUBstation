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
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include "Engine.h"
#include "backends/ManualBackend.h"
#include "cache/CacheStore.h"
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
    settings.render = false;  // (phase 1: what plays live is kept; background rendering below)
    return settings;
}

// With background rendering: renderInBackground() renders (`thread`: its own thread too).
sub::BackgroundFreezingSettings rendering(bool thread = false, double budgetMB = 1024.0) {
    sub::BackgroundFreezingSettings settings = freezing(true, budgetMB);
    settings.render = true;
    settings.renderThread = thread;
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

TEST_CASE("background freezing: what it holds stays within its budget, not yet freed too") {
    // 3 MB: a couple of dozen blocks for four strips, while edits make what was
    // kept no good (it goes, freed once no callback can still see it), and a
    // strip goes (its empty blocks are freed with the last snapshot showing it).
    const Wavs wavs;
    const auto budget = static_cast<size_t>(3.0 * 1024 * 1024);
    checkSameButCold(both(
        [&](Session& s) {
            const Song song = twoPasses(s, wavs);
            const auto check = [&] {
                if (!s.freezing) return;
                const sub::BackgroundFreezingStats stats = s.engine.backgroundFreezingStats();
                CHECK(stats.bytes + stats.unfreedBytes <= budget);
            };
            for (int i = 0; i < 20; ++i) {
                s.run(0.25);
                check();
            }
            s.mark();
            setParam(s.engine, song.drumsGain, "gain", -9.f);
            s.engine.removeTrack(song.bass);
            for (int i = 0; i < 16; ++i) {  // (before idle() lets go of the snapshot still showing the bass)
                s.run(static_cast<double>(kBuffer) / kSampleRate);
                check();
            }
            for (int i = 0; i < 20; ++i) {
                s.run(0.25);
                check();
            }
            if (s.freezing) CHECK(s.fromCache(song.group) > 0);
        },
        1, freezing(true, 3.0)), 1);
}

TEST_CASE("background freezing: the store hands out blocks within its budget, counting what isn't freed") {
    // The store alone, with no audio callbacks finishing (the epoch stands
    // still): what it lets go of can't be freed yet, and still counts.
    constexpr size_t kBlock = sizeof(float) * 2 * sub::kCacheBlockFrames;
    sub::CacheSettings settings;
    settings.enabled = true;
    settings.idleSeconds = 0.0;
    settings.budgetBytes = static_cast<int64_t>(4 * kBlock);
    std::atomic<uint64_t> epoch{1};
    std::atomic<bool> running{true};
    std::atomic<uint64_t> backgroundEpoch{0};
    std::atomic<bool> backgroundBusy{false};
    std::atomic<int64_t> playhead{0};
    sub::CacheStore store(settings, epoch, running, backgroundEpoch, backgroundBusy, playhead);
    auto point = std::make_shared<sub::CachePoint>();
    store.setPoints({{point, nullptr}});
    store.service();
    CHECK_EQ(store.stats().bytes, 2 * kBlock);  // two empty blocks to capture into
    // Three captured: kept, and empty ones handed out while the budget allows.
    for (int64_t cell = 0; cell < 3; ++cell) {
        sub::CacheBlock* block = nullptr;
        REQUIRE(point->live().spares.pop(block));
        block->cell = cell;
        block->from = 0;
        block->to = static_cast<int>(sub::kCacheBlockFrames);
        block->version = point->version.load();
        std::fill_n(block->samples.get(), 2 * sub::kCacheBlockFrames, 0.25f);
        REQUIRE(point->live().completed.push(block));
        store.service();
    }
    CHECK_EQ(store.stats().blocks, size_t{3});
    CHECK_EQ(store.stats().bytes, 4 * kBlock);  // three kept, one to capture into
    // No longer good: they go once a seam can't fade out of them any more, but
    // aren't freed while a callback may still read them. Nothing new meanwhile.
    point->version.fetch_add(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    store.service();
    sub::CacheStore::Stats stats = store.stats();
    CHECK_EQ(stats.blocks, size_t{0});
    CHECK_EQ(stats.unfreedBytes, 3 * kBlock);
    CHECK(stats.bytes + stats.unfreedBytes <= 4 * kBlock);
    // A callback finished: they are freed, and empty blocks handed out again.
    epoch.fetch_add(1);
    store.service();
    stats = store.stats();
    CHECK_EQ(stats.unfreedBytes, size_t{0});
    CHECK_EQ(stats.bytes, 2 * kBlock);
    // A strip gone: its empty blocks count until the last snapshot showing it lets go.
    std::shared_ptr<sub::CachePoint> snapshot = point;
    point.reset();
    store.setPoints({});
    store.service();
    stats = store.stats();
    CHECK_EQ(stats.bytes, size_t{0});
    CHECK_EQ(stats.unfreedBytes, 2 * kBlock);
    snapshot.reset();
    store.service();
    CHECK_EQ(store.stats().unfreedBytes, size_t{0});
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

TEST_CASE("background freezing: devices reset while idle start again where an edit lands") {
    // Idle over two seconds, the drums' devices are reset offline (idle()); an
    // edit where it plays then starts them again at once.
    const Wavs wavs;
    checkSameButCold(both([&](Session& s) {
        const Song song = twoPasses(s, wavs);
        s.run(2.0);  // from the cache
        std::this_thread::sleep_for(std::chrono::milliseconds(2100));
        s.engine.idle();
        s.mark();
        setParam(s.engine, song.drumsGain, "gain", -9.f);
        s.run(2.0);
        if (s.freezing) CHECK(s.engine.backgroundFreezingStats(song.drums).framesLive > 0);
    }), 1);
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

TEST_CASE("background freezing: an armed instrument plays from the cache while no MIDI comes in") {
    // Armed with Auto monitoring, it hears its MIDI input; while none comes,
    // its clips' notes are all it plays: the arrangement's.
    requireTestPlugins();
    checkSame(both([&](Session& s) {
        std::vector<sub::NoteDesc> notes;
        for (int i = 0; i < 12; ++i) notes.push_back({i * 1.0, 0.5, 60 + i, 80});
        const uint32_t track = synthTrack(s.engine, false, notes);
        s.engine.setTrackMidiInput(track, true, "", 0);
        s.engine.setTrackArmed(track, true);
        s.engine.play();
        s.run(6.0);
        s.settle();
        s.locate(0.0);
        const uint64_t cached = s.fromCache(track);
        s.run(5.0);
        if (s.freezing) CHECK(s.fromCache(track) - cached >= framesOf(5.0 - kWarm) - 4 * kBuffer);
    }));
}

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

// --- Latency ------------------------------------------------------------------------

namespace {

enum { FX_GAIN, FX_LATENCY };    // SUB Test Effect's parameters
constexpr int kFxLatency = 4096;  // samples (85 ms): the most it takes, far longer than the warm-up below

// A track playing the two clips of noise through SUB Test Effect, kFxLatency late.
uint32_t latentTrack(sub::Engine& engine, const Wavs& wavs) {
    engine.loadSource(wavs.drums);
    const uint32_t track = engine.addTrack();
    engine.setTrackClips(track, twoClips(wavs.drums, 3.0));
    const uint32_t effect = addTestPlugin(engine, engine.trackChain(track), "SUB Test Effect");
    engine.setProcessorParam(effect, FX_LATENCY, static_cast<float>(kFxLatency));
    engine.idle();  // the plug-in asked for a restart to change its latency
    REQUIRE(engine.processorInfo(effect).latency == kFxLatency);
    return track;
}

// A warm-up shorter than the latency (the effect remembers nothing else).
sub::BackgroundFreezingSettings shortWarmUp() {
    sub::BackgroundFreezingSettings settings = freezing();
    settings.warmSeconds = 0.01;
    return settings;
}

}  // namespace

TEST_CASE("background freezing: an edit reaches as far as a device's latency carries it") {
    // The first clip (to 3 s) changes: what comes out of the effect changes
    // until 3 s and its latency, longer than the warm-up.
    requireTestPlugins();
    const Wavs wavs;
    checkSameButCold(both(
        [&](Session& s) {
            const uint32_t track = latentTrack(s.engine, wavs);
            s.engine.play();
            s.run(6.0);
            s.settle();
            s.locate(0.0);
            s.run(4.0);  // from the cache
            std::vector<sub::ClipDesc> clips = twoClips(wavs.drums, 3.0);
            clips[0].gain = 0.5f;
            s.engine.setTrackClips(track, clips);
            s.mark();
            s.locate(0.0);
            s.run(5.0);
        },
        1, shortWarmUp()), 1);
}

TEST_CASE("background freezing: what a latent device still holds after a jump isn't kept") {
    // Playing starts at 2 s: the effect puts out its empty latency first, as it
    // doesn't playing through from the start. None of that is kept.
    requireTestPlugins();
    const Wavs wavs;
    checkSame(both(
        [&](Session& s) {
            const uint32_t track = latentTrack(s.engine, wavs);
            s.locate(2.0);
            s.engine.play();
            s.run(3.0);
            s.settle();
            s.locate(0.0);
            const uint64_t cached = s.fromCache(track);
            s.run(6.0);
            if (s.freezing) CHECK(s.fromCache(track) - cached > framesOf(2.0));
        },
        1, shortWarmUp()));
}

// --- Background rendering (phase 2) -------------------------------------------------

namespace {

// Renders in the background now, until there is nothing left to render (or `seconds`).
void renderAll(Session& s, double seconds = 30.0) {
    if (!s.freezing) return;
    s.engine.renderInBackground(static_cast<int64_t>(seconds * kSampleRate));
    s.settle();
}

// The song, with its devices settled on the values it gave them: the live ones
// ramp to them the first time they run, shadows are prepared with them (and
// the background renders from before the start: the start is cached too).
Song settledSong(Session& s, const Wavs& wavs) {
    const Song song = makeSong(s.engine, wavs);
    s.run(0.1);  // (stopped: they run on silence)
    return song;
}

}  // namespace

TEST_CASE("background rendering: the first pass plays from the cache, sample for sample") {
    // Rendered in the background before playing (from a warm-up before the
    // start), every strip plays from the cache the first time it plays.
    const Wavs wavs;
    checkSame(both(
        [&](Session& s) {
            const Song song = settledSong(s, wavs);
            renderAll(s);
            s.engine.play();
            const uint64_t drums = s.fromCache(song.drums), group = s.fromCache(song.group);
            const uint64_t master = s.fromCache(sub::Engine::kMaster);
            s.run(6.0);
            if (!s.freezing) return;
            const sub::BackgroundFreezingStats stats = s.engine.backgroundFreezingStats();
            CHECK(stats.framesRendered > 0);
            CHECK(stats.shadows >= 4);  // (drums' two devices, the bass's, the group's, the master's)
            // (All of it: it warmed up before the start.)
            CHECK(s.fromCache(song.drums) - drums >= framesOf(6.0) - 4 * kBuffer);
            CHECK(s.fromCache(song.group) - group >= framesOf(6.0) - 4 * kBuffer);
            CHECK(s.fromCache(sub::Engine::kMaster) - master >= framesOf(6.0) - 4 * kBuffer);
        },
        1, rendering()));
}

TEST_CASE("background rendering: an edit ahead is rendered again before it plays") {
    const Wavs wavs;
    checkSame(both(
        [&](Session& s) {
            const Song song = settledSong(s, wavs);
            renderAll(s);
            s.engine.play();
            s.run(1.0);
            // The second clips (from 3 s) change; the background renders them again.
            s.engine.setTrackClips(song.drums, twoClips(wavs.drums, song.split, 0.5f));
            s.engine.setTrackClips(song.bass, twoClips(wavs.bass, song.split, 0.5f));
            renderAll(s);
            const uint64_t drums = s.fromCache(song.drums), group = s.fromCache(song.group);
            s.run(5.0);
            if (!s.freezing) return;
            CHECK_EQ(s.fromCache(song.drums) - drums, framesOf(5.0));  // through the edit: never live
            CHECK_EQ(s.fromCache(song.group) - group, framesOf(5.0));
        },
        1, rendering()));
}

TEST_CASE("background rendering: a device's new parameter and state are rendered again") {
    const Wavs wavs;
    for (const std::string edit : {"parameter", "state"}) {
        INFO("edit: " + edit);
        checkSameButCold(both(
            [&](Session& s) {
                const Song song = settledSong(s, wavs);
                setParam(s.engine, song.drumsGain, "gain", -12.f);
                const std::vector<uint8_t> quieter = s.engine.processorState(song.drumsGain);
                setParam(s.engine, song.drumsGain, "gain", -3.f);
                renderAll(s);
                s.engine.play();
                s.run(2.0);
                s.mark();
                if (edit == "parameter") setParam(s.engine, song.drumsGain, "gain", -9.f);
                if (edit == "state") s.engine.setProcessorState(song.drumsGain, quieter);
                s.run(0.25);  // (live, cold, meanwhile)
                renderAll(s);
                const uint64_t drums = s.fromCache(song.drums);
                s.run(3.0);
                // From half a second on (what plays sooner, the live renderer keeps itself).
                if (s.freezing) CHECK(s.fromCache(song.drums) - drums >= framesOf(2.5) - 4 * kBuffer);
            },
            1, rendering()), 1);
    }
}

TEST_CASE("background rendering: plug-ins have shadows too") {
    requireTestPlugins();
    const Wavs wavs;
    checkSame(both(
        [&](Session& s) {
            s.engine.loadSource(wavs.drums);
            const uint32_t track = s.engine.addTrack();
            s.engine.setTrackClips(track, twoClips(wavs.drums, 3.0));
            const uint32_t effect = addTestPlugin(s.engine, s.engine.trackChain(track), "SUB Test Effect");
            s.engine.setProcessorParam(effect, FX_GAIN, 0.3f);
            renderAll(s);
            s.engine.play();
            const uint64_t cached = s.fromCache(track);
            s.run(6.0);
            if (!s.freezing) return;
            CHECK(s.engine.backgroundFreezingStats().shadows >= 1);
            CHECK(s.fromCache(track) - cached >= framesOf(6.0 - 2 * kWarm) - 4 * kBuffer);
        },
        1, rendering()));
}

TEST_CASE("background rendering: on its own thread, while it plays") {
    // The background renderer's thread renders as the first pass plays (it may
    // or may not have got far): what plays is the same either way.
    const Wavs wavs;
    checkSameButCold(both(
        [&](Session& s) {
            const Song song = settledSong(s, wavs);
            if (s.freezing) {
                // Until it has rendered some (it starts once edits have settled: a second).
                const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (s.engine.backgroundFreezingStats().framesRendered == 0 &&
                       std::chrono::steady_clock::now() < until) {
                    s.engine.idle();
                    s.settle();
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                }
                CHECK(s.engine.backgroundFreezingStats().framesRendered > 0);
            }
            s.engine.play();
            s.run(6.0);
            s.mark();  // (a locate where devices stood idle: they start cold)
            s.locate(0.0);
            s.run(6.0);
            if (s.freezing) CHECK(s.fromCache(song.drums) > 0);
        },
        1, rendering(true)), 1);
}

TEST_CASE("background rendering: switched off, the shadows go") {
    const Wavs wavs;
    Session s(true, rendering(), 1);
    settledSong(s, wavs);
    renderAll(s);
    CHECK(s.engine.backgroundFreezingStats().shadows > 0);
    s.engine.setBackgroundFreezing(freezing());  // (render: false)
    s.engine.idle();
    CHECK_EQ(s.engine.backgroundFreezingStats().shadows, size_t{0});
    s.engine.setBackgroundFreezing(rendering());
    renderAll(s);
    CHECK(s.engine.backgroundFreezingStats().shadows > 0);
    s.engine.setBackgroundFreezing(freezing(false));
    s.engine.idle();
    CHECK_EQ(s.engine.backgroundFreezingStats().shadows, size_t{0});
}


TEST_CASE("background rendering: it renders what isn't cached, then nothing") {
    // From a warm-up per strip on the way to the master before the start
    // (bass, group, master) to a warm-up past the song's end.
    const Wavs wavs;
    Session s(true, rendering(), 1);
    settledSong(s, wavs);
    const int64_t first = s.engine.renderInBackground(framesOf(30.0));
    s.settle();
    CHECK(first >= static_cast<int64_t>(framesOf(6.0)));
    CHECK(first <= static_cast<int64_t>(framesOf(6.0 + 4 * kWarm)) + 4096);
    CHECK_EQ(s.engine.renderInBackground(framesOf(30.0)), 0);
}

TEST_CASE("background rendering: with the budget spent, it stops") {
    // Room for a few blocks (the song needs dozens): what it renders past them
    // isn't kept, so it stops (and waits a while before it tries again).
    const Wavs wavs;
    Session s(true, rendering(false, 1.0), 1);
    settledSong(s, wavs);
    const int64_t rendered = s.engine.renderInBackground(framesOf(60.0));
    CHECK(rendered > 0);
    CHECK(rendered < static_cast<int64_t>(framesOf(15.0)));
    s.settle();
    const sub::BackgroundFreezingStats stats = s.engine.backgroundFreezingStats();
    CHECK(stats.bytes + stats.unfreedBytes <= static_cast<size_t>(1024 * 1024));
}

TEST_CASE("background rendering: where a note is held, it starts before it") {
    // Two instruments; a short note of each changes, at 1 s and at 4.5 s. On
    // the first, a long note (0 to 4 s) is held there: its devices, starting
    // again while the cache plays, would come clean only where it ends, so the
    // background starts them before it (at the song's start here) and keeps
    // that too. It goes on to the second.
    requireTestPlugins();
    const auto heldNotes = [](int velocity) {
        std::vector<sub::NoteDesc> n = {{0.0, 8.0, 48, 100}};  // (beats: 120 BPM)
        for (int i = 0; i < 8; ++i) n.push_back({i * 1.0, 0.5, 60 + i, i == 2 ? velocity : 80});
        return n;
    };
    const auto otherNotes = [](int velocity) {
        return std::vector<sub::NoteDesc>{{0.0, 1.0, 60, 80}, {9.0, 1.0, 62, velocity}, {11.0, 1.0, 64, 80}};
    };
    checkSame(both(
        [&](Session& s) {
            const uint32_t held = synthTrack(s.engine, false, heldNotes(80));
            const uint32_t other = synthTrack(s.engine, false, otherNotes(80));
            s.run(0.1);  // (stopped: settled)
            renderAll(s);
            s.engine.setTrackNotes(held, heldNotes(40));
            s.engine.setTrackNotes(other, otherNotes(40));
            const int64_t rendered = s.freezing ? s.engine.renderInBackground(framesOf(60.0)) : 0;
            s.settle();
            s.engine.play();
            const uint64_t heldCached = s.fromCache(held), otherCached = s.fromCache(other);
            s.run(5.5);
            if (!s.freezing) return;
            CHECK(rendered < static_cast<int64_t>(framesOf(20.0)));
            CHECK_EQ(s.fromCache(other) - otherCached, framesOf(5.5));  // through its edit
            CHECK_EQ(s.fromCache(held) - heldCached, framesOf(5.5));    // ... and this one through its own
        },
        1, rendering()));
}

TEST_CASE("background rendering: while it plays, an edit behind the playhead is rendered too") {
    // Nothing is left to render ahead, so it goes back to the edit: it renders
    // there until it is done, though the playhead is far past it.
    const Wavs wavs;
    checkSameButCold(both(
        [&](Session& s) {
            const Song song = settledSong(s, wavs);
            renderAll(s);
            s.engine.play();
            s.run(4.0);
            std::vector<sub::ClipDesc> clips = twoClips(wavs.drums, song.split);
            clips[0].gain = 0.5f;  // (to 3 s: what plays here stays as it is)
            s.engine.setTrackClips(song.drums, clips);
            const int64_t rendered = s.freezing ? s.engine.renderInBackground(framesOf(30.0)) : 0;
            s.settle();
            s.mark();  // (what the delay still holds from before the jump goes on without the cache)
            s.locate(0.0);
            const uint64_t drums = s.fromCache(song.drums);
            s.run(5.5);
            if (!s.freezing) return;
            CHECK(rendered < static_cast<int64_t>(framesOf(15.0)));
            CHECK_EQ(s.fromCache(song.drums) - drums, framesOf(5.5));
        },
        1, rendering()),
        1);
}

TEST_CASE("background rendering: inside an edit's ringing, it starts early enough") {
    // The bass changes from 1 to 1.25 s (a warm-up is 1 s here): the bass to
    // 2.25 s, the group to 3.25 s, the master to 4.25 s. Stopped at 3.5 s, the
    // first gap from the playhead on is the master's alone; for it to be kept
    // there, the group and the bass must start before the bass's own change,
    // though neither has a gap near 3.5 s.
    const Wavs wavs;
    sub::BackgroundFreezingSettings settings = rendering();
    settings.warmSeconds = 1.0;
    const auto clips = [&](float gain) {
        return std::vector<sub::ClipDesc>{clip(wavs.bass, 0.0, 1.0), clip(wavs.bass, 2.0, 0.25, 1.0, gain),
                                          clip(wavs.bass, 2.5, 4.75, 1.25)};
    };
    checkSameButCold(both(
                         [&](Session& s) {
                             const Song song = settledSong(s, wavs);
                             s.engine.setTrackClips(song.bass, clips(1.f));
                             renderAll(s);
                             s.engine.play();
                             s.run(3.5);
                             s.mark();  // (stopping cuts what was playing from the cache short)
                             s.engine.stop();
                             s.run(0.1);
                             s.engine.setTrackClips(song.bass, clips(0.5f));
                             if (s.freezing) s.engine.renderInBackground(framesOf(30.0));
                             s.settle();
                             s.mark();  // (what the delay still holds from before goes on without the cache)
                             s.locate(0.0);
                             s.engine.play();
                             const uint64_t master = s.fromCache(sub::Engine::kMaster);
                             s.run(5.5);
                             if (s.freezing) CHECK_EQ(s.fromCache(sub::Engine::kMaster) - master, framesOf(5.5));
                         },
                         1, settings),
                     2);
}

namespace {

// The drums' second clip is cut short: a short one (2 to 2.5 s) of gain `gain` between.
std::vector<sub::ClipDesc> shortClipBetween(const Wavs& wavs, float gain) {
    return {clip(wavs.drums, 0.0, 2.0), clip(wavs.drums, 4.0, 0.5, 2.0, gain), clip(wavs.drums, 5.0, 3.5, 2.5)};
}

}  // namespace

TEST_CASE("background rendering: once a change has rung out, what it had is kept, not rendered again") {
    // The short clip of the drums (2 to 2.5 s) changes. Through the 50 ms delay
    // it rings on a little; then what the drums put out matches what they put
    // out before. The first time, the background renders on for the whole
    // warm-up (2 s here), and learns how long a change rings on in the drums
    // (and in the master). The second time, from where the drums match again
    // for long enough, it keeps the blocks it had (in the drums, and then in
    // the master) instead of rendering on.
    const Wavs wavs;
    sub::BackgroundFreezingSettings settings = rendering();
    settings.warmSeconds = 2.0;
    checkSame(both(
        [&](Session& s) {
            const Song song = settledSong(s, wavs);
            s.engine.setTrackClips(song.drums, shortClipBetween(wavs, 1.f));
            renderAll(s);
            const sub::BackgroundFreezingStats first = s.engine.backgroundFreezingStats();
            s.engine.setTrackClips(song.drums, shortClipBetween(wavs, 0.5f));
            renderAll(s);
            const sub::BackgroundFreezingStats second = s.engine.backgroundFreezingStats();
            s.engine.setTrackClips(song.drums, shortClipBetween(wavs, 0.25f));
            renderAll(s);
            const sub::BackgroundFreezingStats third = s.engine.backgroundFreezingStats();
            s.engine.play();
            const uint64_t drums = s.fromCache(song.drums), master = s.fromCache(sub::Engine::kMaster);
            s.run(5.5);  // (then they start again a warm-up before what was kept ends)
            if (!s.freezing) return;
            CHECK(second.framesReplayed - first.framesReplayed < framesOf(1.0));  // (only once it had learnt)
            CHECK(third.framesReplayed - second.framesReplayed > framesOf(4.0));
            // The drums ran from a warm-up before the clip to just after it, the master as long again
            // (it waits for the drums); the rest of their warm-ups (2 s and 4 s) was kept from what they had.
            CHECK(second.framesRenderedLive - first.framesRenderedLive > framesOf(2 * (2.0 + 0.5 + 2.0)));
            CHECK(third.framesRenderedLive - second.framesRenderedLive < framesOf(2 * (2.0 + 0.5 + 0.5)));
            CHECK_EQ(s.fromCache(song.drums) - drums, framesOf(5.5));
            CHECK_EQ(s.fromCache(sub::Engine::kMaster) - master, framesOf(5.5));
        },
        1, settings));
}

TEST_CASE("background rendering: a change that rings on unheard a while isn't taken to have rung out") {
    // The delay is 1 s: what the short clip changes comes back a second later,
    // after half a second that matches. The second change shows it (the first
    // is before anything was kept); after the third, the drums must match for
    // 2 s, which the warm-up (3 s here) leaves no room for: what plays is right.
    const Wavs wavs;
    sub::BackgroundFreezingSettings settings = rendering();
    settings.warmSeconds = 3.0;
    checkSame(both(
        [&](Session& s) {
            const Song song = settledSong(s, wavs);
            setParam(s.engine, song.delay, "l_time", 1000.f);
            setParam(s.engine, song.delay, "r_time", 1000.f);
            s.engine.setTrackClips(song.drums, shortClipBetween(wavs, 1.f));
            s.run(0.1);
            renderAll(s);
            s.engine.setTrackClips(song.drums, shortClipBetween(wavs, 0.5f));
            renderAll(s);
            const uint64_t replayed = s.engine.backgroundFreezingStats(song.drums).framesReplayed;
            s.engine.setTrackClips(song.drums, shortClipBetween(wavs, 0.25f));
            renderAll(s);
            s.engine.play();
            s.run(6.0);
            // (From 3.5 s, where the drums last differed, they would have to match up to 5.5 s.)
            if (s.freezing) CHECK(s.engine.backgroundFreezingStats(song.drums).framesReplayed - replayed < framesOf(0.5));
        },
        1, settings));
}
