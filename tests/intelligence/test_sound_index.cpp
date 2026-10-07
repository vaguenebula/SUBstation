// The sound similarity index: analysing a library in the background, saving
// and checking fingerprints, searches, and its threads' manners.

#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "Sounds.h"
#include "similarity/SoundIndex.h"

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
    index.setWakeCallback([&] {
        if (std::this_thread::get_id() == main) onMain = true;
        ++wakes;
    });
    std::atomic<int> calls{0};
    auto files = std::make_shared<const SoundIndex::Library>(library.paths);
    index.setLibrarySource([&, files] {
        if (std::this_thread::get_id() == main) onMain = true;
        ++calls;
        return files;
    });
    REQUIRE(index.waitIdle(30.0));
    CHECK(calls.load() >= 1);
    CHECK(wakes.load() >= 1);
    index.take();
    const int before = wakes.load();
    index.find(SoundQuery{library.paths[0]});
    REQUIRE(index.waitIdle(30.0));
    CHECK(wakes.load() > before);
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
