// Engine: background freezing's edit side (docs/engine/background-freeze.md,
// cache/StripCache.h). What changes a strip's cache: its version, bumped when
// something time-global changes (what the strip is made of, a device's
// parameters or state, the mixer of what feeds it), and its dirty log, which
// says from where on something time-local changed (clips, notes, automation),
// worked out against the last snapshot. Both reach downstream: a group's cache
// changes with what goes into it.
#include "Engine.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace sub {
namespace {

constexpr int64_t kNever = std::numeric_limits<int64_t>::max();
constexpr size_t kMaxDirtyEntries = 64;  // per strip; blocks older than the forgotten ones are no good
constexpr int64_t kObservedNs = 1'500'000'000;  // a display read or an editor open keeps a strip live this long
constexpr int64_t kIdleBeforeResetNs = 2'000'000'000;
constexpr auto kResetBudget = std::chrono::milliseconds(10);

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

uint64_t mix(uint64_t h, uint64_t v) noexcept {
    v += 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    v ^= v >> 30;
    v *= 0xBF58476D1CE4E5B9ull;
    v ^= v >> 27;
    return h ^ v;
}
uint64_t mix(uint64_t h, double v) noexcept { return mix(h, std::bit_cast<uint64_t>(v)); }
uint64_t mix(uint64_t h, float v) noexcept { return mix(h, static_cast<uint64_t>(std::bit_cast<uint32_t>(v))); }
uint64_t mix(uint64_t h, const void* p) noexcept { return mix(h, static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p))); }
uint64_t mix(uint64_t h, int64_t v) noexcept { return mix(h, static_cast<uint64_t>(v)); }
uint64_t mix(uint64_t h, int v) noexcept { return mix(h, static_cast<uint64_t>(static_cast<int64_t>(v))); }
uint64_t mix(uint64_t h, bool v) noexcept { return mix(h, static_cast<uint64_t>(v ? 1 : 2)); }

uint64_t clipHash(const ClipRender& c) noexcept {
    uint64_t h = 0;
    h = mix(h, c.source.get());
    h = mix(h, c.start);
    h = mix(h, c.length);
    h = mix(h, c.sourceOffset);
    h = mix(h, c.rate);
    h = mix(h, c.gain);
    h = mix(h, c.panLeft);
    h = mix(h, c.panRight);
    h = mix(h, static_cast<int>(c.playback));
    h = mix(h, static_cast<int>(c.stretchConfig));
    h = mix(h, c.transpose);
    h = mix(h, c.preserveFormants);
    h = mix(h, c.key);
    h = mix(h, c.fadeIn);
    h = mix(h, c.fadeOut);
    h = mix(h, c.fadeInCurve);
    h = mix(h, c.fadeOutCurve);
    return h;
}

// A stretch of the timeline something changed over (empty: nothing).
struct Span {
    int64_t from = kNever;
    int64_t to = std::numeric_limits<int64_t>::min();
    bool empty() const noexcept { return from >= to; }
    void add(int64_t a, int64_t b) noexcept {
        if (a >= b) return;
        from = std::min(from, a);
        to = std::max(to, b);
    }
    void add(const Span& other) noexcept {
        if (!other.empty()) add(other.from, other.to);
    }
    // What it changes after devices that remember a warm-up's worth of what they heard.
    Span widened(int64_t by) const noexcept {
        Span out = *this;
        if (!out.empty() && out.to < kNever) out.to = out.to > kNever - by ? kNever : out.to + by;
        return out;
    }
};

// Where two lists of things on the timeline differ: the stretches of those in
// either that the other doesn't have. Each is (start, end, hash).
struct Item {
    int64_t start, end;
    uint64_t hash;
    friend bool operator<(const Item& a, const Item& b) noexcept {
        return a.start != b.start ? a.start < b.start : a.end != b.end ? a.end < b.end : a.hash < b.hash;
    }
    friend bool operator==(const Item& a, const Item& b) noexcept = default;
};

Span itemsDiffer(std::vector<Item> a, std::vector<Item> b) {
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    Span span;
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        if (i < a.size() && j < b.size() && a[i] == b[j]) {
            ++i;
            ++j;
        } else if (j >= b.size() || (i < a.size() && a[i] < b[j])) {
            span.add(a[i].start, a[i].end);
            ++i;
        } else {
            span.add(b[j].start, b[j].end);
            ++j;
        }
    }
    return span;
}

