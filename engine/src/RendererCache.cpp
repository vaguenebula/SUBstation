// Renderer: background freezing, phase 1 (cache/StripCache.h, and
// docs/engine/background-freeze.md). Each strip, each live chunk: is its cache
// good for the whole chunk, and should it play? If so its devices don't run;
// if not they do, and what they put out is kept when it is what they would put
// out had they run from far before (their state carries nothing the arrangement
// doesn't). Where the output changes from one to the other, it crossfades.
#include "Renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace sub {
namespace {

int64_t floorDiv(int64_t a, int64_t b) noexcept { return a >= 0 ? a / b : -((-a + b - 1) / b); }

// Where `block`'s good frames end (within its cell): its version must be the
// strip's, and a later time-local change ends it where that begins.
int goodTo(const CacheBlock& block, uint64_t version, const DirtyLog* dirty,
           const DirtyAmendment* amend = nullptr) noexcept {
    if (block.version != version) return block.from;
    if (!dirty) return block.to;
    const int64_t start = block.start();
    return static_cast<int>(
        dirty->goodUntil(block.generation, start + block.from, start + block.to, block.context, amend) - start);
}

// Frames good blocks cover from `position` on in `context`, up to `length`.
// (Before the song's start, silence covers them: nothing plays there. Only the
// background renderer goes there, to warm up.)
int64_t coveredFrom(const BlockSet* blocks, const DirtyLog* dirty, uint64_t version, int64_t position,
                    const CacheContext& context, int64_t length, const DirtyAmendment* amend = nullptr) noexcept {
    int64_t got = std::clamp<int64_t>(-position, 0, length);
    if (!blocks) return got;
    while (got < length) {
        const int64_t at = position + got;
        const int64_t cell = floorDiv(at, kCacheBlockFrames);
        const auto offset = static_cast<int>(at - cell * kCacheBlockFrames);
        int to = offset;
        if (!blocks->findGood(cell, context, offset,
                              [&](const CacheBlock& b) { return goodTo(b, version, dirty, amend); }, to)) {
            break;
        }
        got += std::min<int64_t>(length - got, to - offset);
    }
    return got;
}

// The rendering thread takes a strip's devices to run them (false: idle() is
// resetting them).
bool claimDevices(CachePoint& point) noexcept {
    int owner = point.devicesOwner.load(std::memory_order_acquire);
    if (owner == CachePoint::kDevicesRunning) return true;
    return owner == CachePoint::kDevicesIdle &&
           point.devicesOwner.compare_exchange_strong(owner, CachePoint::kDevicesRunning, std::memory_order_acq_rel);
}

// How long the notes sounding at `position` (begun before it) still sound.
// Playing that starts there (a jump, devices starting again) leaves them out
// or starts them late, which is not what the arrangement plays, until they end.
int64_t heldAcross(const TrackRender& track, int64_t position) noexcept {
    int64_t held = 0;
    for (const NoteRender& note : track.notes) {
        if (note.start >= position) break;
        if (note.end > position) held = std::max(held, note.end - position);
    }
    return held;
}

// Whether the changes since `generation` over [from, to) of a strip whose
// devices run there have passed (each began, and its own part ended, before
// `from`), and there is one (else nothing changed there). `until`: as far as
// they can ring on.
bool changesPassed(const DirtyLog& dirty, uint64_t generation, int64_t from, int64_t to, int64_t& until) noexcept {
    if (generation < dirty.horizon) return false;
    bool any = false;
    until = from;
    for (const DirtyLog::Entry& e : dirty.entries) {
        if (e.generation <= generation || e.from >= to || e.to <= from) continue;
        if (e.core > from) return false;
        any = true;
        until = std::max(until, e.to);
    }
    return any;
}

// Whether what `cache`'s strip puts out over [from, to) may differ from what it
// put out at generation `generation`: a change since still rings on in it (as
// far as its background lane found out), or, for one without devices to ring
// on, it changed itself or what feeds it did.
bool outChangedSince(const RenderSnapshot& snap, const StripCacheRender& cache, uint64_t generation, int64_t from,
                     int64_t to, int depth) noexcept {
    const DirtyLog* dirty = cache.dirty.get();
    if (cache.devicesOn) {
        const CachePoint::LiveState* s =
            cache.cacheable && cache.point ? &cache.point->lanes[CachePoint::kBackgroundLane].state : nullptr;
        const DirtyAmendment* amend = s && s->amended && s->amendVersion == cache.version ? &s->amend : nullptr;
        return dirty && dirty->goodUntil(generation, from, to, {}, amend) < to;
    }
    if (dirty) {
        if (generation < dirty->horizon) return true;
        for (const DirtyLog::Entry& e : dirty->entries) {
            if (e.generation > generation && e.from < to && e.core > from) return true;
        }
    }
    if (depth >= 16) return true;
    for (const int t : cache.sources) {
        if (outChangedSince(snap, snap.tracks[static_cast<size_t>(t)].cache, generation, from, to, depth + 1)) {
            return true;
        }
    }
    return false;
}

}  // namespace

