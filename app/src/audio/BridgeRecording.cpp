// Recording: the armed tracks' takes, written to WAV files in the recordings
// folder (MIDI takes: their notes), drawn live while they record, and handed on
// as RecordedTakes when the recording ends.

#include "audio/AudioFiles.h"
#include "audio/BridgePrivate.h"

#include "model/Project.h"
#include "model/TrackNames.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

namespace sub::app {

bool EngineBridge::isRecording() const { return !d_->recording.isEmpty(); }

QStringList EngineBridge::recordTargets() const {
    QStringList targets;
    for (const Track& track : project_->tracks()) {
        if (track.armed && track.hasInput() && !project_->isFrozen(track.id)) targets.append(track.id);
    }
    return targets;
}

QString EngineBridge::startRecording(double countInBeats) {
    if (isRecording()) return {};
    const QStringList tracks = recordTargets();
    if (tracks.isEmpty()) return QStringLiteral("Arm a MIDI track, or an audio track that has an input, to record.");
    if (!engine_.deviceStatus().open) {
        return QStringLiteral("No audio device is open. Choose one in Options > Preferences.");
    }
    QStringList audio;
    for (const QString& trackId : tracks) {
        if (!project_->track(trackId).isMidi()) audio.append(trackId);
    }
    for (const QString& trackId : audio) {
        pushInput(trackId);  // (a source the engine couldn't take before)
        const std::vector<int> input = project_->track(trackId).input;
        if (!input.empty()) openInputs(input);
    }
    const QString folder = recordingsFolder(*project_);
    if (!audio.isEmpty() && !QDir().mkpath(folder)) {
        return QStringLiteral("Could not create the recordings folder %1").arg(QDir::toNativeSeparators(folder));
    }
    const QDateTime now = QDateTime::currentDateTime();  // local time, in the file names
    std::vector<sub::RecordTarget> targets;
    QSet<QString> paths;
    for (const QString& trackId : tracks) {
        const Track& track = project_->track(trackId);
        const quint32 engineId = d_->trackIds.value(trackId);
        if (track.isMidi()) {
            targets.push_back({engineId, {}});  // its notes, no file
            continue;
        }
        QString path = takePath(folder, takeName(track), now);
        while (paths.contains(path)) {  // two tracks of the same name
            const QFileInfo info(path);
            path = info.dir().filePath(info.completeBaseName() + QStringLiteral("_.wav"));
        }
        paths.insert(path);
        targets.push_back({engineId, path.toStdString()});
    }
    try {
        engine_.startRecording(targets, countInBeats);
    } catch (const std::exception& error) {
        return QString::fromStdString(error.what());
    }
    d_->recording.clear();
    d_->liveTakes.clear();
    for (size_t i = 0; i < targets.size(); ++i) {
        const QString& trackId = tracks[static_cast<qsizetype>(i)];
        d_->recording.insert(targets[i].trackId, trackId);
        LiveTake live;
        live.trackId = trackId;
        live.midi = project_->track(trackId).isMidi();
        d_->liveTakes.insert(trackId, live);
    }
    Q_EMIT recordingChanged(true);
    pollPosition();
    return {};
}

std::vector<RecordedTake> EngineBridge::stopRecording() {
    if (d_->recording.isEmpty()) return {};
    const QMap<quint32, QString> recording = std::exchange(d_->recording, {});
    d_->liveTakes.clear();
    std::vector<RecordedTake> takes;
    for (const sub::RecordedTake& take : engine_.stopRecording()) {
        if (!take.error.empty()) Q_EMIT statusMessage(QString::fromStdString(take.error));
        if (take.droppedFrames) {
            Q_EMIT statusMessage(QStringLiteral("The disk fell behind while recording: %1 samples were lost "
                                                "(silence in the take).")
                                     .arg(take.droppedFrames));
        }
        const auto trackId = recording.constFind(take.trackId);
        if (trackId == recording.constEnd() || take.frames <= 0) continue;
        const double rate = take.sampleRate;
        RecordedTake recorded;
        recorded.trackId = *trackId;
        recorded.path = QString::fromStdString(take.path);
        recorded.startSec = static_cast<double>(take.startSample) / rate;
        recorded.durationSec = static_cast<double>(take.frames) / rate;
        recorded.midi = take.midi;
        for (const sub::RecordedNote& note : take.notes) {
            recorded.notes.push_back({static_cast<double>(note.start) / rate, static_cast<double>(note.end) / rate,
                                      note.key, note.velocity});
        }
        takes.push_back(std::move(recorded));
    }
    Q_EMIT recordingChanged(false);
    Q_EMIT recordingUpdated();
    if (!takes.empty()) Q_EMIT takesRecorded(takes);
    return takes;
}

void EngineBridge::pollRecording() {
    if (d_->recording.isEmpty()) return;
    for (const sub::RecordingProgress& progress : engine_.recordingProgress()) {
        const auto trackId = d_->recording.constFind(progress.trackId);
        if (trackId == d_->recording.constEnd()) continue;
        const auto live = d_->liveTakes.find(*trackId);
        if (live == d_->liveTakes.end()) continue;
        live->started = progress.started;
        live->startSample = progress.startSample;
        live->frames = progress.frames;
        if (progress.midi) {
            live->notes.clear();
            for (const sub::RecordedNote& note : progress.notes) {
                live->notes.push_back({note.start, note.end, note.key, note.velocity, note.channel});
            }
            continue;
        }
        if (!progress.peaks.empty()) live->addPeaks(progress.peaks);
    }
    Q_EMIT recordingUpdated();
    if (!engine_.isRecording()) {  // a locate, or a device change, ended it
        if (!engine_.deviceStatus().open || !engine_.isPlaying()) Q_EMIT statusMessage(QStringLiteral("Recording stopped."));
        stopRecording();
    }
}

const QMap<QString, LiveTake>& EngineBridge::liveTakes() const { return d_->liveTakes; }

}  // namespace sub::app
