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
using sub::app::EngineBridge;
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
        EngineBridge* bridge = session_->bridge();
        const auto ofTrack = [this](const QString& trackId) {
            if (trackId == trackId_)
                refresh();
        };
        // Its value; what is mapped to it (and their names, a device's, a
        // plug-in's parameters') and over which ranges; its name.
        connections_ << connect(project, &Project::deviceParamChanged, this,
                                [this](const QString& trackId, const QString& deviceId, const QString&) {
                                    if (trackId == trackId_ && deviceId == rackId_)
                                        refresh();
                                });
        connections_ << connect(project, &Project::devicesChanged, this, ofTrack);
        connections_ << connect(project, &Project::reset, this, &RackMacro::refresh);
        connections_ << connect(bridge, &EngineBridge::devicesLoaded, this, ofTrack);
        // Its automation: set, playing or overridden; and while it plays, it follows it.
        connections_ << connect(project, &Project::automationChanged, this,
                                [this](const QString& owner, const QString&) {
                                    if (owner == trackId_)
                                        refresh();
                                });
        connections_ << connect(bridge, &EngineBridge::automationStateChanged, this, [this](const QString& owner) {
            if (owner == trackId_ || owner.isEmpty())
                refresh();
        });
        connections_ << connect(bridge, &EngineBridge::positionChanged, this, [this] {
            if (automation_ == QLatin1String("on"))
                refreshValue();
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

QString RackMacro::key() const { return sub::app::automation::deviceKey(rackId_, sub::app::macroParam(index_)); }

QString RackMacro::toolTip() const {
    const QString tip = mapped() ? mappings_.join(u'\n')
                                 : QStringLiteral("Nothing mapped: right-click a parameter of a device in the rack");
    return name() + u'\n' + tip;
}

void RackMacro::refresh() {
    QString name = QStringLiteral("Macro %1").arg(index_ + 1);
    bool named = false;
    QString automation;
    QStringList mappings;
    QVariantList list;
    QStringList keys;
    QVariantList entries;
    const Project* project = session_ ? session_->project() : nullptr;
    const Device* rack = project ? project->findDevice(trackId_, rackId_) : nullptr;
    if (rack != nullptr && rack->isRack()) {
        name = sub::app::macroName(*rack, index_);
        named = index_ < sub::app::macroCount(*rack) && !rack->macroNames[size_t(index_)].isEmpty();
        const EngineBridge* bridge = session_->bridge();
        automation = bridge->isOverridden(trackId_, key()) ? QStringLiteral("off")
                     : bridge->isAutomated(trackId_, key()) ? QStringLiteral("on")
                                                            : QString();
        for (const sub::app::MacroMapping& mapping : rack->macros) {
            const Device* device = mapping.macro == index_ ? project->findDevice(trackId_, mapping.deviceId) : nullptr;
            if (device == nullptr)
                continue;
            const auto spec = session_->bridge()->paramSpec(
                trackId_, sub::app::automation::deviceKey(device->id, mapping.paramId));
            const QString text = sub::app::deviceName(*device) + QStringLiteral(": ") +
                                 (spec ? spec->name : mapping.paramId);
            mappings << text;
            list << QVariantMap{{QStringLiteral("deviceId"), mapping.deviceId},
                                {QStringLiteral("paramId"), mapping.paramId},
                                {QStringLiteral("name"), text},
                                {QStringLiteral("low"), mapping.low},
                                {QStringLiteral("high"), mapping.high},
                                {QStringLiteral("lowText"), formatTarget(mapping.deviceId, mapping.paramId, mapping.low)},
                                {QStringLiteral("highText"),
                                 formatTarget(mapping.deviceId, mapping.paramId, mapping.high)}};
            keys << mapping.deviceId + u'/' + mapping.paramId;
            entries << QVariantMap{{QStringLiteral("text"), QStringLiteral("Unmap ") + text},
                                   {QStringLiteral("deviceId"), mapping.deviceId},
                                   {QStringLiteral("paramId"), mapping.paramId}};
        }
    }
    entries_ = entries;
    const bool same = name == name_ && named == named_ && automation == automation_ && mappings == mappings_ &&
                      list == mappingList_;
    name_ = name;
    named_ = named;
    automation_ = automation;
    mappings_ = mappings;
    mappingList_ = list;
    if (!same)
        Q_EMIT changed();
    if (keys != mappingKeys_) {
        mappingKeys_ = keys;
        Q_EMIT mappingKeysChanged();
    }
    refreshValue();
}

void RackMacro::refreshValue() {
    double value = 0.0;
    const Project* project = session_ ? session_->project() : nullptr;
    if (const Device* rack = project ? project->findDevice(trackId_, rackId_) : nullptr) {
        std::optional<double> current;
        if (automation_ == QLatin1String("on"))
            current = session_->bridge()->currentValue(trackId_, key());
        value = current ? *current : rack->params.value(sub::app::macroParam(index_), 0.0);
    }
    if (value == value_)
        return;
    value_ = value;
    Q_EMIT changed();
}

void RackMacro::set(double value, const QString& mergeKey) {
    if (session_ && session_->project()->findDevice(trackId_, rackId_))
        session_->editor()->setMacro(trackId_, rackId_, index_, value, mergeKey);
    refresh();
}

void RackMacro::touch() {
    if (session_ && session_->project()->findDevice(trackId_, rackId_))
        session_->editor()->touchParameter(trackId_, key());
}

void RackMacro::rename(const QString& name) {
    if (session_ && session_->project()->findDevice(trackId_, rackId_))
        session_->editor()->renameMacro(trackId_, rackId_, index_, name);
}

QVariantList RackMacro::unmapEntries() const { return entries_; }

void RackMacro::unmap(const QString& deviceId, const QString& paramId) {
    if (session_ && session_->project()->findDevice(trackId_, rackId_))
        session_->editor()->unmapMacro(trackId_, rackId_, deviceId, paramId);
}

void RackMacro::setRange(const QString& deviceId, const QString& paramId, double low, double high,
                         const QString& mergeKey) {
    if (session_ && session_->project()->findDevice(trackId_, rackId_))
        session_->editor()->setMacroRange(trackId_, rackId_, deviceId, paramId, low, high, mergeKey);
}

QString RackMacro::formatTarget(const QString& deviceId, const QString& paramId, double normalized) const {
    if (!session_ || !session_->project()->findDevice(trackId_, deviceId))
        return {};
    const auto spec = session_->bridge()->paramSpec(trackId_, sub::app::automation::deviceKey(deviceId, paramId));
    return spec ? spec->formatNormalized(normalized) : QString();
}

namespace {

int macroCountOf(const sub::app::Session* session, const QString& trackId, const QString& rackId) {
    const Device* rack = session ? session->project()->findDevice(trackId, rackId) : nullptr;
    return rack != nullptr ? sub::app::macroCount(*rack) : 0;
}

}  // namespace

bool RackMacro::canAddMacro() const {
    const int count = macroCountOf(session_, trackId_, rackId_);
    return count > 0 && count < sub::app::kMaxMacroCount;
}

bool RackMacro::canRemoveMacro() const { return macroCountOf(session_, trackId_, rackId_) > 1; }

void RackMacro::addMacro() {
    if (canAddMacro())
        session_->editor()->setMacroCount(trackId_, rackId_, macroCountOf(session_, trackId_, rackId_) + 1);
}

void RackMacro::removeLastMacro() {
    if (canRemoveMacro())
        session_->editor()->setMacroCount(trackId_, rackId_, macroCountOf(session_, trackId_, rackId_) - 1);
}

bool RackMacro::canAutomate() const { return session_ && session_->bridge()->canAutomate(trackId_, key()); }

bool RackMacro::hasEnvelope() const {
    return session_ && session_->project()->hasOwner(trackId_) && !session_->project()->envelope(trackId_, key()).empty();
}

bool RackMacro::isOverridden() const { return session_ && session_->bridge()->isOverridden(trackId_, key()); }

void RackMacro::showAutomation() {
    if (session_ && session_->project()->hasOwner(trackId_))
        session_->editor()->showAutomation(trackId_, key());
}

void RackMacro::deleteAutomation() {
    if (session_ && session_->project()->hasOwner(trackId_))
        session_->editor()->clearEnvelope(trackId_, key());
}

void RackMacro::reEnableAutomation() {
    if (session_)
        session_->bridge()->reEnableAutomation(trackId_);
}

// --- RackMacros ---------------------------------------------------------------------------------

RackMacros::RackMacros(QObject* parent) : QObject(parent) {}

void RackMacros::setSession(sub::app::Session* session) {
    if (session == session_)
        return;
    for (const QMetaObject::Connection& connection : std::as_const(connections_))
        disconnect(connection);
    connections_.clear();
    session_ = session;
    if (session_) {
        Project* project = session_->project();
        connections_ << connect(project, &Project::devicesChanged, this, [this](const QString& trackId) {
            if (trackId == trackId_)
                refresh();
        });
        connections_ << connect(project, &Project::reset, this, &RackMacros::refresh);
    }
    Q_EMIT targetChanged();
    refresh();
}

void RackMacros::setTrackId(const QString& trackId) {
    if (trackId == trackId_)
        return;
    trackId_ = trackId;
    Q_EMIT targetChanged();
    refresh();
}

void RackMacros::setRackId(const QString& rackId) {
    if (rackId == rackId_)
        return;
    rackId_ = rackId;
    Q_EMIT targetChanged();
    refresh();
}

int RackMacros::maximum() const { return sub::app::kMaxMacroCount; }

void RackMacros::add() {
    if (session_ && count_ > 0)
        session_->editor()->setMacroCount(trackId_, rackId_, count_ + 1);
}

void RackMacros::remove() {
    if (session_ && count_ > 1)
        session_->editor()->setMacroCount(trackId_, rackId_, count_ - 1);
}

void RackMacros::refresh() {
    const Device* rack = session_ ? session_->project()->findDevice(trackId_, rackId_) : nullptr;
    const int count = rack != nullptr ? sub::app::macroCount(*rack) : 0;
    if (count == count_)
        return;
    count_ = count;
    Q_EMIT countChanged();
}

}  // namespace sub::ui
