#include "model/Project.h"

#include "model/Errors.h"
#include "model/TrackNames.h"

#include <QHash>

#include <algorithm>
#include <stdexcept>

namespace sub::app {

namespace {

[[noreturn]] void missing(const char* what, const QString& id) {
    throw std::out_of_range(std::string("no ") + what + " " + id.toStdString());
}

double asDouble(const ChainValue& value) {
    if (const auto* d = std::get_if<double>(&value)) return *d;
    if (const auto* b = std::get_if<bool>(&value)) return *b ? 1.0 : 0.0;
    throw std::bad_variant_access();
}

}  // namespace

QString chainFieldName(ChainField field) {
    switch (field) {
    case ChainField::Name: return QStringLiteral("name");
    case ChainField::VolumeDb: return QStringLiteral("volume_db");
    case ChainField::Pan: return QStringLiteral("pan");
    case ChainField::Mute: return QStringLiteral("mute");
    case ChainField::Solo: return QStringLiteral("solo");
    }
    return {};
}

std::optional<ChainField> chainFieldFromName(const QString& name) {
    for (ChainField field : {ChainField::Name, ChainField::VolumeDb, ChainField::Pan, ChainField::Mute,
                             ChainField::Solo}) {
        if (chainFieldName(field) == name) return field;
    }
    return std::nullopt;
}

ChainValue chainValue(const Chain& chain, ChainField field) {
    switch (field) {
    case ChainField::Name: return chain.name;
    case ChainField::VolumeDb: return chain.volumeDb;
    case ChainField::Pan: return chain.pan;
    case ChainField::Mute: return chain.mute;
    case ChainField::Solo: return chain.solo;
    }
    return {};
}

QString settingsFieldName(SettingsField field) {
    switch (field) {
    case SettingsField::Tempo: return QStringLiteral("tempo");
    case SettingsField::TimeSignature: return QStringLiteral("time_signature");
    case SettingsField::Key: return QStringLiteral("key");
    case SettingsField::LoopEnabled: return QStringLiteral("loop_enabled");
    case SettingsField::LoopStart: return QStringLiteral("loop_start");
    case SettingsField::LoopEnd: return QStringLiteral("loop_end");
    case SettingsField::AutomationLocked: return QStringLiteral("automation_locked");
    }
    return {};
}

Project::Project(QObject* parent) : QObject(parent), master_(newMaster()) {}

SettingsValue Project::setting(SettingsField field) const {
    switch (field) {
    case SettingsField::Tempo: return tempo_;
    case SettingsField::TimeSignature: return timeSignature_;
    case SettingsField::Key: return key_;
    case SettingsField::LoopEnabled: return loopEnabled_;
    case SettingsField::LoopStart: return loopStart_;
    case SettingsField::LoopEnd: return loopEnd_;
    case SettingsField::AutomationLocked: return automationLocked_;
    }
    return {};
}

// --- Queries ---

const Track* Project::findTrack(const QString& trackId) const {
    if (trackId == kMaster) return &master_;
    for (const Track& track : tracks_) {
        if (track.id == trackId) return &track;
    }
    for (const Track& track : returns_) {
        if (track.id == trackId) return &track;
    }
    return nullptr;
}

const Track& Project::track(const QString& trackId) const {
    const Track* found = findTrack(trackId);
    if (found == nullptr) missing("track", trackId);
    return *found;
}

Track& Project::trackRef(const QString& trackId) { return const_cast<Track&>(track(trackId)); }

int Project::trackIndex(const QString& trackId) const {
    for (int index = 0; index < static_cast<int>(tracks_.size()); ++index) {
        if (tracks_[index].id == trackId) return index;
    }
    missing("track", trackId);
}

bool Project::hasTrack(const QString& trackId) const {
    return std::any_of(tracks_.begin(), tracks_.end(), [&](const Track& t) { return t.id == trackId; });
}

const Clip* Project::findClip(const QString& trackId, const QString& clipId) const {
    const Track* owner = findTrack(trackId);
    if (owner == nullptr) return nullptr;
    for (const Clip& clip : owner->clips) {
        if (clip.id == clipId) return &clip;
    }
    return nullptr;
}

const Clip& Project::clip(const QString& trackId, const QString& clipId) const {
    for (const Clip& clip : track(trackId).clips) {
        if (clip.id == clipId) return clip;
    }
    missing("clip", clipId);
}

double Project::endBeat() const {
    double end = 0.0;
    for (const Track& t : tracks_) {
        for (const Clip& c : t.clips) end = std::max(end, c.endBeat(tempo_));
    }
    return end;
}

bool Project::hasOwner(const QString& owner) const {
    return owner == kMaster || hasTrack(owner) || hasReturn(owner);
}

std::vector<const Track*> Project::allTracks() const {
    std::vector<const Track*> all;
    for (const Track& t : tracks_) all.push_back(&t);
    for (const Track& t : returns_) all.push_back(&t);
    all.push_back(&master_);
    return all;
}

const EnvelopeMap& Project::automation(const QString& owner) const { return track(owner).automation; }

Envelope Project::envelope(const QString& owner, const QString& key) const {
    return automation(owner).value(key);
}

const AutomationView& Project::automationView(const QString& owner) const { return track(owner).automationView; }

QStringList Project::owners() const {
    QStringList ids;
    for (const Track& t : tracks_) ids.append(t.id);
    for (const Track& t : returns_) ids.append(t.id);
    ids.append(kMaster);
    return ids;
}

// --- Returns and sends ---

bool Project::hasReturn(const QString& trackId) const {
    return std::any_of(returns_.begin(), returns_.end(), [&](const Track& t) { return t.id == trackId; });
}

int Project::returnIndex(const QString& trackId) const {
    for (int index = 0; index < static_cast<int>(returns_.size()); ++index) {
        if (returns_[index].id == trackId) return index;
    }
    missing("return track", trackId);
}

QString Project::returnLetter(const QString& trackId) const { return sub::app::returnLetter(returnIndex(trackId)); }

std::vector<const Track*> Project::senders() const {
    std::vector<const Track*> all;
    for (const Track& t : tracks_) all.push_back(&t);
    for (const Track& t : returns_) all.push_back(&t);
    return all;
}

bool Project::wouldCycle(const QString& trackId, const QString& returnId) const {
    return sub::app::wouldCycle(tracks_, returns_, trackId, returnId);
}

// --- Inputs ---

bool Project::inputWouldCycle(const QString& trackId, const QString& sourceId) const {
    return sub::app::inputWouldCycle(tracks_, returns_, trackId, sourceId);
}

bool Project::sidechainWouldCycle(const QString& trackId, const QString& sourceId) const {
    return sub::app::sidechainWouldCycle(tracks_, returns_, trackId, sourceId);
}

std::vector<const Track*> Project::sidechainSources(const QString& trackId) const {
    std::vector<const Track*> sources;
    for (const Track* t : senders()) {
        if (t->id != trackId) sources.push_back(t);
    }
    return sources;
}

std::vector<const Track*> Project::inputSources(const QString& trackId) const { return sidechainSources(trackId); }

QString Project::inputName(const QString& sourceId) const {
    return sourceId == kMaster ? QStringLiteral("Resampling") : track(sourceId).name;
}

std::vector<const Track*> Project::sendTargets(const QString& trackId) const {
    std::vector<const Track*> targets;
    if (trackId == kMaster) return targets;
    const RoutingGraph graph = routingGraph(tracks_, returns_);  // once, not once per return
    for (const Track& r : returns_) {
        if (!feeds(graph, r.id, trackId)) targets.push_back(&r);
    }
    return targets;
}

// --- Outputs ---

bool Project::outputWouldCycle(const QString& trackId, const Output& output) const {
    return sub::app::outputWouldCycle(tracks_, returns_, trackId, output);
}

std::optional<QString> Project::outputTarget(const QString& trackId) const {
    return sub::app::outputTarget(track(trackId), tracks_, returns_);
}

// --- Groups ---

int Project::subtreeEnd(int index) const {
    const QString trackId = tracks_.at(index).id;
    int end = index + 1;
    while (end < static_cast<int>(tracks_.size()) && isDescendant(tracks_[end].id, trackId)) ++end;
    return end;
}

std::vector<const Track*> Project::descendants(const QString& trackId) const {
    const int index = trackIndex(trackId);
    std::vector<const Track*> result;
    for (int i = index + 1; i < subtreeEnd(index); ++i) result.push_back(&tracks_[i]);
    return result;
}

QStringList Project::withContents(const QStringList& trackIds) const {
    QSet<QString> wanted;
    for (const QString& id : trackIds) {
        if (hasTrack(id)) wanted.insert(id);
    }
    QStringList result;
    for (const Track& t : tracks_) {
        bool in = wanted.contains(t.id);
        if (!in) {
            for (const QString& group : ancestors(t.id)) {
                if (wanted.contains(group)) {
                    in = true;
                    break;
                }
            }
        }
        if (in) result.append(t.id);
    }
    return result;
}

std::vector<const Track*> Project::children(const QString& trackId) const {
    std::vector<const Track*> result;
    for (const Track& t : tracks_) {
        if (t.parent == trackId) result.push_back(&t);
    }
    return result;
}

QStringList Project::ancestors(const QString& trackId) const {
    QStringList result;
    std::optional<QString> parent = track(trackId).parent;
    while (parent) {
        result.append(*parent);
        parent = track(*parent).parent;
    }
    return result;
}

bool Project::isDescendant(const QString& trackId, const QString& groupId) const {
    return ancestors(trackId).contains(groupId);
}

int Project::depth(const QString& trackId) const { return static_cast<int>(ancestors(trackId).size()); }

bool Project::isHidden(const QString& trackId) const {
    for (const QString& group : ancestors(trackId)) {
        if (track(group).folded) return true;
    }
    return false;
}

std::optional<QString> Project::parentAt(int index) const {
    if (index >= 0 && index < static_cast<int>(tracks_.size())) return tracks_[index].parent;
    return std::nullopt;
}

TrackTree Project::tree() const {
    TrackTree result;
    for (const Track& t : tracks_) result.push_back({t.id, t.parent});
    return result;
}

QString Project::nextColor() const {
    return kTrackColors.at(static_cast<qsizetype>(tracks_.size()) % kTrackColors.size());
}

QString Project::uniqueTrackName(const QString& base) const {
    QSet<QString> names;
    for (const Track& t : tracks_) names.insert(t.name);
    for (const Track& t : returns_) names.insert(t.name);
    if (!names.contains(base)) return base;
    int n = 2;
    while (names.contains(QStringLiteral("%1 %2").arg(base).arg(n))) ++n;
    return QStringLiteral("%1 %2").arg(base).arg(n);
}

// --- Freezing ---

std::optional<QString> Project::frozenBy(const QString& trackId) const {
    if (trackId == kMaster || !hasOwner(trackId)) return std::nullopt;
    std::optional<QString> holder;
    if (track(trackId).frozen) holder = trackId;
    if (hasTrack(trackId)) {
        for (const QString& group : ancestors(trackId)) {
            if (track(group).frozen) holder = group;
        }
    }
    return holder;
}

bool Project::isFrozen(const QString& trackId) const { return frozenBy(trackId).has_value(); }

std::optional<QString> Project::freezeProblem(const QString& trackId) const {
    if (trackId == kMaster) return QStringLiteral("The master can't be frozen");
    const Track& frozenTrack = track(trackId);
    if (frozenTrack.frozen) return QStringLiteral("%1 is frozen already").arg(frozenTrack.name);
    if (const auto holder = frozenBy(trackId)) {
        return QStringLiteral("%1 is in %2, which is frozen").arg(frozenTrack.name, track(*holder).name);
    }
    for (const Track* owner : allTracks()) {
        for (const Device* device : iterDevices(owner->devices)) {
            const auto& sidechain = device->sidechain;
            if (sidechain && sidechain->trackId == trackId && sidechain->tap != kPostFader &&
                sidechain->tap != kPreFader) {
                return QStringLiteral("%1 takes %2's signal before its devices or after one of them as a sidechain, "
                                      "which its frozen audio doesn't have")
                    .arg(owner->name, frozenTrack.name);
            }
        }
    }
    return std::nullopt;
}

std::optional<QString> Project::flattenProblem(const QString& trackId) const {
    const Track& flat = track(trackId);
    if (!flat.hasClips()) return QStringLiteral("%1 can't be flattened: only audio and MIDI tracks can").arg(flat.name);
    if (!flat.frozen) return QStringLiteral("Freeze %1 first").arg(flat.name);
    return std::nullopt;
}

// --- Devices ---

const Device* Project::findDevice(const QString& trackId, const QString& deviceId) const {
    const Track* owner = findTrack(trackId);
    return owner == nullptr ? nullptr : sub::app::findDevice(owner->devices, deviceId);
}

const Device& Project::device(const QString& trackId, const QString& deviceId) const {
    const Device* found = sub::app::findDevice(track(trackId).devices, deviceId);
    if (found == nullptr) missing("device", deviceId);
    return *found;
}

Device& Project::deviceRef(const QString& trackId, const QString& deviceId) {
    return const_cast<Device&>(device(trackId, deviceId));
}

bool Project::hasDevice(const QString& trackId, const QString& deviceId) const {
    return hasOwner(trackId) && sub::app::findDevice(track(trackId).devices, deviceId) != nullptr;
}

std::optional<QString> Project::deviceOwner(const QString& deviceId) const {
    for (const Track* t : allTracks()) {
        if (sub::app::findDevice(t->devices, deviceId) != nullptr) return t->id;
    }
    return std::nullopt;
}

const Chain& Project::chain(const QString& trackId, const QString& chainId) const {
    for (const ConstRackChain& rc : iterChains(track(trackId).devices)) {
        if (rc.chain->id == chainId) return *rc.chain;
    }
    missing("chain", chainId);
}

Chain& Project::chainRef(const QString& trackId, const QString& chainId) {
    return const_cast<Chain&>(chain(trackId, chainId));
}

const Device& Project::chainRack(const QString& trackId, const QString& chainId) const {
    for (const ConstRackChain& rc : iterChains(track(trackId).devices)) {
        if (rc.chain->id == chainId) return *rc.rack;
    }
    missing("chain", chainId);
}

// --- Mutations ---

void Project::insertTrack(Track track, int index) {
    index = std::max(0, std::min(index, static_cast<int>(tracks_.size())));
    const QString id = track.id;
    tracks_.insert(tracks_.begin() + index, std::move(track));
    QStringList renamed = renumber(index);
    renamed.removeAll(id);
    Q_EMIT trackInserted(id, index);
    announceRenamed(renamed);
}

std::pair<Track, int> Project::removeTrack(const QString& trackId) {
    const int index = trackIndex(trackId);
    Track removed = std::move(tracks_[index]);
    tracks_.erase(tracks_.begin() + index);
    const QStringList renamed = renumber(index);
    Q_EMIT trackRemoved(trackId, index);
    announceRenamed(renamed);
    return {std::move(removed), index};
}

void Project::insertReturn(Track track, int index) {
    index = std::max(0, std::min(index, static_cast<int>(returns_.size())));
    const QString id = track.id;
    track.name = track.nameSource();
    returns_.insert(returns_.begin() + index, std::move(track));
    Q_EMIT returnInserted(id, index);
}

std::pair<Track, int> Project::removeReturn(const QString& trackId) {
    const int index = returnIndex(trackId);
    Track removed = std::move(returns_[index]);
    returns_.erase(returns_.begin() + index);
    Q_EMIT returnRemoved(trackId, index);
    return {std::move(removed), index};
}

void Project::arrangeTracks(const TrackTree& tree) {
    QHash<QString, int> indexOf;
    QStringList have;
    for (int i = 0; i < static_cast<int>(tracks_.size()); ++i) {
        indexOf.insert(tracks_[i].id, i);
        have.append(tracks_[i].id);
    }
    QStringList listed;
    for (const TreeEntry& entry : tree) listed.append(entry.id);
    have.sort();
    listed.sort();
    if (have != listed) throw EditError(QStringLiteral("an arrangement lists every track once"));
    std::vector<std::optional<QString>> oldParents;
    for (const Track& t : tracks_) oldParents.push_back(t.parent);
    std::vector<Track> arranged;
    arranged.reserve(tracks_.size());
    for (const TreeEntry& entry : tree) {
        arranged.push_back(std::move(tracks_[indexOf.value(entry.id)]));
        arranged.back().parent = entry.parent;
    }
    if (const auto problem = treeProblem(arranged)) {
        for (Track& t : arranged) {
            const int old = indexOf.value(t.id);
            t.parent = oldParents[old];
            tracks_[old] = std::move(t);
        }
        throw EditError(*problem);
    }
    tracks_ = std::move(arranged);
    const QStringList renamed = renumber(0);
    Q_EMIT tracksArranged();
    announceRenamed(renamed);
}

void Project::updateTrack(const QString& trackId, TrackField field, const TrackValue& value) {
    updateTrack(trackId, TrackValues{{field, value}});
}

void Project::updateTrack(const QString& trackId, const TrackValues& values) {
    Track& changed = trackRef(trackId);
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) changed.setValue(it.key(), it.value());
    if (values.contains(TrackField::Name)) {  // (numbered: its template is what changed)
        if (trackId == kMaster || hasReturn(trackId))
            renumber(static_cast<int>(tracks_.size()), true);
        else
            renumber(trackIndex(trackId));
    }
    Q_EMIT trackChanged(trackId);
}

