// The sound similarity index: analysing a library in the background, saving
// and checking fingerprints and the library's statistics, searches, stopping
// work under way, and its threads' manners.

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

#include "Sounds.h"
#include "platform/Paths.h"
#include "similarity/EssentiaExtractor.h"
#include "similarity/SoundIndex.h"
#include "similarity/SoundStore.h"

using namespace subtest;
using namespace sub::intelligence;

namespace {

// A small library: kicks, snares, hats and claps, four of each, varied.
struct Library {
    std::vector<std::string> paths;
    std::map<std::string, std::string> kindOf;

    Library() {
        Random random(7);
        for (uint64_t i = 0; i < 4; ++i) {
            add("kick", i, kick(random.range(110, 180), random.range(42, 60), random.range(0.15, 0.35)));
            add("snare", i, snare(random.range(160, 230), random.range(0.08, 0.16), random.range(0.6, 0.8), 0.5, 10 + i));
            add("hat", i, hat(random.range(0.02, 0.05), 0.4, 20 + i));
            add("clap", i, clap(random.range(0.009, 0.014), random.range(0.06, 0.1), 0.5, 30 + i));
        }
    }
    void add(const std::string& kind, uint64_t i, const Samples& s) {
        const std::string path = wav("library/" + kind + "/" + kind + std::to_string(i) + ".wav", s);
        paths.push_back(path);
        kindOf[path] = kind;
    }
    std::string first(const std::string& kind) const {
        for (const auto& p : paths)
            if (kindOf.at(p) == kind) return p;
        return {};
    }
};

// An extractor of two features (a sound's level and its length) that takes
// `delay` seconds over each sound, looking at its cancel flag all the while.
class SlowExtractor final : public FeatureExtractor {
public:
    explicit SlowExtractor(std::shared_ptr<std::atomic<double>> delay, std::shared_ptr<std::atomic<int>> started)
        : delay_(std::move(delay)), started_(std::move(started)) {}

    static const FeatureSchema& slowSchema() {
        static const FeatureSchema s = [] {
            FeatureSchema schema;
            schema.extractor = "slow";
            schema.settings = "test";
            schema.features = {{"level", Aspect::Timbre, 1.f}, {"length", Aspect::Rhythm, 0.05f}};
            return schema;
        }();
        return s;
    }
    const FeatureSchema& schema() const override { return slowSchema(); }

