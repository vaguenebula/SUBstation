#pragma once
// Background freezing, phase 1 (docs/engine/background-freeze.md): what a strip
// played live, unchanged, is kept, and played again instead of running its
// devices.
//
// A strip's *cache point* is its signal after its devices, before its fader, in
// blocks on a grid of the timeline (kCacheBlockFrames). The audio thread captures
// blocks as the strip plays live and hands them to the cache store's thread,
// which publishes them as an immutable BlockSet; the audio thread plays from them
// when every block it needs is valid. Whether a block is valid, it works out
// itself, every chunk, from the strip's version (an atomic the edit side bumps
// after each time-global change: a parameter, the mixer upstream, the
// structure) and the snapshot's dirty log (time-local changes: clips, notes,
// automation, each changing the strip's signal from some position on). So a
// stale block never plays, whatever the store's thread is doing.
//
// Threads: the edit side (under the engine's mutex) writes `version`,
// `lastChangeNs`, `observed`, `observedUntilNs` and `accountedChanges`; the
// store's thread publishes `blocks` and owns everything in StoreState; each
// renderer (the live one, the background one: a Lane each) owns its lane's
// state, on the thread that renders the strip in a chunk (one at a time,
// chunks in order).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

#include "rt/RtUtils.h"

namespace sub {

class Processor;

// Frames per block on the grid (0.34 s at 48 kHz).
inline constexpr int64_t kCacheBlockFrames = 16384;

// What the engine's settings for background freezing are (Engine::setBackgroundFreezing).
struct CacheSettings {
    std::atomic<bool> enabled{false};
    std::atomic<double> idleSeconds{10.0};  // a strip plays from its cache once nothing changed it for this long
    std::atomic<double> warmSeconds{8.0};   // how long devices must run before what they put out is kept
    std::atomic<int64_t> budgetBytes{int64_t{1} << 30};
    std::atomic<bool> background{false};    // the background renderer renders what hasn't played yet (BackgroundRenderer.h)
};

// How the playhead came to a frame: in order, from far before (Linear), or
// soon after the loop wrapped from `wrapFrom` (its end) to `wrapTo` (its start),
// when what plays still carries what the loop's end left (a reverb's tail).
struct CacheContext {
    int64_t wrapFrom = -1;  // -1: Linear
    int64_t wrapTo = -1;
    bool linear() const noexcept { return wrapFrom < 0; }
    friend bool operator==(const CacheContext&, const CacheContext&) = default;
    friend bool operator<(const CacheContext& a, const CacheContext& b) noexcept {
        return a.wrapFrom != b.wrapFrom ? a.wrapFrom < b.wrapFrom : a.wrapTo < b.wrapTo;
    }
};

// A stretch of a strip's signal within one cell of the grid. Immutable once
// published; the store owns it.
struct CacheBlock {
    int64_t cell = 0;          // its frames are cell * kCacheBlockFrames + [from, to)
    int from = 0, to = 0;
    uint64_t version = 0;      // the strip's version it was captured at
    uint64_t generation = 0;   // the snapshot's generation it was captured with
    CacheContext context;
    bool silent = false;       // all zeros: no samples kept
    std::unique_ptr<float[]> samples;  // left then right, kCacheBlockFrames each (none if silent)

    int64_t start() const noexcept { return cell * kCacheBlockFrames; }
    const float* left() const noexcept { return samples.get(); }
    const float* right() const noexcept { return samples.get() + kCacheBlockFrames; }
    size_t bytes() const noexcept { return samples ? sizeof(float) * 2 * kCacheBlockFrames : 0; }
};

// A strip's published blocks, sorted by (cell, context, from). Immutable.
// Blocks of a cell may overlap: one partly out of date, and one captured since
// over the part that is.
struct BlockSet {
    std::vector<const CacheBlock*> blocks;