void Renderer::prepareCacheChunk(const RenderSnapshot& snap, int frames, ChunkFlags flags) noexcept {
    CacheChunk& c = cacheChunk_;
    c.on = flags.live && cacheSettings_ && cacheSettings_->enabled.load(std::memory_order_relaxed) &&
           (cacheLane_ == CachePoint::kLiveLane || cacheSettings_->background.load(std::memory_order_relaxed));
    c.usable = false;
    c.numPieces = 0;
    c.jumpAt = -1;
    c.wrapAt = -1;
    c.startPosition = position_;
    c.wrappedLately = false;
    c.generation = snap.generation;
    c.looping = flags.loop && !recording_ && snap.loopEnabled;
    const double warmSeconds = cacheSettings_ ? cacheSettings_->warmSeconds.load(std::memory_order_relaxed) : 0.0;
    c.warmFrames = std::max<int64_t>(0, std::llround(warmSeconds * snap.sampleRate));
    c.contextFrames = c.warmFrames + snap.outputLatency();
    if (c.on) {
        c.nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
        c.idleNs = std::llround(std::max(0.0, cacheSettings_->idleSeconds.load(std::memory_order_relaxed)) * 1e9);
        c.plannedFade = std::max(1, static_cast<int>(std::lround(0.005 * snap.sampleRate)));
        c.unplannedFade = std::max(1, static_cast<int>(std::lround(0.020 * snap.sampleRate)));
    }
    // The playhead's history is kept whether the cache is on or not, so that it
    // is right when it comes on: frames since the loop last wrapped (what plays
    // carries the loop's end that long), or "long ago" after a jump.
    constexpr int64_t kLongAgo = std::numeric_limits<int64_t>::max() / 2;
    if (!playing_ || numSegments_ == 0) {
        framesSinceWrap_ = kLongAgo;
        c.endPosition = position_ + frames;
        c.playedAfter = playedFrames_;
        c.jumps = jumps_;
        return;
    }
    bool usable = segments_[0].offset == 0;  // (a count-in's end stands still first)
    c.startPosition = segments_[0].position;
    for (int s = 0; s < numSegments_; ++s) {
        const Segment& segment = segments_[s];
        if (segment.wrap) {
            framesSinceWrap_ = 0;
            lastWrap_ = {snap.loopEnd, segment.position};
            if (c.wrapAt < 0) c.wrapAt = segment.offset;
        } else if (segment.jump || segment.chase) {  // (playing starts: what the devices did stopped isn't the arrangement)
            framesSinceWrap_ = kLongAgo;
            ++jumps_;
            if (c.jumpAt < 0) {
                c.jumpAt = segment.offset;
                c.jumpTo = segment.position;
            }
        }
        int done = 0;
        while (done < segment.length) {
            if (c.numPieces >= kMaxCachePieces) {
                usable = false;
                break;
            }
            CachePiece piece{segment.offset + done, segment.length - done, segment.position + done, {}};
            if (framesSinceWrap_ < c.contextFrames) {
                piece.context = lastWrap_;
                piece.length = static_cast<int>(std::min<int64_t>(piece.length, c.contextFrames - framesSinceWrap_));
            }
            c.pieces[static_cast<size_t>(c.numPieces++)] = piece;
            framesSinceWrap_ = std::min(kLongAgo, framesSinceWrap_ + piece.length);
            done += piece.length;
        }
        c.endPosition = segment.position + segment.length;
    }
    c.wrappedLately = framesSinceWrap_ < c.contextFrames + frames;
    playedFrames_ += static_cast<uint64_t>(frames - segments_[0].offset);
    c.playedAfter = playedFrames_;
    c.jumps = jumps_;
    c.usable = usable && c.numPieces > 0;
}

