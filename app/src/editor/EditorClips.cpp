// Editing clips: adding, moving, trimming, splitting and consolidating them,
// and time selections (delete, duplicate, copy, cut, paste and move a range,
// with the automation under it, and over frozen tracks with their frozen audio;
// reverse or deactivate what is in it).

#include "editor/EditorSupport.h"
#include "editor/ProjectEditor.h"

#include "model/Commands.h"
#include "model/Edits.h"
#include "model/Ids.h"
#include "model/Keys.h"
#include "model/Notes.h"

#include <QFileInfo>
#include <QHash>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <set>

namespace sub::app {

using editing::Macro;

namespace {

std::vector<double> beats(const std::set<double>& edges) { return {edges.begin(), edges.end()}; }

// Clips (or frozen audio's segments) with the stretch between two beats moved
// (or copied) by `delta` as a block: it replaces everything where it lands,
// empty parts too, as a stretch of frozen audio does. Clips across its edges
// are split there; those wholly inside a moved stretch keep their ids.
std::vector<Clip> movedBlock(const std::vector<Clip>& clips, double start, double end, double delta, bool copy,
                             double tempo) {
    std::vector<Clip> pieces = edits::sliceRange(clips, start, end, tempo, !copy);
    std::vector<Clip> kept = copy ? clips : edits::removeRange(clips, start, end, tempo);
    kept = edits::removeRange(kept, start + delta, end + delta, tempo);
    QSet<QString> ids;
    for (Clip& piece : pieces) {
        piece.startBeat += delta;
        ids.insert(piece.id);
        kept.push_back(std::move(piece));
    }
    return edits::resolveOverlaps(kept, ids, tempo);
}

// Clip ids by track, in the order the tracks come in `refs`.
OrderedMap<QString, QSet<QString>> idsByTrack(const ClipRefs& refs) {
    OrderedMap<QString, QSet<QString>> ids;
    for (const ClipRef& ref : refs) ids[ref.trackId].insert(ref.clipId);
    return ids;
}

}  // namespace

bool ProjectEditor::commitClips(const QString& text, const QMap<QString, std::vector<Clip>>& after,
                                const QString& mergeKey) {
    ClipLists before;
    for (auto it = after.constBegin(); it != after.constEnd(); ++it) before.insert(it.key(), project_->track(it.key()).clips);
    return before == after || push(std::make_unique<SetClipsCommand>(project_, text, before, after, mergeKey));
}

bool ProjectEditor::commitMoved(const QString& text, const QMap<QString, std::vector<Clip>>& after,
                                const QMap<LaneRef, Envelope>& envelopes,
                                const QMap<QString, std::vector<Clip>>& frozen) {
    // A time selection's edit, as one undo step: the clips, what of frozen
    // tracks' audio plays (`frozen`: it goes along with their clips), and the
    // automation going with them. Refused (false; `refused` says why) if it
    // would change what frozen audio holds without taking that audio along: the
    // clips of a frozen track, or automation baked into frozen audio.
    const Project& p = *project_;
    ClipLists before;
    for (auto it = after.constBegin(); it != after.constEnd(); ++it) before.insert(it.key(), p.track(it.key()).clips);
    FrozenSegments frozenBefore;
    FrozenSegments frozenAfter;
    bool frozenChanges = false;
    for (auto it = frozen.constBegin(); it != frozen.constEnd(); ++it) {
        frozenBefore.insert(it.key(), p.track(it.key()).frozen->segments);
        frozenAfter.insert(it.key(), it.value());
        frozenChanges = frozenChanges || frozenSegments(it.key()) != it.value();
    }
    auto command =
        std::make_unique<SetClipsCommand>(project_, text, before, after, QString(), frozenBefore, frozenAfter);
    if (const auto problem = frozenProblem(*command)) {
        Q_EMIT refused(*problem);
        return false;
    }
    const QStringList carried = frozen.keys();
    for (auto it = envelopes.constBegin(); it != envelopes.constEnd(); ++it) {
        const auto& [owner, key] = it.key();
        if (laneFrozen(owner, key) && !carriesFrozen(owner, carried)) {
            Q_EMIT refused(QStringLiteral("%1 is frozen: unfreeze it to change its automation")
                               .arg(p.track(*p.frozenBy(owner)).name));
            return false;
        }
    }
    std::optional<Macro> macro;
    if (!envelopes.isEmpty()) macro.emplace(undoStack_, text);
    if (before != after || frozenChanges) undoStack_->push(command.release());
    for (auto it = envelopes.constBegin(); it != envelopes.constEnd(); ++it) {  // (checked above)
        const auto& [owner, key] = it.key();
        const Envelope nw = automation::normalize(it.value());
        const Envelope old = p.envelope(owner, key);
        if (nw != old) {
            undoStack_->push(std::make_unique<SetEnvelopeCommand>(project_, owner, key, old, nw, text).release());
        }
    }
    return true;
}

QMap<LaneRef, Envelope> ProjectEditor::carriedAutomation(const std::vector<Span>& spans, double deltaBeats,
                                                        bool copyClips) const {
    // The envelopes after the automation under moving (or copied) clips went
    // with them, unless automation is locked. `spans` are (source track,
    // destination track, start, end) of the clips. Only envelopes with
    // breakpoints under the clips move. Across tracks, only the mixer's
    // automation goes along (a device's belongs to its track); a device's stays
    // where it is.
    const Project& p = *project_;
    if (p.automationLocked()) return {};
    OrderedMap<std::pair<QString, QString>, std::vector<std::pair<double, double>>> byMove;
    for (const Span& span : spans) {
        if (span.end > span.start) byMove[{span.source, span.dest}].emplace_back(span.start, span.end);
    }
    QMap<LaneRef, Envelope> changed;
    QMap<LaneRef, std::set<double>> edges;
    const auto current = [&](const QString& owner, const QString& key) {
        const LaneRef lane{owner, key};
        return changed.contains(lane) ? changed.value(lane) : p.envelope(owner, key);
    };
    struct Paste {
        QString dest;
        QString key;
        double at;
        double length;
        Envelope content;
    };
    std::vector<Paste> pastes;
    for (const auto& [move, ranges] : byMove) {
        const auto& [source, dest] = move;
        if (source == dest && deltaBeats == 0 && !copyClips) continue;
        for (const auto& [start, end] : automation::mergeSpans(ranges)) {
            for (const auto& [key, points] : p.automation(source)) {
                if ((dest != source && !automation::isMixerKey(key)) || !automation::hasPointsIn(points, start, end)) {
                    continue;
                }
                pastes.push_back({dest, key, start + deltaBeats, end - start, automation::copyRange(points, start, end)});
                if (!copyClips) {
                    changed.insert({source, key}, automation::removeRange(current(source, key), start, end));
                    edges[{source, key}].insert({start, end});
                }
            }
        }
    }
    for (const Paste& paste : pastes) {
        changed.insert({paste.dest, paste.key},
                       automation::pasteRange(current(paste.dest, paste.key), paste.content, paste.at, paste.length));
        edges[{paste.dest, paste.key}].insert({paste.at, paste.at + paste.length});
    }
    QMap<LaneRef, Envelope> result;
    for (auto it = changed.constBegin(); it != changed.constEnd(); ++it) {
        const Envelope points = automation::dropRedundant(it.value(), beats(edges.value(it.key())));
        if (points != p.envelope(it.key().first, it.key().second)) result.insert(it.key(), points);
    }
    return result;
}

ClipRefs ProjectEditor::addClips(const QString& trackId, double startBeat,
                                 const std::vector<std::pair<QString, double>>& sources, int trackIndex) {
    Project& p = *project_;
    if (sources.empty()) return {};
    if (!trackId.isEmpty() && p.isFrozen(trackId) && p.track(trackId).isAudio()) {
        Q_EMIT refused(QStringLiteral("%1 is frozen: unfreeze it to change its clips").arg(p.track(*p.frozenBy(trackId)).name));
        return {};
    }
    QString target = trackId;
    ClipRefs refs;
    Macro macro(undoStack_, sources.size() == 1 ? QStringLiteral("Add Clip") : QStringLiteral("Add Clips"));
    if (target.isEmpty() || !p.track(target).isAudio()) {
        target = addAudioTrack(trackIndex);  // (named by the clips: "3 Kick")
        if (target.isEmpty()) return {};
    }
    const double tempo = p.tempo();
    std::vector<Clip> clips = p.track(target).clips;
    QSet<QString> newIds;
    double position = std::max(0.0, startBeat);
    for (const auto& [path, duration] : sources) {
        const QFileInfo file(path);
        Clip clip = Clip::audio(newId(), path, file.completeBaseName(), position, duration, 0.0, duration);
        clipSettings(file.fileName(), duration, tempo, p.key()).applyTo(clip);
        newIds.insert(clip.id);
        refs.append({target, clip.id});
        position = clip.endBeat(tempo);
        clips.push_back(std::move(clip));
    }
    commitClips(QStringLiteral("Add Clip"), {{target, edits::resolveOverlaps(clips, newIds, tempo)}});
    return refs;
}

std::optional<ClipRef> ProjectEditor::addMidiClip(const QString& trackId, double startBeat, double lengthBeats) {
    const Track* track = project_->findTrack(trackId);
    if (track == nullptr || !track->isMidi() || lengthBeats < edits::kMinMidiClipBeats) return std::nullopt;
    const Clip clip = Clip::midi(newId(), QString(), std::max(0.0, startBeat), lengthBeats);
    std::vector<Clip> clips = track->clips;
    clips.push_back(clip);
    commitClips(QStringLiteral("Insert MIDI Clip"),
                {{trackId, edits::resolveOverlaps(clips, {clip.id}, project_->tempo())}});
    if (project_->findClip(trackId, clip.id) == nullptr) return std::nullopt;  // (not on a frozen track)
    return ClipRef{trackId, clip.id};
}

ClipRefs ProjectEditor::addMidiClipsOver(double startBeat, double endBeat, const QStringList& trackIds) {
    ClipRefs refs;
    for (const QString& id : trackIds) {
        if (!project_->hasTrack(id) || !project_->track(id).isMidi()) continue;
        if (const auto ref = addMidiClip(id, startBeat, endBeat - startBeat)) refs.append(*ref);
    }
    return refs;
}

std::pair<double, double> ProjectEditor::midiClipSpan(const QString& trackId, double beat, double gridStep) const {
    const double tempo = project_->tempo();
    const auto& clips = project_->track(trackId).clips;
    beat = std::max(0.0, beat);
    double start = gridStep > 0 ? std::floor(beat / gridStep + 1e-9) * gridStep : beat;
    for (const Clip& c : clips) {  // not reaching back over the clip before
        if (c.endBeat(tempo) <= beat + edits::kEps) start = std::max(start, c.endBeat(tempo));
    }
    double end = start + project_->timeSignature().beatsPerBar();
    for (const Clip& c : clips) {
        if (c.startBeat > start + edits::kEps) end = std::min(end, c.startBeat);
    }
    return {start, end - start};
}

void ProjectEditor::setClipNotes(const ClipRef& ref, const std::vector<Note>& clipNotes, const QString& text,
                                 const QString& mergeKey) {
    setClipsNotes({{ref, clipNotes}}, text, mergeKey);
}

void ProjectEditor::setClipsNotes(const std::vector<std::pair<ClipRef, std::vector<Note>>>& clipNotes,
                                  const QString& text, const QString& mergeKey) {
    ClipRefs refs;
    QHash<QString, std::vector<Note>> normalized;  // clip id -> its notes (ids are unique in the project)
    for (const auto& [ref, list] : clipNotes) {
        refs.append(ref);
        normalized.insert(ref.clipId, notes::normalize(list));
    }
    updateClips(
        refs,
        [&](const Clip& clip) {
            Clip changed = clip;
            changed.notes = normalized.value(clip.id, clip.notes);
            return changed;
        },
        text, mergeKey);
}

int ProjectEditor::clampTrackDelta(const ClipRefs& refs, int trackDelta) const {
    const Project& p = *project_;
    std::vector<int> indices;
    for (const ClipRef& ref : refs) indices.push_back(p.trackIndex(ref.trackId));
    if (indices.empty()) return 0;
    const auto [lowest, highest] = std::minmax_element(indices.begin(), indices.end());
    trackDelta = std::max(-*lowest, std::min(trackDelta, static_cast<int>(p.tracks().size()) - 1 - *highest));
    for (int i : indices) {
        if (p.tracks()[i + trackDelta].kind != p.tracks()[i].kind) return 0;
    }
    return trackDelta;
}

ClipRefs ProjectEditor::moveClips(const ClipRefs& refs, double deltaBeats, int trackDelta, bool copyClips) {
    const Project& p = *project_;
    const double tempo = p.tempo();
    std::vector<std::pair<QString, Clip>> moving;
    for (const ClipRef& ref : refs) moving.emplace_back(ref.trackId, p.clip(ref.trackId, ref.clipId));
    if (moving.empty()) return {};
    // Keep the whole group inside the timeline, the track list, and tracks of its kind.
    double earliest = moving.front().second.startBeat;
    for (const auto& [_, clip] : moving) earliest = std::min(earliest, clip.startBeat);
    deltaBeats = std::max(deltaBeats, -earliest);
    std::vector<int> indices;
    for (const auto& [trackId, _] : moving) indices.push_back(p.trackIndex(trackId));
    trackDelta = clampTrackDelta(refs, trackDelta);

    QMap<QString, std::vector<Clip>> lists;
    for (const Track& t : p.tracks()) lists.insert(t.id, t.clips);
    QSet<QString> affected;
    QMap<QString, QSet<QString>> winners;
    ClipRefs result;
    QSet<QString> movingIds;
    for (const auto& [_, clip] : moving) movingIds.insert(clip.id);
    if (!copyClips) {
        for (const auto& [trackId, _] : moving) {
            auto& list = lists[trackId];
            list.erase(std::remove_if(list.begin(), list.end(), [&](const Clip& c) { return movingIds.contains(c.id); }),
                       list.end());
            affected.insert(trackId);
        }
    }
    std::vector<Span> spans;
    for (std::size_t i = 0; i < moving.size(); ++i) {
        const Clip& clip = moving[i].second;
        const QString dest = p.tracks()[indices[i] + trackDelta].id;
        Clip moved = clip;
        moved.startBeat = clip.startBeat + deltaBeats;
        if (copyClips) moved.id = newId();
        winners[dest].insert(moved.id);
        affected.insert(dest);
        result.append({dest, moved.id});
        lists[dest].push_back(std::move(moved));
        spans.push_back({moving[i].first, dest, clip.startBeat, clip.endBeat(tempo)});
    }
    QMap<QString, std::vector<Clip>> after;
    for (const QString& id : affected) after.insert(id, edits::resolveOverlaps(lists.value(id), winners.value(id), tempo));
    commitMoved(copyClips ? QStringLiteral("Copy Clips") : QStringLiteral("Move Clips"), after,
                carriedAutomation(spans, deltaBeats, copyClips));
    return result;
}

void ProjectEditor::replaceClip(const QString& trackId, const Clip& clip, const QString& text) {
    std::vector<Clip> clips = project_->track(trackId).clips;
    for (Clip& c : clips) {
        if (c.id == clip.id) c = clip;
    }
    commitClips(text, {{trackId, edits::resolveOverlaps(clips, {clip.id}, project_->tempo())}});
}

void ProjectEditor::updateClips(const ClipRefs& refs, const std::function<Clip(const Clip&)>& change,
                                const QString& text, const QString& mergeKey) {
    const auto ids = idsByTrack(refs);
    ClipLists current;
    for (const auto& [trackId, _] : ids) current.insert(trackId, project_->track(trackId).clips);
    // Within one drag, work from the clips as they were when the drag began, so
    // dragging the segment BPM down and back up doesn't leave clips trimmed.
    ClipLists baseline = current;
    const int index = undoStack_->index();
    const auto* last = index > 0 ? dynamic_cast<const SetClipsCommand*>(undoStack_->command(index - 1)) : nullptr;
    if (!mergeKey.isEmpty() && last != nullptr && last->mergeKey() == mergeKey && last->before().keys() == current.keys()) {
        baseline = last->before();
    }
    const double tempo = project_->tempo();
    ClipLists after;
    for (auto it = baseline.constBegin(); it != baseline.constEnd(); ++it) {
        const QSet<QString> changing = ids.value(it.key());
        std::vector<Clip> clips;
        for (const Clip& c : it.value()) clips.push_back(changing.contains(c.id) ? change(c) : c);
        after.insert(it.key(), edits::fitToTempo(clips, tempo));
    }
    commitClips(text, after, mergeKey);
}

void ProjectEditor::deleteClips(const ClipRefs& refs) {
    QMap<QString, std::vector<Clip>> after;
    for (const auto& [trackId, ids] : idsByTrack(refs)) {
        std::vector<Clip> kept;
        for (const Clip& c : project_->track(trackId).clips) {
            if (!ids.contains(c.id)) kept.push_back(c);
        }
        after.insert(trackId, kept);
    }
    commitClips(refs.size() > 1 ? QStringLiteral("Delete Clips") : QStringLiteral("Delete Clip"), after);
}

ClipRefs ProjectEditor::duplicateClips(const ClipRefs& refs) {
    std::vector<Clip> clips;
    for (const ClipRef& ref : refs) clips.push_back(project_->clip(ref.trackId, ref.clipId));
    if (clips.empty()) return {};
    const auto [start, end] = edits::selectionSpan(clips, project_->tempo());
    return moveClips(refs, end - start, 0, true);
}

void ProjectEditor::splitClips(const ClipRefs& refs, double atBeat) {
    const double tempo = project_->tempo();
    QMap<QString, std::vector<Clip>> after;
    for (const ClipRef& ref : refs) {
        std::vector<Clip> clips = after.contains(ref.trackId) ? after.value(ref.trackId) : project_->track(ref.trackId).clips;
        for (std::size_t i = 0; i < clips.size(); ++i) {
            if (clips[i].id != ref.clipId) continue;
            if (const auto parts = edits::splitClip(clips[i], atBeat, tempo)) {
                clips[i] = parts->first;
                clips.insert(clips.begin() + static_cast<std::ptrdiff_t>(i) + 1, parts->second);
                after.insert(ref.trackId, clips);
            }
            break;
        }
    }
    if (!after.isEmpty()) commitClips(QStringLiteral("Split"), after);
}

OrderedMap<QString, std::vector<Clip>> ProjectEditor::consolidatable(const ClipRefs& refs) const {
    OrderedMap<QString, std::vector<Clip>> byTrack;
    for (const ClipRef& ref : refs) {
        const Clip& clip = project_->clip(ref.trackId, ref.clipId);
        if (clip.isMidi()) byTrack[ref.trackId].push_back(clip);
    }
    OrderedMap<QString, std::vector<Clip>> result;
    for (const auto& [trackId, clips] : byTrack) {
        if (clips.size() > 1) result.insert(trackId, clips);
    }
    return result;
}

ClipRefs ProjectEditor::consolidateClips(const ClipRefs& refs) {
    const double tempo = project_->tempo();
    QMap<QString, std::vector<Clip>> after;
    ClipRefs joined;
    for (const auto& [trackId, clips] : consolidatable(refs)) {
        const Clip clip = edits::consolidateMidi(clips);
        QSet<QString> ids;
        for (const Clip& c : clips) ids.insert(c.id);
        std::vector<Clip> kept;
        for (const Clip& c : project_->track(trackId).clips) {
            if (!ids.contains(c.id)) kept.push_back(c);
        }
        kept.push_back(clip);
        after.insert(trackId, edits::resolveOverlaps(kept, {clip.id}, tempo));
        joined.append({trackId, clip.id});
    }
    if (!after.isEmpty()) commitClips(QStringLiteral("Consolidate"), after);
    return joined;
}

ClipRefs ProjectEditor::clipsAt(const QStringList& trackIds, double beat) const {
    const double tempo = project_->tempo();
    ClipRefs refs;
    for (const QString& id : trackIds) {
        const Track* track = project_->findTrack(id);
        if (track == nullptr) continue;
        for (const Clip& c : track->clips) {
            if (c.startBeat < beat && beat < c.endBeat(tempo)) refs.append({id, c.id});
        }
    }
    return refs;
}

std::optional<TimeRange> ProjectEditor::clipsArea(const ClipRefs& refs) const {
    const Project& p = *project_;
    const auto& tracks = p.tracks();
    QHash<ClipRef, std::pair<int, const Clip*>> byRef;
    for (int row = 0; row < static_cast<int>(tracks.size()); ++row) {
        for (const Clip& c : tracks[row].clips) byRef.insert({tracks[row].id, c.id}, {row, &c});
    }
    int top = -1;
    int bottom = -1;
    double start = 0.0;
    double end = 0.0;
    for (const ClipRef& ref : refs) {
        const auto found = byRef.constFind(ref);
        if (found == byRef.constEnd()) continue;
        const auto [row, clip] = found.value();
        if (top < 0) {
            top = bottom = row;
            start = clip->startBeat;
            end = clip->endBeat(p.tempo());
        } else {
            top = std::min(top, row);
            bottom = std::max(bottom, row);
            start = std::min(start, clip->startBeat);
            end = std::max(end, clip->endBeat(p.tempo()));
        }
    }
    if (top < 0) return std::nullopt;
    TimeRange area{start, end, {}};
    for (int row = top; row <= bottom; ++row) area.trackIds.append(tracks[row].id);
    return area;
}

QSet<ClipRef> ProjectEditor::clipsInRange(double start, double end, const QStringList& trackIds) const {
    const double tempo = project_->tempo();
    QSet<ClipRef> refs;
    for (const QString& id : trackIds) {
        const Track* track = project_->findTrack(id);
        if (track == nullptr) continue;
        for (const Clip& c : track->clips) {
            if (c.startBeat < end && c.endBeat(tempo) > start) refs.insert({id, c.id});
        }
    }
    return refs;
}

// --- Time selections ---

QMap<LaneRef, Envelope> ProjectEditor::clearedAutomation(double start, double end, const QStringList& trackIds) const {
    // The envelopes of these tracks with their automation between two beats
    // deleted (none if automation is locked: it stays where it is).
    const Project& p = *project_;
    if (p.automationLocked()) return {};
    QMap<LaneRef, Envelope> changed;
    for (const QString& id : trackIds) {
        for (const auto& [key, points] : p.automation(id)) {
            if (!automation::hasPointsIn(points, start, end)) continue;
            const Envelope cleared = automation::dropRedundant(automation::removeRange(points, start, end), {start, end});
            if (cleared != points) changed.insert({id, key}, cleared);
        }
    }
    return changed;
}

bool ProjectEditor::deleteRange(double start, double end, const QStringList& trackIds) {
    const double tempo = project_->tempo();
    QStringList tracks;
    QMap<QString, std::vector<Clip>> after;
    for (const QString& id : trackIds) {
        if (!project_->hasTrack(id)) continue;
        tracks.append(id);
        after.insert(id, edits::removeRange(project_->track(id).clips, start, end, tempo));
    }
    if (const auto problem = frozenAreaProblem(tracks)) {
        Q_EMIT refused(*problem);
        return false;
    }
    QMap<QString, std::vector<Clip>> frozen;
    for (const QString& holder : frozenRenders(tracks)) {
        frozen.insert(holder, edits::removeRange(frozenSegments(holder), start, end, tempo));
    }
    return commitMoved(QStringLiteral("Delete Time Selection"), after, clearedAutomation(start, end, tracks), frozen);
}

std::optional<ClipRefs> ProjectEditor::duplicateRange(double start, double end, const QStringList& trackIds) {
    const Project& p = *project_;
    const double tempo = p.tempo();
    const double length = end - start;
    QStringList tracks;
    QMap<QString, std::vector<Clip>> after;
    ClipRefs result;
    std::vector<Span> spans;
    for (const QString& id : trackIds) {
        if (!p.hasTrack(id)) continue;
        tracks.append(id);
        spans.push_back({id, id, start, end});
        const auto& clips = p.track(id).clips;
        std::vector<Clip> copies = edits::sliceRange(clips, start, end, tempo);
        // On a frozen track the copy replaces everything where it lands, as its frozen audio's does.
        const bool frozen = p.isFrozen(id);
        if (copies.empty() && !frozen) continue;
        QSet<QString> ids;
        std::vector<Clip> all = frozen ? edits::removeRange(clips, end, end + length, tempo) : clips;
        for (Clip& copy : copies) {
            copy.startBeat += length;
            ids.insert(copy.id);
            result.append({id, copy.id});
            all.push_back(copy);
        }
        after.insert(id, edits::resolveOverlaps(all, ids, tempo));
    }
    if (const auto problem = frozenAreaProblem(tracks)) {
        Q_EMIT refused(*problem);
        return std::nullopt;
    }
    QMap<QString, std::vector<Clip>> frozen;
    for (const QString& holder : frozenRenders(tracks)) {
        frozen.insert(holder, movedBlock(frozenSegments(holder), start, end, length, true, tempo));
    }
    const QString text = QStringLiteral("Duplicate Time Selection");
    if (!commitMoved(text, after, carriedAutomation(spans, length, true), frozen)) return std::nullopt;
    return result;
}

std::optional<ClipboardContent> ProjectEditor::copyRange(double start, double end, const QStringList& trackIds) const {
    const Project& p = *project_;
    const double tempo = p.tempo();
    if (end <= start) return std::nullopt;
    QStringList ids;
    for (const QString& id : trackIds) {
        if (p.hasTrack(id) && !ids.contains(id)) ids.append(id);
    }
    std::sort(ids.begin(), ids.end(), [&](const QString& a, const QString& b) { return p.trackIndex(a) < p.trackIndex(b); });
    ClipboardContent content{end - start, {}};
    // The frozen audio there of each frozen track (or group) the selection takes in whole.
    for (const QString& holder : frozenRenders(ids)) {
        if (!coversFrozen(holder, ids)) continue;
        std::vector<Clip> segments = edits::sliceRange(frozenSegments(holder), start, end, tempo);
        for (Clip& segment : segments) segment.startBeat -= start;
        content.frozen.push_back({holder, p.track(holder).frozen->path, std::move(segments)});
    }
    int top = -1;  // rows count from the topmost track with content (clips, automation, its frozen audio)
    for (const QString& id : ids) {
        const Track& track = p.track(id);
        CopiedTrack copied{id, track.kind, 0, edits::sliceRange(track.clips, start, end, tempo), {}};
        for (Clip& c : copied.clips) c.startBeat -= start;
        if (!p.automationLocked()) {
            for (const auto& [key, points] : track.automation) {
                if (automation::hasPointsIn(points, start, end)) {
                    copied.automation.emplace_back(key, automation::copyRange(points, start, end));
                }
            }
        }
        // (A frozen track (not a group) with frozen audio there is copied too, to paste it back onto.)
        const CopiedFreeze* audio = track.isGroup() ? nullptr : content.frozenOf(id);
        const bool audible = audio != nullptr && !audio->segments.empty();
        if (copied.clips.empty() && copied.automation.empty() && !audible) continue;
        const int index = p.trackIndex(id);
        if (top < 0) top = index;
        copied.row = index - top;
        content.tracks.push_back(std::move(copied));
    }
    if (content.tracks.empty()) return std::nullopt;
    return content;
}

std::optional<ClipboardContent> ProjectEditor::cutRange(double start, double end, const QStringList& trackIds) {
    QStringList tracks;
    for (const QString& id : trackIds) {
        if (project_->hasTrack(id) && !tracks.contains(id)) tracks.append(id);
    }
    if (const auto problem = frozenAreaProblem(tracks)) {
        Q_EMIT refused(*problem);
        return std::nullopt;
    }
    auto content = copyRange(start, end, trackIds);
    if (!content) return std::nullopt;
    const double tempo = project_->tempo();
    QMap<QString, std::vector<Clip>> after;
    QMap<LaneRef, Envelope> envelopes;
    for (const CopiedTrack& copied : content->tracks) {
        after.insert(copied.trackId, edits::removeRange(project_->track(copied.trackId).clips, start, end, tempo));
        for (const auto& [key, _] : copied.automation) {
            const Envelope current = project_->envelope(copied.trackId, key);
            const Envelope points = automation::dropRedundant(automation::removeRange(current, start, end), {start, end});
            if (points != current) envelopes.insert({copied.trackId, key}, points);
        }
    }
    QMap<QString, std::vector<Clip>> frozen;
    for (const QString& holder : frozenRenders(tracks)) {
        frozen.insert(holder, edits::removeRange(frozenSegments(holder), start, end, tempo));
    }
    if (!commitMoved(QStringLiteral("Cut"), after, envelopes, frozen)) return std::nullopt;
    return content;
}

std::optional<QStringList> ProjectEditor::pasteTargets(const ClipboardContent& content, const QString& trackId) const {
    const Project& p = *project_;
    const auto& tracks = p.tracks();
    if (!trackId.isEmpty() && p.hasTrack(trackId)) {
        const int top = p.trackIndex(trackId);
        QStringList dests;
        for (const CopiedTrack& copied : content.tracks) {
            const int row = top + copied.row;
            if (row >= static_cast<int>(tracks.size()) || tracks[row].kind != copied.kind) break;
            dests.append(tracks[row].id);
        }
        if (dests.size() == static_cast<qsizetype>(content.tracks.size())) return dests;
    }
    QStringList dests;
    for (const CopiedTrack& copied : content.tracks) {
        if (!p.hasTrack(copied.trackId) || p.track(copied.trackId).kind != copied.kind) return std::nullopt;
        dests.append(copied.trackId);
    }
    return dests;
}

std::optional<TimeRange> ProjectEditor::paste(const ClipboardContent& content, double atBeat, const QString& trackId) {
    const Project& p = *project_;
    const auto dests = pasteTargets(content, trackId);
    if (!dests) return std::nullopt;
    const double tempo = p.tempo();
    const double at = std::max(0.0, atBeat);
    // Into frozen tracks (and what is in frozen groups) only what was copied
    // from them, with their frozen audio (the same render), onto the tracks it
    // came from.
    const QStringList renders = frozenRenders(*dests);
    for (const QString& holder : renders) {
        const CopiedFreeze* audio = content.frozenOf(holder);
        bool same = audio != nullptr && audio->path == p.track(holder).frozen->path;
        for (qsizetype i = 0; same && i < dests->size(); ++i) {
            const QString& dest = dests->at(i);
            const bool held = dest == holder || p.isDescendant(dest, holder);
            if (held && dest != content.tracks[static_cast<size_t>(i)].trackId) same = false;
        }
        if (!same) {
            Q_EMIT refused(QStringLiteral("%1 is frozen: only what was copied from it can be pasted into it")
                               .arg(p.track(holder).name));
            return std::nullopt;
        }
    }
    QMap<QString, std::vector<Clip>> lists;
    QMap<QString, QSet<QString>> winners;
    QMap<LaneRef, Envelope> changed;
    QMap<QString, std::vector<Clip>> frozen;
    for (const QString& holder : renders) {
        // What is pasted replaces everything where it lands, as its frozen audio does.
        for (const QString& id : p.withContents({holder})) {
            if (p.track(id).hasClips() && !lists.contains(id)) {
                lists.insert(id, edits::removeRange(p.track(id).clips, at, at + content.length, tempo));
            }
        }
        std::vector<Clip> segments = edits::removeRange(frozenSegments(holder), at, at + content.length, tempo);
        for (const Clip& segment : content.frozenOf(holder)->segments) {
            Clip pasted = segment;
            pasted.id = newId();
            pasted.startBeat = segment.startBeat + at;
            segments.push_back(std::move(pasted));
        }
        frozen.insert(holder, segments);
    }
    for (std::size_t i = 0; i < content.tracks.size(); ++i) {
        const CopiedTrack& copied = content.tracks[i];
        const QString& dest = dests->at(static_cast<qsizetype>(i));
        if (!lists.contains(dest)) lists.insert(dest, p.track(dest).clips);
        auto& winning = winners[dest];
        for (const Clip& c : copied.clips) {
            Clip pasted = c;
            pasted.id = newId();
            pasted.startBeat = c.startBeat + at;
            winning.insert(pasted.id);
            lists[dest].push_back(std::move(pasted));
        }
        if (p.automationLocked()) continue;
        for (const auto& [key, points] : copied.automation) {
            if (dest != copied.trackId && !automation::kMixerKeys.contains(key)) continue;
            const LaneRef lane{dest, key};
            const Envelope current = changed.contains(lane) ? changed.value(lane) : p.envelope(dest, key);
            changed.insert(lane, automation::pasteRange(current, points, at, content.length));
        }
    }
    QMap<QString, std::vector<Clip>> after;
    for (auto it = lists.constBegin(); it != lists.constEnd(); ++it) {
        after.insert(it.key(), edits::resolveOverlaps(it.value(), winners.value(it.key()), tempo));
    }
    QMap<LaneRef, Envelope> envelopes;
    for (auto it = changed.constBegin(); it != changed.constEnd(); ++it) {
        const Envelope points = automation::dropRedundant(it.value(), {at, at + content.length});
        if (points != p.envelope(it.key().first, it.key().second)) envelopes.insert(it.key(), points);
    }
    if (!commitMoved(QStringLiteral("Paste"), after, envelopes, frozen)) return std::nullopt;
    TimeRange area{at, at + content.length, {}};
    std::vector<int> rows;
    for (const QString& dest : *dests) {
        if (p.hasTrack(dest)) rows.push_back(p.trackIndex(dest));
    }
    if (!rows.empty()) {
        const auto [top, bottom] = std::minmax_element(rows.begin(), rows.end());
        for (int row = *top; row <= *bottom; ++row) area.trackIds.append(p.tracks()[row].id);
    }
    return area;
}

MovedRange ProjectEditor::movedRange(double start, double end, const QStringList& trackIds, double deltaBeats,
                                     int trackDelta, bool copyClips) const {
    const Project& p = *project_;
    const double tempo = p.tempo();
    MovedRange moved;
    moved.deltaBeats = std::max(deltaBeats, -start);
    ClipRefs refs;
    for (const QString& id : trackIds) refs.append({id, QString()});
    moved.trackDelta = clampTrackDelta(refs, trackDelta);
    QMap<QString, std::vector<Clip>> lists;
    for (const Track& t : p.tracks()) lists.insert(t.id, t.clips);
    QSet<QString> affected;
    QMap<QString, QSet<QString>> winners;
    // Moved clips that were wholly inside the range stay the same clips.
    QMap<QString, std::vector<Clip>> pieces;
    for (const QString& id : trackIds) pieces.insert(id, edits::sliceRange(lists.value(id), start, end, tempo, !copyClips));
    for (const QString& id : trackIds) {
        if (!copyClips && !pieces.value(id).empty()) {
            lists.insert(id, edits::removeRange(lists.value(id), start, end, tempo));
            affected.insert(id);
        }
    }
    // Frozen tracks move in time only (moveRange refuses more), their frozen
    // audio along; there the moved stretch replaces everything where it lands,
    // as that audio does.
    if (moved.trackDelta == 0) {
        for (const QString& id : trackIds) {
            if (!p.isFrozen(id)) continue;
            const double at = start + moved.deltaBeats;
            lists.insert(id, edits::removeRange(lists.value(id), at, at + end - start, tempo));
            affected.insert(id);
        }
        for (const QString& holder : frozenRenders(trackIds)) {
            moved.frozen.insert(holder,
                                movedBlock(frozenSegments(holder), start, end, moved.deltaBeats, copyClips, tempo));
        }
    }
    for (const QString& id : trackIds) {
        const QString dest = p.tracks()[p.trackIndex(id) + moved.trackDelta].id;
        for (const Clip& c : pieces.value(id)) {
            Clip shifted = c;
            shifted.startBeat += moved.deltaBeats;
            winners[dest].insert(shifted.id);
            affected.insert(dest);
            lists[dest].push_back(std::move(shifted));
        }
    }
    for (const QString& id : affected) {
        moved.clips.insert(id, edits::resolveOverlaps(lists.value(id), winners.value(id), tempo));
    }
    return moved;
}

std::pair<double, QStringList> ProjectEditor::moveRange(double start, double end, const QStringList& trackIds,
                                                        double deltaBeats, int trackDelta, bool copyClips) {
    const Project& p = *project_;
    const MovedRange moved = movedRange(start, end, trackIds, deltaBeats, trackDelta, copyClips);
    QStringList dests;
    std::vector<Span> spans;
    for (const QString& id : trackIds) {
        const QString dest = p.tracks()[p.trackIndex(id) + moved.trackDelta].id;
        dests.append(dest);
        spans.push_back({id, dest, start, end});
    }
    const std::pair<double, QStringList> stays{start, trackIds};
    if (moved.trackDelta != 0) {  // (frozen audio is its own track's)
        for (const QString& id : trackIds + dests) {
            if (const auto holder = p.frozenBy(id)) {
                Q_EMIT refused(QStringLiteral("%1 is frozen: clips can't move between it and other tracks")
                                   .arg(p.track(*holder).name));
                return stays;
            }
        }
    }
    if (const auto problem = frozenAreaProblem(trackIds)) {
        Q_EMIT refused(*problem);
        return stays;
    }
    if (!commitMoved(copyClips ? QStringLiteral("Copy Time Selection") : QStringLiteral("Move Time Selection"),
                     moved.clips, carriedAutomation(spans, moved.deltaBeats, copyClips), moved.frozen)) {
        return stays;
    }
    return {start + moved.deltaBeats, dests};
}

QMap<QString, std::vector<Clip>> ProjectEditor::changedInRange(double start, double end, const QStringList& trackIds,
                                                               const std::function<bool(const Clip&)>& chosen,
                                                               const std::function<Clip(const Clip&)>& change,
                                                               ClipRefs& changed) const {
    const double tempo = project_->tempo();
    QMap<QString, std::vector<Clip>> after;
    for (const QString& id : trackIds) {
        if (!project_->hasTrack(id)) continue;
        const auto& clips = project_->track(id).clips;
        std::vector<Clip> inside;
        QSet<QString> ids;
        for (const Clip& c : clips) {
            if (chosen(c) && c.startBeat < end && c.endBeat(tempo) > start) {
                inside.push_back(c);
                ids.insert(c.id);
            }
        }
        if (inside.empty()) continue;
        std::vector<Clip> kept;
        for (const Clip& c : clips) {
            if (!ids.contains(c.id)) kept.push_back(c);
        }
        for (const Clip& c : edits::removeRange(inside, start, end, tempo)) kept.push_back(c);
        for (const Clip& c : edits::sliceRange(inside, start, end, tempo, true)) {
            const Clip piece = change(c);
            changed.append({id, piece.id});
            kept.push_back(piece);
        }
        std::stable_sort(kept.begin(), kept.end(), [](const Clip& a, const Clip& b) { return a.startBeat < b.startBeat; });
        after.insert(id, kept);
    }
    return after;
}

ClipRefs ProjectEditor::reverseRange(double start, double end, const QStringList& trackIds,
                                     const QMap<QString, std::pair<QString, double>>& reversedFiles) {
    ClipRefs result;
    const auto after = changedInRange(
        start, end, trackIds, [&](const Clip& c) { return c.isAudio() && reversedFiles.contains(c.path); },
        [&](const Clip& c) {
            const auto& [path, totalSec] = reversedFiles.value(c.path);
            return edits::reverseClip(c, path, totalSec);
        },
        result);
    if (!commitClips(result.size() == 1 ? QStringLiteral("Reverse Clip") : QStringLiteral("Reverse Clips"), after))
        return {};
    return result;
}

ClipRefs ProjectEditor::setRangeActive(double start, double end, const QStringList& trackIds, bool active) {
    ClipRefs result;
    const auto after = changedInRange(
        start, end, trackIds, [&](const Clip& c) { return c.muted == active; },
        [&](const Clip& c) {
            Clip piece = c;
            piece.muted = !active;
            return piece;
        },
        result);
    const QString what = result.size() == 1 ? QStringLiteral("Clip") : QStringLiteral("Clips");
    if (!commitClips((active ? QStringLiteral("Activate ") : QStringLiteral("Deactivate ")) + what, after)) return {};
    return result;
}

}  // namespace sub::app