Span clipsDiffer(const std::vector<ClipRender>& a, const std::vector<ClipRender>& b) {
    std::vector<Item> x, y;
    x.reserve(a.size());
    y.reserve(b.size());
    for (const ClipRender& c : a) x.push_back({c.start, c.start + c.length, clipHash(c)});
    for (const ClipRender& c : b) y.push_back({c.start, c.start + c.length, clipHash(c)});
    return itemsDiffer(std::move(x), std::move(y));
}

Span notesDiffer(const std::vector<NoteRender>& a, const std::vector<NoteRender>& b) {
    const auto hash = [](const NoteRender& n) { return mix(mix(uint64_t{0}, int{n.key}), int{n.velocity}); };
    std::vector<Item> x, y;
    x.reserve(a.size());
    y.reserve(b.size());
    for (const NoteRender& n : a) x.push_back({n.start, n.end, hash(n)});
    for (const NoteRender& n : b) y.push_back({n.start, n.end, hash(n)});
    return itemsDiffer(std::move(x), std::move(y));
}

// Where an envelope changes: from the breakpoint before the first one that
// differs (the slope into it changes too) to the first of the breakpoints both
// end with alike, or, if their last ones differ, to where both have ended on the
// same value (for ever, if they end on different ones). An envelope coming or
// going changes everything (what it replaces isn't known here).
Span envelopeDiffers(const std::vector<AutomationNode>& a, const std::vector<AutomationNode>& b) {
    const auto same = [](const AutomationNode& x, const AutomationNode& y) {
        return x.time == y.time && x.value == y.value && x.curve == y.curve;
    };
    Span span;
    if (a.empty() != b.empty()) {
        span.add(0, kNever);
        return span;
    }
    const size_t common = std::min(a.size(), b.size());
    size_t i = 0;
    while (i < common && same(a[i], b[i])) ++i;
    if (i == a.size() && i == b.size()) return span;
    size_t k = 0;  // alike at the end (not counting those alike at the start)
    while (k < common - i && same(a[a.size() - 1 - k], b[b.size() - 1 - k])) ++k;
    const int64_t from = i == 0 ? 0 : a[i - 1].time;
    if (k > 0) {
        span.add(from, a[a.size() - k].time + 1);
    } else {
        const bool sameEnd = a.back().value == b.back().value;
        span.add(from, sameEnd ? std::max(a.back().time, b.back().time) + 1 : kNever);
    }
    return span;
}

// A strip's envelopes that change what its devices put out: its devices'
// parameters, and its racks' chains' (their devices', their faders').
using LaneKey = std::pair<const void*, int>;
void collectLanes(const StripRender& strip, std::vector<std::pair<LaneKey, const AutomationRender*>>& out) {
    for (const AutomationRender& lane : strip.automation) out.push_back({{lane.processor.get(), lane.param}, &lane});
    for (const auto& rack : strip.racks) {
        if (!rack) continue;
        for (const ChainRender& chain : rack->chains) {
            out.push_back({{chain.params.get(), -1}, &chain.volume});
            out.push_back({{chain.params.get(), -2}, &chain.pan});
            collectLanes(chain, out);
        }
    }
}

Span lanesDiffer(const StripRender& a, const StripRender& b) {
    std::vector<std::pair<LaneKey, const AutomationRender*>> x, y;
    collectLanes(a, x);
    collectLanes(b, y);
    const auto byKey = [](const auto& l, const auto& r) { return l.first < r.first; };
    std::sort(x.begin(), x.end(), byKey);
    std::sort(y.begin(), y.end(), byKey);
    static const AutomationRender kNone;
    Span span;
    size_t i = 0, j = 0;
    while (i < x.size() || j < y.size()) {
        if (j >= y.size() || (i < x.size() && x[i].first < y[j].first)) {
            span.add(envelopeDiffers(x[i++].second->nodes, kNone.nodes));
        } else if (i >= x.size() || y[j].first < x[i].first) {
            span.add(envelopeDiffers(kNone.nodes, y[j++].second->nodes));
        } else {
            span.add(envelopeDiffers(x[i++].second->nodes, y[j++].second->nodes));
        }
    }
    return span;
}

