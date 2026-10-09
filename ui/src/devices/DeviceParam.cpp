#include "devices/DeviceParam.h"

#include "audio/EngineBridge.h"
#include "controls/Automation.h"
#include "controls/ValueBoxItem.h"
#include "editor/ProjectEditor.h"
#include "model/Automation.h"
#include "model/Device.h"
#include "model/Project.h"

#include <QLocale>

#include <algorithm>
#include <cmath>

namespace sub::ui {

using sub::app::EngineBridge;
using sub::app::Project;

DeviceParam::DeviceParam(QObject* parent) : QObject(parent) {}

void DeviceParam::setSession(sub::app::Session* session) {
    if (session == session_)
        return;
    session_ = session;
    connectSession();
    Q_EMIT targetChanged();
    refresh();
}

void DeviceParam::setTrackId(const QString& trackId) {
    if (trackId == trackId_)
        return;
    trackId_ = trackId;
    Q_EMIT targetChanged();
    refresh();
}

void DeviceParam::setDeviceId(const QString& deviceId) {
    if (deviceId == deviceId_)
        return;
    deviceId_ = deviceId;
    Q_EMIT targetChanged();
    refresh();
}

void DeviceParam::setParamId(const QString& paramId) {
    if (paramId == paramId_)
        return;
    paramId_ = paramId;
    Q_EMIT targetChanged();
    refresh();
}

QString DeviceParam::key() const { return sub::app::automation::deviceKey(deviceId_, paramId_); }

void DeviceParam::connectSession() {
    for (const QMetaObject::Connection& connection : std::as_const(connections_))
        disconnect(connection);
    connections_.clear();
    if (!session_)
        return;
    Project* project = session_->project();
    EngineBridge* bridge = session_->bridge();
    connections_ << connect(project, &Project::deviceParamChanged, this,
                            [this](const QString& trackId, const QString& deviceId, const QString& paramId) {
                                if (trackId == trackId_ && deviceId == deviceId_ && paramId == paramId_)
                                    refreshValue();
                            });
    // The device (or its processor) came or went, was replaced (a preset): read it all again.
    connections_ << connect(project, &Project::devicesChanged, this, [this](const QString& trackId) {
        if (trackId == trackId_)
            refresh();
    });
    connections_ << connect(project, &Project::reset, this, &DeviceParam::refresh);
    connections_ << connect(bridge, &EngineBridge::devicesLoaded, this, [this](const QString& trackId) {
        if (trackId == trackId_)
            refresh();
    });
    connections_ << connect(project, &Project::automationChanged, this, [this](const QString& owner, const QString&) {
        if (owner == trackId_) {
            refreshAutomation();
            refreshValue();
        }
    });
    connections_ << connect(bridge, &EngineBridge::automationStateChanged, this, [this](const QString& owner) {
        if (owner == trackId_ || owner.isEmpty()) {
            refreshAutomation();
            refreshValue();
        }
    });
    // While its automation plays, it follows it.
    connections_ << connect(bridge, &EngineBridge::positionChanged, this, [this] {
        if (automation_ == QLatin1String("on"))
            refreshValue();
    });
}

void DeviceParam::refresh() {
    refreshSpec();
    refreshAutomation();
    refreshValue();
}

void DeviceParam::refreshSpec() {
    std::optional<sub::app::ProcessorParam> spec;
    if (session_ && !paramId_.isEmpty() && session_->project()->findDevice(trackId_, deviceId_)) {
        for (const sub::app::ProcessorParam& param : session_->bridge()->deviceParams(trackId_, deviceId_)) {
            if (param.id == paramId_) {
                spec = param;
                break;
            }
        }
    }
    if (spec == spec_)
        return;
    spec_ = spec;
    Q_EMIT specChanged();
    Q_EMIT valueChanged();  // (its text, in its units)
}

void DeviceParam::refreshAutomation() {
    const QString state = session_ && !deviceId_.isEmpty() ? automationState(*session_->bridge(), trackId_, key())
                                                            : QString();
    if (state == automation_)
        return;
    automation_ = state;
    Q_EMIT automationChanged();
}

void DeviceParam::refreshValue() {
    double value = defaultValue();
    if (session_) {
        if (const sub::app::Device* device = session_->project()->findDevice(trackId_, deviceId_)) {
            std::optional<double> current;
            if (automation_ == QLatin1String("on"))
                current = session_->bridge()->currentValue(trackId_, key());
            value = current ? *current : device->params.value(paramId_, defaultValue());
        }
    }
    if (value == value_)
        return;
    value_ = value;
    Q_EMIT valueChanged();
}

QString DeviceParam::name() const { return spec_ ? spec_->name : paramId_; }
QString DeviceParam::unit() const { return spec_ ? spec_->unit : QString(); }
double DeviceParam::minimum() const { return spec_ ? spec_->minValue : 0.0; }
double DeviceParam::maximum() const { return spec_ ? spec_->maxValue : 1.0; }
double DeviceParam::defaultValue() const { return spec_ ? spec_->defaultValue : 0.0; }
bool DeviceParam::logScale() const { return spec_ && spec_->logScale; }
int DeviceParam::steps() const { return spec_ ? spec_->steps : 0; }
QStringList DeviceParam::labels() const { return spec_ ? spec_->valueLabels : QStringList(); }

bool DeviceParam::bipolar() const {
    if (!spec_)
        return false;
    const QString& u = spec_->unit;
    return spec_->minValue < 0 && 0 < spec_->maxValue &&
           (u.isEmpty() || u == QLatin1String("st") || u == QLatin1String("ct"));
}

int DeviceParam::index() const {
    const QStringList names = labels();
    const int index = static_cast<int>(std::nearbyint(value_));
    return names.isEmpty() ? index : std::clamp(index, 0, int(names.size()) - 1);
}

QString DeviceParam::format(double value) const {
    const QStringList names = labels();
    if (!names.isEmpty())
        return names.at(std::clamp(static_cast<int>(std::nearbyint(value)), 0, int(names.size()) - 1));
    return sub::app::formatValue(value, unit());
}

QVariant DeviceParam::parse(const QString& text) const {
    if (unit() == QLatin1String("Hz")) {  // a frequency: "1.5k", "1500 Hz", "2 kHz"
        QString cleaned = text.trimmed().toLower();
        if (cleaned.endsWith(QLatin1String("hz")))
            cleaned = cleaned.chopped(2).trimmed();
        double scale = 1.0;
        if (cleaned.endsWith(QLatin1Char('k'))) {
            cleaned = cleaned.chopped(1).trimmed();
            scale = 1000.0;
        }
        bool ok = false;
        const double value = QLocale::c().toDouble(cleaned, &ok);
        return ok && std::isfinite(value) ? QVariant(value * scale) : QVariant::fromValue(nullptr);
    }
    return ValueBoxItem::parseNumber(text);
}

void DeviceParam::set(double value, const QString& mergeKey) {
    if (!session_ || !session_->project()->findDevice(trackId_, deviceId_))
        return;
    if (steps() > 0 || isList())
        value = std::nearbyint(value);
    session_->editor()->setDeviceParam(trackId_, deviceId_, paramId_, value, mergeKey);
    refreshValue();
}

void DeviceParam::touch() {
    if (session_ && session_->project()->hasOwner(trackId_))
        session_->editor()->touchParameter(trackId_, key());
}

AutomationTarget DeviceParam::automationTarget() const { return {session_, trackId_, key()}; }

bool DeviceParam::canAutomate() const { return automationTarget().canAutomate(); }

bool DeviceParam::hasEnvelope() const { return automationTarget().hasEnvelope(); }

bool DeviceParam::isOverridden() const { return automationTarget().isOverridden(); }

void DeviceParam::showAutomation() { automationTarget().show(); }

void DeviceParam::deleteAutomation() { automationTarget().deleteEnvelope(); }

void DeviceParam::reEnableAutomation() { automationTarget().reEnable(); }

QString DeviceParam::rackId() const {
    if (!session_)
        return {};
    const Project* project = session_->project();
    const sub::app::Track* track = project->findTrack(trackId_);
    if (!track)
        return {};
    const std::optional<QString> chain = sub::app::containerOf(track->devices, deviceId_);
    if (!chain)
        return {};
    try {
        return project->chainRack(trackId_, *chain).id;
    } catch (const std::exception&) {
        return {};
    }
}

QStringList DeviceParam::macroNames() const {
    QStringList names;
    const QString rack = rackId();
    const sub::app::Device* device = rack.isEmpty() ? nullptr : session_->project()->findDevice(trackId_, rack);
    for (int i = 0; device != nullptr && i < sub::app::macroCount(*device); ++i)
        names << sub::app::macroName(*device, i);
    return names;
}

int DeviceParam::macro() const {
    if (!session_)
        return -1;
    const auto mapped = session_->editor()->macroOf(trackId_, deviceId_, paramId_);
    return mapped ? mapped->second : -1;
}

QString DeviceParam::macroRack() const {
    if (!session_)
        return {};
    const auto mapped = session_->editor()->macroOf(trackId_, deviceId_, paramId_);
    return mapped ? mapped->first : QString();
}

void DeviceParam::mapToMacro(int index) {
    const QString rack = rackId();
    if (session_ && !rack.isEmpty())
        session_->editor()->tryMapMacro(trackId_, rack, index, deviceId_, paramId_);
}

void DeviceParam::unmapFromMacro() {
    if (!session_)
        return;
    const auto mapped = session_->editor()->macroOf(trackId_, deviceId_, paramId_);
    if (mapped)
        session_->editor()->unmapMacro(trackId_, mapped->first, deviceId_, paramId_);
}

}  // namespace sub::ui
