// Editing tracks: adding, deleting, copying and duplicating them, returns and
// sends, groups, a track's mixer, inputs and monitoring, and recorded takes.

#include "editor/EditorSupport.h"
#include "editor/ProjectEditor.h"

#include "model/Commands.h"
#include "model/Devices.h"
#include "model/Edits.h"
#include "model/Errors.h"
#include "model/Ids.h"
#include "model/Notes.h"
#include "model/Numbers.h"
#include "model/Routing.h"
#include "model/TrackNames.h"

#include <QFileInfo>
#include <QHash>
#include <QUndoStack>

#include <algorithm>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace sub::app {

using editing::clampIndex;
using editing::Macro;
using editing::skeleton;

namespace {

// An automation key with the devices (and rack chains) it names renamed by `ids`.
QString renamedKey(const QString& key, const QHash<QString, QString>& ids) {
    const auto parts = automation::parseKey(key);
    if (!parts || parts->kind != QStringLiteral("device")) return key;
    const QString device = ids.value(parts->id, parts->id);
    if (const auto chain = automation::keyChainControl(key)) {
        return automation::chainKey(device, ids.value(chain->chainId, chain->chainId), chain->control);
    }
    return automation::deviceKey(device, parts->param);
}

QStringList deviceAndChainIds(const Device& device) {
    std::vector<Device> holder{device};
    QStringList ids;
    for (const Device* d : iterDevices(holder)) ids.append(d->id);
    for (const ConstRackChain& rc : iterChains(std::as_const(holder))) ids.append(rc.chain->id);
    return ids;
}

QString frozenHolderText(const QString& name) {
    return QStringLiteral("%1 is frozen: unfreeze it to change what is in it").arg(name);
}

}  // namespace

// --- Adding tracks ---

QString ProjectEditor::insertTrack(Track track, int index, const TrackParent& parent, const QString& text,
                                   bool groupColor) {
    const Project& p = *project_;
    const auto& tracks = p.tracks();
    index = clampIndex(index, static_cast<int>(tracks.size()));
    track.parent = std::holds_alternative<AtIndex>(parent) ? p.parentAt(index) : std::get<std::optional<QString>>(parent);
    // A group it can't be in there gives way to the one there.
    std::vector<Track> trial;
    trial.reserve(tracks.size() + 1);
    for (int i = 0; i < static_cast<int>(tracks.size()); ++i) {
        if (i == index) trial.push_back(skeleton(track));
        trial.push_back(skeleton(tracks[i]));
    }
    if (index == static_cast<int>(tracks.size())) trial.push_back(skeleton(track));
    if (treeProblem(trial)) track.parent = p.parentAt(index);
    std::tie(index, track.parent) = outsideFrozen(index, track.parent);
    if (groupColor && track.parent && p.hasTrack(*track.parent)) track.color = p.track(*track.parent).color;
    const QString id = track.id;
    push(std::make_unique<InsertTrackCommand>(project_, std::move(track), index, text));
    return p.hasTrack(id) ? id : QString();
}

InsertionPoint ProjectEditor::insertionPoint(const QString& trackId) const {
    if (trackId.isEmpty() || !project_->hasTrack(trackId)) return {};
    const int index = project_->trackIndex(trackId);
    return {project_->subtreeEnd(index), project_->track(trackId).parent};
}

QString ProjectEditor::addAudioTrack(int index, const QString& name) { return addAudioTrack(index, name, kAtIndex); }

QString ProjectEditor::addAudioTrack(int index, const QString& name, const TrackParent& parent) {
    const Project& p = *project_;
    Track track;
    track.id = newId();
    track.nameTemplate = name.isEmpty() ? contentsName(track) : name;  // ("# Audio": its clips name it)
    track.color = p.nextColor();
    return insertTrack(std::move(track), index, parent, QStringLiteral("Insert Audio Track"), true);
}

QString ProjectEditor::addMidiTrack(int index, const QString& name) {
    return addMidiTrack(index, name, kDefaultInstrument);
}

QString ProjectEditor::addMidiTrack(int index, const QString& name, const QString& instrument,
                                    const std::optional<PluginRef>& plugin, const TrackParent& parent) {
    const Project& p = *project_;
    Track track;
    track.id = newId();
    track.color = p.nextColor();
    track.kind = kMidiKind;
    if (plugin) {
        track.devices.push_back(newDeviceOf(kPluginKind, plugin));
    } else if (!instrument.isEmpty()) {
        track.devices.push_back(newDeviceOf(instrument));
    }
    track.nameTemplate = name.isEmpty() ? contentsName(track) : name;  // ("# Synth": its instrument names it)
    const QString pluginDevice = plugin ? track.devices.front().id : QString();
    const QString id = insertTrack(std::move(track), index, parent, QStringLiteral("Insert MIDI Track"), true);
    if (plugin && !id.isEmpty()) Q_EMIT pluginAdded(id, pluginDevice);
    return id;
}

