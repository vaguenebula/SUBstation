#pragma once
// Preferences › Audio, without its widgets: the driver type, the device, its
// outputs (ASIO), sample rate, buffer size, exclusive mode (WASAPI), the audio
// threads, the driver's own settings, and a status line. Changes apply at once:
// what a device offers (its sample rates, buffer sizes and channels, its
// driver's own settings) is only known while it is open. Each change opens the
// device with the new settings (another driver or device: falling back to the
// device's own settings if it won't take them), and only settings that opened
// are saved (AudioSettings).
//
// The QML dialog calls open() when it shows and close() when it goes; each
// combo box binds to a list of choices ({label, value[, enabled, toolTip]})
// and its current index, and calls the choose... invokable with the index the
// user picked (the same index again does nothing, as a combo box's
// currentIndexChanged).

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>

#include <utility>
#include <vector>

#include "audio/AudioSettings.h"

namespace sub::app {

class EngineBridge;

// Where the master can play on a device with these outputs: each stereo pair
// ("1/2 · Out 1, Out 2"), and the last output alone if their number is odd
// ("3 (mono) · Out 3"). (label, channels).
std::vector<std::pair<QString, std::vector<int>>> outputChoices(const QStringList& names);

inline const QString kNoAsio = QStringLiteral(
    "This build has no ASIO support: unzip Steinberg's ASIO SDK into the project folder and build again (see the "
    "README).");

class AudioPreferences : public QObject {
    Q_OBJECT
    // [{label, value: driver, enabled, toolTip}]: a driver this build hasn't is disabled.
    Q_PROPERTY(QVariantList driverChoices READ driverChoices NOTIFY changed)
    Q_PROPERTY(int driverIndex READ driverIndex NOTIFY changed)
    // [{label, value: device name ("" the system default; null: none to choose)}]
    Q_PROPERTY(QVariantList deviceChoices READ deviceChoices NOTIFY changed)
    Q_PROPERTY(int deviceIndex READ deviceIndex NOTIFY changed)
    Q_PROPERTY(bool deviceEnabled READ deviceEnabled NOTIFY changed)
    // Hardware Setup: the ASIO driver's own settings (buffer size, clock, routing...).
    Q_PROPERTY(bool controlPanelVisible READ controlPanelVisible NOTIFY changed)
    Q_PROPERTY(bool controlPanelEnabled READ controlPanelEnabled NOTIFY changed)
    // [{label, value: [channels]}]: the outputs the master plays on (ASIO).
    Q_PROPERTY(QVariantList outputChoices READ outputChoiceList NOTIFY changed)
    Q_PROPERTY(int outputIndex READ outputIndex NOTIFY changed)
    Q_PROPERTY(bool outputsVisible READ outputsVisible NOTIFY changed)
    // [{label, value: Hz (0: the device's default)}]
    Q_PROPERTY(QVariantList sampleRateChoices READ sampleRateChoices NOTIFY changed)
    Q_PROPERTY(int sampleRateIndex READ sampleRateIndex NOTIFY changed)
    // [{label, value: frames (0: the device's default)}]
    Q_PROPERTY(QVariantList bufferChoices READ bufferChoices NOTIFY changed)
    Q_PROPERTY(int bufferIndex READ bufferIndex NOTIFY changed)
    Q_PROPERTY(bool bufferEnabled READ bufferEnabled NOTIFY changed)
    Q_PROPERTY(QString bufferToolTip READ bufferToolTip NOTIFY changed)
    // "Exclusive mode (lower latency; other apps are silenced)": WASAPI's.
    Q_PROPERTY(bool exclusive READ exclusive NOTIFY changed)
    Q_PROPERTY(bool exclusiveVisible READ exclusiveVisible NOTIFY changed)
    // [{label ("1 (off)", "8 (default)"), value: threads}]
    Q_PROPERTY(QVariantList threadChoices READ threadChoices NOTIFY changed)
    Q_PROPERTY(int threadIndex READ threadIndex NOTIFY changed)
    // "Background freezing": unchanged strips play from a RAM cache (the saved setting).
    Q_PROPERTY(bool backgroundFreezing READ backgroundFreezing NOTIFY changed)
    // What runs (rich text: lines joined with <br>, an error first in red).
    Q_PROPERTY(QString status READ status NOTIFY changed)

public:
    explicit AudioPreferences(EngineBridge* bridge, QObject* parent = nullptr);

