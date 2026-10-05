#include "session/AudioPreferences.h"

#include <QThread>

#include <algorithm>

#include "audio/EngineBridge.h"

namespace sub::app {

namespace {

const QString kAsio = QStringLiteral("ASIO");

QVariant channelsValue(const std::vector<int>& channels) {
    QVariantList list;
    for (int channel : channels) list.append(channel);
    return list;
}

QVariant channelsValue(const QList<int>& channels) {
    QVariantList list;
    for (int channel : channels) list.append(channel);
    return list;
}

std::vector<int> channelsOf(const QVariant& value) {
    std::vector<int> channels;
    for (const QVariant& channel : value.toList()) channels.push_back(channel.toInt());
    return channels;
}

}  // namespace

std::vector<std::pair<QString, std::vector<int>>> outputChoices(const QStringList& names) {
    std::vector<std::pair<QString, std::vector<int>>> choices;
    const int count = static_cast<int>(names.size());
    for (int first = 0; first < count - 1; first += 2) {
        choices.emplace_back(QStringLiteral("%1/%2 · %3, %4")
                                 .arg(QString::number(first + 1), QString::number(first + 2), names[first],
                                      names[first + 1]),
                             std::vector<int>{first, first + 1});
    }
    if (count % 2) {
        const int last = count - 1;
        choices.emplace_back(QStringLiteral("%1 (mono) · %2").arg(QString::number(last + 1), names[last]),
                             std::vector<int>{last});
    }
    return choices;
}

AudioPreferences::AudioPreferences(EngineBridge* bridge, QObject* parent) : QObject(parent), bridge_(bridge) {
    // A driver that restarted, or a device that went away (while the dialog shows).
    connect(bridge_, &EngineBridge::deviceChanged, this, [this] {
        if (following_) refresh();
    });
}

QVariantList AudioPreferences::list(const std::vector<Choice>& choices) {
    QVariantList list;
    for (const Choice& choice : choices) {
        list.append(QVariantMap{{QStringLiteral("label"), choice.label},
                                {QStringLiteral("value"), choice.value},
                                {QStringLiteral("enabled"), choice.enabled},
                                {QStringLiteral("toolTip"), choice.toolTip}});
    }
    return list;
}

int AudioPreferences::select(std::vector<Choice>& choices, const QVariant& value, const QString& defaultLabel) {
    for (size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].value == value) return static_cast<int>(i);
    }
    if (!defaultLabel.isEmpty()) {
        choices.insert(choices.begin(), Choice{defaultLabel, value});
        return 0;
    }
    return choices.empty() ? -1 : 0;
}

bool AudioPreferences::deviceEnabled() const {
    return deviceIndex_ >= 0 && deviceIndex_ < static_cast<int>(devices_.size()) &&
           !devices_[static_cast<size_t>(deviceIndex_)].value.isNull();
}

QString AudioPreferences::bufferToolTip() const {
    return bufferFixed_ ? QStringLiteral("The driver sets its buffer size: change it in Hardware Setup.") : QString();
}

// The saved settings, unless another kind of device runs (the saved one didn't open).
AudioSettings AudioPreferences::currentSettings() const {
    const AudioSettings saved = AudioSettings::load();
    const AudioDeviceStatus status = bridge_->deviceStatus();
    if (status.open && status.backend != saved.driver) {
        AudioSettings running;
        running.driver = status.backend;
        running.deviceName = status.backend == kAsio ? status.name : QString();
        return running;
    }
    if (!EngineBridge::driverTypes().contains(saved.driver)) return AudioSettings();
    return saved;
}

QStringList AudioPreferences::deviceNames(const QString& driver) const {
    QStringList names;
    for (const AudioDeviceEntry& entry : bridge_->listDevices(driver)) names.append(entry.name);
    return names;
}

void AudioPreferences::open() {
    following_ = true;
    settings_ = currentSettings();
    showThreads();
    refresh();
}

void AudioPreferences::close() { following_ = false; }

void AudioPreferences::showThreads() {
    threads_.clear();
    const int defaultThreads = EngineBridge::defaultAudioThreads();
    const int most = std::max({QThread::idealThreadCount(), defaultThreads, 2});
    for (int count = 1; count <= most; ++count) {
        QString label = QString::number(count);
        if (count == 1) label += QStringLiteral(" (off)");
        if (count == defaultThreads) label += QStringLiteral(" (default)");
        threads_.push_back({label, count});
    }
    const int chosen = audioThreads();
    threadIndex_ = select(threads_, chosen > 0 ? chosen : defaultThreads);
}

