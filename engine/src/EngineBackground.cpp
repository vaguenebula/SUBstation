// Engine: background freezing's background rendering (phase 2,
// cache/BackgroundRenderer.h, docs/engine/background-freeze.md). On the main
// thread, in idle(): a shadow instance for every device switched on (built-in
// devices and racks at once; a plug-in one at a time, once nothing has been
// edited for a while), brought to the state of its device when the strip's
// version moves on (once edits have settled); and the background renderer's
// snapshot rebuilt from the live one when either changes.
#include "Engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <thread>
#include <unordered_set>

#include "Rack.h"

namespace sub {
namespace {

constexpr int64_t kSettleNs = 1'000'000'000;  // nothing edited for this long: shadows catch up

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool isPlugin(const Processor& processor) { return processor.typeId().rfind("vst3:", 0) == 0; }

// What stands in the shadow snapshot for a device without a shadow: switched
// off, so the renderer never runs it (its strip is marked unavailable if the
// device it stands for is on).
class Absent final : public Processor {
public:
    Absent() { setEnabled(false); }
    std::string typeId() const override { return "absent"; }
    std::string name() const override { return "Absent"; }
    void prepare(double, int) override {}
    void process(const ProcessContext&, float* const*, int, int) override {}
    const std::vector<ParamInfo>& params() const override {
        static const std::vector<ParamInfo> none;
        return none;
    }
    float getParam(int) const override { return 0.f; }
    void setParam(int, float) override {}
};

std::shared_ptr<TrackParams> copyParams(const std::shared_ptr<TrackParams>& from) {
    if (!from) return nullptr;
    auto params = std::make_shared<TrackParams>();
    params->gain.store(from->gain.load());
    params->pan.store(from->pan.load());
    params->mute.store(from->mute.load());
    params->solo.store(from->solo.load());
    return params;
}

std::shared_ptr<DelayLine> freshLine(const std::shared_ptr<DelayLine>& like) {
    return like ? std::make_shared<DelayLine>(like->capacity()) : nullptr;
}

// Every device of a strip, its racks' too, depth first.
void devicesOf(const StripRender& strip, std::vector<std::shared_ptr<Processor>>& out) {
    for (size_t i = 0; i < strip.inserts.size(); ++i) {
        out.push_back(strip.inserts[i]);
        if (const RackRender* rack = i < strip.racks.size() ? strip.racks[i].get() : nullptr) {
            for (const ChainRender& chain : rack->chains) devicesOf(chain, out);
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// The shadow snapshot

std::shared_ptr<const RenderSnapshot> Engine::buildShadowSnapshotLocked(const RenderSnapshot& live, int64_t& songEnd) {
    static const auto absent = std::make_shared<Absent>();
    auto snap = std::make_shared<RenderSnapshot>();
    snap->sampleRate = live.sampleRate;
    snap->tempo = live.tempo;
    snap->timeSigNum = live.timeSigNum;
    snap->timeSigDen = live.timeSigDen;
    snap->loopEnabled = live.loopEnabled;
    snap->loopStart = live.loopStart;
    snap->loopEnd = live.loopEnd;
    snap->maxLatency = live.maxLatency;
    snap->masterInputs = live.masterInputs;
    snap->generation = live.generation;
    snap->chainDelays = live.chainDelays;
    // (One thread renders it all, in routing order: no scheduler.)
    for (int c = 0; c < kNumStretchConfigs; ++c) {
        for (size_t i = 0; i < live.warpVoices[c].size(); ++i) {
            snap->warpVoices[c].push_back(std::make_shared<WarpVoice>(static_cast<StretchConfig>(c), sampleRate_));
        }
    }

    // A device's shadow, if it has one; else what stands for it, and its strip
    // can't be rendered if it is on. (Not whether it reports the same latency:
    // a plug-in may report its own only once it has processed, and the live
    // one may never have. What it renders is for the strip's version, which a
    // latency the live one reports later moves on.)
    const auto device = [&](const std::shared_ptr<Processor>& processor, bool& unavailable) -> std::shared_ptr<Processor> {
        if (!processor) return processor;
        const auto found = shadows_.find(processor.get());
        if (found != shadows_.end() && found->second.shadow) return found->second.shadow;
        if (processor->isEnabled()) unavailable = true;
        return absent;
    };
    const std::function<void(const StripRender&, StripRender&, bool&)> strip = [&](const StripRender& from,
                                                                                StripRender& to, bool& unavailable) {
        to.params = copyParams(from.params);
        to.inserts.clear();
        for (const auto& insert : from.inserts) to.inserts.push_back(device(insert, unavailable));
        to.latency = from.latency;
        to.automation = from.automation;
        for (AutomationRender& lane : to.automation) {
            bool ignored = false;
            lane.processor = device(lane.processor, ignored);
        }
        to.volume = from.volume;
        to.pan = from.pan;
        to.sidechains = from.sidechains;
        to.deviceTaps = from.deviceTaps;
        to.racks.clear();
        for (const auto& rack : from.racks) {
            if (!rack) {
                to.racks.push_back(nullptr);
                continue;
            }
            auto copy = std::make_shared<RackRender>();
            copy->depth = rack->depth;
            for (const ChainRender& chain : rack->chains) {
                ChainRender c;
                strip(chain, c, unavailable);
                c.id = chain.id;
                c.compensation = chain.compensation;
                c.delayIndex = chain.delayIndex;
                c.delay = freshLine(chain.delay);
                copy->chains.push_back(std::move(c));
            }
            to.racks.push_back(std::move(copy));
        }
    };
    const auto cache = [&](const StripCacheRender& from, uint32_t stripId, bool unavailable) {
        StripCacheRender to = from;
        bool ignored = false;
        for (auto& d : to.devices) d = device(d, ignored);
        const auto synced = shadowVersions_.find(stripId);
        to.version = synced != shadowVersions_.end() ? synced->second : 0;
        to.unavailable = unavailable || to.version == 0;
        return to;
    };

    songEnd = 0;
    snap->tracks.reserve(live.tracks.size());
    for (const TrackRender& from : live.tracks) {
        TrackRender to = from;
        bool unavailable = false;
        strip(from, to, unavailable);
        to.buffers = std::make_shared<TrackBuffers>(Renderer::kMaxBlock);
        // It plays the arrangement: no input, no monitoring, no recording.
        to.input = {};
        to.midiInput = {};
        to.monitor = MonitorMode::Off;
        to.armed = false;
        to.cache = cache(from.cache, from.id, unavailable);
        for (const ClipRender& clip : from.clips) songEnd = std::max(songEnd, clip.start + clip.length);
        for (const NoteRender& note : from.notes) songEnd = std::max(songEnd, note.end);
        snap->tracks.push_back(std::move(to));
    }
    {
        bool unavailable = false;
        strip(live.master, snap->master, unavailable);
        snap->masterCache = cache(live.masterCache, kMaster, unavailable);
    }
    snap->edges.reserve(live.edges.size());
    for (const EdgeRender& from : live.edges) {
        EdgeRender to = from;
        to.delay = freshLine(from.delay);
        to.deviceDelayLine = freshLine(from.deviceDelayLine);
        to.state = std::make_shared<EdgeState>(Renderer::kMaxBlock);
        if (from.state) to.state->gain.store(from.state->gain.load());
        snap->edges.push_back(std::move(to));
    }
    // What rings on after the last clip or note: a warm-up's worth, and the latencies.
    if (songEnd > 0) {
        songEnd += std::llround(cacheSettings_.warmSeconds.load() * live.sampleRate) + live.outputLatency();
    }
    return snap;
}

// ---------------------------------------------------------------------------
// idle()

void Engine::backgroundIdle(bool now) {
    bool rendering = false;
    std::shared_ptr<const RenderSnapshot> live;
    {
        std::lock_guard lock(mutex_);
        rendering = job_ != nullptr;
        live = snapshotHold_;
    }
    const bool on = cacheSettings_.enabled.load() && backgroundRender_;
    cacheSettings_.background.store(on && !rendering);  // (a render job wants the CPU)
    if (!on || !live || live->sampleRate != backgroundRate_ || backgroundThread_ != backgroundHasThread_) {
        // Off (or for another rate, or to start again with or without a thread):
        // the renderer goes, then the shadows it ran.
        background_.reset();
        shadows_.clear();
        shadowVersions_.clear();
        shadowGeneration_ = 0;
        if (!on || !live) return;
    }
    if (!background_) {
        background_ = std::make_unique<BackgroundRenderer>(
            BackgroundRenderer::Watch{cacheSettings_, shared_.positionSamples, shared_.playing, shared_.cpuLoad,
                                      deviceRunningFlag_, backgroundBusy_, backgroundEpoch_,
                                      [store = cacheStore_.get()] { store->service(); }},
            live->sampleRate, backgroundThread_);
        backgroundRate_ = live->sampleRate;
        backgroundHasThread_ = backgroundThread_;
    }

    // The devices of the live snapshot's strips (those switched on want a shadow),
    // and when anything was last edited.
    struct Strip {
        uint32_t id = 0;
        std::shared_ptr<CachePoint> point;
        std::vector<std::shared_ptr<Processor>> devices;
    };
    std::vector<Strip> strips;
    int64_t lastChange = 0;
    for (const TrackRender& track : live->tracks) {
        Strip s{track.id, track.cache.point, {}};
        devicesOf(track, s.devices);
        strips.push_back(std::move(s));
    }
    {
        Strip s{kMaster, live->masterCache.point, {}};
        devicesOf(live->master, s.devices);
        strips.push_back(std::move(s));
    }
    for (const Strip& s : strips) {
        if (s.point) lastChange = std::max(lastChange, s.point->lastChangeNs.load(std::memory_order_relaxed));
    }
    const bool settled = now || nowNs() - lastChange > kSettleNs;

    // Shadows: for devices that have none (a plug-in once things have settled,
    // one per call: loading one can take a while), none for those gone.
    bool changed = false;
    bool pluginLoaded = false;
    std::unordered_set<const Processor*> wanted;
    for (const Strip& s : strips) {
        for (const auto& processor : s.devices) {
            if (!processor || !processor->isEnabled()) continue;
            wanted.insert(processor.get());
            if (shadows_.contains(processor.get())) continue;
            if (isPlugin(*processor) && !now && (!settled || pluginLoaded)) continue;
            Shadow shadow{processor, nullptr};
            try {
                shadow.shadow = processor->createShadow(live->sampleRate, Renderer::kMaxBlock);
            } catch (const std::exception&) {
                shadow.shadow = nullptr;  // (its strip renders only live)
            }
            pluginLoaded = pluginLoaded || isPlugin(*processor);
            shadows_.emplace(processor.get(), std::move(shadow));
            changed = true;
        }
    }
    std::vector<Shadow> retired;  // destroyed once the renderer has let go of them
    for (auto it = shadows_.begin(); it != shadows_.end();) {
        if (wanted.contains(it->first)) {
            ++it;
        } else {
            retired.push_back(std::move(it->second));
            it = shadows_.erase(it);
            changed = true;
        }
    }

    // Strips whose shadows are behind (once edits have settled): synced at
    // another version than the strip's now.
    std::vector<const Strip*> behind;
    if (settled) {
        for (const Strip& s : strips) {
            if (!s.point) continue;
            const auto synced = shadowVersions_.find(s.id);
            if (synced == shadowVersions_.end() || synced->second != s.point->version.load(std::memory_order_acquire)) {
                behind.push_back(&s);
            }
        }
    }
    if (live->generation == shadowGeneration_ && !changed && behind.empty()) return;
    if (!settled && !changed) return;  // (it renders on with what it has meanwhile)

    if (now) {
        while (!background_->park()) std::this_thread::yield();
    } else if (!background_->park()) {
        return;  // it finishes a chunk: next time
    }
    // Shadows to their devices' state: the version first (what they render is
    // at least that new; a later change moves the version on again).
    for (const Strip* s : behind) {
        const uint64_t version = s->point->version.load(std::memory_order_acquire);
        for (const auto& processor : s->devices) {
            const auto found = processor ? shadows_.find(processor.get()) : shadows_.end();
            if (found == shadows_.end() || !found->second.shadow) continue;
            Processor& shadow = *found->second.shadow;
            processor->syncShadow(shadow);
            shadow.setEnabled(processor->isEnabled());
            shadow.idle();  // (a plug-in's restarts: its latency)
            std::vector<ProcessorEvent> ignored;
            shadow.takeEvents(ignored);
        }
        shadowVersions_[s->id] = version;
    }
    int64_t songEnd = 0;
    std::shared_ptr<const RenderSnapshot> shadowSnapshot;
    {
        std::lock_guard lock(mutex_);
        shadowSnapshot = buildShadowSnapshotLocked(*live, songEnd);
    }
    background_->adopt(std::move(shadowSnapshot), songEnd);  // (the last one goes, with what only it held)
    shadowGeneration_ = live->generation;
    background_->unpark();
    retired.clear();
}

// ---------------------------------------------------------------------------
// The API

int64_t Engine::renderInBackground(int64_t frames) {
    backgroundIdle(true);
    return background_ ? background_->renderNow(frames) : 0;
}

}  // namespace sub
