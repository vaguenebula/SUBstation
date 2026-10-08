// Model -> engine for devices: the processors of every chain (a track's own,
// and racks' chains), kept as devices move between chains; racks and their
// chains' mixers; sidechains; parameters, states and on/off switches.
//
// The engine's chains are keyed: a track's own chain by the track's id, a
// rack's chain by the chain's (ids are unique in the project). Each device's
// processor is in the chain the bridge last put it in (`where`).

#include "audio/BridgePrivate.h"

#include "model/Devices.h"
#include "model/Project.h"
#include "model/Timebase.h"

#include <QFileInfo>

#include <algorithm>

namespace sub::app {

namespace {

// The devices of a track the engine has: none while it is frozen.
const std::vector<Device>& loadedDevices(const Track& track) {
    static const std::vector<Device> none;
    return track.frozen ? none : track.devices;
}

std::optional<QByteArray> fromBase64(const QString& text) {
    const auto decoded = QByteArray::fromBase64Encoding(text.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) return std::nullopt;
    return *decoded;
}

std::vector<uint8_t> bytes(const QByteArray& data) { return {data.begin(), data.end()}; }

}  // namespace

// Every chain in the project the engine has, by key: (its track, its devices).
QMap<QString, EngineBridge::ModelChain> EngineBridge::modelChains() const {
    QMap<QString, ModelChain> chains;
    for (const Track* track : project_->allTracks()) {
        const std::vector<Device>& devices = loadedDevices(*track);
        chains.insert(track->id, {track->id, &devices});
        for (const ConstRackChain& rc : iterChains(devices)) chains.insert(rc.chain->id, {track->id, &rc.chain->devices});
    }
    return chains;
}

QStringList EngineBridge::ownedChains(const QString& trackId) const {
    QStringList keys;
    for (auto it = d_->chainOwner.constBegin(); it != d_->chainOwner.constEnd(); ++it) {
        if (it.value() == trackId) keys.append(it.key());
    }
    return keys;
}

// A track's devices (in racks too) to the engine. Devices still there keep
// their processors (a plug-in keeps its state and editor), and so do devices
// that moved here from another chain (of this track or another, into or out of
// a rack), or a rack with everything in it; new ones get one; the rest go.
void EngineBridge::syncDevices(const QString& trackId) {
    Private& d = *d_;
    if (!d.chains.contains(trackId) || d.syncing.contains(trackId)) return;
    d.syncing.insert(trackId);
    QSet<QString> changed;  // chain keys whose devices changed
    try {
        const std::vector<Device>& devices = loadedDevices(project_->track(trackId));
        QMap<QString, std::vector<ChainEntry>> before;
        for (const QString& key : ownedChains(trackId)) before.insert(key, d.devices.value(key));
        place(trackId, trackId, devices, changed);  // every chain, top down
        const QMap<QString, ModelChain> model = modelChains();
        // What left its chain, and isn't in another of this track's.
        for (auto it = before.constBegin(); it != before.constEnd(); ++it) {
            const QString& key = it.key();
            QSet<QString> wanted;
            if (const auto found = model.constFind(key); found != model.constEnd()) {
                for (const Device& device : *found->devices) wanted.insert(device.id);
            }
            for (const auto& [deviceId, processorId] : it.value()) {
                if (!wanted.contains(deviceId) && d.where.value(deviceId) == key) {
                    dispose(trackId, key, deviceId, processorId);
                    changed.insert(key);
                }
            }
        }
        // Rack chains that went (their racks stay).
        for (const QString& key : ownedChains(trackId)) {
            if (model.contains(key) || key == trackId) continue;
            try {
                engine_.removeRackChain(d.chains.value(key));
            } catch (const std::invalid_argument&) {
            }
            dropChain(key);
        }
        // (The track's devices again: disposing may have synced another track, but never changed this one's.)
        const std::vector<Device>& now = loadedDevices(project_->track(trackId));
        QStringList keys{trackId};
        for (const ConstRackChain& rc : iterChains(now)) keys.append(rc.chain->id);
        for (const QString& key : keys) {
            const std::vector<ChainEntry>& entries = d.devices[key];
            std::vector<QString> ids;
            for (const auto& entry : entries) ids.push_back(entry.first);
            std::vector<QString> idsBefore;
            for (const auto& entry : before.value(key)) idsBefore.push_back(entry.first);
            if (changed.contains(key) || ids != idsBefore) {
                std::vector<uint32_t> order;
                for (const auto& [deviceId, processorId] : entries) {
                    if (processorId) order.push_back(*processorId);
                }
                engine_.setChainOrder(d.chains.value(key), order);
                changed.insert(key);
            }
        }
        for (const Device* rack : iterDevices(now)) {
            const std::optional<quint32> processorId = d.pids.value(rack->id);
            if (!rack->isRack() || !processorId) continue;
            std::vector<quint32> order;
            for (const Chain& chain : rack->chains) order.push_back(d.chains.value(chain.id));
            if (!d.rackOrders.contains(rack->id) || d.rackOrders.value(rack->id) != order) {
                engine_.setRackChainOrder(*processorId, order);
                d.rackOrders.insert(rack->id, order);
            }
        }
    } catch (...) {
        d.syncing.remove(trackId);
        throw;
    }
    d.syncing.remove(trackId);
    if (!changed.isEmpty()) {
        pushAutomation(trackId);  // its devices' envelopes go to the new processors
        updateEditorTitles(trackId);
        Q_EMIT devicesLoaded(trackId);
    } else if (project_->hasOwner(trackId) && macroEnvelopes(trackId) != d.macroMoved.value(trackId)) {
        pushAutomation(trackId);  // macros mapped otherwise, or over other ranges
    }
    if (project_->hasOwner(trackId)) pushEnabled(trackId);
    pushChainMixers(trackId);
    pushSidechains();  // (the new processors', or a sidechain changed)
}

// A chain's devices into its engine chain (in no particular order yet), then
// the chains of the racks among them.
void EngineBridge::place(const QString& trackId, const QString& key, const std::vector<Device>& devices,
                         QSet<QString>& changed) {
    Private& d = *d_;
    QMap<QString, std::optional<quint32>> current;
    for (const auto& [deviceId, processorId] : d.devices.value(key)) {
        if (d.where.value(deviceId) == key) current.insert(deviceId, processorId);
    }
    std::vector<ChainEntry> chain;
    for (const Device& device : devices) {
        std::optional<quint32> processorId;
        if (current.contains(device.id)) {
            processorId = current.value(device.id);
        } else {
            const auto [found, taken] = takeOver(trackId, key, device);
            processorId = taken;
            if (!found) {
                processorId = createProcessor(d.chains.value(key), device);
                d.pids.insert(device.id, processorId);
            }
            d.where.insert(device.id, key);
            changed.insert(key);
        }
        chain.emplace_back(device.id, processorId);
    }
    d.devices.insert(key, chain);
    for (size_t i = 0; i < devices.size(); ++i) {
        if (devices[i].isRack() && chain[i].second) placeRack(trackId, devices[i], *chain[i].second, changed);
    }
}

void EngineBridge::placeRack(const QString& trackId, const Device& rack, quint32 processorId, QSet<QString>& changed) {
    Private& d = *d_;
    for (const Chain& chain : rack.chains) {
        std::optional<quint32> engineChain;
        if (d.chains.contains(chain.id)) engineChain = d.chains.value(chain.id);
        if (engineChain && d.rackOfChain.value(chain.id) != rack.id) {
            // The chain is another rack's now: a new engine chain, with its devices.
            const quint32 moved = engine_.addRackChain(processorId, -1);
            for (const auto& [deviceId, inner] : d.devices.value(chain.id)) {
                if (inner) engine_.moveProcessor(*inner, moved, -1);
            }
            try {
                engine_.removeRackChain(*engineChain);
            } catch (const std::invalid_argument&) {
            }
            engineChain = moved;
        } else if (!engineChain) {
            engineChain = engine_.addRackChain(processorId, -1);
            d.devices.insert(chain.id, {});
            changed.insert(chain.id);
        }
        d.chains.insert(chain.id, *engineChain);
        d.chainOwner.insert(chain.id, trackId);
        d.rackOfChain.insert(chain.id, rack.id);
        place(trackId, chain.id, chain.devices, changed);
    }
}

// A device new to a chain: whether its processor is in another chain (of any
// track: it moved here), and which then; it moves along in the engine (a rack
// with its chains and everything in them).
std::pair<bool, std::optional<quint32>> EngineBridge::takeOver(const QString& trackId, const QString& key,
                                                               const Device& device) {
    Private& d = *d_;
    const auto old = d.where.constFind(device.id);
    if (old == d.where.constEnd() || *old == key || !d.pids.contains(device.id)) return {false, std::nullopt};
    const QString from = *old;
    const std::optional<quint32> processorId = d.pids.value(device.id);
    std::vector<ChainEntry>& entries = d.devices[from];
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const ChainEntry& e) { return e.first == device.id; }),
                  entries.end());
    const std::vector<Device> moving{device};
    if (processorId) {
        if (d.chainOwner.value(from) != trackId) {  // its sidechains come back once there (if they can)
            for (const Device* inner : iterDevices(moving)) {
                if (const std::optional<quint32> innerId = d.pids.value(inner->id)) dropSidechain(*innerId);
            }
        }
        engine_.moveProcessor(*processorId, d.chains.value(key), -1);
    }
    for (const ConstRackChain& rc : iterChains(moving)) {
        if (d.chainOwner.contains(rc.chain->id)) d.chainOwner.insert(rc.chain->id, trackId);
    }
    return {true, processorId};
}

