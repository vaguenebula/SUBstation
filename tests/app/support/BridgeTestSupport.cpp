#include "BridgeTestSupport.h"

#include "audio/BridgePrivate.h"
#include "model/Devices.h"
#include "model/Ids.h"
#include "model/Routing.h"
#include "model/Timebase.h"

#include "plugins/Vst3Format.h"

#include <QFileInfo>

#include <algorithm>
#include <cmath>
#include <set>

namespace sub::app {

void BridgeTestAccess::injectEvents(EngineBridge& bridge, const std::vector<sub::ProcessorEventRecord>& events) {
    bridge.d_->injectedEvents.insert(bridge.d_->injectedEvents.end(), events.begin(), events.end());
}

void BridgeTestAccess::stopPluginTimer(EngineBridge& bridge) { bridge.d_->pluginTimer.stop(); }

void BridgeTestAccess::loadNextPlugin(EngineBridge& bridge) { bridge.loadNextPlugin(); }

void BridgeTestAccess::pollMeters(EngineBridge& bridge) { bridge.pollMeters(); }

void BridgeTestAccess::setBusy(EngineBridge& bridge, bool busy) { bridge.d_->busy += busy ? 1 : -1; }

}  // namespace sub::app

namespace sub::app::test {

bool haveTestPlugins(const QString& bundle) { return !bundle.isEmpty() && QFileInfo::exists(bundle); }

namespace {

const std::vector<sub::PluginDescription>& scanned(const QString& bundle) {
    static const std::vector<sub::PluginDescription> found =
        sub::vst3::Vst3Format::instance().scanFile(bundle.toStdString());
    return found;
}

}  // namespace

std::vector<PluginInfo> testPluginInfos(const QString& bundle) {
    std::vector<PluginInfo> plugins;
    if (!haveTestPlugins(bundle)) return plugins;
    for (const sub::PluginDescription& d : scanned(bundle)) {
        PluginInfo info;
        info.name = QString::fromStdString(d.name);
        info.format = QString::fromStdString(d.format);
        info.path = bundle;
        info.uid = QString::fromStdString(d.uid);
        info.vendor = QString::fromStdString(d.vendor);
        info.instrument = d.isInstrument;
        plugins.push_back(info);
    }
    return plugins;
}

std::optional<PluginRef> testPlugin(const QString& bundle, const QString& name) {
    if (!haveTestPlugins(bundle)) return std::nullopt;
    for (const sub::PluginDescription& d : scanned(bundle)) {
        if (QString::fromStdString(d.name) != name) continue;
        PluginRef ref;
        ref.format = QStringLiteral("VST3");
        ref.uid = QString::fromStdString(d.uid);
        ref.name = name;
        ref.vendor = QString::fromStdString(d.vendor);
        ref.path = bundle;
        ref.instrument = d.isInstrument;
        return ref;
    }
    return std::nullopt;
}

// --- Edits ------------------------------------------------------------------------------

QString Edits::addAudioTrack(const QString& name, const std::optional<QString>& parent) {
    Track track;
    track.id = newId();
    track.name = name.isEmpty() ? project_.uniqueTrackName(QStringLiteral("%1 Audio").arg(project_.tracks().size() + 1))
                                : name;
    track.color = project_.nextColor();
    int index = static_cast<int>(project_.tracks().size());
    if (parent) {
        index = project_.subtreeEnd(project_.trackIndex(*parent));
        track.parent = parent;
    } else {
        track.parent = project_.parentAt(index);
    }
    stack_.push(new InsertTrackCommand(&project_, track, index, QStringLiteral("Insert Audio Track")));
    return track.id;
}

QString Edits::addMidiTrack(const QString& name, const std::optional<QString>& instrument,
                            const std::optional<PluginRef>& plugin) {
    Track track;
    track.id = newId();
    track.name = name.isEmpty() ? project_.uniqueTrackName(QStringLiteral("%1 MIDI").arg(project_.tracks().size() + 1))
                                : name;
    track.color = project_.nextColor();
    track.kind = kMidiKind;
    if (plugin) {
        track.devices.push_back(newDevice(kPluginKind, plugin));
    } else if (instrument) {
        track.devices.push_back(newDevice(*instrument));
    }
    const int index = static_cast<int>(project_.tracks().size());
    track.parent = project_.parentAt(index);
    stack_.push(new InsertTrackCommand(&project_, track, index, QStringLiteral("Insert MIDI Track")));
    return track.id;
}

QString Edits::addReturnTrack(const QString& name) {
    Track track;
    track.id = newId();
    const int index = static_cast<int>(project_.returns().size());
    track.name = name.isEmpty() ? project_.uniqueTrackName(returnLetter(index) + QStringLiteral(" Return")) : name;
    track.color = project_.nextColor();
    track.kind = kReturnKind;
    stack_.push(new InsertReturnCommand(&project_, track, index));
    return track.id;
}

void Edits::setClips(const QString& trackId, std::vector<Clip> clips) {
    ClipLists before{{trackId, project_.track(trackId).clips}};
    ClipLists after{{trackId, std::move(clips)}};
    stack_.push(new SetClipsCommand(&project_, QStringLiteral("Add"), before, after));
}

void Edits::setTrack(const QString& trackId, TrackField field, const TrackValue& value, const QString& mergeKey) {
    const TrackValue old = project_.track(trackId).value(field);
    stack_.push(new UpdateTrackCommand(&project_, trackId, field, old, value, QStringLiteral("Change Track"), mergeKey));
}

void Edits::setInput(const QString& trackId, const std::vector<int>& channels, const std::optional<QString>& source) {
    const Track& track = project_.track(trackId);
    const TrackValues old{{TrackField::Input, track.input}, {TrackField::InputTrack, track.inputTrack}};
    const TrackValues now{{TrackField::Input, channels}, {TrackField::InputTrack, source}};
    stack_.push(new UpdateTrackFieldsCommand(&project_, trackId, old, now, QStringLiteral("Change Track Input")));
}

void Edits::setSettings(const SettingsValues& values) {
    SettingsValues old;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) old.insert(it.key(), project_.setting(it.key()));
    stack_.push(new UpdateSettingsCommand(&project_, old, values, QStringLiteral("Change Settings")));
}

