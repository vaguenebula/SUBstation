// Editing device chains: adding, inserting, copying, pasting, moving and
// removing devices, on a track or in a rack's chain.

#include "editor/EditorSupport.h"
#include "editor/ProjectEditor.h"

#include "model/Commands.h"
#include "model/Devices.h"

#include <QUndoStack>

#include <algorithm>

namespace sub::app {

using editing::indexOfDevice;
using editing::Macro;
using editing::optionalId;

namespace {

// Drop the macro mappings whose device isn't (any longer) inside its rack (a
// macro moves parameters of devices in its rack), or whose macro the rack no
// longer has.
void pruneMacros(std::vector<Device>& devices) {
    for (Device* rack : iterDevices(devices)) {
        if (rack->macros.empty()) continue;
        QSet<QString> inside = deviceIdsOf(*rack);
        inside.remove(rack->id);
        std::vector<MacroMapping> kept;
        for (const MacroMapping& m : rack->macros) {
            if (inside.contains(m.deviceId) && m.macro < macroCount(*rack)) kept.push_back(m);
        }
        if (kept != rack->macros) rack->macros = kept;
    }
}

// The racks' macros there are ((rack id, its parameter)).
QSet<DeviceParam> macrosOf(const std::vector<Device>& devices) {
    QSet<DeviceParam> macros;
    for (const Device* rack : iterDevices(devices)) {
        for (int i = 0; i < macroCount(*rack); ++i) macros.insert({rack->id, macroParam(i)});
    }
    return macros;
}

// The ids of the devices of these ids (not instruments), in their order on the
// track, but those in racks among them (they go along with their rack).
QStringList outermost(const std::vector<Device>& devices, const QStringList& deviceIds) {
    std::vector<const Device*> found;
    for (const Device* d : iterDevices(devices)) {
        if (deviceIds.contains(d->id) && !deviceIsInstrument(*d)) found.push_back(d);
    }
    QSet<QString> inside;
    for (const Device* d : found) {
        QSet<QString> ids = deviceIdsOf(*d);
        ids.remove(d->id);
        inside.unite(ids);
    }
    QStringList result;
    for (const Device* d : found) {
        if (!inside.contains(d->id)) result.append(d->id);
    }
    return result;
}

// Takes a device out of wherever it is in the tree; the device.
Device takeDevice(std::vector<Device>& devices, const QString& deviceId) {
    std::vector<Device>* container = chainDevices(devices, containerOf(devices, deviceId));
    const int at = indexOfDevice(*container, deviceId);
    Device device = std::move((*container)[at]);
    container->erase(container->begin() + at);
    return device;
}

bool startsWithInstrument(const std::vector<Device>& chain) { return !chain.empty() && deviceIsInstrument(chain.front()); }

}  // namespace

void ProjectEditor::setDeviceDefaults(DeviceDefaults defaults) { defaultDevice_ = std::move(defaults); }

Device ProjectEditor::newDeviceOf(const QString& kind, const std::optional<PluginRef>& plugin) const {
    // A new device of a kind: as its default preset has it, if there is one.
    if (defaultDevice_) {
        if (auto device = defaultDevice_(kind, plugin)) return *device;
    }
    return newDevice(kind, plugin);
}

QString ProjectEditor::addDevice(const QString& trackId, const QString& kind, int index, const QString& chain) {
    return addDevice(trackId, kind, index, std::nullopt, chain);
}

QString ProjectEditor::addDevice(const QString& trackId, const QString& kind, int index,
                                 const std::optional<PluginRef>& plugin, const QString& chain) {
    if (!project_->hasOwner(trackId)) return {};
    if (kind == kPluginKind && !plugin) return {};
    if (kind != kPluginKind && kind != kRackKind && builtinDevice(kind) == nullptr) return {};
    const Device device = newDeviceOf(kind, plugin);
    const QStringList added = insertDevices(trackId, {device}, index, chain,
                                            QStringLiteral("Add %1").arg(deviceName(device)));
    return added.isEmpty() ? QString() : added.front();
}

bool ProjectEditor::insertDevice(const QString& trackId, const Device& device, int index, const QString& chain,
                                 const QString& text, bool showEditors) {
    return !insertDevices(trackId, {device}, index, chain,
                          text.isEmpty() ? QStringLiteral("Add %1").arg(deviceName(device)) : text, showEditors)
                .isEmpty();
}

QStringList ProjectEditor::insertDevices(const QString& trackId, const std::vector<Device>& devices, int index,
                                         const QString& chain, const QString& text, bool showEditors) {
    const Track* track = project_->findTrack(trackId);
    if (track == nullptr) return {};
    const bool midi = track->isMidi();
    const std::vector<Device> before = track->devices;
    std::vector<Device> after = before;
    std::vector<Device>* target = chainDevices(after, optionalId(chain));
    if (target == nullptr) return {};
    const int depth = rackDepth(after, optionalId(chain));
    const int size = static_cast<int>(target->size());
    int at = index < 0 ? size : std::max(startsWithInstrument(*target) ? 1 : 0, std::min(index, size));  // effects go after the instrument
    std::vector<Device> added;
    for (const Device& device : devices) {
        if (depth + rackHeight(device) > kMaxRackDepth) continue;
        if (deviceIsInstrument(device)) {
            if (!midi) continue;
            std::vector<Device> kept;
            for (Device& d : *target) {
                if (!deviceIsInstrument(d)) kept.push_back(std::move(d));
            }
            // (One may have gone from before it; this one goes first.)
            at += static_cast<int>(kept.size()) - static_cast<int>(target->size()) + 1;
            kept.insert(kept.begin(), device);
            *target = std::move(kept);
        } else {
            at = std::max(startsWithInstrument(*target) ? 1 : 0, at);
            target->insert(target->begin() + at, device);
            ++at;
        }
        added.push_back(device);
    }
    if (added.empty()) return {};
    if (!setDevices(trackId, before, std::move(after), text)) return {};
    if (showEditors) {
        for (const Device* d : iterDevices(added)) {
            if (d->isPlugin()) Q_EMIT pluginAdded(trackId, d->id);
        }
    }
    QStringList ids;
    for (const Device& d : added) ids.append(d.id);
    return ids;
}

std::vector<Device> ProjectEditor::copyDevices(const QString& trackId, const QStringList& deviceIds) const {
    const Track* track = project_->findTrack(trackId);
    if (track == nullptr) return {};
    std::vector<const Device*> found;
    for (const Device* d : iterDevices(track->devices)) {
        if (deviceIds.contains(d->id)) found.push_back(d);
    }
    QSet<QString> inside;
    for (const Device* d : found) {
        QSet<QString> ids = deviceIdsOf(*d);
        ids.remove(d->id);
        inside.unite(ids);
    }
    std::vector<Device> copies;
    for (const Device* d : found) {
        if (!inside.contains(d->id)) copies.push_back(*d);
    }
    return copies;
}

QStringList ProjectEditor::pasteDevices(const QString& trackId, const std::vector<Device>& copied, int index,
                                        const QString& chain, const QSet<QString>& folded, const QString& text) {
    Project& p = *project_;
    std::vector<Device> devices;
    QSet<QString> foldedCopies;
    for (const Device& original : copied) {
        std::vector<Device> holder{original};
        QStringList oldIds;
        for (const Device* d : iterDevices(holder)) oldIds.append(d->id);
        refreshIds(holder.front());
        const std::vector<Device*> inner = iterDevices(holder);
        for (std::size_t i = 0; i < inner.size() && i < static_cast<std::size_t>(oldIds.size()); ++i) {
            if (folded.contains(oldIds[static_cast<qsizetype>(i)])) foldedCopies.insert(inner[i]->id);
            const auto& sidechain = inner[i]->sidechain;
            if (sidechain && (!p.hasOwner(sidechain->trackId) || p.sidechainWouldCycle(trackId, sidechain->trackId))) {
                inner[i]->sidechain.reset();
            }
        }
        devices.push_back(std::move(holder.front()));
    }
    p.addFoldedDevices(foldedCopies);
    QString label = text;
    if (label.isEmpty()) {
        label = devices.size() == 1 ? QStringLiteral("Paste %1").arg(deviceName(devices.front()))
                                    : QStringLiteral("Paste Devices");
    }
    return insertDevices(trackId, devices, index, chain, label, false);
}

void ProjectEditor::setDevicesFolded(const QString& trackId, const QStringList& deviceIds, bool folded) {
    if (project_->hasOwner(trackId)) project_->setDevicesFolded(trackId, QSet<QString>(deviceIds.begin(), deviceIds.end()), folded);
}

void ProjectEditor::setChainListShown(const QString& trackId, const QString& rackId, bool shown) {
    const Device* rack = project_->findDevice(trackId, rackId);
    if (rack != nullptr && rack->isRack()) project_->setChainListShown(trackId, rackId, shown);
}

void ProjectEditor::setRackDevicesShown(const QString& trackId, const QString& rackId, bool shown) {
    const Device* rack = project_->findDevice(trackId, rackId);
    if (rack != nullptr && rack->isRack()) project_->setRackDevicesShown(trackId, rackId, shown);
}

void ProjectEditor::moveDevice(const QString& trackId, const QString& deviceId, int index) {
    if (!project_->hasDevice(trackId, deviceId)) return;
    const auto& devices = project_->track(trackId).devices;
    const auto chain = containerOf(devices, deviceId);
    const int at = indexOfDevice(*chainDevices(devices, chain), deviceId);
    // moveDevices counts positions in the chain before the move: moving right skips the device itself.
    moveDevices(trackId, {deviceId}, index > at ? index + 1 : index, chain.value_or(QString()));
}

bool ProjectEditor::moveDevices(const QString& trackId, const QStringList& deviceIds, int index, const QString& chain) {
    const Track* track = project_->findTrack(trackId);
    if (track == nullptr) return false;
    const auto chainId = optionalId(chain);
    const std::vector<Device> before = track->devices;
    std::vector<Device> after = before;
    const QStringList moving = outermost(after, deviceIds);
    std::vector<Device>* target = chainDevices(after, chainId);
    if (moving.isEmpty() || target == nullptr) return false;
    QSet<QString> inside;
    for (const QString& id : moving) inside.unite(deviceIdsOf(*findDevice(after, id)));
    if (chainId && inside.contains(project_->chainRack(trackId, *chainId).id)) return false;  // into itself
    const int depth = rackDepth(after, chainId);
    for (const QString& id : moving) {
        if (depth + rackHeight(*findDevice(after, id)) > kMaxRackDepth) return false;
    }
    int at = 0;
    const int end = std::min(std::max(0, index), static_cast<int>(target->size()));
    for (int i = 0; i < end; ++i) {
        if (!inside.contains((*target)[i].id)) ++at;
    }
    std::vector<Device> moved;
    for (const QString& id : moving) moved.push_back(takeDevice(after, id));  // out of wherever they are
    target = chainDevices(after, chainId);  // (where it is now)
    const int first = startsWithInstrument(*target) ? 1 : 0;
    at = std::max(first, std::min(at, static_cast<int>(target->size())));
    target->insert(target->begin() + at, std::make_move_iterator(moved.begin()), std::make_move_iterator(moved.end()));
    pruneMacros(after);  // (a device out of its rack leaves its macros)
    if (after == before) return false;
    return push(std::make_unique<SetDevicesCommand>(project_, trackId, before, after,
                                                    moving.size() == 1 ? QStringLiteral("Move Device")
                                                                       : QStringLiteral("Move Devices")));
}

bool ProjectEditor::moveDevicesToTrack(const QString& trackId, const QStringList& deviceIds, const QString& toTrackId,
                                       int index, const QString& chain) {
    Project& p = *project_;
    if (!p.hasOwner(trackId) || !p.hasOwner(toTrackId)) return false;
    const auto chainId = optionalId(chain);
    if (toTrackId == trackId) {
        if (index < 0 && !chainId) return false;
        const std::vector<Device>* target = chainDevices(p.track(trackId).devices, chainId);
        return moveDevices(trackId, deviceIds, index < 0 ? (target ? static_cast<int>(target->size()) : 0) : index, chain);
    }
    std::vector<Device> source = p.track(trackId).devices;
    std::vector<Device> targetDevices = p.track(toTrackId).devices;
    const QStringList moving = outermost(source, deviceIds);
    if (moving.isEmpty() || chainDevices(targetDevices, chainId) == nullptr) return false;
    const int depth = rackDepth(targetDevices, chainId);
    for (const QString& id : moving) {
        if (depth + rackHeight(*findDevice(source, id)) > kMaxRackDepth) return false;
    }
    std::vector<Device> moved;
    for (const QString& id : moving) {
        Device device = takeDevice(source, id);
        std::vector<Device> holder{std::move(device)};
        for (Device* inner : iterDevices(holder)) {  // a sidechain from where they go (or what that feeds) would close a cycle
            if (inner->sidechain && p.sidechainWouldCycle(toTrackId, inner->sidechain->trackId)) inner->sidechain.reset();
        }
        moved.push_back(std::move(holder.front()));
    }
    std::vector<Device>* target = chainDevices(targetDevices, chainId);
    const int size = static_cast<int>(target->size());
    const int at = index < 0 ? size : std::max(startsWithInstrument(*target) ? 1 : 0, std::min(index, size));
    QSet<QString> movedIds;
    for (const Device& d : moved) movedIds.unite(deviceIdsOf(d));
    target->insert(target->begin() + at, std::make_move_iterator(moved.begin()), std::make_move_iterator(moved.end()));
    pruneMacros(source);
    pruneMacros(targetDevices);
    const DeviceLists before{{trackId, p.track(trackId).devices}, {toTrackId, p.track(toTrackId).devices}};
    const DeviceLists after{{trackId, source}, {toTrackId, targetDevices}};
    const QString text = moving.size() == 1 ? QStringLiteral("Move Device") : QStringLiteral("Move Devices");
    std::vector<std::pair<QString, Envelope>> envelopes;
    for (const auto& [key, points] : p.automation(trackId)) {
        const auto device = automation::keyDevice(key);
        if (device && movedIds.contains(*device)) envelopes.emplace_back(key, points);
    }
    // The outputs going into them that would close a cycle where they go (that
    // track, or one it feeds, takes what they put out) go into their groups.
    QSet<QString> cycling;
    for (const Track* t : p.senders()) {
        if (t->output.to == Output::To::Sidechain && movedIds.contains(t->output.id) &&
            p.outputWouldCycle(t->id, Output::track(toTrackId))) {
            cycling.insert(t->output.id);
        }
    }
    auto command = std::make_unique<SetChainsCommand>(project_, before, after, text);
    if (envelopes.empty() && cycling.isEmpty()) return push(std::move(command));
    if (const auto problem = frozenProblem(*command)) {
        Q_EMIT refused(*problem);
        return false;
    }
    Macro macro(undoStack_, text);
    dropOutputs({}, cycling, text);
    push(std::move(command));
    if (envelopes.empty()) return true;
    QMap<LaneRef, Envelope> old;
    QMap<LaneRef, Envelope> nw;
    for (const auto& [key, points] : envelopes) {
        for (const QString& owner : {trackId, toTrackId}) old.insert({owner, key}, p.envelope(owner, key));
        nw.insert({trackId, key}, Envelope{});
        nw.insert({toTrackId, key}, points);
    }
    push(std::make_unique<SetEnvelopesCommand>(project_, old, nw, text));
    return true;
}

void ProjectEditor::removeDevice(const QString& trackId, const QString& deviceId) { removeDevices(trackId, {deviceId}); }

void ProjectEditor::removeDevices(const QString& trackId, const QStringList& deviceIds) {
    const Track* track = project_->findTrack(trackId);
    if (track == nullptr) return;
    const QSet<QString> ids(deviceIds.begin(), deviceIds.end());
    const std::vector<Device> before = track->devices;
    std::vector<Device> after = before;
    const std::function<void(std::vector<Device>&)> prune = [&](std::vector<Device>& devices) {
        devices.erase(std::remove_if(devices.begin(), devices.end(), [&](const Device& d) { return ids.contains(d.id); }),
                      devices.end());
        for (Device& device : devices) {
            for (Chain& chain : device.chains) prune(chain.devices);
        }
    };
    prune(after);
    if (after == before) return;
    const auto removed = deviceIdsOfList(before).size() - deviceIdsOfList(after).size();
    setDevices(trackId, before, std::move(after),
               removed == 1 ? QStringLiteral("Delete Device") : QStringLiteral("Delete Devices"));
}

bool ProjectEditor::setDevices(const QString& trackId, const std::vector<Device>& before, std::vector<Device> after,
                               const QString& text) {
    // Change a track's devices; the automation of devices that leave it goes
    // with them (their parameters', and a rack's chains' faders'), and so do
    // the mappings of macros to them (in the same undo step). So do the
    // mappings and automation of macros a rack no longer has.
    if (const auto problem = frozenProblem(SetDevicesCommand(project_, trackId, before, after, text))) {
        Q_EMIT refused(*problem);
        return false;
    }
    const QSet<QString> gone = deviceIdsOfList(before) - deviceIdsOfList(after);
    QSet<QString> chainsGone;
    for (const ConstRackChain& rc : iterChains(before)) chainsGone.insert(rc.chain->id);
    for (const ConstRackChain& rc : iterChains(std::as_const(after))) chainsGone.remove(rc.chain->id);
    const QSet<DeviceParam> macrosGone = macrosOf(before) - macrosOf(after);
    pruneMacros(after);
    QStringList orphans;
    for (const auto& [key, points] : project_->track(trackId).automation) {
        const auto device = automation::keyDevice(key);
        const auto chain = automation::keyChain(key);
        const auto target = automation::parseKey(key);
        const bool macroGone = device && target && macrosGone.contains({*device, target->param});
        if ((device && gone.contains(*device)) || (chain && chainsGone.contains(*chain)) || macroGone) {
            orphans.append(key);
        }
    }
    const std::vector<const Track*> senders = project_->senders();
    const bool into = std::any_of(senders.begin(), senders.end(), [&](const Track* t) {
        return t->output.to == Output::To::Sidechain && gone.contains(t->output.id);
    });
    if (orphans.isEmpty() && !into) {
        return push(std::make_unique<SetDevicesCommand>(project_, trackId, before, after, text));
    }
    Macro macro(undoStack_, text);
    dropOutputs({}, gone, text);  // (the outputs into them go into their groups)
    push(std::make_unique<SetDevicesCommand>(project_, trackId, before, after, text));
    for (const QString& key : orphans) {
        push(std::make_unique<SetEnvelopeCommand>(project_, trackId, key, project_->envelope(trackId, key), Envelope{},
                                                  text));
    }
    return true;
}

}  // namespace sub::app
