#include "devices/PluginParams.h"

#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "model/Automation.h"
#include "model/Project.h"

#include <algorithm>
#include <cmath>

namespace sub::ui {

using sub::app::EngineBridge;
using sub::app::Project;

// --- PluginParams -----------------------------------------------------------------------------

PluginParams::PluginParams(QObject* parent) : QObject(parent) {}

void PluginParams::setSession(sub::app::Session* session) {
    if (session == session_)
        return;
    for (const QMetaObject::Connection& connection : std::as_const(connections_))
        disconnect(connection);
    connections_.clear();
    session_ = session;
    if (session_) {
        auto ofTrack = [this](const QString& trackId) {
            if (trackId == trackId_)
                refresh();
        };
        auto ofDevice = [this](const QString& trackId, const QString& deviceId) {
            if (trackId == trackId_ && deviceId == deviceId_)
                refresh();
        };
        connections_ << connect(session_->project(), &Project::devicesChanged, this, ofTrack);
        connections_ << connect(session_->project(), &Project::reset, this, &PluginParams::refresh);
        connections_ << connect(session_->bridge(), &EngineBridge::devicesLoaded, this, ofTrack);
        connections_ << connect(session_->bridge(), &EngineBridge::pluginParamsRebuilt, this, ofDevice);
    }
    Q_EMIT targetChanged();
    refresh();
}

void PluginParams::setTrackId(const QString& trackId) {
    if (trackId == trackId_)
        return;
    trackId_ = trackId;
    Q_EMIT targetChanged();
    refresh();
}

void PluginParams::setDeviceId(const QString& deviceId) {
    if (deviceId == deviceId_)
        return;
    deviceId_ = deviceId;
    Q_EMIT targetChanged();
    refresh();
}

QList<int> PluginParams::shown(const QList<sub::app::ProcessorParam>& params) {
    // What a generic editor should offer: what can be automated and isn't the plug-in's own business.
    QList<int> shown;
    for (int i = 0; i < params.size(); ++i) {
        const sub::app::ProcessorParam& p = params[i];
        if (p.automatable && !p.hidden && !p.readOnly)
            shown << i;
    }
    if (shown.isEmpty()) {
        for (int i = 0; i < params.size(); ++i) {
            if (!params[i].hidden && !params[i].readOnly)
                shown << i;
        }
    }
    return shown;
}

void PluginParams::refresh() {
    QList<int> indices;
    bool loaded = false;
    if (session_ && session_->project()->findDevice(trackId_, deviceId_)) {
        loaded = session_->bridge()->engineDeviceId(trackId_, deviceId_).has_value();
        if (loaded)
            indices = shown(session_->bridge()->deviceParams(trackId_, deviceId_));
    }
    if (indices == indices_ && loaded == loaded_)
        return;
    indices_ = indices;
    loaded_ = loaded;
    Q_EMIT indicesChanged();
}

// --- PluginParam ------------------------------------------------------------------------------

PluginParam::PluginParam(QObject* parent) : QObject(parent) {}

void PluginParam::setSession(sub::app::Session* session) {
    if (session == session_)
        return;
    session_ = session;
    connectSession();
    Q_EMIT targetChanged();
    refresh();
}

void PluginParam::setTrackId(const QString& trackId) {
    if (trackId == trackId_)
        return;
    trackId_ = trackId;
    Q_EMIT targetChanged();
    refresh();
}

void PluginParam::setDeviceId(const QString& deviceId) {
    if (deviceId == deviceId_)
        return;
    deviceId_ = deviceId;
    Q_EMIT targetChanged();
    refresh();
}

void PluginParam::setIndex(int index) {
    if (index == index_)
        return;
    index_ = index;
    Q_EMIT targetChanged();
    refresh();
}

void PluginParam::connectSession() {
    for (const QMetaObject::Connection& connection : std::as_const(connections_))
        disconnect(connection);
    connections_.clear();
    if (!session_)
        return;
    Project* project = session_->project();
    EngineBridge* bridge = session_->bridge();
    auto ofTrack = [this](const QString& trackId) {
        if (trackId == trackId_)
            refresh();
    };
    auto valuesOf = [this](const QString& trackId, const QString& deviceId) {
        if (trackId == trackId_ && deviceId == deviceId_) {
            refreshValue();
            refreshAutomation();
        }
    };
    // Values the plug-in changed itself (its editor, automation playing), a
    // preset loaded into it, an edit (or its undo) the bridge passed on to it.
    connections_ << connect(bridge, &EngineBridge::pluginParamsChanged, this, valuesOf);
    connections_ << connect(project, &Project::deviceStateChanged, this, valuesOf);
    connections_ << connect(project, &Project::deviceParamChanged, this,
                            [valuesOf](const QString& trackId, const QString& deviceId, const QString&) {
                                valuesOf(trackId, deviceId);
                            });
    // Its processor made again, or its parameters rebuilt: read it all again.
    connections_ << connect(bridge, &EngineBridge::devicesLoaded, this, ofTrack);
    connections_ << connect(project, &Project::devicesChanged, this, ofTrack);
    connections_ << connect(bridge, &EngineBridge::pluginParamsRebuilt, this,
                            [this](const QString& trackId, const QString& deviceId) {
                                if (trackId == trackId_ && deviceId == deviceId_)
                                    refresh();
                            });
    connections_ << connect(project, &Project::reset, this, &PluginParam::refresh);
    connections_ << connect(project, &Project::automationChanged, this, [this](const QString& owner, const QString&) {
        if (owner == trackId_)
            refreshAutomation();
    });
    connections_ << connect(bridge, &EngineBridge::automationStateChanged, this, [this](const QString& owner) {
        if (owner == trackId_ || owner.isEmpty())
            refreshAutomation();
    });
}

void PluginParam::refresh() {
    std::optional<sub::app::ProcessorParam> spec;
    if (session_ && index_ >= 0 && session_->project()->findDevice(trackId_, deviceId_)) {
        const QList<sub::app::ProcessorParam> params = session_->bridge()->deviceParams(trackId_, deviceId_);
        if (index_ < params.size())
            spec = params[index_];
    }
    if (spec != spec_) {
        spec_ = spec;
        Q_EMIT specChanged();
        text_.clear();  // (read again below)
    }
    refreshValue();
    refreshAutomation();
}

void PluginParam::refreshValue() {
    double value = defaultValue();
    QString text;
    if (session_ && spec_) {
        value = session_->bridge()->deviceParamValue(trackId_, deviceId_, index_).value_or(defaultValue());
        text = format(value);
    }
    if (value == value_ && text == text_)
        return;
    value_ = value;
    text_ = text;
    Q_EMIT valueChanged();
}

void PluginParam::refreshAutomation() {
    QString state;
    if (session_ && spec_) {
        const QString key = sub::app::automation::deviceKey(deviceId_, spec_->id);
        if (session_->bridge()->isOverridden(trackId_, key))
            state = QStringLiteral("off");
        else if (session_->bridge()->isAutomated(trackId_, key))
            state = QStringLiteral("on");
    }
    if (state == automation_)
        return;
    automation_ = state;
    Q_EMIT automationChanged();
}

bool PluginParam::bipolar() const {
    return spec_ && spec_->steps == 0 &&
           std::abs(spec_->defaultValue - 0.5 * (spec_->minValue + spec_->maxValue)) < 1e-6;
}

int PluginParam::listIndex() const {
    const int count = int(labels().size());
    const int index = static_cast<int>(std::nearbyint(value_));
    return count ? std::clamp(index, 0, count - 1) : index;
}

QString PluginParam::format(double value) const {
    if (!session_ || !spec_)
        return QString();
    return session_->bridge()->deviceParamText(trackId_, deviceId_, index_, value);
}

void PluginParam::set(double value, const QString& mergeKey) {
    if (!session_ || !spec_ || !session_->project()->findDevice(trackId_, deviceId_))
        return;
    const std::optional<double> old = session_->bridge()->deviceParamValue(trackId_, deviceId_, index_);
    session_->editor()->setDeviceParam(trackId_, deviceId_, spec_->id, value, mergeKey, old);
    refreshValue();
}

void PluginParam::touch() {
    if (session_ && spec_ && session_->project()->hasOwner(trackId_))
        session_->editor()->touchParameter(trackId_, sub::app::automation::deviceKey(deviceId_, spec_->id));
}

}  // namespace sub::ui