    Extraction extract(const SoundBuffer& sound, float* out, const CancelFlag* cancel) override {
        ++*started_;
        const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(delay_->load());
        while (std::chrono::steady_clock::now() < until) {
            if (cancel && cancel->load()) return Extraction::Cancelled;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        double energy = 0.0;
        for (size_t i = 0; i < sound.count; ++i) energy += static_cast<double>(sound.samples[i]) * sound.samples[i];
        if (energy <= 0.0) return Extraction::Silent;
        out[0] = static_cast<float>(10.0 * std::log10(energy / static_cast<double>(sound.count)));
        out[1] = static_cast<float>(std::log10(static_cast<double>(sound.count) / sound.sampleRate));
        return Extraction::Done;
    }

private:
    std::shared_ptr<std::atomic<double>> delay_;
    std::shared_ptr<std::atomic<int>> started_;
};

struct Slow {
    std::shared_ptr<std::atomic<double>> delay = std::make_shared<std::atomic<double>>(0.0);
    std::shared_ptr<std::atomic<int>> started = std::make_shared<std::atomic<int>>(0);
    ExtractorFactory factory() const {
        return {SlowExtractor::slowSchema(),
                [delay = delay, started = started] { return std::make_unique<SlowExtractor>(delay, started); }};
    }
};

SoundIndexOptions options(const std::string& store = {}) {
    SoundIndexOptions o;
    o.store = store;
    o.threads = 2;
    o.background = false;
    o.saveDelaySeconds = 0.05;
    o.refreshSeconds = 0.0;
    return o;
}

std::shared_ptr<const SimilarityResult> search(SoundIndex& index, SoundQuery query) {
    const uint64_t generation = index.find(std::move(query));
    REQUIRE(index.waitIdle(30.0));
    auto update = index.take();
    REQUIRE(update.result != nullptr);
    CHECK_EQ(update.result->generation, generation);
    return update.result;
}

std::shared_ptr<const SimilarityResult> search(SoundIndex& index, const std::string& path) {
    return search(index, SoundQuery{path});
}

// Whether `done` comes true within `seconds` (the index's threads call the
// wake callback just after waitIdle() returns, not before).
template <typename F>
bool eventually(F done, double seconds = 10.0) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!done()) {
        if (std::chrono::steady_clock::now() > until) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace

TEST_CASE("a library is analysed in the background, and a search ranks it by similarity") {
    const Library library;
    SoundIndex index(options());
    index.setLibrary(library.paths);
    REQUIRE(index.waitIdle(30.0));
    const SoundIndexStatus status = index.status();
    CHECK(!status.busy);
    CHECK_EQ(status.library, library.paths.size());
    CHECK_EQ(status.analysed, library.paths.size());
    CHECK_EQ(status.failed, 0u);
    CHECK_EQ(status.pending(), 0u);

    for (const std::string kind : {"kick", "snare", "hat", "clap"}) {
        INFO(kind);
        const std::string from = library.first(kind);
        const auto result = search(index, from);
        CHECK(result->error.empty());
        CHECK_EQ(result->scored, library.paths.size());
        CHECK_EQ(result->libraryFiles, library.paths.size());
        // Its own kind first (the sound itself isn't among the best).
        REQUIRE(result->best.size() == library.paths.size() - 1);
        for (size_t i = 0; i < 3; ++i) CHECK_EQ(library.kindOf.at(result->best[i].path), kind);
        for (const auto& m : result->best) CHECK(m.path != from);
        for (size_t i = 1; i < result->best.size(); ++i) CHECK(result->best[i - 1].similarity >= result->best[i].similarity);
        // Every file's similarity by its path; the sound itself is 1.
        CHECK_NEAR(result->similarity(from), 1.0, 1e-6);
        CHECK_NEAR(result->similarity(result->best[0].path), result->best[0].similarity, 1e-6);
        CHECK(std::isnan(result->similarity("not/in/the/library.wav")));
    }
}

TEST_CASE("fingerprints are saved, and only new or changed files are analysed again") {
    const Library library;
    const std::string store = utf8(tempDir() / "sound-index.bin");
    {
        SoundIndex index(options(store));
        index.setLibrary(library.paths);
        REQUIRE(index.waitIdle(30.0));
        CHECK_EQ(index.status().analysedThisRun, library.paths.size());
    }  // (closing saves)
    {
        SoundIndex index(options(store));
        index.setLibrary(library.paths);
        REQUIRE(index.waitIdle(30.0));
        const SoundIndexStatus status = index.status();
        CHECK_EQ(status.analysed, library.paths.size());
        CHECK_EQ(status.analysedThisRun, 0u);  // all from the store, checked against their files
    }
    // One file changes (its size and time), another is added.
    const std::string changed = library.paths[0];
    writeWav(pathOf(changed), hat(0.03, 0.7));
    std::filesystem::last_write_time(pathOf(changed),
                                     std::filesystem::last_write_time(pathOf(changed)) + std::chrono::seconds(5));
    std::vector<std::string> more = library.paths;
    more.push_back(wav("library/new.wav", snare()));
    {
        SoundIndex index(options(store));
        index.setLibrary(more);
        REQUIRE(index.waitIdle(30.0));
        const SoundIndexStatus status = index.status();
        CHECK_EQ(status.analysed, more.size());
        CHECK_EQ(status.analysedThisRun, 2u);
        // The changed kick is a hat now.
        const auto result = search(index, library.first("hat"));
        CHECK_EQ(library.kindOf.at(changed), "kick");
        CHECK(result->similarity(changed) > result->similarity(library.paths[4]));  // (another kick)
    }
}

TEST_CASE("a file changed in place is analysed again when the library is next taken") {
    const Library library;
    auto exportAgain = [](const std::string& path) {  // (a kick exported again as a hat, at the same path)
        writeWav(pathOf(path), hat(0.03, 0.7));
        std::filesystem::last_write_time(pathOf(path), std::filesystem::last_write_time(pathOf(path)) + std::chrono::seconds(5));
    };
    {
        SoundIndexOptions o = options();
        o.recheckSeconds = 0.0;
        SoundIndex index(o);
        index.setLibrary(library.paths);
        REQUIRE(index.waitIdle(30.0));
        const uint64_t before = index.status().analysedThisRun;
        const std::string changed = library.paths[0];
        exportAgain(changed);
        index.libraryChanged();
        REQUIRE(index.waitIdle(30.0));
        CHECK_EQ(index.status().analysedThisRun, before + 1);
        const auto result = search(index, library.first("hat"));
        CHECK(result->similarity(changed) > result->similarity(library.paths[4]));  // (a kick still)
    }
    // Files checked less than recheckSeconds (a minute) before aren't checked again.
    SoundIndex index(options());
    index.setLibrary(library.paths);
    REQUIRE(index.waitIdle(30.0));
    const uint64_t before = index.status().analysedThisRun;
    exportAgain(library.paths[4]);
    index.libraryChanged();
    REQUIRE(index.waitIdle(30.0));
    CHECK_EQ(index.status().analysedThisRun, before);
}

TEST_CASE("files that leave the library leave its searches") {
    const Library library;
    SoundIndex index(options());
    index.setLibrary(library.paths);
    REQUIRE(index.waitIdle(30.0));
    std::vector<std::string> fewer(library.paths.begin() + 4, library.paths.end());
    index.setLibrary(fewer);
    REQUIRE(index.waitIdle(30.0));
    CHECK_EQ(index.status().library, fewer.size());
    const auto result = search(index, library.paths[0]);  // (still searchable from)
    CHECK(result->error.empty());
    CHECK_EQ(result->scored, fewer.size());
    for (size_t i = 0; i < 4; ++i) CHECK(std::isnan(result->similarity(library.paths[i])));
    CHECK(!std::isnan(result->similarity(fewer[0])));
}

TEST_CASE("a sound outside the library, or part of a file, is analysed for its search") {
    const Library library;
    SoundIndex index(options());
    index.setLibrary(library.paths);
    REQUIRE(index.waitIdle(30.0));
    // A file elsewhere (a recording in the project's folder).
    const std::string outside = wav("project/recorded kick.wav", kick(160, 48, 0.3, 0.6, 99));
    auto result = search(index, outside);
    CHECK(result->error.empty());
    CHECK_EQ(library.kindOf.at(result->best[0].path), "kick");
    CHECK(std::isnan(result->similarity(outside)));  // (not in the library)
    // The hat half of a loop: a hat, then a kick, a second apart.
    Samples loop = hat(0.03, 1.0, 77);
    const Samples k = kick();
    loop.insert(loop.end(), k.begin(), k.end());
    const std::string loopPath = wav("project/loop.wav", loop);
    result = search(index, SoundQuery{loopPath, 0.0, 0.5});
    CHECK_EQ(library.kindOf.at(result->best[0].path), "hat");
    result = search(index, SoundQuery{loopPath, 1.0, 0.6});
    CHECK_EQ(library.kindOf.at(result->best[0].path), "kick");
    // Not a sound at all.
    const auto junk = tempDir() / "project" / "junk.wav";
    std::ofstream(junk, std::ios::binary) << "nothing to hear";
    result = search(index, utf8(junk));
    CHECK(!result->error.empty());
    CHECK_EQ(result->scored, 0u);
    CHECK(result->best.empty());
}

TEST_CASE("a place gone for a while keeps its fingerprints; one gone long enough loses them") {
    const Library library;
    const std::string store = utf8(tempDir() / "sound-index.bin");
    const std::string elsewhere = wav("project/elsewhere.wav", kick(160, 48, 0.3, 0.6, 99));
    int64_t time = 1760000000;  // (the index's clock: seconds since 1970)
    auto at = [&](int64_t when) {
        SoundIndexOptions o = options(store);
        o.clock = [when] { return when; };
        return o;
    };
    {
        SoundIndex index(at(time));
        index.setLibrary(library.paths);
        REQUIRE(index.waitIdle(30.0));
    }
    // The drive is unplugged: the library is empty. A search from another
    // file changes the store, which is saved.
    time += 3 * 86400;
    {
        SoundIndex index(at(time));
        index.setLibrary({});
        REQUIRE(index.waitIdle(30.0));
        CHECK_EQ(index.status().library, 0u);
        CHECK(search(index, elsewhere)->error.empty());
    }
    const auto saved = readStore(store, essentiaSchema());  // (rewritten, with the absent files still in it)
    REQUIRE(saved.has_value());
    CHECK_EQ(saved->sounds.size(), library.paths.size() + 1);
    // It is back: nothing is analysed again.
    time += 86400;
    {
        SoundIndex index(at(time));
        index.setLibrary(library.paths);
        REQUIRE(index.waitIdle(30.0));
        CHECK_EQ(index.status().analysed, library.paths.size());
        CHECK_EQ(index.status().analysedThisRun, 0u);
    }
    // Gone for longer than keepDays (90): dropped from the store when it is next saved.
    time += 91 * 86400;
    {
        SoundIndex index(at(time));
        index.setLibrary({});
        REQUIRE(index.waitIdle(30.0));
        CHECK(search(index, wav("project/another.wav", snare()))->error.empty());  // (a change: saved)
    }
    const auto pruned = readStore(store, essentiaSchema());
    REQUIRE(pruned.has_value());
    CHECK_EQ(pruned->sounds.size(), 2u);  // the two sounds searched from
    {
        SoundIndex index(at(time));
        index.setLibrary(library.paths);
        REQUIRE(index.waitIdle(30.0));
        CHECK_EQ(index.status().analysedThisRun, library.paths.size());
    }
}

TEST_CASE("a path spelt in another case is the library's file, where names ignore case") {
    if (sub::platform::kCaseSensitivePaths) SKIP("file names are case-sensitive here");
    Library library;
    const std::string kickPath = library.first("kick");
    std::string upper = kickPath;
    for (char& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    std::string slashed = upper;
    for (char& c : slashed)
        if (c == '\\') c = '/';
    SoundIndex index(options());
    index.setLibrary(library.paths);
    REQUIRE(index.waitIdle(30.0));
    const uint64_t analysed = index.status().analysedThisRun;
    // The saved fingerprint is used, and the sound itself isn't among the best.
    auto result = search(index, slashed);
    CHECK(result->error.empty());
    CHECK_EQ(index.status().analysedThisRun, analysed);
    CHECK_NEAR(result->similarity(kickPath), 1.0, 1e-6);
    for (const auto& m : result->best) CHECK(m.path != kickPath);
    CHECK_EQ(result->best.size(), library.paths.size() - 1);

    // A file searched from before the library listed it, spelt otherwise: the
    // library's spelling is what results are looked up by.
    const std::string late = wav("library/late/Late Kick.wav", kick(140, 50, 0.25));
    std::string lateUpper = late;
    for (char& c : lateUpper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    CHECK(search(index, lateUpper)->error.empty());
    library.paths.push_back(late);
    index.setLibrary(library.paths);
    REQUIRE(index.waitIdle(30.0));
    CHECK_EQ(index.status().library, library.paths.size());
    result = search(index, kickPath);
    CHECK(!std::isnan(result->similarity(late)));
    CHECK_EQ(result->scored, library.paths.size());
}

TEST_CASE("files that can't be analysed are counted, and not tried again until they change") {
    Library library;
    const auto junk = tempDir() / "library" / "junk.wav";
    std::ofstream(junk, std::ios::binary) << "not audio";
    library.paths.push_back(utf8(junk));
    library.paths.push_back(wav("library/silent.wav", Samples(frames(0.3), 0.f)));
    const std::string store = utf8(tempDir() / "sound-index.bin");
    {
        SoundIndex index(options(store));
        index.setLibrary(library.paths);
        REQUIRE(index.waitIdle(30.0));
        CHECK_EQ(index.status().failed, 2u);
        CHECK_EQ(index.status().analysed, library.paths.size() - 2);
    }
    SoundIndex index(options(store));
    index.setLibrary(library.paths);
    REQUIRE(index.waitIdle(30.0));
    CHECK_EQ(index.status().failed, 2u);
    CHECK_EQ(index.status().analysedThisRun, 0u);
}

TEST_CASE("a library file not analysed yet is analysed at once when searched from") {
    const Library library;
    SoundIndexOptions o = options();
    o.threads = 1;
    SoundIndex index(o);
    index.setLibrary(library.paths);
    const auto result = search(index, library.paths.back());
    CHECK(result->error.empty());
    CHECK_NEAR(result->similarity(library.paths.back()), 1.0, 1e-6);
}

TEST_CASE("only the latest search's result is handed out, once") {
    const Library library;
    SoundIndex index(options());
    index.setLibrary(library.paths);
    REQUIRE(index.waitIdle(30.0));
    index.find(SoundQuery{library.first("kick")});
    const uint64_t latest = index.find(SoundQuery{library.first("hat")});
    REQUIRE(index.waitIdle(30.0));
    auto update = index.take();
    REQUIRE(update.result != nullptr);
    CHECK_EQ(update.result->generation, latest);
    CHECK_EQ(update.result->query.path, library.first("hat"));
    CHECK(index.take().result == nullptr);
}

TEST_CASE("the wake callback comes from the index's threads; the source from the keeper's") {
    const Library library;
    SoundIndex index(options());
    std::atomic<int> wakes{0};
    std::atomic<bool> onMain{false};
    const auto main = std::this_thread::get_id();
    // The keeper may have signalled already (the store read): then the
    // callback is called at once, on this thread, while it is set. Never after.
    std::atomic<bool> set{false};
    index.setWakeCallback([&] {
        if (set && std::this_thread::get_id() == main) onMain = true;
        ++wakes;
    });
    set = true;
    std::atomic<int> calls{0};
    auto files = std::make_shared<const SoundIndex::Library>(library.paths);
    index.setLibrarySource([&, files] {
        if (std::this_thread::get_id() == main) onMain = true;
        ++calls;
        return files;
    });
    REQUIRE(index.waitIdle(30.0));
    CHECK(calls.load() >= 1);
    CHECK(eventually([&] { return wakes.load() >= 1; }));
    // As the application: it takes only when woken, and the search's result
    // comes to it so. (A wake still on its way from the library's analysis
    // stands for the search's until it is taken: one wake until the next take().)
    int seen = wakes.load();
    index.take();
    const uint64_t generation = index.find(SoundQuery{library.paths[0]});
    std::shared_ptr<const SimilarityResult> result;
    CHECK(eventually([&] {
        if (wakes.load() == seen) return false;
        seen = wakes.load();
        if (auto update = index.take(); update.result) result = update.result;
        return result != nullptr;
    }));
    REQUIRE(result != nullptr);
    CHECK_EQ(result->generation, generation);
    CHECK(!onMain.load());
    // Once replaced, the old source is never called again.
    index.setLibrarySource(nullptr);
    const int callsBefore = calls.load();
    index.libraryChanged();
    REQUIRE(index.waitIdle(30.0));
    CHECK_EQ(calls.load(), callsBefore);
    index.close();
    index.close();  // (idempotent)
}

TEST_CASE("the library's statistics are saved with its fingerprints, and searches measure with them") {
    const Library library;
    const std::string store = utf8(tempDir() / "sound-index.bin");
    {
        SoundIndex index(options(store));
        index.setLibrary(library.paths);
        REQUIRE(index.waitIdle(30.0));
    }  // (closing saves, the statistics measured again)
    const auto saved = readStore(store, essentiaSchema());
    REQUIRE(saved.has_value());
    REQUIRE(saved->statistics.has_value());
    CHECK_EQ(saved->statistics->count, library.paths.size());
    for (const float s : saved->statistics->spread) CHECK(s > 0.f);
    auto similarities = [&] {
        SoundIndex index(options(store));
        index.setLibrary(library.paths);
        REQUIRE(index.waitIdle(30.0));
        CHECK_EQ(index.status().analysedThisRun, 0u);
        const auto result = search(index, library.first("kick"));
        std::vector<float> out;
        for (const auto& path : library.paths) out.push_back(result->similarity(path));
        return out;
    };
    // Every run measures in the saved scale: the same similarities...
    const std::vector<float> first = similarities();
    const std::vector<float> second = similarities();
    for (size_t i = 0; i < first.size(); ++i) CHECK_EQ(first[i], second[i]);
    // ...and saved statistics twice as spread make every sound nearer.
    StoreWriter writer(essentiaSchema(), static_cast<uint32_t>(saved->sounds.size()));
    for (const StoredSound& s : saved->sounds) writer.add(s);
    FeatureStatistics wider = *saved->statistics;
    for (float& s : wider.spread) s *= 2.f;
    REQUIRE(writeStore(store, writer.finish(&wider)));
    const std::vector<float> nearer = similarities();
    for (size_t i = 0; i < first.size(); ++i) {
        if (library.paths[i] == library.first("kick")) continue;
        CHECK(nearer[i] > first[i]);
    }
}

TEST_CASE("saved statistics stay until a library is taken; then they follow it, with or without a store") {
    const Library library;
    const std::string store = utf8(tempDir() / "sound-index.bin");
    {
        SoundIndex index(options(store));
        index.setLibrary(library.paths);
        REQUIRE(index.waitIdle(30.0));
    }
    const auto saved = readStore(store, essentiaSchema());
    REQUIRE(saved.has_value());
    REQUIRE(saved->statistics.has_value());
    // Searched from a sound outside the library before any is taken: they are
    // used as they are, and saved again as they were.
    {
        SoundIndex index(options(store));
        const auto result = search(index, wav("outside/tone.wav", tone(330.0, 0.5, 10.0)));
        CHECK(result->error.empty());
        CHECK_EQ(index.status().statistics, library.paths.size());
    }  // (closing saves the sound searched from)
    const auto again = readStore(store, essentiaSchema());
    REQUIRE(again.has_value());
    REQUIRE(again->statistics.has_value());
    CHECK_EQ(again->sounds.size(), saved->sounds.size() + 1);
    CHECK_EQ(again->statistics->count, saved->statistics->count);
    CHECK(again->statistics->spread == saved->statistics->spread);

    // Nowhere to save them: a search measures them, and measures them again
    // when the library has changed.
    SoundIndex index(options());
    const std::vector<std::string> half(library.paths.begin(), library.paths.begin() + 8);
    index.setLibrary(half);
    REQUIRE(index.waitIdle(30.0));
    search(index, library.first("kick"));
    CHECK_EQ(index.status().statistics, half.size());
    index.setLibrary(library.paths);
    REQUIRE(index.waitIdle(30.0));
    search(index, library.first("kick"));
    CHECK_EQ(index.status().statistics, library.paths.size());
}

TEST_CASE("an extractor that can't be made stops the analysis (saying why), not the program") {
    const Library library;
    const Slow slow;
    for (int how = 0; how < 3; ++how) {
        INFO(std::to_string(how));
        SoundIndexOptions o = options();
        if (how == 0)  // (a model missing, say)
            o.extractor = {SlowExtractor::slowSchema(),
                           []() -> std::unique_ptr<FeatureExtractor> { throw std::runtime_error("no model"); }};
        if (how == 1) o.extractor = {SlowExtractor::slowSchema(), [] { return std::unique_ptr<FeatureExtractor>(); }};
        if (how == 2) o.extractor = {essentiaSchema(), slow.factory().make};  // (another schema's)
        SoundIndex index(o);
        index.setLibrary(library.paths);
        REQUIRE(index.waitIdle(30.0));
        const SoundIndexStatus status = index.status();
        CHECK(!status.busy);
        CHECK(!status.error.empty());
        CHECK_EQ(status.analysed, 0u);
        CHECK_EQ(status.failed, 0u);
        CHECK_EQ(status.pending(), library.paths.size());
        // A search can't analyse its sound either: its result says why.
        const auto result = search(index, library.first("kick"));
        CHECK(!result->error.empty());
        if (how == 0) CHECK(result->error.find("no model") != std::string::npos);
    }
}

TEST_CASE("fingerprints made by another extractor, or as it was, are made again") {
    const Library library;
    const std::string store = utf8(tempDir() / "sound-index.bin");
    const Slow slow;
    {
        SoundIndexOptions o = options(store);
        o.extractor = slow.factory();
        SoundIndex index(o);
        CHECK_EQ(index.schema().extractor, std::string("slow"));
        index.setLibrary(library.paths);
        REQUIRE(index.waitIdle(30.0));
        CHECK_EQ(index.status().analysedThisRun, library.paths.size());
    }
    REQUIRE(readStore(store, SlowExtractor::slowSchema()).has_value());
    {
        SoundIndex index(options(store));  // (Essentia's)
        CHECK_EQ(index.schema().extractor, std::string("essentia"));
        index.setLibrary(library.paths);
        REQUIRE(index.waitIdle(30.0));
        CHECK_EQ(index.status().analysedThisRun, library.paths.size());
        CHECK_EQ(index.status().analysed, library.paths.size());
    }
    // Back to the other: its fingerprints were replaced, so made again.
    SoundIndexOptions o = options(store);
    o.extractor = slow.factory();
    SoundIndex index(o);
    index.setLibrary(library.paths);
    REQUIRE(index.waitIdle(30.0));
    CHECK_EQ(index.status().analysedThisRun, library.paths.size());
}

TEST_CASE("closing stops the analysers in the middle of a file, which is analysed next time") {
    const Library library;
    const std::string store = utf8(tempDir() / "sound-index.bin");
    const Slow slow;
    slow.delay->store(30.0);  // (a file takes half a minute)
    {
        SoundIndexOptions o = options(store);
        o.extractor = slow.factory();
        SoundIndex index(o);
        index.setLibrary(library.paths);
        REQUIRE(eventually([&] { return slow.started->load() >= 2; }));
        const auto start = std::chrono::steady_clock::now();
        index.close();
        CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
        const SoundIndexStatus status = index.status();
        CHECK_EQ(status.analysedThisRun, 0u);
        CHECK_EQ(status.failed, 0u);  // (given up, not failed)
        CHECK_EQ(status.pending(), library.paths.size());
    }
    slow.delay->store(0.0);
    SoundIndexOptions o = options(store);
    o.extractor = slow.factory();
    SoundIndex index(o);
    index.setLibrary(library.paths);
    REQUIRE(index.waitIdle(30.0));
    CHECK_EQ(index.status().analysed, library.paths.size());
    CHECK_EQ(index.status().failed, 0u);
}

TEST_CASE("a search replaced stops at once; a search cancelled hands nothing out") {
    const Library library;
    const Slow slow;
    SoundIndexOptions o = options();
    o.extractor = slow.factory();
    o.analyse = false;  // (only the sounds searched from are analysed)
    SoundIndex index(o);
    index.setLibrary(library.paths);
    REQUIRE(index.waitIdle(30.0));

    // Cancelled while it analyses the sound: it stops, and no result comes.
    slow.delay->store(30.0);
    index.find(SoundQuery{library.paths[0]});
    REQUIRE(eventually([&] { return slow.started->load() >= 1; }));
    auto start = std::chrono::steady_clock::now();
    index.cancelSearch();
    REQUIRE(index.waitIdle(5.0));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    CHECK(index.take().result == nullptr);
    CHECK(!index.searching());

    // Replaced: the first stops, the second's result comes.
    index.find(SoundQuery{library.paths[1]});
    REQUIRE(eventually([&] { return slow.started->load() >= 2; }));
    slow.delay->store(0.0);
    start = std::chrono::steady_clock::now();
    const uint64_t latest = index.find(SoundQuery{library.paths[2]});
    REQUIRE(index.waitIdle(5.0));
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    const auto update = index.take();
    REQUIRE(update.result != nullptr);
    CHECK_EQ(update.result->generation, latest);
    CHECK_EQ(update.result->query.path, library.paths[2]);
    CHECK(update.result->error.empty());

    // Cancelling with nothing under way is harmless.
    index.cancelSearch();
    CHECK(index.take().result == nullptr);
}