bool Renderer::cacheCovers(const BlockSet* blocks, const DirtyLog* dirty, uint64_t version,
                           const DirtyAmendment* amend) const noexcept {
    const CacheChunk& c = cacheChunk_;
    for (int p = 0; p < c.numPieces; ++p) {
        const CachePiece& piece = c.pieces[static_cast<size_t>(p)];
        if (coveredFrom(blocks, dirty, version, piece.position, piece.context, piece.length, amend) < piece.length) {
            return false;
        }
    }
    return c.numPieces > 0;
}

int64_t Renderer::cacheRunway(const RenderSnapshot& snap, const BlockSet* blocks, const DirtyLog* dirty,
                              uint64_t version, int64_t most, const DirtyAmendment* amend) const noexcept {
    const CacheChunk& c = cacheChunk_;
    int64_t position = position_;
    int64_t sinceWrap = framesSinceWrap_;
    CacheContext wrap = lastWrap_;
    int64_t covered = 0;
    for (int guard = 0; covered < most && guard < 64; ++guard) {
        int64_t length = most - covered;
        if (c.looping && position < snap.loopEnd) length = std::min(length, snap.loopEnd - position);
        CacheContext context;
        if (sinceWrap < c.contextFrames) {
            context = wrap;
            length = std::min(length, c.contextFrames - sinceWrap);
        }
        const int64_t got = coveredFrom(blocks, dirty, version, position, context, length, amend);
        covered += got;
        if (renderEnd_ > 0 && !c.looping && position + got >= renderEnd_) return most;  // (nothing to warm up for after)
        if (got < length) break;
        position += length;
        sinceWrap += length;
        if (c.looping && position == snap.loopEnd) {
            position = snap.loopStart;
            sinceWrap = 0;
            wrap = {snap.loopEnd, snap.loopStart};
        }
    }
    return covered;
}

