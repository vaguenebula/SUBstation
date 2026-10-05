#include "devices/DeviceInfo.h"

#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "model/Device.h"
#include "model/Devices.h"
#include "model/Project.h"
#include "session/DeviceSelection.h"

#include <QVariantMap>

#include <algorithm>

namespace sub::ui {

using sub::app::Device;
using sub::app::EngineBridge;
using sub::app::Project;
using sub::app::Sidechain;
using sub::app::Track;

namespace {

QString latencyLine(int latency) { return QStringLiteral("Latency: %1 samples (compensated)").arg(latency); }

}  // namespace

DeviceInfo::DeviceInfo(QObject* parent) : QObject(parent) {}

void DeviceInfo::setSession(sub::app::Session* session) {
    if (session == session_)
        return;
    session_ = session;
    connectSession();
    Q_EMIT targetChanged();
    refresh();
}

void DeviceInfo::setTrackId(const QString& trackId) {
    if (trackId == trackId_)
        return;
    trackId_ = trackId;
    Q_EMIT targetChanged();
    refresh();
}

void DeviceInfo::setDeviceId(const QString& deviceId) {
    if (deviceId == deviceId_)
        return;
    deviceId_ = deviceId;
    Q_EMIT targetChanged();
    refresh();
}

void DeviceInfo::connectSession() {
    for (const QMetaObject::Connection& connection : std::as_const(connections_))
        disconnect(connection);
    connections_.clear();
    if (!session_)
        return;
    Project* project = session_->project();
    EngineBridge* bridge = session_->bridge();
    auto refreshAll = [this] { refresh(); };
    auto ofTrack = [this](const QString& trackId) {
        if (trackId == trackId_)
            refresh();
    };
    auto ofDevice = [this](const QString& trackId, const QString& deviceId) {
        if (trackId == trackId_ && deviceId == deviceId_)
            refresh();
    };
    // Its track's devices changed (it may have moved, gone, been switched or
    // renamed), or another track's: a sidechain's source may have lost (or got
    // back) the device it is taken after. Its sidechain's source renamed (or going).
    connections_ << connect(project, &Project::devicesChanged, this, refreshAll);
    connections_ << connect(project, &Project::trackChanged, this, [this](const QString& trackId) {
        if (trackId == sidechainSource_)
            refresh();
    });
    connections_ << connect(project, &Project::devicesFolded, this, ofTrack);
    connections_ << connect(project, &Project::reset, this, refreshAll);
    for (auto removed : {&Project::trackRemoved, &Project::returnRemoved}) {
        connections_ << connect(project, removed, this, [this](const QString&, int) { refresh(); });
    }
    connections_ << connect(project, &Project::deviceStateChanged, this, ofDevice);
    // Its processor was made again (a plug-in loaded, or failed), or reports something new.
    connections_ << connect(bridge, &EngineBridge::devicesLoaded, this, ofTrack);
    connections_ << connect(bridge, &EngineBridge::pluginParamsChanged, this, ofDevice);
    connections_ << connect(bridge, &EngineBridge::pluginParamsRebuilt, this, ofDevice);
    connections_ << connect(bridge, &EngineBridge::pluginEditorChanged, this, ofDevice);
    connections_ << connect(bridge, &EngineBridge::pluginsPendingChanged, this, refreshAll);
    connections_ << connect(bridge, &EngineBridge::deviceChanged, this, refreshAll);  // (new latencies)
}

std::optional<Sidechain> DeviceInfo::currentSidechain() const {
    const Device* device = session_ ? session_->project()->findDevice(trackId_, deviceId_) : nullptr;
    if (device == nullptr || !device->sidechain || !session_->project()->hasOwner(device->sidechain->trackId))
        return std::nullopt;  // (its source is going: so is the sidechain)
    return device->sidechain;
}

std::vector<std::pair<QString, QString>> DeviceInfo::tapChoices(const QString& sourceTrackId) const {
    std::vector<std::pair<QString, QString>> choices{{QStringLiteral("Pre FX"), sub::app::kPreFx}};
    const Track* source = session_ ? session_->project()->findTrack(sourceTrackId) : nullptr;
    if (source != nullptr) {
        QStringList names;
        for (const Device& device : source->devices)
            names << sub::app::deviceName(device);
        for (int i = 0; i < int(source->devices.size()); ++i) {
            const Device& device = source->devices[size_t(i)];
            QString name = names[i];
            if (names.count(name) > 1)
                name = QStringLiteral("%1 (%2)").arg(name).arg(names.mid(0, i + 1).count(names[i]));
            if (!sub::app::deviceIsInstrument(device))
                choices.emplace_back(QStringLiteral("After ") + name, device.id);
        }
    }
    choices.emplace_back(QStringLiteral("Post FX"), sub::app::kPreFader);
    choices.emplace_back(QStringLiteral("Post Mixer"), sub::app::kPostFader);
    return choices;
}

QString DeviceInfo::tapOf(const Sidechain& sidechain) const {
    const Track* source = session_ ? session_->project()->findTrack(sidechain.trackId) : nullptr;
    if (sidechain.tapDevice() && source != nullptr) {
        const auto found = std::find_if(source->devices.begin(), source->devices.end(),
                                        [&](const Device& d) { return d.id == sidechain.tap; });
        if (found == source->devices.end())
            return sub::app::kPreFader;
        if (sub::app::deviceIsInstrument(*found))
            return sub::app::kPreFx;
    }
    return sidechain.tap;
}

void DeviceInfo::refresh() {
    State s;
    const Project* project = session_ ? session_->project() : nullptr;
    const Track* track = project ? project->findTrack(trackId_) : nullptr;
    const Device* device = track ? sub::app::findDevice(track->devices, deviceId_) : nullptr;
    if (device != nullptr) {
        EngineBridge* bridge = session_->bridge();
        s.exists = true;
        s.kind = device->kind;
        s.isRack = device->isRack();
        s.isPlugin = device->isPlugin();
        s.name = s.isPlugin && device->plugin ? device->plugin->name : sub::app::deviceName(*device);
        s.instrument = sub::app::deviceIsInstrument(*device);
        s.enabled = device->enabled;
        s.folded = project->isDeviceFolded(deviceId_);
        const std::optional<QString> chain = sub::app::containerOf(track->devices, deviceId_);
        s.chainId = chain.value_or(QString());
        if (const auto* siblings = sub::app::chainDevices(track->devices, chain); siblings && !s.instrument) {
            const auto at = std::find_if(siblings->begin(), siblings->end(),
                                         [this](const Device& d) { return d.id == deviceId_; });
            const int index = int(at - siblings->begin());
            const int first = !siblings->empty() && sub::app::deviceIsInstrument(siblings->front()) ? 1 : 0;
            s.canMoveLeft = index > first;
            s.canMoveRight = index < int(siblings->size()) - 1;
        }
        s.loaded = bridge->engineDeviceId(trackId_, deviceId_).has_value();
        const int latency = s.loaded ? bridge->deviceLatency(trackId_, deviceId_) : 0;
        if (s.isPlugin && device->plugin) {
            const sub::app::PluginRef& plugin = *device->plugin;
            s.pending = !s.loaded && bridge->pluginPending(deviceId_);
            if (s.pending) {
                s.pluginMessage = plugin.name + QStringLiteral(" is loading…");  // (they load one by one)
            } else if (!s.loaded) {
                s.pluginMessage = bridge->pluginError(deviceId_);
                if (s.pluginMessage.isEmpty())
                    s.pluginMessage = plugin.name + QStringLiteral(" is not loaded.");
            }
            s.editorOpen = s.loaded && bridge->isPluginEditorOpen(trackId_, deviceId_);
            QStringList lines{plugin.name, plugin.vendor, bridge->pluginPath(plugin).value_or(plugin.path)};
            if (latency)
                lines << latencyLine(latency);
            lines.removeAll(QString());
            s.toolTip = lines.join(u'\n');
        } else if (s.isRack) {
            s.toolTip = sub::app::deviceName(*device) + (latency ? u'\n' + latencyLine(latency) : QString());
        }
        s.hasSidechainInput = bridge->hasSidechainInput(trackId_, deviceId_);
        if (const std::optional<Sidechain> sidechain = currentSidechain()) {
            s.sidechainOn = true;
            const QString tap = tapOf(*sidechain);
            QString where;
            for (const auto& [label, choice] : tapChoices(sidechain->trackId)) {
                if (choice == tap) {
                    where = label;
                    break;
                }
            }
            s.sidechainToolTip =
                QStringLiteral("Sidechain: %1, %2").arg(project->track(sidechain->trackId).name, where);
        } else {
            s.sidechainToolTip = QStringLiteral("Sidechain: none (click to choose a track)");
        }
    }
    const std::optional<Sidechain> sidechain = currentSidechain();
    sidechainSource_ = sidechain ? sidechain->trackId : QString();
    if (s == state_)
        return;
    state_ = s;
    Q_EMIT changed();
}

void DeviceInfo::setEnabled(bool enabled) {
    if (session_ && session_->project()->hasDevice(trackId_, deviceId_))
        session_->editor()->setDeviceEnabled(trackId_, deviceId_, enabled);
}

void DeviceInfo::moveLeft() {
    if (!session_ || !state_.canMoveLeft)
        return;
    const auto ids = session_->deviceSelection()->chainDevices(state_.chainId);
    session_->editor()->moveDevice(trackId_, deviceId_, int(ids.indexOf(deviceId_)) - 1);
}

void DeviceInfo::moveRight() {
    if (!session_ || !state_.canMoveRight)
        return;
    const auto ids = session_->deviceSelection()->chainDevices(state_.chainId);
    session_->editor()->moveDevice(trackId_, deviceId_, int(ids.indexOf(deviceId_)) + 1);
}

void DeviceInfo::showEditor(bool show) {
    if (!session_ || !session_->project()->hasDevice(trackId_, deviceId_))
        return;
    if (show)
        session_->bridge()->openPluginEditor(trackId_, deviceId_);  // (open already: brought to the front)
    else
        session_->bridge()->closePluginEditor(trackId_, deviceId_);
    refresh();
}

void DeviceInfo::openEditor() {
    refresh();
    if (state_.isPlugin && state_.loaded)
        showEditor(true);
}

QVariantList DeviceInfo::sidechainMenu() const {
    QVariantList entries;
    if (!session_ || !session_->project()->hasDevice(trackId_, deviceId_))
        return entries;
    const Project* project = session_->project();
    const std::optional<Sidechain> current = currentSidechain();
    auto entry = [](const QString& text, bool checked, bool enabled, const QString& source, const QString& tap) {
        return QVariantMap{{QStringLiteral("text"), text},          {QStringLiteral("checkable"), true},
                           {QStringLiteral("checked"), checked},    {QStringLiteral("enabled"), enabled},
                           {QStringLiteral("source"), source},      {QStringLiteral("tap"), tap}};
    };
    const QVariantMap separator{{QStringLiteral("separator"), true}};
    entries << entry(QStringLiteral("No Sidechain"), !current, true, QString(), QString()) << separator;
    for (const Track* source : project->sidechainSources(trackId_)) {
        const bool usable = !project->sidechainWouldCycle(trackId_, source->id);
        // A new source: taken where the old one was (after its fader if after a device of it).
        const QString tap = !current || current->tapDevice() ? sub::app::kPostFader : current->tap;
        entries << entry(usable ? source->name : source->name + QStringLiteral(" (this track feeds it)"),
                         current && current->trackId == source->id, usable, source->id, tap);
    }
    if (current) {
        entries << separator;
        const QString tap = tapOf(*current);
        for (const auto& [label, choice] : tapChoices(current->trackId))
            entries << entry(label, choice == tap, true, current->trackId, choice);
    }
    return entries;
}

void DeviceInfo::setSidechain(const QString& sourceTrackId, const QString& tap) {
    if (!session_ || !session_->project()->hasDevice(trackId_, deviceId_))
        return;
    // (A source gone meanwhile is refused: the editor says so, on the status line.)
    session_->editor()->trySetDeviceSidechain(trackId_, deviceId_, sourceTrackId,
                                              tap.isEmpty() ? sub::app::kPostFader : tap);
    refresh();
}

}  // namespace sub::ui
