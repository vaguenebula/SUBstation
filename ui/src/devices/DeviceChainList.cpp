#include "devices/DeviceChainList.h"

#include "session/DeviceSelection.h"

namespace sub::ui {

DeviceChainList::DeviceChainList(QObject* parent) : QObject(parent) {}

void DeviceChainList::setSession(sub::app::Session* session) {
    if (session == session_)
        return;
    disconnect(connection_);
    session_ = session;
    if (session_) {
        connection_ = connect(session_->deviceSelection(), &sub::app::DeviceSelection::changed, this,
                              &DeviceChainList::refresh);
    }
    Q_EMIT targetChanged();
    refresh();
}

void DeviceChainList::setChainId(const QString& chainId) {
    if (chainId == chainId_)
        return;
    chainId_ = chainId;
    Q_EMIT targetChanged();
    refresh();
}

void DeviceChainList::refresh() {
    QString trackId;
    QStringList ids;
    if (session_) {
        const sub::app::DeviceSelection* devices = session_->deviceSelection();
        trackId = devices->trackId();
        const QStringList shown = devices->shownDevices();
        for (const QString& id : devices->chainDevices(chainId_)) {
            if (shown.contains(id))
                ids << id;
        }
    }
    if (trackId == trackId_ && ids == deviceIds_)
        return;
    trackId_ = trackId;
    deviceIds_ = ids;
    Q_EMIT deviceIdsChanged();
}

}  // namespace sub::ui