QString ProjectEditor::addMidiTrackWith(const Device& device, int index, const TrackParent& parent,
                                        const QString& text) {
    Macro macro(undoStack_, text);
    const QString id = addMidiTrack(index, {}, QString(), std::nullopt, parent);
    if (!id.isEmpty()) insertDevice(id, device, -1, {}, text, !device.isRack());
    return id;
}

// --- Deleting tracks ---

void ProjectEditor::deleteTracks(const QStringList& trackIds) {
    const Project& p = *project_;
    QSet<QString> doomed;
    for (const QString& id : trackIds) {
        if (p.hasTrack(id)) doomed.insert(id);
    }
    for (const QString& id : QSet<QString>(doomed)) {
        for (const Track* d : p.descendants(id)) doomed.insert(d->id);
    }
    QSet<QString> returns;
    for (const QString& id : trackIds) {
        if (p.hasReturn(id)) returns.insert(id);
    }
    if (doomed.isEmpty() && returns.isEmpty()) return;
    for (const Track& track : p.tracks()) {  // what is in a frozen group is in its audio
        if (!doomed.contains(track.id)) continue;
        const auto holder = p.frozenBy(track.id);
        if (holder && !doomed.contains(*holder)) {
            Q_EMIT refused(frozenHolderText(p.track(*holder).name));
            return;
        }
    }
    const auto count = doomed.size() + returns.size();
    const QString text = doomed.isEmpty() && count == 1 ? QStringLiteral("Delete Return Track")
                         : count == 1                   ? QStringLiteral("Delete Track")
                                                        : QStringLiteral("Delete Tracks");
    Macro macro(undoStack_, text);
    const QSet<QString> going = doomed | returns;
    dropInputs(going, text);  // (first: undo brings them back after their sources)
    dropSidechains(going, text);
    // The last first: undo brings back each group before what is in it.
    QStringList removed;
    for (auto it = p.tracks().rbegin(); it != p.tracks().rend(); ++it) {
        if (doomed.contains(it->id)) removed.append(it->id);
    }
    for (const QString& id : removed) push(std::make_unique<RemoveTrackCommand>(project_, id, text));
    if (returns.isEmpty()) return;
    QStringList senders;
    for (const Track* t : p.senders()) senders.append(t->id);
    for (const QString& id : senders) {
        const Track& track = p.track(id);
        SendMap kept;
        for (auto it = track.sends.constBegin(); it != track.sends.constEnd(); ++it) {
            if (!returns.contains(it.key())) kept.insert(it.key(), it.value());
        }
        if (!returns.contains(id) && kept != track.sends) {
            push(std::make_unique<UpdateTrackCommand>(project_, id, TrackField::Sends, track.sends, kept, text));
        }
        QStringList keys;
        for (const auto& [key, points] : p.track(id).automation) {
            const auto send = automation::keySend(key);
            if (send && returns.contains(*send)) keys.append(key);
        }
        for (const QString& key : keys) {
            push(std::make_unique<SetEnvelopeCommand>(project_, id, key, p.envelope(id, key), Envelope{}, text));
        }
    }
    QStringList removedReturns;
    for (auto it = p.returns().rbegin(); it != p.returns().rend(); ++it) {
        if (returns.contains(it->id)) removedReturns.append(it->id);
    }
    for (const QString& id : removedReturns) push(std::make_unique<RemoveReturnCommand>(project_, id, text));
}

// --- Returns and sends ---

QString ProjectEditor::addReturnTrack(int index, const QString& name) {
    const Project& p = *project_;
    index = clampIndex(index, static_cast<int>(p.returns().size()));
    Track track;
    track.id = newId();
    track.name = name.isEmpty() ? p.uniqueTrackName(QStringLiteral("%1 Return").arg(returnLetter(index))) : name;
    track.color = p.nextColor();
    track.kind = kReturnKind;
    const QString id = track.id;
    push(std::make_unique<InsertReturnCommand>(project_, std::move(track), index));
    return id;
}

