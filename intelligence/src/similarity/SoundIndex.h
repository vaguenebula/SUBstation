// The sound similarity index: the fingerprint of every file of a sample
// library, kept up to date in the background, and searches for the sounds
// most like a given one.
//
// Threads, all its own (none ever calls into the application but the wake
// callback):
//
// - a keeper, at background priority: reads the saved fingerprints at start
//   (sound-index.bin), takes the library from its source when told it changed
//   (at most once a second), and saves (10 s after a change, and on close);
// - analysers (a quarter of the cores, one to four), at background priority,
//   each with an extractor of its own, made on its thread
//   (SoundIndexOptions::extractor; Essentia's by default): fingerprint the
//   library's new files, then check the saved ones against their files (size
//   and last-write time) and analyse again those that changed (again whenever
//   the library is taken, for files last checked more than recheckSeconds
//   before). Files that can't be decoded are remembered as such until they
//   change. An analyser whose extractor can't be made stops; with none left,
//   nothing is analysed, and the status says why;
// - one for searches, at normal priority (the user waits for it): analyses the
//   sound searched from if it has no fingerprint yet (a file outside the
//   library, a part of a file, a library file not reached yet; its extractor
//   made when it first needs one), then compares it
//   with every analysed file of the library. A search asked for replaces one
//   waiting (and stops one running), and only the latest one's result is
//   handed out; cancelSearch() drops them all.
//
// Closing stops the analysers in the middle of a file (extractors poll a flag
// between frames): the file stays to analyse next time. Searches measure
// features in the library's statistics (Similarity.h), which are saved with
// the fingerprints. A save measures them again if fingerprints changed since; a
// search, once fingerprints changed for a tenth as many files as they describe
// or the library grew or shrank by a tenth. On at most 20000 fingerprints, and
// without holding up the other threads. Until a library is taken, the saved
// ones stay.
//
// When there is a result, or the analysis moved on (at most four times a
// second), the wake callback is called from one of those threads, once until
// the next take(): it must only hand the work over to the application's
// thread, which takes it with take().
//
// The library is a list of paths (UTF-8, the system's form) from a source the
// application sets, called on the keeper's thread: the browser's index, whose
// files it lists. A search's result gives each library file's similarity by
// the same path, so the browser can order its own lists by it. Files are told
// apart by platform::pathKey(), so on Windows a path spelt in another case (a
// clip's) is the same file as the library's.
//
// Fingerprints of files that leave the library are kept (not searched) for
// keepDays after they were last in it: a place on a drive that is unplugged
// for a while, or removed and added again, isn't analysed again.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "platform/Files.h"
#include "similarity/FeatureExtractor.h"
#include "similarity/Similarity.h"

namespace sub::intelligence {

// A sound to search from: a file, or part of one (an audio clip's).
struct SoundQuery {
    std::string path;      // UTF-8, the system's form
    double start = 0.0;    // seconds into the file
    double length = -1.0;  // seconds of it; < 0: to its end

    bool wholeFile() const { return start <= 0.0 && length < 0.0; }
};

struct SoundMatch {
    std::string path;
    float similarity = 0.f;  // 0..1 (Comparison::similarity)
};

// A finished search: how similar each analysed file of the library is to the
// sound, as it was when the search ran. Immutable; shared freely.
class SimilarityResult {
public:
    static constexpr size_t kBest = 256;

    uint64_t generation = 0;
    SoundQuery query;
    std::string error;               // why the sound couldn't be analysed ("" if it was)
    std::vector<SoundMatch> best;    // the most similar files, best first (at most kBest; not the sound itself)
    size_t scored = 0;               // library files with a similarity
    uint64_t libraryFiles = 0;       // the library's files when it ran
    double searchMs = 0.0;           // on the search thread, from taking the query

    // A library file's similarity (0..1) by its path as the library gave it;
    // NaN if it has none (not analysed yet, couldn't be, or not in the library).
    float similarity(std::string_view path) const;