Renderer::CacheStep Renderer::beginCacheStep(const RenderSnapshot& snap, const StripCacheRender& cache,
                                             const TrackRender* track, bool liveInput, int frames) noexcept {
    CacheStep step;
    CachePoint* point = cache.point.get();
    if (!point) return step;
    CachePoint::Lane& lane = point->lanes[static_cast<size_t>(cacheLane_)];
    CachePoint::LiveState& s = lane.state;
    using Mode = CachePoint::LiveState::Mode;
    using Fade = CachePoint::LiveState::Fade;
    const CacheChunk& c = cacheChunk_;
    // Its devices hear the timeline as late as its input's and their own
    // latency make them: they are warm a warm-up after that.
    const int64_t latency = track ? int64_t{track->inputLatency} + track->latency
                                  : int64_t{snap.maxLatency} + snap.master.latency;
    const int64_t warmFrames = c.warmFrames + latency;
    if (!c.on) {  // its devices run as ever; when the cache comes on, they start out fresh (seen nothing)
        if (s.building) s.building->from = s.building->to = 0;
        s.mode = Mode::Live;
        s.fade = Fade::None;
        s.idleInstance = false;
        s.seenVersion = 0;
        s.pendingOut = s.liveInputOut = false;
        s.cleanInOut = 0;
        if (cacheLane_ == CachePoint::kLiveLane) {
            point->idleSinceNs.store(0, std::memory_order_relaxed);
            claimDevices(*point);  // (a reset idle() began just before the cache went off: it is safe, if dry, for a chunk)
        }
        return step;
    }
    step.active = true;

    // What feeds it, this chunk (rendered already: the graph runs sources first).
    bool sourcePending = false, sourceLive = false;
    int64_t sourceClean = 0;
    for (const int t : cache.sources) {
        const CachePoint* source = snap.tracks[static_cast<size_t>(t)].cache.point.get();
        if (!source) continue;
        const CachePoint::LiveState& from = source->lanes[static_cast<size_t>(cacheLane_)].state;
        sourcePending = sourcePending || from.pendingOut;
        sourceLive = sourceLive || from.liveInputOut;
        sourceClean = std::max(sourceClean, from.cleanInOut);
    }
    // Its devices. (The edit side bumps the version, then says which changes it
    // accounts for: seeing the second, the first is seen too.) In the background,
    // shadows in the state of the version they were brought to: if the strip has
    // changed since, what they render is out of date (as is what it feeds).
    const bool background = cacheLane_ == CachePoint::kBackgroundLane;
    uint64_t changes = 0, resets = 0;
    for (const auto& device : cache.devices) {
        changes += device->changeCount();
        resets += device->resetCount();
    }
    const uint64_t accounted = point->accountedChanges.load(std::memory_order_acquire);
    const uint64_t current = point->version.load(std::memory_order_acquire);
    const uint64_t version = background ? cache.version : current;
    const bool pending = sourcePending || (background ? cache.unavailable || current != cache.version
                                                      : changes != accounted);
    if (liveInput) s.liveHold = warmFrames + frames;  // (what it played carries on in its devices a while)
    const bool carriesLive = sourceLive || s.liveHold > 0;
    s.liveHold = std::max<int64_t>(0, s.liveHold - frames);
    const BlockSet* blocks = point->blocks.load(std::memory_order_acquire);
    const DirtyLog* dirty = cache.dirty.get();
    step.blocks = blocks;
    step.version = version;

    // Whether its devices' state still matches the arrangement: not after they
    // stood idle, a jump, a reset, a change of theirs, or an edit of what they
    // already played.
    bool fresh = s.idleInstance || c.jumpAt >= 0 || version != s.seenVersion || changes != s.seenChanges ||
                 resets != s.seenResets || liveInput;
    // (What its devices still carry: about a warm-up's worth before this chunk,
    // and the loop's end if it wrapped within that.)
    if (c.generation != s.seenGeneration && dirty &&
        dirty->changedSince(s.seenGeneration, c.endPosition - frames - warmFrames, c.endPosition,
                            c.wrappedLately ? lastWrap_ : CacheContext{})) {
        fresh = true;
    }

    // (In the background: as good as what its lane found out about the changes
    // since its blocks were captured, while its version stays.)
    const DirtyAmendment* amend = nullptr;
    if (background && s.amended) {
        if (s.amendVersion == version) {
            amend = &s.amend;
        } else {
            s.amended = false;
        }
    }
    step.amend = amend;
    const bool allowed = c.usable && cache.cacheable && !pending && !carriesLive;
    const bool valid = allowed && cacheCovers(blocks, dirty, version, amend);
    // (What the background renders is never heard: it plays from its cache
    // wherever that is good, whoever may be editing or watching.)
    const bool hot = !background && c.nowNs - point->lastChangeNs.load(std::memory_order_relaxed) < c.idleNs;
    const bool observed = !background && (point->observed.load(std::memory_order_relaxed) ||
                                          c.nowNs < point->observedUntilNs.load(std::memory_order_relaxed));
    // A planned seam: the blocks it will need run out within a warm-up, so its
    // devices start now, while the cache still plays, to be warm by then.
    bool exitAhead = false;
    if (valid && !hot && !observed) {
        const int64_t need = warmFrames + 2 * int64_t{frames};  // (the switch can come a chunk before they run out)
        int64_t remaining = static_cast<int64_t>(s.runwayUntil - c.playedAfter);
        if (s.runwayVersion != version || s.runwayGeneration != c.generation || s.runwayBlocks != blocks ||
            s.runwayJumps != c.jumps || remaining < need) {
            remaining = cacheRunway(snap, blocks, dirty, version, need + kMaxBlock, amend);
            s.runwayUntil = c.playedAfter + static_cast<uint64_t>(remaining);
            s.runwayVersion = version;
            s.runwayGeneration = c.generation;
            s.runwayBlocks = blocks;
            s.runwayJumps = c.jumps;
        }
        exitAhead = remaining < need;
    }
    const bool warm = !fresh && !s.idleInstance && s.warmIn == 0;
    Mode mode = Mode::Live;
    if (valid && (hot || observed || exitAhead)) {
        mode = warm ? Mode::Live : Mode::PreRoll;
    } else if (valid) {
        mode = Mode::Cache;
    }
    // Its devices start again, unless idle() is resetting them (a few
    // milliseconds): then the cache plays on, good or not, until it is done.
    // (In the background, a strip without shadows never runs.)
    bool held = false;
    if (mode != Mode::Cache && (background ? cache.unavailable : s.idleInstance && !claimDevices(*point))) {
        mode = Mode::Cache;
        held = true;
    }

    // Seams: where what goes out changes from the cache to the devices, or back.
    const bool wasCacheOut = s.mode != Mode::Live;
    const bool cacheOut = mode != Mode::Live;
    // Planned (its devices warm): the good blocks where they are good, the
    // devices after, each exact where they agree. Unplanned: out of what the
    // cache last played, good or not, into devices still cold.
    if (c.jumpAt == 0) {
        s.fade = Fade::None;  // (the playhead jumped: nothing to be continuous with)
    } else if (cacheOut != wasCacheOut) {
        s.fade = cacheOut ? Fade::ToCache : Fade::ToLive;
        s.fadeLength = cacheOut || valid || warm ? c.plannedFade : c.unplannedFade;
        s.fadeDone = 0;
    }
    s.mode = mode;
    step.cacheOut = cacheOut;
    step.renderLive = mode != Mode::Cache || s.fade == Fade::ToCache;
    step.readCache = cacheOut || s.fade == Fade::ToLive;
    step.anyBlocks = held || (!valid && !warm);

    // What its devices put out matches the arrangement from frame `cleanIn` on
    // (after a warm-up, longer by what feeds it, and by the notes sounding where
    // its playing started or the loop wrapped); from there it is kept.
    int64_t cleanAtStart = 0;
    if (step.renderLive) {
        // After standing idle they start again from silence, holding the notes held here.
        step.resume = s.idleInstance;
        if (fresh) {
            const int at = std::max(0, c.jumpAt);
            const int64_t held = track ? heldAcross(*track, c.jumpAt >= 0 ? c.jumpTo : c.startPosition) : 0;
            s.warmIn = at + warmFrames;
            s.cleanIn = at + held + warmFrames;
        }
        if (track && c.wrapAt >= 0) {
            if (const int64_t held = heldAcross(*track, lastWrap_.wrapTo); held > 0) {
                s.cleanIn = std::max(s.cleanIn, c.wrapAt + held + warmFrames);
            }
        }
        if (sourceClean > 0) s.cleanIn = std::max(s.cleanIn, sourceClean + warmFrames);
        cleanAtStart = s.cleanIn;
        s.cleanIn = std::max<int64_t>(0, s.cleanIn - frames);
        s.warmIn = std::max<int64_t>(0, s.warmIn - frames);
        s.idleInstance = false;
        if (!background) point->idleSinceNs.store(0, std::memory_order_relaxed);
        step.capture = cache.cacheable && c.usable && !pending && !carriesLive && cleanAtStart < frames;
        step.captureFrom = static_cast<int>(cleanAtStart);
    } else {
        if (!s.idleInstance && !background) {
            point->idleSinceNs.store(c.nowNs, std::memory_order_relaxed);
            point->idleEpoch.fetch_add(1, std::memory_order_relaxed);
            point->devicesOwner.store(CachePoint::kDevicesIdle, std::memory_order_release);
        }
        s.idleInstance = true;
    }
    // Convergence (the background's). What it plays where its blocks are good
    // only as far as its lane found out is kept anew (for the live renderer
    // too); what its devices put out alone is compared with what it had.
    if (background) {
        step.recapture = amend && cacheOut && s.fade == Fade::None && !held && allowed &&
                         !cacheCovers(blocks, dirty, version);
        if (step.recapture) {
            step.capture = false;
            step.captureFrom = 0;
        }
        step.compare = dirty && allowed && mode == Mode::Live && s.fade == Fade::None;
        if (!step.compare) s.observeFrom = s.matchFrom = -1;
    }
    // What the strips it feeds hear of it.
    s.pendingOut = pending;
    s.liveInputOut = carriesLive;
    s.cleanInOut = cacheOut && s.fade == Fade::None ? 0 : cleanAtStart;
    s.seenVersion = version;
    s.seenChanges = changes;
    s.seenResets = resets;
    s.seenGeneration = c.generation;
    return step;
}

