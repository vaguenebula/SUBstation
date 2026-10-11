#pragma once
// Undo commands. Every project mutation goes through one of these.
//
// Snapshots, not deltas: each command holds whole before/after values (a clip
// list, a device tree, an envelope, a track) and puts one or the other in place;
// the project gets copies, so the stack's values are never the live model's.
//
// Continuous gestures (dragging a fader, a tempo value, a loop brace) pass a
// merge key; consecutive commands of the same class with the same key and the
// same target collapse into one undo step, keeping the first's "before" and the
// latest's "after". An empty key never merges. The UI makes one key per gesture
// (QUuid::createUuid().toString()), so a whole drag is one undo step.
//
// The editor reads some commands back (what the previous command on the stack
// was, while a drag merges; what a command would change, to refuse it on a
// frozen track): their accessors give what they hold.

#include "model/Automation.h"
#include "model/Clip.h"
#include "model/Device.h"
#include "model/OrderedMap.h"
#include "model/Project.h"
#include "model/Routing.h"
#include "model/Track.h"

#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUndoCommand>

#include <optional>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace sub::app {

// The id of every command that may merge (QUndoCommand::id()); mergeWith then
// checks the class, the merge key and the target.
inline constexpr int kMergeId = 0x6E1;

// A command's id for its merge key: kMergeId, or -1 (it never merges) for none.
inline int mergeId(const QString& mergeKey) { return mergeKey.isEmpty() ? -1 : kMergeId; }

// Track id -> its clips.
using ClipLists = QMap<QString, std::vector<Clip>>;
// Frozen track id -> what of its frozen audio plays (Freeze::segments; none: all of it).
using FrozenSegments = QMap<QString, std::optional<std::vector<Clip>>>;
// Track id -> its devices, in the order given.
using DeviceLists = OrderedMap<QString, std::vector<Device>>;

// Base for single-value changes: `Target` says what changes (std::monostate:
// nothing in particular, the whole of something), `Value` its value; redo()
// puts the new value in place, undo() the old one. It never merges.
template <typename Target, typename Value>
class ValueCommand : public QUndoCommand {
public:
    const Target& target() const { return target_; }
    const Value& oldValue() const { return old_; }
    const Value& newValue() const { return new_; }

protected:
    ValueCommand(Project* project, const QString& text, Target target, Value old, Value nw)
        : QUndoCommand(text),
          project_(project),
          target_(std::move(target)),
          old_(std::move(old)),
          new_(std::move(nw)) {}

    Project* project_;
    Target target_;
    Value old_;
    Value new_;
};

// One that merges while a gesture is in progress: with the next command of the
// same class (`Self`), the same merge key and the same target.
template <typename Self, typename Target, typename Value>
class MergeableCommand : public ValueCommand<Target, Value> {
public:
    int id() const override { return mergeId(mergeKey_); }

    bool mergeWith(const QUndoCommand* other) override {
        const auto* same = dynamic_cast<const Self*>(other);
        if (same == nullptr) return false;
        const MergeableCommand* next = same;
        if (next->mergeKey_ != mergeKey_ || !(next->target_ == this->target_)) return false;
        this->new_ = next->new_;
        return true;
    }

    const QString& mergeKey() const { return mergeKey_; }

protected:
    MergeableCommand(Project* project, const QString& text, Target target, Value old, Value nw, QString mergeKey)
        : ValueCommand<Target, Value>(project, text, std::move(target), std::move(old), std::move(nw)),
          mergeKey_(std::move(mergeKey)) {}

    QString mergeKey_;
};

// Puts a track into one of the project's lists, at an index (past the end:
// last), and takes it out on undo. `Insert` and `Remove` are that list's
// Project methods: insertTrack and removeTrack, or insertReturn and removeReturn.
template <auto Insert, auto Remove>
class InsertCommand : public QUndoCommand {
public:
    void redo() override { (project_->*Insert)(track_, index_); }
    void undo() override { (project_->*Remove)(track_.id); }