Track Project::replaceTrack(Track track) {
    const QString id = track.id;
    const int index = trackIndex(id);
    Track old = std::move(tracks_[index]);
    tracks_.erase(tracks_.begin() + index);
    Q_EMIT trackRemoved(id, index);
    tracks_.insert(tracks_.begin() + index, std::move(track));
    renumber(index);
    Q_EMIT trackInserted(id, index);
    return old;
}

void Project::setFrozen(const QString& trackId, const std::optional<Freeze>& freeze) {
    trackRef(trackId).frozen = freeze;
    Q_EMIT freezeChanged(trackId);
}

void Project::setFrozenSegments(const QString& trackId, std::optional<std::vector<Clip>> segments) {
    std::optional<Freeze>& frozen = trackRef(trackId).frozen;
    if (!frozen) return;
    if (segments) {
        std::stable_sort(segments->begin(), segments->end(),
                         [](const Clip& a, const Clip& b) { return a.startBeat < b.startBeat; });
    }
    if (frozen->segments == segments) return;
    frozen->segments = std::move(segments);
    Q_EMIT clipsChanged(trackId);
}

void Project::setClips(const QString& trackId, std::vector<Clip> clips) {
    std::stable_sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.startBeat < b.startBeat; });
    Track& track = trackRef(trackId);
    const bool followed = namedByContents(track);
    track.clips = std::move(clips);
    Q_EMIT clipsChanged(trackId);
    followContents(trackId, followed);
}

