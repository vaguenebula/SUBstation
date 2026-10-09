#include "devices/DeviceCanvas.h"

#include "devices/DisplayClock.h"

#include "audio/BridgeTypes.h"
#include "audio/EngineBridge.h"
#include "controls/Automation.h"
#include "editor/ProjectEditor.h"
#include "model/Automation.h"
#include "model/Device.h"
#include "model/Project.h"
#include "model/Track.h"

#include <QMouseEvent>
#include <QScopedValueRollback>

namespace sub::ui {

using sub::app::EngineBridge;
using sub::app::Project;

DeviceCanvas::DeviceCanvas(QQuickItem* parent) : SgCanvas(parent) {}

DeviceCanvas::~DeviceCanvas() = default;

void DeviceCanvas::setSession(sub::app::Session* session) {
    if (session == session_)
        return;
    session_ = session;
    connectSession();
    Q_EMIT deviceChanged();
    refresh();
}

void DeviceCanvas::setTrackId(const QString& trackId) {
    if (trackId == trackId_)
        return;
    trackId_ = trackId;
    Q_EMIT deviceChanged();
    refresh();
}

void DeviceCanvas::setDeviceId(const QString& deviceId) {
    if (deviceId == deviceId_)
        return;
    deviceId_ = deviceId;
    Q_EMIT deviceChanged();
    refresh();
}

void DeviceCanvas::connectSession() {
    for (const QMetaObject::Connection& connection : std::as_const(connections_))
        disconnect(connection);
    connections_.clear();
    if (!session_)
        return;
    Project* project = session_->project();
    EngineBridge* bridge = session_->bridge();
    connections_ << connect(project, &Project::deviceParamChanged, this,
                            [this](const QString& trackId, const QString& deviceId, const QString&) {
                                if (trackId == trackId_ && deviceId == deviceId_ && !editing_)
                                    doSync();
                            });
    connections_ << connect(project, &Project::devicesChanged, this, [this](const QString& trackId) {
        if (trackId == trackId_)
            refresh();
    });
    connections_ << connect(project, &Project::trackChanged, this, [this](const QString& trackId) {
        if (trackId == trackId_)
            readTrackName();
    });
    connections_ << connect(project, &Project::trackRemoved, this, [this] { refresh(); });
    connections_ << connect(project, &Project::returnRemoved, this, [this] { refresh(); });
    connections_ << connect(project, &Project::reset, this, &DeviceCanvas::refresh);
    connections_ << connect(project, &Project::settingsChanged, this, &DeviceCanvas::doSync);  // (the tempo)
    connections_ << connect(project, &Project::deviceStateChanged, this,
                            [this](const QString& trackId, const QString& deviceId) {
                                if (trackId == trackId_ && deviceId == deviceId_ && alive_)
                                    stateChanged();
                            });
    connections_ << connect(project, &Project::automationChanged, this, [this](const QString& owner, const QString&) {
        if (owner == trackId_)
            doSync();
    });
    connections_ << connect(bridge, &EngineBridge::automationStateChanged, this, [this](const QString& owner) {
        if (owner == trackId_ || owner.isEmpty())
            doSync();
    });
    connections_ << connect(bridge, &EngineBridge::devicesLoaded, this, [this](const QString& trackId) {
        if (trackId == trackId_)
            refresh();
    });
    // As the playhead moves, parameters following their automation move.
    connections_ << connect(bridge, &EngineBridge::positionChanged, this, [this] {
        if (following_)
            doSync();
    });
    connections_ << connect(DisplayClock::instance(), &DisplayClock::tick, this, [this] {
        if (alive_ && isVisible() && window())
            refreshDisplays();
    });
}

void DeviceCanvas::itemChange(ItemChange change, const ItemChangeData& data) {
    SgCanvas::itemChange(change, data);
    if (change == ItemVisibleHasChanged && data.boolValue)
        doSync();  // (what changed while hidden)
}

void DeviceCanvas::refresh() {
    readDevice();
    doSync();
}

void DeviceCanvas::readDevice() {
    defaults_.clear();
    automatable_.clear();
    displays_.clear();
    const bool alive = device() != nullptr;
    if (alive) {
        for (const sub::app::ProcessorParam& param : session_->bridge()->deviceParams(trackId_, deviceId_)) {
            defaults_.insert(param.id, param.defaultValue);
            if (param.automatable && !param.hidden && !param.readOnly)
                automatable_.append(param.id);
        }
        const QList<sub::app::ProcessorDisplay> displays = session_->bridge()->processorDisplays(trackId_, deviceId_);
        for (int i = 0; i < displays.size(); ++i)
            displays_.insert(displays[i].id, i);
    }
    if (alive != alive_) {
        alive_ = alive;
        Q_EMIT aliveChanged();
    }
    readTrackName();
}

void DeviceCanvas::readTrackName() {
    const sub::app::Track* track = session_ ? session_->project()->findTrack(trackId_) : nullptr;
    const QString name = track ? track->name : QString();
    if (name == trackName_)
        return;
    trackName_ = name;
    Q_EMIT trackNameChanged();
}

void DeviceCanvas::doSync() {
    following_ = followsAutomation();
    sync();
    Q_EMIT synced();
}

void DeviceCanvas::sync() { update(); }

bool DeviceCanvas::followsAutomation() const {
    if (!alive_)
        return false;
    for (const QString& paramId : automatable_)
        if (automationState(paramId) == QLatin1String("on"))
            return true;
    return false;
}

const sub::app::Device* DeviceCanvas::device() const {
    if (!session_ || trackId_.isEmpty() || deviceId_.isEmpty())
        return nullptr;
    return session_->project()->findDevice(trackId_, deviceId_);
}

QString DeviceCanvas::automationState(const QString& paramId) const {
    if (!session_)
        return {};
    return ui::automationState(*session_->bridge(), trackId_, sub::app::automation::deviceKey(deviceId_, paramId));
}

double DeviceCanvas::value(const QString& paramId) const {
    const sub::app::Device* found = device();
    if (found == nullptr)
        return defaultValue(paramId);
    if (automationState(paramId) == QLatin1String("on")) {
        const auto current = session_->bridge()->currentValue(trackId_, sub::app::automation::deviceKey(deviceId_, paramId));
        if (current)
            return *current;
    }
    return found->params.value(paramId, defaultValue(paramId));
}

double DeviceCanvas::sampleRate() const { return session_ ? session_->bridge()->sampleRate() : 48000.0; }

void DeviceCanvas::setParams(const sub::app::OrderedMap<QString, double>& values, const QString& mergeKey,
                             const QString& text) {
    if (device() == nullptr || values.isEmpty())
        return;
    {
        const QScopedValueRollback<bool> editing(editing_, true);  // (one sync for them all, after)
        session_->editor()->setDeviceParams(trackId_, deviceId_, values, mergeKey, text);
    }
    doSync();
}

void DeviceCanvas::setParam(const QString& paramId, double value, const QString& mergeKey) {
    if (device() == nullptr)
        return;
    {
        const QScopedValueRollback<bool> editing(editing_, true);
        session_->editor()->setDeviceParam(trackId_, deviceId_, paramId, value, mergeKey);
    }
    doSync();
}

void DeviceCanvas::setParamValue(const QString& paramId, double value, const QString& mergeKey, const QString& text) {
    setParams({{paramId, value}}, mergeKey, text);
}

bool DeviceCanvas::secondPressOfDoubleClick(const QMouseEvent* event) {
    return doubleClicks_.isSecondPress(event);
}

void DeviceCanvas::touch(const QString& paramId) {
    if (device() != nullptr)
        session_->editor()->touchParameter(trackId_, sub::app::automation::deviceKey(deviceId_, paramId));
}

std::pair<qint64, std::vector<float>> DeviceCanvas::readDisplayAt(const QString& displayId) {
    std::vector<float> values;
    const auto index = displays_.constFind(displayId);
    if (!session_ || index == displays_.constEnd() || device() == nullptr)
        return {0, values};
    // Kept per processor: one made again (its device's processors were) starts from its own 0.
    const auto processor = session_->bridge()->engineDeviceId(trackId_, deviceId_);
    if (!processor)
        return {0, values};
    const QString key = QString::number(*processor) + QLatin1Char(':') + displayId;
    const quint64 position =
        session_->bridge()->readProcessorDisplay(trackId_, deviceId_, *index, positions_.value(key, 0), values);
    positions_.insert(key, position);
    return {static_cast<qint64>(position) - static_cast<qint64>(values.size()), std::move(values)};
}

std::vector<float> DeviceCanvas::readDisplay(const QString& displayId) { return readDisplayAt(displayId).second; }

}  // namespace sub::ui
