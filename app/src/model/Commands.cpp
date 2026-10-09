#include "model/Commands.h"

namespace sub::app {

namespace {

QList<TrackField> fieldsOf(const TrackValues& values) { return values.keys(); }

QList<SettingsField> fieldsOf(const SettingsValues& values) { return values.keys(); }

}  // namespace

// --- SetClipsCommand ---

SetClipsCommand::SetClipsCommand(Project* project, const QString& text, ClipLists before, ClipLists after,
                                 QString mergeKey, FrozenSegments frozenBefore, FrozenSegments frozenAfter)
    : QUndoCommand(text),
      project_(project),
      before_(std::move(before)),
      after_(std::move(after)),
      mergeKey_(std::move(mergeKey)),
      frozenBefore_(std::move(frozenBefore)),
      frozenAfter_(std::move(frozenAfter)) {}

bool SetClipsCommand::mergeWith(const QUndoCommand* other) {
    const auto* next = dynamic_cast<const SetClipsCommand*>(other);
    if (next == nullptr || next->mergeKey_ != mergeKey_ || next->after_.keys() != after_.keys() ||
        next->frozenAfter_.keys() != frozenAfter_.keys()) {
        return false;
    }
    after_ = next->after_;
    frozenAfter_ = next->frozenAfter_;
    return true;
}

void SetClipsCommand::redo() {
    for (auto it = after_.constBegin(); it != after_.constEnd(); ++it) project_->setClips(it.key(), it.value());
    for (auto it = frozenAfter_.constBegin(); it != frozenAfter_.constEnd(); ++it) {
        project_->setFrozenSegments(it.key(), it.value());
    }
}

void SetClipsCommand::undo() {
    for (auto it = before_.constBegin(); it != before_.constEnd(); ++it) project_->setClips(it.key(), it.value());
    for (auto it = frozenBefore_.constBegin(); it != frozenBefore_.constEnd(); ++it) {
        project_->setFrozenSegments(it.key(), it.value());
    }
}

ReplaceFilesCommand::ReplaceFilesCommand(Project* project, const QString& text, ClipLists before, ClipLists after,
                                         DeviceStates statesBefore, DeviceStates statesAfter, QString mergeKey,
                                         bool relink)
    : QUndoCommand(text),
      project_(project),
      before_(std::move(before)),
      after_(std::move(after)),
      statesBefore_(std::move(statesBefore)),
      statesAfter_(std::move(statesAfter)),
      mergeKey_(std::move(mergeKey)),
      relink_(relink) {}

bool ReplaceFilesCommand::mergeWith(const QUndoCommand* other) {
    const auto* next = dynamic_cast<const ReplaceFilesCommand*>(other);
    if (next == nullptr || next->mergeKey_ != mergeKey_ || next->after_.keys() != after_.keys() ||
        next->statesAfter_.keys() != statesAfter_.keys() || next->relink_ != relink_) {
        return false;
    }
    after_ = next->after_;
    statesAfter_ = next->statesAfter_;
    setText(next->text());  // (what was swapped in last)
    // Back where it began (a hot swap trying the file it started from): no undo step.
    setObsolete(after_ == before_ && statesAfter_ == statesBefore_);
    return true;
}

void ReplaceFilesCommand::redo() {
    for (auto it = after_.constBegin(); it != after_.constEnd(); ++it) project_->setClips(it.key(), it.value());
    for (auto it = statesAfter_.constBegin(); it != statesAfter_.constEnd(); ++it) {
        project_->setDeviceState(it.key().first, it.key().second, it.value());
    }
}

void ReplaceFilesCommand::undo() {
    for (auto it = before_.constBegin(); it != before_.constEnd(); ++it) project_->setClips(it.key(), it.value());
    for (auto it = statesBefore_.constBegin(); it != statesBefore_.constEnd(); ++it) {
        project_->setDeviceState(it.key().first, it.key().second, it.value());
    }
}

// --- Tracks ---

ReplaceTrackCommand::ReplaceTrackCommand(Project* project, Track before, Track after, const QString& text)
    : ValueCommand(project, text, {}, std::move(before), std::move(after)) {}

void ReplaceTrackCommand::redo() { project_->replaceTrack(new_); }

void ReplaceTrackCommand::undo() { project_->replaceTrack(old_); }

SetFreezeCommand::SetFreezeCommand(Project* project, const QString& trackId, std::optional<Freeze> old,
                                   std::optional<Freeze> nw, const QString& text)
    : ValueCommand(project, text, trackId, std::move(old), std::move(nw)) {}

void SetFreezeCommand::redo() { project_->setFrozen(trackId(), new_); }

void SetFreezeCommand::undo() { project_->setFrozen(trackId(), old_); }

UpdateTrackCommand::UpdateTrackCommand(Project* project, const QString& trackId, TrackField field, TrackValue old,
                                       TrackValue nw, const QString& text, QString mergeKey)
    : MergeableCommand(project, text, {trackId, field}, std::move(old), std::move(nw), std::move(mergeKey)) {}

void UpdateTrackCommand::redo() { project_->updateTrack(target_.first, target_.second, new_); }

void UpdateTrackCommand::undo() { project_->updateTrack(target_.first, target_.second, old_); }

UpdateTrackFieldsCommand::UpdateTrackFieldsCommand(Project* project, const QString& trackId, TrackValues old,
                                                   TrackValues nw, const QString& text, QString mergeKey)
    : MergeableCommand(project, text, {trackId, fieldsOf(old)}, old, std::move(nw), std::move(mergeKey)) {}

void UpdateTrackFieldsCommand::redo() { project_->updateTrack(target_.first, new_); }

void UpdateTrackFieldsCommand::undo() { project_->updateTrack(target_.first, old_); }

UpdateTracksCommand::UpdateTracksCommand(Project* project, TrackField field, QMap<QString, TrackValue> old,
                                         QMap<QString, TrackValue> nw, const QString& text, QString mergeKey)
    : MergeableCommand(project, text, {field, old.keys()}, old, std::move(nw), std::move(mergeKey)) {}

void UpdateTracksCommand::redo() {
    for (auto it = new_.constBegin(); it != new_.constEnd(); ++it) project_->updateTrack(it.key(), target_.first, it.value());
}

void UpdateTracksCommand::undo() {
    for (auto it = old_.constBegin(); it != old_.constEnd(); ++it) project_->updateTrack(it.key(), target_.first, it.value());
}

// --- Settings ---

SetTempoCommand::SetTempoCommand(Project* project, TempoState old, TempoState nw, QString mergeKey)
    : MergeableCommand(project, QStringLiteral("Change Tempo"), 0, std::move(old), std::move(nw),
                       std::move(mergeKey)) {}

void SetTempoCommand::apply(const TempoState& state) {
    project_->updateSettings({{SettingsField::Tempo, state.tempo}});
    for (auto it = state.clips.constBegin(); it != state.clips.constEnd(); ++it) {
        if (project_->hasTrack(it.key()) && project_->track(it.key()).clips != it.value()) {
            project_->setClips(it.key(), it.value());
        }
    }
}

void SetTempoCommand::redo() { apply(new_); }

void SetTempoCommand::undo() { apply(old_); }

UpdateSettingsCommand::UpdateSettingsCommand(Project* project, SettingsValues old, SettingsValues nw,
                                             const QString& text, QString mergeKey)
    : MergeableCommand(project, text, fieldsOf(nw), std::move(old), nw, std::move(mergeKey)) {}

void UpdateSettingsCommand::redo() { project_->updateSettings(new_); }

void UpdateSettingsCommand::undo() { project_->updateSettings(old_); }

// --- Devices ---

SetDevicesCommand::SetDevicesCommand(Project* project, const QString& trackId, std::vector<Device> before,
                                     std::vector<Device> after, const QString& text)
    : ValueCommand(project, text, trackId, std::move(before), std::move(after)) {}

void SetDevicesCommand::redo() { project_->setDevices(trackId(), new_); }

void SetDevicesCommand::undo() { project_->setDevices(trackId(), old_); }

SetChainsCommand::SetChainsCommand(Project* project, DeviceLists before, DeviceLists after, const QString& text)
    : ValueCommand(project, text, {}, std::move(before), std::move(after)) {}

void SetChainsCommand::redo() { project_->setChains(new_); }

void SetChainsCommand::undo() { project_->setChains(old_); }

SetDeviceParamCommand::SetDeviceParamCommand(Project* project, const QString& trackId, const QString& deviceId,
                                             const QString& paramId, double old, double nw, QString mergeKey)
    : MergeableCommand(project, QStringLiteral("Change Device Parameter"), {trackId, deviceId, paramId}, old, nw,
                       std::move(mergeKey)) {}

void SetDeviceParamCommand::redo() { project_->setDeviceParam(trackId(), deviceId(), paramId(), new_); }

void SetDeviceParamCommand::undo() { project_->setDeviceParam(trackId(), deviceId(), paramId(), old_); }

SetDeviceParamsCommand::SetDeviceParamsCommand(Project* project, const QString& trackId,
                                               QMap<DeviceParam, double> old, QMap<DeviceParam, double> nw,
                                               const QString& text, QString mergeKey)
    : MergeableCommand(project, text, {trackId, nw.keys()}, std::move(old), nw, std::move(mergeKey)) {}

void SetDeviceParamsCommand::redo() { project_->setDeviceParams(target_.first, new_); }

void SetDeviceParamsCommand::undo() { project_->setDeviceParams(target_.first, old_); }

UpdateChainCommand::UpdateChainCommand(Project* project, const QString& trackId, const QString& chainId,
                                       ChainField field, ChainValue old, ChainValue nw, const QString& text,
                                       QString mergeKey)
    : MergeableCommand(project, text, {trackId, chainId, field}, std::move(old), std::move(nw), std::move(mergeKey)) {}

void UpdateChainCommand::redo() { project_->updateChain(trackId(), chainId(), field(), new_); }

void UpdateChainCommand::undo() { project_->updateChain(trackId(), chainId(), field(), old_); }

SetDeviceNameCommand::SetDeviceNameCommand(Project* project, const QString& trackId, const QString& deviceId,
                                           std::optional<QString> old, std::optional<QString> nw, const QString& text)
    : ValueCommand(project, text, {trackId, deviceId}, std::move(old), std::move(nw)) {}

void SetDeviceNameCommand::redo() { project_->setDeviceName(trackId(), deviceId(), new_); }

void SetDeviceNameCommand::undo() { project_->setDeviceName(trackId(), deviceId(), old_); }

SetMacrosCommand::SetMacrosCommand(Project* project, const QString& trackId, const QString& rackId,
                                   std::vector<MacroMapping> old, std::vector<MacroMapping> nw, const QString& text,
                                   QString mergeKey, QMap<DeviceParam, double> oldValues,
                                   QMap<DeviceParam, double> newValues)
    : MergeableCommand(project, text, {trackId, rackId}, {std::move(old), std::move(oldValues)},
                       {std::move(nw), std::move(newValues)}, std::move(mergeKey)) {}

void SetMacrosCommand::apply(const MacroMappings& state) {
    project_->setDeviceMacros(trackId(), rackId(), state.mappings);
    if (!state.values.isEmpty()) project_->setDeviceParams(trackId(), state.values);
}

void SetMacrosCommand::redo() { apply(new_); }

void SetMacrosCommand::undo() { apply(old_); }

SetDeviceStateCommand::SetDeviceStateCommand(Project* project, const QString& trackId, const QString& deviceId,
                                             std::optional<QString> old, std::optional<QString> nw,
                                             const QString& text)
    : ValueCommand(project, text, {trackId, deviceId}, std::move(old), std::move(nw)) {}

void SetDeviceStateCommand::redo() { project_->setDeviceState(trackId(), deviceId(), new_); }

void SetDeviceStateCommand::undo() { project_->setDeviceState(trackId(), deviceId(), old_); }

SetDeviceEnabledCommand::SetDeviceEnabledCommand(Project* project, const QString& trackId, const QString& deviceId,
                                                 bool enabled)
    : QUndoCommand(enabled ? QStringLiteral("Activate Device") : QStringLiteral("Deactivate Device")),
      project_(project),
      trackId_(trackId),
      deviceId_(deviceId),
      enabled_(enabled) {}

void SetDeviceEnabledCommand::redo() { project_->setDeviceEnabled(trackId_, deviceId_, enabled_); }

void SetDeviceEnabledCommand::undo() { project_->setDeviceEnabled(trackId_, deviceId_, !enabled_); }

SetDeviceSidechainCommand::SetDeviceSidechainCommand(Project* project, const QString& trackId,
                                                     const QString& deviceId, std::optional<Sidechain> old,
                                                     std::optional<Sidechain> nw, const QString& text)
    : ValueCommand(project, text, {trackId, deviceId}, std::move(old), std::move(nw)) {}

void SetDeviceSidechainCommand::redo() { project_->setDeviceSidechain(trackId(), deviceId(), new_); }

void SetDeviceSidechainCommand::undo() { project_->setDeviceSidechain(trackId(), deviceId(), old_); }

// --- Automation ---

SetEnvelopesCommand::SetEnvelopesCommand(Project* project, QMap<LaneRef, Envelope> old, QMap<LaneRef, Envelope> nw,
                                         const QString& text, QString mergeKey)
    : MergeableCommand(project, text, nw.keys(), std::move(old), nw, std::move(mergeKey)) {}

void SetEnvelopesCommand::redo() {
    for (auto it = new_.constBegin(); it != new_.constEnd(); ++it) {
        project_->setEnvelope(it.key().first, it.key().second, it.value());
    }
}

void SetEnvelopesCommand::undo() {
    for (auto it = old_.constBegin(); it != old_.constEnd(); ++it) {
        project_->setEnvelope(it.key().first, it.key().second, it.value());
    }
}

SetEnvelopeCommand::SetEnvelopeCommand(Project* project, const QString& owner, const QString& key, Envelope old,
                                       Envelope nw, const QString& text, QString mergeKey)
    : MergeableCommand(project, text, {owner, key}, std::move(old), std::move(nw), std::move(mergeKey)) {}

void SetEnvelopeCommand::redo() { project_->setEnvelope(owner(), automationKey(), new_); }

void SetEnvelopeCommand::undo() { project_->setEnvelope(owner(), automationKey(), old_); }

// --- Groups and returns ---

ArrangeTracksCommand::ArrangeTracksCommand(Project* project, TrackTree before, TrackTree after, const QString& text)
    : ValueCommand(project, text, {}, std::move(before), std::move(after)) {}

void ArrangeTracksCommand::redo() { project_->arrangeTracks(new_); }

void ArrangeTracksCommand::undo() { project_->arrangeTracks(old_); }

}  // namespace sub::app
