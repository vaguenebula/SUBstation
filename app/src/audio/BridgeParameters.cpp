// Automation and parameters: envelopes pushed to the engine, overriding them by
// hand and re-enabling them, and every kind of parameter described to the UI as
// ParamSpecs, with its value now.

#include "audio/BridgePrivate.h"

#include "model/Devices.h"
#include "model/Project.h"

#include <QPointer>

#include <algorithm>

namespace sub::app {

void EngineBridge::onAutomationChanged(const QString& owner, const QString& key) {
    if (automation::keySend(key)) pushSends(owner);  // a send automated before it was set is made
    pushAutomation(owner);
}

// The owner's envelopes to the engine, but those overridden. A parameter mapped
// to an automated macro plays the macro's envelope instead of its own. Targets
// whose envelope no longer plays go back to their own value.
void EngineBridge::pushAutomation(const QString& owner) {
    const auto engineId = engineTrackId(owner);
    if (!engineId) return;
    std::vector<std::pair<QString, Envelope>> envelopes;
    for (const auto& [key, points] : project_->automation(owner)) envelopes.emplace_back(key, points);
    const QMap<QString, Envelope> moved = macroEnvelopes(owner);
    for (auto it = moved.constBegin(); it != moved.constEnd(); ++it) {
        std::erase_if(envelopes, [&](const auto& envelope) { return envelope.first == it.key(); });
        envelopes.emplace_back(it.key(), it.value());
    }
    std::vector<sub::AutomationLaneDesc> lanes;
    QSet<QString> playing;
    for (const auto& [key, points] : envelopes) {
        if (points.empty() || d_->overridden.count({owner, key}) > 0) continue;
        if (auto lane = engineLane(owner, key, points)) {
            lanes.push_back(std::move(*lane));
            playing.insert(key);
        }
    }
    d_->macroMoved.insert(owner, moved);
    engine_.setTrackAutomation(*engineId, lanes);
    const QSet<QString> stopped = d_->automating.value(owner) - playing;
    const QSet<QString> started = playing - d_->automating.value(owner);
    d_->automating.insert(owner, playing);
    for (const QString& key : stopped) pushOwnValue(owner, key);  // (a device's switch: pushEnabled)
    // Devices whose switches start playing are switched on in the engine (their lanes switch them).
    if (project_->hasOwner(owner) && std::any_of(started.begin(), started.end(), automation::isSwitchKey)) {
        pushEnabled(owner);
    }
    Q_EMIT automationStateChanged(owner);
}

QMap<QString, Envelope> EngineBridge::macroEnvelopes(const QString& owner) const {
    QMap<QString, Envelope> moved;
    if (!project_->hasOwner(owner)) return moved;
    // Outer racks first: should a nearer rack's macro move the same parameter, it wins (as macroOf says).
    for (const Device* rack : iterDevices(project_->track(owner).devices)) {
        for (const MacroMapping& mapping : rack->macros) {
            const QString macroKey = automation::deviceKey(rack->id, macroParam(mapping.macro));
            if (d_->overridden.count({owner, macroKey}) > 0) continue;
            Envelope points = project_->envelope(owner, macroKey);
            if (points.empty()) continue;
            for (AutomationPoint& point : points) {
                point.value = mapping.target(point.value);
                if (mapping.high < mapping.low) point.curve = -point.curve;  // (bends follow the macro's)
            }
            moved.insert(automation::deviceKey(mapping.deviceId, mapping.paramId), std::move(points));
        }
    }
    return moved;
}

std::optional<sub::AutomationLaneDesc> EngineBridge::engineLane(const QString& owner, const QString& key,
                                                                const Envelope& points) {
    const auto target = automation::parseKey(key);
    if (!target) return std::nullopt;
    std::vector<sub::AutomationPoint> enginePoints;
    for (const AutomationPoint& p : points) {
        enginePoints.push_back({p.beat, static_cast<float>(p.value), static_cast<float>(p.curve)});
    }
    if (target->kind == u"mixer") return sub::AutomationLaneDesc{0, target->id.toStdString(), enginePoints};
    if (target->kind == u"send") {
        const auto returnId = engineTrackId(target->id);
        if (!returnId) return std::nullopt;
        return sub::AutomationLaneDesc{0, "send:" + std::to_string(*returnId), enginePoints};
    }
    const auto processorId = engineDeviceId(owner, target->id);
    if (const auto control = automation::keyChainControl(key)) {  // a rack chain's fader
        const auto chain = engineChainId(control->chainId);
        if (!processorId || !chain) return std::nullopt;
        return sub::AutomationLaneDesc{
            *processorId, "chain:" + std::to_string(*chain) + ":" + control->control.toStdString(), enginePoints};
    }
    if (!processorId) return std::nullopt;
    return sub::AutomationLaneDesc{*processorId, target->param.toStdString(), enginePoints};
}

// A target no longer automated: back to the value it has in the model.
void EngineBridge::pushOwnValue(const QString& owner, const QString& key) {
    if (automation::kMixerKeys.contains(key)) {
        if (project_->hasOwner(owner)) pushMixer(owner);
        return;
    }
    if (automation::keySend(key) || automation::keyChain(key)) return;  // the engine kept the send's (the fader's) own level
    if (automation::isSwitchKey(key)) {  // a device's on/off: as it is in the model again
        if (project_->hasOwner(owner)) pushEnabled(owner);
        return;
    }
    const auto deviceId = automation::keyDevice(key);
    const auto target = automation::parseKey(key);
    if (deviceId && target && project_->hasDevice(owner, *deviceId)) pushDeviceParam(owner, *deviceId, target->param);
}

bool EngineBridge::isAutomated(const QString& owner, const QString& key) const {
    const auto it = d_->automating.constFind(owner);
    return it != d_->automating.constEnd() && it->contains(key);
}

bool EngineBridge::isOverridden(const QString& owner, const QString& key) const {
    return d_->overridden.count({owner, key}) > 0;
}

bool EngineBridge::hasOverrides() const { return !d_->overridden.empty(); }

void EngineBridge::overrideAutomation(const QString& owner, const QString& key) {
    if (!isAutomated(owner, key)) return;
    d_->overridden.insert({owner, key});
    pushAutomation(owner);
}

void EngineBridge::reEnableAutomation(const QString& owner) {
    QStringList owners;
    for (const auto& [o, key] : d_->overridden) {
        if ((owner.isEmpty() || o == owner) && !owners.contains(o)) owners.append(o);
    }
    for (auto it = d_->overridden.begin(); it != d_->overridden.end();) {
        it = owners.contains(it->first) ? d_->overridden.erase(it) : std::next(it);
    }
    for (const QString& o : owners) guarded("re-enable automation", [&] { pushAutomation(o); });
}

const std::vector<sub::ParamInfo>& EngineBridge::paramInfos(quint32 processorId) {
    auto it = d_->paramInfos.find(processorId);
    if (it == d_->paramInfos.end()) it = d_->paramInfos.insert(processorId, engine_.processorParams(processorId));
    return *it;
}

QString EngineBridge::pluginParamText(quint32 processorId, int index, double value) {
    const std::vector<sub::ParamInfo>& infos = paramInfos(processorId);
    const QString unit = index >= 0 && index < static_cast<int>(infos.size())
                             ? QString::fromStdString(infos[static_cast<size_t>(index)].unit)
                             : QString();
    const QString text =
        QString::fromStdString(engine_.processorParamText(processorId, index, static_cast<float>(value)));
    if (text.isEmpty()) return QString::number(value, 'f', 2);
    return unit.isEmpty() || text.contains(unit) ? text : text + u' ' + unit;
}

std::vector<ParamSpec> EngineBridge::deviceParamSpecs(const QString& trackId, const Device& device) {
    // Every device (a rack too) can be switched on and off: Device On, first.
    if (device.isRack()) {
        QStringList macros;
        for (int i = 0; i < macroCount(device); ++i) macros.append(macroName(device, i));
        std::vector<ParamSpec> specs{deviceOnSpec(device.id, deviceName(device))};
        for (ParamSpec& spec : macroSpecs(device.id, macros, deviceName(device))) specs.push_back(std::move(spec));
        std::vector<std::pair<QString, QString>> chains;
        for (const Chain& chain : device.chains) chains.emplace_back(chain.id, chain.name);
        for (ParamSpec& spec : chainSpecs(device.id, chains, deviceName(device))) specs.push_back(std::move(spec));
        return specs;
    }
    const auto processorId = engineDeviceId(trackId, device.id);
    if (!processorId) return {};
    const quint32 id = *processorId;
    auto cached = d_->paramSpecs.constFind(id);
    if (cached != d_->paramSpecs.constEnd()) return *cached;
    const QString name = deviceName(device);
    const bool isPlugin = d_->pluginIds.contains(id);
    std::vector<ParamSpec> specs{deviceOnSpec(device.id, name)};
    const std::vector<sub::ParamInfo> infos = paramInfos(id);
    for (size_t index = 0; index < infos.size(); ++index) {
        const sub::ParamInfo& info = infos[index];
        if (!info.automatable || info.hidden || info.readOnly) continue;
        std::function<QString(double)> text;
        if (isPlugin) {
            // The plug-in's own text, while it is there.
            const QPointer<EngineBridge> self(this);
            text = [self, id, index = static_cast<int>(index)](double value) {
                if (self) {
                    try {
                        return self->pluginParamText(id, index, value);
                    } catch (const std::exception&) {
                    }
                }
                return QString::number(value, 'f', 2);
            };
        }
        specs.push_back(ParamSpec::fromInfo(info, automation::deviceKey(device.id, QString::fromStdString(info.id)),
                                            name, text));
    }
    d_->paramSpecs.insert(id, specs);
    return specs;
}

std::vector<ParamSpec> EngineBridge::mixerSpecs(const QString& owner) const {
    std::vector<std::pair<QString, QString>> sends;
    if (project_->hasOwner(owner)) {
        for (const Track* ret : project_->sendTargets(owner)) sends.emplace_back(ret->id, project_->returnLetter(ret->id));
    }
    return sub::app::mixerSpecs(owner == kMaster, sends);
}

std::vector<ParamGroup> EngineBridge::paramGroups(const QString& owner) {
    std::vector<ParamGroup> groups;
    groups.push_back({QStringLiteral("mixer"), QStringLiteral("Mixer"), mixerSpecs(owner)});
    if (!project_->hasOwner(owner)) return groups;  // (one that went: its mixer, as any owner's)
    std::vector<Device> devices;
    for (const Device* device : iterDevices(project_->track(owner).devices)) devices.push_back(*device);
    for (const Device& device : devices) {
        groups.push_back({device.id, deviceName(device), deviceParamSpecs(owner, device)});
    }
    return groups;
}

bool EngineBridge::canAutomate(const QString& owner, const QString& key) {
    if (!project_->hasOwner(owner)) return false;
    for (const ParamGroup& group : paramGroups(owner)) {
        for (const ParamSpec& spec : group.specs) {
            if (spec.key == key) return true;
        }
    }
    return false;
}

std::optional<ParamSpec> EngineBridge::paramSpec(const QString& owner, const QString& key) {
    if (automation::kMixerKeys.contains(key)) {  // no need to work out its sends (the master has no activator: none)
        for (const ParamSpec& spec : sub::app::mixerSpecs(owner == kMaster)) {
            if (spec.key == key) return spec;
        }
        return std::nullopt;
    }
    if (automation::isMixerKey(key)) {  // (a return that is gone: none)
        for (const ParamSpec& spec : mixerSpecs(owner)) {
            if (spec.key == key) return spec;
        }
        return std::nullopt;
    }
    if (!project_->hasOwner(owner)) return std::nullopt;
    const auto deviceId = automation::keyDevice(key);
    if (!deviceId) return std::nullopt;
    const Device* found = findDevice(project_->track(owner).devices, *deviceId);
    if (found == nullptr) return std::nullopt;
    const Device device = *found;
    for (const ParamSpec& spec : deviceParamSpecs(owner, device)) {
        if (spec.key == key) return spec;
    }
    // Not loaded: its values are still worth showing.
    if (const auto target = automation::parseKey(key); target && target->param == automation::kDeviceOn) {
        return deviceOnSpec(device.id, deviceName(device));
    }
    ParamSpec spec;
    spec.key = key;
    const auto target = automation::parseKey(key);
    spec.name = target ? target->param : key;
    spec.group = deviceName(device);
    spec.text = [](double value) { return formatValue(value, {}); };
    return spec;
}

std::optional<double> EngineBridge::ownValue(const QString& owner, const QString& key) {
    if (!project_->hasOwner(owner)) return std::nullopt;
    const Track& track = project_->track(owner);
    if (key == automation::kMixerVolume) return track.volumeDb;
    if (key == automation::kMixerPan) return track.pan;
    if (key == automation::kMixerOn) return track.mute ? 0.0 : 1.0;
    if (const auto returnId = automation::keySend(key)) return track.sends.value(*returnId, Send{}).levelDb;
    if (const auto control = automation::keyChainControl(key)) {
        for (const ConstRackChain& rc : iterChains(track.devices)) {
            if (rc.chain->id == control->chainId) {
                return control->control == automation::kChainVolume ? rc.chain->volumeDb : rc.chain->pan;
            }
        }
        return std::nullopt;
    }
    const auto target = automation::parseKey(key);
    if (!target) return std::nullopt;
    const Device* device = findDevice(track.devices, target->id);
    if (device == nullptr) return std::nullopt;
    if (target->param == automation::kDeviceOn) return device->enabled ? 1.0 : 0.0;
    const std::optional<double> own =
        device->params.contains(target->param) ? std::optional(device->params.value(target->param)) : std::nullopt;
    const auto processorId = engineDeviceId(owner, device->id);
    const std::string paramId = target->param.toStdString();
    if (processorId && d_->pluginIds.contains(*processorId)) {  // its values live in the plug-in
        const int index = engine_.processorParamIndex(*processorId, paramId);
        if (index >= 0) return engine_.processorParam(*processorId, index);
    }
    if (!own && processorId) {
        const int index = engine_.processorParamIndex(*processorId, paramId);
        if (index >= 0) return paramInfos(*processorId)[static_cast<size_t>(index)].defaultValue;
    }
    return own;
}

std::optional<double> EngineBridge::currentValue(const QString& owner, const QString& key, std::optional<double> beat) {
    const auto spec = paramSpec(owner, key);
    if (spec && isAutomated(owner, key)) {
        const auto moved = d_->macroMoved.value(owner).constFind(key);  // (it follows its macro's automation)
        const Envelope envelope =
            moved != d_->macroMoved.value(owner).constEnd() ? *moved : project_->envelope(owner, key);
        const auto value = automation::valueAt(envelope, beat ? *beat : position());
        if (value) return spec->fromNormalized(spec->quantize(*value));
    }
    return ownValue(owner, key);
}

}  // namespace sub::app
