// The transport (play, stop, locate, the metronome), previews (a file, a note),
// and polling: the playhead every kPositionPollMs; every kMeterPollMs, in this
// order, recording progress, the meters, the plug-ins (the engine's idle() and
// what they reported) and the device's events. None of these wait for a render
// in the background: it renders without the engine's lock.

#include "audio/BridgePrivate.h"

#include <algorithm>
#include <cmath>

namespace sub::app {

bool EngineBridge::isPlaying() const { return engine_.isPlaying(); }

void EngineBridge::play() {
    engine_.play();
    pollPosition();
}

void EngineBridge::stop() {
    engine_.stop();
    if (!d_->recording.isEmpty()) stopRecording();
    pollPosition();
}

void EngineBridge::locate(double beat) {
    engine_.setPositionBeats(std::max(0.0, beat));
    pollPosition();
}

double EngineBridge::position() const { return engine_.positionBeats(); }

bool EngineBridge::isCountingIn() const { return engine_.isCountingIn(); }

bool EngineBridge::metronome() const { return engine_.metronome(); }

void EngineBridge::setMetronome(bool enabled) {
    if (enabled == engine_.metronome()) return;
    engine_.setMetronome(enabled);
    Q_EMIT metronomeChanged(enabled);
}

void EngineBridge::previewFile(const QString& path) {
    const int request = ++d_->previewRequest;
    requestSource(path, [this, path, request] {
        if (request != d_->previewRequest) return;  // stopped, or another file previewed, while this one loaded
        try {
            engine_.preview(path.toStdString());
        } catch (const std::invalid_argument&) {
            // the device changed rate while loading; ignore this click
        }
    });
}

void EngineBridge::stopPreview() {
    ++d_->previewRequest;
    engine_.stopPreview();
}

void EngineBridge::previewNote(const QString& trackId, int pitch, int velocity) {
    if (const auto engineId = engineTrackId(trackId)) engine_.previewNote(*engineId, pitch, velocity);
}

void EngineBridge::pollPosition() {
    const double now = engine_.positionBeats();
    const double last = d_->lastPosition;
    // (As close as math.isclose(abs_tol=1e-9) has it: unmoved.)
    if (std::abs(now - last) > std::max(1e-9 * std::max(std::abs(now), std::abs(last)), 1e-9)) {
        d_->lastPosition = now;
        Q_EMIT positionChanged(now);
    }
    const bool playing = engine_.isPlaying();
    if (playing != d_->lastPlaying) {
        d_->lastPlaying = playing;
        Q_EMIT transportChanged(playing);
    }
    const bool countingIn = engine_.isCountingIn();
    if (countingIn != d_->lastCountingIn) {
        d_->lastCountingIn = countingIn;
        Q_EMIT countingInChanged(countingIn);
    }
}

void EngineBridge::pollMeters() {
    pollRecording();
    Private& d = *d_;
    QHash<quint32, QString> byEngineId;
    for (auto it = d.trackIds.constBegin(); it != d.trackIds.constEnd(); ++it) byEngineId.insert(it.value(), it.key());
    QHash<quint32, QString> byChain;
    for (auto it = d.rackOfChain.constBegin(); it != d.rackOfChain.constEnd(); ++it) {
        if (d.chains.contains(it.key())) byChain.insert(d.chains.value(it.key()), it.key());
    }
    for (const sub::MeterReading& reading : engine_.takeMeters()) {
        MeterLevel level;
        level.left = reading.left;
        level.right = reading.right;
        if (reading.chainId) {
            const auto chain = byChain.constFind(reading.chainId);
            if (chain != byChain.constEnd()) d.chainMeters.insert(*chain, level);
            continue;
        }
        const auto key = byEngineId.constFind(reading.trackId);
        if (key != byEngineId.constEnd()) d.meters.insert(*key, level);
    }
    Q_EMIT metersUpdated();
    pollPlugins();
    if (!d.busy) pollDevice();  // not from a message loop inside a plug-in's or driver's call
}

const QMap<QString, MeterLevel>& EngineBridge::meters() const { return d_->meters; }

const QMap<QString, MeterLevel>& EngineBridge::chainMeters() const { return d_->chainMeters; }

MeterLevel EngineBridge::trackMeter(const QString& trackId) const { return d_->meters.value(trackId); }

MeterLevel EngineBridge::chainMeter(const QString& chainId) const { return d_->chainMeters.value(chainId); }

QList<float> EngineBridge::takeInputMeters() {
    const std::vector<float> peaks = engine_.takeInputMeters();
    return QList<float>(peaks.begin(), peaks.end());
}

quint64 EngineBridge::scopeWritten() const { return engine_.masterScopeWritten(); }

QList<float> EngineBridge::scopeSamples(int frames) const {
    const std::vector<float> samples = engine_.masterScope(static_cast<size_t>(std::max(0, frames)));
    return QList<float>(samples.begin(), samples.end());
}

double EngineBridge::cpuLoad() const { return engine_.cpuLoad(); }

double EngineBridge::sampleRate() const { return engine_.sampleRate(); }

}  // namespace sub::app
