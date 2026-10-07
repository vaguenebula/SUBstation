// Engine: tracks, their mixer, routing (outputs, sends, cycle checks) and meters.
#include "Engine.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_map>

#include "Routing.h"

namespace sub {

// ---------------------------------------------------------------------------
// Tracks

Engine::TrackModel& Engine::trackLocked(uint32_t trackId) {
    if (trackId == kMaster) return master_;
    return arrangementTrackLocked(trackId);
}

Engine::TrackModel& Engine::arrangementTrackLocked(uint32_t trackId) {
    if (trackId == kMaster) throw std::invalid_argument("The master has no clips or notes, and stays");
    for (auto& track : tracks_) {
        if (track.id == trackId) return track;
    }
    throw std::invalid_argument("Unknown track id " + std::to_string(trackId));
}

uint32_t Engine::addTrack() {
    std::lock_guard lock(mutex_);
    TrackModel track;
    track.id = nextTrackId_++;
    track.params = std::make_shared<TrackParams>();
    track.chainId = addChainLocked(track.id, 0);
    track.buffers = std::make_shared<TrackBuffers>(Renderer::kMaxBlock);
    track.outputState = std::make_shared<EdgeState>(Renderer::kMaxBlock);
    tracks_.push_back(std::move(track));
    rebuildSnapshotLocked();
    return tracks_.back().id;
}

void Engine::removeTrack(uint32_t trackId) {
    std::lock_guard lock(mutex_);
    arrangementTrackLocked(trackId);
    // What went into it goes to the master; the sends into it go, and so do the
    // inputs and sidechains from it.
    for (TrackModel& track : tracks_) {
        if (track.output == trackId) track.output = kMaster;
        std::erase_if(track.sends, [trackId](const SendModel& send) { return send.to == trackId; });
        if (track.inputTrack == trackId) track.inputTrack.reset();
    }
    for (auto& [id, entry] : processors_) {
        if (entry.sidechain && entry.sidechain->source == trackId) entry.sidechain.reset();
    }
    // Its chains go, with their devices.
    for (auto it = processors_.begin(); it != processors_.end();) {
        if (chainLocked(it->second.chainId).stripId == trackId) {
            retireProcessorLocked(std::move(it->second.processor));
            it = processors_.erase(it);
        } else {
            ++it;
        }
    }
    std::erase_if(chains_, [&](const auto& entry) { return entry.second.stripId == trackId; });
    std::erase_if(tracks_, [&](const TrackModel& track) { return track.id == trackId; });
    rebuildSnapshotLocked();
}

void Engine::setTrackClips(uint32_t trackId, const std::vector<ClipDesc>& clips) {
    std::lock_guard lock(mutex_);
    TrackModel& track = arrangementTrackLocked(trackId);
    track.clips = clips;
    track.clipKeys.clear();
    for (const auto& clip : clips) track.clipKeys.push_back(sourceKey(clip.path));
    rebuildSnapshotLocked();
}

void Engine::setTrackNotes(uint32_t trackId, const std::vector<NoteDesc>& notes) {
    std::lock_guard lock(mutex_);
    arrangementTrackLocked(trackId).notes = notes;
    rebuildSnapshotLocked();
}

void Engine::previewNote(uint32_t trackId, int key, int velocity) {
    std::lock_guard lock(mutex_);
    arrangementTrackLocked(trackId);
    shared_.previewNotes.push({trackId, static_cast<uint8_t>(std::clamp(key, 0, 127)),
                               static_cast<uint8_t>(std::clamp(velocity, 0, 127))});
    serviceTransportIfIdleLocked();
}

void Engine::setTrackGain(uint32_t trackId, float gain) {
    std::lock_guard lock(mutex_);
    trackLocked(trackId).params->gain.store(std::max(0.f, gain));
}

void Engine::setTrackPan(uint32_t trackId, float pan) {
    std::lock_guard lock(mutex_);
    trackLocked(trackId).params->pan.store(std::clamp(pan, -1.f, 1.f));
}

void Engine::setTrackMute(uint32_t trackId, bool mute) {
    std::lock_guard lock(mutex_);
    trackLocked(trackId).params->mute.store(mute);
}

void Engine::setTrackSolo(uint32_t trackId, bool solo) {
    std::lock_guard lock(mutex_);
    trackLocked(trackId).params->solo.store(solo);
}

void Engine::setTrackFrozen(uint32_t trackId, bool frozen) {
    std::lock_guard lock(mutex_);
    TrackModel& track = arrangementTrackLocked(trackId);
    if (track.frozen == frozen) return;
    track.frozen = frozen;
    rebuildSnapshotLocked();
}

bool Engine::trackFrozen(uint32_t trackId) {
    std::lock_guard lock(mutex_);
    return arrangementTrackLocked(trackId).frozen;
}

int Engine::trackIndexLocked(uint32_t trackId) const {
    for (size_t i = 0; i < tracks_.size(); ++i) {
        if (tracks_[i].id == trackId) return static_cast<int>(i);
    }
    return -1;
}

std::vector<RouteEdge> Engine::routeEdgesLocked(std::vector<EdgeOrigin>* origins) const {
    std::vector<RouteEdge> edges;
    if (origins) origins->clear();
    for (size_t t = 0; t < tracks_.size(); ++t) {
        const TrackModel& track = tracks_[t];
        const int from = static_cast<int>(t);
        edges.push_back({from, track.output == kMaster ? -1 : trackIndexLocked(track.output)});
        if (origins) origins->push_back({from, kOutputEdge});
        for (size_t s = 0; s < track.sends.size(); ++s) {
            edges.push_back({from, trackIndexLocked(track.sends[s].to)});
            if (origins) origins->push_back({from, static_cast<int>(s)});
        }
    }
    // Last, so that each track's own edges lead its outgoing ones. (The master
    // as a source is no edge: it renders after every track.)
    for (size_t t = 0; t < tracks_.size(); ++t) {
        const TrackModel& track = tracks_[t];
        if (!track.inputTrack || *track.inputTrack == kMaster) continue;
        edges.push_back({trackIndexLocked(*track.inputTrack), static_cast<int>(t), false});
        if (origins) origins->push_back({static_cast<int>(t), kInputEdge});
    }
    // Then the sidechains, by destination (the tracks, then the master's devices)
    // and device (its slot: devices in racks too).
    if (std::none_of(processors_.begin(), processors_.end(), [](const auto& p) { return p.second.sidechain.has_value(); })) {
        return edges;
    }
    const ProcessorIds ids = processorIdsLocked();
    std::unordered_map<int, std::vector<StripSlot>> sourceSlots;  // the sources' devices, as needed
    const auto addSidechains = [&](const TrackModel& strip, int to) {
        const std::vector<StripSlot> slots = stripSlotsLocked(strip, ids);
        for (size_t d = 0; d < slots.size(); ++d) {
            const auto found = processors_.find(slots[d].processorId);
            if (found == processors_.end() || !found->second.sidechain) continue;
            const SidechainModel& sidechain = *found->second.sidechain;
            RouteEdge edge{trackIndexLocked(sidechain.source), to, false};
            if (edge.from < 0) continue;  // (its source went: removeTrack() takes it away)
            auto source = sourceSlots.find(edge.from);
            if (source == sourceSlots.end()) {
                source = sourceSlots.emplace(edge.from, stripSlotsLocked(tracks_[static_cast<size_t>(edge.from)], ids)).first;
            }
            EdgeRender::Tap tap;
            const int slot = sidechainTapLocked(sidechain, source->second, tap);
            edge.tap = tap == EdgeRender::Tap::AfterDevice ? slot + 1 : -1;
            edge.device = slots[d].enabled ? static_cast<int>(d) : -1;  // one switched off isn't lined up
            edges.push_back(edge);
            if (origins) origins->push_back({to, kSidechainEdge, found->first, static_cast<int>(d)});
        }
    };
    for (size_t t = 0; t < tracks_.size(); ++t) addSidechains(tracks_[t], static_cast<int>(t));
    addSidechains(master_, -1);
    return edges;
}

int Engine::sidechainTapLocked(const SidechainModel& sidechain, const std::vector<StripSlot>& slots,
                               EdgeRender::Tap& tap) const {
    if (sidechain.tap == SidechainTap::PreFx) {  // before its first device
        tap = EdgeRender::Tap::AfterDevice;
        return -1;
    }
    tap = sidechain.tap == SidechainTap::PostFader ? EdgeRender::Tap::PostFader : EdgeRender::Tap::PreFader;
    if (sidechain.tap != SidechainTap::AfterDevice) return -1;
    const auto place = std::find_if(slots.begin(), slots.end(),
                                    [&](const StripSlot& slot) { return slot.processorId == sidechain.tapProcessor; });
    if (sidechain.tapProcessor == 0 || place == slots.end()) return -1;  // it left the source: before the fader
    int slot = static_cast<int>(place - slots.begin());
    // A rack switched off passes its input on, without running its chains: after the outermost such rack.
    for (int rack = slots[static_cast<size_t>(slot)].rack; rack >= 0; rack = slots[static_cast<size_t>(rack)].rack) {
        if (!slots[static_cast<size_t>(rack)].processor->isEnabled()) slot = rack;
    }
    tap = EdgeRender::Tap::AfterDevice;
    return slot;
}

void Engine::checkSidechainLocked(uint32_t source, uint32_t strip) const {
    if (strip == kMaster) return;  // everything goes into the master: nothing it feeds feeds a track
    if (wouldCycle(static_cast<int>(tracks_.size()), routeEdgesLocked(), trackIndexLocked(source),
                   trackIndexLocked(strip))) {
        throw std::invalid_argument("A device on track " + std::to_string(strip) +
                                    " can't take its sidechain from track " + std::to_string(source) + ": " +
                                    (source == strip ? "that is its own track" : "its track feeds that one"));
    }
}

void Engine::checkRouteLocked(uint32_t from, uint32_t to, const char* what) const {
    if (wouldCycle(static_cast<int>(tracks_.size()), routeEdgesLocked(), trackIndexLocked(from), trackIndexLocked(to))) {
        throw std::invalid_argument("Track " + std::to_string(from) + " can't " + what + " track " +
                                    std::to_string(to) + ": that track feeds it");
    }
}

void Engine::setTrackOutput(uint32_t trackId, uint32_t outputTrackId) {
    std::lock_guard lock(mutex_);
    TrackModel& track = arrangementTrackLocked(trackId);
    if (outputTrackId != kMaster) {
        arrangementTrackLocked(outputTrackId);
        checkRouteLocked(trackId, outputTrackId, "go into");
    }
    if (track.output == outputTrackId) return;
    track.output = outputTrackId;
    rebuildSnapshotLocked();
}

uint32_t Engine::trackOutput(uint32_t trackId) {
    std::lock_guard lock(mutex_);
    return arrangementTrackLocked(trackId).output;
}

void Engine::setTrackSend(uint32_t trackId, uint32_t toTrackId, float gain, bool preFader) {
    std::lock_guard lock(mutex_);
    TrackModel& track = arrangementTrackLocked(trackId);
    arrangementTrackLocked(toTrackId);  // not the master: everything reaches it anyway
    gain = std::max(0.f, gain);
    const auto it = std::find_if(track.sends.begin(), track.sends.end(),
                                 [toTrackId](const SendModel& send) { return send.to == toTrackId; });
    if (it != track.sends.end()) {
        it->state->gain.store(gain);
        if (it->preFader != preFader) {
            it->preFader = preFader;
            rebuildSnapshotLocked();
        }
        return;
    }
    checkRouteLocked(trackId, toTrackId, "send to");
    SendModel send;
    send.to = toTrackId;
    send.preFader = preFader;
    send.state = std::make_shared<EdgeState>(Renderer::kMaxBlock);
    send.state->gain.store(gain);
    track.sends.push_back(std::move(send));
    rebuildSnapshotLocked();
}

void Engine::removeTrackSend(uint32_t trackId, uint32_t toTrackId) {
    std::lock_guard lock(mutex_);
    TrackModel& track = arrangementTrackLocked(trackId);
    if (std::erase_if(track.sends, [toTrackId](const SendModel& send) { return send.to == toTrackId; }) > 0) {
        rebuildSnapshotLocked();
    }
}

std::vector<SendInfo> Engine::trackSends(uint32_t trackId) {
    std::lock_guard lock(mutex_);
    std::vector<SendInfo> sends;
    for (const SendModel& send : arrangementTrackLocked(trackId).sends) {
        sends.push_back({send.to, send.state->gain.load(), send.preFader});
    }
    return sends;
}

std::vector<MeterReading> Engine::takeMeters() {
    std::lock_guard lock(mutex_);
    std::vector<MeterReading> meters;
    meters.reserve(tracks_.size() + 1);
    meters.push_back({kMaster, master_.params->peakLeft.exchange(0.f), master_.params->peakRight.exchange(0.f)});
    for (const auto& track : tracks_) {
        meters.push_back({track.id, track.params->peakLeft.exchange(0.f), track.params->peakRight.exchange(0.f)});
    }
    for (const auto& [id, chain] : chains_) {
        if (!chain.params) continue;  // a strip's main chain: its strip's fader
        meters.push_back({chain.stripId, chain.params->peakLeft.exchange(0.f), chain.params->peakRight.exchange(0.f), id});
    }
    return meters;
}

// ---------------------------------------------------------------------------
// Automation

void Engine::setTrackAutomation(uint32_t trackId, const std::vector<AutomationLaneDesc>& lanes) {
    std::lock_guard lock(mutex_);
    trackLocked(trackId).automation = lanes;
    rebuildSnapshotLocked();
}

}  // namespace sub
