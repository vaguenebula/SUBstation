// The song's harmony on the application's side (Session.harmony): which notes
// it hears (MIDI tracks heard, not drums; what clips play, where they play it),
// following the project as it changes, the key (the project's, or inferred),
// and the setting that shows it in the piano roll.

#include "EditorFixture.h"
#include "TestSupport.h"

#include "intelligence/Harmony.h"

#include <QSettings>
#include <QSignalSpy>
#include <QTest>

using namespace sub::app;
using test::EditorFixture;

namespace {

std::vector<Note> chord(std::initializer_list<int> pitches, double start, double length) {
    std::vector<Note> notes;
    for (const int p : pitches) notes.push_back({p, start, length, 100});
    return notes;
}

std::vector<Note> operator+(std::vector<Note> a, const std::vector<Note>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

QString names(const Harmony& harmony) {
    QStringList list;
    for (const auto& span : harmony.chords())
        list.append(QStringLiteral("%1 [%2, %3)")
                        .arg(QString::fromStdString(span.chord.name()))
                        .arg(span.start)
                        .arg(span.end));
    return list.join(QStringLiteral(" | "));
}

// A MIDI track with a clip of `notes` from `start`, `length` beats long.
QString midiTrack(EditorFixture& f, const QString& name, double start, double length, const std::vector<Note>& notes) {
    const QString track = f.editor.addMidiTrack(-1, name);
    const auto ref = f.editor.addMidiClip(track, start, length);
    f.editor.setClipNotes(*ref, notes, QStringLiteral("Notes"));
    return track;
}

}  // namespace

class TestHarmony : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void hearsWhatTheMidiTracksPlay() {
        EditorFixture f;
        // C then G in a clip at bar 2, its second bar's notes past its end.
        midiTrack(f, QStringLiteral("Keys"), 4, 8, chord({48, 52, 55}, 0, 4) + chord({43, 47, 50}, 4, 4) +
                                                       chord({41, 45, 48}, 8, 4));
        const auto notes = Harmony::songNotes(f.project);
        QCOMPARE(int(notes.size()), 6);
        QCOMPARE(notes.front().start, 4.0);  // on the timeline
        Harmony harmony(&f.project);
        QCOMPARE(names(harmony), QStringLiteral("C [4, 8) | G [8, 12)"));
    }

    void leavesOutDrumsAndWhatIsMuted() {
        EditorFixture f;
        midiTrack(f, QStringLiteral("Chords"), 0, 4, chord({48, 52, 55}, 0, 4));
        midiTrack(f, QStringLiteral("Drum Rack"), 0, 4, chord({36, 38, 42}, 0, 4));
        const QString muted = midiTrack(f, QStringLiteral("Pad"), 0, 4, chord({49, 53, 56}, 0, 4));
        f.editor.setTrackParam(muted, TrackField::Mute, 1);
        const QString inGroup = midiTrack(f, QStringLiteral("Lead"), 0, 4, chord({66, 70}, 0, 4));
        const QString group = f.editor.groupTracks({inGroup});
        f.editor.setTrackParam(group, TrackField::Mute, 1);
        const QString deactivated = midiTrack(f, QStringLiteral("Arp"), 0, 4, chord({61, 64}, 0, 4));
        f.editor.setRangeActive(0, 4, {deactivated}, false);  // (its clip deactivated: silent)
        f.editor.addAudioTrack();  // audio: for later

        const auto notes = Harmony::songNotes(f.project);
        QCOMPARE(int(notes.size()), 3);
        QCOMPARE(notes.front().pitch, 48);

        for (const char* name : {"Drums", "Kick", "snare 2", "Hi-Hat", "HATS", "Perc Loop", "Claps", "Toms_01", "Shaker"})
            QVERIFY2(Harmony::isDrumTrack(QString::fromLatin1(name)), name);
        for (const char* name : {"Keys", "808", "Bass", "Pad", "Lead", "Strings", "Tomorrow", "Snarky"})
            QVERIFY2(!Harmony::isDrumTrack(QString::fromLatin1(name)), name);
    }