    QVariantList driverChoices() const { return list(drivers_); }
    int driverIndex() const { return driverIndex_; }
    QVariantList deviceChoices() const { return list(devices_); }
    int deviceIndex() const { return deviceIndex_; }
    bool deviceEnabled() const;
    bool controlPanelVisible() const { return asio(); }
    bool controlPanelEnabled() const { return controlPanelEnabled_; }
    QVariantList outputChoiceList() const { return list(outputs_); }
    int outputIndex() const { return outputIndex_; }
    bool outputsVisible() const { return asio(); }
    QVariantList sampleRateChoices() const { return list(sampleRates_); }
    int sampleRateIndex() const { return sampleRateIndex_; }
    QVariantList bufferChoices() const { return list(buffers_); }
    int bufferIndex() const { return bufferIndex_; }
    bool bufferEnabled() const { return !bufferFixed_; }
    QString bufferToolTip() const;
    bool exclusive() const { return settings_.exclusive; }
    // WASAPI's exclusive mode: ASIO, and the System driver off Windows, have none.
    bool exclusiveVisible() const { return settings_.driver == QLatin1String("WASAPI"); }
    QVariantList threadChoices() const { return list(threads_); }
    int threadIndex() const { return threadIndex_; }
    bool backgroundFreezing() const { return backgroundFreezingSetting(); }
    QString status() const { return status_; }

    // The settings shown (as last opened, or asked for).
    const AudioSettings& settings() const { return settings_; }

    // The dialog shows: the saved settings (unless another kind of device runs:
    // the saved one didn't open), and from now on what the device does.
    Q_INVOKABLE void open();
    // The dialog went.
    Q_INVOKABLE void close();
    Q_INVOKABLE void chooseDriver(int index);
    Q_INVOKABLE void chooseDevice(int index);
    Q_INVOKABLE void chooseOutputs(int index);
    Q_INVOKABLE void chooseSampleRate(int index);
    Q_INVOKABLE void chooseBufferSize(int index);
    Q_INVOKABLE void setExclusive(bool exclusive);
    Q_INVOKABLE void chooseThreads(int index);
    Q_INVOKABLE void setBackgroundFreezing(bool enabled);
    // Hardware Setup (it may run a message loop: the UI disables the dialog meanwhile).
    Q_INVOKABLE void showControlPanel();

Q_SIGNALS:
    void changed();

private:
    struct Choice {
        QString label;
        QVariant value;
        bool enabled = true;
        QString toolTip;
    };

    static QVariantList list(const std::vector<Choice>& choices);
    // The index of the choice with this value; one is added (first) for it if
    // there is none and `defaultLabel` is given; else the first (-1: none).
    static int select(std::vector<Choice>& choices, const QVariant& value, const QString& defaultLabel = {});
    bool asio() const { return settings_.driver == QStringLiteral("ASIO"); }
    AudioSettings currentSettings() const;
    QStringList deviceNames(const QString& driver) const;
    void refresh(const QString& error = {});
    void settingChosen();
    void openDevice(const AudioSettings& settings, bool fallBack = false);
    void showThreads();

    EngineBridge* bridge_;
    AudioSettings settings_;
    bool following_ = false;
    std::vector<Choice> drivers_;
    int driverIndex_ = -1;
    std::vector<Choice> devices_;
    int deviceIndex_ = -1;
    std::vector<Choice> outputs_;
    int outputIndex_ = -1;
    std::vector<Choice> sampleRates_;
    int sampleRateIndex_ = -1;
    std::vector<Choice> buffers_;
    int bufferIndex_ = -1;
    bool bufferFixed_ = false;
    bool controlPanelEnabled_ = false;
    std::vector<Choice> threads_;
    int threadIndex_ = -1;
    QString status_;
};

}  // namespace sub::app
