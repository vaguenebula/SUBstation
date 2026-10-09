#pragma once
// What the engine bridge hands the UI about the engine, as the application's own
// types (the UI never includes the engine's headers): a file's format, meter
// readings, the audio device's status and what it offers, a processor's
// parameters and displays, and parameter groups for automation. Gadgets, so QML
// can read them as values.

#include "model/ParamSpec.h"

#include <QList>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>

#include <vector>

namespace sub::app {

// What Export Audio writes (EngineBridge::startExport): a WAV file of
// `bitDepth` (16, 24 or 32 float), or an MP3 file at `bitrate` kbps (32 to 320).
struct AudioExportFormat {
    bool mp3 = false;
    int bitDepth = 24;
    int bitrate = 320;
};

// An audio file's length and format, from its header (EngineBridge::fileInfo).
struct AudioFileInfo {
    Q_GADGET
    Q_PROPERTY(qint64 frames MEMBER frames)
    Q_PROPERTY(int channels MEMBER channels)
    Q_PROPERTY(int sampleRate MEMBER sampleRate)
    Q_PROPERTY(double duration MEMBER duration)

public:
    qint64 frames = 0;  // at the file's own sample rate
    int channels = 0;
    int sampleRate = 0;
    double duration = 0.0;  // seconds

    friend bool operator==(const AudioFileInfo&, const AudioFileInfo&) = default;
};

// A meter's peak levels since the last reading (linear, 1 = full scale).
struct MeterLevel {
    Q_GADGET
    Q_PROPERTY(double left MEMBER left)
    Q_PROPERTY(double right MEMBER right)

public:
    double left = 0.0;
    double right = 0.0;

    friend bool operator==(const MeterLevel&, const MeterLevel&) = default;
};

// The audio device running (EngineBridge::deviceStatus).
struct AudioDeviceStatus {
    Q_GADGET
    Q_PROPERTY(bool open MEMBER open)
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString backend MEMBER backend)
    Q_PROPERTY(int sampleRate MEMBER sampleRate)
    Q_PROPERTY(int bufferFrames MEMBER bufferFrames)
    Q_PROPERTY(double latencyMs MEMBER latencyMs)
    Q_PROPERTY(double inputLatencyMs MEMBER inputLatencyMs)
    Q_PROPERTY(QList<int> inputChannels MEMBER inputChannels)
    Q_PROPERTY(QList<int> outputChannels MEMBER outputChannels)
    Q_PROPERTY(bool exclusive MEMBER exclusive)

public:
    bool open = false;
    QString name;
    QString backend;  // the driver type: "WASAPI" (or the default one) or "ASIO"
    int sampleRate = 0;
    int bufferFrames = 0;
    double latencyMs = 0.0;  // output latency
    double inputLatencyMs = 0.0;
    QList<int> inputChannels;   // the device channels open, in the order the engine sees them
    QList<int> outputChannels;  // the master plays on the first two (or mixed to mono on one)
    bool exclusive = false;

    friend bool operator==(const AudioDeviceStatus&, const AudioDeviceStatus&) = default;
};

// What the open device offers (EngineBridge::deviceCapabilities).
struct AudioDeviceCaps {
    Q_GADGET
    Q_PROPERTY(QStringList inputNames MEMBER inputNames)
    Q_PROPERTY(QStringList outputNames MEMBER outputNames)
    Q_PROPERTY(QList<int> sampleRates MEMBER sampleRates)
    Q_PROPERTY(QList<int> bufferSizes MEMBER bufferSizes)
    Q_PROPERTY(int preferredBufferFrames MEMBER preferredBufferFrames)
    Q_PROPERTY(bool hasControlPanel MEMBER hasControlPanel)

public:
    QStringList inputNames;  // all of the device's channels, open or not
    QStringList outputNames;
    QList<int> sampleRates;  // the rates it can run at; empty: any
    QList<int> bufferSizes;  // the sizes it offers; empty: any
    int preferredBufferFrames = 0;
    bool hasControlPanel = false;

