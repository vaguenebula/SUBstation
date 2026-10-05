#include "TransportState.h"

#include <QVariantMap>

#include <optional>

#include "audio/BridgeTypes.h"
#include "audio/EngineBridge.h"
#include "model/Keys.h"
#include "model/Project.h"
#include "model/Timebase.h"

namespace sub::ui {

TransportState::TransportState(QObject* parent) : QObject(parent) { refresh(); }

void TransportState::setSession(app::Session* session) {
    if (session == session_) return;
    for (const QMetaObject::Connection& connection : connections_) disconnect(connection);
    connections_.clear();
    session_ = session;
    if (session_) {
        app::Project* project = session_->project();
        app::EngineBridge* bridge = session_->bridge();
        connections_ << connect(project, &app::Project::settingsChanged, this, &TransportState::refresh)
                     << connect(project, &app::Project::reset, this, &TransportState::refresh)
                     << connect(bridge, &app::EngineBridge::positionChanged, this, &TransportState::showPosition)
                     << connect(bridge, &app::EngineBridge::metersUpdated, this, &TransportState::meterTick)
                     << connect(bridge, &app::EngineBridge::deviceChanged, this, &TransportState::refreshDevice);
    }
    Q_EMIT sessionChanged();
    refresh();
}

int TransportState::numerator() const {
    return session_ ? session_->project()->timeSignature().numerator : 4;
}

int TransportState::denominator() const {
    return session_ ? session_->project()->timeSignature().denominator : 4;
}

QList<qreal> TransportState::denominators() const {
    QList<qreal> values;
    for (int denominator : app::kValidDenominators) values << denominator;
    return values;
}

QVariantList TransportState::keys() const {
    QVariantList keys{QVariantMap{{QStringLiteral("label"), QStringLiteral("No Key")}, {QStringLiteral("name"), QString()}}};
    for (const app::Key& key : app::allKeys())
        keys << QVariantMap{{QStringLiteral("label"), key.label()}, {QStringLiteral("name"), key.name()}};
    return keys;
}

int TransportState::keyIndex() const {
    if (!session_) return 0;
    const std::optional<app::Key>& key = session_->project()->key();
    if (!key) return 0;
    const std::vector<app::Key> all = app::allKeys();
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i] == *key) return static_cast<int>(i) + 1;
    return 0;
}

QString TransportState::formatPosition(double beat, int numerator, int denominator) {
    const app::BarPosition at = app::splitPosition(beat, app::TimeSignature{numerator, denominator});
    return QStringLiteral("%1. %2. %3").arg(at.bar + 1, 3).arg(at.beat + 1).arg(at.sixteenth + 1);
}

void TransportState::showPosition(double beat) {
    const QString text = formatPosition(beat, numerator(), denominator());
    if (text == positionText_) return;  // (the playhead moves 60 times a second; the text less often)
    positionText_ = text;
    Q_EMIT positionTextChanged();
}

void TransportState::meterTick() {
    cpuTicks_ = (cpuTicks_ + 1) % kCpuEvery;
    if (cpuTicks_ != 0 || !session_) return;
    const QString text =
        QStringLiteral("CPU %1%").arg(QString::number(session_->bridge()->cpuLoad() * 100.0, 'f', 0));
    if (text == cpuText_) return;
    cpuText_ = text;
    Q_EMIT cpuTextChanged();
}

void TransportState::refreshDevice() {
    QString text = QStringLiteral("No audio device");
    QString toolTip = QStringLiteral("No audio output is open. Click to open Preferences.");
    if (session_) {
        const app::AudioDeviceStatus status = session_->bridge()->deviceStatus();
        if (status.open) {
            // (Counted in characters, as Python counts them, not UTF-16 units.)
            const QList<uint> name = status.name.toUcs4();
            const QString shown = name.size() <= kDeviceNameMax
                                      ? status.name
                                      : QString::fromUcs4(reinterpret_cast<const char32_t*>(name.constData()),
                                                          kDeviceNameMax - 1) +
                                            QStringLiteral("…");
            text = QStringLiteral("%1 · %2 kHz").arg(shown, QString::number(status.sampleRate / 1000.0, 'g', 6));
            toolTip = QStringLiteral("%1: %2\n%3 Hz, %4 frames, ~%5 ms output latency%6\nClick to open Preferences.")
                          .arg(status.backend, status.name)
                          .arg(status.sampleRate)
                          .arg(status.bufferFrames)
                          .arg(QString::number(status.latencyMs, 'f', 1),
                               status.exclusive ? QStringLiteral(" (exclusive)") : QString());
        }
    }
    if (text == deviceText_ && toolTip == deviceToolTip_) return;
    deviceText_ = text;
    deviceToolTip_ = toolTip;
    Q_EMIT deviceChanged();
}

void TransportState::refresh() {
    Q_EMIT settingsChanged();
    showPosition(session_ ? session_->bridge()->position() : 0.0);
    refreshDevice();
}

}  // namespace sub::ui