// A device left a chain of this track and isn't on it any more: if it went to
// another track, that track takes it over now; otherwise it goes (and a rack
// with what is in it, but what of that went elsewhere).
void EngineBridge::dispose(const QString& trackId, const QString& key, const QString& deviceId,
                           std::optional<quint32> processorId) {
    Private& d = *d_;
    const std::optional<QString> owner = project_->deviceOwner(deviceId);
    if (owner && *owner != trackId && d.chains.contains(*owner) && !d.syncing.contains(*owner)) {
        syncDevices(*owner);
        if (d.where.value(deviceId) != key) return;
    }
    for (const QString& chain : chainsOf(deviceId)) {
        const std::vector<ChainEntry> entries = d.devices.value(chain);
        for (const auto& [inner, innerId] : entries) {
            if (d.where.value(inner) == chain) dispose(trackId, chain, inner, innerId);
        }
    }
    forgetProcessor(deviceId, processorId);
}

QStringList EngineBridge::chainsOf(const QString& rackId) const {
    QStringList chains;
    for (auto it = d_->rackOfChain.constBegin(); it != d_->rackOfChain.constEnd(); ++it) {
        if (it.value() == rackId) chains.append(it.key());
    }
    return chains;
}

void EngineBridge::dropChain(const QString& key) {
    d_->chains.remove(key);
    d_->devices.remove(key);
    d_->chainOwner.remove(key);
    d_->rackOfChain.remove(key);
    d_->chainMixer.remove(key);
    d_->chainMeters.remove(key);
}