void Edits::setSend(const QString& trackId, const QString& returnId, std::optional<double> levelDb,
                    std::optional<bool> preFader) {
    const Track& track = project_.track(trackId);
    const SendMap sends = track.sends;
    const Send old = sends.value(returnId, Send{});
    Send send = old;
    if (levelDb) send.levelDb = std::clamp(*levelDb, automation::kMinVolumeDb, automation::kMaxVolumeDb);
    if (preFader) send.preFader = *preFader;
    SendMap after = sends;
    after.insert(returnId, send);
    stack_.push(new UpdateTrackCommand(&project_, trackId, TrackField::Sends, sends, after, QStringLiteral("Change Send")));
}

void Edits::setEnvelope(const QString& owner, const QString& key, const Envelope& points) {
    stack_.push(new SetEnvelopeCommand(&project_, owner, key, project_.envelope(owner, key), points,
                                       QStringLiteral("Change Automation")));
}

namespace {

// The tree with these tracks (and what is in them) taken out and put, in
// their order, before the track at `at` (in the tree as it is), in `parent`.
TrackTree arranged(const Project& p, const QStringList& roots, int at, const std::optional<QString>& parent) {
    QSet<QString> moving(roots.begin(), roots.end());
    for (const QString& root : roots) {
        for (const Track* d : p.descendants(root)) moving.insert(d->id);
    }
    TrackTree staying;
    TrackTree block;
    int position = 0;
    for (int i = 0; i < static_cast<int>(p.tracks().size()); ++i) {
        const Track& t = p.tracks()[static_cast<size_t>(i)];
        if (moving.contains(t.id)) {
            block.push_back({t.id, roots.contains(t.id) ? parent : t.parent});
        } else {
            staying.push_back({t.id, t.parent});
            if (i < at) ++position;
        }
    }
    TrackTree tree(staying.begin(), staying.begin() + position);
    tree.insert(tree.end(), block.begin(), block.end());
    tree.insert(tree.end(), staying.begin() + position, staying.end());
    return tree;
}

}  // namespace

