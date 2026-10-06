#pragma once
// The cache's own thread (background freezing, StripCache.h): it takes the blocks
// the rendering thread captured, publishes them per strip, drops those no longer
// good, keeps within the memory budget, and hands out empty blocks to capture
// into. It never touches the engine's edit model and never takes its lock; the
// edit side tells it the strips there are (setPoints), and what has changed
// time-locally for each (their dirty logs).
//
// What it unpublishes waits for the audio thread to be out of it, by the audio
// epoch, as retired snapshots do (DeferredReleasePool). A block no longer good
// stays published a while (kKeepInvalidNs): a seam fades out of it.

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "cache/StripCache.h"

namespace sub {

class CacheStore {
public:
    // `audioEpoch` counts finished audio callbacks; `deviceRunning` says whether
    // there are any; `backgroundEpoch` and `backgroundBusy` the same of the
    // background renderer's chunks (BackgroundRenderer.h); `playhead` (timeline
    // samples) says what to keep when memory runs short. All outlive the store.
    CacheStore(const CacheSettings& settings, const std::atomic<uint64_t>& audioEpoch,
               const std::atomic<bool>& deviceRunning, const std::atomic<uint64_t>& backgroundEpoch,
               const std::atomic<bool>& backgroundBusy, const std::atomic<int64_t>& playhead);
    ~CacheStore();
    CacheStore(const CacheStore&) = delete;
    CacheStore& operator=(const CacheStore&) = delete;

    // The edit side, after each snapshot: every strip's point and dirty log.
    struct Entry {
        std::shared_ptr<CachePoint> point;
        std::shared_ptr<const DirtyLog> dirty;
    };
    void setPoints(std::vector<Entry> points);

    // One round of the thread's work, now, on the calling thread (the thread does
    // one every few milliseconds; tests call it to be sure).
    void service();

    struct Stats {
        size_t blocks = 0;        // published
        size_t silentBlocks = 0;
        size_t bytes = 0;         // published and handed out
        size_t unfreedBytes = 0;  // no longer, but not freed yet (bytes + unfreedBytes stay within the budget)
    };
    Stats stats() const;

private:
    static constexpr int kSparesPerPoint = 2;  // (per lane)
    static constexpr int64_t kKeepInvalidNs = 300'000'000;

    struct Retired {
        const BlockSet* set = nullptr;
        CacheBlock* block = nullptr;
        uint64_t epoch = 0;            // the audio epoch when it was unpublished
        uint64_t backgroundEpoch = 0;  // the background renderer's
    };
    void stampLocked(std::vector<Retired>& retired);  // with the epochs now (after unpublishing)

    void run();
    void serviceLocked();
    // Publishes `point`'s blocks as they now are, retiring what was published.
    void publishLocked(CachePoint& point);
    void ingestLocked(CachePoint& point, CachePoint::Lane& lane, CacheBlock* block, const DirtyLog* dirty);
    void respareLocked(CachePoint::Lane& lane, CacheBlock* block);  // one never published, emptied and handed out again
    bool goodLocked(const CachePoint& point, const CacheBlock& block, const DirtyLog* dirty) const;
    int goodToLocked(const CachePoint& point, const CacheBlock& block, const DirtyLog* dirty) const;  // its good frames end
    void dropAllLocked(CachePoint& point);
    void retireLocked(CacheBlock* block);
    void freeRetiredLocked(bool all);

    const CacheSettings& settings_;
    const std::atomic<uint64_t>& audioEpoch_;
    const std::atomic<bool>& deviceRunning_;
    const std::atomic<uint64_t>& backgroundEpoch_;
    const std::atomic<bool>& backgroundBusy_;
    const std::atomic<int64_t>& playhead_;

    mutable std::mutex mutex_;
    std::vector<Entry> points_;
    std::vector<Retired> retired_;
    std::vector<Retired> pendingRetire_;  // this round's, stamped with the epoch after publishing
    size_t bytes_ = 0;                    // owned blocks' samples (published and handed out)
    // Samples no longer owned but not freed yet: blocks retired (until no
    // callback can see them), and the empty blocks of strips gone (until the
    // last snapshot showing them goes). New blocks are allocated only while
    // these and bytes_ together are within the budget.
    size_t retiredBytes_ = 0;
    struct Dying {
        std::shared_ptr<CachePoint> point;
        size_t bytes = 0;
    };
    std::vector<Dying> dying_;
    size_t dyingBytes_ = 0;
    void freeDyingLocked(bool all);

    std::mutex wakeMutex_;
    std::condition_variable wake_;
    bool quit_ = false;
    std::thread thread_;
};

}  // namespace sub