void Project::setDevices(const QString& trackId, std::vector<Device> devices) {
    Track& track = trackRef(trackId);
    const bool followed = namedByContents(track);
    track.devices = std::move(devices);
    Q_EMIT devicesChanged(trackId);
    followContents(trackId, followed);
}

void Project::setChains(const OrderedMap<QString, std::vector<Device>>& chains) {
    QSet<QString> followed;
    for (const auto& [trackId, devices] : chains) {
        Track& track = trackRef(trackId);
        if (namedByContents(track)) followed.insert(trackId);
        track.devices = devices;
    }
    for (const auto& entry : chains) Q_EMIT devicesChanged(entry.first);
    for (const auto& entry : chains) followContents(entry.first, followed.contains(entry.first));
}

void Project::followContents(const QString& trackId, bool followed) {
    if (!followed) return;
    Track& track = trackRef(trackId);
    const QString nameTemplate = contentsName(track);
    if (nameTemplate == track.nameSource()) return;
    track.nameTemplate = nameTemplate;
    renumber(trackIndex(trackId));
    Q_EMIT trackChanged(trackId);
}

QStringList Project::renumber(int from, bool returns) {
    QStringList renamed;
    for (int i = std::max(0, from); i < static_cast<int>(tracks_.size()); ++i) {
        Track& t = tracks_[static_cast<size_t>(i)];
        if (t.nameTemplate.isEmpty()) t.nameTemplate = t.name;
        const QString name = numberedName(t.nameTemplate, i + 1);
        if (name == t.name) continue;
        t.name = name;
        renamed.append(t.id);
    }
    if (returns) {  // (not numbered: named as their templates are)
        for (Track& t : returns_) t.name = t.nameSource();
        master_.name = master_.nameSource();
    }
    return renamed;
}

