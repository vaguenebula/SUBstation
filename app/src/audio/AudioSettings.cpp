#include "audio/AudioSettings.h"

#include "AudioDevice.h"  // sub::kDefaultDriver

#include <QSettings>
#include <QVariant>

#include <algorithm>
#include <cmath>

namespace sub::app {

namespace {

// Channel numbers saved as "2,3" (none if any of them isn't a number).
std::vector<int> channelsFrom(const QVariant& value) {
    std::vector<int> channels;
    for (const QString& part : value.toString().split(u',')) {
        if (part.trimmed().isEmpty()) continue;
        bool ok = false;
        const int channel = part.trimmed().toInt(&ok);
        if (!ok) return {};
        channels.push_back(channel);
    }
    return channels;
}

QString channelsText(const std::vector<int>& channels) {
    QStringList parts;
    for (int channel : channels) parts.append(QString::number(channel));
    return parts.join(u',');
}

int intValue(const QSettings& settings, const QString& key, int fallback) {
    bool ok = false;
    const int value = settings.value(key, fallback).toInt(&ok);
    return ok ? value : fallback;
}

}  // namespace

QString defaultDriver() { return QString::fromUtf8(sub::kDefaultDriver); }

QStringList audioDrivers() { return {defaultDriver(), QStringLiteral("ASIO")}; }

AudioSettings AudioSettings::load() {
    const QSettings s;
    AudioSettings settings;
    const QString driver = s.value(QStringLiteral("audio/driver"), defaultDriver()).toString();
    settings.driver = audioDrivers().contains(driver) ? driver : defaultDriver();
    settings.deviceName = s.value(QStringLiteral("audio/device"), QString()).toString();
    settings.sampleRate = intValue(s, QStringLiteral("audio/sample_rate"), 0);
    settings.bufferFrames = intValue(s, QStringLiteral("audio/buffer_frames"), 256);
    settings.exclusive = s.value(QStringLiteral("audio/exclusive"), QStringLiteral("false")).toString().toLower() ==
                         u"true";
    settings.outputChannels = channelsFrom(s.value(QStringLiteral("audio/output_channels"), QString()));
    settings.inputChannels = channelsFrom(s.value(QStringLiteral("audio/input_channels"), QString()));
    return settings;
}

void AudioSettings::save() const {
    QSettings s;
    s.setValue(QStringLiteral("audio/driver"), driver);
    s.setValue(QStringLiteral("audio/device"), deviceName);
    s.setValue(QStringLiteral("audio/sample_rate"), sampleRate);
    s.setValue(QStringLiteral("audio/buffer_frames"), bufferFrames);
    s.setValue(QStringLiteral("audio/exclusive"), exclusive ? QStringLiteral("true") : QStringLiteral("false"));
    s.setValue(QStringLiteral("audio/output_channels"), channelsText(outputChannels));
    s.setValue(QStringLiteral("audio/input_channels"), channelsText(inputChannels));
}

int audioThreads() {
    bool ok = false;
    const int value = QSettings().value(kAudioThreadsKey, 0).toInt(&ok);
    return ok ? std::max(0, value) : 0;
}

void setAudioThreads(int threads) { QSettings().setValue(kAudioThreadsKey, std::max(0, threads)); }

bool backgroundFreezing() {
    const QByteArray environment = qgetenv("SUBSTATION_BACKGROUND_FREEZE");
    if (!environment.isEmpty()) return environment == "1";
    return backgroundFreezingSetting();
}

bool backgroundFreezingSetting() {
    return QSettings().value(kBackgroundFreezingKey, QStringLiteral("false")).toString().toLower() == u"true";
}

void setBackgroundFreezingSetting(bool enabled) {
    QSettings().setValue(kBackgroundFreezingKey, enabled ? QStringLiteral("true") : QStringLiteral("false"));
}

QSet<QString> disabledMidiInputs() {
    const QVariant value = QSettings().value(kMidiDisabledKey);
    QSet<QString> names;
    // (QSettings gives a one-item list back as a string.)
    for (const QString& name : value.toStringList()) {
        if (!name.isEmpty()) names.insert(name);
    }
    return names;
}

void setMidiInputDisabled(const QString& name, bool disabled) {
    QSet<QString> names = disabledMidiInputs();
    if (disabled) {
        names.insert(name);
    } else {
        names.remove(name);
    }
    QStringList sorted(names.cbegin(), names.cend());
    sorted.sort();
    QSettings().setValue(kMidiDisabledKey, sorted);
}

const std::vector<std::pair<QString, double>>& recordQuantizeChoices() {
    static const std::vector<std::pair<QString, double>> choices{
        {QStringLiteral("No Quantization"), 0.0}, {QStringLiteral("1/4"), 1.0},
        {QStringLiteral("1/8"), 0.5},             {QStringLiteral("1/8 Triplet"), 1.0 / 3.0},
        {QStringLiteral("1/16"), 0.25},           {QStringLiteral("1/16 Triplet"), 1.0 / 6.0},
        {QStringLiteral("1/32"), 0.125}};
    return choices;
}

double recordQuantize() {
    bool ok = false;
    const double value = QSettings().value(kRecordQuantizeKey, 0.0).toDouble(&ok);
    if (!ok) return 0.0;
    const auto& choices = recordQuantizeChoices();
    const bool known = std::any_of(choices.begin(), choices.end(),
                                   [&](const auto& choice) { return std::abs(value - choice.second) < 1e-9; });
    return known ? value : 0.0;
}

void setRecordQuantize(double grid) { QSettings().setValue(kRecordQuantizeKey, grid); }

}  // namespace sub::app
