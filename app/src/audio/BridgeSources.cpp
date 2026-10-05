// Audio sources: files decoded in a thread pool (of two), at the engine's rate,
// for clips and previews. Each file is decoded once (keyed by sourceKey(), so
// two spellings of a file share one source); what waits for it runs on the main
// thread when it is ready.

#include "audio/AudioFiles.h"
#include "audio/BridgePrivate.h"

namespace sub::app {

std::shared_ptr<const sub::AudioSource> EngineBridge::source(const QString& path) const {
    return d_->sources.value(sourceKey(path));
}

Waveform EngineBridge::waveform(const QString& path) const { return Waveform(source(path)); }

bool EngineBridge::isLoading(const QString& path) const { return d_->loading.contains(sourceKey(path)); }

QString EngineBridge::loadError(const QString& path) const { return d_->failed.value(sourceKey(path)); }

void EngineBridge::requestSource(const QString& path, std::function<void()> then) {
    const QString key = sourceKey(path);
    const auto source = d_->sources.constFind(key);
    if (source != d_->sources.constEnd() && (*source)->sampleRate() == static_cast<uint32_t>(engine_.sampleRate())) {
        if (then) then();
        return;
    }
    if (const auto loading = d_->loading.find(key); loading != d_->loading.end()) {
        if (then) loading->push_back(std::move(then));
        return;
    }
    d_->failed.remove(key);
    std::vector<std::function<void()>> waiting;
    if (then) waiting.push_back(std::move(then));
    d_->loading.insert(key, std::move(waiting));
    // (The bridge waits for these before it goes: the result comes to it on the main thread.)
    const std::string utf8 = path.toStdString();
    d_->pool.start([this, path, utf8] {
        try {
            std::shared_ptr<sub::AudioSource> loaded = engine_.loadSource(utf8);
            QMetaObject::invokeMethod(this, [this, path, loaded] { onLoaded(path, loaded); }, Qt::QueuedConnection);
        } catch (const std::exception& error) {
            const QString message = QString::fromStdString(error.what());
            QMetaObject::invokeMethod(this, [this, path, message] { onFailed(path, message); }, Qt::QueuedConnection);
        } catch (...) {
            const QString message = QStringLiteral("Could not decode ") + path;
            QMetaObject::invokeMethod(this, [this, path, message] { onFailed(path, message); }, Qt::QueuedConnection);
        }
    });
}

void EngineBridge::onLoaded(const QString& path, std::shared_ptr<sub::AudioSource> source) {
    const QString key = sourceKey(path);
    d_->sources.insert(key, std::move(source));
    const std::vector<std::function<void()>> callbacks = d_->loading.take(key);
    Q_EMIT sourceReady(path);
    for (const auto& callback : callbacks) guarded("source ready", callback);
}

void EngineBridge::onFailed(const QString& path, const QString& message) {
    const QString key = sourceKey(path);
    d_->loading.remove(key);
    d_->failed.insert(key, message);
    Q_EMIT sourceFailed(path, message);
    Q_EMIT statusMessage(message);
}

std::optional<AudioFileInfo> EngineBridge::fileInfo(const QString& path) {
    const QString key = sourceKey(path);
    if (!d_->fileInfo.contains(key)) {
        try {
            const sub::AudioFileInfo probed = sub::AudioSource::probe(path.toStdString());
            AudioFileInfo info;
            info.frames = probed.frames;
            info.channels = static_cast<int>(probed.channels);
            info.sampleRate = static_cast<int>(probed.sampleRate);
            info.duration = probed.duration;
            d_->fileInfo.insert(key, info);
        } catch (const std::exception& error) {
            Q_EMIT statusMessage(QString::fromStdString(error.what()));
            return std::nullopt;
        }
    }
    return d_->fileInfo.value(key);
}

// After a sample-rate change: every source used is fetched again.
void EngineBridge::refreshSources() {
    const QStringList stale = d_->sources.keys();
    d_->sources.clear();
    for (const QString& key : stale) requestSource(key);
}

}  // namespace sub::app
