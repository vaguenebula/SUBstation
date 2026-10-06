#include "cache/BackgroundRenderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace sub {
namespace {

using Clock = std::chrono::steady_clock;

constexpr double kAheadSeconds = 30.0;  // what it looks at first, ahead of the playhead while it plays
constexpr double kLeadSeconds = 0.5;    // (closer than this, the live renderer keeps what plays itself)
constexpr auto kIdleWait = std::chrono::milliseconds(50);      // nothing to render: it looks again
constexpr auto kStuckWait = std::chrono::seconds(2);           // the budget is spent: it waits,
constexpr auto kMaxStuckWait = std::chrono::seconds(64);       // longer each time
constexpr auto kOverloadPause = std::chrono::seconds(5);
// Before a warm-up, as a strip's devices start before their blocks run out
// (RendererCache.cpp): they switch at a chunk's start, up to a chunk early.
constexpr int64_t kMargin = 2 * int64_t{Renderer::kMaxBlock};

// The lowest priority there is: it runs on what no one else wants.
void lowerPriority() {
#ifdef _WIN32
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_IDLE);
#elif defined(SCHED_IDLE)
    sched_param param{};
    pthread_setschedparam(pthread_self(), SCHED_IDLE, &param);
#endif
}

int64_t floorDiv(int64_t a, int64_t b) noexcept { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Where `block`'s good frames end within its cell (as the renderer works it out).
int goodTo(const CacheBlock& block, uint64_t version, const DirtyLog* dirty) noexcept {
    if (block.version != version) return block.from;
    if (!dirty) return block.to;
    const int64_t start = block.start();
    return static_cast<int>(
        dirty->goodUntil(block.generation, start + block.from, start + block.to, block.context) - start);
}

// The first frame in [from, to) good Linear blocks don't cover; `to` if none.
// (Before the song's start, silence covers them, as the renderer has it.)
int64_t uncovered(const BlockSet* blocks, const DirtyLog* dirty, uint64_t version, int64_t from, int64_t to) noexcept {
    int64_t at = std::max<int64_t>(from, std::min<int64_t>(0, to));
    while (at < to) {
        if (!blocks) return at;
        const int64_t cell = floorDiv(at, kCacheBlockFrames);
        const auto offset = static_cast<int>(at - cell * kCacheBlockFrames);
        int good = offset;
        if (!blocks->findGood(cell, {}, offset, [&](const CacheBlock& b) { return goodTo(b, version, dirty); }, good)) {
            return at;
        }
        at = cell * kCacheBlockFrames + good;
    }
    return to;
}

// The first frame in [from, to) a good Linear block covers; `to` if none.
int64_t covered(const BlockSet* blocks, const DirtyLog* dirty, uint64_t version, int64_t from, int64_t to) noexcept {
    if (!blocks) return to;
    for (int64_t cell = floorDiv(from, kCacheBlockFrames); cell * kCacheBlockFrames < to; ++cell) {
        const int64_t start = cell * kCacheBlockFrames;
        const auto offset = static_cast<int>(std::max<int64_t>(0, from - start));
        const int next =
            blocks->nextGood(cell, {}, offset, [&](const CacheBlock& b) { return goodTo(b, version, dirty); });
        if (next >= 0) return std::min(to, start + next);
    }
    return to;
}

}  // namespace

BackgroundRenderer::BackgroundRenderer(Watch watch, double sampleRate, bool thread) : watch_(watch) {
    renderer_.prepare(sampleRate);
    renderer_.setCacheSettings(&watch_.settings);
    renderer_.setCacheLane(CachePoint::kBackgroundLane);
    renderer_.setPlaying(true);
    if (thread) thread_ = std::thread([this] { run(); });
}

