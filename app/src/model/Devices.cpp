#include "model/Devices.h"

#include "model/Errors.h"
#include "model/Ids.h"

#include <algorithm>

namespace sub::app {

const std::vector<BuiltinDevice>& builtinDevices() {
    static const std::vector<BuiltinDevice> devices = [] {
        std::vector<BuiltinDevice> list;
        for (const sub::BuiltinInfo& info : sub::BuiltinRegistry::instance().devices()) {
            BuiltinDevice device;
            device.kind = QString::fromStdString(info.id);
            device.name = QString::fromStdString(info.name);
            device.instrument = info.isInstrument();
            device.category = device.instrument ? QStringLiteral("Instruments") : QStringLiteral("Audio Effects");
            for (const sub::ParamInfo& param : info.params) {
                device.defaults.insert(QString::fromStdString(param.id), static_cast<double>(param.defaultValue));
            }
            device.info = &info;
            list.push_back(std::move(device));
        }
        return list;
    }();
    return devices;
}

const BuiltinDevice* builtinDevice(const QString& kind) {
    for (const BuiltinDevice& device : builtinDevices()) {
        if (device.kind == kind) return &device;
    }
    return nullptr;
}

std::vector<std::pair<QString, QStringList>> builtinCategories() {
    std::vector<std::pair<QString, QStringList>> categories;
    for (const BuiltinDevice& device : builtinDevices()) {
        auto it = std::find_if(categories.begin(), categories.end(),
                               [&](const auto& entry) { return entry.first == device.category; });
        if (it == categories.end()) {
            categories.emplace_back(device.category, QStringList{});
            it = categories.end() - 1;
        }
        it->second.append(device.kind);
    }
    return categories;
}

bool isInstrument(const QString& kind, const std::optional<PluginRef>& plugin) {
    if (kind == kPluginKind) return plugin && plugin->instrument;
    const BuiltinDevice* builtin = builtinDevice(kind);
    return builtin != nullptr && builtin->instrument;
}

bool deviceIsInstrument(const Device& device) {
    if (device.isRack()) {
        for (const Chain& chain : device.chains) {
            for (const Device& d : chain.devices) {
                if (deviceIsInstrument(d)) return true;
            }
        }
        return false;
    }
    return isInstrument(device.kind, device.plugin);
}

bool loadsInto(const Device& preset, const Device& device) {
    if (preset.kind != device.kind || deviceIsInstrument(preset) != deviceIsInstrument(device)) return false;
    if (preset.isPlugin()) return preset.plugin && device.plugin && preset.plugin->uid == device.plugin->uid;
    return true;
}

QString deviceName(const Device& device) {
    if (device.isRack() && device.name && !device.name->isEmpty()) return *device.name;
    return kindName(device);
}

QString kindName(const Device& device) {
    if (device.isRack()) {
        return deviceIsInstrument(device) ? QStringLiteral("Instrument Rack") : QStringLiteral("Audio Effect Rack");
    }
    if (device.plugin) return device.plugin->name;
    const BuiltinDevice* builtin = builtinDevice(device.kind);
    return builtin != nullptr ? builtin->name : device.kind;
}

Device newDevice(const QString& kind, const std::optional<PluginRef>& plugin) {
    if (kind == kPluginKind) {
        if (!plugin) throw EditError(QStringLiteral("a plug-in device needs a plug-in"));
        Device device;
        device.id = newId();
        device.kind = kind;
        device.plugin = plugin;
        return device;
    }
    if (kind == kRackKind) return newRack({});
    const BuiltinDevice* builtin = builtinDevice(kind);
    if (builtin == nullptr) throw EditError(QStringLiteral("There is no %1 device").arg(kind));
    Device device;
    device.id = newId();
    device.kind = kind;
    device.params = builtin->defaults;
    return device;
}

Device newRack(std::vector<Chain> chains) {
    Device rack;
    rack.id = newId();
    rack.kind = kRackKind;
    for (int i = 0; i < kDefaultMacroCount; ++i) rack.params.insert(macroParam(i), 0.0);
    rack.macroNames.resize(kDefaultMacroCount);
    rack.chains = std::move(chains);
    return rack;
}

Chain newChain(const QString& name, std::vector<Device> devices) {
    Chain chain;
    chain.id = newId();
    chain.name = name;
    chain.devices = std::move(devices);
    return chain;
}

const sub::ParamInfo* builtinParamInfo(const QString& kind, const QString& paramId) {
    const BuiltinDevice* builtin = builtinDevice(kind);
    if (builtin == nullptr) return nullptr;
    const std::string id = paramId.toStdString();
    for (const sub::ParamInfo& param : builtin->info->params) {
        if (param.id == id) return &param;
    }
    return nullptr;
}

QSet<QString> deviceIdsOfList(const std::vector<Device>& devices) {
    QSet<QString> ids;
    for (const Device* d : iterDevices(devices)) ids.insert(d->id);
    return ids;
}

QSet<QString> deviceIdsOf(const Device& device) {
    QSet<QString> ids{device.id};
    for (const Chain& chain : device.chains) ids.unite(deviceIdsOfList(chain.devices));
    return ids;
}

QSet<QString> innerDeviceIds(const Device& rack) {
    QSet<QString> ids;
    for (const Chain& chain : rack.chains) ids.unite(deviceIdsOfList(chain.devices));
    ids.remove(rack.id);  // (should something in it have its id)
    return ids;
}

}  // namespace sub::app