void Project::announceRenamed(const QStringList& trackIds) {
    for (const QString& id : trackIds) Q_EMIT trackChanged(id);
}

void Project::updateChain(const QString& trackId, const QString& chainId, ChainField field, const ChainValue& value) {
    Chain& changed = chainRef(trackId, chainId);
    switch (field) {
    case ChainField::Name: changed.name = std::get<QString>(value); break;
    case ChainField::VolumeDb: changed.volumeDb = asDouble(value); break;
    case ChainField::Pan: changed.pan = asDouble(value); break;
    case ChainField::Mute: changed.mute = std::get<bool>(value); break;
    case ChainField::Solo: changed.solo = std::get<bool>(value); break;
    }
    Q_EMIT chainChanged(trackId, chainId);
}

void Project::setDeviceMacros(const QString& trackId, const QString& deviceId,
                              const std::vector<MacroMapping>& macros) {
    deviceRef(trackId, deviceId).macros = macros;
    Q_EMIT devicesChanged(trackId);
}

void Project::setDeviceName(const QString& trackId, const QString& deviceId, const std::optional<QString>& name) {
    const bool followed = namedByContents(trackRef(trackId));
    deviceRef(trackId, deviceId).name = name;
    Q_EMIT devicesChanged(trackId);
    followContents(trackId, followed);
}

