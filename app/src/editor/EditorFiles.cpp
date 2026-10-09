// The files clips and devices play: one put in place of another (the File
// Manager's Replace, a hot swap), and files found somewhere else (missing
// ones located).

#include "editor/ProjectEditor.h"

#include "model/Commands.h"
#include "model/Edits.h"

#include <QSet>
#include <QUndoStack>

namespace sub::app {

bool ProjectEditor::replaceFile(const FileUses& uses, const QString& path, double seconds, const QString& text,
                                const QString& mergeKey) {
    const Project& p = *project_;
    if (path.isEmpty() || seconds <= 0.0) return false;
    QMap<QString, QSet<QString>> clipIds;
    for (const ClipRef& ref : uses.clips) {
        if (p.findClip(ref.trackId, ref.clipId)) clipIds[ref.trackId].insert(ref.clipId);
    }
    ClipLists current;
    for (auto it = clipIds.constBegin(); it != clipIds.constEnd(); ++it) {
        current.insert(it.key(), p.track(it.key()).clips);
    }
    DeviceStates statesCurrent;
    for (const DeviceRef& ref : uses.devices) {
        const Device* device = p.hasOwner(ref.trackId) ? p.findDevice(ref.trackId, ref.deviceId) : nullptr;
        if (device) statesCurrent.insert({ref.trackId, ref.deviceId}, device->state);
    }
    if (current.isEmpty() && statesCurrent.isEmpty()) return false;
    // Within one hot swap, from what was there before it began.
    ClipLists baseline = current;
    DeviceStates statesBaseline = statesCurrent;
    const int index = undoStack_->index();
    const auto* last = index > 0 ? dynamic_cast<const ReplaceFilesCommand*>(undoStack_->command(index - 1)) : nullptr;
    if (!mergeKey.isEmpty() && last != nullptr && last->mergeKey() == mergeKey &&
        last->before().keys() == current.keys() && last->statesBefore().keys() == statesCurrent.keys()) {
        baseline = last->before();
        statesBaseline = last->statesBefore();
    }
    const double tempo = p.tempo();
    ClipLists after;
    for (auto it = baseline.constBegin(); it != baseline.constEnd(); ++it) {
        const QSet<QString> replacing = clipIds.value(it.key());
        std::vector<Clip> clips;
        for (const Clip& clip : it.value()) {
            clips.push_back(replacing.contains(clip.id) ? edits::replaceFile(clip, path, seconds) : clip);
        }
        after.insert(it.key(), edits::fitToTempo(clips, tempo));
    }
    DeviceStates statesAfter;
    for (auto it = statesBaseline.constBegin(); it != statesBaseline.constEnd(); ++it) {
        statesAfter.insert(it.key(), withDeviceFile(it.value(), path));
    }
    if (after == current && statesAfter == statesCurrent) return false;
    return push(
        std::make_unique<ReplaceFilesCommand>(project_, text, current, after, statesCurrent, statesAfter, mergeKey));
}

bool ProjectEditor::relinkFiles(const QMap<QString, QString>& moved, const QString& text) {
    const Project& p = *project_;
    if (moved.isEmpty()) return false;
    ClipLists before;
    ClipLists after;
    for (const Track* track : p.allTracks()) {
        if (!track->isAudio()) continue;
        std::vector<Clip> clips = track->clips;
        bool changed = false;
        for (Clip& clip : clips) {
            for (auto it = moved.constBegin(); it != moved.constEnd(); ++it) {
                const Clip relinked = edits::relinkFile(clip, it.key(), it.value());
                if (relinked == clip) continue;
                clip = relinked;
                changed = true;
            }
        }
        if (!changed) continue;
        before.insert(track->id, track->clips);
        after.insert(track->id, clips);
    }
    DeviceStates statesBefore;
    DeviceStates statesAfter;
    for (auto it = moved.constBegin(); it != moved.constEnd(); ++it) {
        for (const DeviceRef& ref : fileUses(p, it.key()).devices) {
            const std::pair<QString, QString> key{ref.trackId, ref.deviceId};
            const std::optional<QString> state = p.device(ref.trackId, ref.deviceId).state;
            if (!statesBefore.contains(key)) statesBefore.insert(key, state);
            statesAfter.insert(key, withDeviceFile(statesAfter.value(key, state), it.value()));
        }
    }
    if (after.isEmpty() && statesAfter.isEmpty()) return false;
    return push(std::make_unique<ReplaceFilesCommand>(project_, text, before, after, statesBefore, statesAfter,
                                                      QString(), true));
}

}  // namespace sub::app