void ProjectEditor::setSend(const QString& trackId, const QString& returnId, std::optional<double> levelDb,
                            std::optional<bool> preFader, const QString& mergeKey) {
    const Project& p = *project_;
    const Track& track = p.track(trackId);
    if (track.isMaster() || !p.hasReturn(returnId) || p.wouldCycle(trackId, returnId)) {
        throw EditError(QStringLiteral("%1 can't send to that track").arg(track.name));
    }
    const Send old = track.sends.value(returnId, Send{});
    const Send nw{levelDb ? std::clamp(*levelDb, automation::kMinVolumeDb, automation::kMaxVolumeDb) : old.levelDb,
                  preFader ? *preFader : old.preFader};
    if (!track.sends.contains(returnId) || nw != old) {
        const QString text =
            nw.preFader == old.preFader ? QStringLiteral("Change Send") : QStringLiteral("Toggle Pre-Fader Send");
        SendMap sends = track.sends;
        sends.insert(returnId, nw);
        push(std::make_unique<UpdateTrackCommand>(project_, trackId, TrackField::Sends, track.sends, sends, text,
                                                  mergeKey));
    }
    if (levelDb) Q_EMIT parameterTouched(trackId, automation::sendKey(returnId));
}

void ProjectEditor::removeSend(const QString& trackId, const QString& returnId) {
    const Track* track = project_->findTrack(trackId);
    if (track == nullptr || !track->sends.contains(returnId)) return;
    SendMap kept = track->sends;
    kept.remove(returnId);
    push(std::make_unique<UpdateTrackCommand>(project_, trackId, TrackField::Sends, track->sends, kept,
                                              QStringLiteral("Remove Send")));
}

// --- Groups ---

QStringList ProjectEditor::roots(const QStringList& trackIds) const {
    const Project& p = *project_;
    QSet<QString> wanted;
    for (const QString& id : trackIds) {
        if (p.hasTrack(id)) wanted.insert(id);
    }
    QStringList result;
    for (const Track& t : p.tracks()) {
        if (!wanted.contains(t.id)) continue;
        const QStringList ancestors = p.ancestors(t.id);
        if (std::none_of(ancestors.begin(), ancestors.end(), [&](const QString& a) { return wanted.contains(a); })) {
            result.append(t.id);
        }
    }
    return result;
}

TrackTree ProjectEditor::arranged(const QStringList& roots, int at, const std::optional<QString>& parent) const {
    const Project& p = *project_;
    QSet<QString> moving(roots.begin(), roots.end());
    for (const QString& root : roots) {
        for (const Track* d : p.descendants(root)) moving.insert(d->id);
    }
    TrackTree staying;
    TrackTree block;
    for (const Track& t : p.tracks()) {
        if (!moving.contains(t.id)) {
            staying.push_back({t.id, t.parent});
        } else {
            block.push_back({t.id, roots.contains(t.id) ? parent : t.parent});
        }
    }
    int position = 0;
    const int end = std::min(std::max(0, at), static_cast<int>(p.tracks().size()));
    for (int i = 0; i < end; ++i) {
        if (!moving.contains(p.tracks()[i].id)) ++position;
    }
    TrackTree tree(staying.begin(), staying.begin() + position);
    tree.insert(tree.end(), block.begin(), block.end());
    tree.insert(tree.end(), staying.begin() + position, staying.end());
    return tree;
}

void ProjectEditor::arrange(const TrackTree& tree, const QString& text, const QSet<QString>& going) {
    Project& p = *project_;
    if (tree == p.tree()) return;
    if (const auto problem = arrangementProblem(tree, going)) {
        Q_EMIT refused(*problem);
        return;
    }
    // A track taking its input from a group it comes into (or from what that
    // group feeds) loses that input first: it would close a cycle; and so does
    // a device taking its sidechain from one. Copies, without their inputs and
    // sidechains, which come back one by one unless they close a cycle with
    // those before them. (Their devices only as far as sidechains go: those
    // kept, one by one, flat.)
    QHash<QString, std::optional<QString>> parents;
    for (const TreeEntry& entry : tree) parents.insert(entry.id, entry.parent);
    std::vector<Track> tracks;
    for (const Track& t : p.tracks()) {
        Track copy = skeleton(t);
        copy.parent = parents.value(t.id);
        copy.inputTrack.reset();
        tracks.push_back(std::move(copy));
    }
    std::vector<Track> returns;
    for (const Track& r : p.returns()) returns.push_back(skeleton(r));
    QStringList cycling;
    for (std::size_t i = 0; i < tracks.size(); ++i) {
        const auto& source = p.tracks()[i].inputTrack;
        if (source && feeds(routingGraph(tracks, returns), tracks[i].id, *source)) {
            cycling.append(tracks[i].id);
        } else {
            tracks[i].inputTrack = source;
        }
    }
    struct Keyed {
        QString trackId;
        QString deviceId;
        Sidechain sidechain;
    };
    std::vector<Keyed> cyclingSidechains;
    const auto check = [&](Track& copy, const Track& original) {
        for (const Device* device : iterDevices(original.devices)) {
            if (!device->sidechain) continue;
            if (feeds(routingGraph(tracks, returns), copy.id, device->sidechain->trackId)) {
                cyclingSidechains.push_back({copy.id, device->id, *device->sidechain});
            } else {
                Device flat = *device;
                flat.chains.clear();
                copy.devices.push_back(std::move(flat));
            }
        }
    };
    for (std::size_t i = 0; i < tracks.size(); ++i) check(tracks[i], p.tracks()[i]);
    for (std::size_t i = 0; i < returns.size(); ++i) check(returns[i], p.returns()[i]);
    if (cycling.isEmpty() && cyclingSidechains.empty()) {
        push(std::make_unique<ArrangeTracksCommand>(project_, p.tree(), tree, text));
        return;
    }
    Macro macro(undoStack_, text);
    for (const QString& id : cycling) {
        push(std::make_unique<UpdateTrackCommand>(project_, id, TrackField::InputTrack, p.track(id).inputTrack,
                                                  std::optional<QString>(), text));
    }
    for (const Keyed& keyed : cyclingSidechains) {
        push(std::make_unique<SetDeviceSidechainCommand>(project_, keyed.trackId, keyed.deviceId, keyed.sidechain,
                                                         std::nullopt, text));
    }
    push(std::make_unique<ArrangeTracksCommand>(project_, p.tree(), tree, text));
}