void Project::setDeviceParam(const QString& trackId, const QString& deviceId, const QString& paramId, double value) {
    deviceRef(trackId, deviceId).params.insert(paramId, value);
    Q_EMIT deviceParamChanged(trackId, deviceId, paramId);
}

void Project::setDeviceParams(const QString& trackId, const QMap<DeviceParam, double>& values) {
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        deviceRef(trackId, it.key().first).params.insert(it.key().second, it.value());
    }
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        Q_EMIT deviceParamChanged(trackId, it.key().first, it.key().second);
    }
}

void Project::setDeviceEnabled(const QString& trackId, const QString& deviceId, bool enabled) {
    deviceRef(trackId, deviceId).enabled = enabled;
    Q_EMIT devicesChanged(trackId);
}

void Project::setDeviceSidechain(const QString& trackId, const QString& deviceId,
                                 const std::optional<Sidechain>& sidechain) {
    deviceRef(trackId, deviceId).sidechain = sidechain;
    Q_EMIT devicesChanged(trackId);
}

void Project::setDeviceState(const QString& trackId, const QString& deviceId, const std::optional<QString>& state) {
    deviceRef(trackId, deviceId).state = state;
    Q_EMIT deviceStateChanged(trackId, deviceId);
}

void Project::storePluginState(const QString& trackId, const QString& deviceId, const QString& state,
                               const QString& pluginPath) {
    Device& device = deviceRef(trackId, deviceId);
    device.state = state;
    if (device.plugin && device.plugin->path != pluginPath) device.plugin->path = pluginPath;  // found elsewhere
}