    const Track& track() const { return track_; }
    int index() const { return index_; }

protected:
    InsertCommand(Project* project, Track track, int index, const QString& text)
        : QUndoCommand(text), project_(project), track_(std::move(track)), index_(index) {}

private:
    Project* project_;
    Track track_;
    int index_;
};

// Takes a track out of one of the project's lists, and puts it back where it
// was on undo (`Remove`, `Insert`: as InsertCommand's).
template <auto Remove, auto Insert>
class RemoveCommand : public QUndoCommand {
public:
    void redo() override { saved_ = (project_->*Remove)(trackId_); }
    void undo() override {
        if (saved_) (project_->*Insert)(saved_->first, saved_->second);
    }

    const QString& trackId() const { return trackId_; }

protected:
    RemoveCommand(Project* project, const QString& trackId, const QString& text)
        : QUndoCommand(text), project_(project), trackId_(trackId) {}

private:
    Project* project_;
    QString trackId_;
    std::optional<std::pair<Track, int>> saved_;  // the track and where it was, while removed
};

// Replaces the clip lists of one or more tracks (moves, trims, splits...),
// and, for an edit of a time selection over frozen tracks, what of their
// frozen audio plays (Freeze::segments), so both change (and are undone)
// together. With a merge key (a knob drag in the clip view) consecutive edits
// merge (those of the same tracks).
class SetClipsCommand : public QUndoCommand {
public:
    SetClipsCommand(Project* project, const QString& text, ClipLists before, ClipLists after, QString mergeKey = {},
                    FrozenSegments frozenBefore = {}, FrozenSegments frozenAfter = {});

    int id() const override { return mergeId(mergeKey_); }
    bool mergeWith(const QUndoCommand* other) override;
    void redo() override;
    void undo() override;

    const ClipLists& before() const { return before_; }
    const ClipLists& after() const { return after_; }
    const FrozenSegments& frozenBefore() const { return frozenBefore_; }
    // The frozen tracks whose audio the edit takes along (and what of it plays then).
    const FrozenSegments& frozenAfter() const { return frozenAfter_; }
    const QString& mergeKey() const { return mergeKey_; }

private:
    Project* project_;
    ClipLists before_;
    ClipLists after_;
    QString mergeKey_;
    FrozenSegments frozenBefore_;
    FrozenSegments frozenAfter_;
};

// (track id, device id) -> a device's state (Device::state).
using DeviceStates = QMap<std::pair<QString, QString>, std::optional<QString>>;

// Puts other files in place of some (the File Manager, a hot swap): the clips
// of the tracks playing them (whole lists) and the states of the built-in
// devices naming them (a sampler's sample), together, as one undo step. With
// a merge key (a hot swap trying file after file) consecutive replacements of
// the same tracks and devices merge. `relink`: the files were only found
// somewhere else (the same audio), so frozen tracks may take it.
class ReplaceFilesCommand : public QUndoCommand {
public:
    ReplaceFilesCommand(Project* project, const QString& text, ClipLists before, ClipLists after,
                        DeviceStates statesBefore, DeviceStates statesAfter, QString mergeKey = {},
                        bool relink = false);

    int id() const override { return mergeId(mergeKey_); }
    bool mergeWith(const QUndoCommand* other) override;
    void redo() override;
    void undo() override;

    const ClipLists& before() const { return before_; }
    const ClipLists& after() const { return after_; }
    const DeviceStates& statesBefore() const { return statesBefore_; }
    const DeviceStates& statesAfter() const { return statesAfter_; }
    const QString& mergeKey() const { return mergeKey_; }
    bool relink() const { return relink_; }

private:
    Project* project_;
    ClipLists before_;
    ClipLists after_;
    DeviceStates statesBefore_;
    DeviceStates statesAfter_;
    QString mergeKey_;
    bool relink_;
};

class InsertTrackCommand : public InsertCommand<&Project::insertTrack, &Project::removeTrack> {
public:
    InsertTrackCommand(Project* project, Track track, int index, const QString& text = QStringLiteral("Insert Track"))
        : InsertCommand(project, std::move(track), index, text) {}
};