bool anyEnabled(const StripRender& strip) {
    for (size_t i = 0; i < strip.inserts.size(); ++i) {
        if (!strip.inserts[i]->isEnabled()) continue;
        const RackRender* rack = i < strip.racks.size() ? strip.racks[i].get() : nullptr;
        if (!rack) return true;
        for (const ChainRender& chain : rack->chains) {
            if (anyEnabled(chain)) return true;
        }
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// While a snapshot is built

void Engine::updateCacheLocked(RenderSnapshot& snap, const RenderSnapshot* previous) {
    const int64_t now = nowNs();
    const uint64_t generation = snap.generation;
    std::unordered_map<uint32_t, TrackModel*> models;
    for (TrackModel& track : tracks_) models[track.id] = &track;
    std::unordered_map<uint32_t, const TrackRender*> before;
    if (previous) {
        for (const TrackRender& track : previous->tracks) before[track.id] = &track;
    }
    // Each strip's devices, from the model (a frozen track's too: they still count changes).
    std::unordered_map<uint32_t, std::vector<std::shared_ptr<Processor>>> devicesOf;
    for (const auto& [id, entry] : processors_) {
        const auto chain = chains_.find(entry.chainId);
        if (chain != chains_.end()) devicesOf[chain->second.stripId].push_back(entry.processor);
    }
    std::unordered_map<const EdgeState*, const EdgeRender*> edgesBefore;
    if (previous) {
        for (const EdgeRender& edge : previous->edges) edgesBefore[edge.state.get()] = &edge;
    }

    const auto chainSignature = [&](auto&& self, uint64_t h, const StripRender& strip) -> uint64_t {
        for (size_t i = 0; i < strip.inserts.size(); ++i) {
            const Processor& insert = *strip.inserts[i];
            h = mix(h, static_cast<const void*>(&insert));
            h = mix(h, insert.isEnabled());
            h = mix(h, insertLatency(insert));
            const int sidechain = i < strip.sidechains.size() ? strip.sidechains[i] : -1;
            if (sidechain >= 0) {
                const EdgeRender& edge = snap.edges[static_cast<size_t>(sidechain)];
                h = mix(h, static_cast<int>(snap.tracks[static_cast<size_t>(edge.from)].id));
                h = mix(h, static_cast<int>(edge.tap));
                h = mix(h, edge.tapDevice);
                h = mix(h, edge.compensation);
                h = mix(h, edge.deviceDelay);
            }
            if (const RackRender* rack = i < strip.racks.size() ? strip.racks[i].get() : nullptr) {
                for (const ChainRender& chain : rack->chains) {
                    h = mix(h, static_cast<int>(chain.id));
                    h = mix(h, static_cast<const void*>(chain.params.get()));
                    h = mix(h, chain.compensation);
                    h = mix(h, chain.latency);
                    h = self(self, h, chain);
                }
            }
        }
        return mix(h, static_cast<int>(strip.deviceTaps.size()));
    };
    uint64_t global = 0;
    global = mix(global, snap.sampleRate);
    global = mix(global, snap.tempo);
    global = mix(global, snap.timeSigNum);
    global = mix(global, snap.timeSigDen);

    // In snapshot order: what feeds a strip comes before it. A change of what a
    // strip hears changes what its devices put out a warm-up longer (they
    // remember: the same assumption the capture makes), and so on downstream.
    const int64_t warm = std::max<int64_t>(0, std::llround(cacheSettings_.warmSeconds.load() * snap.sampleRate));
    const size_t count = snap.tracks.size();
    std::vector<uint64_t> signatures(count, 0);
    std::vector<Span> changedOut(count);  // what changed of its signal after its devices
    std::vector<Span> changedFader(count);  // its fader's envelopes: what it feeds hears a change
    const auto incoming = [&](const std::vector<int>& edges, uint64_t& h, Span& in) {
        for (const int e : edges) {
            const EdgeRender& edge = snap.edges[static_cast<size_t>(e)];
            const auto source = static_cast<size_t>(edge.from);
            h = mix(h, static_cast<int>(edge.kind));
            h = mix(h, static_cast<int>(edge.tap));
            h = mix(h, edge.tapDevice);
            h = mix(h, edge.compensation);
            h = mix(h, static_cast<int>(snap.tracks[source].id));
            h = mix(h, signatures[source]);
            in.add(changedOut[source]);
            in.add(changedFader[source]);
            const auto old = edgesBefore.find(edge.state.get());
            if (old != edgesBefore.end()) in.add(envelopeDiffers(old->second->level.nodes, edge.level.nodes));
        }
    };
    const auto sourcesOf = [&](const std::vector<int>& edges) {
        std::vector<int> sources;
        for (const int e : edges) {
            const int from = snap.edges[static_cast<size_t>(e)].from;
            if (std::find(sources.begin(), sources.end(), from) == sources.end()) sources.push_back(from);
        }
        return sources;
    };
    // Into its dirty log, at this generation, and forgetting the oldest.
    const auto settle = [&](TrackModel& model, StripCacheRender& cache, uint64_t signature, const Span& changed) {
        TrackModel::Cache& c = model.cache;
        CachePoint& point = *c.point;
        bool touched = false;
        if (const auto found = devicesOf.find(model.id); found != devicesOf.end()) cache.devices = found->second;
        uint64_t changes = 0;
        for (const auto& device : cache.devices) changes += device->changeCount();
        const bool accounted = changes == point.accountedChanges.load(std::memory_order_relaxed);
        if (signature != c.signature || !accounted) {
            c.signature = signature;
            point.version.fetch_add(1, std::memory_order_release);
            point.accountedChanges.store(changes, std::memory_order_release);  // (after the version)
            touched = true;
        }
        if (!changed.empty()) {
            c.dirty.push_back({generation, changed.from, changed.to});
            while (c.dirty.size() > kMaxDirtyEntries) {
                c.horizon = c.dirty.front().generation + 1;
                c.dirty.pop_front();
            }
            touched = true;
            auto log = std::make_shared<DirtyLog>();
            log->horizon = c.horizon;
            log->entries.assign(c.dirty.begin(), c.dirty.end());
            c.log = std::move(log);
        }
        if (touched) point.lastChangeNs.store(now, std::memory_order_relaxed);
        cache.point = c.point;
        cache.dirty = c.log;
    };

    std::vector<CacheStore::Entry> entries;
    entries.reserve(count + 1);
    for (size_t t = 0; t < count; ++t) {
        TrackRender& render = snap.tracks[t];
        TrackModel& model = *models.at(render.id);
        uint64_t h = mix(global, static_cast<int>(render.id));
        h = mix(h, render.latency);
        h = mix(h, render.inputLatency);
        h = mix(h, render.frozen);
        h = chainSignature(chainSignature, h, render);
        Span in;
        incoming(render.incoming, h, in);
        signatures[t] = h;
        if (const auto old = before.find(render.id); old != before.end()) {
            const TrackRender& was = *old->second;
            in.add(clipsDiffer(was.clips, render.clips));
            in.add(notesDiffer(was.notes, render.notes));
            in.add(lanesDiffer(was, render));
            changedFader[t].add(envelopeDiffers(was.volume.nodes, render.volume.nodes));
            changedFader[t].add(envelopeDiffers(was.pan.nodes, render.pan.nodes));
        }
        changedOut[t] = in.widened(warm);
        StripCacheRender& cache = render.cache;
        cache.sources = sourcesOf(render.incoming);
        cache.cacheable = !render.frozen && render.deviceTaps.empty() && anyEnabled(render);
        settle(model, cache, h, changedOut[t]);
        entries.push_back({cache.point, cache.dirty});
    }

    // The master: what goes into it (summed, and into its devices' sidechains).
    {
        std::vector<int> into = snap.masterInputs;
        for (size_t e = 0; e < snap.edges.size(); ++e) {
            if (snap.edges[e].to < 0 && snap.edges[e].kind == EdgeRender::Kind::Sidechain) {
                into.push_back(static_cast<int>(e));
            }
        }
        uint64_t h = mix(global, static_cast<int>(kMaster));
        h = mix(h, snap.master.latency);
        h = mix(h, snap.maxLatency);
        h = chainSignature(chainSignature, h, snap.master);
        Span in;
        incoming(into, h, in);
        if (previous) in.add(lanesDiffer(previous->master, snap.master));
        StripCacheRender& cache = snap.masterCache;
        cache.sources = sourcesOf(into);
        cache.cacheable = snap.master.deviceTaps.empty() && anyEnabled(snap.master);
        settle(master_, cache, h, in.widened(warm));
        entries.push_back({cache.point, cache.dirty});
    }
    if (cacheStore_) cacheStore_->setPoints(std::move(entries));
}

// ---------------------------------------------------------------------------
// Changes no snapshot shows

void Engine::touchCacheLocked(TrackModel& strip) {
    CachePoint& point = *strip.cache.point;
    point.version.fetch_add(1, std::memory_order_release);
    point.lastChangeNs.store(nowNs(), std::memory_order_relaxed);
}

void Engine::touchCacheLocked(uint32_t stripId, bool itself) {
    // Downstream through the published snapshot, which shows the routing as it is.
    const RenderSnapshot* snap = snapshotHold_.get();
    if (!snap) return;
    if (stripId == kMaster) {
        if (itself) touchCacheLocked(master_);
        return;
    }
    std::vector<char> seen(snap->tracks.size(), 0);
    std::vector<size_t> todo;
    bool master = false;
    for (size_t t = 0; t < snap->tracks.size(); ++t) {
        if (snap->tracks[t].id == stripId) todo.push_back(t);
    }
    if (todo.empty()) return;
    if (itself) seen[todo.front()] = 1;
    while (!todo.empty()) {
        const size_t t = todo.back();
        todo.pop_back();
        for (const int e : snap->tracks[t].outgoing) {
            const int to = snap->edges[static_cast<size_t>(e)].to;
            if (to < 0) {
                master = true;
            } else if (!seen[static_cast<size_t>(to)]) {
                seen[static_cast<size_t>(to)] = 1;
                todo.push_back(static_cast<size_t>(to));
            }
        }
    }
    // Once each, even reached by several paths.
    for (size_t t = 0; t < snap->tracks.size(); ++t) {
        if (!seen[t]) continue;
        for (TrackModel& track : tracks_) {
            if (track.id == snap->tracks[t].id) touchCacheLocked(track);
        }
    }
    if (master) touchCacheLocked(master_);
}

Engine::TrackModel* Engine::stripOfProcessorLocked(uint32_t processorId) {
    const auto entry = processors_.find(processorId);
    if (entry == processors_.end()) return nullptr;
    const auto chain = chains_.find(entry->second.chainId);
    if (chain == chains_.end()) return nullptr;
    if (chain->second.stripId == kMaster) return &master_;
    for (TrackModel& track : tracks_) {
        if (track.id == chain->second.stripId) return &track;
    }
    return nullptr;
}

void Engine::processorChangedLocked(uint32_t processorId) {
    TrackModel* strip = stripOfProcessorLocked(processorId);
    if (!strip) return;
    touchCacheLocked(strip->id, true);
    // The changes its devices count now are accounted for (after the version:
    // the audio thread reads them the other way round).
    uint64_t changes = 0;
    for (const auto& [id, entry] : processors_) {
        const auto chain = chains_.find(entry.chainId);
        if (chain != chains_.end() && chain->second.stripId == strip->id) changes += entry.processor->changeCount();
    }
    strip->cache.point->accountedChanges.store(changes, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// idle()

void Engine::cacheIdle(bool rendering) {
    const int64_t now = nowNs();
    struct Reset {
        uint32_t stripId;
        uint64_t epoch;
        std::vector<std::shared_ptr<Processor>> devices;
    };
    std::vector<Reset> resets;
    std::vector<std::pair<std::shared_ptr<Processor>, std::shared_ptr<CachePoint>>> editors;
    {
        std::lock_guard lock(mutex_);
        // Each strip's devices, and the changes they count.
        std::unordered_map<uint32_t, std::vector<std::shared_ptr<Processor>>> devicesOf;
        for (const auto& [id, entry] : processors_) {
            const auto chain = chains_.find(entry.chainId);
            if (chain != chains_.end()) devicesOf[chain->second.stripId].push_back(entry.processor);
        }
        static const std::vector<std::shared_ptr<Processor>> kNone;
        const auto visit = [&](TrackModel& strip) {
            const auto found = devicesOf.find(strip.id);
            const auto& devices = found != devicesOf.end() ? found->second : kNone;
            uint64_t changes = 0;
            for (const auto& device : devices) changes += device->changeCount();
            CachePoint& point = *strip.cache.point;
            if (changes != point.accountedChanges.load(std::memory_order_relaxed)) {
                // A device changed itself (its editor, the plug-in): its strip and what that feeds.
                touchCacheLocked(strip.id, true);
                point.accountedChanges.store(changes, std::memory_order_release);
            }
            for (const auto& device : devices) {
                if (device->hasEditor()) editors.emplace_back(device, strip.cache.point);
            }
            // Its devices haven't run for a while (it plays from its cache): they
            // start again from silence, so that a sudden change of plan doesn't
            // play what they held from long ago.
            const int64_t idleSince = point.idleSinceNs.load(std::memory_order_relaxed);
            const uint64_t epoch = point.idleEpoch.load(std::memory_order_relaxed);
            if (!rendering && !devices.empty() && idleSince != 0 && now - idleSince > kIdleBeforeResetNs &&
                epoch != strip.cache.resetEpoch) {
                resets.push_back({strip.id, epoch, devices});
            }
        };
        for (TrackModel& track : tracks_) visit(track);
        visit(master_);
    }
    // Plug-in calls, without the lock.
    for (const auto& [device, point] : editors) {
        if (device->isEditorOpen()) point->observedUntilNs.store(now + kObservedNs, std::memory_order_relaxed);
    }
    const auto started = std::chrono::steady_clock::now();
    for (Reset& reset : resets) {
        if (std::chrono::steady_clock::now() - started > kResetBudget) break;
        for (const auto& device : reset.devices) {
            device->resetOffline();
            device->requestReset();
        }
        std::lock_guard lock(mutex_);
        if (reset.stripId == kMaster) {
            master_.cache.resetEpoch = reset.epoch;
        } else if (const int index = trackIndexLocked(reset.stripId); index >= 0) {  // (it may have gone meanwhile)
            tracks_[static_cast<size_t>(index)].cache.resetEpoch = reset.epoch;
        }
    }
}

// ---------------------------------------------------------------------------
// The API

void Engine::setBackgroundFreezing(const BackgroundFreezingSettings& settings) {
    std::lock_guard lock(mutex_);
    cacheSettings_.idleSeconds.store(std::max(0.0, settings.idleSeconds));
    const double warm = std::max(0.0, settings.warmSeconds);
    if (warm != cacheSettings_.warmSeconds.exchange(warm)) {
        // What was kept, and the dirty logs, assumed the old warm-up.
        for (TrackModel& track : tracks_) touchCacheLocked(track);
        touchCacheLocked(master_);
    }
    cacheSettings_.budgetBytes.store(static_cast<int64_t>(std::max(0.0, settings.budgetMB) * 1024.0 * 1024.0));
    cacheSettings_.enabled.store(settings.enabled);
}

BackgroundFreezingSettings Engine::backgroundFreezing() {
    BackgroundFreezingSettings settings;
    settings.enabled = cacheSettings_.enabled.load();
    settings.idleSeconds = cacheSettings_.idleSeconds.load();
    settings.warmSeconds = cacheSettings_.warmSeconds.load();
    settings.budgetMB = static_cast<double>(cacheSettings_.budgetBytes.load()) / (1024.0 * 1024.0);
    return settings;
}

namespace {
void addCounts(BackgroundFreezingStats& stats, const CachePoint& point) {
    stats.framesFromCache += point.framesFromCache.load(std::memory_order_relaxed);
    stats.framesLive += point.framesLive.load(std::memory_order_relaxed);
    stats.framesCaptured += point.framesCaptured.load(std::memory_order_relaxed);
}
}  // namespace

BackgroundFreezingStats Engine::backgroundFreezingStats() {
    BackgroundFreezingStats stats;
    if (cacheStore_) {
        const CacheStore::Stats store = cacheStore_->stats();
        stats.blocks = store.blocks;
        stats.silentBlocks = store.silentBlocks;
        stats.bytes = store.bytes;
    }
    std::lock_guard lock(mutex_);
    for (const TrackModel& track : tracks_) addCounts(stats, *track.cache.point);
    addCounts(stats, *master_.cache.point);
    return stats;
}

BackgroundFreezingStats Engine::backgroundFreezingStats(uint32_t trackId) {
    std::lock_guard lock(mutex_);
    BackgroundFreezingStats stats;
    addCounts(stats, *trackLocked(trackId).cache.point);
    return stats;
}

void Engine::setTrackObserved(uint32_t trackId, bool observed) {
    std::lock_guard lock(mutex_);
    trackLocked(trackId).cache.point->observed.store(observed, std::memory_order_relaxed);
}

void Engine::serviceBackgroundFreezing() {
    if (cacheStore_) cacheStore_->service();
}

}  // namespace sub