// Shows the settings, with the choices the open device offers.
void AudioPreferences::refresh(const QString& error) {
    const AudioSettings& s = settings_;
    const bool isAsio = asio();
    const AudioDeviceStatus status = bridge_->deviceStatus();
    const bool running = status.open && status.backend == s.driver;
    const std::optional<AudioDeviceCaps> caps =
        running ? std::optional<AudioDeviceCaps>(bridge_->deviceCapabilities()) : std::nullopt;

    drivers_.clear();
    const QStringList available = EngineBridge::driverTypes();
    for (const QString& driver : audioDrivers()) {
        const bool here = available.contains(driver);
        drivers_.push_back({driver, driver, here, here ? QString() : kNoAsio});
    }
    driverIndex_ = select(drivers_, s.driver);

    devices_.clear();
    if (isAsio) {
        for (const QString& name : deviceNames(kAsio)) devices_.push_back({name, name});
        if (devices_.empty()) devices_.push_back({QStringLiteral("No ASIO driver is installed"), QVariant()});
        deviceIndex_ = select(devices_, running ? status.name : s.deviceName);
    } else {
        devices_.push_back({QStringLiteral("System Default"), QString()});
        for (const AudioDeviceEntry& entry : bridge_->listDevices(s.driver)) {
            devices_.push_back({entry.name + (entry.isDefault ? QStringLiteral("  (default)") : QString()), entry.name});
        }
        deviceIndex_ = select(devices_, s.deviceName);
    }

    outputs_.clear();
    for (const auto& [label, channels] : outputChoices(caps && isAsio ? caps->outputNames : QStringList())) {
        outputs_.push_back({label, channelsValue(channels)});
    }
    outputIndex_ = select(outputs_, running ? channelsValue(status.outputChannels) : channelsValue(s.outputChannels));

    sampleRates_.clear();
    if (caps && !caps->sampleRates.isEmpty()) {
        for (int rate : caps->sampleRates) sampleRates_.push_back({QStringLiteral("%1 Hz").arg(rate), rate});
        sampleRateIndex_ = select(sampleRates_, status.sampleRate);
    } else {
        for (int rate : kSampleRates) {
            sampleRates_.push_back({rate == 0 ? QStringLiteral("Device Default") : QStringLiteral("%1 Hz").arg(rate), rate});
        }
        sampleRateIndex_ = select(sampleRates_, s.sampleRate);
    }

    buffers_.clear();
    if (caps && !caps->bufferSizes.isEmpty()) {
        for (int frames : caps->bufferSizes) buffers_.push_back({QStringLiteral("%1 samples").arg(frames), frames});
        bufferIndex_ = select(buffers_, status.bufferFrames);
    } else {
        for (int frames : kBufferSizes) buffers_.push_back({QStringLiteral("%1 samples").arg(frames), frames});
        bufferIndex_ = select(buffers_, s.bufferFrames, QStringLiteral("Device Default"));
    }

    controlPanelEnabled_ = running && isAsio && caps->hasControlPanel;
    bufferFixed_ = running && isAsio && buffers_.size() == 1;

    QStringList lines;
    if (!error.isEmpty()) lines.append(QStringLiteral("<span style='color:#ff6b5e'>%1</span>").arg(error.toHtmlEscaped()));
    if (status.open) {
        const QString latency =
            status.backend == kAsio
                ? QStringLiteral("input latency %1 ms · output latency %2 ms")
                      .arg(QString::number(status.inputLatencyMs, 'f', 1), QString::number(status.latencyMs, 'f', 1))
                : QStringLiteral("~%1 ms output latency").arg(QString::number(status.latencyMs, 'f', 1));
        lines.append(QStringLiteral("Running: %1 (%2)<br>%3 Hz · %4 samples · %5%6")
                         .arg(status.name.toHtmlEscaped(), status.backend, QString::number(status.sampleRate),
                              QString::number(status.bufferFrames), latency,
                              status.exclusive ? QStringLiteral(" · exclusive") : QString()));
    } else {
        lines.append(QStringLiteral("No audio device is open."));
    }
    status_ = lines.join(QStringLiteral("<br>"));
    Q_EMIT changed();
}