class RemoveTrackCommand : public RemoveCommand<&Project::removeTrack, &Project::insertTrack> {
public:
    RemoveTrackCommand(Project* project, const QString& trackId, const QString& text = QStringLiteral("Delete Track"))
        : RemoveCommand(project, trackId, text) {}
};

// Puts another track in a track's place, of the same id (a track flattened: an
// audio track now).
class ReplaceTrackCommand : public ValueCommand<std::monostate, Track> {
public:
    ReplaceTrackCommand(Project* project, Track before, Track after, const QString& text);

    void redo() override;
    void undo() override;
};

// Freezes a track (its frozen audio), or unfreezes it (none). Target: the track's id.
class SetFreezeCommand : public ValueCommand<QString, std::optional<Freeze>> {
public:
    SetFreezeCommand(Project* project, const QString& trackId, std::optional<Freeze> old, std::optional<Freeze> nw,
                     const QString& text);

    void redo() override;
    void undo() override;

    const QString& trackId() const { return target_; }
};

// One setting of a track: target (track id, field).
class UpdateTrackCommand
    : public MergeableCommand<UpdateTrackCommand, std::pair<QString, TrackField>, TrackValue> {
public:
    UpdateTrackCommand(Project* project, const QString& trackId, TrackField field, TrackValue old, TrackValue nw,
                       const QString& text, QString mergeKey = {});

    void redo() override;
    void undo() override;

    const QString& trackId() const { return target_.first; }
    TrackField field() const { return target_.second; }
};

// Several settings of one track at once (one change): old and new map fields to
// values. Target: (track id, the fields).
class UpdateTrackFieldsCommand
    : public MergeableCommand<UpdateTrackFieldsCommand, std::pair<QString, QList<TrackField>>, TrackValues> {
public:
    UpdateTrackFieldsCommand(Project* project, const QString& trackId, TrackValues old, TrackValues nw,
                             const QString& text, QString mergeKey = {});

    void redo() override;
    void undo() override;

    const QString& trackId() const { return target_.first; }
};

// One mixer setting on several tracks at once; old and new map track id to
// value. Target: (field, the track ids).
class UpdateTracksCommand
    : public MergeableCommand<UpdateTracksCommand, std::pair<TrackField, QStringList>, QMap<QString, TrackValue>> {
public:
    UpdateTracksCommand(Project* project, TrackField field, QMap<QString, TrackValue> old,
                        QMap<QString, TrackValue> nw, const QString& text, QString mergeKey = {});

    void redo() override;
    void undo() override;

    TrackField field() const { return target_.first; }
};

// A tempo and the clips that go with it (the trims that keep unwarped clips
// from overlapping).
struct TempoState {
    double tempo = 120.0;
    ClipLists clips;  // track id -> its clips

    friend bool operator==(const TempoState&, const TempoState&) = default;
};

// Tempo change plus the clip trims that keep unwarped clips from overlapping.
// While a tempo drag merges, its old state stays the one before the drag, so
// undo restores untrimmed clips. (Its target is fixed: every tempo change.)
class SetTempoCommand : public MergeableCommand<SetTempoCommand, int, TempoState> {
public:
    SetTempoCommand(Project* project, TempoState old, TempoState nw, QString mergeKey = {});

    void redo() override;
    void undo() override;

private:
    void apply(const TempoState& state);
};

// Changes project settings; old and new map fields to values. Target: the fields.
class UpdateSettingsCommand : public MergeableCommand<UpdateSettingsCommand, QList<SettingsField>, SettingsValues> {
public:
    UpdateSettingsCommand(Project* project, SettingsValues old, SettingsValues nw, const QString& text,
                          QString mergeKey = {});

    void redo() override;
    void undo() override;
};

// A track's whole device tree. Target: the track's id.
class SetDevicesCommand : public ValueCommand<QString, std::vector<Device>> {
public:
    SetDevicesCommand(Project* project, const QString& trackId, std::vector<Device> before,
                      std::vector<Device> after, const QString& text);

    void redo() override;
    void undo() override;

