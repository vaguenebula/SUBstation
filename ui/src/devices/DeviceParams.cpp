#include "devices/DeviceParams.h"

#include "audio/BridgeTypes.h"
#include "audio/EngineBridge.h"
#include "model/Project.h"

namespace sub::ui {

DeviceParams::DeviceParams(QObject* parent) : QObject(parent) {}

void DeviceParams::setSession(sub::app::Session* session) {
    if (session == session_)
        return;
    for (const QMetaObject::Connection& connection : std::as_const(connections_))
        disconnect(connection);
    connections_.clear();
    session_ = session;
    if (session_) {
        auto follow = [this](const QString& trackId) {
            if (trackId == trackId_)
                refresh();
        };
        connections_ << connect(session_->project(), &sub::app::Project::devicesChanged, this, follow);
        connections_ << connect(session_->bridge(), &sub::app::EngineBridge::devicesLoaded, this, follow);
        connections_ << connect(session_->project(), &sub::app::Project::reset, this, &DeviceParams::refresh);
    }
    Q_EMIT targetChanged();
    refresh();
}

void DeviceParams::setTrackId(const QString& trackId) {
    if (trackId == trackId_)
        return;
    trackId_ = trackId;
    Q_EMIT targetChanged();
    refresh();
}

void DeviceParams::setDeviceId(const QString& deviceId) {
    if (deviceId == deviceId_)
        return;
    deviceId_ = deviceId;
    Q_EMIT targetChanged();
    refresh();
}

void DeviceParams::refresh() {
    QStringList ids;
    if (session_ && session_->project()->findDevice(trackId_, deviceId_)) {
        for (const sub::app::ProcessorParam& param : session_->bridge()->deviceParams(trackId_, deviceId_))
            ids << param.id;
    }
    if (ids == ids_)
        return;
    ids_ = ids;
    Q_EMIT idsChanged();
}

}  // namespace sub::ui