bool EngineBridge::hasSidechainInput(const QString& trackId, const QString& deviceId) {
    const auto processorId = engineDeviceId(trackId, deviceId);
    return processorId && engine_.processorInfo(*processorId).hasSidechain;
}

// The sidechain the engine should give a device's processor (none: none, or
// one from a track the engine hasn't yet).
std::optional<EngineBridge::SidechainState> EngineBridge::wantedSidechain(const Device& device, quint32 processorId) {
    if (!device.sidechain || device.sidechain->trackId == kMaster) return std::nullopt;
    const Sidechain sidechain = *device.sidechain;
    const auto source = engineTrackId(sidechain.trackId);
    if (!source || !engine_.processorInfo(processorId).hasSidechain) return std::nullopt;
    if (sidechain.tap == kPostFader) return SidechainState{*source, sub::SidechainTap::PostFader, 0};
    if (sidechain.tap == kPreFx) {
        // A MIDI track's own audio is its instrument's (or its instrument rack's): before its effects.
        const Track* sourceTrack = project_->findTrack(sidechain.trackId);
        if (sourceTrack != nullptr && !sourceTrack->devices.empty() && deviceIsInstrument(sourceTrack->devices.front())) {
            if (const auto instrument = engineDeviceId(sidechain.trackId, sourceTrack->devices.front().id)) {
                return SidechainState{*source, sub::SidechainTap::AfterDevice, *instrument};
            }
        }
        return SidechainState{*source, sub::SidechainTap::PreFx, 0};
    }
    const std::optional<quint32> tapped =
        sidechain.tap == kPreFader ? std::nullopt : engineDeviceId(sidechain.trackId, sidechain.tap);
    if (!tapped) {  // before the fader (also while the device it is taken after isn't on the source)
        return SidechainState{*source, sub::SidechainTap::PreFader, 0};
    }
    return SidechainState{*source, sub::SidechainTap::AfterDevice, *tapped};
}