    // A block of `cell` in `context` whose frames include `offset` (null: none).
    const CacheBlock* find(int64_t cell, const CacheContext& context, int offset) const noexcept {
        for (auto it = first(cell, context); it != blocks.end() && (*it)->cell == cell && (*it)->context == context;
             ++it) {
            if ((*it)->from <= offset && offset < (*it)->to) return *it;
        }
        return nullptr;
    }
    // Of those, the one good furthest on, and where its good frames end in the
    // cell (`goodTo(block)`); null and `offset` if none is good there.
    template <typename GoodTo>
    const CacheBlock* findGood(int64_t cell, const CacheContext& context, int offset, GoodTo&& goodTo,
                               int& end) const noexcept {
        const CacheBlock* best = nullptr;
        end = offset;
        for (auto it = first(cell, context); it != blocks.end() && (*it)->cell == cell && (*it)->context == context;
             ++it) {
            if ((*it)->from > offset || offset >= (*it)->to) continue;
            const int good = goodTo(**it);
            if (good > end) {
                end = good;
                best = *it;
            }
        }
        return best;
    }
    // The first offset at or after `offset` in `cell` that some block in `context`
    // is good at (`goodTo(block)`, as above); -1 if none.
    template <typename GoodTo>
    int nextGood(int64_t cell, const CacheContext& context, int offset, GoodTo&& goodTo) const noexcept {
        int next = -1;
        for (auto it = first(cell, context); it != blocks.end() && (*it)->cell == cell && (*it)->context == context;
             ++it) {
            const int from = std::max((*it)->from, offset);
            if (goodTo(**it) > from && (next < 0 || from < next)) next = from;
        }
        return next;
    }

private:
    std::vector<const CacheBlock*>::const_iterator first(int64_t cell, const CacheContext& context) const noexcept {
        return std::lower_bound(blocks.begin(), blocks.end(), std::make_pair(cell, context),
                                [](const CacheBlock* b, const std::pair<int64_t, CacheContext>& key) {
                                    return b->cell != key.first ? b->cell < key.first : b->context < key.second;
                                });
    }
};

// The time-local changes a strip's cache must honour: each changed its signal
// over [from, to) (timeline samples; how long a change rings on in devices is in
// `to` already) at generation `generation`. A block captured at an older
// generation is no good where a later change overlaps it, nor, soon after the
// loop wrapped, where one still rang at the loop's end (what played there rings
// on after the wrap); one older than `horizon` (changes before it were
// forgotten) is no good at all. Immutable; the snapshot holds it.
struct DirtyLog {
    struct Entry {
        uint64_t generation;
        int64_t from, to;
    };
    uint64_t horizon = 0;
    std::vector<Entry> entries;  // by generation, ascending

    // Within [start, end) in `context`, where what was captured at `generation`
    // stops being good: `end` if all of it is, `start` if none.
    int64_t goodUntil(uint64_t generation, int64_t start, int64_t end,
                      const CacheContext& context = {}) const noexcept {
        if (generation < horizon) return start;
        auto it = std::upper_bound(entries.begin(), entries.end(), generation,
                                   [](uint64_t g, const Entry& e) { return g < e.generation; });
        int64_t until = end;
        for (; it != entries.end(); ++it) {
            if (!context.linear() && it->from < context.wrapFrom && it->to >= context.wrapFrom) return start;
            if (it->from < until && it->to > start) until = std::max(start, it->from);
        }
        return until;
    }
    // Whether something changed over [start, end) (in `context`) since `generation`.
    bool changedSince(uint64_t generation, int64_t start, int64_t end, const CacheContext& context = {}) const noexcept {
        return goodUntil(generation, start, end, context) < end;
    }
};

struct CachePoint {
    CachePoint() = default;
    ~CachePoint();  // frees the blocks still in its queues
    CachePoint(const CachePoint&) = delete;
    CachePoint& operator=(const CachePoint&) = delete;

    // --- The edit side writes these; the others read them ---
    std::atomic<uint64_t> version{1};
    std::atomic<int64_t> lastChangeNs{0};       // steady clock: it stays live until it has been unchanged a while
    std::atomic<bool> observed{false};          // its devices are shown (Engine::setTrackObserved)
    std::atomic<int64_t> observedUntilNs{0};    // ... or were lately (a display read, an editor open)
    std::atomic<uint64_t> accountedChanges{0};  // its devices' change counts the version accounts for