// Arranges the tracks so. A track taking its input from a group it comes into
// (or from what that group feeds) loses that input first: it would close a
// cycle; and so does a device taking its sidechain from one.
void Edits::arrange(const TrackTree& tree, const QString& text) {
    const Project& p = project_;
    QHash<QString, std::optional<QString>> parents;
    for (const TreeEntry& entry : tree) parents.insert(entry.id, entry.parent);
    std::vector<Track> tracks;
    for (const Track& t : p.tracks()) {
        Track copy = t;
        copy.parent = parents.value(t.id);
        copy.inputTrack.reset();
        copy.devices.clear();
        tracks.push_back(copy);
    }
    std::vector<Track> returns;
    for (const Track& r : p.returns()) {
        Track copy = r;
        copy.devices.clear();
        returns.push_back(copy);
    }
    QStringList cycling;
    for (size_t i = 0; i < tracks.size(); ++i) {
        const std::optional<QString> source = p.tracks()[i].inputTrack;
        if (source && feeds(routingGraph(tracks, returns), tracks[i].id, *source)) {
            cycling.append(tracks[i].id);
        } else {
            tracks[i].inputTrack = source;
        }
    }
    std::vector<std::pair<QString, Device>> cyclingSidechains;
    const auto check = [&](std::vector<Track>& copies, const std::vector<Track>& originals) {
        for (size_t i = 0; i < copies.size(); ++i) {
            for (const Device* device : iterDevices(originals[i].devices)) {
                if (!device->sidechain) continue;
                if (feeds(routingGraph(tracks, returns), copies[i].id, device->sidechain->trackId)) {
                    cyclingSidechains.emplace_back(copies[i].id, *device);
                } else {
                    Device flat = *device;
                    flat.chains.clear();
                    copies[i].devices.push_back(flat);
                }
            }
        }
    };
    check(tracks, p.tracks());
    check(returns, p.returns());
    stack_.beginMacro(text);
    for (const QString& trackId : cycling) {
        stack_.push(new UpdateTrackCommand(&project_, trackId, TrackField::InputTrack, p.track(trackId).inputTrack,
                                           std::optional<QString>(), text));
    }
    for (const auto& [trackId, device] : cyclingSidechains) {
        stack_.push(new SetDeviceSidechainCommand(&project_, trackId, device.id, device.sidechain, std::nullopt, text));
    }
    stack_.push(new ArrangeTracksCommand(&project_, p.tree(), tree, text));
    stack_.endMacro();
}

QString Edits::groupTracks(const QStringList& trackIds) {
    QStringList roots;
    for (const Track& t : project_.tracks()) {
        if (!trackIds.contains(t.id)) continue;
        const QStringList ancestors = project_.ancestors(t.id);
        if (std::none_of(ancestors.begin(), ancestors.end(), [&](const QString& a) { return trackIds.contains(a); })) {
            roots.append(t.id);
        }
    }
    const Track& first = project_.track(roots.first());
    Track group;
    group.id = newId();
    group.name = project_.uniqueTrackName(QStringLiteral("%1 Group").arg(project_.tracks().size() + 1));
    group.color = project_.nextColor();
    group.kind = kGroupKind;
    group.parent = first.parent;
    const int index = project_.trackIndex(first.id);
    const QString text = QStringLiteral("Group Tracks");
    stack_.beginMacro(text);
    stack_.push(new InsertTrackCommand(&project_, group, index, text));
    arrange(arranged(project_, roots, project_.trackIndex(group.id) + 1, group.id), text);
    stack_.endMacro();
    return group.id;
}

void Edits::moveTracks(const QStringList& trackIds, int index, const std::optional<QString>& parent) {
    arrange(arranged(project_, trackIds, index, parent), QStringLiteral("Move Tracks"));
}

