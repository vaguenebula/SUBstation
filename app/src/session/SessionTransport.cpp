// The session's transport: play, record (with the count-in), stop, locate.

#include "session/Session.h"

#include <QSettings>

#include <algorithm>

#include "audio/EngineBridge.h"
#include "model/Project.h"
#include "session/Selection.h"

namespace sub::app {

namespace {

const std::vector<int> kCountInBars{0, 1, 2, 4};

}  // namespace

void Session::togglePlay() {
    if (bridge_->isPlaying()) {
        bridge_->stop();
        bridge_->locate(playStart_);  // Ableton: back to where playback started
    } else {
        playStart_ = selection_->insertBeat();
        bridge_->locate(playStart_);
        bridge_->play();
    }
}

void Session::toggleRecord() {
    if (bridge_->isRecording()) {
        bridge_->stopRecording();  // punch out: playing goes on
        return;
    }
    const bool playing = bridge_->isPlaying();
    if (!playing) {
        playStart_ = selection_->insertBeat();
        bridge_->locate(playStart_);
    }
    const QString error = bridge_->startRecording(playing ? 0.0 : countInBeats());
    if (!error.isEmpty()) Q_EMIT statusMessage(error);
}

void Session::stop() {
    if (bridge_->isPlaying()) {
        bridge_->stop();
        bridge_->locate(playStart_);
    } else {
        locate(0.0);
    }
}

void Session::locate(double beat) {
    selection_->setInsert(beat);
    playStart_ = beat;
    bridge_->locate(beat);
}

int Session::countInBars() const {
    bool ok = false;
    const int bars = QSettings().value(kCountInKey, 0).toInt(&ok);
    return ok && std::find(kCountInBars.begin(), kCountInBars.end(), bars) != kCountInBars.end() ? bars : 0;
}

void Session::setCountInBars(int bars) {
    if (std::find(kCountInBars.begin(), kCountInBars.end(), bars) == kCountInBars.end() || bars == countInBars()) return;
    QSettings().setValue(kCountInKey, bars);
    Q_EMIT countInBarsChanged();
}

QVariantList Session::countInChoices() const {
    QVariantList choices;
    for (int bars : kCountInBars) {
        const QString label = bars == 0 ? QStringLiteral("No Count-In")
                                        : QStringLiteral("Count-In %1 Bar%2").arg(bars).arg(bars > 1 ? QStringLiteral("s") : QString());
        choices.append(QVariantMap{{QStringLiteral("label"), label}, {QStringLiteral("value"), bars}});
    }
    return choices;
}

double Session::countInBeats() const {
    const TimeSignature& ts = project_->timeSignature();
    return countInBars() * ts.numerator * 4.0 / ts.denominator;
}

}  // namespace sub::app