void Project::setDevicesFolded(const QString& trackId, const QSet<QString>& deviceIds, bool folded) {
    const QSet<QString> before = foldedDevices_;
    if (folded) {
        foldedDevices_.unite(deviceIds);
    } else {
        foldedDevices_.subtract(deviceIds);
    }
    if (foldedDevices_ != before) Q_EMIT devicesFolded(trackId);
}

void Project::addFoldedDevices(const QSet<QString>& deviceIds) { foldedDevices_.unite(deviceIds); }

void Project::setChainListShown(const QString& trackId, const QString& rackId, bool shown) {
    if (shown == shownChainLists_.contains(rackId)) return;
    if (shown) {
        shownChainLists_.insert(rackId);
    } else {
        shownChainLists_.remove(rackId);
    }
    Q_EMIT rackViewChanged(trackId);
}

void Project::setRackDevicesShown(const QString& trackId, const QString& rackId, bool shown) {
    if (shown != hiddenRackDevices_.contains(rackId)) return;
    if (shown) {
        hiddenRackDevices_.remove(rackId);
    } else {
        hiddenRackDevices_.insert(rackId);
    }
    Q_EMIT rackViewChanged(trackId);
}

void Project::updateSettings(const SettingsValues& values) {
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        const SettingsValue& value = it.value();
        switch (it.key()) {
        case SettingsField::Tempo: tempo_ = std::get<double>(value); break;
        case SettingsField::TimeSignature: timeSignature_ = std::get<TimeSignature>(value); break;
        case SettingsField::Key: key_ = std::get<std::optional<Key>>(value); break;
        case SettingsField::LoopEnabled: loopEnabled_ = std::get<bool>(value); break;
        case SettingsField::LoopStart: loopStart_ = std::get<double>(value); break;
        case SettingsField::LoopEnd: loopEnd_ = std::get<double>(value); break;
        case SettingsField::AutomationLocked: automationLocked_ = std::get<bool>(value); break;
        }
    }
    Q_EMIT settingsChanged();
}

