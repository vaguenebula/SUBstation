// Editing racks: grouping devices into one and ungrouping it, its chains
// (adding, removing, duplicating, moving, renaming, their mixers), and its
// macros (turning them, how many there are, their names, mapping them to
// parameters and over which range).

#include "editor/EditorSupport.h"
#include "editor/ProjectEditor.h"

#include "model/Commands.h"
#include "model/Devices.h"
#include "model/Errors.h"

#include <QUndoStack>

#include <algorithm>
#include <functional>
#include <set>

namespace sub::app {

using editing::indexOfDevice;
using editing::optionalId;

namespace {

// The rack holding a chain of this id, and the chain's index in it; null: none.
std::pair<Device*, int> rackOfChain(std::vector<Device>& devices, const QString& chainId) {
    for (const RackChain& rc : iterChains(devices)) {
        if (rc.chain->id != chainId) continue;
        for (int i = 0; i < static_cast<int>(rc.rack->chains.size()); ++i) {
            if (rc.rack->chains[i].id == chainId) return {rc.rack, i};
        }
    }
    return {nullptr, -1};
}

// A rack chain of this id; null: none.
const Chain* findChain(const std::vector<Device>& devices, const QString& chainId) {
    for (const ConstRackChain& rc : iterChains(devices)) {
        if (rc.chain->id == chainId) return rc.chain;
    }
    return nullptr;
}

}  // namespace

QString ProjectEditor::groupDevices(const QString& trackId, const QStringList& deviceIds) {
    const Track* track = project_->findTrack(trackId);
    if (track == nullptr) return {};
    QSet<QString> wanted;
    for (const QString& id : deviceIds) {
        if (findDevice(track->devices, id) != nullptr) wanted.insert(id);
    }
    std::set<std::optional<QString>> containers;
    std::optional<QString> chain;
    for (const QString& id : wanted) {
        chain = containerOf(track->devices, id);
        containers.insert(chain);
    }
    if (containers.size() != 1) return {};
    const std::vector<Device> before = track->devices;
    std::vector<Device> after = before;
    std::vector<Device>& devices = *chainDevices(after, chain);
    std::vector<Device> grouped;
    int height = 0;
    for (const Device& d : devices) {
        if (!wanted.contains(d.id)) continue;
        grouped.push_back(d);
        height = std::max(height, rackHeight(d));
    }
    if (rackDepth(after, chain) + 1 + height > kMaxRackDepth) return {};
    const int at = indexOfDevice(devices, grouped.front().id);
    const QString name = deviceName(grouped.front());
    Device rack = newRack({newChain(name, std::move(grouped))});
    const QString id = rack.id;
    devices.erase(std::remove_if(devices.begin(), devices.end(), [&](const Device& d) { return wanted.contains(d.id); }),
                  devices.end());
    devices.insert(devices.begin() + at, std::move(rack));
    push(std::make_unique<SetDevicesCommand>(project_, trackId, before, after, QStringLiteral("Group Devices")));
    return project_->hasDevice(trackId, id) ? id : QString();
}

bool ProjectEditor::ungroupRack(const QString& trackId, const QString& rackId) {
    const Track* track = project_->findTrack(trackId);
    if (track == nullptr) return false;
    const Device* rack = findDevice(track->devices, rackId);
    if (rack == nullptr || !rack->isRack()) return false;
    const std::vector<Device> before = track->devices;
    std::vector<Device> after = before;
    std::vector<Device>& devices = *chainDevices(after, containerOf(after, rackId));
    const int at = indexOfDevice(devices, rackId);
    std::vector<Device> inner;
    for (const Chain& chain : devices[at].chains) inner.insert(inner.end(), chain.devices.begin(), chain.devices.end());
    devices.erase(devices.begin() + at);
    devices.insert(devices.begin() + at, inner.begin(), inner.end());
    if (std::count_if(devices.begin(), devices.end(), [](const Device& d) { return deviceIsInstrument(d); }) > 1) {
        return false;
    }
    const auto instrument = std::find_if(devices.begin(), devices.end(), [](const Device& d) { return deviceIsInstrument(d); });
    if (instrument != devices.end() && instrument != devices.begin()) {
        Device moved = std::move(*instrument);
        devices.erase(instrument);
        devices.insert(devices.begin(), std::move(moved));
    }
    setDevices(trackId, before, std::move(after), QStringLiteral("Ungroup Rack"));
    return true;
}

QString ProjectEditor::addRackChain(const QString& trackId, const QString& rackId, int index, const QString& name) {
    const std::vector<Device> before = project_->track(trackId).devices;
    std::vector<Device> after = before;
    Device* rack = findDevice(after, rackId);
    if (rack == nullptr || !rack->isRack()) throw EditError(QStringLiteral("Chains belong to racks"));
    const int count = static_cast<int>(rack->chains.size());
    Chain chain = newChain(name.isEmpty() ? QStringLiteral("Chain %1").arg(count + 1) : name);
    const QString id = chain.id;
    rack->chains.insert(rack->chains.begin() + editing::clampIndex(index, count), std::move(chain));
    push(std::make_unique<SetDevicesCommand>(project_, trackId, before, after, QStringLiteral("Add Chain")));
    for (const ConstRackChain& rc : iterChains(project_->track(trackId).devices)) {
        if (rc.chain->id == id) return id;
    }
    return {};
}

void ProjectEditor::removeRackChains(const QString& trackId, const QStringList& chainIds) {
    const Track* track = project_->findTrack(trackId);
    if (track == nullptr) return;
    const QSet<QString> ids(chainIds.begin(), chainIds.end());
    const std::vector<Device> before = track->devices;
    std::vector<Device> after = before;
    const std::function<void(std::vector<Device>&)> prune = [&](std::vector<Device>& devices) {
        for (Device& device : devices) {
            auto& chains = device.chains;
            chains.erase(std::remove_if(chains.begin(), chains.end(), [&](const Chain& c) { return ids.contains(c.id); }),
                         chains.end());
            for (Chain& chain : chains) prune(chain.devices);
        }
    };
    prune(after);
    if (after != before) {
        setDevices(trackId, before, std::move(after),
                   ids.size() == 1 ? QStringLiteral("Delete Chain") : QStringLiteral("Delete Chains"));
    }
}

QString ProjectEditor::duplicateRackChain(const QString& trackId, const QString& chainId) {
    const Track* track = project_->findTrack(trackId);
    if (track == nullptr) return {};
    const std::vector<Device> before = track->devices;
    std::vector<Device> after = before;
    const auto [rack, index] = rackOfChain(after, chainId);
    if (rack == nullptr) return {};
    Device holder = newRack({rack->chains[index]});
    refreshIds(holder);
    const QString id = holder.chains.front().id;
    rack->chains.insert(rack->chains.begin() + index + 1, std::move(holder.chains.front()));
    push(std::make_unique<SetDevicesCommand>(project_, trackId, before, after, QStringLiteral("Duplicate Chain")));
    for (const ConstRackChain& rc : iterChains(project_->track(trackId).devices)) {
        if (rc.chain->id == id) return id;
    }
    return {};
}

void ProjectEditor::moveRackChain(const QString& trackId, const QString& chainId, int index) {
    const Track* track = project_->findTrack(trackId);
    if (track == nullptr) return;
    const std::vector<Device> before = track->devices;
    std::vector<Device> after = before;
    const auto [rack, at] = rackOfChain(after, chainId);
    if (rack == nullptr) return;
    Chain chain = std::move(rack->chains[at]);
    rack->chains.erase(rack->chains.begin() + at);
    const int count = static_cast<int>(rack->chains.size());
    rack->chains.insert(rack->chains.begin() + std::clamp(index, 0, count), std::move(chain));
    if (after != before) {
        push(std::make_unique<SetDevicesCommand>(project_, trackId, before, after, QStringLiteral("Move Chain")));
    }
}

void ProjectEditor::renameChain(const QString& trackId, const QString& chainId, const QString& name) {
    const Track* track = project_->findTrack(trackId);
    const Chain* chain = track != nullptr ? findChain(track->devices, chainId) : nullptr;
    if (chain == nullptr) return;
    const QString old = chain->name;
    if (!name.isEmpty() && name != old) {
        push(std::make_unique<UpdateChainCommand>(project_, trackId, chainId, ChainField::Name, old, name,
                                                  QStringLiteral("Rename Chain")));
    }
}

void ProjectEditor::setChainParam(const QString& trackId, const QString& chainId, ChainField field, double value,
                                  const QString& mergeKey) {
    const Chain& chain = project_->chain(trackId, chainId);
    if (field == ChainField::Solo) {
        if (chain.solo != (value != 0.0)) project_->updateChain(trackId, chainId, ChainField::Solo, value != 0.0);
        return;
    }
    QString label;
    switch (field) {
    case ChainField::VolumeDb:
        label = QStringLiteral("Change Chain Volume");
        value = std::clamp(value, automation::kMinVolumeDb, automation::kMaxVolumeDb);
        break;
    case ChainField::Pan:
        label = QStringLiteral("Change Chain Pan");
        value = std::clamp(value, -1.0, 1.0);
        break;
    case ChainField::Mute: label = QStringLiteral("Toggle Chain Activator"); break;
    default: return;  // (a chain's name: renameChain)
    }
    const ChainValue old = chainValue(chain, field);
    const ChainValue nw = field == ChainField::Mute ? ChainValue(value != 0.0) : ChainValue(value);
    if (nw != old) push(std::make_unique<UpdateChainCommand>(project_, trackId, chainId, field, old, nw, label, mergeKey));
    if (field == ChainField::VolumeDb || field == ChainField::Pan) {
        const QString& rack = project_->chainRack(trackId, chainId).id;
        Q_EMIT parameterTouched(
            trackId, automation::chainKey(rack, chainId,
                                          field == ChainField::VolumeDb ? automation::kChainVolume : automation::kChainPan));
    }
}

void ProjectEditor::setChainParam(const QString& trackId, const QString& chainId, const QString& field, double value,
                                  const QString& mergeKey) {
    const auto known = chainFieldFromName(field);
    const Track* track = project_->findTrack(trackId);
    if (!known || *known == ChainField::Name || track == nullptr || findChain(track->devices, chainId) == nullptr) return;
    setChainParam(trackId, chainId, *known, value, mergeKey);
}

// --- Macros ---

void ProjectEditor::setParamInfo(ParamInfoSource describe) { describe_ = std::move(describe); }

void ProjectEditor::setOwnValue(OwnValueSource read) { readOwn_ = std::move(read); }

std::optional<ParamSpec> ProjectEditor::paramInfo(const QString& trackId, const QString& deviceId,
                                                  const QString& paramId) const {
    const Device& device = project_->device(trackId, deviceId);
    if (!device.isPlugin() && !device.isRack()) {
        const sub::ParamInfo* info = builtinParamInfo(device.kind, paramId);
        if (info == nullptr) return std::nullopt;
        return ParamSpec::fromInfo(*info, automation::deviceKey(deviceId, paramId), deviceName(device));
    }
    return describe_ ? describe_(trackId, deviceId, paramId) : std::nullopt;
}

QMap<DeviceParam, double> ProjectEditor::macroTargets(const QString& trackId, const QString& rackId, int index,
                                                      double value) const {
    const Device& rack = project_->device(trackId, rackId);
    value = std::clamp(value, 0.0, 1.0);
    QMap<DeviceParam, double> values{{{rackId, macroParam(index)}, value}};
    for (const MacroMapping& mapping : rack.macros) {
        if (mapping.macro != index || !project_->hasDevice(trackId, mapping.deviceId)) continue;
        if (const auto info = paramInfo(trackId, mapping.deviceId, mapping.paramId)) {
            values.insert({mapping.deviceId, mapping.paramId}, info->fromNormalized(mapping.target(value)));
        }
    }
    return values;
}

double ProjectEditor::ownParamValue(const QString& trackId, const QString& deviceId, const QString& paramId,
                                    double fallback) const {
    const auto& params = project_->device(trackId, deviceId).params;
    if (params.contains(paramId)) return params.value(paramId);
    if (readOwn_) {  // set in a plug-in's own editor
        if (const auto own = readOwn_(trackId, automation::deviceKey(deviceId, paramId))) return *own;
    }
    const auto info = paramInfo(trackId, deviceId, paramId);  // a default value: as it was
    return info ? info->defaultValue : fallback;
}

void ProjectEditor::setMacro(const QString& trackId, const QString& rackId, int index, double value,
                             const QString& mergeKey) {
    const Device* rack = project_->findDevice(trackId, rackId);
    if (rack == nullptr || !rack->isRack() || index < 0 || index >= macroCount(*rack)) return;
    const QMap<DeviceParam, double> nw = macroTargets(trackId, rackId, index, value);
    QMap<DeviceParam, double> old;
    for (auto it = nw.constBegin(); it != nw.constEnd(); ++it) {
        old.insert(it.key(), ownParamValue(trackId, it.key().first, it.key().second, it.value()));
    }
    if (old != nw) {
        push(std::make_unique<SetDeviceParamsCommand>(project_, trackId, old, nw, QStringLiteral("Change Macro"), mergeKey));
    }
}

void ProjectEditor::setMacroCount(const QString& trackId, const QString& rackId, int count) {
    const Track* track = project_->findTrack(trackId);
    const Device* rack = track != nullptr ? findDevice(track->devices, rackId) : nullptr;
    if (rack == nullptr || !rack->isRack()) return;
    count = std::clamp(count, 1, kMaxMacroCount);
    const int had = macroCount(*rack);
    if (count == had) return;
    const std::vector<Device> before = track->devices;
    std::vector<Device> after = before;
    Device& changed = *findDevice(after, rackId);
    changed.macroNames.resize(static_cast<size_t>(count));
    for (int i = count; i < had; ++i) changed.params.remove(macroParam(i));  // (its mappings and automation: setDevices)
    for (int i = had; i < count; ++i) changed.params.insert(macroParam(i), 0.0);
    setDevices(trackId, before, std::move(after),
               count > had ? QStringLiteral("Add Macro") : QStringLiteral("Remove Macro"));
}

void ProjectEditor::renameMacro(const QString& trackId, const QString& rackId, int index, const QString& name) {
    const Track* track = project_->findTrack(trackId);
    const Device* rack = track != nullptr ? findDevice(track->devices, rackId) : nullptr;
    if (rack == nullptr || index < 0 || index >= macroCount(*rack)) return;
    QString given = name.trimmed();
    if (given == QStringLiteral("Macro %1").arg(index + 1)) given.clear();  // named by its number
    if (given == rack->macroNames[static_cast<size_t>(index)]) return;
    const std::vector<Device> before = track->devices;
    std::vector<Device> after = before;
    findDevice(after, rackId)->macroNames[static_cast<size_t>(index)] = given;
    push(std::make_unique<SetDevicesCommand>(project_, trackId, before, after, QStringLiteral("Rename Macro")));
}

void ProjectEditor::mapMacro(const QString& trackId, const QString& rackId, int index, const QString& deviceId,
                             const QString& paramId, double low, double high) {
    const Device& rack = project_->device(trackId, rackId);
    if (!rack.isRack() || index < 0 || index >= macroCount(rack) || deviceId == rackId ||
        !deviceIdsOf(rack).contains(deviceId)) {
        throw EditError(QStringLiteral("A macro moves parameters of devices in its rack"));
    }
    std::vector<MacroMapping> nw;
    for (const MacroMapping& m : rack.macros) {
        if (m.deviceId != deviceId || m.paramId != paramId) nw.push_back(m);
    }
    nw.push_back(MacroMapping{index, deviceId, paramId, low, high});
    push(std::make_unique<SetMacrosCommand>(project_, trackId, rackId, rack.macros, nw, QStringLiteral("Map Macro")));
}

void ProjectEditor::setMacroRange(const QString& trackId, const QString& rackId, const QString& deviceId,
                                  const QString& paramId, double low, double high, const QString& mergeKey) {
    const Device* rack = project_->findDevice(trackId, rackId);
    if (rack == nullptr || !rack->isRack()) return;
    std::vector<MacroMapping> nw = rack->macros;
    const auto mapping = std::find_if(nw.begin(), nw.end(), [&](const MacroMapping& m) {
        return m.deviceId == deviceId && m.paramId == paramId;
    });
    if (mapping == nw.end()) return;
    mapping->low = std::clamp(low, 0.0, 1.0);
    mapping->high = std::clamp(high, 0.0, 1.0);
    if (nw == rack->macros) return;
    // The parameter goes where its macro puts it in the new range.
    QMap<DeviceParam, double> oldValues, newValues;
    if (project_->hasDevice(trackId, deviceId)) {
        if (const auto info = paramInfo(trackId, deviceId, paramId)) {
            const double value = info->fromNormalized(mapping->target(rack->params.value(macroParam(mapping->macro))));
            newValues.insert({deviceId, paramId}, value);
            oldValues.insert({deviceId, paramId}, ownParamValue(trackId, deviceId, paramId, value));
        }
    }
    push(std::make_unique<SetMacrosCommand>(project_, trackId, rackId, rack->macros, nw,
                                            QStringLiteral("Change Macro Range"), mergeKey, oldValues, newValues));
}

void ProjectEditor::unmapMacro(const QString& trackId, const QString& rackId, const QString& deviceId,
                               const QString& paramId) {
    const Device* rack = project_->findDevice(trackId, rackId);
    if (rack == nullptr) return;
    std::vector<MacroMapping> kept;
    for (const MacroMapping& m : rack->macros) {
        if (m.deviceId != deviceId || m.paramId != paramId) kept.push_back(m);
    }
    if (kept != rack->macros) {
        push(std::make_unique<SetMacrosCommand>(project_, trackId, rackId, rack->macros, kept,
                                                QStringLiteral("Remove Macro Mapping")));
    }
}

std::optional<std::pair<QString, int>> ProjectEditor::macroOf(const QString& trackId, const QString& deviceId,
                                                              const QString& paramId) const {
    const auto& devices = project_->track(trackId).devices;
    auto chain = containerOf(devices, deviceId);
    while (chain) {
        const Device& rack = project_->chainRack(trackId, *chain);
        for (const MacroMapping& mapping : rack.macros) {
            if (mapping.deviceId == deviceId && mapping.paramId == paramId) return std::make_pair(rack.id, mapping.macro);
        }
        chain = containerOf(devices, rack.id);
    }
    return std::nullopt;
}

}  // namespace sub::app