// Every device's sidechain to the engine (devices in racks too): those
// changing go first, so that no step closes a cycle.
void EngineBridge::pushSidechains() {
    Private& d = *d_;
    std::vector<std::pair<quint32, SidechainState>> wanted;
    std::vector<std::pair<QString, QString>> sidechained;  // (track, device)
    for (const Track* track : project_->allTracks()) {
        for (const Device* device : iterDevices(track->devices)) {
            if (device->sidechain) sidechained.emplace_back(track->id, device->id);
        }
    }
    for (const auto& [trackId, deviceId] : sidechained) {
        const auto processorId = engineDeviceId(trackId, deviceId);
        if (!processorId) continue;
        const Device* device = project_->findDevice(trackId, deviceId);
        if (device == nullptr) continue;
        if (const auto state = wantedSidechain(*device, *processorId)) wanted.emplace_back(*processorId, *state);
    }
    const auto wantedOf = [&](quint32 processorId) -> const SidechainState* {
        for (const auto& [id, state] : wanted) {
            if (id == processorId) return &state;
        }
        return nullptr;
    };
    std::vector<quint32> changing;
    for (const auto& [processorId, state] : d.sidechains) {
        const SidechainState* want = wantedOf(processorId);
        if (want == nullptr || !(*want == state)) changing.push_back(processorId);
    }
    for (const quint32 processorId : changing) dropSidechain(processorId);
    for (const auto& [processorId, state] : wanted) {
        if (d.sidechains.count(processorId) > 0) continue;
        try {
            engine_.setProcessorSidechain(processorId, state.source, state.tap, state.tapProcessor);
        } catch (const std::invalid_argument&) {
            continue;  // a cycle with a route another change hasn't undone yet: it comes with that change
        }
        d.sidechains[processorId] = state;
    }
}

void EngineBridge::dropSidechain(quint32 processorId) {
    if (d_->sidechains.erase(processorId) > 0) engine_.clearProcessorSidechain(processorId);
}

// The devices switched on or off as the model has them; those whose switch's
// automation plays are on (their lanes switch them). One switched by hand while
// its switch is automated overrides that automation.
void EngineBridge::pushEnabled(const QString& trackId) {
    std::vector<std::pair<QString, bool>> devices;
    for (const Device* device : iterDevices(project_->track(trackId).devices)) {
        devices.emplace_back(device->id, device->enabled);
    }
    QStringList switched;  // by hand, while automated
    for (const auto& [deviceId, own] : devices) {
        const QString key = automation::deviceOnKey(deviceId);
        const auto before = d_->ownEnabled.constFind(deviceId);
        if (before != d_->ownEnabled.constEnd() && *before != own && isAutomated(trackId, key)) switched.append(key);
        d_->ownEnabled.insert(deviceId, own);
        const auto processorId = engineDeviceId(trackId, deviceId);
        if (!processorId) continue;
        const bool enabled = own || isAutomated(trackId, key);
        const auto told = d_->enabled.constFind(*processorId);
        if (told != d_->enabled.constEnd() && *told == enabled) continue;
        engine_.setProcessorEnabled(*processorId, enabled);
        d_->enabled.insert(*processorId, enabled);
    }
    for (const QString& key : switched) overrideAutomation(trackId, key);
}