void Edits::deleteTracks(const QStringList& trackIds) {
    Project& p = project_;
    QSet<QString> doomed;
    QSet<QString> returns;
    for (const QString& id : trackIds) {
        if (p.hasTrack(id)) {
            doomed.insert(id);
            for (const Track* d : p.descendants(id)) doomed.insert(d->id);
        } else if (p.hasReturn(id)) {
            returns.insert(id);
        }
    }
    const QSet<QString> going = doomed + returns;
    const QString text = QStringLiteral("Delete Tracks");
    stack_.beginMacro(text);
    for (const Track& t : p.tracks()) {  // the inputs taken from them (first: undo brings them back after their sources)
        if (t.inputTrack && going.contains(*t.inputTrack)) {
            stack_.push(new UpdateTrackCommand(&p, t.id, TrackField::InputTrack, t.inputTrack, std::optional<QString>(),
                                               text));
        }
    }
    std::vector<std::tuple<QString, QString, Sidechain>> sidechains;
    for (const Track* t : p.allTracks()) {
        if (going.contains(t->id)) continue;
        for (const Device* device : iterDevices(t->devices)) {
            if (device->sidechain && going.contains(device->sidechain->trackId)) {
                sidechains.emplace_back(t->id, device->id, *device->sidechain);
            }
        }
    }
    for (const auto& [trackId, deviceId, sidechain] : sidechains) {
        stack_.push(new SetDeviceSidechainCommand(&p, trackId, deviceId, sidechain, std::nullopt, text));
    }
    QStringList order;  // the last first: undo brings back each group before what is in it
    for (auto it = p.tracks().rbegin(); it != p.tracks().rend(); ++it) {
        if (doomed.contains(it->id)) order.append(it->id);
    }
    for (const QString& id : order) stack_.push(new RemoveTrackCommand(&p, id, text));
    if (!returns.isEmpty()) {
        QStringList senders;
        for (const Track* t : p.senders()) senders.append(t->id);
        for (const QString& id : senders) {
            const Track& t = p.track(id);
            SendMap kept;
            for (auto it = t.sends.constBegin(); it != t.sends.constEnd(); ++it) {
                if (!returns.contains(it.key())) kept.insert(it.key(), it.value());
            }
            if (!returns.contains(id) && kept != t.sends) {
                stack_.push(new UpdateTrackCommand(&p, id, TrackField::Sends, t.sends, kept, text));
            }
            QStringList keys;
            for (const auto& entry : p.track(id).automation) {
                const auto returnId = automation::keySend(entry.first);
                if (returnId && returns.contains(*returnId)) keys.append(entry.first);
            }
            for (const QString& key : keys) {
                stack_.push(new SetEnvelopeCommand(&p, id, key, p.envelope(id, key), {}, text));
            }
        }
        QStringList gone;
        for (auto it = p.returns().rbegin(); it != p.returns().rend(); ++it) {
            if (returns.contains(it->id)) gone.append(it->id);
        }
        for (const QString& id : gone) stack_.push(new RemoveReturnCommand(&p, id, text));
    }
    stack_.endMacro();
}

QString Edits::addDevice(const QString& trackId, const QString& kind, const std::optional<PluginRef>& plugin,
                         std::optional<int> index) {
    const Device device = newDevice(kind, plugin);
    const std::vector<Device> before = project_.track(trackId).devices;
    std::vector<Device> after = before;
    if (deviceIsInstrument(device)) {  // first, replacing the one there
        std::vector<Device> kept;
        for (const Device& d : after) {
            if (!deviceIsInstrument(d)) kept.push_back(d);
        }
        kept.insert(kept.begin(), device);
        after = kept;
    } else {
        const int first = !after.empty() && deviceIsInstrument(after.front()) ? 1 : 0;
        const int at = index ? std::max(first, std::min(*index, static_cast<int>(after.size())))
                             : static_cast<int>(after.size());
        after.insert(after.begin() + at, device);
    }
    stack_.push(new SetDevicesCommand(&project_, trackId, before, after, QStringLiteral("Add Device")));
    return device.id;
}

void Edits::insertDevice(const QString& trackId, const Device& device) {
    const std::vector<Device> before = project_.track(trackId).devices;
    std::vector<Device> after = before;
    after.push_back(device);
    stack_.push(new SetDevicesCommand(&project_, trackId, before, after, QStringLiteral("Add Device")));
}

void Edits::removeDevices(const QString& trackId, const QStringList& deviceIds) {
    const std::vector<Device> before = project_.track(trackId).devices;
    std::vector<Device> after = before;
    for (const QString& id : deviceIds) {
        std::vector<Device>* chain = chainDevices(after, containerOf(after, id));
        if (chain == nullptr) continue;
        chain->erase(std::remove_if(chain->begin(), chain->end(), [&](const Device& d) { return d.id == id; }),
                     chain->end());
    }
    stack_.push(new SetDevicesCommand(&project_, trackId, before, after, QStringLiteral("Delete Device")));
}