    const QString& trackId() const { return target_; }
};

// Changes several tracks' devices in one go: devices moving between them stay
// the same devices (a plug-in isn't loaded again).
class SetChainsCommand : public ValueCommand<std::monostate, DeviceLists> {
public:
    SetChainsCommand(Project* project, DeviceLists before, DeviceLists after, const QString& text);

    void redo() override;
    void undo() override;
};

// One parameter of a device. Target: (track id, device id, param id).
class SetDeviceParamCommand
    : public MergeableCommand<SetDeviceParamCommand, std::tuple<QString, QString, QString>, double> {
public:
    SetDeviceParamCommand(Project* project, const QString& trackId, const QString& deviceId, const QString& paramId,
                          double old, double nw, QString mergeKey = {});

    void redo() override;
    void undo() override;

    const QString& trackId() const { return std::get<0>(target_); }
    const QString& deviceId() const { return std::get<1>(target_); }
    const QString& paramId() const { return std::get<2>(target_); }
};

// Several parameters of a track's devices at once, {(device id, param id):
// value}: a rack's macro and the parameters mapped to it. Target: (track id,
// the parameters).
class SetDeviceParamsCommand
    : public MergeableCommand<SetDeviceParamsCommand, std::pair<QString, QList<DeviceParam>>,
                              QMap<DeviceParam, double>> {
public:
    SetDeviceParamsCommand(Project* project, const QString& trackId, QMap<DeviceParam, double> old,
                           QMap<DeviceParam, double> nw, const QString& text, QString mergeKey = {});

    void redo() override;
    void undo() override;

    const QString& trackId() const { return target_.first; }
};

// A rack chain's name or mixer setting. Target: (track id, chain id, field).
class UpdateChainCommand
    : public MergeableCommand<UpdateChainCommand, std::tuple<QString, QString, ChainField>, ChainValue> {
public:
    UpdateChainCommand(Project* project, const QString& trackId, const QString& chainId, ChainField field,
                       ChainValue old, ChainValue nw, const QString& text, QString mergeKey = {});

    void redo() override;
    void undo() override;

    const QString& trackId() const { return std::get<0>(target_); }
    const QString& chainId() const { return std::get<1>(target_); }
    ChainField field() const { return std::get<2>(target_); }
};

// What a command on one device changes: (track id, device id).
using DeviceTarget = std::pair<QString, QString>;

// A rack's name (none: named by its kind).
class SetDeviceNameCommand : public ValueCommand<DeviceTarget, std::optional<QString>> {
public:
    SetDeviceNameCommand(Project* project, const QString& trackId, const QString& deviceId,
                         std::optional<QString> old, std::optional<QString> nw, const QString& text);

    void redo() override;
    void undo() override;

    const QString& trackId() const { return target_.first; }
    const QString& deviceId() const { return target_.second; }
};

// A rack's macro mappings, and the values of parameters they move ({(device
// id, param id): value}: a range changed puts its parameter where the macro is
// in its new range). Target: (track id, rack id); a drag of a range merges.
struct MacroMappings {
    std::vector<MacroMapping> mappings;
    QMap<DeviceParam, double> values;

    friend bool operator==(const MacroMappings&, const MacroMappings&) = default;
};
class SetMacrosCommand : public MergeableCommand<SetMacrosCommand, std::pair<QString, QString>, MacroMappings> {
public:
    SetMacrosCommand(Project* project, const QString& trackId, const QString& rackId, std::vector<MacroMapping> old,
                     std::vector<MacroMapping> nw, const QString& text, QString mergeKey = {},
                     QMap<DeviceParam, double> oldValues = {}, QMap<DeviceParam, double> newValues = {});

    void redo() override;
    void undo() override;

    const QString& trackId() const { return target_.first; }
    const QString& rackId() const { return target_.second; }

private:
    void apply(const MacroMappings& state);
};