void Renderer::checkConvergence(const RenderSnapshot& snap, const StripCacheRender& cache, CachePoint::LiveState& s,
                                const CacheStep& step, const float* left, const float* right) noexcept {
    constexpr int64_t kMatchFrames = 8192;  // a match at least this long, and twice its tail (and the latencies on the way)
    constexpr float kQuiet = 1e-4f;         // with something louder than this (-80 dBFS) in it
    const CacheChunk& c = cacheChunk_;
    const DirtyLog& dirty = *cache.dirty;
    // (What feeds it reaches it this much later at most: a change there may still be on its way.)
    const int64_t margin = snap.outputLatency();
    if (s.tailVersion != step.version) s.tail = -1;
    for (int p = 0; p < c.numPieces; ++p) {
        const CachePiece& piece = c.pieces[static_cast<size_t>(p)];
        int done = 0;
        while (done < piece.length) {
            const int64_t at = piece.position + done;
            const int64_t cell = floorDiv(at, kCacheBlockFrames);
            const auto offset = static_cast<int>(at - cell * kCacheBlockFrames);
            int n = static_cast<int>(std::min<int64_t>(piece.length - done, kCacheBlockFrames - offset));
            const CacheBlock* block = step.blocks && piece.context.linear() && at >= 0
                                          ? step.blocks->findLatest(cell, {}, offset, step.version)
                                          : nullptr;
            bool comparable = false, same = false;
            float peak = 0.f;
            int64_t until = 0;
            if (block) {
                n = std::min(n, block->to - offset);
                comparable = changesPassed(dirty, block->generation, at, at + n, until);
                for (size_t i = 0; comparable && i < cache.sources.size(); ++i) {
                    const StripCacheRender& source = snap.tracks[static_cast<size_t>(cache.sources[i])].cache;
                    comparable = !outChangedSince(snap, source, block->generation, at - margin, at + n, 0);
                }
                same = comparable;
                const float* outLeft = left + piece.offset + done;
                const float* outRight = right + piece.offset + done;
                for (int i = 0; same && i < n; ++i) {
                    const float l = block->silent ? 0.f : block->left()[offset + i];
                    const float r = block->silent ? 0.f : block->right()[offset + i];
                    same = outLeft[i] == l && outRight[i] == r;
                    peak = std::max({peak, std::fabs(l), std::fabs(r)});
                }
            }
            if (!comparable) {
                s.observeFrom = s.matchFrom = -1;
            } else {
                // Watched from where the change passed to where it can ring on
                // no longer: the last difference tells how long it rang on.
                if (s.observeFrom < 0 || s.observeGen != block->generation || s.observeTo != at) {
                    s.observeFrom = s.lastDifference = at;
                    s.observeGen = block->generation;
                    s.observeUntil = until;
                    s.matchFrom = -1;
                }
                s.observeTo = at + n;
                s.observeUntil = std::max(s.observeUntil, until);
                if (!same) {
                    s.lastDifference = at + n;
                    s.matchFrom = -1;
                } else if (s.matchFrom < 0) {
                    s.matchFrom = at;
                    s.matchFrames = n;
                    s.matchLoud = peak > kQuiet;
                } else {
                    s.matchFrames += n;
                    s.matchLoud = s.matchLoud || peak > kQuiet;
                }
                if (s.observeTo >= s.observeUntil) {
                    // (Seen through: what it learnt holds for this version of its devices.)
                    s.tail = std::max(s.tail, s.lastDifference - s.observeFrom);
                    s.tailVersion = step.version;
                    s.observeFrom = -1;
                }
            }
            done += n;
        }
    }
    if (s.tail >= 0 && s.matchFrom >= 0 && s.matchLoud &&
        s.matchFrames >= std::max(kMatchFrames, 2 * s.tail) + margin) {
        // The changes since have rung out where the match began: from there,
        // what its blocks hold is what its devices would put out.
        s.amend = {s.observeGen, c.generation, s.matchFrom};
        s.amended = true;
        s.amendVersion = step.version;
        s.runwayVersion = 0;  // (what is good changed: the runway is worked out again)
        s.observeFrom = s.matchFrom = -1;
    }
}

