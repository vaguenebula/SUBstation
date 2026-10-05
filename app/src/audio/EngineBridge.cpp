// The engine bridge's start-up and shut-down: its state, the project's signals
// it listens to, its timers and thread pools. Its parts are in the Bridge*.cpp
// files, one an area (tracks, inputs, devices, plug-ins, loading, parameters,
// sources, transport, recording, freezing, reversing, the audio device).

#include "audio/BridgePrivate.h"

#include "model/Project.h"

#include <QDebug>

#include <exception>

namespace sub::app {

void guarded(const char* what, const std::function<void()>& body) {
    try {
        body();
    } catch (const EditError& error) {
        qWarning("EngineBridge (%s): %s", what, qUtf8Printable(error.message()));
    } catch (const std::exception& error) {
        qWarning("EngineBridge (%s): %s", what, error.what());
    }
}

EngineBridge::EngineBridge(sub::Engine& engine, Project* project, QObject* parent)
    : QObject(parent), engine_(engine), project_(project), d_(std::make_unique<Private>()) {
    d_->trackIds.insert(kMaster, sub::Engine::kMaster);
    d_->chains.insert(kMaster, engine_.trackChain(sub::Engine::kMaster));
    d_->chainOwner.insert(kMaster, kMaster);

    d_->pluginTimer.setSingleShot(true);
    connect(&d_->pluginTimer, &QTimer::timeout, this, [this] { guarded("loading plug-ins", [&] { loadNextPlugin(); }); });
    d_->pool.setMaxThreadCount(2);
    d_->statePool.setMaxThreadCount(1);

    connect(project, &Project::reset, this, [this] { guarded("reset", [&] { onReset(); }); });
    connect(project, &Project::trackInserted, this,
            [this](const QString& trackId, int) { guarded("trackInserted", [&] { addEngineTrack(trackId); }); });
    connect(project, &Project::trackRemoved, this,
            [this](const QString& trackId, int) { guarded("trackRemoved", [&] { onTrackRemoved(trackId); }); });
    connect(project, &Project::returnInserted, this,
            [this](const QString& trackId, int) { guarded("returnInserted", [&] { addEngineTrack(trackId); }); });
    connect(project, &Project::returnRemoved, this,
            [this](const QString& trackId, int) { guarded("returnRemoved", [&] { onTrackRemoved(trackId); }); });
    connect(project, &Project::trackChanged, this,
            [this](const QString& trackId) { guarded("trackChanged", [&] { onTrackChanged(trackId); }); });
    connect(project, &Project::tracksArranged, this, [this] { guarded("tracksArranged", [&] { pushOutputs(); }); });
    connect(project, &Project::clipsChanged, this,
            [this](const QString& trackId) { guarded("clipsChanged", [&] { pushClips(trackId); }); });
    connect(project, &Project::devicesChanged, this,
            [this](const QString& trackId) { guarded("devicesChanged", [&] { syncDevices(trackId); }); });
    connect(project, &Project::freezeChanged, this,
            [this](const QString& trackId) { guarded("freezeChanged", [&] { onFreezeChanged(trackId); }); });
    connect(project, &Project::chainChanged, this, [this](const QString& trackId, const QString& chainId) {
        guarded("chainChanged", [&] { onChainChanged(trackId, chainId); });
    });
    connect(project, &Project::deviceParamChanged, this,
            [this](const QString& trackId, const QString& deviceId, const QString& paramId) {
                guarded("deviceParamChanged", [&] { onDeviceParamChanged(trackId, deviceId, paramId); });
            });
    connect(project, &Project::deviceStateChanged, this, [this](const QString& trackId, const QString& deviceId) {
        guarded("deviceStateChanged", [&] { pushDeviceState(trackId, deviceId); });
    });
    connect(project, &Project::trackChanged, this,
            [this](const QString& trackId) { guarded("trackChanged", [&] { updateEditorTitles(trackId); }); });
    connect(project, &Project::settingsChanged, this, [this] { guarded("settingsChanged", [&] { pushSettings(); }); });
    connect(project, &Project::automationChanged, this, [this](const QString& owner, const QString& key) {
        guarded("automationChanged", [&] { onAutomationChanged(owner, key); });
    });

    d_->positionTimer.setInterval(kPositionPollMs);
    connect(&d_->positionTimer, &QTimer::timeout, this, [this] { guarded("playhead", [&] { pollPosition(); }); });
    d_->positionTimer.start();
    d_->meterTimer.setInterval(kMeterPollMs);
    connect(&d_->meterTimer, &QTimer::timeout, this, [this] { guarded("meters", [&] { pollMeters(); }); });
    d_->meterTimer.start();

    onReset();
}

EngineBridge::~EngineBridge() {
    d_->positionTimer.stop();
    d_->meterTimer.stop();
    d_->pluginTimer.stop();
    // Their results come to this object, which is going: what is posted to it goes with it.
    d_->pool.waitForDone();
    d_->statePool.waitForDone();
}

void EngineBridge::shutdown() {
    stopLoadingPlugins();
    closeAllEditors();
    waitForDeviceStates();
    removeEngineTracks();
    d_->devices.clear();
    d_->pluginIds.clear();
    for (const std::string& name : engine_.openMidiInputs()) engine_.closeMidiInput(name);
    engine_.idle(true);
}

std::optional<quint32> EngineBridge::engineTrackId(const QString& trackId) const {
    const auto it = d_->trackIds.constFind(trackId);
    if (it == d_->trackIds.constEnd()) return std::nullopt;
    return *it;
}

std::optional<quint32> EngineBridge::engineDeviceId(const QString& trackId, const QString& deviceId) const {
    const auto key = d_->where.constFind(deviceId);
    if (key == d_->where.constEnd() || d_->chainOwner.value(*key) != trackId) return std::nullopt;
    return d_->pids.value(deviceId);
}

std::optional<quint32> EngineBridge::engineChainId(const QString& chainId) const {
    if (!d_->rackOfChain.contains(chainId)) return std::nullopt;
    const auto it = d_->chains.constFind(chainId);
    if (it == d_->chains.constEnd()) return std::nullopt;
    return *it;
}

}  // namespace sub::app