    // (The index fills it in.)
    void setScores(const std::vector<std::pair<uint64_t, float>>& scores);
    static uint64_t pathHash(std::string_view path);

private:
    std::vector<uint64_t> keys_;  // open addressing by pathHash; 0 is empty
    std::vector<float> values_;
    uint64_t mask_ = 0;
};

struct SoundIndexStatus {
    bool busy = false;          // reading the saved fingerprints, taking the library, analysing or checking files
    bool analysing = false;     // files wait to be analysed or checked, or are (the library taken)
    uint64_t library = 0;       // the library's files
    uint64_t analysed = 0;      // of them, with a fingerprint
    uint64_t failed = 0;        // of them, that couldn't be analysed (not decodable, silent)
    uint64_t version = 0;       // changes when fingerprints are added or change
    std::string error;          // why the library can't be analysed ("" if it can)
    // For tests and benchmarks.
    double loadMs = 0.0;           // reading the saved fingerprints
    uint64_t analysedThisRun = 0;  // files analysed (not read from the store) since it started
    uint64_t statistics = 0;       // the library's files its statistics were measured on (0: none yet)

    uint64_t pending() const { return library > analysed + failed ? library - analysed - failed : 0; }
};

struct SoundIndexOptions {
    std::string store;          // where fingerprints are saved (UTF-8; "" for nowhere)
    unsigned threads = 0;       // analysers; 0: a quarter of the cores, one to four
    bool analyse = true;        // analyse the library (off: only the sounds searched from; tests)
    bool background = true;     // analysers and keeper at background priority (tests turn it off)
    ExtractorFactory extractor; // what makes fingerprints (one extractor per thread); empty: defaultExtractorFactory()
    std::optional<AspectWeights> weights;  // how much each aspect counts in searches; none: the extractor's
    double saveDelaySeconds = 10.0;
    double refreshSeconds = 1.0;  // the least time between takings of the library
    double recheckSeconds = 60.0; // a file checked longer ago than this is checked again when the library is taken
    size_t maxReferences = 1000;  // sounds outside the library whose fingerprints are kept
    double keepDays = 90.0;       // how long the fingerprints of files that left the library are kept
    std::function<int64_t()> clock;  // now, in seconds since 1970 (tests); empty: the system's
};

class SoundIndex {
public:
    using Library = std::vector<std::string>;
    using LibrarySource = std::function<std::shared_ptr<const Library>()>;

    explicit SoundIndex(SoundIndexOptions options = {});
    ~SoundIndex();
    SoundIndex(const SoundIndex&) = delete;
    SoundIndex& operator=(const SoundIndex&) = delete;

    // As the browser's: called from the index's threads, once until the next
    // take(); must only post the work to the application's thread. If a wake
    // is due already (the index signalled before a callback was set), the new
    // callback is called at once, on this thread.
    void setWakeCallback(std::function<void()> wake);

    // Where the library comes from: called on the keeper's thread after
    // libraryChanged() (and once when set). Returns the files (null: no change).
    // Waits for a call to the previous source to end, so once this returns the
    // previous one is never called again (null: none).
    void setLibrarySource(LibrarySource source);
    // The library changed: its source is called again (soon; at most once a second).
    void libraryChanged();
    // A fixed library (tests).
    void setLibrary(Library files);

    // Starts a search for the sounds most like `query` (stopping one running);
    // returns its generation.
    uint64_t find(SoundQuery query);
    // Drops the search waiting or running: no result comes of it.
    void cancelSearch();

    // What the fingerprints hold (the extractor's schema).
    const FeatureSchema& schema() const { return schema_; }

    struct Update {
        SoundIndexStatus status;
        std::shared_ptr<const SimilarityResult> result;  // the latest search's, if it finished since the last take()
    };
    Update take();
    SoundIndexStatus status() const;
    bool searching() const;

