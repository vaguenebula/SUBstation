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

}  // namespace

QString macroParam(int index) { return QStringLiteral("macro%1").arg(index + 1); }

std::optional<QString> Sidechain::tapDevice() const {
    if (tap == kPostFader || tap == kPreFader || tap == kPreFx) return std::nullopt;
    return tap;
}

double MacroMapping::target(double value) const { return low + std::max(0.0, std::min(1.0, value)) * (high - low); }

bool Device::operator==(const Device& other) const {
    return id == other.id && kind == other.kind && enabled == other.enabled && params == other.params &&
           plugin == other.plugin && state == other.state && sidechain == other.sidechain && chains == other.chains &&
           macros == other.macros && name == other.name;
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

std::vector<Device>* chainDevices(std::vector<Device>& devices, const std::optional<QString>& chain) {
    if (!chain) return &devices;
    for (const RackChain& rc : iterChains(devices)) {
        if (rc.chain->id == *chain) return &rc.chain->devices;
    }
    return nullptr;
}

const std::vector<Device>* chainDevices(const std::vector<Device>& devices, const std::optional<QString>& chain) {
    if (!chain) return &devices;
    for (const ConstRackChain& rc : iterChains(devices)) {
        if (rc.chain->id == *chain) return &rc.chain->devices;
    }
    return nullptr;
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
    for (const ConstRackChain& rc : iterChains(devices)) {
        if (rc.chain->id == *chain) return 1 + rackDepth(devices, containerOf(devices, rc.rack->id));
    }
    return 0;
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
