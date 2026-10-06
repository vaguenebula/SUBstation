// Engine: device chains, racks, sidechains, and the processors in them.
#include "Engine.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "Rack.h"
#include "builtin/BuiltinProcessor.h"
#include "builtin/BuiltinRegistry.h"
#include "plugins/Vst3Format.h"

namespace sub {

// ---------------------------------------------------------------------------
// Device chains

Engine::ChainModel& Engine::chainLocked(uint32_t chainId) {
    auto it = chains_.find(chainId);
    if (it == chains_.end()) throw std::invalid_argument("Unknown chain id " + std::to_string(chainId));
    return it->second;
}

const std::vector<std::shared_ptr<Processor>>& Engine::insertsLocked(const TrackModel& track) const {
    return chains_.at(track.chainId).inserts;
}

uint32_t Engine::addChainLocked(uint32_t stripId, uint32_t parentRack) {
    const uint32_t id = nextChainId_++;
    chains_[id] = ChainModel{id, stripId, parentRack, {}};
    return id;
}

uint32_t Engine::trackChain(uint32_t trackId) {
    std::lock_guard lock(mutex_);
    return trackLocked(trackId).chainId;
}

uint32_t Engine::processorChain(uint32_t processorId) {
    std::lock_guard lock(mutex_);
    processorLocked(processorId);
    return processors_[processorId].chainId;
}

std::shared_ptr<Processor> Engine::processorLocked(uint32_t processorId) {
    auto it = processors_.find(processorId);
    if (it == processors_.end()) throw std::invalid_argument("Unknown device id " + std::to_string(processorId));
    return it->second.processor;
}

std::shared_ptr<Processor> Engine::processor(uint32_t processorId) {
    std::lock_guard lock(mutex_);
    return processorLocked(processorId);
}

namespace {

// Where `index` puts a device in a chain of `count` (-1 or past the end: last).
int insertPosition(int index, size_t count) {
    const auto size = static_cast<int>(count);
    return (index < 0 || index > size) ? size : index;
}

}  // namespace

uint32_t Engine::insertProcessorLocked(uint32_t chainId, std::shared_ptr<Processor> processor, int index, bool rack) {
    ChainModel& chain = chainLocked(chainId);
    chain.inserts.insert(chain.inserts.begin() + insertPosition(index, chain.inserts.size()), processor);
    const uint32_t id = nextProcessorId_++;
    processors_[id] = {chainId, std::move(processor), std::nullopt, rack, {}};
    rebuildSnapshotLocked();
    return id;
}

Engine::ProcessorIds Engine::processorIdsLocked() const {
    ProcessorIds ids;
    ids.reserve(processors_.size());
    for (const auto& [id, entry] : processors_) ids.emplace(entry.processor.get(), id);
    return ids;
}

std::vector<Engine::StripSlot> Engine::stripSlotsLocked(const TrackModel& track, const ProcessorIds& ids) const {
    std::vector<StripSlot> slots;
    const auto collect = [&](const auto& self, uint32_t chainId, int rack, int chain, bool enabled) -> void {
        const auto& inserts = chains_.at(chainId).inserts;
        for (size_t i = 0; i < inserts.size(); ++i) {
            StripSlot slot;
            slot.processor = inserts[i];
            const auto id = ids.find(inserts[i].get());
            slot.processorId = id != ids.end() ? id->second : 0;
            slot.chainId = chainId;
            slot.index = static_cast<int>(i);
            slot.rack = rack;
            slot.chain = chain;
            slot.enabled = enabled && inserts[i]->isEnabled();
            const auto entry = processors_.find(slot.processorId);
            slot.isRack = entry != processors_.end() && entry->second.rack;
            const int at = static_cast<int>(slots.size());
            slots.push_back(slot);
            if (!slot.isRack) continue;
            const std::vector<uint32_t>& chains = entry->second.chains;
            for (size_t c = 0; c < chains.size(); ++c) self(self, chains[c], at, static_cast<int>(c), slot.enabled);
        }
    };
    collect(collect, track.chainId, -1, 0, true);
    return slots;
}

void Engine::rackContentsLocked(uint32_t rackId, std::vector<uint32_t>& processors, std::vector<uint32_t>& chains) const {
    for (const uint32_t chain : processors_.at(rackId).chains) {
        chains.push_back(chain);
        for (const auto& [id, entry] : processors_) {
            if (entry.chainId != chain) continue;
            processors.push_back(id);
            if (entry.rack) rackContentsLocked(id, processors, chains);
        }
    }
}

void Engine::removeChainContentsLocked(uint32_t chainId) {
    std::vector<uint32_t> doomed, chains{chainId};
    for (const auto& [id, entry] : processors_) {
        if (entry.chainId != chainId) continue;
        doomed.push_back(id);
        if (entry.rack) rackContentsLocked(id, doomed, chains);
    }
    for (const uint32_t id : doomed) {
        retireProcessorLocked(std::move(processors_.at(id).processor));
        processors_.erase(id);
    }
    for (const uint32_t chain : chains) chains_.erase(chain);
}

Engine::ProcessorEntry& Engine::rackLocked(uint32_t rackId) {
    processorLocked(rackId);
    ProcessorEntry& entry = processors_.at(rackId);
    if (!entry.rack) throw std::invalid_argument("Device " + std::to_string(rackId) + " is not a rack");
    return entry;
}

Engine::ChainModel& Engine::rackChainLocked(uint32_t chainId) {
    ChainModel& chain = chainLocked(chainId);
    if (chain.parentRack == 0) throw std::invalid_argument("Chain " + std::to_string(chainId) + " is no rack's");
    return chain;
}

int Engine::chainDepthLocked(uint32_t chainId) const {
    int depth = 0;
    for (const ChainModel* chain = &chains_.at(chainId); chain->parentRack != 0;
         chain = &chains_.at(processors_.at(chain->parentRack).chainId)) {
        ++depth;
    }
    return depth;
}

int Engine::rackHeightLocked(uint32_t rackId) const {
    int deepest = 0;
    for (const uint32_t chain : processors_.at(rackId).chains) {
        for (const auto& [id, entry] : processors_) {
            if (entry.chainId == chain && entry.rack) deepest = std::max(deepest, rackHeightLocked(id));
        }
    }
    return 1 + deepest;
}

void Engine::checkRackSidechainsLocked(uint32_t rackId, uint32_t strip) {
    std::vector<uint32_t> inside, chains;
    rackContentsLocked(rackId, inside, chains);
    for (const uint32_t id : inside) {
        const ProcessorEntry& entry = processors_.at(id);
        if (entry.sidechain) checkSidechainLocked(entry.sidechain->source, strip);
    }
}

uint32_t Engine::addRack(uint32_t chainId, int index) {
    std::lock_guard lock(mutex_);
    chainLocked(chainId);
    if (chainDepthLocked(chainId) >= kMaxRackDepth) {
        throw std::invalid_argument("Racks nest at most " + std::to_string(kMaxRackDepth) + " deep");
    }
    return insertProcessorLocked(chainId, std::make_shared<RackProcessor>(), index, true);
}

uint32_t Engine::addRackChain(uint32_t rackId, int index) {
    std::lock_guard lock(mutex_);
    const uint32_t strip = chainLocked(rackLocked(rackId).chainId).stripId;
    const uint32_t id = addChainLocked(strip, rackId);
    chains_.at(id).params = std::make_shared<TrackParams>();
    std::vector<uint32_t>& chains = processors_.at(rackId).chains;
    chains.insert(chains.begin() + insertPosition(index, chains.size()), id);
    rebuildSnapshotLocked();
    return id;
}

void Engine::removeRackChain(uint32_t chainId) {
    std::lock_guard lock(mutex_);
    const uint32_t rack = rackChainLocked(chainId).parentRack;
    std::erase(processors_.at(rack).chains, chainId);
    removeChainContentsLocked(chainId);
    rebuildSnapshotLocked();
}

void Engine::setRackChainOrder(uint32_t rackId, const std::vector<uint32_t>& chainIds) {
    std::lock_guard lock(mutex_);
    ProcessorEntry& rack = rackLocked(rackId);
    std::vector<uint32_t> sorted = chainIds, current = rack.chains;
    std::sort(sorted.begin(), sorted.end());
    std::sort(current.begin(), current.end());
    if (sorted != current) throw std::invalid_argument("The order must list each of the rack's chains once");
    rack.chains = chainIds;
    rebuildSnapshotLocked();
}

std::vector<uint32_t> Engine::rackChains(uint32_t rackId) {
    std::lock_guard lock(mutex_);
    return rackLocked(rackId).chains;
}

uint32_t Engine::chainRack(uint32_t chainId) {
    std::lock_guard lock(mutex_);
    return chainLocked(chainId).parentRack;
}

std::vector<uint32_t> Engine::chainProcessors(uint32_t chainId) {
    std::lock_guard lock(mutex_);
    const ProcessorIds ids = processorIdsLocked();
    std::vector<uint32_t> order;
    for (const auto& insert : chainLocked(chainId).inserts) order.push_back(ids.at(insert.get()));
    return order;
}

// A rack chain's fader is inside its strip: the strip's cache goes, and what it feeds.
void Engine::setChainGain(uint32_t chainId, float gain) {
    std::lock_guard lock(mutex_);
    ChainModel& chain = rackChainLocked(chainId);
    gain = std::max(0.f, gain);
    if (chain.params->gain.exchange(gain) != gain) touchCacheLocked(chain.stripId, true);
}

void Engine::setChainPan(uint32_t chainId, float pan) {
    std::lock_guard lock(mutex_);
    ChainModel& chain = rackChainLocked(chainId);
    pan = std::clamp(pan, -1.f, 1.f);
    if (chain.params->pan.exchange(pan) != pan) touchCacheLocked(chain.stripId, true);
}

void Engine::setChainMute(uint32_t chainId, bool mute) {
    std::lock_guard lock(mutex_);
    ChainModel& chain = rackChainLocked(chainId);
    if (chain.params->mute.exchange(mute) != mute) touchCacheLocked(chain.stripId, true);
}

void Engine::setChainSolo(uint32_t chainId, bool solo) {
    std::lock_guard lock(mutex_);
    ChainModel& chain = rackChainLocked(chainId);
    if (chain.params->solo.exchange(solo) != solo) touchCacheLocked(chain.stripId, true);
}

void Engine::retireProcessorLocked(std::shared_ptr<Processor> processor) {
    processor->closeEditor();
    graveyard_.push_back(std::move(processor));  // destroyed in idle(), once no snapshot uses it
}

uint32_t Engine::addBuiltinProcessor(uint32_t chainId, const std::string& type, int index) {
    auto processor = BuiltinRegistry::instance().create(type);
    // Files a device loads (a sampler's sample) come from the shared cache, at
    // the engine's rate. (Processors go before the engine does.)
    if (auto* builtin = dynamic_cast<BuiltinProcessor*>(processor.get())) {
        builtin->setSourceLoader([this](const std::string& path) { return loadSource(path); });
    }
    std::lock_guard lock(mutex_);
    chainLocked(chainId);
    processor->prepare(sampleRate_, Renderer::kMaxBlock);  // before the audio thread can see it
    return insertProcessorLocked(chainId, std::move(processor), index);
}

uint32_t Engine::addPluginProcessor(uint32_t chainId, const std::string& format, const std::string& path,
                                    const std::string& uid, int index) {
    if (format != "VST3") throw std::invalid_argument("Unsupported plug-in format: " + format);
    double rate = 0.0;
    {
        std::lock_guard lock(mutex_);
        chainLocked(chainId);
        rate = sampleRate_;
    }
    // Loading can take a while (and show dialogs), so it doesn't hold the lock.
    auto plugin = vst3::Vst3Format::instance().instantiate(path, uid, rate, Renderer::kMaxBlock);
    std::lock_guard lock(mutex_);
    if (sampleRate_ != rate) plugin->prepare(sampleRate_, Renderer::kMaxBlock);  // the device changed meanwhile
    if (!chains_.contains(chainId)) {  // its track went while the plug-in loaded (a dialog's message loop)
        retireProcessorLocked(std::move(plugin));
        throw std::invalid_argument("Unknown chain id " + std::to_string(chainId));
    }
    return insertProcessorLocked(chainId, std::move(plugin), index);
}

void Engine::removeProcessor(uint32_t processorId) {
    std::lock_guard lock(mutex_);
    auto processor = processorLocked(processorId);
    std::erase(chainLocked(processors_[processorId].chainId).inserts, processor);
    if (processors_[processorId].rack) {  // its chains go, with everything in them
        for (const uint32_t chain : std::vector<uint32_t>(processors_[processorId].chains)) {
            removeChainContentsLocked(chain);
        }
    }
    processors_.erase(processorId);
    retireProcessorLocked(std::move(processor));
    rebuildSnapshotLocked();
}

void Engine::setChainOrder(uint32_t chainId, const std::vector<uint32_t>& processorIds) {
    std::lock_guard lock(mutex_);
    ChainModel& chain = chainLocked(chainId);
    std::vector<std::shared_ptr<Processor>> order;
    for (const uint32_t id : processorIds) {
        auto it = processors_.find(id);
        if (it == processors_.end() || it->second.chainId != chainId) {
            throw std::invalid_argument("Device " + std::to_string(id) + " is not in chain " + std::to_string(chainId));
        }
        if (std::find(order.begin(), order.end(), it->second.processor) != order.end()) {
            throw std::invalid_argument("Device " + std::to_string(id) + " is listed twice");
        }
        order.push_back(it->second.processor);
    }
    if (order.size() != chain.inserts.size()) {
        throw std::invalid_argument("The order must list all of the chain's devices");
    }
    chain.inserts = std::move(order);
    rebuildSnapshotLocked();
}

void Engine::moveProcessor(uint32_t processorId, uint32_t toChainId, int index) {
    std::lock_guard lock(mutex_);
    auto processor = processorLocked(processorId);
    ChainModel& to = chainLocked(toChainId);
    ProcessorEntry& entry = processors_[processorId];
    ChainModel& from = chainLocked(entry.chainId);
    if (entry.rack) {
        // Not into itself, nor nested too deep.
        for (uint32_t chain = toChainId; chains_.at(chain).parentRack != 0;
             chain = processors_.at(chains_.at(chain).parentRack).chainId) {
            if (chains_.at(chain).parentRack == processorId) {
                throw std::invalid_argument("A rack can't go into one of its own chains");
            }
        }
        if (chainDepthLocked(toChainId) + rackHeightLocked(processorId) > kMaxRackDepth) {
            throw std::invalid_argument("Racks nest at most " + std::to_string(kMaxRackDepth) + " deep");
        }
    }
    if (from.stripId != to.stripId) {
        if (entry.sidechain) checkSidechainLocked(entry.sidechain->source, to.stripId);
        if (entry.rack) checkRackSidechainsLocked(processorId, to.stripId);
    }
    std::erase(from.inserts, processor);
    to.inserts.insert(to.inserts.begin() + insertPosition(index, to.inserts.size()), processor);
    if (from.stripId != to.stripId) {
        // Another strip's signal: what it holds of the old one (a reverb's tail,
        // notes that strip will release elsewhere) stops; a rack's devices go along.
        processor->requestReset();
        if (entry.rack) {
            std::vector<uint32_t> inside, chains;
            rackContentsLocked(processorId, inside, chains);
            for (const uint32_t chain : chains) chains_.at(chain).stripId = to.stripId;
            for (const uint32_t id : inside) processors_.at(id).processor->requestReset();
        }
    }
    entry.chainId = toChainId;
    rebuildSnapshotLocked();
}

ProcessorInfo Engine::processorInfo(uint32_t processorId) {
    auto p = processor(processorId);
    return {p->typeId(), p->name(), p->latencySamples(), p->tailSamples(), p->hasEditor(), p->hasSidechain()};
}

void Engine::setProcessorSidechain(uint32_t processorId, uint32_t sourceTrackId, SidechainTap tap,
                                   uint32_t tapProcessorId) {
    std::lock_guard lock(mutex_);
    const auto processor = processorLocked(processorId);
    ProcessorEntry& entry = processors_[processorId];
    if (!processor->hasSidechain()) {
        throw std::invalid_argument("Device " + std::to_string(processorId) + " has no sidechain input");
    }
    if (sourceTrackId == kMaster) {
        throw std::invalid_argument("The master can't be a sidechain: it renders after every track");
    }
    const TrackModel& source = arrangementTrackLocked(sourceTrackId);
    if (tap == SidechainTap::AfterDevice) {
        const auto found = processors_.find(tapProcessorId);
        const auto& inserts = insertsLocked(source);
        if (found == processors_.end() ||
            std::find(inserts.begin(), inserts.end(), found->second.processor) == inserts.end()) {
            throw std::invalid_argument("Device " + std::to_string(tapProcessorId) + " is not on track " +
                                        std::to_string(sourceTrackId));
        }
    } else {
        tapProcessorId = 0;
    }
    if (!entry.sidechain || entry.sidechain->source != sourceTrackId) {
        checkSidechainLocked(sourceTrackId, chainLocked(entry.chainId).stripId);
    }
    if (!entry.sidechain) {
        entry.sidechain.emplace();
        entry.sidechain->state = std::make_shared<EdgeState>(Renderer::kMaxBlock);
    }
    entry.sidechain->source = sourceTrackId;
    entry.sidechain->tap = tap;
    entry.sidechain->tapProcessor = tapProcessorId;
    rebuildSnapshotLocked();
}

void Engine::clearProcessorSidechain(uint32_t processorId) {
    std::lock_guard lock(mutex_);
    processorLocked(processorId);
    ProcessorEntry& entry = processors_[processorId];
    if (!entry.sidechain) return;
    entry.sidechain.reset();
    rebuildSnapshotLocked();
}

std::optional<SidechainInfo> Engine::processorSidechain(uint32_t processorId) {
    std::lock_guard lock(mutex_);
    processorLocked(processorId);
    const ProcessorEntry& entry = processors_[processorId];
    if (!entry.sidechain) return std::nullopt;
    return SidechainInfo{entry.sidechain->source, entry.sidechain->tap, entry.sidechain->tapProcessor};
}

std::vector<ParamInfo> Engine::processorParams(uint32_t processorId) { return processor(processorId)->params(); }

int Engine::processorParamIndex(uint32_t processorId, const std::string& paramId) {
    auto p = processor(processorId);
    const auto& params = p->params();
    for (size_t i = 0; i < params.size(); ++i) {
        if (params[i].id == paramId) return static_cast<int>(i);
    }
    return -1;
}

float Engine::processorParam(uint32_t processorId, int index) { return processor(processorId)->getParam(index); }

// Plug-in calls from here on are made without holding the lock (see Engine.h).
void Engine::setProcessorParam(uint32_t processorId, int index, float value) {
    const std::shared_ptr<Processor> p = processor(processorId);
    p->setParam(index, value);
    p->noteChange();  // (a plug-in counts its own as well; built-in devices don't)
    std::lock_guard lock(mutex_);
    processorChangedLocked(processorId);
}

std::string Engine::processorParamText(uint32_t processorId, int index, float value) {
    return processor(processorId)->paramText(index, value);
}

std::vector<DisplayInfo> Engine::processorDisplays(uint32_t processorId) {
    return processor(processorId)->displays();
}

uint64_t Engine::readProcessorDisplay(uint32_t processorId, int index, uint64_t position, std::vector<float>& out) {
    std::shared_ptr<Processor> p;
    {
        // Someone watches it: its strip's devices keep running (background freezing).
        std::lock_guard lock(mutex_);
        p = processorLocked(processorId);
        if (TrackModel* strip = stripOfProcessorLocked(processorId)) {
            const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
            strip->cache.point->observedUntilNs.store(now + 1'500'000'000, std::memory_order_relaxed);
        }
    }
    return p->readDisplay(index, position, out);
}

void Engine::setProcessorEnabled(uint32_t processorId, bool enabled) {
    std::lock_guard lock(mutex_);
    processorLocked(processorId)->setEnabled(enabled);
    rebuildSnapshotLocked();  // a switched-off device adds no latency
}

std::vector<uint8_t> Engine::processorState(uint32_t processorId) { return processor(processorId)->getState(); }

void Engine::setProcessorState(uint32_t processorId, const std::vector<uint8_t>& state) {
    const std::shared_ptr<Processor> p = processor(processorId);
    p->setState(state);
    p->noteChange();
    std::lock_guard lock(mutex_);
    processorChangedLocked(processorId);
}

bool Engine::openEditor(uint32_t processorId, uintptr_t ownerWindow, const std::string& title) {
    return processor(processorId)->openEditor(reinterpret_cast<void*>(ownerWindow), title);
}

void Engine::closeEditor(uint32_t processorId) { processor(processorId)->closeEditor(); }

bool Engine::isEditorOpen(uint32_t processorId) { return processor(processorId)->isEditorOpen(); }

bool Engine::setEditorVisible(uint32_t processorId, bool visible) {
    return processor(processorId)->setEditorVisible(visible);
}

void Engine::setEditorTitle(uint32_t processorId, const std::string& title) {
    processor(processorId)->setEditorTitle(title);
}

std::vector<ProcessorEventRecord> Engine::takeProcessorEvents() {
    std::vector<std::pair<uint32_t, std::shared_ptr<Processor>>> current;
    {
        std::lock_guard lock(mutex_);
        for (const auto& [id, entry] : processors_) current.emplace_back(id, entry.processor);
    }
    std::vector<ProcessorEventRecord> records;
    std::vector<ProcessorEvent> events;
    for (const auto& [id, p] : current) {
        events.clear();
        p->takeEvents(events);
        for (const ProcessorEvent& event : events) {
            ProcessorEventRecord record;
            static_cast<ProcessorEvent&>(record) = event;
            record.processorId = id;
            records.push_back(record);
        }
    }
    return records;
}

}  // namespace sub
