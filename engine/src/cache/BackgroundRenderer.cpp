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
    skipped_.assign(snapshot_ ? snapshot_->tracks.size() + 1 : 0, {});
    if (snapshot_) renderer_.syncTempo(*snapshot_);
}

int64_t BackgroundRenderer::renderNow(int64_t frames) {
    std::lock_guard lock(renderMutex_);
    stuckUntil_ = {};
    int64_t done = 0;
    for (auto started = Clock::now(); done < frames && step(); started = Clock::now()) {
        done += kChunk;
        if (watch_.afterChunk) watch_.afterChunk();
        timeChunk(Clock::now() - started);
    }
    return done;
}

void BackgroundRenderer::timeChunk(Clock::duration took) {
    const double seconds = std::chrono::duration<double>(took).count();
    secondsPerChunk_ = secondsPerChunk_ > 0.0 ? 0.9 * secondsPerChunk_ + 0.1 * seconds : seconds;
}

double BackgroundRenderer::speed(double sampleRate) const {
    if (secondsPerChunk_ <= 0.0) return 1.0;  // (not known yet: as fast as it plays)
    return std::clamp(kChunk / sampleRate / secondsPerChunk_, 0.05, 1000.0);
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
        timeChunk(Clock::now() - started);  // (waits too: how fast it gets on)
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
        c.own = (track.cache.devicesOn ? warm : 0) + track.inputLatency + track.latency;  // (without devices: no warm-up)
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
    master.own = (snap.masterCache.devicesOn ? warm : 0) + snap.maxLatency + snap.master.latency;
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
        // (Over as long as all of its chain could take: what feeds what feeds
        // it is live from the start of it, if that isn't cached.)
        const int64_t start = gap - chain.lead - kMargin;
        const bool cached = from.cacheable && from.point &&
                            uncovered(from.point->blocks.load(std::memory_order_acquire), from.dirty.get(),
                                      from.version, start, gap) >= gap;
        if (!cached) longest = std::max(longest, leadAt(static_cast<size_t>(source), gap, depth + 1));
    }
    return chain.own + longest;
}

int64_t BackgroundRenderer::startAt(int64_t gap, std::vector<size_t>* running) const {
    const RenderSnapshot& snap = *snapshot_;
    const size_t tracks = snap.tracks.size();
    // The strips whose devices run up to the gap: those it is for (prerollAt),
    // and what feeds them that doesn't play from its cache meanwhile (leadAt).
    std::vector<char> runs(tracks + 1, 0);
    const auto mark = [&](auto& self, size_t strip, int depth) -> void {
        if (runs[strip] || depth >= 64) return;
        runs[strip] = 1;
        const StripCacheRender& cache = strip < tracks ? snap.tracks[strip].cache : snap.masterCache;
        for (const int source : cache.sources) {
            if (source < 0 || static_cast<size_t>(source) >= tracks) continue;
            const StripCacheRender& from = snap.tracks[static_cast<size_t>(source)].cache;
            const int64_t start = gap - chains_[strip].lead - kMargin;
            const bool cached = from.cacheable && from.point &&
                                uncovered(from.point->blocks.load(std::memory_order_acquire), from.dirty.get(),
                                          from.version, start, gap) >= gap;
            if (!cached) self(self, static_cast<size_t>(source), depth + 1);
        }
    };
    for (size_t t = 0; t <= tracks; ++t) {
        const StripCacheRender& cache = t < tracks ? snap.tracks[t].cache : snap.masterCache;
        if (!cache.cacheable || t >= chains_.size() || !chains_[t].keep) continue;
        if (stripGap(cache, t, gap, gap + 1) == gap) mark(mark, t, 0);
    }
    // From a warm-up before it, and before any note those instruments hold
    // there: devices that start while one sounds aren't clean until it ends
    // (it starts late). (The song's start holds none.)
    int64_t start = gap - prerollAt(gap) - kMargin;
    for (bool moved = true; moved && start > 0;) {
        moved = false;
        for (size_t t = 0; t < tracks; ++t) {
            const TrackRender& track = snap.tracks[t];
            if (!runs[t] || !track.cache.devicesOn) continue;
            for (const NoteRender& note : track.notes) {  // (by start)
                if (note.start >= start) break;
                if (note.end > start) {
                    start = note.start;
                    moved = true;
                    break;
                }
            }
        }
    }
    if (running) {
        running->clear();
        for (size_t t = 0; t <= tracks; ++t) {
            if (runs[t]) running->push_back(t);
        }
    }
    return start;
}

