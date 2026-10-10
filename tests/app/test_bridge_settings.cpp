// The audio and MIDI preferences (AudioSettings and the other QSettings keys),
// where the bridge puts its files (recordings, frozen audio, reversed copies),
// how it names them and makes their folders, the float WAV writer, the EQ's
// curve, the count of calls that may call the bridge back (BusyScope), and what
// the UI reads of the engine through the bridge (device status, the scope, MIDI
// inputs).

#include "BridgeTestSupport.h"
#include "TestSupport.h"

#include "audio/AudioFiles.h"
#include "audio/AudioSettings.h"
#include "audio/BridgePrivate.h"
#include "audio/DisperserResponse.h"
#include "audio/EqResponse.h"
#include "audio/EngineBridge.h"
#include "model/Paths.h"
#include "model/Project.h"

#include "builtin/DisperserDesign.h"
#include "builtin/EqDesign.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>

#include <cmath>

using namespace sub::app;
using sub::app::test::Studio;

class TestBridgeSettings : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }
    void init() { QSettings().clear(); }

    void audioSettingsHaveDefaultsAndRoundTrip() {
        const AudioSettings defaults = AudioSettings::load();
        QCOMPARE(defaults.driver, defaultDriver());
        QCOMPARE(defaults.driver, QString::fromUtf8(sub::kDefaultDriver));
        QCOMPARE(defaults.deviceName, QString());
        QCOMPARE(defaults.sampleRate, 0);
        QCOMPARE(defaults.bufferFrames, 256);
        QVERIFY(!defaults.exclusive);
        QVERIFY(defaults.outputChannels.empty() && defaults.inputChannels.empty());
        QVERIFY(defaults == AudioSettings{});

        AudioSettings asio;
        asio.driver = QStringLiteral("ASIO");
        asio.deviceName = QStringLiteral("SUB Test ASIO");
        asio.sampleRate = 48000;
        asio.bufferFrames = 128;
        asio.outputChannels = {2, 3};
        asio.inputChannels = {0};
        asio.save();
        QVERIFY(AudioSettings::load() == asio);
        const QSettings s;
        QCOMPARE(s.value("audio/output_channels").toString(), QStringLiteral("2,3"));
        QCOMPARE(s.value("audio/exclusive").toString(), QStringLiteral("false"));

        AudioSettings exclusive;
        exclusive.exclusive = true;
        exclusive.bufferFrames = 0;
        exclusive.save();
        QVERIFY(AudioSettings::load() == exclusive);
        QCOMPARE(QSettings().value("audio/exclusive").toString(), QStringLiteral("true"));
    }

    void anUnknownDriverIsTheDefaultOne() {
        QSettings().setValue("audio/driver", "CoreAudio");
        QCOMPARE(AudioSettings::load().driver, defaultDriver());
        QCOMPARE(audioDrivers(), QStringList({defaultDriver(), QStringLiteral("ASIO")}));
        QSettings().setValue("audio/output_channels", "2,x");  // (edited by hand)
        QVERIFY(AudioSettings::load().outputChannels.empty());
    }

    void theAudioThreadsPreference() {
        // Preferences > Audio Threads applies at once and is remembered (the default unless chosen).
        QCOMPARE(audioThreads(), 0);
        setAudioThreads(-3);
        QCOMPARE(audioThreads(), 0);
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const int defaultThreads = EngineBridge::defaultAudioThreads();
        QCOMPARE(defaultThreads, sub::Engine::defaultAudioThreads());
        bridge.applyAudioThreads();
        QCOMPARE(studio.engine.audioThreads(), defaultThreads);
        const int chosen = defaultThreads != 1 ? 1 : 2;
        bridge.chooseAudioThreads(chosen);
        QCOMPARE(studio.engine.audioThreads(), chosen);
        QCOMPARE(audioThreads(), chosen);
        studio.engine.setAudioThreads(3);
        bridge.applyAudioThreads();  // (on start-up)
        QCOMPARE(bridge.audioThreads(), chosen);
        bridge.chooseAudioThreads(defaultThreads);
        QCOMPARE(studio.engine.audioThreads(), defaultThreads);
        QCOMPARE(audioThreads(), 0);  // back to the default: not pinned
    }

    void midiInputsTurnedOff() {
        QVERIFY(disabledMidiInputs().isEmpty());  // every input plays, a new one too
        setMidiInputDisabled(QStringLiteral("Keys"), true);
        setMidiInputDisabled(QStringLiteral("Pads"), true);
        QCOMPARE(disabledMidiInputs(), QSet<QString>({QStringLiteral("Keys"), QStringLiteral("Pads")}));
        setMidiInputDisabled(QStringLiteral("Keys"), false);
        QCOMPARE(disabledMidiInputs(), QSet<QString>({QStringLiteral("Pads")}));
        QSettings().setValue(kMidiDisabledKey, QStringLiteral("Drums"));  // (a one-item list comes back as a string)
        QCOMPARE(disabledMidiInputs(), QSet<QString>({QStringLiteral("Drums")}));
        QSettings().setValue(kMidiDisabledKey, QString());
        QVERIFY(disabledMidiInputs().isEmpty());
    }

    void recordQuantization() {
        QCOMPARE(recordQuantize(), 0.0);
        setRecordQuantize(0.25);
        QCOMPARE(recordQuantize(), 0.25);
        setRecordQuantize(1.0 / 3.0);
        QCOMPARE(recordQuantize(), 1.0 / 3.0);
        setRecordQuantize(0.3);  // not a choice
        QCOMPARE(recordQuantize(), 0.0);
        QCOMPARE(recordQuantizeChoices().size(), size_t{7});
        QCOMPARE(recordQuantizeChoices().front().first, QStringLiteral("No Quantization"));
    }

    void takeNames() {
        test::TempDir dir;
        const QDateTime when(QDate(2026, 10, 1), QTime(12, 30, 5));
        const QString first = takePath(dir.path(), QStringLiteral("Vox: \"lead\""), when);
        QCOMPARE(QFileInfo(first).fileName(), QStringLiteral("Vox_ _lead_ 2026-10-01 123005.wav"));
        QFile(first).open(QIODevice::WriteOnly);
        QCOMPARE(QFileInfo(takePath(dir.path(), QStringLiteral("Vox: \"lead\""), when)).fileName(),
                 QStringLiteral("Vox_ _lead_ 2026-10-01 123005 2.wav"));
        QCOMPARE(QFileInfo(takePath(dir.path(), QStringLiteral(" .. "), when)).fileName(),
                 QStringLiteral("Audio 2026-10-01 123005.wav"));
        QCOMPARE(QFileInfo(takePath(dir.path(), QStringLiteral("A\tB."), when)).fileName(),
                 QStringLiteral("A_B 2026-10-01 123005.wav"));
    }

    void whereFilesGo() {
        Project project;
        const QString recordings = qEnvironmentVariable("SUBSTATION_RECORDINGS");
        QVERIFY(!recordings.isEmpty());
        QCOMPARE(recordingsFolder(project), recordings);
        QCOMPARE(freezeFolder(project), QDir(recordings).filePath("Freeze"));
        QCOMPARE(reversedFolder(project), QDir(recordings).filePath("Reversed"));
        test::TempDir dir;
        project.setPath(dir.path("song.gilproj"));  // saved: beside it
        QCOMPARE(recordingsFolder(project), QDir(dir.path()).filePath("Recordings"));
        QCOMPARE(freezeFolder(project), QDir(dir.path()).filePath("Freeze"));
        QCOMPARE(reversedFolder(project), QDir(dir.path()).filePath("Reversed"));
        const test::ScopedEnv unset("SUBSTATION_RECORDINGS", std::nullopt);
        project.setPath({});
        QVERIFY(recordingsFolder(project).endsWith("SUBstation/Recordings"));
    }

    void reversedCopiesAreNumbered() {
        test::TempDir dir;
        const QString first = reversedPath(dir.path(), QStringLiteral("/samples/ramp.wav"));
        QCOMPARE(QFileInfo(first).fileName(), QStringLiteral("ramp R.wav"));
        QFile(first).open(QIODevice::WriteOnly);
        QCOMPARE(QFileInfo(reversedPath(dir.path(), QStringLiteral("ramp.flac"))).fileName(),
                 QStringLiteral("ramp R 2.wav"));
    }

    void audioFiles() {
        QVERIFY(isAudioFile(QStringLiteral("a.WAV")) && isAudioFile(QStringLiteral("b.flac")));
        QVERIFY(isAudioFile(QStringLiteral("c.mp3")) && isAudioFile(QStringLiteral("d.wave")));
        QVERIFY(!isAudioFile(QStringLiteral("e.gilproj")));
        QCOMPARE(audioExtensions().size(), 4);
        QCOMPARE(pathIdentity(QStringLiteral("/a/b/../c.wav")), pathIdentity(QStringLiteral("/a/c.wav")));
    }

    void foldersAreMadeOrSayWhyNot() {
        test::TempDir dir;
        QVERIFY(!makeFolder(dir.path(QStringLiteral("a/b")), QStringLiteral("the folder")));
        QVERIFY(QFileInfo(dir.path(QStringLiteral("a/b"))).isDir());
        QVERIFY(!makeFolder(dir.path(QStringLiteral("a/b")), QStringLiteral("the folder")));  // (there already)
        QVERIFY(QFile(dir.path(QStringLiteral("file"))).open(QIODevice::WriteOnly));  // a file where a folder would go
        const QString inside = dir.path(QStringLiteral("file/Freeze"));
        QCOMPARE(makeFolder(inside, QStringLiteral("the freeze folder")),
                 std::optional<QString>(QStringLiteral("Could not create the freeze folder ") +
                                        QDir::toNativeSeparators(inside)));
    }

    void busyLastsAsLongAsTheCallWhateverItThrows() {
        int busy = 0;
        try {
            const BusyScope outer(busy);
            const BusyScope inner(busy);
            QCOMPARE(busy, 2);
            throw 1;  // (not a std::exception)
        } catch (int) {
        }
        QCOMPARE(busy, 0);
    }

    void floatWavFilesDecodeAsWritten() {
        test::TempDir dir;
        QCOMPARE(floatWavHeader(2, 10, 48000).size(), 58);
        std::vector<std::vector<float>> channels(2, std::vector<float>(1000));
        for (int i = 0; i < 1000; ++i) {
            channels[0][static_cast<size_t>(i)] = static_cast<float>(i) / 1000.f;
            channels[1][static_cast<size_t>(i)] = -static_cast<float>(i) / 2000.f;
        }
        const QString path = dir.path("float.wav");
        QVERIFY(writeFloatWav(path, channels, 48000));
        QCOMPARE(QFileInfo(path).size(), qint64{58 + 1000 * 2 * 4});
        sub::Engine engine;
        const auto source = engine.loadSource(path.toStdString());
        QCOMPARE(source->frames(), int64_t{1000});
        QCOMPARE(source->channels(), 2u);
        QCOMPARE(source->channelData(0)[500], 0.5f);
        QCOMPARE(source->channelData(1)[500], -0.25f);
    }

    void theEqCurveIsTheEngines() {
        const QList<double> frequencies{20.0, 1000.0, 20000.0, 30000.0};
        const QList<double> curve = eqResponseDb(sub::eq::Bell, 1000.0, 6.0, 1.0, 0, 48000.0, frequencies);
        QCOMPARE(curve.size(), 4);
        QVERIFY(std::abs(curve[1] - 6.0) < 0.05);
        QVERIFY(std::abs(curve[0]) < 0.1);
        const sub::eq::Design design = sub::eq::design(sub::eq::Bell, 1000.0, 6.0, 1.0, 0, 48000.0);
        QCOMPARE(curve[1], sub::eq::responseDb(design, 1000.0, 48000.0));
        // Above Nyquist: Nyquist's.
        QCOMPARE(eqResponseDb(sub::eq::Bell, 1000.0, 6.0, 1.0, 0, 48000.0, {24000.0, 40000.0})[1],
                 eqResponseDb(sub::eq::Bell, 1000.0, 6.0, 1.0, 0, 48000.0, {24000.0})[0]);
    }

    void theDisperserCurveIsTheEngines() {
        const QList<double> frequencies{20.0, 200.0, 1000.0, 5000.0, 30000.0};
        const QList<double> curve = disperserGroupDelayMs(32, 1000.0, 2.0, 48000.0, frequencies);
        QCOMPARE(curve.size(), 5);
        for (qsizetype i = 0; i < frequencies.size(); ++i)
            QCOMPARE(curve[i], sub::disperser::groupDelayMs(32, 1000.0, 2.0, 48000.0, frequencies[i]));
        QVERIFY(curve[2] > curve[1] && curve[2] > curve[3]);  // it peaks where it is tuned
        QVERIFY(std::abs(curve[2] - 32 * 2 * 2.0 / (3.14159265358979 * 1000.0) * 1000.0) < 0.5);  // ~2Q/(pi f) a stage
        // Above Nyquist: Nyquist's. No stages: no delay.
        QCOMPARE(curve[4], disperserGroupDelayMs(32, 1000.0, 2.0, 48000.0, {24000.0})[0]);
        QCOMPARE(disperserGroupDelayMs(0, 1000.0, 2.0, 48000.0, {1000.0})[0], 0.0);
        // Its peak: its poles' frequency, a little below where it is tuned (f sqrt(1 - 1/4Q²)), kept
        // below Nyquist; at 0 Hz for a pinch so low its poles are real.
        QVERIFY(std::abs(disperserPeakFrequency(1000.0, 2.0, 48000.0) / (1000.0 * std::sqrt(1.0 - 1.0 / 16.0)) - 1.0) <
                0.01);
        QVERIFY(std::abs(disperserPeakFrequency(1000.0, 10.0, 48000.0) / 1000.0 - 1.0) < 0.01);
        QVERIFY(std::abs(disperserPeakFrequency(20000.0, 10.0, 44100.0) - 0.45 * 44100.0) < 50.0);
        QCOMPARE(disperserPeakFrequency(1000.0, 0.1, 48000.0), 0.0);
        const double peak = disperserPeakFrequency(20000.0, 10.0, 44100.0);
        const double atPeak = disperserGroupDelayMs(64, 20000.0, 10.0, 44100.0, {peak})[0];
        for (const double nearby : {19000.0, 19500.0, 20000.0, 20500.0})
            QVERIFY(atPeak >= disperserGroupDelayMs(64, 20000.0, 10.0, 44100.0, {nearby})[0]);
    }

    void startingAudioFallsBackOrSaysSo() {
        // The saved device is gone: the device's own settings, then the system default output.
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        AudioSettings gone;
        gone.deviceName = QStringLiteral("A device that isn't there");
        gone.sampleRate = 22050;
        gone.save();
        QSignalSpy messages(&bridge, &EngineBridge::statusMessage);
        QSignalSpy changed(&bridge, &EngineBridge::deviceChanged);
        const bool opened = bridge.startAudio();
        QCOMPARE(opened, bridge.deviceStatus().open);
        QVERIFY(changed.count() >= 2);  // (each try: the saved settings, the device's own, the system default)
        QVERIFY(!messages.isEmpty());
        const QString said = messages.last().first().toString();
        if (opened) {
            QVERIFY2(said.endsWith("Using the system default output instead."), qPrintable(said));
        } else {
            QVERIFY2(said.startsWith("Audio is off: ") && said.endsWith("Choose a device in Options > Preferences."),
                     qPrintable(said));
        }
        bridge.closeDevice();
        QVERIFY(!bridge.deviceStatus().open);
    }

    void theDeviceWithoutOne() {
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        QSignalSpy messages(&bridge, &EngineBridge::statusMessage);
        QSignalSpy changed(&bridge, &EngineBridge::deviceChanged);
        // A driver asking to be reset when none was opened: said so.
        const QString error = bridge.resetDevice();
        QVERIFY(!error.isEmpty());
        QCOMPARE(changed.count(), 1);
        QCOMPARE(messages.last().first().toString(), QStringLiteral("The audio device could not restart: ") + error +
                                                         QStringLiteral(". Choose a device in Options > Preferences."));
        QVERIFY(!bridge.showDeviceControlPanel());  // no driver's own settings without one
        bridge.closeDevice();
        QCOMPARE(changed.count(), 2);
        AudioSettings unknown;
        unknown.driver = QStringLiteral("CoreAudio");
        QCOMPARE(bridge.openDevice(unknown), QStringLiteral("Unknown driver type: CoreAudio"));
        QVERIFY(!bridge.deviceStatus().open);
    }

    void whatTheUiReadsOfTheEngine() {
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        // The device, as gadgets QML can read.
        const auto status = bridge.property("deviceStatus").value<AudioDeviceStatus>();
        QVERIFY(!status.open);
        QVERIFY(bridge.inputNames().isEmpty());  // none while no device is open
        QCOMPARE(bridge.property("sampleRate").toDouble(), double{test::kSampleRate});
        QVERIFY(EngineBridge::driverTypes().contains(defaultDriver()));
        QVERIFY(bridge.listDevices(QStringLiteral("Nope")).isEmpty());
        // The oscilloscope's feed: scopeWritten, scopeSamples().
        QCOMPARE(bridge.property("scopeWritten").toULongLong(), quint64{0});  // (it stands still without a device)
        QList<float> samples;
        QVERIFY(QMetaObject::invokeMethod(&bridge, "scopeSamples", Qt::DirectConnection,
                                          Q_RETURN_ARG(QList<float>, samples), Q_ARG(int, 64)));
        QVERIFY(samples.size() <= 64);
        QVERIFY(bridge.cpuLoad() >= 0.0);
        // Meters: none yet; the master's after a poll.
        QCOMPARE(bridge.trackMeter(kMaster), MeterLevel{});
        QSignalSpy meters(&bridge, &EngineBridge::metersUpdated);
        BridgeTestAccess::pollMeters(bridge);
        QCOMPARE(meters.count(), 1);
        QVERIFY(bridge.meters().contains(kMaster));
    }

    void midiInputs() {
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        // The computer keyboard is one more MIDI input, always there.
        QCOMPARE(bridge.midiInputChoices().last(), kComputerKeyboard);
        QCOMPARE(bridge.midiInputChoices().size(), bridge.midiInputs().size() + 1);
        bridge.sendMidi({0x90, 60, 100});  // dropped: no device runs
        bridge.sendMidi({0x90, 300, 100});  // not a MIDI message: nothing
        bridge.sendMidi({}, QStringLiteral("Keys"));
        bridge.setMidiInputEnabled(QStringLiteral("Keys"), false);
        QVERIFY(disabledMidiInputs().contains(QStringLiteral("Keys")));
        QVERIFY(!bridge.isMidiInputOpen(QStringLiteral("Keys")));
        bridge.setMidiInputEnabled(QStringLiteral("Keys"), true);
        QVERIFY(disabledMidiInputs().isEmpty());
        for (const QString& name : bridge.midiInputs()) {
            QVERIFY(bridge.isMidiInputOpen(name) || bridge.midiErrors().contains(name));
        }
    }
};

QTEST_GUILESS_MAIN(TestBridgeSettings)
#include "test_bridge_settings.moc"