void Renderer::resumeDevices(const StripCacheRender& cache, const TrackRender* track,
                             TrackBuffers* buffers) noexcept {
    // What they held is from long ago (a reverb's tail, notes since released):
    // silence it, as a locate would.
    for (const auto& device : cache.devices) device->reset();
    if (!track || !buffers || !playing_ || numSegments_ == 0 || segments_[0].chase) return;
    // The notes underway here sound from here (the arrangement's: they started
    // while the devices stood idle). Before the chunk's events after this offset.
    const Segment& segment = segments_[0];
    const int offset = segment.offset;
    int at = 0;
    while (at < buffers->numEvents && buffers->events[static_cast<size_t>(at)].sampleOffset <= offset) ++at;
    for (const NoteRender& note : track->notes) {
        if (note.start >= segment.position) break;
        if (note.end <= segment.position) continue;
        if (buffers->numEvents >= static_cast<int>(buffers->events.size())) break;
        std::copy_backward(buffers->events.begin() + at, buffers->events.begin() + buffers->numEvents,
                           buffers->events.begin() + buffers->numEvents + 1);
        buffers->events[static_cast<size_t>(at++)] = ProcessEvent::noteOn(offset, note.key, note.velocity);
        ++buffers->numEvents;
    }
}

void Renderer::endCacheStep(const RenderSnapshot& snap, const StripCacheRender& cache, const CacheStep& step,
                            float* left, float* right, int frames, WorkerScratch& scratch) noexcept {
    if (!step.active) return;
    CachePoint::Lane& lane = cache.point->lanes[static_cast<size_t>(cacheLane_)];
    CachePoint::LiveState& s = lane.state;
    using Fade = CachePoint::LiveState::Fade;
    if (step.compare) checkConvergence(snap, cache, s, step, left, right);
    // What the devices put out is kept before anything mixes into it.
    if (step.capture) {
        captureChunk(lane, step, left, right);
    } else if (!step.recapture && s.building && s.building->to > s.building->from) {
        finishBlock(lane);  // the stretch it was capturing ended
    }
    if (step.readCache) {
        float* cacheLeft = scratch.cacheLeft.data();
        float* cacheRight = scratch.cacheRight.data();
        readCache(step.blocks, cache.dirty.get(), step.version, step.anyBlocks, left, right, cacheLeft, cacheRight,
                  step.amend);
        if (s.fade == Fade::None) {
            std::copy_n(cacheLeft, frames, left);
            std::copy_n(cacheRight, frames, right);
        } else {
            // Equal samples stay as they are (a seam between what agrees is exact).
            const bool toLive = s.fade == Fade::ToLive;
            const float length = static_cast<float>(s.fadeLength);
            for (int i = 0; i < frames; ++i) {
                const float g = std::min(1.f, static_cast<float>(s.fadeDone + i + 1) / length);
                const float live = toLive ? g : 1.f - g;
                if (left[i] != cacheLeft[i]) left[i] = left[i] * live + cacheLeft[i] * (1.f - live);
                if (right[i] != cacheRight[i]) right[i] = right[i] * live + cacheRight[i] * (1.f - live);
            }
            s.fadeDone += frames;
            if (s.fadeDone >= s.fadeLength) s.fade = Fade::None;
        }
    } else if (s.fade == Fade::ToLive) {
        s.fade = Fade::None;  // (nothing to fade out of)
    }
    if (step.recapture) {
        captureChunk(lane, step, left, right);
        lane.framesReplayed.fetch_add(static_cast<uint64_t>(frames), std::memory_order_relaxed);
    }
    const auto counted = static_cast<uint64_t>(frames);
    if (step.cacheOut && !step.renderLive) {
        lane.framesFromCache.fetch_add(counted, std::memory_order_relaxed);
    } else {
        lane.framesLive.fetch_add(counted, std::memory_order_relaxed);
    }
}