bool ProjectEditor::validTree(const TrackTree& tree) const {
    std::vector<Track> tracks;
    tracks.reserve(tree.size());
    for (const TreeEntry& entry : tree) {
        const Track* original = project_->findTrack(entry.id);
        if (original == nullptr) return false;
        Track t = skeleton(*original);
        t.parent = entry.parent;
        tracks.push_back(std::move(t));
    }
    return !treeProblem(tracks);
}

QString ProjectEditor::groupTracks(const QStringList& trackIds) {
    const Project& p = *project_;
    const QStringList rootIds = roots(trackIds);
    if (rootIds.isEmpty()) return {};
    if (const auto problem = heldProblem(rootIds)) {
        Q_EMIT refused(*problem);
        return {};
    }
    const Track& first = p.track(rootIds.front());
    const int index = p.trackIndex(first.id);
    Track group;
    group.id = newId();
    group.nameTemplate = QStringLiteral("# Group");
    group.color = p.nextColor();
    group.kind = kGroupKind;
    group.height = kDefaultGroupHeight;
    group.parent = first.parent;
    const QString id = group.id;
    const QString text = QStringLiteral("Group Tracks");
    Macro macro(undoStack_, text);
    push(std::make_unique<InsertTrackCommand>(project_, std::move(group), index, text));
    arrange(arranged(rootIds, p.trackIndex(id) + 1, id), text);
    return id;
}

void ProjectEditor::ungroup(const QStringList& groupIds) {
    const Project& p = *project_;
    QStringList groups;
    for (const QString& id : roots(groupIds)) {
        if (p.track(id).isGroup()) groups.append(id);
    }
    for (const QString& group : QStringList(groups)) {
        for (const Track* d : p.descendants(group)) {
            if (d->isGroup() && groupIds.contains(d->id)) groups.append(d->id);
        }
    }
    if (groups.isEmpty()) return;
    if (const auto problem = heldProblem(groups)) {
        Q_EMIT refused(*problem);
        return;
    }
    const QString text = QStringLiteral("Ungroup Tracks");
    Macro macro(undoStack_, text);
    const QSet<QString> going(groups.begin(), groups.end());
    dropInputs(going, text);
    dropSidechains(going, text);
    TrackTree tree = p.tree();
    for (const QString& groupId : groups) {
        std::optional<QString> parent;
        for (const TreeEntry& entry : tree) {
            if (entry.id == groupId) parent = entry.parent;
        }
        for (TreeEntry& entry : tree) {
            if (entry.parent == groupId) entry.parent = parent;
        }
    }
    arrange(tree, text, going);
    for (auto it = groups.crbegin(); it != groups.crend(); ++it) {
        push(std::make_unique<RemoveTrackCommand>(project_, *it, text));
    }
}

bool ProjectEditor::canMoveTracks(const QStringList& trackIds, int index, const QString& parent) const {
    const Project& p = *project_;
    const QStringList rootIds = roots(trackIds);
    if (rootIds.isEmpty() || (!parent.isEmpty() && (!p.hasTrack(parent) || !p.track(parent).isGroup()))) return false;
    if (!parent.isEmpty()) {  // a group can't go into itself
        if (rootIds.contains(parent)) return false;
        for (const QString& root : rootIds) {
            if (p.isDescendant(parent, root)) return false;
        }
    }
    const TrackTree tree = arranged(rootIds, index, editing::optionalId(parent));
    return tree != p.tree() && validTree(tree) && !arrangementProblem(tree);
}

