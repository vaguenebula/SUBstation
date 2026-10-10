#include "model/Device.h"

#include "model/Ids.h"

#include <QHash>

#include <algorithm>

namespace sub::app {

namespace {

template <typename Devices, typename Out>
void collectDevices(Devices& devices, Out& out) {
    for (auto& device : devices) {
        out.push_back(&device);
        for (auto& chain : device.chains) collectDevices(chain.devices, out);
    }
}

template <typename Devices, typename Out>
void collectChains(Devices& devices, Out& out) {
    for (auto& device : devices) {
        for (auto& chain : device.chains) {
            out.push_back({&device, &chain});
            collectChains(chain.devices, out);
        }
    }
}

// The first rack chain of this id in the order collectChains() has them.
template <typename Devices, typename Found>
Found searchChain(Devices& devices, const QString& chainId) {
    for (auto& device : devices) {
        for (auto& chain : device.chains) {
            if (chain.id == chainId) return {&device, &chain};
            const Found inner = searchChain<Devices, Found>(chain.devices, chainId);
            if (inner.chain != nullptr) return inner;
        }
    }
    return {nullptr, nullptr};
}

}  // namespace

QString macroParam(int index) { return QStringLiteral("macro%1").arg(index + 1); }

std::optional<int> macroIndex(const QString& paramId) {
    if (!paramId.startsWith(QLatin1String("macro"))) return std::nullopt;
    bool ok = false;
    const int number = paramId.mid(5).toInt(&ok);
    if (!ok || number < 1 || number > kMaxMacroCount || macroParam(number - 1) != paramId) return std::nullopt;
    return number - 1;
}

int macroCount(const Device& rack) { return rack.isRack() ? static_cast<int>(rack.macroNames.size()) : 0; }

QString macroName(const Device& rack, int index) {
    const QString given = index >= 0 && index < macroCount(rack) ? rack.macroNames[static_cast<size_t>(index)] : QString();
    return given.isEmpty() ? QStringLiteral("Macro %1").arg(index + 1) : given;
}

std::optional<QString> Sidechain::tapDevice() const {
    if (tap == kPostFader || tap == kPreFader || tap == kPreFx) return std::nullopt;
    return tap;
}

double MacroMapping::target(double value) const { return low + std::max(0.0, std::min(1.0, value)) * (high - low); }

bool Device::operator==(const Device& other) const {
    return id == other.id && kind == other.kind && enabled == other.enabled && params == other.params &&
           plugin == other.plugin && state == other.state && sidechain == other.sidechain && chains == other.chains &&
           macros == other.macros && macroNames == other.macroNames && name == other.name;
}

bool Chain::operator==(const Chain& other) const {
    return id == other.id && name == other.name && devices == other.devices && volumeDb == other.volumeDb &&
           pan == other.pan && mute == other.mute && solo == other.solo;
}

std::vector<Device*> iterDevices(std::vector<Device>& devices) {
    std::vector<Device*> out;
    collectDevices(devices, out);
    return out;
}

std::vector<const Device*> iterDevices(const std::vector<Device>& devices) {
    std::vector<const Device*> out;
    collectDevices(devices, out);
    return out;
}

std::vector<RackChain> iterChains(std::vector<Device>& devices) {
    std::vector<RackChain> out;
    collectChains(devices, out);
    return out;
}

std::vector<ConstRackChain> iterChains(const std::vector<Device>& devices) {
    std::vector<ConstRackChain> out;
    collectChains(devices, out);
    return out;
}

std::optional<std::vector<int>> devicePath(const std::vector<Device>& devices, const QString& deviceId) {
    for (int index = 0; index < static_cast<int>(devices.size()); ++index) {
        const Device& device = devices[index];
        if (device.id == deviceId) return std::vector<int>{index};
        for (int c = 0; c < static_cast<int>(device.chains.size()); ++c) {
            if (auto inner = devicePath(device.chains[c].devices, deviceId)) {
                std::vector<int> path{index, c};
                path.insert(path.end(), inner->begin(), inner->end());
                return path;
            }
        }
    }
    return std::nullopt;
}

Device& deviceAt(std::vector<Device>& devices, const std::vector<int>& path) {
    Device* device = &devices.at(path.at(0));
    for (size_t i = 1; i + 1 < path.size(); i += 2) device = &device->chains.at(path[i]).devices.at(path[i + 1]);
    return *device;
}

const Device& deviceAt(const std::vector<Device>& devices, const std::vector<int>& path) {
    const Device* device = &devices.at(path.at(0));
    for (size_t i = 1; i + 1 < path.size(); i += 2) device = &device->chains.at(path[i]).devices.at(path[i + 1]);
    return *device;
}

Device* findDevice(std::vector<Device>& devices, const QString& deviceId) {
    const auto path = devicePath(devices, deviceId);
    return path ? &deviceAt(devices, *path) : nullptr;
}

const Device* findDevice(const std::vector<Device>& devices, const QString& deviceId) {
    const auto path = devicePath(devices, deviceId);
    return path ? &deviceAt(devices, *path) : nullptr;
}

RackChain findChain(std::vector<Device>& devices, const QString& chainId) {
    return searchChain<std::vector<Device>, RackChain>(devices, chainId);
}

ConstRackChain findChain(const std::vector<Device>& devices, const QString& chainId) {
    return searchChain<const std::vector<Device>, ConstRackChain>(devices, chainId);
}

int chainIndex(const Device& rack, const QString& chainId) {
    for (int i = 0; i < static_cast<int>(rack.chains.size()); ++i) {
        if (rack.chains[i].id == chainId) return i;
    }
    return -1;
}

std::vector<Device>* chainDevices(std::vector<Device>& devices, const std::optional<QString>& chain) {
    if (!chain) return &devices;
    Chain* found = findChain(devices, *chain).chain;
    return found != nullptr ? &found->devices : nullptr;
}

const std::vector<Device>* chainDevices(const std::vector<Device>& devices, const std::optional<QString>& chain) {
    if (!chain) return &devices;
    const Chain* found = findChain(devices, *chain).chain;
    return found != nullptr ? &found->devices : nullptr;
}

std::optional<QString> containerOf(const std::vector<Device>& devices, const QString& deviceId) {
    const auto path = devicePath(devices, deviceId);
    if (!path || path->size() == 1) return std::nullopt;
    const std::vector<int> parentPath(path->begin(), path->end() - 2);
    const Device& parent = deviceAt(devices, parentPath);
    return parent.chains.at((*path)[path->size() - 2]).id;
}

int rackDepth(const std::vector<Device>& devices, const std::optional<QString>& chain) {
    if (!chain) return 0;
    const Device* rack = findChain(devices, *chain).rack;
    return rack != nullptr ? 1 + rackDepth(devices, containerOf(devices, rack->id)) : 0;
}

void refreshIds(Device& device) {
    std::vector<Device> holder;
    holder.push_back(std::move(device));
    QHash<QString, QString> renamed;
    for (Device* inner : iterDevices(holder)) {
        const QString fresh = newId();
        renamed.insert(inner->id, fresh);
        inner->id = fresh;
    }
    for (const RackChain& rc : iterChains(holder)) rc.chain->id = newId();
    for (Device* rack : iterDevices(holder)) {
        std::vector<MacroMapping> kept;
        for (const MacroMapping& m : rack->macros) {
            const auto found = renamed.constFind(m.deviceId);
            if (found != renamed.constEnd()) kept.push_back({m.macro, *found, m.paramId, m.low, m.high});
        }
        rack->macros = std::move(kept);
    }
    device = std::move(holder.front());
}

int rackHeight(const Device& device) {
    if (!device.isRack()) return 0;
    int deepest = 0;
    for (const Chain& c : device.chains) {
        for (const Device& d : c.devices) deepest = std::max(deepest, rackHeight(d));
    }
    return 1 + deepest;
}

}  // namespace sub::app