void AudioPreferences::chooseDriver(int index) {
    if (index == driverIndex_ || index < 0 || index >= static_cast<int>(drivers_.size())) return;
    const Choice& choice = drivers_[static_cast<size_t>(index)];
    if (!choice.enabled) return;
    const QString driver = choice.value.toString();
    driverIndex_ = index;
    const QStringList names = deviceNames(driver);
    if (driver == kAsio && names.isEmpty()) {
        AudioSettings none;
        none.driver = driver;
        settings_ = none;
        refresh(QStringLiteral("No ASIO driver is installed."));
        return;
    }
    const QString name =
        names.contains(settings_.deviceName) ? settings_.deviceName : (driver == kAsio ? names.first() : QString());
    // A new driver type starts from its defaults, but keeps the sample rate if it can.
    AudioSettings defaults;
    defaults.driver = driver;
    defaults.deviceName = name;
    if (driver == kAsio) defaults.bufferFrames = 0;
    defaults.sampleRate = settings_.sampleRate;
    openDevice(defaults, true);
}

void AudioPreferences::chooseDevice(int index) {
    if (index == deviceIndex_ || index < 0 || index >= static_cast<int>(devices_.size())) return;
    const QVariant name = devices_[static_cast<size_t>(index)].value;
    deviceIndex_ = index;
    if (name.isNull()) return;
    // Another device: its own channels and buffer size (ASIO), but the same sample rate if it can.
    AudioSettings next = settings_;
    next.deviceName = name.toString();
    if (asio()) next.bufferFrames = 0;
    next.outputChannels.clear();
    next.inputChannels.clear();
    openDevice(next, true);
}

void AudioPreferences::chooseOutputs(int index) {
    if (index == outputIndex_ || index < 0 || index >= static_cast<int>(outputs_.size())) return;
    outputIndex_ = index;
    settingChosen();
}

void AudioPreferences::chooseSampleRate(int index) {
    if (index == sampleRateIndex_ || index < 0 || index >= static_cast<int>(sampleRates_.size())) return;
    sampleRateIndex_ = index;
    settingChosen();
}

void AudioPreferences::chooseBufferSize(int index) {
    if (index == bufferIndex_ || index < 0 || index >= static_cast<int>(buffers_.size())) return;
    bufferIndex_ = index;
    settingChosen();
}

void AudioPreferences::setExclusive(bool exclusive) {
    if (exclusive == settings_.exclusive) return;
    settings_.exclusive = exclusive;
    settingChosen();
}

// A sample rate, buffer size, output pair or exclusive mode chosen: the device
// opens with all of them as shown.
void AudioPreferences::settingChosen() {
    AudioSettings next = settings_;
    const bool isAsio = asio();
    auto value = [](const std::vector<Choice>& choices, int index) {
        return index >= 0 && index < static_cast<int>(choices.size()) ? choices[static_cast<size_t>(index)].value
                                                                       : QVariant();
    };
    next.sampleRate = value(sampleRates_, sampleRateIndex_).toInt();
    next.bufferFrames = value(buffers_, bufferIndex_).toInt();
    next.exclusive = settings_.exclusive && !isAsio;
    const QVariant outputs = value(outputs_, outputIndex_);
    if (isAsio && !outputs.isNull()) next.outputChannels = channelsOf(outputs);
    openDevice(next);
}

// Opens the device with these settings, and saves them if it opens. With
// `fallBack`, a device that doesn't take them opens with its own instead.
void AudioPreferences::openDevice(const AudioSettings& settings, bool fallBack) {
    settings_ = settings;
    QString error = bridge_->openDevice(settings);
    if (!error.isEmpty() && fallBack) {
        AudioSettings own = settings;
        own.sampleRate = 0;
        own.bufferFrames = 0;
        own.outputChannels.clear();
        own.inputChannels.clear();
        if (own != settings && bridge_->openDevice(own).isEmpty()) {
            settings_ = own;
            error.clear();
        }
    }
    if (error.isEmpty()) settings_.save();
    refresh(error);
}

void AudioPreferences::chooseThreads(int index) {
    if (index == threadIndex_ || index < 0 || index >= static_cast<int>(threads_.size())) return;
    threadIndex_ = index;
    bridge_->chooseAudioThreads(threads_[static_cast<size_t>(index)].value.toInt());
    Q_EMIT changed();
}

void AudioPreferences::showControlPanel() {
    if (!bridge_->showDeviceControlPanel()) refresh(QStringLiteral("The driver has no settings dialog of its own."));
}

}  // namespace sub::app