bool ProjectEditor::moveTracks(const QStringList& trackIds, int index, const QString& parent) {
    if (!canMoveTracks(trackIds, index, parent)) return false;
    const QStringList rootIds = roots(trackIds);
    arrange(arranged(rootIds, index, editing::optionalId(parent)),
            rootIds.size() == 1 ? QStringLiteral("Move Track") : QStringLiteral("Move Tracks"));
    return true;
}

void ProjectEditor::setFolded(const QString& trackId, bool folded) {
    const Track* track = project_->findTrack(trackId);
    if (track != nullptr && track->folded != folded) project_->updateTrack(trackId, TrackField::Folded, folded);
}

// --- Copying tracks ---

std::optional<CopiedTracks> ProjectEditor::copyTracks(const QStringList& trackIds) const {
    const Project& p = *project_;
    const QStringList rootIds = roots(trackIds);
    if (rootIds.isEmpty()) return std::nullopt;
    CopiedTracks copied;
    copied.roots = rootIds;
    for (const QString& root : rootIds) {
        copied.tracks.push_back(p.track(root));
        for (const Track* d : p.descendants(root)) copied.tracks.push_back(*d);
    }
    for (const Track& t : copied.tracks) {
        for (const Device* d : iterDevices(t.devices)) {
            if (p.isDeviceFolded(d->id)) copied.folded.insert(d->id);
        }
    }
    return copied;
}

std::optional<CopiedTracks> ProjectEditor::cutTracks(const QStringList& trackIds) {
    auto copied = copyTracks(trackIds);
    if (!copied) return std::nullopt;
    deleteTracks(copied->roots);
    for (const QString& root : copied->roots) {
        if (project_->hasTrack(root)) return std::nullopt;
    }
    return copied;
}

QStringList ProjectEditor::pasteTracks(const CopiedTracks& copied, const QString& after) {
    const InsertionPoint point = insertionPoint(after);
    return insertCopies(copied, point.index ? *point.index : static_cast<int>(project_->tracks().size()), point.parent,
                        copied.roots.size() == 1 ? QStringLiteral("Paste Track") : QStringLiteral("Paste Tracks"));
}

QStringList ProjectEditor::duplicateTracks(const QStringList& trackIds) {
    const auto copied = copyTracks(trackIds);
    if (!copied) return {};
    const Project& p = *project_;
    const QString last = copied->roots.back();
    return insertCopies(*copied, p.subtreeEnd(p.trackIndex(last)), p.track(last).parent,
                        copied->roots.size() == 1 ? QStringLiteral("Duplicate Track")
                                                  : QStringLiteral("Duplicate Tracks"));
}

QStringList ProjectEditor::insertCopies(const CopiedTracks& copied, int index, std::optional<QString> parent,
                                        const QString& text) {
    Project& p = *project_;
    QHash<QString, QString> renamed;
    for (const Track& t : copied.tracks) renamed.insert(t.id, newId());
    // A track a copy hears (its input, a sidechain): the copy of it, if copied
    // too; none if it is gone.
    const auto source = [&](const QString& trackId) -> std::optional<QString> {
        if (renamed.contains(trackId)) return renamed.value(trackId);
        if (trackId == kMaster || p.hasTrack(trackId) || p.hasReturn(trackId)) return trackId;
        return std::nullopt;
    };
    std::tie(index, parent) = outsideFrozen(index, parent);
    std::vector<Track> copies;
    QSet<QString> folded;
    for (const Track& original : copied.tracks) {
        Track track = original;
        track.id = renamed.value(original.id);
        // Numbered by its place ("# Kick"), or a name of its own ("Vox 2").
        if (!original.nameSource().contains(QChar(kNumberMark)))
            track.nameTemplate = p.uniqueTrackName(original.nameSource());
        track.armed = false;
        if (copied.roots.contains(original.id)) {
            track.parent = parent;
        } else if (original.parent) {
            track.parent = renamed.value(*original.parent);
        }
        for (Clip& clip : track.clips) clip.id = newId();
        QHash<QString, QString> ids;  // its devices' and rack chains' ids: the copies'
        for (Device& device : track.devices) {
            const QStringList old = deviceAndChainIds(device);
            refreshIds(device);
            const QStringList nw = deviceAndChainIds(device);
            for (qsizetype i = 0; i < old.size() && i < nw.size(); ++i) ids.insert(old[i], nw[i]);
        }
        for (Device* device : iterDevices(track.devices)) {
            if (!device->sidechain) continue;
            const auto heard = source(device->sidechain->trackId);
            if (heard) {
                device->sidechain->trackId = *heard;
            } else {
                device->sidechain.reset();
            }
        }
        for (auto it = ids.constBegin(); it != ids.constEnd(); ++it) {
            if (copied.folded.contains(it.key())) folded.insert(it.value());
        }
        if (track.inputTrack) track.inputTrack = source(*track.inputTrack);
        SendMap sends;
        for (auto it = track.sends.constBegin(); it != track.sends.constEnd(); ++it) {
            if (p.hasReturn(it.key())) sends.insert(it.key(), it.value());
        }
        track.sends = sends;
        EnvelopeMap envelopes;
        for (const auto& [key, points] : track.automation) {
            const auto send = automation::keySend(key);
            if (!send || p.hasReturn(*send)) envelopes.insert(renamedKey(key, ids), points);
        }
        track.automation = envelopes;
        AutomationView& view = track.automationView;
        if (view.key && !view.key->isEmpty()) view.key = renamedKey(*view.key, ids);
        for (QString& lane : view.lanes) lane = renamedKey(lane, ids);
        copies.push_back(std::move(track));
    }
    p.addFoldedDevices(folded);
    QStringList made;
    {
        Macro macro(undoStack_, text);
        for (int offset = 0; offset < static_cast<int>(copies.size()); ++offset) {
            Track& track = copies[offset];
            if (track.parent == parent) {  // (a group the tree doesn't let it be in: the one there)
                const std::optional<QString> group = parent;
                insertTrack(std::move(track), index + offset, group, text);
            } else {
                push(std::make_unique<InsertTrackCommand>(project_, std::move(track), index + offset, text));
            }
        }
    }
    for (const QString& root : copied.roots) {
        if (p.hasTrack(renamed.value(root))) made.append(renamed.value(root));
    }
    return made;
}