void Edits::moveDevices(const QString& trackId, const QStringList& deviceIds, int index,
                        const std::optional<QString>& chain) {
    const std::vector<Device> before = project_.track(trackId).devices;
    std::vector<Device> after = before;
    std::vector<Device> moving;
    for (const Device* d : iterDevices(after)) {
        if (deviceIds.contains(d->id)) moving.push_back(*d);
    }
    const std::vector<Device>* target = chainDevices(after, chain);
    QSet<QString> inside;
    for (const Device& d : moving) inside.unite(deviceIdsOf(d));
    int at = 0;
    for (int i = 0; i < std::min(index, static_cast<int>(target->size())); ++i) {
        if (!inside.contains((*target)[static_cast<size_t>(i)].id)) ++at;
    }
    for (const Device& d : moving) {
        std::vector<Device>* from = chainDevices(after, containerOf(after, d.id));
        from->erase(std::remove_if(from->begin(), from->end(), [&](const Device& x) { return x.id == d.id; }),
                    from->end());
    }
    std::vector<Device>* into = chainDevices(after, chain);
    const int first = !into->empty() && deviceIsInstrument(into->front()) ? 1 : 0;  // nothing goes before an instrument
    at = std::max(first, std::min(at, static_cast<int>(into->size())));
    into->insert(into->begin() + at, moving.begin(), moving.end());
    stack_.push(new SetDevicesCommand(&project_, trackId, before, after, QStringLiteral("Move Devices")));
}

void Edits::moveDevicesToTrack(const QString& trackId, const QStringList& deviceIds, const QString& toTrackId) {
    std::vector<Device> source = project_.track(trackId).devices;
    std::vector<Device> target = project_.track(toTrackId).devices;
    std::vector<Device> moving;
    for (const Device* d : iterDevices(source)) {
        if (deviceIds.contains(d->id)) moving.push_back(*d);
    }
    QSet<QString> moved;
    for (Device& d : moving) {
        std::vector<Device>* from = chainDevices(source, containerOf(source, d.id));
        from->erase(std::remove_if(from->begin(), from->end(), [&](const Device& x) { return x.id == d.id; }),
                    from->end());
        moved.unite(deviceIdsOf(d));
    }
    for (Device& d : moving) {
        // A sidechain from where they go (or what that feeds) would close a cycle.
        std::vector<Device> one{d};
        for (Device* inner : iterDevices(one)) {
            if (inner->sidechain && project_.sidechainWouldCycle(toTrackId, inner->sidechain->trackId)) {
                inner->sidechain.reset();
            }
        }
        d = one.front();
    }
    target.insert(target.end(), moving.begin(), moving.end());
    DeviceLists before{{trackId, project_.track(trackId).devices}, {toTrackId, project_.track(toTrackId).devices}};
    DeviceLists after{{trackId, source}, {toTrackId, target}};
    QMap<LaneRef, Envelope> oldEnvelopes;
    QMap<LaneRef, Envelope> newEnvelopes;
    for (const auto& [key, points] : project_.automation(trackId)) {
        const auto deviceId = automation::keyDevice(key);
        if (!deviceId || !moved.contains(*deviceId)) continue;
        oldEnvelopes.insert({trackId, key}, points);
        oldEnvelopes.insert({toTrackId, key}, project_.envelope(toTrackId, key));
        newEnvelopes.insert({trackId, key}, {});
        newEnvelopes.insert({toTrackId, key}, points);
    }
    const QString text = QStringLiteral("Move Devices");
    stack_.beginMacro(text);
    stack_.push(new SetChainsCommand(&project_, before, after, text));
    if (!oldEnvelopes.isEmpty()) stack_.push(new SetEnvelopesCommand(&project_, oldEnvelopes, newEnvelopes, text));
    stack_.endMacro();
}

QString Edits::groupDevices(const QString& trackId, const QStringList& deviceIds) {
    const std::vector<Device> before = project_.track(trackId).devices;
    std::vector<Device> after = before;
    const std::optional<QString> chain = containerOf(after, deviceIds.first());
    std::vector<Device>* devices = chainDevices(after, chain);
    std::vector<Device> grouped;
    int at = -1;
    for (int i = 0; i < static_cast<int>(devices->size()); ++i) {
        if (!deviceIds.contains((*devices)[static_cast<size_t>(i)].id)) continue;
        if (at < 0) at = i;
        grouped.push_back((*devices)[static_cast<size_t>(i)]);
    }
    Device rack = newRack({newChain(deviceName(grouped.front()), grouped)});
    devices->erase(std::remove_if(devices->begin(), devices->end(),
                                  [&](const Device& d) { return deviceIds.contains(d.id); }),
                   devices->end());
    devices->insert(devices->begin() + at, rack);
    stack_.push(new SetDevicesCommand(&project_, trackId, before, after, QStringLiteral("Group Devices")));
    return rack.id;
}

