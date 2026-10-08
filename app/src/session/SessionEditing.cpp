// The session's Create and Edit commands: what the main window's menus did
// with what is selected (dispatching to the arrangement's actions and the
// device view's while it has the focus), adding to the selected track, and
// freezing.

#include "session/Session.h"

#include <QFileInfo>

#include <algorithm>

#include "audio/EngineBridge.h"
#include "browser/BrowserController.h"
#include "browser/PresetIndex.h"
#include "editor/ProjectEditor.h"
#include "io/Serialization.h"
#include "model/Devices.h"
#include "model/Errors.h"
#include "model/Project.h"
#include "session/ArrangementActions.h"
#include "session/DeviceSelection.h"
#include "session/Renders.h"
#include "session/Selection.h"
#include "session/SessionSupport.h"

namespace sub::app {

namespace {

QStringList names(const Project& project, const QStringList& trackIds) {
    QStringList list;
    for (const QString& id : trackIds) {
        if (const Track* track = project.findTrack(id)) list.append(track->name);
    }
    return list;
}

QList<ClipRef> sortedClips(const Selection& selection) {
    QList<ClipRef> refs(selection.clips().begin(), selection.clips().end());
    std::sort(refs.begin(), refs.end());
    return refs;
}

}  // namespace

// Where a new track goes: after the selected one (and what is in it), in its group.
Session::Place Session::afterSelectedTrack() const {
    const InsertionPoint point = editor_->insertionPoint(selection_->trackId());
    return {point.index.value_or(-1), point.parent};
}

// --- Create -------------------------------------------------------------------------------------

QString Session::insertAudioTrack() {
    const Place place = afterSelectedTrack();
    const QString track = editor_->addAudioTrack(place.index, QString(), TrackParent(place.parent));
    selection_->selectTrack(track, true);
    return track;
}

QString Session::insertMidiTrack() {
    const Place place = afterSelectedTrack();
    const QString track = editor_->addMidiTrack(place.index, QString(), kDefaultInstrument, std::nullopt,
                                                TrackParent(place.parent));
    selection_->selectTrack(track, true);
    return track;
}

QString Session::insertReturnTrack() {
    const QString selected = selection_->trackId();
    const int index = !selected.isEmpty() && project_->hasReturn(selected) ? project_->returnIndex(selected) + 1 : -1;
    const QString track = editor_->addReturnTrack(index);
    selection_->selectTrack(track, true);
    return track;
}

void Session::insertMidiClip(double gridStep) {
    ClipRefs refs;
    if (const auto& range = selection_->timeRange()) {
        refs = editor_->addMidiClipsOver(range->start, range->end, range->trackIds);
    } else {
        const QString trackId = selection_->trackId();
        const Track* track = !trackId.isEmpty() && project_->hasTrack(trackId) ? project_->findTrack(trackId) : nullptr;
        if (track != nullptr && track->isMidi()) {
            const auto [start, length] = editor_->midiClipSpan(trackId, selection_->insertBeat(), gridStep);
            if (const auto ref = editor_->addMidiClip(trackId, start, length)) refs.append(*ref);
        }
    }
    if (refs.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("Select a MIDI track (or a time range on one) to insert a MIDI clip."));
        return;
    }
    selection_->selectClips(*editor_, refs, refs.front().trackId);
    arrangement_->requestClipView(refs, refs.front());  // straight into the piano roll
}

void Session::groupSelected() {
    if (selection_->focus() == Selection::Focus::Devices) {
        if (!devices_->groupSelected()) Q_EMIT statusMessage(QStringLiteral("Select the devices to group."));
        return;
    }
    QStringList tracks;
    for (const QString& id : selection_->trackIds()) {
        if (project_->hasTrack(id)) tracks.append(id);
    }
    const QString group = tracks.isEmpty() ? QString() : editor_->groupTracks(tracks);
    if (group.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("Select the tracks to group."));
    } else {
        selection_->selectTrack(group, true);
    }
}

