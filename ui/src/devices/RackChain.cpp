#include "devices/RackChain.h"

#include "audio/BridgeTypes.h"
#include "audio/EngineBridge.h"
#include "controls/Automation.h"
#include "editor/ProjectEditor.h"
#include "model/Automation.h"
#include "model/Device.h"
#include "model/Project.h"
#include "model/Timebase.h"

namespace sub::ui {

using sub::app::EngineBridge;
using sub::app::Project;

RackChain::RackChain(QObject* parent) : QObject(parent) {}

void RackChain::setSession(sub::app::Session* session) {
    if (session == session_)
        return;
    session_ = session;
    connectSession();
    Q_EMIT targetChanged();
    refresh();
}

void RackChain::setTrackId(const QString& trackId) {
    if (trackId == trackId_)
        return;
    trackId_ = trackId;
    Q_EMIT targetChanged();
    refresh();
}

void RackChain::setRackId(const QString& rackId) {
    if (rackId == rackId_)
        return;
    rackId_ = rackId;
    Q_EMIT targetChanged();
    refresh();
}

void RackChain::setChainId(const QString& chainId) {
    if (chainId == chainId_)
        return;
    chainId_ = chainId;
    Q_EMIT targetChanged();
    refresh();
}

QString RackChain::volumeKey() const {
    return sub::app::automation::chainKey(rackId_, chainId_, sub::app::automation::kChainVolume);
}

QString RackChain::panKey() const {
    return sub::app::automation::chainKey(rackId_, chainId_, sub::app::automation::kChainPan);
}

QString RackChain::automationStateOf(const QString& key) const {
    return automationState(*session_->bridge(), trackId_, key);
}

void RackChain::connectSession() {
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
    connections_ << connect(project, &Project::chainChanged, this, [this](const QString& trackId, const QString& chainId) {
        if (trackId == trackId_ && chainId == chainId_)
            refresh();
    });
    connections_ << connect(project, &Project::devicesChanged, this, ofTrack);
    connections_ << connect(project, &Project::reset, this, &RackChain::refresh);
    connections_ << connect(project, &Project::automationChanged, this,
                            [ofTrack](const QString& owner, const QString&) { ofTrack(owner); });
    connections_ << connect(bridge, &EngineBridge::automationStateChanged, this, [this](const QString& owner) {
        if (owner == trackId_ || owner.isEmpty())
            refresh();
    });
    // Volume and pan follow their automation as it plays.
    connections_ << connect(bridge, &EngineBridge::positionChanged, this, [this] {
        if (state_.volumeAutomation == QLatin1String("on") || state_.panAutomation == QLatin1String("on"))
            refresh();
    });
    connections_ << connect(bridge, &EngineBridge::metersUpdated, this, [this] {
        if (!state_.exists)
            return;
        const sub::app::MeterLevel level = session_->bridge()->chainMeter(chainId_);
        Q_EMIT meterUpdated(level.left, level.right);
    });
}

void RackChain::refresh() {
    State s;
    const Project* project = session_ ? session_->project() : nullptr;
    const sub::app::Device* rack = project ? project->findDevice(trackId_, rackId_) : nullptr;
    const sub::app::Chain* chain = nullptr;
    if (rack != nullptr) {
        for (const sub::app::Chain& each : rack->chains) {
            if (each.id == chainId_)
                chain = &each;
        }
    }
    if (chain != nullptr) {
        s.exists = true;
        s.name = chain->name;
        s.mute = chain->mute;
        s.solo = chain->solo;
        s.volumeAutomation = automationStateOf(volumeKey());
        s.panAutomation = automationStateOf(panKey());
        s.volume = chain->volumeDb;
        s.pan = chain->pan;
        // As heard: following their automation while it plays.
        if (s.volumeAutomation == QLatin1String("on"))
            s.volume = session_->bridge()->currentValue(trackId_, volumeKey()).value_or(chain->volumeDb);
        if (s.panAutomation == QLatin1String("on"))
            s.pan = session_->bridge()->currentValue(trackId_, panKey()).value_or(chain->pan);
    }
    if (s == state_)
        return;
    state_ = s;
    Q_EMIT changed();
}

void RackChain::setMute(bool mute) {
    if (state_.exists)
        session_->editor()->setChainParam(trackId_, chainId_, QStringLiteral("mute"), mute ? 1.0 : 0.0);
    refresh();
}

void RackChain::setSolo(bool solo) {
    if (state_.exists)
        session_->editor()->setChainParam(trackId_, chainId_, QStringLiteral("solo"), solo ? 1.0 : 0.0);
    refresh();
}

void RackChain::setVolume(double volumeDb, const QString& mergeKey) {
    if (state_.exists)
        session_->editor()->setChainParam(trackId_, chainId_, QStringLiteral("volume_db"), volumeDb, mergeKey);
    refresh();
}

void RackChain::setPan(double pan, const QString& mergeKey) {
    if (state_.exists)
        session_->editor()->setChainParam(trackId_, chainId_, QStringLiteral("pan"), pan, mergeKey);
    refresh();
}

void RackChain::rename(const QString& name) {
    const QString trimmed = name.trimmed();
    if (state_.exists && !trimmed.isEmpty())
        session_->editor()->renameChain(trackId_, chainId_, trimmed);
}

void RackChain::duplicate() {
    if (state_.exists)
        session_->editor()->duplicateRackChain(trackId_, chainId_);
}

void RackChain::remove() {
    if (state_.exists)
        session_->editor()->removeRackChains(trackId_, {chainId_});
}

void RackChain::addChain() {
    if (session_ && session_->project()->findDevice(trackId_, rackId_))
        session_->editor()->tryAddRackChain(trackId_, rackId_);
}

void RackChain::showVolumeAutomation() {
    if (state_.exists)
        session_->editor()->showAutomation(trackId_, volumeKey());
}

void RackChain::showPanAutomation() {
    if (state_.exists)
        session_->editor()->showAutomation(trackId_, panKey());
}

QString RackChain::formatVolume(double volumeDb) { return sub::app::formatDb(volumeDb); }

QString RackChain::formatPan(double pan) { return sub::app::formatPan(pan); }

QVariant RackChain::parsePan(const QString& text) {
    const std::optional<double> pan = sub::app::parsePan(text);
    return pan ? QVariant(*pan) : QVariant::fromValue(nullptr);
}

}  // namespace sub::ui