// --- A track's settings ---

void ProjectEditor::renameTrack(const QString& trackId, const QString& name) {
    const Track* track = project_->findTrack(trackId);
    if (track != nullptr && !name.isEmpty() && name != track->nameSource()) {  // (its template: "# Lead")
        push(std::make_unique<UpdateTrackCommand>(project_, trackId, TrackField::Name, track->nameSource(), name,
                                                  QStringLiteral("Rename Track")));
    }
}

void ProjectEditor::setTrackColor(const QString& trackId, const QString& color) {
    const Track* track = project_->findTrack(trackId);
    if (track != nullptr && color != track->color) {
        push(std::make_unique<UpdateTrackCommand>(project_, trackId, TrackField::Color, track->color, color,
                                                  QStringLiteral("Change Track Color")));
    }
}

void ProjectEditor::setTrackParam(const QString& trackId, TrackField field, double value, const QString& mergeKey) {
    QString label;
    switch (field) {
    case TrackField::VolumeDb: label = QStringLiteral("Change Volume"); break;
    case TrackField::Pan: label = QStringLiteral("Change Pan"); break;
    case TrackField::Mute: label = QStringLiteral("Toggle Track Activator"); break;
    case TrackField::Solo: break;
    default: throw std::invalid_argument("not a mixer setting: " + trackFieldName(field).toStdString());
    }
    if (trackId == kMaster) {
        if (field != TrackField::VolumeDb && field != TrackField::Pan) {
            throw EditError(QStringLiteral("The master is always heard"));
        }
        label = field == TrackField::VolumeDb ? QStringLiteral("Change Master Volume")
                                              : QStringLiteral("Change Master Pan");
    }
    if (field == TrackField::Solo) {
        soloTracks({trackId}, value != 0.0);
        return;
    }
    if (field == TrackField::Pan) value = std::clamp(value, -1.0, 1.0);
    const Track& track = project_->track(trackId);
    if (field == TrackField::Mute) {
        const bool mute = value != 0.0;
        Q_EMIT switchedByHand(trackId, automation::kMixerOn);
        if (mute != track.mute) {
            push(std::make_unique<UpdateTrackCommand>(project_, trackId, field, track.mute, mute, label, mergeKey));
        }
        Q_EMIT parameterTouched(trackId, automation::kMixerOn);  // (its activator)
        return;
    }
    const double old = field == TrackField::VolumeDb ? track.volumeDb : track.pan;
    if (value != old) push(std::make_unique<UpdateTrackCommand>(project_, trackId, field, old, value, label, mergeKey));
    Q_EMIT parameterTouched(trackId, field == TrackField::VolumeDb ? automation::kMixerVolume : automation::kMixerPan);
}