    // Waits until the library is analysed and checked and no search runs; false
    // on timeout (tests, benchmarks).
    bool waitIdle(double seconds);
    // Stops the threads (saving what changed); no wake calls after it. Idempotent.
    void close();

private:
    enum class State : uint8_t { Pending, Analysed, Failed };
    struct Entry {
        std::string path;
        uint64_t hash = 0;
        platform::FileStamp stamp;
        State state = State::Pending;
        bool inLibrary = false;
        // When its stamp was last compared with its file's (or it was analysed);
        // never, this run, if it is the clock's epoch.
        std::chrono::steady_clock::time_point checked{};
        bool queued = false;    // waiting to be analysed
        bool checking = false;  // waiting to be checked
        bool reference = false;
        uint64_t used = 0;
        int64_t seen = 0;  // when it was last in the library (seconds since 1970; 0: never)
    };
    struct Hash {
        using is_transparent = void;
        size_t operator()(std::string_view s) const { return std::hash<std::string_view>()(s); }
    };

    void keeperLoop();
    void workerLoop();
    void searchLoop();
    void load();
    // `keys`: the files' pathKey()s, made before the lock is taken.
    void applyLibrary(const Library& files, const std::vector<std::string>& keys);
    void save(std::unique_lock<std::mutex>& lock);
    // The library's statistics, measured again if they are out of date (lets
    // go of the lock while it measures); `exact`: if any fingerprint changed.
    void refreshStatistics(std::unique_lock<std::mutex>& lock, bool exact);
    std::shared_ptr<SimilarityResult> runSearch(uint64_t generation, const SoundQuery& query);
    uint32_t addEntry(std::string path, std::string key);
    std::vector<uint32_t> libraryRows() const;  // the library's analysed entries
    int64_t now() const;
    void setState(Entry& entry, State state);
    void changed();  // fingerprints changed: save soon, tell the application
    bool busyLocked() const;
    SoundIndexStatus statusLocked() const;
    void wake();
    void wakeForProgress();

    SoundIndexOptions options_;
    ExtractorFactory factory_;
    std::unique_ptr<FeatureExtractor> searchExtractor_;  // (only the search thread makes and uses it)
    FeatureSchema schema_;
    size_t dims_ = 0;
    AspectWeights weights_;
    unsigned analysers_ = 0;  // with an extractor (or making it)
    CancelFlag stopAnalysis_{false};  // set on close: analysers give up the file they're on
    CancelFlag stopSearch_{false};    // set when the running search is replaced or cancelled

    std::mutex wakeMutex_;
    std::function<void()> wakeCallback_;
    bool signalled_ = false;
    std::chrono::steady_clock::time_point lastProgressWake_{};

    std::mutex sourceMutex_;  // held while the source is called
    LibrarySource source_;

    mutable std::mutex mutex_;
    std::condition_variable keeperWake_, workerWake_, searchWake_, idle_;
    bool stop_ = false;
    bool closed_ = false;
    bool loaded_ = false;
    bool libraryDirty_ = false;
    bool refreshing_ = false;
    bool haveLibrary_ = false;  // one was taken
    bool saveDue_ = false;
    std::chrono::steady_clock::time_point saveAt_{}, nextRefresh_{};
    std::vector<Entry> entries_;
    std::vector<float> fingerprints_;  // dims_ per entry
    FeatureStatistics statistics_;     // the library's (measured or saved)
    uint64_t statisticsVersion_ = 0;   // counts_.version they were measured at
    bool measuring_ = false;           // a thread measures them (without the lock)
    std::unordered_map<std::string, uint32_t, Hash, std::equal_to<>> byKey_;  // by platform::pathKey()
    std::deque<uint32_t> analyseQueue_, checkQueue_;
    unsigned running_ = 0;  // analysers working on a file
    uint64_t used_ = 0;     // the reference counter
    SoundIndexStatus counts_;

    std::optional<std::pair<uint64_t, SoundQuery>> pending_;
    bool searchRunning_ = false;
    std::atomic<uint64_t> latest_{0};
    std::shared_ptr<const SimilarityResult> finished_;

    std::thread keeper_, search_;
    std::vector<std::thread> workers_;
};

}  // namespace sub::intelligence