    // --- The store publishes; the rendering thread reads ---
    std::atomic<const BlockSet*> blocks{nullptr};

    // --- Between the live rendering thread and the store ---
    std::atomic<int64_t> idleSinceNs{0};   // its devices haven't been called since (0: they are)
    std::atomic<uint64_t> idleEpoch{0};    // counts the times they stopped being called
    // Who has its devices: the rendering thread while it runs them, the engine's
    // idle() while it resets them offline (they are suspended then: a plug-in
    // would let its input through), neither while they stand idle. Each takes
    // them from kDevicesIdle only.
    static constexpr int kDevicesIdle = 0, kDevicesRunning = 1, kDevicesResetting = 2;
    std::atomic<int> devicesOwner{kDevicesIdle};

    // --- A renderer's ---
    struct LiveState {
        enum class Mode : uint8_t { Live, Cache, PreRoll };
        Mode mode = Mode::Live;
        // A seam: the output crossfades from the cache to the devices' (toLive)
        // or the other way, over `fadeLength` frames, `fadeDone` of them done.
        enum class Fade : uint8_t { None, ToLive, ToCache };
        Fade fade = Fade::None;
        int fadeLength = 0, fadeDone = 0;
        bool idleInstance = true;   // its devices weren't called in the last chunk
        int64_t warmIn = 0;         // frames its devices must still run to be warm (a warm-up after starting cold)
        int64_t cleanIn = 0;        // ... and to match the arrangement (what feeds them too, notes held over)
        int64_t liveHold = 0;       // frames its output still carries live input (MIDI input, preview notes)
        uint64_t seenVersion = 0, seenGeneration = 0, seenChanges = 0, seenResets = 0;
        // Until how many frames played (the renderer's count) the blocks it will
        // need are good, and for what that was worked out (never dereferenced).
        uint64_t runwayUntil = 0;
        uint64_t runwayVersion = 0, runwayGeneration = 0, runwayJumps = ~uint64_t{0};
        const BlockSet* runwayBlocks = nullptr;
        // What this chunk hands on to the strips it feeds.
        bool pendingOut = false;    // its devices changed and the version doesn't say so yet
        bool liveInputOut = false;  // it carries live input
        int64_t cleanInOut = 0;     // frames until what it puts out matches the arrangement
        // The block being captured into (from `spares`).
        CacheBlock* building = nullptr;
    };
    // What a renderer keeps of the strip: the live renderer's, and the
    // background renderer's (rendering with shadow instances: BackgroundRenderer.h).
    // Each renders the strip on one thread at a time, chunks in order, and
    // hands what it captures to the store.
    struct Lane {
        LiveState state;
        SpscQueue<CacheBlock*, 16> completed;  // captured, for the store to publish
        SpscQueue<CacheBlock*, 4> spares;      // empty blocks the store hands out to capture into
        // Statistics (the renderer adds; anyone reads).
        std::atomic<uint64_t> framesFromCache{0}, framesLive{0}, framesCaptured{0};
        std::atomic<uint64_t> framesLost{0};  // it would have kept, but had no block for (the budget is spent)
        int storeSpares = 0;  // the store's (under its lock): handed out (in `spares` or being captured into)
    };
    static constexpr int kLiveLane = 0, kBackgroundLane = 1, kLanes = 2;
    std::array<Lane, kLanes> lanes;
    Lane& live() noexcept { return lanes[kLiveLane]; }
    const Lane& live() const noexcept { return lanes[kLiveLane]; }

    // --- The store's thread's (under the store's lock) ---
    struct StoreState {
        std::vector<CacheBlock*> owned;  // published, sorted like a BlockSet
        std::vector<int64_t> invalidSinceNs;  // per owned block: since when it has been no good (0: it is)
    };
    StoreState store;
};

inline CachePoint::~CachePoint() {
    for (Lane& lane : lanes) {
        CacheBlock* block = nullptr;
        while (lane.completed.pop(block)) delete block;
        while (lane.spares.pop(block)) delete block;
        delete lane.state.building;
    }
    // The published blocks belong to the store, which frees them.
}

}  // namespace sub