void ProjectEditor::setTracksParam(const QMap<QString, double>& values, TrackField field, const QString& mergeKey) {
    if (field != TrackField::VolumeDb && field != TrackField::Pan) {
        throw std::invalid_argument("not a fader: " + trackFieldName(field).toStdString());
    }
    const bool volume = field == TrackField::VolumeDb;
    const double low = volume ? automation::kMinVolumeDb : -1.0;
    const double high = volume ? automation::kMaxVolumeDb : 1.0;
    QMap<QString, double> nw;
    QMap<QString, double> old;
    for (auto it = values.constBegin(); it != values.constEnd(); ++it) {
        const Track& track = project_->track(it.key());
        nw.insert(it.key(), std::clamp(it.value(), low, high));
        old.insert(it.key(), volume ? track.volumeDb : track.pan);
    }
    if (nw.size() == 1) {
        setTrackParam(nw.firstKey(), field, nw.first(), mergeKey);
        return;
    }
    if (nw != old) {
        QMap<QString, TrackValue> before;
        QMap<QString, TrackValue> after;
        for (auto it = nw.constBegin(); it != nw.constEnd(); ++it) {
            before.insert(it.key(), old.value(it.key()));
            after.insert(it.key(), it.value());
        }
        push(std::make_unique<UpdateTracksCommand>(project_, field, before, after,
                                                   volume ? QStringLiteral("Change Volume") : QStringLiteral("Change Pan"),
                                                   mergeKey));
    }
    for (const QString& id : nw.keys()) {
        Q_EMIT parameterTouched(id, volume ? automation::kMixerVolume : automation::kMixerPan);
    }
}

void ProjectEditor::soloTracks(const QStringList& trackIds, bool solo, bool exclusive) {
    const QSet<QString> ids(trackIds.begin(), trackIds.end());
    QStringList senders;
    for (const Track* t : project_->senders()) senders.append(t->id);
    for (const QString& id : senders) {
        if (!ids.contains(id) && !(exclusive && solo)) continue;
        const bool nw = ids.contains(id) && solo;
        if (project_->track(id).solo != nw) project_->updateTrack(id, TrackField::Solo, nw);
    }
}

void ProjectEditor::armTracks(const QStringList& trackIds, bool armed, bool exclusive) {
    const Project& p = *project_;
    const QSet<QString> ids(trackIds.begin(), trackIds.end());
    if (armed && std::any_of(ids.begin(), ids.end(), [&](const QString& t) { return p.hasTrack(t) && p.isFrozen(t); })) {
        Q_EMIT refused(QStringLiteral("A frozen track doesn't record: unfreeze it first"));
    }
    QStringList all;
    for (const Track& t : p.tracks()) all.append(t.id);
    for (const QString& id : all) {
        const Track& track = p.track(id);
        if (track.isGroup() || (armed && ids.contains(id) && p.isFrozen(id))) continue;  // nothing to record
        const bool wanted = ids.contains(id) ? armed : (track.armed && !(exclusive && armed));
        if (wanted != track.armed) project_->updateTrack(id, TrackField::Armed, wanted);
    }
}

void ProjectEditor::setTrackHeight(const QString& trackId, int height) {
    // View state: saved with the project but not worth an undo step.
    if (project_->findTrack(trackId) != nullptr) project_->updateTrack(trackId, TrackField::Height, height);
}

// --- Inputs ---

void ProjectEditor::setTrackInput(const QString& trackId, const std::vector<int>& channels) {
    if (channels.size() > 2) throw EditError(QStringLiteral("An input is one channel or a pair"));
    setInput(trackId, channels, std::nullopt);
}

void ProjectEditor::setTrackInputTrack(const QString& trackId, const std::optional<QString>& sourceId) {
    const Project& p = *project_;
    const Track& track = p.track(trackId);
    if (sourceId && (!track.isAudio() || !(*sourceId == kMaster || p.hasTrack(*sourceId) || p.hasReturn(*sourceId)) ||
                     p.inputWouldCycle(trackId, *sourceId))) {
        throw EditError(QStringLiteral("%1 can't take its input from that track").arg(track.name));
    }
    setInput(trackId, {}, sourceId);
}

void ProjectEditor::setInput(const QString& trackId, const std::vector<int>& channels,
                             const std::optional<QString>& sourceId) {
    const Track& track = project_->track(trackId);
    const TrackValues old{{TrackField::Input, track.input}, {TrackField::InputTrack, track.inputTrack}};
    const TrackValues nw{{TrackField::Input, channels}, {TrackField::InputTrack, sourceId}};
    if (nw != old) {
        push(std::make_unique<UpdateTrackFieldsCommand>(project_, trackId, old, nw, QStringLiteral("Change Track Input")));
    }
}

void ProjectEditor::dropInputs(const QSet<QString>& sourceIds, const QString& text) {
    QStringList dropped;
    for (const Track& t : project_->tracks()) {
        if (t.inputTrack && sourceIds.contains(*t.inputTrack)) dropped.append(t.id);
    }
    for (const QString& id : dropped) {
        push(std::make_unique<UpdateTrackCommand>(project_, id, TrackField::InputTrack, project_->track(id).inputTrack,
                                                  std::optional<QString>(), text));
    }
}

