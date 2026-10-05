// Freezing: rendering a track's signal before its fader into its frozen audio,
// and a frozen track in the engine (its frozen audio as its only clip, its
// devices' processors gone until it is unfrozen). And exporting the
// arrangement, the other render in the background.

#include "audio/AudioFiles.h"
#include "audio/BridgePrivate.h"

#include "model/Project.h"

#include <QDateTime>
#include <QDir>
#include <QFile>

namespace sub::app {

void EngineBridge::pushFrozen(const QString& trackId) {
    const auto engineId = engineTrackId(trackId);
    if (!engineId || trackId == kMaster) return;
    const bool frozen = project_->track(trackId).frozen.has_value();
    if (frozen == d_->frozen.contains(trackId)) return;
    engine_.setTrackFrozen(*engineId, frozen);
    if (frozen) {
        d_->frozen.insert(trackId);
    } else {
        d_->frozen.remove(trackId);
    }
}

// Frozen: it plays its frozen audio, and its devices go (their states kept);
// unfrozen, they come back.
void EngineBridge::onFreezeChanged(const QString& trackId) {
    const auto engineId = engineTrackId(trackId);
    if (!engineId) return;
    const Track& track = project_->track(trackId);
    const bool frozen = track.frozen.has_value();
    const bool midi = track.isMidi();
    if (frozen) {  // its plug-ins go: their states into the model, to be saved while it is frozen
        QSet<QString> ids;
        for (const Device* device : iterDevices(track.devices)) ids.insert(device->id);
        storePluginStates(ids);
    }
    pushFrozen(trackId);
    if (!frozen && midi) engine_.setTrackClips(*engineId, {});  // (its frozen audio: it plays its notes again)
    pushClips(trackId);
    syncDevices(trackId);
    pushSidechains();
}

Freeze EngineBridge::renderFreeze(const QString& trackId) {
    waitForDeviceStates();  // plug-ins and samples still loading
    const std::unique_ptr<FreezeRender> render = startFreeze(trackId);
    const std::optional<Freeze> freeze = finishFreeze(*render);
    if (!freeze) throw EditError(QStringLiteral("The render was cancelled"));
    return *freeze;
}

std::unique_ptr<FreezeRender> EngineBridge::startFreeze(const QString& trackId) {
    const Track* track = project_->findTrack(trackId);
    if (track == nullptr) throw EditError(QStringLiteral("That track can't be frozen"));
    const QString name = track->name;
    const auto engineId = engineTrackId(trackId);
    const double end = project_->endBeat();
    if (!engineId || trackId == kMaster) throw EditError(name + QStringLiteral(" can't be frozen"));
    if (end <= 0) throw EditError(QStringLiteral("There is nothing to freeze yet: the arrangement is empty"));
    const QString folder = freezeFolder(*project_);
    if (!QDir().mkpath(folder)) {
        throw EditError(QStringLiteral("Could not create the freeze folder %1").arg(QDir::toNativeSeparators(folder)));
    }
    const QString path = takePath(folder, name + QStringLiteral(" Freeze"), QDateTime::currentDateTime());
    if (isPlaying()) stop();
    try {
        std::shared_ptr<sub::RenderJob> job =
            engine_.startTrackRender(*engineId, path.toStdString(), 0.0, end, kFreezeTailSeconds);
        return std::make_unique<FreezeRender>(trackId, std::move(job), project_->tempo());
    } catch (const std::exception& error) {
        throw EditError(QString::fromStdString(error.what()));
    }
}

std::optional<Freeze> EngineBridge::finishFreeze(FreezeRender& render) {
    const std::optional<qint64> frames = render.finish();
    if (!frames) return std::nullopt;
    const QString path = render.path();
    // Decoded now, so that it plays as soon as the track is frozen (no gap while it loads).
    try {
        d_->sources.insert(sourceKey(path), engine_.loadSource(path.toStdString()));
    } catch (const std::exception& error) {
        throw EditError(QString::fromStdString(error.what()));
    }
    Q_EMIT sourceReady(path);
    Freeze freeze;
    freeze.path = path;
    freeze.durationSec = static_cast<double>(*frames) / engine_.sampleRate();
    freeze.tempo = render.tempo();
    return freeze;
}

void EngineBridge::discardFreeze(const Freeze& freeze) {
    d_->sources.remove(sourceKey(freeze.path));
    engine_.releaseUnusedSources();
    // (which also lets go of others no track plays: those are decoded again when asked for)
    for (auto it = d_->sources.begin(); it != d_->sources.end();) {
        it = engine_.cachedSource((*it)->path()) != nullptr ? std::next(it) : d_->sources.erase(it);
    }
    QFile::remove(freeze.path);
}

std::unique_ptr<EngineRender> EngineBridge::startExport(const QString& path, double startBeat, double endBeat,
                                                        int bitDepth) {
    try {
        return std::make_unique<EngineRender>(
            engine_.startExport(path.toStdString(), startBeat, endBeat, bitDepth));
    } catch (const std::exception& error) {
        throw EditError(QString::fromStdString(error.what()));
    }
}

std::optional<qint64> EngineBridge::finishExport(EngineRender& render) { return render.finish(); }

}  // namespace sub::app