BackgroundRenderer::~BackgroundRenderer() {
    {
        std::lock_guard lock(stateMutex_);
        quit_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

bool BackgroundRenderer::park() {
    parkRequested_.store(true, std::memory_order_seq_cst);
    return renderMutex_.try_lock();
}

void BackgroundRenderer::unpark() {
    parkRequested_.store(false, std::memory_order_seq_cst);
    renderMutex_.unlock();
    wake_.notify_all();
}

void BackgroundRenderer::adopt(std::shared_ptr<const RenderSnapshot> snapshot, int64_t songEnd) {
    if (snapshot_) renderer_.finishCaptures(*snapshot_);
    snapshot_ = std::move(snapshot);
    songEnd_ = songEnd;
    renderer_.setRenderEnd(songEnd);
    positioned_ = false;
    lastGap_ = -1;
    stuckUntil_ = {};
    stuckWait_ = {};
    chains_.clear();
    lookFrom_.assign(snapshot_ ? snapshot_->tracks.size() + 1 : 0, 0);
    if (snapshot_) renderer_.syncTempo(*snapshot_);
}

int64_t BackgroundRenderer::renderNow(int64_t frames) {
    std::lock_guard lock(renderMutex_);
    stuckUntil_ = {};
    int64_t done = 0;
    while (done < frames && step()) {
        done += kChunk;
        if (watch_.afterChunk) watch_.afterChunk();
    }
    return done;
}

void BackgroundRenderer::run() {
    lowerPriority();
    const auto wait = [this](Clock::duration duration) {
        std::unique_lock lock(stateMutex_);
        wake_.wait_for(lock, duration, [this] { return quit_; });
        return !quit_;
    };
    for (;;) {
        {
            std::lock_guard lock(stateMutex_);
            if (quit_) return;
        }
        const bool on = watch_.settings.enabled.load(std::memory_order_relaxed) &&
                        watch_.settings.background.load(std::memory_order_relaxed);
        if (!on || parkRequested_.load(std::memory_order_seq_cst)) {
            if (!wait(kIdleWait)) return;
            continue;
        }
        std::unique_lock render(renderMutex_, std::try_to_lock);
        if (!render.owns_lock() || parkRequested_.load(std::memory_order_seq_cst)) {
            if (render.owns_lock()) render.unlock();
            if (!wait(kIdleWait)) return;
            continue;
        }
        const auto started = Clock::now();
        const bool rendered = step();
        render.unlock();
        if (rendered && watch_.afterChunk) watch_.afterChunk();
        if (!rendered) {
            if (!wait(kIdleWait)) return;
            continue;
        }
        // The governor: the live callbacks come first.
        if (watch_.deviceRunning.load(std::memory_order_relaxed)) {
            const float load = watch_.load.load(std::memory_order_relaxed);
            if (load > 0.8f) {
                if (!wait(kOverloadPause)) return;
            } else if (load > 0.6f) {
                if (!wait(Clock::now() - started)) return;  // half its share
            }
        }
    }
}

bool BackgroundRenderer::step() {
    const RenderSnapshot* snap = snapshot_.get();
    if (!snap || Clock::now() < stuckUntil_) return false;
    // (The store frees nothing it unpublished while this reads blocks.)
    watch_.busy.store(true, std::memory_order_seq_cst);
    struct Done {
        Watch& watch;
        ~Done() {
            watch.epoch.fetch_add(1, std::memory_order_seq_cst);
            watch.busy.store(false, std::memory_order_seq_cst);
        }
    } done{watch_};

    const int64_t position = renderer_.position();
    bool replan = !positioned_ || position >= windowEnd_;
    if (!replan && ahead_ && watch_.playing.load(std::memory_order_relaxed)) {
        // It went for a gap ahead of the playhead, and the playhead caught up:
        // what it would render next plays before it is done. (Counting from the
        // gap: the warm-up before it is never kept.)
        const int64_t lead = std::llround(kLeadSeconds * snap->sampleRate);
        replan = std::max(position, lastGap_) < watch_.playhead.load(std::memory_order_relaxed) + lead;
    }
    if (!replan && position >= nextCheck_) {
        // Is there more to render close ahead? If not, it jumps.
        const int64_t gap = firstGap(position, windowEnd_);
        replan = gap >= windowEnd_ || gap - position > longestLead() + kCacheBlockFrames;
        nextCheck_ = position + kCacheBlockFrames;
    }
    if (replan && !plan()) return false;
    renderer_.renderBackground(*snap, kChunk, false);
    framesRendered_.fetch_add(kChunk, std::memory_order_relaxed);
    return true;
}

void BackgroundRenderer::computeChains() {
    const RenderSnapshot& snap = *snapshot_;
    const int64_t warm =
        std::llround(std::max(0.0, watch_.settings.warmSeconds.load(std::memory_order_relaxed)) * snap.sampleRate);
    const size_t tracks = snap.tracks.size();
    chains_.assign(tracks + 1, {});
    std::vector<char> state(tracks, 0);  // 0: not yet, 1: on the way (a cycle: never), 2: done
    const auto own = [](const StripCacheRender& cache) {
        const CachePoint* point = cache.point.get();
        return point && !cache.unavailable && point->version.load(std::memory_order_acquire) == cache.version;
    };
    // The renderer's rule (RendererCache.cpp): a strip is clean a warm-up and
    // its latency after it starts, or after what feeds it is clean.
    const auto chain = [&](auto& self, size_t t) -> const Chain& {
        Chain& c = chains_[t];
        if (state[t] == 2) return c;
        if (state[t] == 1) return c;  // (keep stays false)
        state[t] = 1;
        const TrackRender& track = snap.tracks[t];
        bool keep = own(track.cache);
        int64_t lead = 0;
        for (const int source : track.cache.sources) {
            if (source < 0 || static_cast<size_t>(source) >= tracks) continue;
            const Chain& from = self(self, static_cast<size_t>(source));
            keep = keep && from.keep;
            lead = std::max(lead, from.lead);
        }
        c.keep = keep;
        c.own = warm + track.inputLatency + track.latency;
        c.lead = lead + c.own;
        state[t] = 2;
        return c;
    };
    for (size_t t = 0; t < tracks; ++t) chain(chain, t);
    Chain& master = chains_[tracks];
    master.keep = own(snap.masterCache);
    for (const int source : snap.masterCache.sources) {
        if (source < 0 || static_cast<size_t>(source) >= tracks) continue;
        master.keep = master.keep && chains_[static_cast<size_t>(source)].keep;
        master.lead = std::max(master.lead, chains_[static_cast<size_t>(source)].lead);
    }
    master.own = warm + snap.maxLatency + snap.master.latency;
    master.lead += master.own;
}

int64_t BackgroundRenderer::leadAt(size_t strip, int64_t gap, int depth) const {
    const RenderSnapshot& snap = *snapshot_;
    const Chain& chain = chains_[strip];
    if (depth >= 64) return chain.lead;
    const StripCacheRender& cache = strip < snap.tracks.size() ? snap.tracks[strip].cache : snap.masterCache;
    int64_t longest = 0;
    for (const int source : cache.sources) {
        if (source < 0 || static_cast<size_t>(source) >= snap.tracks.size()) continue;
        const StripCacheRender& from = snap.tracks[static_cast<size_t>(source)].cache;
        const int64_t start = gap - chain.own - kMargin;
        const bool cached = from.cacheable && from.point &&
                            uncovered(from.point->blocks.load(std::memory_order_acquire), from.dirty.get(),
                                      from.version, start, gap) >= gap;
        if (!cached) longest = std::max(longest, leadAt(static_cast<size_t>(source), gap, depth + 1));
    }
    return chain.own + longest;
}

int64_t BackgroundRenderer::longestLead() const {
    int64_t lead = 0;
    for (const Chain& c : chains_) {
        if (c.keep) lead = std::max(lead, c.lead);
    }
    return lead;
}

bool BackgroundRenderer::plan() {
    const RenderSnapshot& snap = *snapshot_;
    // A jump: what it was capturing goes to the store now, to be counted.
    renderer_.finishCaptures(snap);
    if (watch_.afterChunk) watch_.afterChunk();
    computeChains();
    const int64_t end = songEnd_;
    const int64_t playhead = std::clamp<int64_t>(watch_.playhead.load(std::memory_order_relaxed), 0, end);
    const int64_t lead = std::llround(kLeadSeconds * snap.sampleRate);
    const int64_t ahead = std::llround(kAheadSeconds * snap.sampleRate);
    struct Window {
        int64_t from, to;
    };
    // (While it plays: not what plays before it would be done.)
    const int64_t next = watch_.playing.load(std::memory_order_relaxed) ? playhead + lead : playhead;
    Window windows[3] = {{0, 0}, {next, end}, {0, playhead}};
    if (watch_.playing.load(std::memory_order_relaxed)) windows[0] = {next, std::min(end, playhead + ahead)};
    for (const Window& window : windows) {
        while (window.from < window.to) {
            const int64_t gap = firstGap(window.from, window.to);
            if (gap >= window.to) break;
            const uint64_t lost = framesLost();
            const bool playing = watch_.playing.load(std::memory_order_relaxed);
            if (gap == lastGap_ && positioned_ && renderer_.position() <= gap) {
                // (Still on its way there: it renders on.)
                ahead_ = playing && gap >= playhead;
                windowEnd_ = window.to;
                return true;
            }
            // The same gap again, rendered past: what it rendered there wasn't
            // kept. With no blocks to keep it in (the budget is spent), it waits;
            // otherwise it looks past it for the strips without one there.
            if (gap == lastGap_) {
                lastGap_ = -1;
                if (lost != lostAtPlan_) {
                    stuckWait_ = std::clamp<Clock::duration>(stuckWait_ * 2, kStuckWait, kMaxStuckWait);
                    stuckUntil_ = Clock::now() + stuckWait_;
                    positioned_ = false;
                    return false;
                }
                skipGap(gap);
                continue;
            }
            lastGap_ = gap;
            lostAtPlan_ = lost;
            ahead_ = playing && gap >= playhead;
            // From a warm-up before it (the longest of the strips it is for):
            // before the song's start if need be, through the silence there.
            renderer_.preRollFrom(gap - prerollAt(gap) - kMargin);
            windowEnd_ = window.to;
            nextCheck_ = gap + kCacheBlockFrames;
            positioned_ = true;
            return true;
        }
    }
    positioned_ = false;  // (nothing more to render)
    lastGap_ = -1;
    stuckWait_ = {};
    return false;
}

void BackgroundRenderer::skipGap(int64_t gap) {
    const RenderSnapshot& snap = *snapshot_;
    const auto skip = [&](const StripCacheRender& cache, size_t strip) {
        if (!cache.cacheable || strip >= chains_.size() || !chains_[strip].keep) return;
        const BlockSet* blocks = cache.point->blocks.load(std::memory_order_acquire);
        if (uncovered(blocks, cache.dirty.get(), cache.version, gap, gap + 1) != gap) return;
        lookFrom_[strip] = std::max(lookFrom_[strip], covered(blocks, cache.dirty.get(), cache.version, gap, songEnd_));
    };
    for (size_t t = 0; t < snap.tracks.size(); ++t) skip(snap.tracks[t].cache, t);
    skip(snap.masterCache, snap.tracks.size());
}

uint64_t BackgroundRenderer::framesLost() const {
    uint64_t frames = 0;
    const auto add = [&frames](const StripCacheRender& cache) {
        if (cache.point) frames += cache.point->lanes[CachePoint::kBackgroundLane].framesLost.load(std::memory_order_relaxed);
    };
    for (const TrackRender& track : snapshot_->tracks) add(track.cache);
    add(snapshot_->masterCache);
    return frames;
}

int64_t BackgroundRenderer::firstGap(int64_t from, int64_t to) const {
    const RenderSnapshot& snap = *snapshot_;
    int64_t first = to;
    const auto look = [&](const StripCacheRender& cache, size_t strip) {
        // (Not one it can keep: not worth caching, or without shadows up to date, it or what feeds it.)
        if (!cache.cacheable || strip >= chains_.size() || !chains_[strip].keep) return;
        const int64_t start = std::max(from, lookFrom_[strip]);
        if (start >= first) return;
        const BlockSet* blocks = cache.point->blocks.load(std::memory_order_acquire);
        first = std::min(first, uncovered(blocks, cache.dirty.get(), cache.version, start, first));
    };
    for (size_t t = 0; t < snap.tracks.size(); ++t) look(snap.tracks[t].cache, t);
    look(snap.masterCache, snap.tracks.size());
    return first;
}

int64_t BackgroundRenderer::prerollAt(int64_t gap) const {
    const RenderSnapshot& snap = *snapshot_;
    int64_t longest = 0;
    const auto look = [&](const StripCacheRender& cache, size_t strip) {
        if (!cache.cacheable || strip >= chains_.size() || !chains_[strip].keep || lookFrom_[strip] > gap) return;
        const BlockSet* blocks = cache.point->blocks.load(std::memory_order_acquire);
        if (uncovered(blocks, cache.dirty.get(), cache.version, gap, gap + 1) != gap) return;
        longest = std::max(longest, leadAt(strip, gap));
    };
    for (size_t t = 0; t < snap.tracks.size(); ++t) look(snap.tracks[t].cache, t);
    look(snap.masterCache, snap.tracks.size());
    return longest;
}

}  // namespace sub
