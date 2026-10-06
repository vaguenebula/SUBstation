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
                       const std::atomic<bool>& deviceRunning, const std::atomic<int64_t>& playhead)
    : settings_(settings), audioEpoch_(audioEpoch), deviceRunning_(deviceRunning), playhead_(playhead) {
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
        CacheBlock* block = nullptr;
        while (point.completed.pop(block)) {
            --point.store.spares;
            retireLocked(block);
        }
        dropAllLocked(point);
        // The empty blocks it still has go with it (CachePoint's destructor),
        // once the snapshots still showing it have gone.
        const size_t spares = static_cast<size_t>(point.store.spares) * kBlockBytes;
        bytes_ -= spares;
        point.store.spares = 0;
        dyingBytes_ += spares;
        dying_.push_back({old.point, spares});
    }
    points_ = std::move(points);
    if (!pendingRetire_.empty()) {
        const uint64_t epoch = audioEpoch_.load(std::memory_order_seq_cst);
        for (Retired& r : pendingRetire_) r.epoch = epoch;
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
    if (block.version != point.version.load(std::memory_order_acquire)) return false;
    if (!dirty) return true;
    const int64_t start = block.start() + block.from;
    return dirty->goodUntil(block.generation, start, block.start() + block.to, block.context) > start;
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

void CacheStore::respareLocked(CachePoint& point, CacheBlock* block) {
    block->from = block->to = 0;
    if (point.spares.push(block)) {
        ++point.store.spares;
    } else {
        bytes_ -= block->bytes();
        delete block;
    }
}

void CacheStore::ingestLocked(CachePoint& point, CacheBlock* block, const DirtyLog* dirty) {
    --point.store.spares;  // (it was never published: it can be handed out again at once)
    // Empty, or captured at a version or generation already gone (it would never play).
    if (block->to <= block->from || !goodLocked(point, *block, dirty)) {
        respareLocked(point, block);
        return;
    }
    // Where it overlaps the good blocks of its cell and context, they keep their
    // frames (it is cut short at an end they cover); those it covers whole, and
    // those no longer good, go.
    auto& owned = point.store.owned;
    auto& since = point.store.invalidSinceNs;
    for (const CacheBlock* old : owned) {
        if (old->cell != block->cell || old->context != block->context) continue;
        if (old->to <= block->from || block->to <= old->from) continue;
        if (!goodLocked(point, *old, dirty)) continue;
        if (old->from <= block->from) block->from = std::max(block->from, old->to);
        if (old->to >= block->to) block->to = std::min(block->to, old->from);
        if (block->to <= block->from) break;
    }
    if (block->to <= block->from) {  // nothing new in it
        respareLocked(point, block);
        return;
    }
    if (silentBlock(*block)) {
        bytes_ -= block->bytes();
        block->samples.reset();
        block->silent = true;
    }
    for (size_t i = 0; i < owned.size();) {
        CacheBlock* old = owned[i];
        if (old->cell == block->cell && old->context == block->context && old->to > block->from &&
            block->to > old->from) {
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

void CacheStore::freeRetiredLocked(bool all) {
    const uint64_t epoch = audioEpoch_.load(std::memory_order_seq_cst);
    const bool idle = all || !deviceRunning_.load(std::memory_order_seq_cst);
    std::erase_if(retired_, [&](const Retired& r) {
        if (!idle && epoch <= r.epoch) return false;
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
        CacheBlock* block = nullptr;
        while (point.completed.pop(block)) {
            if (enabled) {
                ingestLocked(point, block, dirty);
            } else {
                --point.store.spares;
                retireLocked(block);
            }
            touched = true;
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
        const uint64_t epoch = audioEpoch_.load(std::memory_order_seq_cst);
        for (Retired& r : pendingRetire_) r.epoch = epoch;
        retired_.insert(retired_.end(), pendingRetire_.begin(), pendingRetire_.end());
        pendingRetire_.clear();
    }
    freeRetiredLocked(false);
    freeDyingLocked(false);

    // Empty blocks to capture into, while the budget allows (counting what
    // isn't freed yet).
    if (enabled) {
        for (Entry& entry : points_) {
            CachePoint& point = *entry.point;
            while (point.store.spares < kSparesPerPoint &&
                   bytes_ + retiredBytes_ + dyingBytes_ + kBlockBytes <= budget) {
                auto* block = new CacheBlock;
                block->samples = std::make_unique<float[]>(2 * kCacheBlockFrames);
                if (!point.spares.push(block)) {
                    delete block;
                    break;
                }
                bytes_ += kBlockBytes;
                ++point.store.spares;
            }
        }
    }
}

}  // namespace sub