    void followsTheProjectOnceItSettles() {
        EditorFixture f;
        Harmony harmony(&f.project);
        QSignalSpy changed(&harmony, &Harmony::changed);
        QCOMPARE(names(harmony), QString());
        const QString track = midiTrack(f, QStringLiteral("Keys"), 0, 4, chord({48, 52, 55}, 0, 4));
        f.editor.renameTrack(track, QStringLiteral("Piano"));
        QCOMPARE(changed.count(), 0);  // not yet: once, when the edits settle
        QTRY_COMPARE(changed.count(), 1);
        QTest::qWait(Harmony::kSettleMs * 2);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(names(harmony), QStringLiteral("C [0, 4)"));

        // What it doesn't hear changes nothing: a fader, the loop, the tempo, an audio track.
        f.editor.setTrackParam(track, TrackField::VolumeDb, -6);
        f.editor.setLoop(true, 0, 8);
        f.editor.setTempo(140);
        f.editor.addAudioTrack();
        QTest::qWait(Harmony::kSettleMs * 2);
        QCOMPARE(changed.count(), 1);

        f.editor.setTrackParam(track, TrackField::Mute, 1);
        QTRY_COMPARE(changed.count(), 2);
        QCOMPARE(names(harmony), QString());
        f.stack.undo();
        QCOMPARE(names(harmony), QStringLiteral("C [0, 4)"));  // (asked before it settles: inferred now)
    }

    void theKeyIsTheProjectsOrInferred() {
        EditorFixture f;
        Harmony harmony(&f.project);
        QVERIFY(!harmony.key());
        QCOMPARE(harmony.keyLabel(), QString());
        QVERIFY(!harmony.keyInferred());

        // A minor: i iv V i.
        midiTrack(f, QStringLiteral("Keys"), 0, 16,
                  chord({45, 57, 60, 64}, 0, 4) + chord({50, 57, 62, 65}, 4, 4) + chord({52, 56, 59, 64}, 8, 4) +
                      chord({45, 57, 60, 64}, 12, 4));
        QCOMPARE(harmony.keyLabel(), QStringLiteral("A Minor"));
        QVERIFY(harmony.keyInferred());
        QCOMPARE(names(harmony), QStringLiteral("Am [0, 4) | Dm [4, 8) | E [8, 12) | Am [12, 16)"));

        f.editor.setKeyByName(QStringLiteral("F#m"));
        QCOMPARE(harmony.keyLabel(), QStringLiteral("F# Minor"));
        QVERIFY(!harmony.keyInferred());
    }

    void chordsChangeOnTheProjectsBars() {
        EditorFixture f;
        Harmony harmony(&f.project);
        // In 3/4 a C chord, then G from the second bar (beat 3): heard where they change.
        midiTrack(f, QStringLiteral("Keys"), 0, 6, chord({48, 52, 55}, 0, 3) + chord({43, 47, 50}, 3, 3));
        f.editor.setTimeSignature(3, 4);
        QCOMPARE(names(harmony), QStringLiteral("C [0, 3) | G [3, 6)"));
    }

    void shownIsASettingOnAtFirst() {
        QSettings().remove(Harmony::kShownKey);
        EditorFixture f;
        Harmony harmony(&f.project);
        QVERIFY(harmony.shown());
        QSignalSpy shown(&harmony, &Harmony::shownChanged);
        harmony.setShown(false);
        harmony.setShown(false);
        QCOMPARE(shown.count(), 1);
        QVERIFY(!Harmony(&f.project).shown());
        harmony.setShown(true);
        QVERIFY(Harmony(&f.project).shown());
    }
};

QTEST_GUILESS_MAIN(TestHarmony)
#include "test_harmony.moc"
