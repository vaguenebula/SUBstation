#include "files/HotSwap.h"

#include <QFileInfo>
#include <QUndoStack>
#include <QUuid>

#include "audio/AudioFiles.h"
#include "audio/EngineBridge.h"
#include "browser/BrowserController.h"
#include "editor/ProjectEditor.h"
#include "model/Edits.h"
#include "model/Paths.h"
#include "model/Project.h"

namespace sub::app {

HotSwap::HotSwap(Project* project, ProjectEditor* editor, QUndoStack* undoStack, EngineBridge* bridge,
                 BrowserController* browser, QObject* parent)
    : QObject(parent), project_(project), editor_(editor), bridge_(bridge), browser_(browser) {
    connect(project_, &Project::reset, this, &HotSwap::stop);
    // Any other edit, an undo or a redo: the user is doing something else.
    connect(undoStack, &QUndoStack::indexChanged, this, [this] {
        if (!swapping_) stop();
    });
    connect(project_, &Project::clipsChanged, this, &HotSwap::prune);
    connect(project_, &Project::devicesChanged, this, &HotSwap::prune);
    for (auto gone : {&Project::trackRemoved, &Project::returnRemoved}) {
        connect(project_, gone, this, [this](const QString&, int) { prune(); });
    }
}

QString HotSwap::name() const { return path_.isEmpty() ? QString() : QFileInfo(path_).completeBaseName(); }

QString HotSwap::usesText() const { return active_ ? sub::app::usesText(*project_, uses_) : QString(); }

bool HotSwap::start(const FileUses& uses, const QString& path, const std::optional<ClipRef>& anchor) {
    stop();
    FileUses frozen;
    const FileUses changeable = changeableUses(*project_, uses, &frozen);
    if (changeable.isEmpty()) {
        if (!frozen.isEmpty())
            Q_EMIT statusMessage(QStringLiteral("Samples on frozen tracks can't be swapped: unfreeze them first"));
        return false;
    }
    if (!frozen.isEmpty()) Q_EMIT statusMessage(QStringLiteral("Samples on frozen tracks are left as they are"));
    uses_ = changeable;
    anchor_ = anchor;
    path_ = path;
    origin_ = path;
    mergeKey_ = QUuid::createUuid().toString();
    active_ = true;
    Q_EMIT changed();
    findSimilar();  // (what it is likely to be swapped with)
    return true;
}

bool HotSwap::startClip(const ClipRef& ref) {
    const Clip* clip = project_->findClip(ref.trackId, ref.clipId);
    if (!clip || !clip->isAudio() || clip->path.isEmpty()) return false;
    const FileUses uses = fileUses(*project_, clip->path);
    return !uses.isEmpty() && start(uses, clip->path, ref);
}

bool HotSwap::startFile(const QString& path) {
    const FileUses uses = fileUses(*project_, path);
    return !uses.isEmpty() && start(uses, path);
}

void HotSwap::findSimilar() {
    if (!active_ || !browser_ || path_.isEmpty()) return;
    if (!browser_->canFindSimilar()) return;
    // Started on a clip: the part of its file it plays (a kick cut out of a loop finds kicks).
    double start = 0.0;
    double length = -1.0;
    const Clip* clip = anchor_ ? project_->findClip(anchor_->trackId, anchor_->clipId) : nullptr;
    if (clip && samePath(clip->path, path_) && !edits::playsWholeFile(*clip)) {
        start = clip->offsetSec;
        length = clip->durationSec;
    }
    browser_->findSimilar(path_, start, length);
}

bool HotSwap::swap(const QString& path) {
    if (!active_ || path.isEmpty() || !isAudioFile(path) || QFileInfo(path).isDir()) return false;
    if (samePath(path, path_)) return true;
    const FileUses uses = changeableUses(*project_, uses_);
    if (uses.isEmpty()) {  // (frozen meanwhile)
        stop();
        return false;
    }
    const std::optional<AudioFileInfo> info = bridge_->fileInfo(path);  // (it says why it couldn't read it)
    if (!info || info->duration <= 0.0) return false;
    path_ = path;
    swapping_ = true;
    editor_->replaceFile(uses, path, info->duration,
                         QStringLiteral("Hot-Swap %1").arg(QFileInfo(path).completeBaseName()), mergeKey_);
    swapping_ = false;
    Q_EMIT changed();
    return true;
}

void HotSwap::commit(const QString& path) {
    if (!active_) return;
    swap(path);
    stop();
}

void HotSwap::stop() {
    if (!active_) return;
    active_ = false;
    uses_ = {};
    anchor_.reset();
    mergeKey_.clear();
    Q_EMIT changed();
}

void HotSwap::prune() {
    if (active_ && existingUses(*project_, uses_).isEmpty()) stop();
}

}  // namespace sub::app