QString Edits::addRackChain(const QString& trackId, const QString& rackId) {
    const std::vector<Device> before = project_.track(trackId).devices;
    std::vector<Device> after = before;
    Device* rack = findDevice(after, rackId);
    Chain chain = newChain(QStringLiteral("Chain %1").arg(rack->chains.size() + 1));
    rack->chains.push_back(chain);
    stack_.push(new SetDevicesCommand(&project_, trackId, before, after, QStringLiteral("Add Chain")));
    return chain.id;
}

void Edits::setChain(const QString& trackId, const QString& chainId, ChainField field, const ChainValue& value) {
    const ChainValue old = chainValue(project_.chain(trackId, chainId), field);
    stack_.push(new UpdateChainCommand(&project_, trackId, chainId, field, old, value, QStringLiteral("Change Chain")));
}

void Edits::setDeviceParam(const QString& trackId, const QString& deviceId, const QString& paramId, double value) {
    const Device& device = project_.device(trackId, deviceId);
    double old = device.params.value(paramId, value);
    if (!device.params.contains(paramId)) {
        if (const sub::ParamInfo* info = builtinParamInfo(device.kind, paramId)) old = info->defaultValue;
    }
    stack_.push(new SetDeviceParamCommand(&project_, trackId, deviceId, paramId, old, value));
}

void Edits::setDeviceParams(const QString& trackId, const QMap<DeviceParam, double>& values) {
    QMap<DeviceParam, double> old;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        const Device& device = project_.device(trackId, it.key().first);
        old.insert(it.key(), device.params.value(it.key().second, it.value()));
    }
    stack_.push(new SetDeviceParamsCommand(&project_, trackId, old, values, QStringLiteral("Change Parameters")));
}

void Edits::setDeviceSidechain(const QString& trackId, const QString& deviceId,
                               const std::optional<Sidechain>& sidechain) {
    stack_.push(new SetDeviceSidechainCommand(&project_, trackId, deviceId, project_.device(trackId, deviceId).sidechain,
                                              sidechain, QStringLiteral("Change Sidechain")));
}

void Edits::setDeviceState(const QString& trackId, const QString& deviceId, const std::optional<QString>& state) {
    stack_.push(new SetDeviceStateCommand(&project_, trackId, deviceId, project_.device(trackId, deviceId).state, state,
                                          QStringLiteral("Load Preset")));
}

void Edits::setMacros(const QString& trackId, const QString& rackId, const std::vector<MacroMapping>& macros) {
    stack_.push(new SetMacrosCommand(&project_, trackId, rackId, project_.device(trackId, rackId).macros, macros,
                                     QStringLiteral("Map Macro")));
}

void Edits::freeze(const QString& trackId, const Freeze& freeze) {
    if (project_.hasTrack(trackId) && project_.track(trackId).armed) setTrack(trackId, TrackField::Armed, false);
    stack_.push(new SetFreezeCommand(&project_, trackId, std::nullopt, freeze, QStringLiteral("Freeze Track")));
}

void Edits::unfreeze(const QString& trackId) {
    stack_.push(new SetFreezeCommand(&project_, trackId, project_.track(trackId).frozen, std::nullopt,
                                     QStringLiteral("Unfreeze Track")));
}

// --- Studio ------------------------------------------------------------------------------

Studio::Studio(bool clipFades) {
    if (!clipFades) engine.setClipFadeMs(0);
    bridge = std::make_unique<EngineBridge>(engine, &project);
}

Studio::~Studio() {
    bridge->shutdown();
    engine.closeDevice();
    bridge.reset();
}

std::vector<float> Studio::render(int64_t frames, double startBeat) {
    return engine.renderOffline(startBeat, frames);
}

float Studio::level(int64_t frames) { return render(frames).at(static_cast<size_t>(2 * (frames - 1))); }

QString Studio::clipTrack(const QString& path, double seconds, const QString& name) {
    engine.loadSource(path.toStdString());
    const QString trackId = edit.addAudioTrack(name);
    edit.setClips(trackId, {audioClip(trackId + QStringLiteral("c"), path, 0.0, seconds)});
    return trackId;
}

Clip audioClip(const QString& id, const QString& path, double startBeat, double seconds) {
    return Clip::audio(id, path, QFileInfo(path).completeBaseName(), startBeat, seconds, 0.0, seconds);
}

float peak(const std::vector<float>& samples, size_t from) {
    float most = 0.f;
    for (size_t i = from; i < samples.size(); ++i) most = std::max(most, std::abs(samples[i]));
    return most;
}

}  // namespace sub::app::test