void Session::ungroupSelected() {
    if (selection_->focus() == Selection::Focus::Devices) {
        if (!devices_->ungroupSelected()) Q_EMIT statusMessage(QStringLiteral("Select a rack to ungroup."));
        return;
    }
    QStringList groups;
    for (const QString& id : selection_->trackIds()) {
        if (project_->hasTrack(id) && project_->track(id).isGroup()) groups.append(id);
    }
    if (groups.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("Select a group track to ungroup."));
    } else {
        editor_->ungroup(groups);
    }
}

void Session::deleteSelectedTracks() { editor_->deleteTracks(ownersAmong(*project_, selection_->trackIds())); }

// --- Adding to the selected track -------------------------------------------------------------

void Session::addDevice(const QString& kind, const std::optional<PluginRef>& plugin) {
    const QString trackId = selection_->trackId();
    const bool hasTrack = !trackId.isEmpty() && project_->hasOwner(trackId);  // a track, a return or the master
    if (isInstrument(kind, plugin) && !(hasTrack && project_->track(trackId).isMidi())) {
        // As in Ableton: an instrument chosen with no MIDI track selected gets a new one.
        const Place place = afterSelectedTrack();
        const QString track =
            editor_->addMidiTrack(place.index, QString(), plugin ? QString() : kind, plugin, TrackParent(place.parent));
        selection_->selectTrack(track, true);
    } else if (hasTrack) {
        editor_->addDevice(trackId, kind, -1, plugin);
    } else {
        Q_EMIT statusMessage(QStringLiteral("Select a track to add the device to."));
    }
}

void Session::addDeviceToSelectedTrack(const QString& kind) { addDevice(kind, std::nullopt); }

void Session::addPluginToSelectedTrack(const QVariantMap& plugin) {
    if (const auto ref = pluginRefFromMap(plugin)) addDevice(kPluginKind, ref);
}

void Session::addPresetToSelectedTrack(const QString& path) {
    Device device;
    try {
        device = loadPreset(path);
    } catch (const ProjectFileError& error) {
        Q_EMIT statusMessage(error.message());
        return;
    }
    const QString text = QStringLiteral("Load Preset ") + fileStem(path);
    const QString trackId = selection_->trackId();
    const bool hasTrack = !trackId.isEmpty() && project_->hasOwner(trackId);
    if (deviceIsInstrument(device) && !(hasTrack && project_->track(trackId).isMidi())) {
        const Place place = afterSelectedTrack();
        const QString track = editor_->addMidiTrackWith(device, place.index, TrackParent(place.parent), text);
        selection_->selectTrack(track, true);
    } else if (!hasTrack) {
        Q_EMIT statusMessage(QStringLiteral("Select a track to add the preset to."));
    } else if (!editor_->insertDevice(trackId, device, -1, QString(), text, !device.isRack())) {
        Q_EMIT statusMessage(QStringLiteral("The preset can't go there: racks nest at most 8 deep."));
    }
}

void Session::addFileAtInsert(const QString& path) {
    const auto info = bridge_->fileInfo(path);
    if (!info) return;
    const QString selected = selection_->trackId();
    const QString trackId = project_->hasTrack(selected) ? selected : QString();
    const ClipRefs refs = editor_->addClips(trackId, selection_->insertBeat(), {{path, info->duration}},
                                            static_cast<int>(project_->tracks().size()));
    if (!refs.isEmpty()) selection_->selectClips(*editor_, refs);
}

void Session::presetSaved(const QString&) { presets_->rescan(); }

// --- Edit ---------------------------------------------------------------------------------------

QString Session::whatIsCopied(const QString& verb) {
    const Selection& selection = *selection_;
    if (selection.focus() == Selection::Focus::Devices) return QStringLiteral("devices");
    if (selection.focus() == Selection::Focus::Track && !selection.timeRange()) {
        for (const QString& id : selection.trackIds()) {
            if (project_->hasTrack(id)) return QStringLiteral("tracks");
        }
        Q_EMIT statusMessage(QStringLiteral("Only the arrangement's tracks can be %1, not returns or the master.").arg(verb));
        return {};
    }
    if (selection.timeRange() && !selection.lanes().isEmpty()) return QStringLiteral("automation");
    if (selection.points()) {
        Q_EMIT statusMessage(
            QStringLiteral("Breakpoints can't be %1: select a time range on the automation lane.").arg(verb));
        return {};
    }
    return selection.clipRange() ? QStringLiteral("clips") : QString();
}