void EngineBridge::onChainChanged(const QString& trackId, const QString& chainId) {
    const Chain& chain = project_->chain(trackId, chainId);
    const double volumeDb = chain.volumeDb;
    const double pan = chain.pan;
    const auto old = d_->chainMixer.constFind(chainId);
    if (old != d_->chainMixer.constEnd()) {  // changed by hand while automated: its automation stops
        const Private::ChainMixer before = *old;
        const QString rackId = project_->chainRack(trackId, chainId).id;
        if (volumeDb != before.volumeDb) {
            overrideAutomation(trackId, automation::chainKey(rackId, chainId, automation::kChainVolume));
        }
        if (pan != before.pan) overrideAutomation(trackId, automation::chainKey(rackId, chainId, automation::kChainPan));
    }
    pushChainMixers(trackId);
}

// A track's rack chains' faders to the engine (those that changed).
void EngineBridge::pushChainMixers(const QString& trackId) {
    if (!project_->hasOwner(trackId)) return;
    for (const ConstRackChain& rc : iterChains(project_->track(trackId).devices)) {
        const Chain& chain = *rc.chain;
        const auto engineChain = d_->chains.constFind(chain.id);
        const Private::ChainMixer state{chain.volumeDb, chain.pan, chain.mute, chain.solo};
        const auto found = d_->chainMixer.constFind(chain.id);
        const Private::ChainMixer* old = found != d_->chainMixer.constEnd() ? &*found : nullptr;
        if (engineChain == d_->chains.constEnd() || (old != nullptr && *old == state)) continue;
        const quint32 id = *engineChain;
        if (!old || old->volumeDb != state.volumeDb) engine_.setChainGain(id, static_cast<float>(dbToGain(chain.volumeDb)));
        if (!old || old->pan != state.pan) engine_.setChainPan(id, static_cast<float>(chain.pan));
        if (!old || old->mute != state.mute) engine_.setChainMute(id, chain.mute);
        if (!old || old->solo != state.solo) engine_.setChainSolo(id, chain.solo);
        d_->chainMixer.insert(chain.id, state);
    }
}

std::optional<quint32> EngineBridge::createProcessor(quint32 chainId, const Device& device) {
    if (device.isRack()) {  // (its chains come next: placeRack)
        try {
            return engine_.addRack(chainId, -1);
        } catch (const std::invalid_argument& error) {  // nested too deep (a file edited by hand)
            Q_EMIT statusMessage(QString::fromStdString(error.what()));
            return std::nullopt;
        }
    }
    if (device.isPlugin()) return loadPlugin(chainId, device);
    quint32 processorId = 0;
    try {
        processorId = engine_.addBuiltinProcessor(chainId, device.kind.toStdString(), -1);
    } catch (const std::exception&) {  // a device of a newer version (a preset, a project): missing, as a plug-in
        pluginFailed(device.id, deviceName(device) + QStringLiteral(" is not a device this version of SUBstation has."));
        return std::nullopt;
    }
    for (auto it = device.params.constBegin(); it != device.params.constEnd(); ++it) {
        setParam(processorId, it.key(), it.value());
    }
    if (device.state && !device.state->isEmpty()) setBuiltinState(processorId, device);
    return processorId;
}

std::optional<QString> EngineBridge::pluginPath(const PluginRef& plugin) const {
    if (!plugin.path.isEmpty() && QFileInfo::exists(plugin.path)) return plugin.path;
    const auto known = d_->knownPlugins.constFind(plugin.uid);
    if (known == d_->knownPlugins.constEnd()) return std::nullopt;
    return *known;
}

