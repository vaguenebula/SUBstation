// Plug-ins: where they are (the scan), their states, their editors (open, shown
// with the selected track's devices, hidden and closed), and what they report
// (edits made in their editors, parameters changed or rebuilt). And what a
// device's editor reads of its processor: its parameters and displays.

#include "audio/BridgePrivate.h"

#include "model/Devices.h"
#include "model/Project.h"
#include "plugins/PluginInfo.h"

#include <algorithm>

namespace sub::app {

namespace {

ProcessorParam processorParam(const sub::ParamInfo& info) {
    ProcessorParam param;
    param.id = QString::fromStdString(info.id);
    param.name = QString::fromStdString(info.name);
    param.unit = QString::fromStdString(info.unit);
    param.minValue = info.minValue;
    param.maxValue = info.maxValue;
    param.defaultValue = info.defaultValue;
    param.logScale = info.logScale;
    for (const std::string& label : info.valueLabels) param.valueLabels.append(QString::fromStdString(label));
    param.steps = info.steps;
    param.automatable = info.automatable;
    param.readOnly = info.readOnly;
    param.hidden = info.hidden;
    return param;
}

}  // namespace

void EngineBridge::setKnownPlugins(const std::vector<PluginInfo>& plugins) {
    Private& d = *d_;
    d.knownPlugins.clear();
    for (const PluginInfo& plugin : plugins) d.knownPlugins.insert(plugin.uid, plugin.path);
    QStringList reloadedTracks;
    // (Keys first: loading a plug-in may run a message loop.)
    const QMap<QString, ModelChain> chains = modelChains();
    for (auto it = chains.constBegin(); it != chains.constEnd(); ++it) {
        const QString key = it.key();
        const QString trackId = it->trackId;
        if (!d.devices.contains(key)) continue;
        const std::vector<ChainEntry> chain = d.devices.value(key);
        if (std::none_of(chain.begin(), chain.end(), [](const ChainEntry& e) { return !e.second; })) continue;
        std::vector<ChainEntry> reloaded;
        for (const auto& [deviceId, processorId] : chain) {
            std::optional<quint32> loaded = processorId;
            const Device* device = project_->findDevice(trackId, deviceId);
            if (!processorId && device != nullptr && device->isPlugin() && pluginPath(*device->plugin)) {
                const Device copy = *device;
                loaded = loadPlugin(d.chains.value(key), copy);
                d.pids.insert(deviceId, loaded);
            }
            reloaded.emplace_back(deviceId, loaded);
        }
        if (reloaded != chain) {
            d.devices.insert(key, reloaded);
            std::vector<uint32_t> order;
            for (const auto& entry : reloaded) {
                if (entry.second) order.push_back(*entry.second);
            }
            engine_.setChainOrder(d.chains.value(key), order);
            if (!reloadedTracks.contains(trackId)) reloadedTracks.append(trackId);
        }
    }
    for (const QString& trackId : reloadedTracks) {
        if (project_->hasOwner(trackId)) pushEnabled(trackId);
        pushAutomation(trackId);
        Q_EMIT devicesLoaded(trackId);
    }
}

std::optional<QByteArray> EngineBridge::pluginState(const QString& trackId, const QString& deviceId) {
    const auto processorId = engineDeviceId(trackId, deviceId);
    if (!processorId || !d_->pluginIds.contains(*processorId)) return std::nullopt;
    const std::vector<uint8_t> state = engine_.processorState(*processorId);
    return QByteArray(reinterpret_cast<const char*>(state.data()), static_cast<qsizetype>(state.size()));
}

QString EngineBridge::applyPluginState(const QString& trackId, const QString& deviceId, const QByteArray& state) {
    const auto processorId = engineDeviceId(trackId, deviceId);
    if (!processorId || !d_->pluginIds.contains(*processorId)) return QStringLiteral("the plug-in isn't loaded.");
    QString problem;
    ++d_->busy;  // (the plug-in may run a message loop)
    try {
        engine_.setProcessorState(*processorId, std::vector<uint8_t>(state.begin(), state.end()));
    } catch (const std::exception& error) {  // another plug-in's settings
        problem = QString::fromStdString(error.what());
    }
    --d_->busy;
    return problem;
}

void EngineBridge::storePluginStates(const std::optional<QSet<QString>>& deviceIds) {
    std::vector<std::pair<QString, QString>> plugins;  // (track, device)
    for (const Track* track : project_->allTracks()) {
        for (const Device* device : iterDevices(track->devices)) {
            if (deviceIds && !deviceIds->contains(device->id)) continue;
            if (device->isPlugin()) plugins.emplace_back(track->id, device->id);
        }
    }
    for (const auto& [trackId, deviceId] : plugins) {
        const auto processorId = engineDeviceId(trackId, deviceId);
        if (!processorId || !d_->pluginIds.contains(*processorId)) continue;
        const Device& device = project_->device(trackId, deviceId);
        QString state = device.state.value_or(QString());
        try {
            const std::vector<uint8_t> bytes = engine_.processorState(*processorId);
            state = QString::fromLatin1(
                QByteArray(reinterpret_cast<const char*>(bytes.data()), static_cast<qsizetype>(bytes.size())).toBase64());
        } catch (const std::exception& error) {
            Q_EMIT statusMessage(device.plugin->name + QStringLiteral(": ") + QString::fromStdString(error.what()));
        }
        // (And where it was found, if it moved.)
        project_->storePluginState(trackId, deviceId, state, d_->pluginIds.value(*processorId));
    }
}

std::optional<QString> EngineBridge::paramIdAt(quint32 processorId, int index) {
    auto it = d_->paramIds.find(processorId);
    if (it == d_->paramIds.end()) {
        QStringList ids;
        for (const sub::ParamInfo& info : engine_.processorParams(processorId)) ids.append(QString::fromStdString(info.id));
        it = d_->paramIds.insert(processorId, ids);
    }
    if (index < 0 || index >= it->size()) return std::nullopt;
    return it->at(index);
}

QString EngineBridge::editorTitle(const QString& trackId, const QString& deviceId) const {
    const Device* device = project_->findDevice(trackId, deviceId);
    const Track* track = project_->findTrack(trackId);
    const QString name = device == nullptr ? QString() : device->plugin ? device->plugin->name : device->kind;
    return name + QStringLiteral(" - ") + (track != nullptr ? track->name : QString());
}

void EngineBridge::setOwnerWindow(std::function<uintptr_t()> ownerWindow) {
    d_->ownerWindow = ownerWindow ? std::move(ownerWindow) : [] { return uintptr_t{0}; };
}

bool EngineBridge::openPluginEditor(const QString& trackId, const QString& deviceId, bool report) {
    const auto processorId = engineDeviceId(trackId, deviceId);
    if (!processorId) return false;
    const QString title = editorTitle(trackId, deviceId);
    bool opened = false;
    ++d_->busy;
    try {
        opened = engine_.openEditor(*processorId, d_->ownerWindow(), title.toStdString());
    } catch (const std::exception& error) {
        qWarning("EngineBridge (open editor): %s", error.what());
    }
    --d_->busy;
    if (opened) {
        d_->editorsWanted.insert(deviceId);
    } else {
        d_->editorsWanted.remove(deviceId);
        if (report) {
            const Device* device = project_->findDevice(trackId, deviceId);
            const QString name = device == nullptr ? QString() : deviceName(*device);
            Q_EMIT statusMessage(name + QStringLiteral(" has no editor."));
        }
    }
    Q_EMIT pluginEditorChanged(trackId, deviceId);
    return opened;
}

void EngineBridge::closePluginEditor(const QString& trackId, const QString& deviceId) {
    d_->editorsWanted.remove(deviceId);
    const auto processorId = engineDeviceId(trackId, deviceId);
    if (!processorId) return;
    auto& hidden = d_->hiddenEditors;
    hidden.erase(std::remove(hidden.begin(), hidden.end(), *processorId), hidden.end());
    engine_.closeEditor(*processorId);
    Q_EMIT pluginEditorChanged(trackId, deviceId);
}

void EngineBridge::requestPluginEditor(const QString& trackId, const QString& deviceId) {
    guarded("plug-in editor", [&] {
        loadPluginNow(deviceId);  // (if it waits to load)
        if (d_->editorsTrack && trackId == *d_->editorsTrack) {
            openPluginEditor(trackId, deviceId, false);  // having none is fine here
        } else {
            d_->editorsWanted.insert(deviceId);
        }
    });
}

void EngineBridge::showPluginEditors(const QString& trackId) {
    Private& d = *d_;
    const std::optional<QString> shown = trackId.isEmpty() ? std::nullopt : std::optional(trackId);
    if (shown == d.editorsTrack) return;
    d.editorsTrack = shown;
    // A copy: opening an editor may run a message loop that changes the chains.
    std::vector<std::pair<QString, std::vector<ChainEntry>>> chains;
    for (auto it = d.devices.constBegin(); it != d.devices.constEnd(); ++it) {
        chains.emplace_back(d.chainOwner.value(it.key()), it.value());
    }
    for (const auto& [chainTrack, chain] : chains) {
        for (const auto& [deviceId, processorId] : chain) {
            if (!processorId || !d.pluginIds.contains(*processorId)) continue;
            if (!shown || chainTrack != *shown) {
                if (engine_.isEditorOpen(*processorId)) {
                    engine_.setEditorVisible(*processorId, false);
                    d.hiddenEditors.push_back(*processorId);
                    Q_EMIT pluginEditorChanged(chainTrack, deviceId);
                }
            } else if (d.editorsWanted.contains(deviceId) && !engine_.isEditorOpen(*processorId)) {
                auto& hidden = d.hiddenEditors;
                hidden.erase(std::remove(hidden.begin(), hidden.end(), *processorId), hidden.end());
                if (engine_.setEditorVisible(*processorId, true)) {
                    Q_EMIT pluginEditorChanged(chainTrack, deviceId);
                } else {  // it was closed meanwhile (too many hidden, or its plug-in reloaded)
                    openPluginEditor(chainTrack, deviceId, false);
                }
            }
        }
    }
    while (static_cast<int>(d.hiddenEditors.size()) > kMaxHiddenEditors) {
        const quint32 oldest = d.hiddenEditors.front();
        d.hiddenEditors.erase(d.hiddenEditors.begin());
        engine_.closeEditor(oldest);  // still wanted: it reopens when shown
    }
}

bool EngineBridge::isPluginEditorOpen(const QString& trackId, const QString& deviceId) {
    const auto processorId = engineDeviceId(trackId, deviceId);
    return processorId && engine_.isEditorOpen(*processorId);
}

void EngineBridge::closeAllEditors() {
    d_->hiddenEditors.clear();
    for (const std::vector<ChainEntry>& chain : d_->devices) {
        for (const auto& [deviceId, processorId] : chain) {
            if (processorId && d_->pluginIds.contains(*processorId)) engine_.closeEditor(*processorId);
        }
    }
}

void EngineBridge::updateEditorTitles(const QString& trackId) {
    for (const QString& key : ownedChains(trackId)) {
        for (const auto& [deviceId, processorId] : d_->devices.value(key)) {
            if (processorId && d_->pluginIds.contains(*processorId)) {  // hidden editors too; nothing without one
                engine_.setEditorTitle(*processorId, editorTitle(trackId, deviceId).toStdString());
            }
        }
    }
}

void EngineBridge::dispatchProcessorEvents(const std::vector<sub::ProcessorEventRecord>& events) {
    if (events.empty()) return;
    Private& d = *d_;
    QHash<quint32, std::pair<QString, QString>> places;  // processor -> (track, device)
    for (auto it = d.devices.constBegin(); it != d.devices.constEnd(); ++it) {
        if (!d.chainOwner.contains(it.key())) continue;
        for (const auto& [deviceId, processorId] : it.value()) {
            if (processorId) places.insert(*processorId, {d.chainOwner.value(it.key()), deviceId});
        }
    }
    std::vector<std::pair<QString, QString>> changed;  // in the order they came
    const auto change = [&](const std::pair<QString, QString>& place) {
        if (std::find(changed.begin(), changed.end(), place) == changed.end()) changed.push_back(place);
    };
    bool dirty = false;
    using Type = sub::ProcessorEvent::Type;
    for (const sub::ProcessorEventRecord& event : events) {
        const auto found = places.constFind(event.processorId);
        if (found == places.constEnd()) continue;
        const auto [trackId, deviceId] = *found;
        if ((event.type == Type::ParamEdited || event.type == Type::ParamTouched) &&
            !engine_.isEditorOpen(event.processorId)) {
            // Not the user (who edits in the editor): the plug-in itself, as some do when
            // their state is restored. Not an edit to undo; its parameters show anew.
            change(*found);
            continue;
        }
        switch (event.type) {
        case Type::ParamEdited:
            if (const auto paramId = paramIdAt(event.processorId, event.paramIndex)) {
                Q_EMIT pluginParamEdited(trackId, deviceId, *paramId, event.value, event.oldValue, event.gesture);
            }
            break;
        case Type::ParamTouched:
            if (const auto paramId = paramIdAt(event.processorId, event.paramIndex)) {
                Q_EMIT pluginParamTouched(trackId, deviceId, *paramId);
            }
            break;
        case Type::ParamsChanged:
        case Type::LatencyChanged: change(*found); break;
        case Type::ParamInfoChanged:
            d.paramIds.remove(event.processorId);
            d.paramInfos.remove(event.processorId);
            d.paramSpecs.remove(event.processorId);
            pushAutomation(trackId);  // its parameters may be elsewhere in the list now
            Q_EMIT pluginParamsRebuilt(trackId, deviceId);
            break;
        case Type::EditorClosed:
            d.editorsWanted.remove(deviceId);
            Q_EMIT pluginEditorChanged(trackId, deviceId);
            break;
        case Type::EditorRequested: requestPluginEditor(trackId, deviceId); break;  // like any editor, shown with its track
        case Type::StateDirty: dirty = true; break;
        }
    }
    for (const auto& [trackId, deviceId] : changed) Q_EMIT pluginParamsChanged(trackId, deviceId);
    if (dirty) Q_EMIT pluginStateDirty();
}

void EngineBridge::pollPlugins() {
    engine_.idle();
    if (d_->busy) return;  // not from a message loop inside a plug-in call
    std::vector<sub::ProcessorEventRecord> events = std::exchange(d_->injectedEvents, {});
    for (sub::ProcessorEventRecord& event : engine_.takeProcessorEvents()) events.push_back(event);
    dispatchProcessorEvents(events);
}

// --- What a device's editor reads ----------------------------------------------------------------

QList<ProcessorParam> EngineBridge::deviceParams(const QString& trackId, const QString& deviceId) {
    QList<ProcessorParam> params;
    const auto processorId = engineDeviceId(trackId, deviceId);
    if (!processorId) return params;
    for (const sub::ParamInfo& info : paramInfos(*processorId)) params.append(processorParam(info));
    return params;
}

std::optional<double> EngineBridge::deviceParamValue(const QString& trackId, const QString& deviceId, int index) {
    const auto processorId = engineDeviceId(trackId, deviceId);
    if (!processorId || index < 0 || index >= static_cast<int>(paramInfos(*processorId).size())) return std::nullopt;
    return engine_.processorParam(*processorId, index);
}

QString EngineBridge::deviceParamText(const QString& trackId, const QString& deviceId, int index, double value) {
    const auto processorId = engineDeviceId(trackId, deviceId);
    if (!processorId || index < 0 || index >= static_cast<int>(paramInfos(*processorId).size())) {
        return QString::number(value, 'f', 2);
    }
    return pluginParamText(*processorId, index, value);
}

int EngineBridge::deviceLatency(const QString& trackId, const QString& deviceId) {
    const auto processorId = engineDeviceId(trackId, deviceId);
    return processorId ? engine_.processorInfo(*processorId).latency : 0;
}

QList<ProcessorDisplay> EngineBridge::processorDisplays(const QString& trackId, const QString& deviceId) {
    QList<ProcessorDisplay> displays;
    const auto processorId = engineDeviceId(trackId, deviceId);
    if (!processorId) return displays;
    for (const sub::DisplayInfo& info : engine_.processorDisplays(*processorId)) {
        ProcessorDisplay display;
        display.id = QString::fromStdString(info.id);
        display.samplesPerValue = info.samplesPerValue;
        displays.append(display);
    }
    return displays;
}

quint64 EngineBridge::readProcessorDisplay(const QString& trackId, const QString& deviceId, int index,
                                           quint64 position, std::vector<float>& out) {
    const auto processorId = engineDeviceId(trackId, deviceId);
    if (!processorId) return position;
    return engine_.readProcessorDisplay(*processorId, index, position, out);
}

}  // namespace sub::app
