// Editing a device's settings: its parameters and state, loading a preset into
// it, a rack's name, its on/off switch and its sidechain.

#include "editor/EditorSupport.h"
#include "editor/ProjectEditor.h"

#include "model/Commands.h"
#include "model/Devices.h"
#include "model/Errors.h"

#include <QUndoStack>

#include <algorithm>

namespace sub::app {

using editing::Macro;

void ProjectEditor::setDeviceParam(const QString& trackId, const QString& deviceId, const QString& paramId,
                                   double value, const QString& mergeKey) {
    if (!project_->hasDevice(trackId, deviceId)) return;
    setDeviceParam(trackId, deviceId, paramId, value, mergeKey, std::nullopt);
}

void ProjectEditor::setDeviceParam(const QString& trackId, const QString& deviceId, const QString& paramId,
                                   double value, const QString& mergeKey, std::optional<double> old) {
    if (!old) {
        const auto& params = project_->device(trackId, deviceId).params;
        if (params.contains(paramId)) old = params.value(paramId);
    }
    if (!old || *old != value) {
        push(std::make_unique<SetDeviceParamCommand>(project_, trackId, deviceId, paramId, old ? *old : value, value,
                                                     mergeKey));
    }
    Q_EMIT parameterTouched(trackId, automation::deviceKey(deviceId, paramId));
}

void ProjectEditor::setDeviceParams(const QString& trackId, const QString& deviceId,
                                    const OrderedMap<QString, double>& values, const QString& mergeKey,
                                    const QString& text) {
    const Device& device = project_->device(trackId, deviceId);
    QMap<DeviceParam, double> nw;
    QMap<DeviceParam, double> old;
    for (const auto& [paramId, value] : values) {
        nw.insert({deviceId, paramId}, value);
        double own = value;
        if (device.params.contains(paramId)) {
            own = device.params.value(paramId);
        } else if (const auto info = paramInfo(trackId, deviceId, paramId)) {  // a default value: as it was
            own = info->defaultValue;
        }
        old.insert({deviceId, paramId}, own);
    }
    if (old != nw) push(std::make_unique<SetDeviceParamsCommand>(project_, trackId, old, nw, text, mergeKey));
    if (!values.isEmpty()) Q_EMIT parameterTouched(trackId, automation::deviceKey(deviceId, values.begin()->first));
}

void ProjectEditor::setDeviceParams(const QString& trackId, const QString& deviceId, const QStringList& paramIds,
                                    const QList<double>& values, const QString& mergeKey, const QString& text) {
    if (!project_->hasDevice(trackId, deviceId) || paramIds.size() != values.size()) return;
    OrderedMap<QString, double> params;
    for (qsizetype i = 0; i < paramIds.size(); ++i) params.insert(paramIds[i], values[i]);
    setDeviceParams(trackId, deviceId, params, mergeKey, text);
}

void ProjectEditor::touchParameter(const QString& owner, const QString& key) { Q_EMIT parameterTouched(owner, key); }

void ProjectEditor::setDeviceState(const QString& trackId, const QString& deviceId, const std::optional<QString>& old,
                                   const std::optional<QString>& state, const QString& text) {
    push(std::make_unique<SetDeviceStateCommand>(project_, trackId, deviceId, old, state, text));
}

bool ProjectEditor::loadPresetInto(const QString& trackId, const QString& deviceId, const Device& preset,
                                   const QString& text) {
    const Project& p = *project_;
    const Device device = p.device(trackId, deviceId);
    if (!loadsInto(preset, device)) return false;
    if (device.isRack()) {
        const std::vector<Device> before = p.track(trackId).devices;
        if (rackDepth(before, containerOf(before, deviceId)) + rackHeight(preset) > kMaxRackDepth) return false;
        std::vector<Device> after = before;
        Device* rack = findDevice(after, deviceId);
        rack->chains = preset.chains;
        rack->macros = preset.macros;
        rack->macroNames = preset.macroNames;
        rack->params = preset.params;
        rack->name = preset.name;
        setDevices(trackId, before, std::move(after), text);
        return true;
    }
    std::vector<std::unique_ptr<QUndoCommand>> commands;
    if (!device.isPlugin()) {  // (a plug-in's parameters are in its state)
        QMap<DeviceParam, double> nw;
        QMap<DeviceParam, double> old;
        for (auto it = preset.params.constBegin(); it != preset.params.constEnd(); ++it) {
            nw.insert({deviceId, it.key()}, it.value());
            double own = it.value();
            if (device.params.contains(it.key())) {
                own = device.params.value(it.key());
            } else if (const sub::ParamInfo* info = builtinParamInfo(device.kind, it.key())) {
                own = info->defaultValue;  // (a parameter the device has no value for: at its default)
            }
            old.insert({deviceId, it.key()}, own);
        }
        if (old != nw) commands.push_back(std::make_unique<SetDeviceParamsCommand>(project_, trackId, old, nw, text));
    }
    if (preset.state != device.state && (preset.state || !device.isPlugin())) {
        commands.push_back(
            std::make_unique<SetDeviceStateCommand>(project_, trackId, deviceId, device.state, preset.state, text));
    }
    if (commands.size() == 1) {
        push(std::move(commands.front()));
    } else if (!commands.empty()) {
        Macro macro(undoStack_, text);
        for (auto& command : commands) push(std::move(command));
    }
    return true;
}

void ProjectEditor::renameRack(const QString& trackId, const QString& deviceId, const QString& name,
                               const QString& text) {
    const Device* device = project_->findDevice(trackId, deviceId);
    if (device == nullptr) return;
    const std::optional<QString> nw = name.isEmpty() ? std::nullopt : std::optional<QString>(name);
    if (device->isRack() && device->name != nw) {
        push(std::make_unique<SetDeviceNameCommand>(project_, trackId, deviceId, device->name, nw, text));
    }
}

void ProjectEditor::setDeviceEnabled(const QString& trackId, const QString& deviceId, bool enabled) {
    const Device* device = project_->findDevice(trackId, deviceId);
    if (device == nullptr) return;
    Q_EMIT switchedByHand(trackId, automation::deviceOnKey(deviceId));
    if (device->enabled != enabled) push(std::make_unique<SetDeviceEnabledCommand>(project_, trackId, deviceId, enabled));
    Q_EMIT parameterTouched(trackId, automation::deviceOnKey(deviceId));
}

void ProjectEditor::setDeviceSidechain(const QString& trackId, const QString& deviceId,
                                       const std::optional<Sidechain>& sidechain) {
    const Project& p = *project_;
    const Device& device = p.device(trackId, deviceId);
    if (sidechain) {
        const QString& source = sidechain->trackId;
        if (!(p.hasTrack(source) || p.hasReturn(source)) || p.sidechainWouldCycle(trackId, source)) {
            throw EditError(QStringLiteral("%1 can't take its sidechain from that track").arg(deviceName(device)));
        }
        if (const auto tapped = sidechain->tapDevice()) {  // in its own chain, or in a rack's there
            if (findDevice(p.track(source).devices, *tapped) == nullptr) {
                throw EditError(QStringLiteral("A sidechain can only be taken after one of its source's devices"));
            }
        }
    }
    if (sidechain != device.sidechain) {
        const QString text = sidechain ? QStringLiteral("Change Sidechain") : QStringLiteral("Remove Sidechain");
        push(std::make_unique<SetDeviceSidechainCommand>(project_, trackId, deviceId, device.sidechain, sidechain, text));
    }
}

}  // namespace sub::app