    friend bool operator==(const AudioDeviceCaps&, const AudioDeviceCaps&) = default;
};

// A device of a driver type (EngineBridge::listDevices).
struct AudioDeviceEntry {
    Q_GADGET
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(bool isDefault MEMBER isDefault)

public:
    QString name;
    bool isDefault = false;

    friend bool operator==(const AudioDeviceEntry&, const AudioDeviceEntry&) = default;
};

// A processor's parameter, as the engine describes it (its ParamInfo): what a
// device's editor shows. Values are plain (in its own units); automation works
// on normalized ones (0..1), which toNormalized() and fromNormalized() map to
// and from, as the engine does.
struct ProcessorParam {
    Q_GADGET
    Q_PROPERTY(QString id MEMBER id)
    Q_PROPERTY(QString name MEMBER name)
    Q_PROPERTY(QString unit MEMBER unit)
    Q_PROPERTY(double minValue MEMBER minValue)
    Q_PROPERTY(double maxValue MEMBER maxValue)
    Q_PROPERTY(double defaultValue MEMBER defaultValue)
    Q_PROPERTY(bool logScale MEMBER logScale)
    Q_PROPERTY(QStringList valueLabels MEMBER valueLabels)
    Q_PROPERTY(int steps MEMBER steps)
    Q_PROPERTY(bool automatable MEMBER automatable)
    Q_PROPERTY(bool readOnly MEMBER readOnly)
    Q_PROPERTY(bool hidden MEMBER hidden)
    Q_PROPERTY(int stepCount READ stepCount)

public:
    QString id;
    QString name;
    QString unit;
    double minValue = 0.0;
    double maxValue = 1.0;
    double defaultValue = 0.0;
    bool logScale = false;  // knobs move evenly in log(value)
    QStringList valueLabels;  // non-empty: a choice; values 0..n-1 name these
    int steps = 0;  // > 0: only whole values from minValue to minValue + steps
    bool automatable = true;
    bool readOnly = false;  // set by the processor itself (a meter)
    bool hidden = false;  // not for a generic editor (a plug-in's own bypass)

    // Steps between the lowest and highest value of a discrete parameter; 0 if continuous.
    int stepCount() const;
    Q_INVOKABLE double toNormalized(double plain) const;
    Q_INVOKABLE double fromNormalized(double normalized) const;

    friend bool operator==(const ProcessorParam&, const ProcessorParam&) = default;
};

// A stream of values a device's own editor draws besides its parameters (a
// meter's readings, a curve: EngineBridge::readProcessorDisplay).
struct ProcessorDisplay {
    Q_GADGET
    Q_PROPERTY(QString id MEMBER id)
    Q_PROPERTY(int samplesPerValue MEMBER samplesPerValue)

public:
    QString id;  // what the editor asks for it by ("reduction")
    int samplesPerValue = 1;  // the audio each value stands for: 1 for samples, more for a meter

    friend bool operator==(const ProcessorDisplay&, const ProcessorDisplay&) = default;
};

// What an owner (a track or the master) has that can be automated, one group a
// part (EngineBridge::paramGroups): its mixer (id "mixer"), then each device (by id).
struct ParamGroup {
    QString id;
    QString name;
    std::vector<ParamSpec> specs;
};

}  // namespace sub::app

Q_DECLARE_METATYPE(sub::app::AudioFileInfo)
Q_DECLARE_METATYPE(sub::app::MeterLevel)
Q_DECLARE_METATYPE(sub::app::AudioDeviceStatus)
Q_DECLARE_METATYPE(sub::app::AudioDeviceCaps)
Q_DECLARE_METATYPE(sub::app::AudioDeviceEntry)
Q_DECLARE_METATYPE(sub::app::ProcessorParam)
Q_DECLARE_METATYPE(sub::app::ProcessorDisplay)
