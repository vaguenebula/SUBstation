// Freezing: freezing, unfreezing and flattening tracks, and what frozen audio
// holds, which can't change (the edits a frozen track, or a track in a frozen
// group, refuses).

#include "editor/EditorSupport.h"
#include "editor/ProjectEditor.h"

#include "model/Commands.h"
#include "model/Ids.h"

#include <QHash>
#include <QUndoStack>

#include <algorithm>

namespace sub::app {

using editing::Macro;

namespace {

// Track ids, each once, in their order.
QStringList distinct(const QStringList& ids) {
    QStringList result;
    for (const QString& id : ids) {
        if (!result.contains(id)) result.append(id);
    }
    return result;
}

}  // namespace

QStringList ProjectEditor::freezeTracks(const OrderedMap<QString, Freeze>& freezes) {
    const Project& p = *project_;
    QStringList frozen;
    for (const auto& [trackId, _] : freezes) {
        if (!p.hasOwner(trackId) || p.freezeProblem(trackId)) continue;
        if (p.hasTrack(trackId)) {  // (a track in a group frozen with it is in the group's audio)
            const QStringList ancestors = p.ancestors(trackId);
            if (std::any_of(ancestors.begin(), ancestors.end(), [&](const QString& a) { return freezes.contains(a); })) {
                continue;
            }
        }
        frozen.append(trackId);
    }
    if (frozen.isEmpty()) return {};
    QStringList arranged;
    for (const QString& id : frozen) {
        if (p.hasTrack(id)) arranged.append(id);
    }
    armTracks(arranged, false);  // a frozen track doesn't record
    const QString text = frozen.size() == 1 ? QStringLiteral("Freeze Track") : QStringLiteral("Freeze Tracks");
    Macro macro(undoStack_, text);
    for (const QString& id : frozen) {
        push(std::make_unique<SetFreezeCommand>(project_, id, std::nullopt, freezes.value(id), text));
    }
    return frozen;
}

QStringList ProjectEditor::unfreezeTracks(const QStringList& trackIds) {
    const Project& p = *project_;
    QStringList thawed;
    for (const QString& id : distinct(trackIds)) {
        if (p.hasOwner(id) && p.frozenBy(id) == id) thawed.append(id);
    }
    if (thawed.isEmpty()) return {};
    const QString text = thawed.size() == 1 ? QStringLiteral("Unfreeze Track") : QStringLiteral("Unfreeze Tracks");
    Macro macro(undoStack_, text);
    for (const QString& id : thawed) {
        push(std::make_unique<SetFreezeCommand>(project_, id, p.track(id).frozen, std::nullopt, text));
    }
    return thawed;
}

QStringList ProjectEditor::flattenTracks(const QStringList& trackIds) {
    const Project& p = *project_;
    QStringList flat;
    for (const QString& id : distinct(trackIds)) {
        if (p.hasTrack(id) && !p.flattenProblem(id)) flat.append(id);
    }
    if (flat.isEmpty()) return {};
    const QString text = flat.size() == 1 ? QStringLiteral("Flatten Track") : QStringLiteral("Flatten Tracks");
    Macro macro(undoStack_, text);
    for (const QString& id : flat) {
        const Track track = p.track(id);
        Track after = track;
        after.kind = kAudioKind;
        Clip clip = track.frozen->clip(id, track.name);
        clip.id = newId();
        after.clips = {clip};
        after.devices.clear();
        after.frozen.reset();
        EnvelopeMap envelopes;
        for (const auto& [key, points] : track.automation) {
            if (!automation::keyDevice(key)) envelopes.insert(key, points);
        }
        after.automation = envelopes;
        AutomationView& view = after.automationView;
        if (view.key && automation::keyDevice(*view.key)) view.key.reset();
        view.lanes.removeIf([](const QString& k) { return automation::keyDevice(k).has_value(); });
        push(std::make_unique<ReplaceTrackCommand>(project_, track, after, text));
    }
    return flat;
}

// --- What frozen audio holds ---

std::optional<QString> ProjectEditor::frozenProblem(const QUndoCommand& command) const {
    // Why a command can't be made: it changes the clips, devices or device
    // automation of a frozen track (or of a track in a frozen group); none: it
    // can. (Taking away a sidechain whose source goes is fine.)
    const Project& p = *project_;
    QStringList tracks;
    QString what = QStringLiteral("devices");
    if (const auto* clips = dynamic_cast<const SetClipsCommand*>(&command)) {
        for (auto it = clips->after().constBegin(); it != clips->after().constEnd(); ++it) {
            if (!clips->before().contains(it.key()) || clips->before().value(it.key()) != it.value()) tracks.append(it.key());
        }
        what = QStringLiteral("clips");
    } else if (const auto* chains = dynamic_cast<const SetChainsCommand*>(&command)) {
        tracks = chains->after().keys();
    } else if (const auto* param = dynamic_cast<const SetDeviceParamCommand*>(&command)) {
        tracks = {param->trackId()};
    } else if (const auto* params = dynamic_cast<const SetDeviceParamsCommand*>(&command)) {
        tracks = {params->trackId()};
    } else if (const auto* chain = dynamic_cast<const UpdateChainCommand*>(&command)) {
        tracks = {chain->trackId()};
    } else if (const auto* sidechain = dynamic_cast<const SetDeviceSidechainCommand*>(&command)) {
        if (sidechain->newValue()) tracks = {sidechain->trackId()};
    } else if (const auto* devices = dynamic_cast<const SetDevicesCommand*>(&command)) {
        tracks = {devices->trackId()};
    } else if (const auto* enabled = dynamic_cast<const SetDeviceEnabledCommand*>(&command)) {
        tracks = {enabled->trackId()};
    } else if (const auto* state = dynamic_cast<const SetDeviceStateCommand*>(&command)) {
        tracks = {state->trackId()};
    } else if (const auto* macros = dynamic_cast<const SetMacrosCommand*>(&command)) {
        tracks = {macros->trackId()};
    } else if (const auto* name = dynamic_cast<const SetDeviceNameCommand*>(&command)) {
        tracks = {name->trackId()};
    } else if (const auto* envelope = dynamic_cast<const SetEnvelopeCommand*>(&command)) {
        if (laneFrozen(envelope->owner(), envelope->automationKey())) tracks = {envelope->owner()};
        what = QStringLiteral("automation");
    } else if (const auto* envelopes = dynamic_cast<const SetEnvelopesCommand*>(&command)) {
        for (const LaneRef& lane : envelopes->newValue().keys()) {
            if (laneFrozen(lane.first, lane.second)) tracks.append(lane.first);
        }
        what = QStringLiteral("automation");
    }
    for (const QString& id : tracks) {
        if (!p.hasOwner(id) || !p.isFrozen(id)) continue;
        return QStringLiteral("%1 is frozen: unfreeze it to change its %2").arg(p.track(*p.frozenBy(id)).name, what);
    }
    return std::nullopt;
}

bool ProjectEditor::laneFrozen(const QString& owner, const QString& key) const {
    const Project& p = *project_;
    const auto holder = p.hasOwner(owner) ? p.frozenBy(owner) : std::nullopt;
    if (!holder || automation::keySend(key)) return false;
    return *holder != owner || !automation::isMixerKey(key);
}

std::pair<int, std::optional<QString>> ProjectEditor::outsideFrozen(int index,
                                                                    const std::optional<QString>& parent) const {
    // Where a track meant for `index` in `parent` goes: there, unless that is in
    // a frozen group (which would then hear it): then after that group, in its group.
    const Project& p = *project_;
    const auto holder = parent ? p.frozenBy(*parent) : std::nullopt;
    if (!holder) return {index, parent};
    return {p.subtreeEnd(p.trackIndex(*holder)), p.track(*holder).parent};
}

std::optional<QString> ProjectEditor::heldProblem(const QStringList& trackIds) const {
    // Why these tracks can't leave their groups: one is in a frozen group.
    const Project& p = *project_;
    for (const QString& id : trackIds) {
        const auto holder = p.frozenBy(id);
        if (holder && *holder != id) {
            return QStringLiteral("%1 is frozen: unfreeze it to change what is in it").arg(p.track(*holder).name);
        }
    }
    return std::nullopt;
}

std::optional<QString> ProjectEditor::arrangementProblem(const TrackTree& tree, const QSet<QString>& going) const {
    // Why the tracks can't be arranged so (none: they can): a track would go
    // into or out of a frozen group (or a group in one), whose audio holds what
    // is in it. (Groups `going` go next: those frozen hold nothing then.)
    const Project& p = *project_;
    QHash<QString, std::optional<QString>> before;
    for (const TreeEntry& entry : p.tree()) before.insert(entry.id, entry.parent);
    QHash<QString, std::optional<QString>> after;
    for (const TreeEntry& entry : tree) after.insert(entry.id, entry.parent);
    const auto holder = [&](const QString& trackId,
                            const QHash<QString, std::optional<QString>>& parents) -> std::optional<QString> {
        std::optional<QString> parent = parents.value(trackId);
        while (parent) {
            const Track* group = p.findTrack(*parent);
            if (!going.contains(*parent) && group != nullptr && group->frozen) return parent;
            parent = parents.value(*parent);
        }
        return std::nullopt;
    };
    for (const TreeEntry& entry : tree) {
        if (entry.parent == before.value(entry.id)) continue;
        auto frozen = holder(entry.id, before);
        if (!frozen) frozen = holder(entry.id, after);
        if (frozen) {
            return QStringLiteral("%1 is frozen: unfreeze it to change what is in it").arg(p.track(*frozen).name);
        }
    }
    return std::nullopt;
}

}  // namespace sub::app