std::optional<quint32> EngineBridge::loadPlugin(quint32 chainId, const Device& device) {
    const PluginRef plugin = *device.plugin;
    const QString deviceId = device.id;
    const std::optional<QString> path = pluginPath(plugin);
    if (!path) {
        pluginFailed(deviceId, plugin.name + QStringLiteral(" is not installed."));
        return std::nullopt;
    }
    if (d_->deferring) {  // it loads after the project shows (BridgeLoading.cpp)
        deferPlugin(deviceId);
        return std::nullopt;
    }
    // Its state as it was when the device went away (undo), else as saved.
    const std::optional<QString> saved = device.state;
    quint32 processorId = 0;
    ++d_->busy;
    try {
        processorId = engine_.addPluginProcessor(chainId, plugin.format.toStdString(), path->toStdString(),
                                                 plugin.uid.toStdString(), -1);
    } catch (const std::exception& error) {
        --d_->busy;
        pluginFailed(deviceId, plugin.name + QStringLiteral(" could not be loaded: ") + QString::fromStdString(error.what()));
        return std::nullopt;
    }
    --d_->busy;
    d_->pluginIds.insert(processorId, *path);
    d_->pluginErrors.remove(deviceId);
    std::optional<QByteArray> state;
    if (d_->pluginStates.contains(deviceId)) state = d_->pluginStates.take(deviceId);
    if (!state && saved && !saved->isEmpty()) state = fromBase64(*saved);
    if (state && !state->isEmpty()) setPluginState(processorId, plugin.name, *state);
    return processorId;
}

void EngineBridge::pluginFailed(const QString& deviceId, const QString& message) {
    d_->pluginErrors.insert(deviceId, message);
    Q_EMIT statusMessage(message);
}

void EngineBridge::setPluginState(quint32 processorId, const QString& name, const QByteArray& state) {
    ++d_->busy;
    try {
        engine_.setProcessorState(processorId, bytes(state));
    } catch (const std::exception& error) {
        Q_EMIT statusMessage(name + QStringLiteral(": its settings could not be restored (") +
                             QString::fromStdString(error.what()) + u')');
    }
    --d_->busy;
}

// A device's processor goes away (with its track if not `remove`; a rack with
// its chains and what is still in them). A plug-in's state is kept, in case the
// device comes back (undo), but not its editor: undo and redo don't open editors.
void EngineBridge::forgetProcessor(const QString& deviceId, std::optional<quint32> processorId, bool remove) {
    Private& d = *d_;
    for (const QString& chain : chainsOf(deviceId)) {  // (the engine removes them with the rack)
        const std::vector<ChainEntry> entries = d.devices.value(chain);
        for (const auto& [inner, innerId] : entries) {
            if (d.where.value(inner) == chain) forgetProcessor(inner, innerId, false);
        }
        dropChain(chain);
    }
    d.rackOrders.remove(deviceId);
    d.pids.remove(deviceId);
    d.where.remove(deviceId);
    d.pluginErrors.remove(deviceId);
    d.editorsWanted.remove(deviceId);
    d.ownEnabled.remove(deviceId);
    if (!processorId) return;
    const quint32 id = *processorId;
    d.hiddenEditors.erase(std::remove(d.hiddenEditors.begin(), d.hiddenEditors.end(), id), d.hiddenEditors.end());
    if (d.pluginIds.contains(id)) {
        try {
            const std::vector<uint8_t> state = engine_.processorState(id);
            d.pluginStates.insert(deviceId, QByteArray(reinterpret_cast<const char*>(state.data()),
                                                       static_cast<qsizetype>(state.size())));
        } catch (const std::exception&) {
        }
        d.pluginIds.remove(id);
    }
    d.enabled.remove(id);
    d.sidechains.erase(id);
    d.paramIds.remove(id);
    d.paramInfos.remove(id);
    d.paramSpecs.remove(id);
    if (remove) engine_.removeProcessor(id);
}