bool Renderer::readCache(const BlockSet* blocks, const DirtyLog* dirty, uint64_t version, bool anyBlocks,
                         const float* fallbackLeft, const float* fallbackRight, float* outLeft, float* outRight,
                         const DirtyAmendment* amend) const noexcept {
    const CacheChunk& c = cacheChunk_;
    bool complete = c.numPieces > 0;
    int covered = 0;  // frames of the chunk the pieces cover (from the start)
    for (int p = 0; p < c.numPieces; ++p) {
        const CachePiece& piece = c.pieces[static_cast<size_t>(p)];
        int done = 0;
        while (done < piece.length) {
            const int64_t at = piece.position + done;
            const int64_t cell = floorDiv(at, kCacheBlockFrames);
            const auto offset = static_cast<int>(at - cell * kCacheBlockFrames);
            const int out = piece.offset + done;
            int n = static_cast<int>(std::min<int64_t>(piece.length - done, kCacheBlockFrames - offset));
            const CacheBlock* block = nullptr;
            if (at < 0) {  // (silence before the song's start: coveredFrom())
                n = static_cast<int>(std::min<int64_t>(n, -at));
                std::fill_n(outLeft + out, n, 0.f);
                std::fill_n(outRight + out, n, 0.f);
                done += n;
                continue;
            }
            if (blocks && anyBlocks) {
                block = blocks->find(cell, piece.context, offset);
                if (block) n = std::min(n, block->to - offset);
            } else if (blocks) {
                int to = offset;
                block = blocks->findGood(cell, piece.context, offset,
                                         [&](const CacheBlock& b) { return goodTo(b, version, dirty, amend); }, to);
                if (block) n = std::min(n, to - offset);
            }
            if (!block) {
                // Missing: up to where a block of the cell starts again, the fallback.
                n = 1;
                while (done + n < piece.length && offset + n < kCacheBlockFrames && blocks &&
                       !blocks->find(cell, piece.context, offset + n)) {
                    ++n;
                }
                std::copy_n(fallbackLeft + out, n, outLeft + out);
                std::copy_n(fallbackRight + out, n, outRight + out);
                complete = false;
            } else if (block->silent) {
                std::fill_n(outLeft + out, n, 0.f);
                std::fill_n(outRight + out, n, 0.f);
            } else {
                std::copy_n(block->left() + offset, n, outLeft + out);
                std::copy_n(block->right() + offset, n, outRight + out);
            }
            done += n;
        }
        covered = std::max(covered, piece.offset + piece.length);
    }
    // (Frames no piece covers: none in a usable chunk; otherwise the fallback.)
    if (c.numPieces == 0) {
        std::copy_n(fallbackLeft, chunkFrames_, outLeft);
        std::copy_n(fallbackRight, chunkFrames_, outRight);
    } else if (covered < chunkFrames_) {
        std::copy_n(fallbackLeft + covered, chunkFrames_ - covered, outLeft + covered);
        std::copy_n(fallbackRight + covered, chunkFrames_ - covered, outRight + covered);
        complete = false;
    }
    return complete;
}

