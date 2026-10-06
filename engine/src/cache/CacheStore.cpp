#include "cache/CacheStore.h"

#include <algorithm>
#include <chrono>

namespace sub {
namespace {

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr size_t kBlockBytes = sizeof(float) * 2 * kCacheBlockFrames;

bool before(const CacheBlock* a, const CacheBlock* b) {
    if (a->cell != b->cell) return a->cell < b->cell;
    if (a->context != b->context) return a->context < b->context;
    return a->from < b->from;
}

bool silentBlock(const CacheBlock& block) {
    const float* left = block.left();
    const float* right = block.right();
    for (int i = block.from; i < block.to; ++i) {
        if (left[i] != 0.f || right[i] != 0.f) return false;
    }
    return true;
}

}  // namespace

CacheStore::CacheStore(const CacheSettings& settings, const std::atomic<uint64_t>& audioEpoch,
                       const std::atomic<bool>& deviceRunning, const std::atomic<uint64_t>& backgroundEpoch,
                       const std::atomic<bool>& backgroundBusy, const std::atomic<int64_t>& playhead)
    : settings_(settings),
      audioEpoch_(audioEpoch),
      deviceRunning_(deviceRunning),
      backgroundEpoch_(backgroundEpoch),
      backgroundBusy_(backgroundBusy),
      playhead_(playhead) {
    thread_ = std::thread([this] { run(); });
}

CacheStore::~CacheStore() {
    {
        std::lock_guard lock(wakeMutex_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
    std::lock_guard lock(mutex_);
    for (Entry& entry : points_) dropAllLocked(*entry.point);
    retired_.insert(retired_.end(), pendingRetire_.begin(), pendingRetire_.end());
    pendingRetire_.clear();
    freeRetiredLocked(true);  // the audio thread has stopped by now (the engine closed its device first)
    freeDyingLocked(true);
}

void CacheStore::setPoints(std::vector<Entry> points) {
    std::lock_guard lock(mutex_);
    // Strips gone: their blocks go too (the snapshot that still shows them reads them until it is retired).
    for (Entry& old : points_) {
        const bool kept = std::any_of(points.begin(), points.end(),
                                      [&](const Entry& entry) { return entry.point == old.point; });
        if (kept) continue;
        CachePoint& point = *old.point;
        size_t spares = 0;
        for (CachePoint::Lane& lane : point.lanes) {
            CacheBlock* block = nullptr;
            while (lane.completed.pop(block)) {
                --lane.storeSpares;
                retireLocked(block);
            }
            spares += static_cast<size_t>(lane.storeSpares) * kBlockBytes;
            lane.storeSpares = 0;
        }
        dropAllLocked(point);
        // The empty blocks it still has go with it (CachePoint's destructor),
        // once the snapshots still showing it have gone.
        bytes_ -= spares;
        dyingBytes_ += spares;
        dying_.push_back({old.point, spares});
    }
    points_ = std::move(points);
    if (!pendingRetire_.empty()) {
        stampLocked(pendingRetire_);
        retired_.insert(retired_.end(), pendingRetire_.begin(), pendingRetire_.end());
        pendingRetire_.clear();
    }
}

void CacheStore::service() {
    std::lock_guard lock(mutex_);
    serviceLocked();
}

CacheStore::Stats CacheStore::stats() const {
    std::lock_guard lock(mutex_);
    Stats stats;
    for (const Entry& entry : points_) {
        for (const CacheBlock* block : entry.point->store.owned) {
            ++stats.blocks;
            if (block->silent) ++stats.silentBlocks;
        }
    }
    stats.bytes = bytes_;
    stats.unfreedBytes = retiredBytes_ + dyingBytes_;
    return stats;
}

void CacheStore::run() {
    for (;;) {
        {
            std::unique_lock lock(wakeMutex_);
            wake_.wait_for(lock, std::chrono::milliseconds(5), [this] { return quit_; });
            if (quit_) return;
        }
        service();
    }
}

bool CacheStore::goodLocked(const CachePoint& point, const CacheBlock& block, const DirtyLog* dirty) const {
    return goodToLocked(point, block, dirty) > block.from;
}

int CacheStore::goodToLocked(const CachePoint& point, const CacheBlock& block, const DirtyLog* dirty) const {
    if (block.version != point.version.load(std::memory_order_acquire)) return block.from;
    if (!dirty) return block.to;
    const int64_t start = block.start();
    return static_cast<int>(
        dirty->goodUntil(block.generation, start + block.from, start + block.to, block.context) - start);
}

void CacheStore::retireLocked(CacheBlock* block) {
    bytes_ -= block->bytes();
    retiredBytes_ += block->bytes();
    pendingRetire_.push_back({nullptr, block, 0});
}

void CacheStore::freeDyingLocked(bool all) {
    std::erase_if(dying_, [&](Dying& d) {
        if (!all && d.point.use_count() > 1) return false;  // (a snapshot still shows it)
        dyingBytes_ -= d.bytes;
        return true;  // (its destructor frees its blocks, here or where the last holder lets go)
    });
}

void CacheStore::dropAllLocked(CachePoint& point) {
    for (CacheBlock* block : point.store.owned) retireLocked(block);
    point.store.owned.clear();
    point.store.invalidSinceNs.clear();
    if (const BlockSet* old = point.blocks.exchange(nullptr, std::memory_order_seq_cst)) {
        pendingRetire_.push_back({old, nullptr, 0});
    }
}

void CacheStore::respareLocked(CachePoint::Lane& lane, CacheBlock* block) {
    block->from = block->to = 0;
    if (lane.spares.push(block)) {
        ++lane.storeSpares;
    } else {
        bytes_ -= block->bytes();
        delete block;
    }
}

void CacheStore::ingestLocked(CachePoint& point, CachePoint::Lane& lane, CacheBlock* block, const DirtyLog* dirty) {
    --lane.storeSpares;  // (it was never published: it can be handed out again at once)
    // Empty, or captured at a version or generation already gone (it would never play).
    if (block->to <= block->from || !goodLocked(point, *block, dirty)) {
        respareLocked(lane, block);
        return;
    }
    // Where it overlaps what is good of the blocks of its cell and context, they
    // keep their frames (it is cut short at an end they cover); those whose good
    // frames it covers all go (those partly out of date stay beside it).
    auto& owned = point.store.owned;
    auto& since = point.store.invalidSinceNs;
    for (const CacheBlock* old : owned) {
        if (old->cell != block->cell || old->context != block->context) continue;
        const int good = goodToLocked(point, *old, dirty);  // its good frames: [from, good)
        if (good <= old->from || good <= block->from || block->to <= old->from) continue;
        if (old->from <= block->from) block->from = std::max(block->from, good);
        if (good >= block->to) block->to = std::min(block->to, old->from);
        if (block->to <= block->from) break;
    }
    if (block->to <= block->from) {  // nothing new in it
        respareLocked(lane, block);
        return;
    }
    if (silentBlock(*block)) {
        bytes_ -= block->bytes();
        block->samples.reset();
        block->silent = true;
    }
    for (size_t i = 0; i < owned.size();) {
        CacheBlock* old = owned[i];
        const int good = goodToLocked(point, *old, dirty);
        if (old->cell == block->cell && old->context == block->context && old->to > block->from &&
            block->to > old->from && (good <= old->from || (block->from <= old->from && good <= block->to))) {
            retireLocked(old);
            owned.erase(owned.begin() + static_cast<std::ptrdiff_t>(i));
            since.erase(since.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
    const auto at = std::upper_bound(owned.begin(), owned.end(), block, before);
    since.insert(since.begin() + (at - owned.begin()), 0);
    owned.insert(at, block);
}

void CacheStore::publishLocked(CachePoint& point) {
    auto* set = new BlockSet;
    set->blocks.assign(point.store.owned.begin(), point.store.owned.end());
    if (const BlockSet* old = point.blocks.exchange(set, std::memory_order_seq_cst)) {
        pendingRetire_.push_back({old, nullptr, 0});
    }
}

void CacheStore::stampLocked(std::vector<Retired>& retired) {
    const uint64_t epoch = audioEpoch_.load(std::memory_order_seq_cst);
    const uint64_t background = backgroundEpoch_.load(std::memory_order_seq_cst);
    for (Retired& r : retired) {
        r.epoch = epoch;
        r.backgroundEpoch = background;
    }
}

void CacheStore::freeRetiredLocked(bool all) {
    // Each renderer either reads nothing now (whatever it reads next is
    // published), or has finished a chunk (a callback) since it was unpublished.
    const uint64_t epoch = audioEpoch_.load(std::memory_order_seq_cst);
    const bool idle = all || !deviceRunning_.load(std::memory_order_seq_cst);
    const uint64_t background = backgroundEpoch_.load(std::memory_order_seq_cst);
    const bool backgroundIdle = all || !backgroundBusy_.load(std::memory_order_seq_cst);
    std::erase_if(retired_, [&](const Retired& r) {
        if (!idle && epoch <= r.epoch) return false;
        if (!backgroundIdle && background <= r.backgroundEpoch) return false;
        if (r.block) retiredBytes_ -= r.block->bytes();
        delete r.set;
        delete r.block;
        return true;
    });
}

void CacheStore::serviceLocked() {
    const int64_t now = nowNs();
    const bool enabled = settings_.enabled.load(std::memory_order_relaxed);
    const auto budget = static_cast<size_t>(std::max<int64_t>(0, settings_.budgetBytes.load(std::memory_order_relaxed)));
    std::vector<CachePoint*> changed;

    for (Entry& entry : points_) {
        CachePoint& point = *entry.point;
        const DirtyLog* dirty = entry.dirty.get();
        bool touched = false;
        for (CachePoint::Lane& lane : point.lanes) {
            CacheBlock* block = nullptr;
            while (lane.completed.pop(block)) {
                if (enabled) {
                    ingestLocked(point, lane, block, dirty);
                } else {
                    --lane.storeSpares;
                    retireLocked(block);
                }
                touched = true;
            }
        }
        if (!enabled) {
            if (!point.store.owned.empty()) dropAllLocked(point);
            continue;
        }
        // Blocks no longer good go once a seam can no longer fade out of them.
        auto& owned = point.store.owned;
        auto& since = point.store.invalidSinceNs;
        for (size_t i = 0; i < owned.size();) {
            if (goodLocked(point, *owned[i], dirty)) {
                since[i] = 0;
            } else if (since[i] == 0) {
                since[i] = now;
            } else if (now - since[i] > kKeepInvalidNs) {
                retireLocked(owned[i]);
                owned.erase(owned.begin() + static_cast<std::ptrdiff_t>(i));
                since.erase(since.begin() + static_cast<std::ptrdiff_t>(i));
                touched = true;
                continue;
            }
            ++i;
        }
        if (touched) changed.push_back(&point);
    }

    // Over the budget: the blocks no longer good first, then those farthest from the playhead.
    if (enabled && bytes_ > budget) {
        const int64_t playhead = playhead_.load(std::memory_order_relaxed);
        struct Candidate {
            CachePoint* point;
            const CacheBlock* block;
            bool good;
            int64_t distance;
        };
        std::vector<Candidate> candidates;
        for (Entry& entry : points_) {
            for (const CacheBlock* block : entry.point->store.owned) {
                if (block->silent) continue;  // (no samples to free)
                const int64_t start = block->start() + block->from;
                candidates.push_back({entry.point.get(), block, goodLocked(*entry.point, *block, entry.dirty.get()),
                                      std::abs(start - playhead)});
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.good != b.good ? !a.good : a.distance > b.distance;
        });
        for (const Candidate& c : candidates) {
            if (bytes_ <= budget) break;
            auto& owned = c.point->store.owned;
            const auto it = std::find(owned.begin(), owned.end(), c.block);
            const auto index = it - owned.begin();
            c.point->store.invalidSinceNs.erase(c.point->store.invalidSinceNs.begin() + index);
            owned.erase(it);
            retireLocked(const_cast<CacheBlock*>(c.block));
            if (std::find(changed.begin(), changed.end(), c.point) == changed.end()) changed.push_back(c.point);
        }
    }

    for (CachePoint* point : changed) publishLocked(*point);
    // What this round unpublished waits for the callbacks that may still see it.
    if (!pendingRetire_.empty()) {
        stampLocked(pendingRetire_);
        retired_.insert(retired_.end(), pendingRetire_.begin(), pendingRetire_.end());
        pendingRetire_.clear();
    }
    freeRetiredLocked(false);
    freeDyingLocked(false);

    // Empty blocks to capture into, while the budget allows (counting what
    // isn't freed yet).
    // (The background renderer's lane only while it renders.)
    if (enabled) {
        const int lanes = settings_.background.load(std::memory_order_relaxed) ? CachePoint::kLanes : 1;
        for (Entry& entry : points_) {
            for (int l = 0; l < lanes; ++l) {
                CachePoint::Lane& lane = entry.point->lanes[static_cast<size_t>(l)];
                while (lane.storeSpares < kSparesPerPoint &&
                       bytes_ + retiredBytes_ + dyingBytes_ + kBlockBytes <= budget) {
                    auto* block = new CacheBlock;
                    block->samples = std::make_unique<float[]>(2 * kCacheBlockFrames);
                    if (!lane.spares.push(block)) {
                        delete block;
                        break;
                    }
                    bytes_ += kBlockBytes;
                    ++lane.storeSpares;
                }
            }
        }
    }
}

}  // namespace sub