void Session::cut() {
    const QString what = whatIsCopied(QStringLiteral("cut"));
    if (what == u"devices") {
        devices_->cutSelected();
    } else if (what == u"automation") {
        arrangement_->cutAutomation();
    } else if (what == u"clips") {
        arrangement_->cutArea();
    } else if (what == u"tracks") {
        arrangement_->cutTracks(selection_->trackIds());
    }
}

void Session::copy() {
    const QString what = whatIsCopied(QStringLiteral("copied"));
    if (what == u"devices") {
        devices_->copySelected();
    } else if (what == u"automation") {
        arrangement_->copyAutomation();
    } else if (what == u"clips") {
        arrangement_->copyArea();
    } else if (what == u"tracks") {
        arrangement_->copyTracks(selection_->trackIds());
    }
}

void Session::paste() {
    if (selection_->focus() == Selection::Focus::Devices) {
        devices_->paste();
    } else {
        arrangement_->paste();
    }
}

void Session::duplicate() {
    Selection& selection = *selection_;
    if (selection.focus() == Selection::Focus::Devices) {
        devices_->duplicateSelected();
        return;
    }
    if (selection.timeRange() && !selection.lanes().isEmpty()) {
        const TimeRange range = *selection.timeRange();
        const QList<LaneRef> lanes = selection.lanes();
        editor_->duplicateAutomationRange(range.start, range.end, lanes);
        selection.setTimeRange(range.end, 2 * range.end - range.start, range.trackIds, std::nullopt, lanes);  // the copy
        selection.setInsert(range.end);
        return;
    }
    if (selection.focus() == Selection::Focus::Track && !selection.timeRange()) {
        QStringList tracks;
        for (const QString& id : selection.trackIds()) {
            if (project_->hasTrack(id)) tracks.append(id);
        }
        if (tracks.isEmpty()) {
            Q_EMIT statusMessage(QStringLiteral("Only the arrangement's tracks can be duplicated, not returns or the master."));
        } else {
            arrangement_->duplicateTracks(tracks);
        }
        return;
    }
    arrangement_->duplicateArea();
}

void Session::deleteSelection() {
    Selection& selection = *selection_;
    if (selection.focus() == Selection::Focus::Devices) {
        devices_->deleteSelected();  // not clips selected before: they aren't what the user is on
        return;
    }
    if (const auto points = selection.points()) {
        const QList<int> indices(points->indices.begin(), points->indices.end());
        editor_->deleteAutomationPoints(points->owner, points->key, indices);
        selection.selectPoints(points->owner, points->key, QSet<int>());
    } else if (selection.timeRange() && !selection.lanes().isEmpty()) {
        editor_->deleteAutomationRange(selection.timeRange()->start, selection.timeRange()->end, selection.lanes());
    } else if (selection.timeRange()) {
        arrangement_->deleteArea();
    } else if (selection.focus() == Selection::Focus::Track) {
        deleteSelectedTracks();
    }
}

void Session::split() {
    ClipRefs refs = sortedClips(*selection_);
    const QString trackId = selection_->trackId();
    if (refs.isEmpty() && !trackId.isEmpty() && project_->hasTrack(trackId)) {
        refs = editor_->clipsAt({trackId}, selection_->insertBeat());
    }
    if (!refs.isEmpty()) editor_->splitClips(refs, selection_->insertBeat());
}