void Renderer::finishCaptures(const RenderSnapshot& snap) noexcept {
    const auto finish = [this](const StripCacheRender& cache) {
        if (!cache.point) return;
        CachePoint::Lane& lane = cache.point->lanes[static_cast<size_t>(cacheLane_)];
        if (lane.state.building && lane.state.building->to > lane.state.building->from) finishBlock(lane);
    };
    for (const TrackRender& track : snap.tracks) finish(track.cache);
    finish(snap.masterCache);
}

void Renderer::finishBlock(CachePoint::Lane& lane) noexcept {
    CacheBlock* block = lane.state.building;
    if (!block) return;
    if (block->to > block->from && lane.completed.push(block)) {
        lane.framesCaptured.fetch_add(static_cast<uint64_t>(block->to - block->from), std::memory_order_relaxed);
        lane.state.building = nullptr;
    } else {
        lane.framesLost.fetch_add(static_cast<uint64_t>(block->to - block->from), std::memory_order_relaxed);
        block->from = block->to = 0;  // (the store is behind: what it holds is lost)
    }
}

void Renderer::captureChunk(CachePoint::Lane& lane, const CacheStep& step, const float* left,
                            const float* right) noexcept {
    const CacheChunk& c = cacheChunk_;
    CachePoint::LiveState& s = lane.state;
    const uint64_t generation = c.generation;
    for (int p = 0; p < c.numPieces; ++p) {
        const CachePiece& piece = c.pieces[static_cast<size_t>(p)];
        const int skip = std::clamp(step.captureFrom - piece.offset, 0, piece.length);
        // (Nothing plays before the song's start: the background renderer only warms up there.)
        const auto before = static_cast<int>(std::clamp<int64_t>(-piece.position, 0, piece.length));
        int done = std::max(skip, before);
        while (done < piece.length) {
            const int64_t at = piece.position + done;
            const int64_t cell = floorDiv(at, kCacheBlockFrames);
            const auto offset = static_cast<int>(at - cell * kCacheBlockFrames);
            const int n = static_cast<int>(std::min<int64_t>(piece.length - done, kCacheBlockFrames - offset));
            CacheBlock* block = s.building;
            if (block && block->to > block->from &&
                (block->cell != cell || block->context != piece.context || block->to != offset ||
                 block->version != step.version || block->generation != generation)) {
                finishBlock(lane);
                block = s.building;
            }
            if (!block && !lane.spares.pop(block)) {  // none to capture into (the budget is spent)
                lane.framesLost.fetch_add(static_cast<uint64_t>(piece.length - done), std::memory_order_relaxed);
                return;
            }
            s.building = block;
            if (block->to <= block->from) {
                block->cell = cell;
                block->from = block->to = offset;
                block->version = step.version;
                block->generation = generation;
                block->context = piece.context;
                block->silent = false;
            }
            const int src = piece.offset + done;
            std::copy_n(left + src, n, block->samples.get() + offset);
            std::copy_n(right + src, n, block->samples.get() + kCacheBlockFrames + offset);
            block->to = offset + n;
            if (block->to == kCacheBlockFrames) finishBlock(lane);
            done += n;
        }
    }
}

}  // namespace sub