// Replaces a device's state: a plug-in's whole state (loading a preset), a
// built-in device's besides its parameters (a sampler's sample); base64.
class SetDeviceStateCommand : public ValueCommand<DeviceTarget, std::optional<QString>> {
public:
    SetDeviceStateCommand(Project* project, const QString& trackId, const QString& deviceId,
                          std::optional<QString> old, std::optional<QString> nw, const QString& text);

    void redo() override;
    void undo() override;

    const QString& trackId() const { return target_.first; }
    const QString& deviceId() const { return target_.second; }
};

class SetDeviceEnabledCommand : public QUndoCommand {
public:
    SetDeviceEnabledCommand(Project* project, const QString& trackId, const QString& deviceId, bool enabled);

    void redo() override;
    void undo() override;

    const QString& trackId() const { return trackId_; }
    const QString& deviceId() const { return deviceId_; }
    bool enabled() const { return enabled_; }

private:
    Project* project_;
    QString trackId_;
    QString deviceId_;
    bool enabled_;
};

// A device's sidechain (none: none).
class SetDeviceSidechainCommand : public ValueCommand<DeviceTarget, std::optional<Sidechain>> {
public:
    SetDeviceSidechainCommand(Project* project, const QString& trackId, const QString& deviceId,
                              std::optional<Sidechain> old, std::optional<Sidechain> nw, const QString& text);

    void redo() override;
    void undo() override;

    const QString& trackId() const { return target_.first; }
    const QString& deviceId() const { return target_.second; }
};

// A device's MIDI input: the MIDI track whose notes it plays ("": its own track's).
class SetDeviceMidiFromCommand : public ValueCommand<DeviceTarget, QString> {
public:
    SetDeviceMidiFromCommand(Project* project, const QString& trackId, const QString& deviceId, QString old,
                             QString nw, const QString& text);

    void redo() override;
    void undo() override;

    const QString& trackId() const { return target_.first; }
    const QString& deviceId() const { return target_.second; }
};

// Replaces several envelopes at once, {(owner, key): envelope} (moving a time
// range on several lanes). With a merge key, one drag is one undo step.
// Target: the lanes.
class SetEnvelopesCommand
    : public MergeableCommand<SetEnvelopesCommand, QList<LaneRef>, QMap<LaneRef, Envelope>> {
public:
    SetEnvelopesCommand(Project* project, QMap<LaneRef, Envelope> old, QMap<LaneRef, Envelope> nw,
                        const QString& text, QString mergeKey = {});

    void redo() override;
    void undo() override;
};

// Replaces one automation envelope (an empty one: no automation). Dragging
// points passes a merge key, so one drag is one undo step. Target: (owner, key).
class SetEnvelopeCommand : public MergeableCommand<SetEnvelopeCommand, LaneRef, Envelope> {
public:
    SetEnvelopeCommand(Project* project, const QString& owner, const QString& key, Envelope old, Envelope nw,
                       const QString& text, QString mergeKey = {});

    void redo() override;
    void undo() override;

    const QString& owner() const { return target_.first; }
    const QString& automationKey() const { return target_.second; }
};

// Reorders the tracks and changes which group each is in (grouping, ungrouping,
// moving tracks into or out of a group): before and after list every track's
// (id, parent) in order.
class ArrangeTracksCommand : public ValueCommand<std::monostate, TrackTree> {
public:
    ArrangeTracksCommand(Project* project, TrackTree before, TrackTree after, const QString& text);

    void redo() override;
    void undo() override;
};

class InsertReturnCommand : public InsertCommand<&Project::insertReturn, &Project::removeReturn> {
public:
    InsertReturnCommand(Project* project, Track track, int index,
                        const QString& text = QStringLiteral("Insert Return Track"))
        : InsertCommand(project, std::move(track), index, text) {}
};

// Takes a return away; the sends into it are taken away first, in the same macro.
class RemoveReturnCommand : public RemoveCommand<&Project::removeReturn, &Project::insertReturn> {
public:
    RemoveReturnCommand(Project* project, const QString& trackId,
                        const QString& text = QStringLiteral("Delete Return Track"))
        : RemoveCommand(project, trackId, text) {}
};

}  // namespace sub::app