void Session::selectAll() {
    ClipRefs refs;
    QStringList tracks;
    for (const Track& track : project_->tracks()) {
        tracks.append(track.id);
        for (const Clip& clip : track.clips) refs.append({track.id, clip.id});
    }
    const auto area = editor_->clipsArea(refs);
    if (!area) return;
    selection_->setTimeRange(area->start, area->end, tracks, editor_->clipsInRange(area->start, area->end, tracks));
}

void Session::consolidate() { arrangement_->consolidate(); }

void Session::reverseClips() { arrangement_->reverseSelection(); }

void Session::toggleClipActivation() { arrangement_->toggleActivation(); }

QVariantMap Session::renameTarget(bool browserListFocused) {
    if (browserListFocused) {
        const BrowserItem* item = browser_->results()->item(browser_->currentRow());
        if (item != nullptr && item->kind == ItemKind::Preset) {
            return {{QStringLiteral("kind"), QStringLiteral("preset")},
                    {QStringLiteral("path"), item->path},
                    {QStringLiteral("name"), fileStem(item->path)}};
        }
    }
    if (selection_->focus() == Selection::Focus::Devices) {
        QVariantMap chain = devices_->renameTarget();
        if (!chain.isEmpty()) {
            chain.insert(QStringLiteral("kind"), QStringLiteral("chain"));
            return chain;
        }
    }
    // A track (or a return); not the master, nor a track folded away in its group.
    const QString trackId = selection_->trackId();
    const bool renamable = project_->hasReturn(trackId) || (project_->hasTrack(trackId) && !project_->isHidden(trackId));
    if (!trackId.isEmpty() && renamable) {
        return {{QStringLiteral("kind"), QStringLiteral("track")},
                {QStringLiteral("trackId"), trackId},
                {QStringLiteral("name"), project_->track(trackId).nameSource()}};  // ("# Kick")
    }
    Q_EMIT statusMessage(QStringLiteral("Select a track, a rack chain or a preset to rename."));
    return {};
}

void Session::soloSelectedTracks() {
    QStringList tracks = ownersAmong(*project_, selection_->trackIds());
    if (tracks.isEmpty()) return;
    const bool solo = !std::all_of(tracks.begin(), tracks.end(), [this](const QString& id) { return project_->track(id).solo; });
    if (!solo) {
        tracks.clear();
        for (const Track* track : project_->senders()) tracks.append(track->id);
    }
    editor_->soloTracks(tracks, solo, true);
}

// --- Freezing -----------------------------------------------------------------------------------

void Session::toggleFreeze() {
    const QStringList tracks = ownersAmong(*project_, selection_->trackIds());
    if (!tracks.isEmpty() &&
        std::all_of(tracks.begin(), tracks.end(), [this](const QString& id) { return project_->isFrozen(id); })) {
        const QStringList changed = unfreezeTracks(tracks);
        if (!changed.isEmpty()) {
            Q_EMIT statusMessage(QStringLiteral("Unfroze %1").arg(names(*project_, changed).join(QStringLiteral(", "))));
        }
        return;
    }
    QStringList unfrozen;
    for (const QString& id : tracks) {
        if (!project_->isFrozen(id)) unfrozen.append(id);
    }
    freeze(unfrozen, true);
}

void Session::freezeTracks(const QStringList& trackIds) { freeze(trackIds, false); }

// Freezes these tracks in the background (`report`: "Froze ..." when done).
void Session::freeze(const QStringList& trackIds, bool report) {
    const QStringList owners = ownersAmong(*project_, trackIds);
    QStringList tracks;
    for (const QString& id : owners) {  // not those in a group frozen with them
        const QStringList ancestors = project_->hasTrack(id) ? project_->ancestors(id) : QStringList();
        if (std::none_of(ancestors.begin(), ancestors.end(), [&](const QString& a) { return owners.contains(a); })) {
            tracks.append(id);
        }
    }
    if (tracks.isEmpty()) return;
    if (bridge_->isRecording()) {
        Q_EMIT editor_->refused(QStringLiteral("Stop recording to freeze tracks"));
        return;
    }
    QStringList problems;
    QStringList freezable;
    for (const QString& id : tracks) {
        if (const auto problem = project_->freezeProblem(id)) {
            problems.append(*problem);
        } else {
            freezable.append(id);
        }
    }
    if (!problems.isEmpty()) Q_EMIT editor_->refused(problems.front());
    if (freezable.isEmpty()) return;
    auto* render = new FreezeTracksRender(render_, bridge_, editor_, freezable, finishFreeze_, this);
    if (report) {
        connect(render, &Render::finished, this, [this, render] {
            const QStringList& frozen = render->frozen();
            if (frozen.isEmpty()) return;
            const QString verb = project_->isFrozen(frozen.front()) ? QStringLiteral("Froze") : QStringLiteral("Unfroze");
            Q_EMIT statusMessage(verb + u' ' + names(*project_, frozen).join(QStringLiteral(", ")));
        });
    }
    startRender(render);
}