void BackgroundRenderer::runDevices(int64_t from, int64_t gap, const std::vector<size_t>& strips) {
    const RenderSnapshot& snap = *snapshot_;
    const auto lane = [&](size_t strip) -> CachePoint::LiveState* {
        const StripCacheRender& cache = strip < snap.tracks.size() ? snap.tracks[strip].cache : snap.masterCache;
        return cache.point ? &cache.point->lanes[CachePoint::kBackgroundLane].state : nullptr;
    };
    for (size_t t = 0; t <= snap.tracks.size(); ++t) {
        if (CachePoint::LiveState* s = lane(t)) s->runFrom = s->runTo = 0;
    }
    for (const size_t t : strips) {
        if (CachePoint::LiveState* s = lane(t)) {
            s->runFrom = from;
            s->runTo = gap;
        }
    }
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
        int64_t from = window.from;
        while (from < window.to) {
            const int64_t gap = firstGap(from, window.to);
            if (gap >= window.to) break;
            const uint64_t lost = framesLost();
            const bool playing = watch_.playing.load(std::memory_order_relaxed);
            // A gap ahead it can't warm up for before the playhead gets there (at
            // half the speed it renders at lately) is the live renderer's: it
            // plays it, and keeps it. Going for it anyway, it would start again
            // each time the playhead moved on, and never keep anything. Further on.
            if (playing && gap >= playhead) {
                const auto warmUp = static_cast<double>(gap - startAt(gap));
                const int64_t reach = playhead + lead + std::llround(2.0 * warmUp / speed(snap.sampleRate));
                if (gap < reach) {
                    from = reach;
                    continue;
                }
            }
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
            // From a warm-up before it (the longest of the strips it is for), and
            // where their notes allow (startAt): before the song's start if need
            // be, through the silence there. Their devices run from there on,
            // even where their blocks are good, so that they are clean by the gap.
            std::vector<size_t> running;
            const int64_t start = startAt(gap, &running);
            runDevices(start, gap, running);
            renderer_.preRollFrom(start);
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
        skipped_[strip].push_back({gap, covered(blocks, cache.dirty.get(), cache.version, gap, songEnd_)});
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
        first = stripGap(cache, strip, from, first);
    };
    for (size_t t = 0; t < snap.tracks.size(); ++t) look(snap.tracks[t].cache, t);
    look(snap.masterCache, snap.tracks.size());
    return first;
}

int64_t BackgroundRenderer::stripGap(const StripCacheRender& cache, size_t strip, int64_t from, int64_t to) const {
    const BlockSet* blocks = cache.point->blocks.load(std::memory_order_acquire);
    int64_t at = from;
    while (at < to) {
        at = uncovered(blocks, cache.dirty.get(), cache.version, at, to);
        bool skip = false;
        for (const Skipped& s : skipped_[strip]) {
            if (s.from <= at && at < s.to) {
                at = s.to;
                skip = true;
            }
        }
        if (!skip) return at;
    }
    return to;
}

int64_t BackgroundRenderer::prerollAt(int64_t gap) const {
    const RenderSnapshot& snap = *snapshot_;
    int64_t longest = 0;
    const auto look = [&](const StripCacheRender& cache, size_t strip) {
        if (!cache.cacheable || strip >= chains_.size() || !chains_[strip].keep) return;
        if (stripGap(cache, strip, gap, gap + 1) != gap) return;
        longest = std::max(longest, leadAt(strip, gap));
    };
    for (size_t t = 0; t < snap.tracks.size(); ++t) look(snap.tracks[t].cache, t);
    look(snap.masterCache, snap.tracks.size());
    return longest;
}

}  // namespace sub