// Every device of a chain goes (a track's own chain: with its track if not `remove`).
void EngineBridge::forgetChainDevices(const QString& key, bool remove) {
    const std::vector<ChainEntry> entries = d_->devices.value(key);
    for (const auto& [deviceId, processorId] : entries) {
        if (d_->where.value(deviceId) == key) forgetProcessor(deviceId, processorId, remove);
    }
    d_->devices.remove(key);
}

std::optional<sub::ParamInfo> EngineBridge::deviceParamInfo(const QString& trackId, const QString& deviceId,
                                                            const QString& paramId) {
    const auto processorId = engineDeviceId(trackId, deviceId);
    if (!processorId) return std::nullopt;
    const std::string id = paramId.toStdString();
    for (const sub::ParamInfo& info : paramInfos(*processorId)) {
        if (info.id == id) return info;
    }
    return std::nullopt;
}

void EngineBridge::onDeviceParamChanged(const QString& trackId, const QString& deviceId, const QString& paramId) {
    overrideAutomation(trackId, automation::deviceKey(deviceId, paramId));
    pushDeviceParam(trackId, deviceId, paramId);
}

void EngineBridge::pushDeviceParam(const QString& trackId, const QString& deviceId, const QString& paramId) {
    const auto processorId = engineDeviceId(trackId, deviceId);
    const Device& device = project_->device(trackId, deviceId);
    if (!processorId || !device.params.contains(paramId)) return;
    const double value = device.params.value(paramId);
    if (d_->pluginIds.contains(*processorId)) {
        const int index = engine_.processorParamIndex(*processorId, paramId.toStdString());
        if (index < 0 || static_cast<double>(engine_.processorParam(*processorId, index)) == value) {
            return;  // an edit made in the plug-in's own editor: it has the value already
        }
        engine_.setProcessorParam(*processorId, index, static_cast<float>(value));
        return;
    }
    setParam(*processorId, paramId, value);
}

void EngineBridge::setParam(quint32 processorId, const QString& paramId, double value) {
    const int index = engine_.processorParamIndex(processorId, paramId.toStdString());
    if (index >= 0) engine_.setProcessorParam(processorId, index, static_cast<float>(value));
}

void EngineBridge::pushDeviceState(const QString& trackId, const QString& deviceId) {
    const auto processorId = engineDeviceId(trackId, deviceId);
    const Device& device = project_->device(trackId, deviceId);
    if (!processorId) return;
    if (!device.isPlugin()) {
        setBuiltinState(*processorId, device);
    } else if (device.state && !device.state->isEmpty()) {
        if (const auto state = fromBase64(*device.state)) setPluginState(*processorId, device.plugin->name, *state);
    }
}

// A built-in device's state is the model's (none: its defaults); it is
// restored in the background (it may load files: a sampler's sample), one at a
// time, in order.
void EngineBridge::setBuiltinState(quint32 processorId, const Device& device) {
    std::vector<uint8_t> state;
    if (device.state && !device.state->isEmpty()) {
        if (const auto decoded = fromBase64(*device.state)) state = bytes(*decoded);
    }
    const QString name = deviceName(device);
    // (The bridge waits for these before it goes: a failure is said on the main thread, if it is still there.)
    d_->statePool.start([this, processorId, name, state] {
        try {
            engine_.setProcessorState(processorId, state);
        } catch (const std::invalid_argument&) {
            // the device went meanwhile
        } catch (const std::exception& error) {
            const QString message = name + QStringLiteral(": ") + QString::fromStdString(error.what());
            QMetaObject::invokeMethod(this, [this, message] { Q_EMIT statusMessage(message); }, Qt::QueuedConnection);
        }
    });
}

void EngineBridge::waitForDeviceStates() {
    loadPendingPlugins();
    d_->statePool.waitForDone();
}

bool EngineBridge::devicesReady() const { return d_->pendingPlugins.isEmpty() && d_->statePool.waitForDone(0); }

const QMap<QString, QString>& EngineBridge::pluginErrors() const { return d_->pluginErrors; }

QString EngineBridge::pluginError(const QString& deviceId) const { return d_->pluginErrors.value(deviceId); }

}  // namespace sub::app
