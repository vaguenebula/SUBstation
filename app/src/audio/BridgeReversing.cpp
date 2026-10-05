// Reversing audio: a reversed copy of a file, written as a WAV file in the
// reversed folder (the project's "Reversed" folder once it is saved), for
// reversed clips to play (as Ableton does). The copy is written a chunk at a
// time on a thread of its own (ReverseJob), so a long file neither holds up the
// window nor needs a second copy of itself in memory.

#include "audio/AudioFiles.h"
#include "audio/BridgePrivate.h"

#include "model/Project.h"

#include <QDir>
#include <QFileInfo>

namespace sub::app {

std::optional<QString> EngineBridge::reversedCopy(const QString& path) {
    const QString key = sourceKey(path);
    QString copy = d_->reversed.value(key);
    if (copy.isEmpty() || !QFileInfo(copy).isFile()) {
        copy.clear();
        for (const Track& track : project_->tracks()) {
            for (const Clip& clip : track.clips) {
                if (clip.isAudio() && !clip.reversedFrom.isEmpty() && sourceKey(clip.reversedFrom) == key &&
                    QFileInfo(clip.path).isFile()) {
                    copy = clip.path;
                    break;
                }
            }
            if (!copy.isEmpty()) break;
        }
    }
    if (copy.isEmpty()) return std::nullopt;
    d_->reversed.insert(key, copy);
    if (!source(copy)) requestSource(copy);
    return copy;
}

std::unique_ptr<ReverseJob> EngineBridge::startReversed(const QString& path) {
    const std::shared_ptr<const sub::AudioSource> decoded = source(path);
    if (!decoded) {
        const QString reason = loadError(path).isEmpty() ? QStringLiteral("is still loading") : QStringLiteral("can't be found");
        throw EditError(QFileInfo(path).fileName() + u' ' + reason + QStringLiteral(": it can't be reversed"));
    }
    const QString folder = reversedFolder(*project_);
    if (!QDir().mkpath(folder)) {
        throw EditError(QStringLiteral("Could not create the folder %1").arg(QDir::toNativeSeparators(folder)));
    }
    return std::make_unique<ReverseJob>(engine_, decoded, reversedPath(folder, path));
}

std::optional<std::pair<QString, double>> EngineBridge::finishReversed(const QString& path, ReverseJob& job) {
    std::shared_ptr<sub::AudioSource> loaded = job.finish();
    if (!loaded) return std::nullopt;
    d_->sources.insert(sourceKey(job.path()), std::move(loaded));
    d_->reversed.insert(sourceKey(path), job.path());
    Q_EMIT sourceReady(job.path());
    return std::pair{job.path(), job.seconds()};
}

std::pair<QString, double> EngineBridge::renderReversed(const QString& path) {
    const std::optional<QString> copy = reversedCopy(path);
    const auto decoded = source(path);
    if (copy && decoded) return {*copy, static_cast<double>(decoded->frames()) / decoded->sampleRate()};
    const std::unique_ptr<ReverseJob> job = startReversed(path);
    const auto reversed = finishReversed(path, *job);
    if (!reversed) throw EditError(QStringLiteral("Reversing was cancelled"));
    return *reversed;
}

}  // namespace sub::app
