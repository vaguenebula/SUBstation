#include "devices/RackChains.h"

#include "editor/ProjectEditor.h"
#include "model/Device.h"
#include "model/Project.h"
#include "session/DeviceSelection.h"

namespace sub::ui {

using sub::app::Project;

RackChains::RackChains(QObject* parent) : QObject(parent) {}

void RackChains::setSession(sub::app::Session* session) {
    if (session == session_)
        return;
    for (const QMetaObject::Connection& connection : std::as_const(connections_))
        disconnect(connection);
    connections_.clear();
    session_ = session;
    if (session_) {
        connections_ << connect(session_->project(), &Project::devicesChanged, this, [this](const QString& trackId) {
            if (trackId == trackId_)
                refresh();
        });
        connections_ << connect(session_->project(), &Project::reset, this, &RackChains::refresh);
        connections_ << connect(session_->deviceSelection(), &sub::app::DeviceSelection::changed, this,
                                &RackChains::refresh);
    }
    Q_EMIT targetChanged();
    refresh();
}

void RackChains::setTrackId(const QString& trackId) {
    if (trackId == trackId_)
        return;
    trackId_ = trackId;
    Q_EMIT targetChanged();
    refresh();
}

void RackChains::setRackId(const QString& rackId) {
    if (rackId == rackId_)
        return;
    rackId_ = rackId;
    Q_EMIT targetChanged();
    refresh();
}

void RackChains::addChain() {
    if (session_ && session_->project()->findDevice(trackId_, rackId_))
        session_->editor()->tryAddRackChain(trackId_, rackId_);
}

void RackChains::refresh() {
    QStringList ids;
    QString shown;
    const sub::app::Device* rack = session_ ? session_->project()->findDevice(trackId_, rackId_) : nullptr;
    if (rack != nullptr) {
        for (const sub::app::Chain& chain : rack->chains)
            ids << chain.id;
        if (session_->deviceSelection()->trackId() == trackId_)
            shown = session_->deviceSelection()->shownChain(rackId_);
    }
    // (Both changed before either is said: a chain shown is always one of them.)
    const bool idsChanged = ids != chainIds_;
    const bool shownChanged = shown != shownChain_;
    chainIds_ = ids;
    shownChain_ = shown;
    if (shownChanged)
        Q_EMIT shownChainChanged();
    if (idsChanged)
        Q_EMIT chainIdsChanged();
}

}  // namespace sub::ui
