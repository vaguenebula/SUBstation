#include "devices/RackMacro.h"

#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "model/Automation.h"
#include "model/Device.h"
#include "model/Devices.h"
#include "model/Project.h"

#include <QVariantMap>

namespace sub::ui {

using sub::app::Device;
using sub::app::Project;

RackMacro::RackMacro(QObject* parent) : QObject(parent) {}

void RackMacro::setSession(sub::app::Session* session) {
    if (session == session_)
        return;
    for (const QMetaObject::Connection& connection : std::as_const(connections_))
        disconnect(connection);
    connections_.clear();
    session_ = session;
    if (session_) {
        Project* project = session_->project();
        // Its value; what is mapped to it (and their names, a device's, a plug-in's parameters').
        connections_ << connect(project, &Project::deviceParamChanged, this,
                                [this](const QString& trackId, const QString& deviceId, const QString&) {
                                    if (trackId == trackId_ && deviceId == rackId_)
                                        refresh();
                                });
        connections_ << connect(project, &Project::devicesChanged, this, [this](const QString& trackId) {
            if (trackId == trackId_)
                refresh();
        });
        connections_ << connect(project, &Project::reset, this, &RackMacro::refresh);
        connections_ << connect(session_->bridge(), &sub::app::EngineBridge::devicesLoaded, this,
                                [this](const QString& trackId) {
                                    if (trackId == trackId_)
                                        refresh();
                                });
    }
    Q_EMIT targetChanged();
    refresh();
}

void RackMacro::setTrackId(const QString& trackId) {
    if (trackId == trackId_)
        return;
    trackId_ = trackId;
    Q_EMIT targetChanged();
    refresh();
}

void RackMacro::setRackId(const QString& rackId) {
    if (rackId == rackId_)
        return;
    rackId_ = rackId;
    Q_EMIT targetChanged();
    refresh();
}

void RackMacro::setIndex(int index) {
    if (index == index_)
        return;
    index_ = index;
    Q_EMIT targetChanged();
    refresh();
}

QString RackMacro::toolTip() const {
    const QString tip = mapped() ? mappings_.join(u'\n')
                                 : QStringLiteral("Nothing mapped: right-click a parameter of a device in the rack");
    return name() + u'\n' + tip;
}

void RackMacro::refresh() {
    double value = 0.0;
    QStringList mappings;
    QVariantList entries;
    const Project* project = session_ ? session_->project() : nullptr;
    const Device* rack = project ? project->findDevice(trackId_, rackId_) : nullptr;
    if (rack != nullptr) {
        value = rack->params.value(sub::app::macroParam(index_), 0.0);
        for (const sub::app::MacroMapping& mapping : rack->macros) {
            const Device* device = mapping.macro == index_ ? project->findDevice(trackId_, mapping.deviceId) : nullptr;
            if (device == nullptr)
                continue;
            const auto spec = session_->bridge()->paramSpec(
                trackId_, sub::app::automation::deviceKey(device->id, mapping.paramId));
            const QString text = sub::app::deviceName(*device) + QStringLiteral(": ") +
                                 (spec ? spec->name : mapping.paramId);
            mappings << text;
            entries << QVariantMap{{QStringLiteral("text"), QStringLiteral("Unmap ") + text},
                                   {QStringLiteral("deviceId"), mapping.deviceId},
                                   {QStringLiteral("paramId"), mapping.paramId}};
        }
    }
    entries_ = entries;
    if (value == value_ && mappings == mappings_)
        return;
    value_ = value;
    mappings_ = mappings;
    Q_EMIT changed();
}

void RackMacro::set(double value, const QString& mergeKey) {
    if (session_ && session_->project()->findDevice(trackId_, rackId_))
        session_->editor()->setMacro(trackId_, rackId_, index_, value, mergeKey);
    refresh();
}

QVariantList RackMacro::unmapEntries() const { return entries_; }

void RackMacro::unmap(const QString& deviceId, const QString& paramId) {
    if (session_ && session_->project()->findDevice(trackId_, rackId_))
        session_->editor()->unmapMacro(trackId_, rackId_, deviceId, paramId);
}

}  // namespace sub::ui
