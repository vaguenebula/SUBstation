// The session's files and settings: saving and opening (the title, the last
// folder), the recent projects (as the old settings had them: casefold
// de-duplication, a one-item list read back as a string, at most ten, missing
// files dropped), Export Audio's choices, the count-in and record
// quantization, recording with nothing armed, the about text, and the
// preferences (audio threads, devices, MIDI inputs).

#include "SessionFixture.h"
#include "TestSupport.h"

#include "AppInfo.h"
#include "audio/AudioSettings.h"
#include "session/AudioPreferences.h"
#include "session/MidiPreferences.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>

using namespace sub::app;
using sub::app::test::SessionFixture;
using sub::app::test::TempDir;

namespace {

QString recent(const QString& path) { return QDir::toNativeSeparators(QFileInfo(path).canonicalFilePath()); }

QStringList labels(const QVariantList& items) {
    QStringList list;
    for (const QVariant& item : items) list.append(item.toMap().value(QStringLiteral("label")).toString());
    return list;
}

QVariantList values(const QVariantList& items) {
    QVariantList list;
    for (const QVariant& item : items) list.append(item.toMap().value(QStringLiteral("value")));
    return list;
}

}  // namespace

class TestSessionFiles : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }
    void init() { QSettings().clear(); }

    void saveAndOpen() {
        SessionFixture f;
        Session& s = f.s();
        TempDir dir;
        f.editor().addAudioTrack();
        f.editor().addMidiTrack();
        f.editor().setTempo(98.0);
        QVERIFY(!s.clean());
        const QString path = dir.path(QStringLiteral("song.gilproj"));
        QSignalSpy titles(&s, &Session::titleChanged);
        QVERIFY(s.saveProjectAs(path));
        QVERIFY(s.clean() && !titles.isEmpty());
        QVERIFY(s.title().startsWith(QStringLiteral("song - ")));
        QCOMPARE(s.title(), QStringLiteral("song - SUBstation"));
        QCOMPARE(f.lastMessage(), QStringLiteral("Saved song.gilproj"));
        QCOMPARE(QDir(s.lastFolder()).canonicalPath(), QDir(dir.path()).canonicalPath());

        s.newProject();
        QVERIFY(f.project().tracks().empty());
        QCOMPARE(s.title(), QStringLiteral("Untitled - SUBstation"));
        QCOMPARE(f.selection().trackId(), QString());
        QVERIFY(f.stack().count() == 0 && s.clean());

        QSignalSpy opened(&s, &Session::projectOpened);
        f.selection().setInsert(5.0);
        QVERIFY(s.openProject(path));
        QCOMPARE(opened.count(), 1);
        QCOMPARE(f.project().tracks().size(), size_t{2});
        QCOMPARE(f.project().tempo(), 98.0);
        QCOMPARE(f.engine.tempo(), 98.0);
        QCOMPARE(f.selection().insertBeat(), 0.0);  // a session of its own
        QVERIFY(s.clean() && f.stack().count() == 0);
        QCOMPARE(f.lastMessage(), QStringLiteral("Opened song.gilproj"));

        // Ctrl+S saves to its file; a project never saved asks where.
        f.editor().setTempo(100.0);
        QVERIFY(s.saveProject());
        QVERIFY(s.clean());
        s.newProject();
        QSignalSpy asked(&s, &Session::saveAsRequested);
        QVERIFY(!s.saveProject());
        QCOMPARE(asked.count(), 1);
        QCOMPARE(QFileInfo(s.suggestedSavePath()).fileName(), QStringLiteral("Untitled.gilproj"));
    }

    void aDisperserIsSavedAndOpenedWithItsAutomation() {
        SessionFixture f;
        Session& s = f.s();
        TempDir dir;
        const QString track = f.editor().addAudioTrack();
        const QString device = f.editor().addDevice(track, QStringLiteral("disperser"));
        QVERIFY(!device.isEmpty());
        const QMap<QString, double> values{{QStringLiteral("amount"), 40.0},
                                           {QStringLiteral("freq"), 250.0},
                                           {QStringLiteral("pinch"), 3.5},
                                           {QStringLiteral("bypass"), 1.0}};
        for (auto it = values.begin(); it != values.end(); ++it) f.editor().setDeviceParam(track, device, it.key(), it.value());
        const QString key = automation::deviceKey(device, QStringLiteral("freq"));
        const Envelope envelope{{0.0, 0.2, 0.0}, {4.0, 0.8, 0.5}};
        f.editor().setEnvelope(track, key, envelope);
        const QString path = dir.path(QStringLiteral("dispersed.gilproj"));
        QVERIFY(s.saveProjectAs(path));

        s.newProject();
        QVERIFY(f.project().tracks().empty());
        QVERIFY(s.openProject(path));
        const Device* opened = f.project().findDevice(track, device);
        QVERIFY(opened);
        QCOMPARE(opened->kind, QStringLiteral("disperser"));
        for (auto it = values.begin(); it != values.end(); ++it) QCOMPARE(opened->params.value(it.key()), it.value());
        const Envelope back = f.project().envelope(track, key);
        QCOMPARE(back.size(), envelope.size());
        for (size_t i = 0; i < back.size(); ++i) {
            QCOMPARE(back[i].beat, envelope[i].beat);
            QCOMPARE(back[i].value, envelope[i].value);
            QCOMPARE(back[i].curve, envelope[i].curve);
        }
        // The engine has it as it was saved.
        const auto id = f.bridge().engineDeviceId(track, device);
        QVERIFY(id);
        const std::vector<sub::ParamInfo> params = f.engine.processorParams(*id);
        for (size_t i = 0; i < params.size(); ++i) {
            const QString paramId = QString::fromStdString(params[i].id);
            QVERIFY2(values.contains(paramId), qPrintable(paramId));
            QCOMPARE(double(f.engine.processorParam(*id, int(i))), values.value(paramId));
        }
    }

    void aFileThatCantBeOpenedIsAWarning() {
        SessionFixture f;
        Session& s = f.s();
        TempDir dir;
        const QString track = f.editor().addAudioTrack();
        const QString path = dir.path(QStringLiteral("broken.gilproj"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("not a project");
        file.close();
        QVERIFY(!s.openProject(path));
        QCOMPARE(f.warnings.size(), 1);
        QVERIFY(f.project().hasTrack(track));  // the open project stays as it was
        QVERIFY(s.recentProjects().isEmpty());
        // Saving where it can't be written is a warning too.
        QVERIFY(!s.saveProjectAs(dir.path(QStringLiteral("no such folder/song.gilproj"))));
        QCOMPARE(f.warnings.size(), 2);
        QVERIFY(f.warnings.back().startsWith(QStringLiteral("Could not save song.gilproj")));
        QVERIFY(!s.clean());
    }

    void openRecent() {
        SessionFixture f;
        Session& s = f.s();
        TempDir dir;
        const QString first = dir.path(QStringLiteral("first.gilproj"));
        const QString second = dir.path(QStringLiteral("second & more.gilproj"));
        QVERIFY(s.saveProjectAs(first));
        QVERIFY(s.saveProjectAs(second));
        QVERIFY(s.saveProjectAs(first));  // back to the top, not listed twice
        QCOMPARE(s.recentProjects(), (QStringList{recent(first), recent(second)}));

        const QVariantList items = s.recentMenuItems();
        QCOMPARE(labels(items), (QStringList{QStringLiteral("&1  first.gilproj"), QStringLiteral("&2  second && more.gilproj")}));
        QCOMPARE(items[1].toMap().value(QStringLiteral("toolTip")).toString(), recent(second));

        const QString chosen = items[1].toMap().value(QStringLiteral("path")).toString();
        QVERIFY(s.recentProjectAvailable(chosen));
        QVERIFY(s.openProject(chosen));
        QCOMPARE(QFileInfo(f.project().path()).canonicalFilePath(), QFileInfo(second).canonicalFilePath());
        QCOMPARE(s.recentProjects().front(), recent(second));

        // A missing file is dropped from the list.
        const QString gone = recent(second);
        QVERIFY(QFile::remove(second));
        QVERIFY(!s.recentProjectAvailable(gone));
        QCOMPARE(f.warnings.back(), QStringLiteral("second & more.gilproj can't be found. It was removed from the list."));
        QCOMPARE(s.recentProjects(), QStringList{recent(first)});
        // However its path is written (here with Qt's separators, the list has the system's).
        QVERIFY(s.saveProjectAs(second));
        QVERIFY(QFile::remove(second));
        QVERIFY(!s.recentProjectAvailable(second));
        QCOMPARE(s.recentProjects(), QStringList{recent(first)});

        s.clearRecentProjects();  // Clear List
        QVERIFY(s.recentMenuItems().isEmpty());
    }

    void recentProjectsAsTheOldSettingsHaveThem() {
        SessionFixture f;
        Session& s = f.s();
        QSettings().setValue(Session::kRecentKey, QStringLiteral("C:/Music/only.gilproj"));  // a one-item list, read back as a string
        QCOMPARE(s.recentProjects(), QStringList{QStringLiteral("C:/Music/only.gilproj")});
        QSettings().setValue(Session::kRecentKey, 5);
        QVERIFY(s.recentProjects().isEmpty());
        // At most ten, the latest first; the same file in another case is the same file.
        TempDir dir;
        QStringList saved;
        for (int i = 0; i < 12; ++i) {
            const QString path = dir.path(QStringLiteral("song %1.gilproj").arg(i));
            QVERIFY(s.saveProjectAs(path));
            saved.prepend(recent(path));
        }
        QCOMPARE(s.recentProjects(), saved.mid(0, Session::kMaxRecent));
        QStringList upper = s.recentProjects();
        upper[0] = upper[0].toUpper();
        QSettings().setValue(Session::kRecentKey, upper);
        QVERIFY(s.saveProjectAs(dir.path(QStringLiteral("song 11.gilproj"))));
        QCOMPARE(s.recentProjects().size(), Session::kMaxRecent);
        QCOMPARE(s.recentProjects().front(), saved.front());
        QCOMPARE(labels(s.recentMenuItems()).back(), QStringLiteral("10  song 2.gilproj"));  // (no mnemonic past 9)
    }

    void exportChoices() {
        SessionFixture f;
        Session& s = f.s();
        QCOMPARE(labels(s.exportRangeChoices()), QStringList{QStringLiteral("Arrangement (start to end of last clip)")});
        f.editor().setLoop(true, 4.0, 8.0);
        QCOMPARE(values(s.exportRangeChoices()), (QVariantList{QStringLiteral("arrangement"), QStringLiteral("loop")}));
        QCOMPARE(labels(s.exportBitDepthChoices()),
                 (QStringList{QStringLiteral("16-bit"), QStringLiteral("24-bit"), QStringLiteral("32-bit float")}));
        QCOMPARE(s.defaultExportBitDepth(), 24);
        QCOMPARE(s.exportProblem(QStringLiteral("arrangement")), QStringLiteral("There is nothing to export yet."));
        QCOMPARE(s.exportProblem(QStringLiteral("loop")), QString());
        TempDir dir;
        QVERIFY(!s.exportAudio(dir.path(QStringLiteral("mix.wav")), QStringLiteral("arrangement"), 24));
        QCOMPARE(f.informations, QStringList{QStringLiteral("There is nothing to export yet.")});
        QCOMPARE(QFileInfo(s.suggestedExportPath()).fileName(), QStringLiteral("Untitled.wav"));
    }

    void countInAndRecordQuantization() {
        SessionFixture f;
        Session& s = f.s();
        QCOMPARE(s.countInBars(), 0);
        QCOMPARE(labels(s.countInChoices()),
                 (QStringList{QStringLiteral("No Count-In"), QStringLiteral("Count-In 1 Bar"), QStringLiteral("Count-In 2 Bars"),
                              QStringLiteral("Count-In 4 Bars")}));
        QSignalSpy changed(&s, &Session::countInBarsChanged);
        s.setCountInBars(1);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(QSettings().value(Session::kCountInKey).toInt(), 1);
        QCOMPARE(s.countInBeats(), 4.0);
        f.editor().setTimeSignature(6, 8);
        QCOMPARE(s.countInBeats(), 3.0);  // a bar of 6/8
        s.setCountInBars(3);  // not a choice
        QCOMPARE(s.countInBars(), 1);
        QSettings().setValue(Session::kCountInKey, QStringLiteral("lots"));
        QCOMPARE(s.countInBars(), 0);

        QCOMPARE(s.recordQuantize(), 0.0);
        QCOMPARE(labels(s.recordQuantizeChoices()).front(), QStringLiteral("No Quantization"));
        s.setRecordQuantize(0.25);
        QCOMPARE(s.recordQuantize(), 0.25);
        QCOMPARE(recordQuantize(), 0.25);
    }

    void nothingToRecord() {
        SessionFixture f;
        Session& s = f.s();
        s.toggleRecord();
        QVERIFY(f.lastMessage().contains(QStringLiteral("Arm a MIDI track")));
        QVERIFY(!f.bridge().isPlaying() && !f.bridge().isRecording());
    }

    void aboutAndClosing() {
        SessionFixture f;
        Session& s = f.s();
        QCOMPARE(s.aboutTitle(), QStringLiteral("About SUBstation"));
        QVERIFY(s.aboutText().startsWith(QStringLiteral("<b>SUBstation</b> ") + version()));
        QVERIFY(s.aboutText().contains(QStringLiteral("VST is a registered trademark")));
        QVERIFY(s.requestClose());  // no render running: the UI goes on
        QCOMPARE(s.confirmDiscardText(), QStringLiteral("Save changes to the current project?"));
        QCOMPARE(s.projectFilter(), QStringLiteral("SUBstation Project (*.gilproj)"));
    }

    // --- Preferences ---

    void audioThreadsPreference() {
        SessionFixture f;
        AudioPreferences& prefs = *f.s().audioPreferences();
        sub::Engine& engine = f.engine;
        const int defaultThreads = sub::Engine::defaultAudioThreads();
        QCOMPARE(audioThreads(), 0);
        QCOMPARE(engine.audioThreads(), defaultThreads);
        prefs.open();
        const auto threadValue = [&](int index) {
            return prefs.threadChoices().at(index).toMap().value(QStringLiteral("value")).toInt();
        };
        const auto threadIndex = [&](int threads) {
            for (int i = 0; i < prefs.threadChoices().size(); ++i) {
                if (threadValue(i) == threads) return i;
            }
            return -1;
        };
        QCOMPARE(threadValue(prefs.threadIndex()), defaultThreads);
        QCOMPARE(labels(prefs.threadChoices()).front(), defaultThreads == 1 ? QStringLiteral("1 (off) (default)")
                                                                            : QStringLiteral("1 (off)"));
        QVERIFY(labels(prefs.threadChoices()).at(prefs.threadIndex()).contains(QStringLiteral("(default)")));
        const int chosen = defaultThreads != 1 ? 1 : 2;
        prefs.chooseThreads(threadIndex(chosen));
        QCOMPARE(engine.audioThreads(), chosen);
        QCOMPARE(audioThreads(), chosen);
        prefs.close();

        engine.setAudioThreads(3);
        f.bridge().applyAudioThreads();  // (on start-up)
        QCOMPARE(engine.audioThreads(), chosen);
        prefs.open();
        QCOMPARE(threadValue(prefs.threadIndex()), chosen);
        prefs.chooseThreads(threadIndex(defaultThreads));
        QCOMPARE(engine.audioThreads(), defaultThreads);
        QCOMPARE(audioThreads(), 0);  // back to the default: not pinned
        prefs.close();
    }

    void audioPreferencesShowTheDevices() {
        SessionFixture f;
        AudioPreferences& prefs = *f.s().audioPreferences();
        prefs.open();
        QVERIFY(prefs.deviceChoices().size() >= 1);  // the system default, at least
        QCOMPARE(labels(prefs.driverChoices()), audioDrivers());
        const QVariantMap asio = prefs.driverChoices().back().toMap();
        QCOMPARE(asio.value(QStringLiteral("enabled")).toBool(), EngineBridge::driverTypes().contains(QStringLiteral("ASIO")));
        QCOMPARE(labels(prefs.driverChoices()).at(prefs.driverIndex()), prefs.settings().driver);
        QVERIFY(!prefs.outputsVisible() && prefs.exclusiveVisible() == (prefs.settings().driver == QStringLiteral("WASAPI")) && !prefs.controlPanelVisible());
        if (!f.bridge().deviceStatus().open) {
            QCOMPARE(prefs.status(), QStringLiteral("No audio device is open."));
            // Without a device, the choices are the preferences' own, the saved ones current.
            QCOMPARE(values(prefs.sampleRateChoices()).at(prefs.sampleRateIndex()).toInt(), prefs.settings().sampleRate);
            QCOMPARE(labels(prefs.sampleRateChoices()).front(), QStringLiteral("Device Default"));
            QCOMPARE(values(prefs.bufferChoices()).at(prefs.bufferIndex()).toInt(), 256);
        }
        prefs.close();
    }

    void aBufferSizeNotOfferedShowsAsDeviceDefault() {
        AudioSettings saved;
        saved.bufferFrames = 0;
        saved.save();
        SessionFixture f;
        AudioPreferences& prefs = *f.s().audioPreferences();
        prefs.open();
        if (f.bridge().deviceStatus().open) QSKIP("a device runs here: its own buffer sizes are listed");
        QCOMPARE(labels(prefs.bufferChoices()).front(), QStringLiteral("Device Default"));
        QCOMPARE(prefs.bufferIndex(), 0);
        prefs.close();
    }

    void outputChoicesArePairs() {
        const auto choices = outputChoices({QStringLiteral("Out 1"), QStringLiteral("Out 2"), QStringLiteral("Out 3")});
        QCOMPARE(choices.size(), size_t{2});
        QCOMPARE(choices[0].first, QStringLiteral("1/2 · Out 1, Out 2"));
        QCOMPARE(choices[0].second, (std::vector<int>{0, 1}));
        QCOMPARE(choices[1].first, QStringLiteral("3 (mono) · Out 3"));
        QCOMPARE(choices[1].second, std::vector<int>{2});
        QVERIFY(outputChoices({}).empty());
    }

    void preferencesListTheMidiInputs() {
        SessionFixture f;
        MidiPreferences& midi = *f.s().midiPreferences();
        midi.open();
        const QStringList names = f.bridge().midiInputs();  // whatever this computer has
        QCOMPARE(midi.inputs().size(), names.size());
        if (names.isEmpty()) QCOMPARE(midi.status(), QStringLiteral("No MIDI input is connected."));
        midi.refresh();
        QCOMPARE(midi.inputs().size(), f.bridge().midiInputs().size());
    }
};

QTEST_GUILESS_MAIN(TestSessionFiles)
#include "test_session_files.moc"
