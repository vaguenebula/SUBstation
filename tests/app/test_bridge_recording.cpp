// Recording through the bridge: what records (armed tracks with an input, not
// frozen), why it can't, the tracks' inputs, monitoring and arming in the
// engine, and with a running device a take recorded, drawn live and handed on as
// RecordedTakes when the recording ends (by stopping, or stopping the transport).
// From tests/test_ui_recording.py and tests/test_midi_input.py (their bridge
// parts). No audio device is needed: the parts that need one running skip
// without (on Linux the default device usually doesn't open).

#include "BridgeTestSupport.h"
#include "TestSupport.h"

#include "audio/AudioFiles.h"

#include <QDir>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTest>

using namespace sub::app;
using sub::app::test::Studio;

class TestBridgeRecording : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void nothingToRecord() {
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        QSignalSpy changed(&bridge, &EngineBridge::recordingChanged);
        QCOMPARE(bridge.startRecording(),
                 QStringLiteral("Arm a MIDI track, or an audio track that has an input, to record."));
        const QString track = studio.edit.addAudioTrack();
        studio.edit.setTrack(track, TrackField::Armed, true);
        QCOMPARE(bridge.startRecording(),
                 QStringLiteral("Arm a MIDI track, or an audio track that has an input, to record."));  // no input
        studio.edit.setInput(track, {0}, std::nullopt);
        if (!bridge.deviceStatus().open) {
            QCOMPARE(bridge.startRecording(),
                     QStringLiteral("No audio device is open. Choose one in Options > Preferences."));
        }
        QVERIFY(!bridge.isRecording() && changed.isEmpty() && !bridge.isPlaying());
        QVERIFY(bridge.stopRecording().empty());
        QVERIFY(changed.isEmpty());
    }

    void whatRecords() {
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString audio = studio.edit.addAudioTrack();
        const QString keys = studio.edit.addMidiTrack();
        const QString silent = studio.edit.addMidiTrack();
        const QString resampling = studio.edit.addAudioTrack();
        const QString group = studio.edit.groupTracks({audio});
        studio.edit.setInput(audio, {0, 1}, std::nullopt);
        studio.edit.setInput(resampling, {}, kMaster);  // the mix
        studio.edit.setTrack(silent, TrackField::MidiInput, std::optional<MidiInput>());
        for (const QString& id : {audio, keys, silent, resampling, group}) studio.edit.setTrack(id, TrackField::Armed, true);
        QCOMPARE(bridge.recordTargets(), (QStringList{audio, keys, resampling}));
        Freeze freeze;
        freeze.path = QStringLiteral("frozen.wav");
        studio.edit.freeze(group, freeze);  // frozen tracks don't record (nor what is in a frozen group)
        QCOMPARE(bridge.recordTargets(), (QStringList{keys, resampling}));
        // The engine has the tracks' inputs, monitoring and arming.
        sub::Engine& engine = studio.engine;
        QCOMPARE(engine.trackInputTrack(*bridge.engineTrackId(resampling)), std::optional<uint32_t>(sub::Engine::kMaster));
        studio.edit.setTrack(keys, TrackField::Monitor, QStringLiteral("in"));
        studio.edit.setTrack(keys, TrackField::MidiInput, std::optional<MidiInput>(MidiInput{kComputerKeyboard, 10}));
        QCOMPARE(bridge.midiInputChoices().last(), kComputerKeyboard);
    }

    void liveTakesGrow() {
        LiveTake take;
        QCOMPARE(take.peakCount(), qint64{0});
        take.addPeaks({-0.25f, 0.5f, -0.1f, 0.2f});
        take.addPeaks({-0.3f, 0.3f});
        QCOMPARE(take.peakCount(), qint64{3});
        QCOMPARE(take.peakMin(2), -0.3f);
        QCOMPARE(take.peakMax(0), 0.5f);
        QCOMPARE(LiveTake::kPeakFrames, sub::Engine::kRecordPeakFrames);
    }

    void aTakeRecordedWithARunningDevice() {
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString error = bridge.openDevice(AudioSettings{});
        if (!error.isEmpty()) QSKIP(qPrintable(QStringLiteral("no audio device runs here: ") + error));
        const QString keys = studio.edit.addMidiTrack(QStringLiteral("Keys"));
        studio.edit.setTrack(keys, TrackField::Armed, true);
        QSignalSpy changed(&bridge, &EngineBridge::recordingChanged);
        QSignalSpy updated(&bridge, &EngineBridge::recordingUpdated);
        QSignalSpy recorded(&bridge, &EngineBridge::takesRecorded);
        QCOMPARE(bridge.startRecording(), QString());
        QVERIFY(bridge.isRecording() && bridge.isPlaying());
        QCOMPARE(changed.count(), 1);
        QVERIFY(bridge.liveTakes().contains(keys) && bridge.liveTakes().value(keys).midi);
        QTest::qWait(200);
        bridge.sendMidi({0x90, 60, 100});  // the computer keyboard
        QTest::qWait(200);
        bridge.sendMidi({0x80, 60, 0});
        QTRY_VERIFY_WITH_TIMEOUT(!updated.isEmpty(), 2000);
        bridge.stop();  // stops playing, and the recording
        QVERIFY(!bridge.isRecording() && !bridge.isPlaying());
        QCOMPARE(changed.count(), 2);
        QVERIFY(bridge.liveTakes().isEmpty());
        QCOMPARE(recorded.count(), 1);
        const auto takes = recorded.first().first().value<std::vector<RecordedTake>>();
        QCOMPARE(takes.size(), size_t{1});
        QCOMPARE(takes.front().trackId, keys);
        QVERIFY(takes.front().midi && takes.front().path.isEmpty() && takes.front().durationSec > 0.0);
        bridge.closeDevice();
    }

    void anAudioTakeGoesToTheRecordingsFolder() {
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString error = bridge.openDevice(AudioSettings{});
        if (!error.isEmpty()) QSKIP(qPrintable(QStringLiteral("no audio device runs here: ") + error));
        const QString track = studio.edit.addAudioTrack(QStringLiteral("Vox"));
        studio.edit.setInput(track, {}, kMaster);  // resampling: needs no input channel
        studio.edit.setTrack(track, TrackField::Armed, true);
        QCOMPARE(bridge.startRecording(), QString());
        QTRY_VERIFY_WITH_TIMEOUT(bridge.liveTakes().value(track).frames > 0, 3000);
        const std::vector<RecordedTake> takes = bridge.stopRecording();
        QVERIFY(bridge.isPlaying());  // (punch out: it keeps playing)
        QCOMPARE(takes.size(), size_t{1});
        QCOMPARE(QFileInfo(takes.front().path).absolutePath(),
                 QFileInfo(recordingsFolder(studio.project)).absoluteFilePath());
        QVERIFY(QFileInfo(takes.front().path).fileName().startsWith("Vox "));
        QVERIFY(QFileInfo::exists(takes.front().path));
        bridge.closeDevice();
    }
};

QTEST_GUILESS_MAIN(TestBridgeRecording)
#include "test_bridge_recording.moc"