void ProjectEditor::dropSidechains(const QSet<QString>& sourceIds, const QString& text) {
    // (Those on them keep theirs: they come back together.)
    struct Keyed {
        QString trackId;
        QString deviceId;
        Sidechain sidechain;
    };
    std::vector<Keyed> dropped;
    for (const Track* track : project_->allTracks()) {
        if (sourceIds.contains(track->id)) continue;
        for (const Device* device : iterDevices(track->devices)) {
            if (device->sidechain && sourceIds.contains(device->sidechain->trackId)) {
                dropped.push_back({track->id, device->id, *device->sidechain});
            }
        }
    }
    for (const Keyed& keyed : dropped) {
        push(std::make_unique<SetDeviceSidechainCommand>(project_, keyed.trackId, keyed.deviceId, keyed.sidechain,
                                                         std::nullopt, text));
    }
}

void ProjectEditor::setTrackMonitor(const QString& trackId, const QString& mode) {
    if (!kMonitorModes.contains(mode)) throw EditError(QStringLiteral("There is no monitoring mode %1").arg(mode));
    const QString old = project_->track(trackId).monitor;
    if (mode != old) {
        push(std::make_unique<UpdateTrackCommand>(project_, trackId, TrackField::Monitor, old, mode,
                                                  QStringLiteral("Change Monitoring")));
    }
}

void ProjectEditor::setTrackMidiInput(const QString& trackId, const std::optional<MidiInput>& midiInput) {
    if (midiInput && (midiInput->channel < 0 || midiInput->channel > 16)) {
        throw EditError(QStringLiteral("A MIDI channel is 1-16, or 0 for all"));
    }
    const auto old = project_->track(trackId).midiInput;
    if (midiInput != old) {
        push(std::make_unique<UpdateTrackCommand>(project_, trackId, TrackField::MidiInput, old, midiInput,
                                                  QStringLiteral("Change MIDI Input")));
    }
}

// --- Recording ---

ClipRefs ProjectEditor::addRecordings(const std::vector<RecordedTake>& takes, double quantize) {
    const Project& p = *project_;
    const double tempo = p.tempo();
    OrderedMap<QString, std::vector<Clip>> byTrack;
    for (const RecordedTake& take : takes) {
        if (!p.hasTrack(take.trackId) || take.durationSec <= 0) continue;
        const Track& track = p.track(take.trackId);
        if (track.isMidi() != take.midi) continue;
        if (take.midi) {
            byTrack[take.trackId].push_back(recordedMidiClip(track, take, quantize));
            continue;
        }
        double startBeat = secondsToBeats(take.startSec, tempo);
        double offset = 0.0;
        if (startBeat < 0) {  // it began before the timeline (the count-in's latency)
            offset = -take.startSec;
            startBeat = 0.0;
        }
        const double duration = take.durationSec - offset;
        if (duration <= 0) continue;
        byTrack[take.trackId].push_back(Clip::audio(newId(), take.path, QFileInfo(take.path).completeBaseName(),
                                                    startBeat, duration, offset, take.durationSec));
    }
    QMap<QString, std::vector<Clip>> after;
    ClipRefs refs;
    for (const auto& [trackId, clips] : byTrack) {
        QSet<QString> newIds;
        std::vector<Clip> all = p.track(trackId).clips;
        for (const Clip& clip : clips) {
            newIds.insert(clip.id);
            all.push_back(clip);
            refs.append({trackId, clip.id});
        }
        after.insert(trackId, edits::resolveOverlaps(all, newIds, tempo));
    }
    if (!after.isEmpty()) commitClips(QStringLiteral("Record"), after);
    return refs;
}

Clip ProjectEditor::recordedMidiClip(const Track& track, const RecordedTake& take, double quantize) const {
    const double tempo = project_->tempo();
    const double start = std::max(0.0, secondsToBeats(take.startSec, tempo));
    const double end = secondsToBeats(take.startSec + take.durationSec, tempo);
    std::vector<Note> clipNotes;
    for (const RecordedTakeNote& played : take.notes) {
        double begin = secondsToBeats(played.start, tempo) - start;
        const double length = secondsToBeats(played.end, tempo) - start - begin;
        if (quantize > 0) {  // on the arrangement's grid, unless that is outside the clip
            const double snapped = roundHalfEven((start + begin) / quantize) * quantize - start;
            if (snapped >= 0 && snapped < end - start) begin = snapped;
        }
        if (begin < 0 || begin >= end - start || length <= 0) continue;
        clipNotes.push_back(Note{std::clamp(played.pitch, 0, 127), begin, length, std::clamp(played.velocity, 1, 127)});
    }
    return Clip::midi(newId(), track.name, start, end - start, 0.0, notes::normalize(clipNotes));
}

}  // namespace sub::app