QStringList Session::unfreezeTracks(const QStringList& trackIds) {
    QStringList holders;
    for (const QString& id : ownersAmong(*project_, trackIds)) {
        if (const auto holder = project_->frozenBy(id)) {
            if (!holders.contains(*holder)) holders.append(*holder);
        }
    }
    return editor_->unfreezeTracks(holders);
}

QStringList Session::flattenTracks(const QStringList& trackIds) {
    QStringList tracks;
    for (const QString& id : ownersAmong(*project_, trackIds)) {
        if (project_->hasTrack(id)) tracks.append(id);
    }
    const QStringList flat = editor_->flattenTracks(tracks);
    if (flat.isEmpty()) {
        for (const QString& id : tracks) {
            if (const auto problem = project_->flattenProblem(id)) {
                Q_EMIT editor_->refused(*problem);
                break;
            }
        }
        return {};
    }
    for (qsizetype i = 0; i < flat.size(); ++i) {
        selection_->selectTrack(flat[i], true, i > 0 ? Selection::Mode::Toggle : Selection::Mode::Replace);
    }
    return flat;
}

void Session::flattenSelectedTracks() {
    const QStringList flat = flattenTracks(selection_->trackIds());
    if (!flat.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("Flattened %1").arg(names(*project_, flat).join(QStringLiteral(", "))));
    }
}

QVariantMap Session::freezeActions(const QStringList& trackIds) const {
    const bool plural = trackIds.size() > 1;
    const bool unfreeze = std::all_of(trackIds.begin(), trackIds.end(), [this](const QString& id) {
        return project_->hasOwner(id) && project_->isFrozen(id);
    });
    QVariantMap actions;
    actions.insert(QStringLiteral("unfreeze"), unfreeze);
    if (unfreeze) {
        actions.insert(QStringLiteral("freezeText"), plural ? QStringLiteral("Unfreeze Tracks") : QStringLiteral("Unfreeze Track"));
        actions.insert(QStringLiteral("freezeEnabled"), true);
        actions.insert(QStringLiteral("freezeToolTip"), QString());
    } else {
        actions.insert(QStringLiteral("freezeText"), plural ? QStringLiteral("Freeze Tracks") : QStringLiteral("Freeze Track"));
        bool enabled = false;
        QString toolTip;
        for (const QString& id : trackIds) {
            if (!project_->hasOwner(id) || project_->isFrozen(id)) continue;
            const auto problem = project_->freezeProblem(id);
            if (!problem) {
                enabled = true;
            } else if (toolTip.isEmpty()) {
                toolTip = *problem;
            }
        }
        actions.insert(QStringLiteral("freezeEnabled"), enabled);
        actions.insert(QStringLiteral("freezeToolTip"), enabled ? QString() : toolTip);
    }
    actions.insert(QStringLiteral("flattenText"), plural ? QStringLiteral("Flatten Tracks") : QStringLiteral("Flatten Track"));
    actions.insert(QStringLiteral("flattenEnabled"),
                   std::any_of(trackIds.begin(), trackIds.end(), [this](const QString& id) {
                       return project_->hasTrack(id) && !project_->flattenProblem(id);
                   }));
    return actions;
}

}  // namespace sub::app
