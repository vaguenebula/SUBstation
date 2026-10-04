// Keys and file names: tempo and key read from a sample's name, transposing
// to the project's key, and what a dropped clip starts with (tests/test_keys.py).

#include "TestSupport.h"

#include "io/Serialization.h"
#include "model/Clip.h"
#include "model/Commands.h"
#include "model/Keys.h"
#include "model/Project.h"

#include <QTest>
#include <QUndoStack>

using namespace sub::app;

Q_DECLARE_METATYPE(std::optional<Key>)
Q_DECLARE_METATYPE(std::optional<double>)

namespace {
const Key kC{0, false};
const Key kAMinor{9, true};
}  // namespace

class TestKeys : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { test::prepareApplication(); }

    void parseFilename_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<std::optional<double>>("bpm");
        QTest::addColumn<std::optional<Key>>("key");
        const std::optional<double> none;
        const std::optional<Key> noKey;
        auto row = [](const QString& name, std::optional<double> bpm, std::optional<Key> key) {
            QTest::newRow(name.toUtf8().constData()) << name << bpm << key;
        };
        row(QStringLiteral("Pack_Bass_Loop_128_Am.wav"), 128.0, kAMinor);
        row(QStringLiteral("Keys 92bpm F# minor.wav"), 92.0, Key{6, true});
        row(QStringLiteral("Vox_Ebmaj_140BPM.wav"), 140.0, Key{3, false});
        row(QStringLiteral("drum_loop_bpm95.wav"), 95.0, noKey);
        row(QStringLiteral("Synth_Lead_Cmaj7_120.wav"), 120.0, kC);
        row(QStringLiteral("808_C#m_150.wav"), 150.0, Key{1, true});
        row(QStringLiteral("bass-g-min-100.wav"), 100.0, Key{7, true});
        row(QStringLiteral("Melody - Key of A - 110.wav"), 110.0, Key{9, false});
        row(QStringLiteral("Pad Bbm.wav"), none, Key{10, true});
        row(QStringLiteral("Loop 2 120 C.wav"), 120.0, kC);
        row(QStringLiteral("A Day in the Life.wav"), none, noKey);  // "A" is a word here, not a key
        row(QStringLiteral("Kick 01.wav"), none, noKey);
        row(QStringLiteral("FX_Riser_08.wav"), none, noKey);
        row(QStringLiteral("Song (Original Mix).mp3"), none, noKey);
        row(QString::fromUtf8("Strings_F♯_minor_90.wav"), 90.0, Key{6, true});  // a sharp sign
        row(QStringLiteral("Bass_Gsharp_maj.wav"), none, Key{8, false});
    }

    void parseFilename() {
        QFETCH(QString, name);
        QFETCH(std::optional<double>, bpm);
        QFETCH(std::optional<Key>, key);
        const FileInfo info = sub::app::parseFilename(name);
        QCOMPARE(info.bpm, bpm);
        QVERIFY(info.key == key);
    }

    void transposeTakesTheShortestWayAndTreatsRelativeKeysAlike() {
        QCOMPARE(transposeTo(kAMinor, kC), 0);  // same notes
        QCOMPARE(transposeTo(Key{2}, kC), -2);
        QCOMPARE(transposeTo(Key{10}, kC), 2);
        QCOMPARE(transposeTo(Key{6}, kC), -6);  // tritone: down
        QCOMPARE(transposeTo(Key{1, true}, kAMinor), -4);
        QCOMPARE(transposeTo(std::nullopt, kC), 0);
        QCOMPARE(transposeTo(kC, std::nullopt), 0);
    }

    void keyNamesRoundTrip() {
        for (const Key& key : allKeys()) QVERIFY(keyFromName(key.name()) == key);
        QCOMPARE(allKeys().size(), size_t(24));
        QVERIFY(allKeys()[1] == (Key{0, true}));  // C, Cm, C#, ...
        QVERIFY(!keyFromName(QString()));
        QVERIFY(!keyFromName(QStringLiteral("H")));
        QCOMPARE(Key(Key{6, true}).name(), QStringLiteral("F#m"));
        QCOMPARE(Key(Key{10, false}).label(), QStringLiteral("Bb Major"));
    }

    void clipSettings() {
        ClipSettings loop = sub::app::clipSettings(QStringLiteral("Loop_128_D.wav"), 2.0, 120.0, kC);
        QCOMPARE(loop.warp, std::optional<bool>(true));
        QCOMPARE(loop.segmentBpm, std::optional<double>(128.0));
        QCOMPARE(loop.transpose, std::optional<int>(-2));
        ClipSettings song = sub::app::clipSettings(QStringLiteral("Song.wav"), 180.0, 124.0, kC);  // long: warped
        QVERIFY((song == ClipSettings{true, 124.0, std::nullopt}));
        QVERIFY(sub::app::clipSettings(QStringLiteral("Kick.wav"), 0.5, 120.0, kC).isEmpty());  // a one-shot plays as it is
        QVERIFY(sub::app::clipSettings(QStringLiteral("Stab_F.wav"), 0.5, 120.0, std::nullopt).isEmpty());

        Clip clip = Clip::audio(QStringLiteral("c"), QStringLiteral("Loop_128_D.wav"), QStringLiteral("Loop"), 0.0, 2.0);
        loop.applyTo(clip);
        QVERIFY(clip.isWarped() && clip.segmentBpm == 128.0 && clip.transpose == -2);
    }

    // (From test_key_is_undoable_and_saved, without the editor: the key goes
    // through an UpdateSettingsCommand and the project file.)
    void keyIsUndoableAndSaved() {
        Project project;
        QUndoStack stack;
        stack.push(new UpdateSettingsCommand(&project, {{SettingsField::Key, std::optional<Key>()}},
                                             {{SettingsField::Key, std::optional<Key>(kAMinor)}},
                                             QStringLiteral("Change Key")));
        QVERIFY(project.key() == kAMinor);
        const QJsonObject data = projectToJson(project);
        QCOMPARE(data.value(QStringLiteral("key")).toString(), QStringLiteral("Am"));
        stack.undo();
        QVERIFY(!project.key());
        loadInto(project, data);
        QVERIFY(project.key() == kAMinor);
    }
};

QTEST_GUILESS_MAIN(TestKeys)
#include "test_keys.moc"
