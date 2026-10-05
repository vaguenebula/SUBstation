#include "session/ArrangementActions.h"

#include <QFileInfo>
#include <QMap>

#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

#include "audio/EngineBridge.h"
#include "audio/ReverseJob.h"
#include "audio/Waveform.h"
#include "editor/ProjectEditor.h"
#include "io/Serialization.h"
#include "model/Devices.h"
#include "model/Errors.h"
#include "session/RenderProgress.h"
#include "session/Renders.h"
#include "session/Selection.h"
#include "session/SessionSupport.h"

namespace sub::app {

namespace {

const QString kInstrumentsOnMidiTracks =
    QStringLiteral("Instruments go on MIDI tracks. Drop one below the tracks to make one.");

}  // namespace

ArrangementActions::ArrangementActions(ProjectEditor* editor, Selection* selection, EngineBridge* bridge,
                                       RenderProgress* progress, QObject* parent)
    : QObject(parent),
      editor_(editor),
      project_(editor->project()),
      selection_(selection),
      bridge_(bridge),
      progress_(progress) {}

QString ArrangementActions::clipboardKind() const {
    if (std::holds_alternative<ClipboardContent>(clipboard_)) return QStringLiteral("clips");
    if (std::holds_alternative<CopiedAutomation>(clipboard_)) return QStringLiteral("automation");
    if (std::holds_alternative<CopiedTracks>(clipboard_)) return QStringLiteral("tracks");
    return {};
}

void ArrangementActions::setClipboard(Clipboard clipboard) {
    clipboard_ = std::move(clipboard);
    Q_EMIT clipboardChanged();
}

void ArrangementActions::requestClipView(const ClipRefs& refs, const ClipRef& lead) {
    Q_EMIT clipViewRequested(clipRefList(refs), lead.trackId, lead.clipId);
}

// --- The selected area ------------------------------------------------------------------------

void ArrangementActions::deleteArea() {
    if (!selection_->clipRange()) return;
    const TimeRange range = *selection_->timeRange();
    if (!editor_->deleteRange(range.start, range.end, range.trackIds)) return;  // (refused: the editor said why)
    selection_->setTimeRange(range.start, range.end, range.trackIds, QSet<ClipRef>());
}

void ArrangementActions::duplicateArea() {
    if (!selection_->clipRange()) return;
    const TimeRange range = *selection_->timeRange();
    const double length = range.end - range.start;
    if (!editor_->duplicateRange(range.start, range.end, range.trackIds)) return;
    selection_->setTimeRange(range.end, range.end + length, range.trackIds,
                             editor_->clipsInRange(range.end, range.end + length, range.trackIds));
    selection_->setInsert(range.end);
}

void ArrangementActions::copyArea() {
    if (!selection_->clipRange()) return;
    const TimeRange range = *selection_->timeRange();
    auto content = editor_->copyRange(range.start, range.end, range.trackIds);
    if (!content) {
        Q_EMIT statusMessage(QStringLiteral("There is nothing in the selection to copy."));
        return;
    }
    setClipboard(std::move(*content));
}

void ArrangementActions::cutArea() {
    if (!selection_->clipRange()) return;
    const TimeRange range = *selection_->timeRange();
    if (!editor_->copyRange(range.start, range.end, range.trackIds)) {
        Q_EMIT statusMessage(QStringLiteral("There is nothing in the selection to cut."));
        return;
    }
    auto content = editor_->cutRange(range.start, range.end, range.trackIds);
    if (!content) return;  // (refused: the editor said why)
    setClipboard(std::move(*content));
    selection_->setTimeRange(range.start, range.end, range.trackIds, QSet<ClipRef>());
}

void ArrangementActions::copyAutomation() {
    if (!selection_->timeRange() || selection_->lanes().isEmpty()) return;
    const TimeRange range = *selection_->timeRange();
    auto content = editor_->copyAutomationRange(range.start, range.end, selection_->lanes());
    if (!content) {
        Q_EMIT statusMessage(QStringLiteral("There is no automation in the selection to copy."));
        return;
    }
    setClipboard(std::move(*content));
}

void ArrangementActions::cutAutomation() {
    if (!selection_->timeRange() || selection_->lanes().isEmpty()) return;
    const TimeRange range = *selection_->timeRange();
    auto content = editor_->cutAutomationRange(range.start, range.end, selection_->lanes());
    if (!content) {
        Q_EMIT statusMessage(QStringLiteral("There is no automation in the selection to cut."));
        return;
    }
    setClipboard(std::move(*content));
}

void ArrangementActions::copyTracks(const QStringList& trackIds) {
    storeTrackPluginStates(*bridge_, *project_, trackIds);
    if (auto content = editor_->copyTracks(trackIds)) setClipboard(std::move(*content));
}

void ArrangementActions::cutTracks(const QStringList& trackIds) {
    storeTrackPluginStates(*bridge_, *project_, trackIds);
    if (auto content = editor_->cutTracks(trackIds)) setClipboard(std::move(*content));
}

QStringList ArrangementActions::duplicateTracks(const QStringList& trackIds) {
    storeTrackPluginStates(*bridge_, *project_, trackIds);
    const QStringList copies = editor_->duplicateTracks(trackIds);
    for (qsizetype i = 0; i < copies.size(); ++i) {
        selection_->selectTrack(copies[i], true, i > 0 ? Selection::Mode::Toggle : Selection::Mode::Replace);
    }
    return copies;
}

// Copies of copied tracks after track `after` (and what is in it), in its
// group ("": last); they are selected.
void ArrangementActions::pasteTracks(const CopiedTracks& content, QString after) {
    if (!after.isEmpty() && !project_->hasTrack(after)) after.clear();  // (a return, or the master: last)
    const QStringList copies = editor_->pasteTracks(content, after);
    for (qsizetype i = 0; i < copies.size(); ++i) {
        selection_->selectTrack(copies[i], true, i > 0 ? Selection::Mode::Toggle : Selection::Mode::Replace);
    }
}

// Copied automation at `atBeat`: onto `lanes` (default: the selected ones) as
// ProjectEditor::automationPasteTargets puts it, else the lanes it came from.
// The pasted range is selected, and the insert marker goes to its end.
void ArrangementActions::pasteAutomation(const CopiedAutomation& content, double atBeat,
                                         std::optional<QList<LaneRef>> lanes) {
    if (!lanes) {
        lanes = selection_->timeRange() ? selection_->lanes() : QList<LaneRef>();
        if (lanes->isEmpty() && selection_->points()) lanes = QList<LaneRef>{{selection_->points()->owner, selection_->points()->key}};
    }
    const QList<LaneRef> pasted = editor_->pasteAutomation(content, atBeat, *lanes);
    if (pasted.isEmpty()) {
        Q_EMIT statusMessage(QStringLiteral("The copied automation can't go there: its lanes are gone."));
        return;
    }
    const double at = std::max(0.0, atBeat);
    QStringList trackIds;
    for (const LaneRef& lane : pasted) {
        if (project_->hasTrack(lane.first) && !trackIds.contains(lane.first)) trackIds.append(lane.first);
    }
    selection_->setTimeRange(at, at + content.length, trackIds, std::nullopt, pasted);
    selection_->setInsert(at + content.length);
}

void ArrangementActions::paste() { paste(selection_->insertBeat(), QString()); }

void ArrangementActions::paste(double atBeat, const QString& trackId) {
    if (!hasClipboard()) {
        Q_EMIT statusMessage(
            QStringLiteral("Nothing to paste: copy (Ctrl+C) or cut (Ctrl+X) clips, automation or tracks first."));
        return;
    }
    const QString onto = trackId.isEmpty() ? selection_->trackId() : trackId;
    if (const auto* tracks = std::get_if<CopiedTracks>(&clipboard_)) {
        const CopiedTracks content = *tracks;  // (the clipboard may change while pasting)
        pasteTracks(content, onto);
        return;
    }
    if (const auto* automation = std::get_if<CopiedAutomation>(&clipboard_)) {
        const CopiedAutomation content = *automation;
        pasteAutomation(content, atBeat);
        return;
    }
    const ClipboardContent content = std::get<ClipboardContent>(clipboard_);
    if (!editor_->pasteTargets(content, onto)) {
        Q_EMIT statusMessage(QStringLiteral("The copied clips can't go there: paste them onto tracks of their kind."));
        return;
    }
    const auto area = editor_->paste(content, atBeat, onto);
    if (!area) return;  // (refused: the editor said why)
    selection_->setTimeRange(area->start, area->end, area->trackIds,
                             editor_->clipsInRange(area->start, area->end, area->trackIds));
    selection_->setInsert(area->end);
}

void ArrangementActions::pasteAutomationAt(const QString& owner, const QString& key) {
    const auto* automation = std::get_if<CopiedAutomation>(&clipboard_);
    if (automation == nullptr) return;
    const LaneRef lane{owner, key};
    const bool selected = selection_->timeRange() && selection_->lanes().contains(lane);
    const CopiedAutomation content = *automation;
    pasteAutomation(content, selection_->insertBeat(), selected ? selection_->lanes() : QList<LaneRef>{lane});
}

void ArrangementActions::consolidate() {
    QList<ClipRef> refs(selection_->clips().begin(), selection_->clips().end());
    std::sort(refs.begin(), refs.end());
    const ClipRefs joined = editor_->consolidateClips(refs);
    if (!joined.isEmpty()) selection_->selectClips(*editor_, joined);
}

void ArrangementActions::splitAt(double beat) {
    QList<ClipRef> refs(selection_->clips().begin(), selection_->clips().end());
    std::sort(refs.begin(), refs.end());
    editor_->splitClips(refs, beat);
}

bool ArrangementActions::canConsolidate() const {
    QList<ClipRef> refs(selection_->clips().begin(), selection_->clips().end());
    std::sort(refs.begin(), refs.end());
    return !editor_->consolidatable(refs).isEmpty();
}

// (What R reverses: the audio clips in the selected area, whichever are selected as clips.)
bool ArrangementActions::canReverse() const {
    if (!selection_->clipRange()) return false;
    const TimeRange range = *selection_->timeRange();
    for (const ClipRef& ref : editor_->clipsInRange(range.start, range.end, range.trackIds)) {
        const Clip* clip = project_->findClip(ref.trackId, ref.clipId);
        if (clip != nullptr && clip->isAudio()) return true;
    }
    return false;
}

QVariantMap ArrangementActions::insertMidiClip(const QString& trackId, double beat, double gridStep) {
    const Track* track = project_->findTrack(trackId);
    if (track == nullptr || !track->isMidi()) return {};
    const auto& range = selection_->timeRange();
    ClipRefs refs;
    double start = 0.0;
    if (range && range->trackIds.contains(trackId) && range->start <= beat && beat <= range->end) {
        refs = editor_->addMidiClipsOver(range->start, range->end, range->trackIds);
        start = range->start;
    } else {
        const auto [at, length] = editor_->midiClipSpan(trackId, beat, gridStep);
        start = at;
        if (const auto ref = editor_->addMidiClip(trackId, at, length)) refs.append(*ref);
    }
    const auto ref = std::find_if(refs.begin(), refs.end(), [&](const ClipRef& r) { return r.trackId == trackId; });
    if (ref == refs.end()) return {};
    const ClipRef lead = *ref;
    selection_->selectClips(*editor_, refs, trackId);
    selection_->setInsert(start);
    requestClipView(ClipRefs(selection_->clips().begin(), selection_->clips().end()), lead);
    return clipRefMap(lead);
}

QString ArrangementActions::insertTrackAfter(const QString& trackId, bool midi) {
    const InsertionPoint point = editor_->insertionPoint(project_->hasTrack(trackId) ? trackId : QString());
    const TrackParent parent(point.parent);
    const int index = point.index.value_or(-1);
    return midi ? editor_->addMidiTrack(index, QString(), kDefaultInstrument, std::nullopt, parent)
                : editor_->addAudioTrack(index, QString(), parent);
}

// --- Reversing --------------------------------------------------------------------------------

void ArrangementActions::reverseSelection() {
    if (!selection_->clipRange()) {
        Q_EMIT statusMessage(QStringLiteral("Select audio clips (or a time range over them) to reverse them."));
        return;
    }
    const TimeRange range = *selection_->timeRange();
    const double tempo = project_->tempo();
    std::vector<Clip> clips;
    for (const QString& trackId : range.trackIds) {
        if (!project_->hasTrack(trackId)) continue;
        for (const Clip& clip : project_->track(trackId).clips) {
            if (clip.isAudio() && clip.startBeat < range.end && clip.endBeat(tempo) > range.start) clips.push_back(clip);
        }
    }
    if (clips.empty()) {
        Q_EMIT statusMessage(QStringLiteral("There are no audio clips in the selection to reverse."));
        return;
    }
    QMap<QString, std::pair<QString, double>> reversed;
    QStringList toWrite;  // files with no reversed copy yet
    for (const Clip& clip : clips) {
        if (reversed.contains(clip.path) || toWrite.contains(clip.path)) continue;
        const Waveform source = bridge_->waveform(clip.path);
        const double seconds =
            source.isNull() || source.sampleRate() <= 0 ? 0.0 : static_cast<double>(source.frames()) / source.sampleRate();
        if (!clip.reversedFrom.isEmpty() && QFileInfo::exists(clip.reversedFrom)) {  // back to the file it came from
            const double length = source.isNull() ? clip.sourceDurationSec : seconds;
            if (length > 0) {
                reversed.insert(clip.path, {clip.reversedFrom, length});
                continue;
            }
        }
        const std::optional<QString> copy = bridge_->reversedCopy(clip.path);
        if (copy && !source.isNull()) {
            reversed.insert(clip.path, {*copy, seconds});
        } else {
            toWrite.append(clip.path);
        }
    }

    // Reversed copies of the files that have none: short ones written at once;
    // longer ones in the background (the progress shown, and Cancel: none made).
    std::vector<std::pair<QString, std::unique_ptr<ReverseJob>>> jobs;
    for (const QString& path : toWrite) {
        try {
            jobs.emplace_back(path, bridge_->startReversed(path));
        } catch (const EditError& error) {
            Q_EMIT statusMessage(error.message());
        }
    }
    double total = 0.0;
    for (const auto& [path, job] : jobs) total += job->seconds();
    if (jobs.empty() || total <= reverseInPlaceSeconds_) {
        for (size_t i = 0; i < jobs.size(); ++i) {
            auto& [path, job] = jobs[i];
            std::optional<std::pair<QString, double>> result;
            try {
                result = bridge_->finishReversed(path, *job);
            } catch (const EditError& error) {
                Q_EMIT statusMessage(error.message());
                continue;
            }
            if (!result) return;  // (cancelled)
            reversed.insert(path, *result);
        }
        reverseWith(range.start, range.end, range.trackIds, reversed);
        return;
    }
    auto* render = new ReverseClipsRender(progress_, bridge_, std::move(jobs), this);
    connect(render, &Render::statusMessage, this, &ArrangementActions::statusMessage);
    connect(render, &Render::finished, this, [this, render, range, reversed]() mutable {
        if (!render->completed()) return;  // cancelled: nothing changes
        for (auto it = render->reversed().begin(); it != render->reversed().end(); ++it) reversed.insert(it.key(), it.value());
        reverseWith(range.start, range.end, range.trackIds, reversed);
    });
    if (startRender_) {
        startRender_(render);
    } else {
        render->start();
    }
}

void ArrangementActions::reverseWith(double start, double end, const QStringList& trackIds,
                                     const QMap<QString, std::pair<QString, double>>& reversed) {
    if (reversed.isEmpty()) return;
    if (!editor_->reverseRange(start, end, trackIds, reversed).isEmpty()) {
        selection_->setTimeRange(start, end, trackIds, editor_->clipsInRange(start, end, trackIds));
    }
}

// --- Drops ------------------------------------------------------------------------------------

QVariantList ArrangementActions::dropSources(const QStringList& paths) {
    QVariantList sources;
    for (const QString& path : paths) {
        if (const auto info = bridge_->fileInfo(path)) {
            sources.append(QVariantMap{{QStringLiteral("path"), path},
                                       {QStringLiteral("name"), fileStem(path)},
                                       {QStringLiteral("duration"), info->duration}});
        }
    }
    return sources;
}

void ArrangementActions::dropFiles(const QStringList& paths, const QString& trackId, double beat) {
    std::vector<std::pair<QString, double>> sources;
    for (const QString& path : paths) {
        if (const auto info = bridge_->fileInfo(path)) sources.emplace_back(path, info->duration);
    }
    if (sources.empty()) return;
    const Track* track = trackId.isEmpty() ? nullptr : project_->findTrack(trackId);
    // Audio goes only on an audio track: otherwise it gets a new track.
    const QString onto = track != nullptr && track->isAudio() && project_->hasTrack(trackId) ? trackId : QString();
    const ClipRefs refs =
        editor_->addClips(onto, std::max(0.0, beat), sources, static_cast<int>(project_->tracks().size()));
    if (!refs.isEmpty()) selection_->selectClips(*editor_, refs);
}

bool ArrangementActions::dropDevices(const QStringList& kinds, const QVariantList& plugins, const QString& trackId) {
    std::vector<std::pair<QString, std::optional<PluginRef>>> devices;  // built-in ones, then plug-ins
    for (const QString& kind : kinds) {
        if (builtinDevice(kind) != nullptr) devices.emplace_back(kind, std::nullopt);
    }
    for (const PluginRef& ref : pluginRefsFromList(plugins)) devices.emplace_back(kPluginKind, ref);
    if (devices.empty()) return false;
    QString target = trackId;
    if (!target.isEmpty() && project_->hasOwner(target)) {
        bool refused = false;
        for (const auto& [kind, plugin] : devices) refused = editor_->addDevice(target, kind, -1, plugin).isEmpty() || refused;
        if (refused) Q_EMIT statusMessage(kInstrumentsOnMidiTracks);
    } else {
        const auto instrument = std::find_if(devices.begin(), devices.end(),
                                             [](const auto& d) { return isInstrument(d.first, d.second); });
        if (instrument == devices.end()) return false;
        target = editor_->addMidiTrack(-1, QString(), instrument->second ? QString() : instrument->first,
                                       instrument->second);
        for (const auto& [kind, plugin] : devices) {
            if (!isInstrument(kind, plugin)) editor_->addDevice(target, kind, -1, plugin);
        }
    }
    selection_->selectTrack(target);  // show its devices
    return true;
}

bool ArrangementActions::dropPresets(const QStringList& paths, const QString& trackId) {
    std::vector<std::pair<QString, Device>> loaded;
    for (const QString& path : paths) {
        try {
            loaded.emplace_back(fileStem(path), loadPreset(path));
        } catch (const ProjectFileError& error) {
            Q_EMIT statusMessage(error.message());
        }
    }
    QString target = trackId.isEmpty() || !project_->hasOwner(trackId) ? QString() : trackId;
    if (target.isEmpty()) {
        const auto instrument = std::find_if(loaded.begin(), loaded.end(),
                                             [](const auto& entry) { return deviceIsInstrument(entry.second); });
        if (instrument == loaded.end()) return false;
        target = editor_->addMidiTrackWith(instrument->second, -1, kAtIndex,
                                           QStringLiteral("Load Preset ") + instrument->first);
        loaded.erase(instrument);
    }
    bool refused = false;
    for (const auto& [name, device] : loaded) {
        if (!editor_->insertDevice(target, device, -1, {}, QStringLiteral("Load Preset ") + name, !device.isRack())) {
            refused = true;
        }
    }
    if (refused) Q_EMIT statusMessage(kInstrumentsOnMidiTracks);
    selection_->selectTrack(target);  // show its devices
    return true;
}

bool ArrangementActions::dropMovedDevices(const QString& fromTrackId, const QStringList& deviceIds,
                                          const QString& toTrackId) {
    if (fromTrackId == toTrackId || !project_->hasOwner(fromTrackId) || !project_->hasOwner(toTrackId)) return false;
    if (!editor_->moveDevicesToTrack(fromTrackId, deviceIds, toTrackId)) return false;
    selection_->selectTrack(toTrackId);  // show them where they went
    return true;
}

}  // namespace sub::app
