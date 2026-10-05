#pragma once
// Persistent audio and MIDI preferences (QSettings), per user, under the
// application's organisation and name ("SUBstation"; the tests use "SUBstation
// Tests"):
//
//   audio/driver           the driver type: the default one (sub::kDefaultDriver:
//                          "WASAPI" on Windows) or "ASIO"         AudioSettings
//   audio/device           device or driver name; "": the system default (WASAPI),
//                          the first driver (ASIO)
//   audio/sample_rate      0: the rate the device runs at
//   audio/buffer_frames    0: the device's preferred size; default 256
//   audio/exclusive        WASAPI exclusive mode ("true" / "false")
//   audio/output_channels  "2,3": the pair the master plays on; "": the first two
//   audio/input_channels   ASIO inputs to open; none until something records
//   audio/threads          render threads; 0: the engine's default   audioThreads()
//   midi/disabled_inputs   MIDI inputs turned off                     disabledMidiInputs()
//   record/quantize        record quantization grid in beats; 0: as played

#include <QSet>
#include <QString>
#include <QStringList>

#include <utility>
#include <vector>

namespace sub::app {

// The driver type every build has (sub::kDefaultDriver): "WASAPI" on Windows,
// miniaudio's default backend ("System") elsewhere.
QString defaultDriver();
// The driver types the preferences offer: the default one, and ASIO.
QStringList audioDrivers();
inline const std::vector<int> kBufferSizes{64, 128, 256, 512, 1024, 2048};
inline const std::vector<int> kSampleRates{0, 44100, 48000, 88200, 96000};  // 0 = device default

struct AudioSettings {
    QString driver = defaultDriver();
    QString deviceName;  // empty = the system default (WASAPI), the first driver (ASIO)
    int sampleRate = 0;  // 0 = the rate the device runs at
    int bufferFrames = 256;  // 0 = the device's preferred size
    bool exclusive = false;  // WASAPI
    // Device channels (0-based). The master plays on the first two outputs; {} = the device's first two.
    std::vector<int> outputChannels;
    // Inputs to open (ASIO); none until something records.
    std::vector<int> inputChannels;

    // The saved settings (an unknown driver type: the default one).
    static AudioSettings load();
    void save() const;

    friend bool operator==(const AudioSettings&, const AudioSettings&) = default;
};

inline const QString kAudioThreadsKey = QStringLiteral("audio/threads");
inline const QString kMidiDisabledKey = QStringLiteral("midi/disabled_inputs");
inline const QString kRecordQuantizeKey = QStringLiteral("record/quantize");

// The threads chosen to render audio (the audio thread and its workers); 0:
// the engine's default (one per core but one).
int audioThreads();
void setAudioThreads(int threads);

// MIDI inputs the user turned off. Every other input is used, so a device
// plugged in for the first time just plays.
QSet<QString> disabledMidiInputs();
void setMidiInputDisabled(const QString& name, bool disabled);

// Record quantization choices: (label, grid in beats); 0 = off.
const std::vector<std::pair<QString, double>>& recordQuantizeChoices();
// The grid recorded MIDI notes start on, in beats (0: as played).
double recordQuantize();
void setRecordQuantize(double grid);

}  // namespace sub::app