void Project::setEnvelope(const QString& owner, const QString& key, const Envelope& points) {
    EnvelopeMap& envelopes = trackRef(owner).automation;
    if (!points.empty()) {
        envelopes.insert(key, points);
    } else {
        envelopes.remove(key);
    }
    Q_EMIT automationChanged(owner, key);
}

void Project::setAutomationView(const QString& owner, const AutomationView& view) {
    trackRef(owner).automationView = view;
    Q_EMIT automationViewChanged(owner);
}

void Project::replaceContents(ProjectContents contents) {
    tempo_ = contents.tempo;
    timeSignature_ = contents.timeSignature;
    key_ = contents.key;
    loopEnabled_ = contents.loopEnabled;
    loopStart_ = contents.loopStart;
    loopEnd_ = contents.loopEnd;
    master_ = contents.master ? std::move(*contents.master) : newMaster();
    automationLocked_ = contents.automationLocked;
    tracks_ = std::move(contents.tracks);
    returns_ = std::move(contents.returns);
    renumber(0, true);
    foldedDevices_ = std::move(contents.foldedDevices);
    shownChainLists_ = std::move(contents.shownChainLists);
    hiddenRackDevices_ = std::move(contents.hiddenRackDevices);
    const bool pathChange = path_ != contents.path;
    path_ = contents.path;
    Q_EMIT reset();
    // The settings are new too: what shows them through their properties (the
    // tempo box, Lock Envelopes, the loop button) is notified by this signal only.
    Q_EMIT settingsChanged();
    if (pathChange) Q_EMIT pathChanged();
}

void Project::clear() { replaceContents(ProjectContents{}); }

void Project::setPath(const QString& path) {
    if (path_ == path) return;
    path_ = path;
    Q_EMIT pathChanged();
}

}  // namespace sub::app
