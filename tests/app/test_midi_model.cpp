// MIDI in the model: clips as windows onto notes, the piano roll's note maths,
// and MIDI tracks in files (the parts of tests/test_midi_model.py that need
// neither the editor nor the engine bridge).

#include "TestSupport.h"

#include "io/Serialization.h"
#include "model/Edits.h"
#include "model/Errors.h"
#include "model/Notes.h"
#include "model/Project.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QTest>

#include <cmath>
#include <set>

using namespace sub::app;

namespace {

constexpr double kTempo = 120.0;
constexpr int C = 60, E = 64, G = 67;

Clip midiClip(double start = 0.0, double beats = 4.0, double offset = 0.0, const std::vector<Note>& clipNotes = {},
              const QString& id = QStringLiteral("m")) {
    return Clip::midi(id, QStringLiteral("m"), start, beats, offset, notes::normalize(clipNotes));
}

double round6(double value) { return std::round(value * 1e6) / 1e6; }

struct Played {
    double start;
    double end;
    int pitch;
    friend bool operator==(const Played&, const Played&) = default;
};

std::vector<Played> played(const Clip& clip) {
    std::vector<Played> result;
    for (const PlayedNote& p : clip.playedNotes()) result.push_back({round6(p.start), round6(p.end), p.note.pitch});
    return result;
}

std::vector<Played> joined(std::vector<Played> a, const std::vector<Played>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

Note note(int pitch, double start, double length, int velocity = 100) { return Note{pitch, start, length, velocity}; }

Project* projectWith(std::vector<Track> tracks) {
    auto* project = new Project;
    ProjectContents contents;
    contents.tracks = std::move(tracks);
    project->replaceContents(std::move(contents));
    return project;
}

QJsonObject readJson(const QString& path) {
    QFile file(path);
    file.open(QIODevice::ReadOnly);
    return QJsonDocument::fromJson(file.readAll()).object();
}

void writeJson(const QString& path, const QJsonObject& data) {
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(QJsonDocument(data).toJson());
}

}  // namespace

class TestMidiModel : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { test::prepareApplication(); }

    // --- Clips ---

    void midiClipPlaysNotesStartingInItsWindow() {
        const Clip clip = midiClip(8.0, 2.0, 1.0,
                                   {
                                       note(C, 0.0, 4.0),  // starts before the window: not played (no chasing)
                                       note(E, 1.0, 0.5),  // at the window start: plays at the clip start
                                       note(G, 2.5, 1.0),  // runs past the window: cut at the clip end
                                       note(C, 3.0, 1.0),  // at the window end: not played
                                   });
        QVERIFY((played(clip) == std::vector<Played>{{8.0, 8.5, E}, {9.5, 10.0, G}}));
        QCOMPARE(clip.lengthBeats(kTempo), 2.0);  // beats, whatever the tempo
        QCOMPARE(clip.lengthBeats(60.0), 2.0);
        const auto notesPlayed = clip.playedNotes();
        QCOMPARE(notesPlayed[0].note.velocity, 100);
    }

    void splittingAndTrimmingKeepEveryNote() {
        const Clip clip = midiClip(4.0, 4.0, 0.0, {note(C, 0.0, 1.0), note(E, 2.0, 1.0)});
        const auto parts = edits::splitClip(clip, 6.0, kTempo);
        QVERIFY(parts);
        const auto& [left, right] = *parts;
        QCOMPARE(left.id, QStringLiteral("m"));
        QCOMPARE(left.durationBeats, 2.0);
        QVERIFY((played(left) == std::vector<Played>{{4.0, 5.0, C}}));
        QCOMPARE(right.startBeat, 6.0);
        QCOMPARE(right.offsetBeats, 2.0);
        QVERIFY((played(right) == std::vector<Played>{{6.0, 7.0, E}}));
        QVERIFY(right.notes == left.notes && left.notes == clip.notes);  // hidden, not lost
        // Trimming the right piece's start back out reveals the first note again.
        const Clip restored = edits::trimStart(right, 4.0, kTempo);
        QVERIFY(played(restored) == played(clip));
        QCOMPARE(edits::trimEnd(left, 20.0, kTempo).durationBeats, 16.0);  // no source to run out of
        QVERIFY(!edits::splitClip(clip, 8.0, kTempo));
    }

    void trimmingBeforeTheFirstContentBeatMovesTheNotesAlong() {
        const Clip clip = midiClip(4.0, 2.0, 0.0, {note(C, 0.5, 1.0)});
        const Clip grown = edits::trimStart(clip, 1.0, kTempo);
        QCOMPARE(grown.startBeat, 1.0);
        QCOMPARE(grown.durationBeats, 5.0);
        QCOMPARE(grown.offsetBeats, 0.0);
        QVERIFY(grown.notes == std::vector<Note>{note(C, 3.5, 1.0)});
        QVERIFY(played(grown) == played(clip));
        QVERIFY((played(clip) == std::vector<Played>{{4.5, 5.5, C}}));
        QCOMPARE(edits::trimStart(clip, -3.0, kTempo).startBeat, 0.0);
    }

    void timeSelectionEditsOnMidiClips() {
        const Clip clip = midiClip(0.0, 8.0, 0.0, {note(C, 1.0, 1.0), note(E, 5.0, 1.0)});
        const auto pieces = edits::removeRange({clip}, 2.0, 4.0, kTempo);
        QCOMPARE(pieces.size(), size_t(2));
        QCOMPARE(pieces[0].endBeat(kTempo), 2.0);
        QCOMPARE(pieces[1].startBeat, 4.0);
        QCOMPARE(pieces[1].endBeat(kTempo), 8.0);
        QVERIFY((joined(played(pieces[0]), played(pieces[1])) == std::vector<Played>{{1.0, 2.0, C}, {5.0, 6.0, E}}));
        const auto copies = edits::sliceRange({clip}, 4.0, 6.0, kTempo);
        QCOMPARE(copies.size(), size_t(1));
        QVERIFY(copies[0].id != clip.id);
        QVERIFY((played(copies[0]) == std::vector<Played>{{5.0, 6.0, E}}));
        // New clips win overlaps, as for audio.
        const Clip newer = midiClip(3.0, 2.0, 0.0, {}, QStringLiteral("new"));
        const auto result = edits::resolveOverlaps({clip, newer}, {QStringLiteral("new")}, kTempo);
        QCOMPARE(result.size(), size_t(3));
        QVERIFY(result[0].id == u"m" && result[0].startBeat == 0.0 && result[0].endBeat(kTempo) == 3.0);
        QVERIFY(result[1].id == u"new" && result[1].startBeat == 3.0 && result[1].endBeat(kTempo) == 5.0);
        QVERIFY(result[2].id != u"m" && result[2].startBeat == 5.0 && result[2].endBeat(kTempo) == 8.0);
    }

    // --- Notes ---

    void consolidateJoinsMidiClipsIntoOne() {
        // Two clips with a gap; the first hides a note past its end and cuts one at it.
        const Clip a = Clip::midi(QStringLiteral("a"), QStringLiteral("Bass"), 4.0, 2.0, 1.0,
                                  {note(60, 1.0, 0.5), note(62, 2.5, 2.0), note(64, 3.5, 1.0)});
        const Clip b = Clip::midi(QStringLiteral("b"), QStringLiteral("Lead"), 8.0, 1.0, 0.0, {note(67, 0.0, 0.5)});
        const Clip joinedClip = edits::consolidateMidi({b, a});
        QCOMPARE(joinedClip.id, QStringLiteral("a"));
        QCOMPARE(joinedClip.name, QStringLiteral("Bass"));
        QCOMPARE(joinedClip.startBeat, 4.0);
        QCOMPARE(joinedClip.durationBeats, 5.0);
        QCOMPARE(joinedClip.offsetBeats, 0.0);
        QVERIFY(joinedClip.isMidi());
        QVERIFY((joinedClip.notes == std::vector<Note>{note(60, 0.0, 0.5), note(62, 1.5, 0.5), note(67, 4.0, 0.5)}));
    }

    void noteNamesFollowAbleton() {
        QCOMPARE(notes::noteName(60), QStringLiteral("C3"));
        QCOMPARE(notes::noteName(61), QStringLiteral("C#3"));
        QCOMPARE(notes::noteName(0), QStringLiteral("C-2"));
        QCOMPARE(notes::noteName(127), QStringLiteral("G8"));
        QVERIFY(notes::isBlackKey(61) && !notes::isBlackKey(64));
    }

    void placedNotesShortenOrReplaceNotesOnTheSameKey() {
        const Note longNote = note(C, 0.0, 4.0);
        QVERIFY((notes::place({longNote, note(E, 0.0, 4.0)}, {}, {note(C, 2.0, 1.0)}) ==
                 std::vector<Note>{note(C, 0.0, 2.0), note(E, 0.0, 4.0), note(C, 2.0, 1.0)}));
        QVERIFY((notes::place({note(C, 1.0, 1.0)}, {}, {note(C, 0.0, 4.0)}) == std::vector<Note>{note(C, 0.0, 4.0)}));
        QVERIFY((notes::place({note(C, 1.0, 4.0)}, {}, {note(C, 0.0, 2.0)}) ==
                 std::vector<Note>{note(C, 0.0, 2.0), note(C, 2.0, 3.0)}));
        // Moving a note means removing the old one and placing the new one.
        QVERIFY((notes::place({longNote}, {longNote}, {note(G, 1.0, 4.0)}) == std::vector<Note>{note(G, 1.0, 4.0)}));
    }

    void movesAndResizesStayInRange() {
        const std::vector<Note> group{note(C, 1.0, 1.0), note(120, 2.0, 1.0)};
        QVERIFY((notes::clampMove(group, -5.0, 12) == std::pair<double, int>{-1.0, 7}));
        QVERIFY((notes::shifted(group, -1.0, 7) == std::vector<Note>{note(67, 0.0, 1.0), note(127, 1.0, 1.0)}));
        QVERIFY((notes::resized(group, notes::Edge::End, -2.0, 0.25) ==
                 std::vector<Note>{note(C, 1.0, 0.25), note(120, 2.0, 0.25)}));
        QVERIFY((notes::resized({note(C, 1.0, 1.0)}, notes::Edge::Start, -3.0) == std::vector<Note>{note(C, 0.0, 2.0)}));
        QVERIFY((notes::resized({note(C, 1.0, 1.0)}, notes::Edge::Start, 3.0, 0.25) ==
                 std::vector<Note>{note(C, 1.75, 0.25)}));
        QVERIFY((notes::withVelocity({note(C, 0.0, 1.0, 100), note(E, 0.0, 1.0, 20)}, -30) ==
                 std::vector<Note>{note(C, 0.0, 1.0, 70), note(E, 0.0, 1.0, 1)}));
    }

    void legatoJoinsNotesAndChords() {
        const std::vector<Note> chord{note(C, 0.0, 0.5), note(E, 0.0, 0.25)};
        const std::vector<Note> melody{note(G, 1.5, 0.25), note(C, 3.0, 2.0)};
        std::vector<Note> all = chord;
        all.insert(all.end(), melody.begin(), melody.end());
        const auto result = notes::legato(all, all, 4.0);
        // A chord reaches the next start together; the last note reaches the clip's end
        // (and is shortened to it); legato shortens as well as lengthens.
        QVERIFY((result == std::vector<Note>{note(C, 0.0, 1.5), note(E, 0.0, 1.5), note(G, 1.5, 1.5), note(C, 3.0, 1.0)}));
        // Only the targets count as "next": an unselected note in between is passed over,
        // unless it is on the same key, which a note never runs into.
        const Note between = note(E, 1.0, 0.5);
        QCOMPARE(notes::legato({note(C, 0.0, 0.25), note(G, 2.0, 0.25)}, {between}, 4.0)[0].length, 2.0);
        QCOMPARE(notes::legato({note(E, 0.0, 0.25)}, {between}, 4.0)[0].length, 1.0);
        const Note pastTheEnd = note(C, 5.0, 1.0);
        QVERIFY(notes::legato({pastTheEnd}, {}, 4.0) == std::vector<Note>{pastTheEnd});
        // The last target stops at the next note after it, on any key, not the clip's end.
        const Note after = note(G, 3.0, 0.5);
        QCOMPARE(notes::legato({note(C, 0.0, 0.25), note(E, 1.0, 0.25)}, {after}, 4.0)[1].length, 2.0);
    }

    void timeScaledDoublesAndHalvesFromTheFirstNote() {
        const std::vector<Note> group{note(C, 1.0, 0.5), note(E, 2.0, 1.0)};
        QVERIFY((notes::timeScaled(group, 2.0) == std::vector<Note>{note(C, 1.0, 1.0), note(E, 3.0, 2.0)}));
        QVERIFY((notes::timeScaled(group, 0.5) == std::vector<Note>{note(C, 1.0, 0.25), note(E, 1.5, 0.5)}));
        QVERIFY(notes::timeScaled({}, 2.0).empty());
    }

    void quantizeMovesStartsOntoTheGrid() {
        const std::vector<Note> playedNotes{note(C, 0.1, 0.5), note(E, 0.9, 0.2), note(G, 1.3, 1.0, 90)};
        auto starts = [](std::vector<Note> result) {
            std::sort(result.begin(), result.end(), notes::byTime);
            std::vector<std::pair<double, double>> list;
            for (const Note& n : result) list.emplace_back(round6(n.start), n.length);
            return list;
        };
        using Starts = std::vector<std::pair<double, double>>;
        QVERIFY((starts(notes::quantized(playedNotes, 0.25)) == Starts{{0.0, 0.5}, {1.0, 0.2}, {1.25, 1.0}}));
        QVERIFY((starts(notes::quantized(playedNotes, 0.25, 0.5)) == Starts{{0.05, 0.5}, {0.95, 0.2}, {1.275, 1.0}}));
        QVERIFY(std::abs(notes::quantized({note(C, 0.3, 0.1)}, 1.0 / 3)[0].start - 1.0 / 3) < 1e-9);  // triplets
        // Two notes on a key landing together: the longer one stays.
        QVERIFY((notes::quantized({note(C, 0.9, 0.5), note(C, 1.05, 1.0)}, 1.0) == std::vector<Note>{note(C, 1.0, 1.0)}));
        QCOMPARE(notes::kQuantizeGrids.size(), size_t(6));
    }

    void humanizeNudgesTimingAndVelocityWithinBounds() {
        std::vector<Note> playedNotes;
        for (int i = 0; i < 40; ++i) playedNotes.push_back(note(C + i, i, 0.5, 100));
        QRandomGenerator rng(7);
        const auto result = notes::humanized(playedNotes, rng, 0.5);
        QRandomGenerator again(7);
        QVERIFY(result == notes::humanized(playedNotes, again, 0.5));  // repeatable with the same seed
        QMap<int, Note> byPitch;
        for (const Note& n : result) byPitch.insert(n.pitch, n);
        std::set<double> shifts;
        std::set<int> changes;
        double largestShift = 0.0;
        int largestChange = 0;
        for (const Note& n : playedNotes) {
            const Note& moved = byPitch.value(n.pitch);
            shifts.insert(moved.start - n.start);
            changes.insert(moved.velocity - n.velocity);
            largestShift = std::max(largestShift, std::abs(moved.start - n.start));
            largestChange = std::max(largestChange, std::abs(moved.velocity - n.velocity));
            QCOMPARE(moved.length, n.length);
        }
        QVERIFY(largestShift <= 0.5 * notes::kHumanizeBeats && shifts.size() > 10);
        QVERIFY(largestChange <= 0.5 * notes::kHumanizeVelocity + 0.5 && changes.size() > 3);
        QRandomGenerator still(7);
        QVERIFY(notes::humanized(playedNotes, still, 0.0) == playedNotes);
    }

    void normalizeSortsAndDropsExactDuplicates() {
        QVERIFY((notes::normalize({note(E, 1.0, 1.0), note(C, 1.0, 1.0), note(E, 1.0, 1.0), note(C, 0.0, 2.0)}) ==
                 std::vector<Note>{note(C, 0.0, 2.0), note(C, 1.0, 1.0), note(E, 1.0, 1.0)}));
        QVERIFY((notes::span({note(C, 1.0, 1.0), note(E, 0.5, 4.0)}) == std::pair<double, double>{0.5, 4.5}));
    }

    // --- Files ---

    void midiInputsRoundTrip() {
        test::TempDir dir;
        Track a = test::makeTrack(QStringLiteral("a"), QStringLiteral("A"), kMidiKind);
        a.armed = true;
        a.monitor = QStringLiteral("in");
        Track b = test::makeTrack(QStringLiteral("b"), QStringLiteral("B"), kMidiKind);
        b.midiInput = MidiInput{QStringLiteral("Keys"), 10};
        Track c = test::makeTrack(QStringLiteral("c"), QStringLiteral("C"), kMidiKind);
        c.midiInput.reset();
        std::unique_ptr<Project> project(projectWith({a, b, c}));
        const QString target = dir.path(QStringLiteral("song.gilproj"));
        saveProject(*project, target);
        Project loaded;
        loadProject(loaded, target);
        QCOMPARE(loaded.tracks().size(), size_t(3));
        QVERIFY(loaded.tracks()[0].midiInput == MidiInput{} && loaded.tracks()[0].armed &&
                loaded.tracks()[0].monitor == u"in");
        QVERIFY((loaded.tracks()[1].midiInput == MidiInput{QStringLiteral("Keys"), 10}) && !loaded.tracks()[1].armed &&
                loaded.tracks()[1].monitor == u"auto");
        QVERIFY(!loaded.tracks()[2].midiInput);
        // MIDI tracks saved before there was MIDI input hear every input.
        QJsonObject data = readJson(target);
        QJsonArray tracks = data[QStringLiteral("tracks")].toArray();
        for (qsizetype i = 0; i < tracks.size(); ++i) {
            QJsonObject track = tracks[i].toObject();
            track.remove(QStringLiteral("midi_input"));
            tracks[i] = track;
        }
        data[QStringLiteral("tracks")] = tracks;
        data[QStringLiteral("version")] = 6;
        writeJson(target, data);
        loadProject(loaded, target);
        for (const Track& t : loaded.tracks()) QVERIFY(t.midiInput == MidiInput{});
    }

    void midiTracksRoundTrip() {
        test::TempDir dir;
        const Clip clip = midiClip(2.0, 4.0, 1.0, {note(C, 1.0, 0.5, 90), note(G, 2.25, 1.0)});
        Track keys = test::makeTrack(QStringLiteral("t1"), QStringLiteral("Keys"), kMidiKind);
        keys.clips = {clip};
        std::unique_ptr<Project> project(projectWith({keys}));
        const QString target = dir.path(QStringLiteral("song.gilproj"));
        saveProject(*project, target);
        QJsonObject data = readJson(target);
        QVERIFY(data[QStringLiteral("version")].toInt() >= 2);  // 2 added MIDI tracks
        QJsonObject track = data[QStringLiteral("tracks")].toArray()[0].toObject();
        QCOMPARE(track[QStringLiteral("kind")].toString(), QStringLiteral("midi"));
        const QJsonArray notesData = track[QStringLiteral("clips")].toArray()[0].toObject()[QStringLiteral("notes")].toArray();
        QCOMPARE(notesData, (QJsonArray{QJsonArray{C, 1.0, 0.5, 90}, QJsonArray{G, 2.25, 1.0, 100}}));
        Project loaded;
        loadProject(loaded, target);
        QVERIFY(loaded.tracks()[0].clips == std::vector<Clip>{clip});
        QCOMPARE(projectToJson(loaded, target), projectToJson(*project, target));

        track[QStringLiteral("kind")] = QStringLiteral("video");
        QJsonArray tracks{track};
        data[QStringLiteral("tracks")] = tracks;
        writeJson(target, data);
        Project other;
        QVERIFY_THROWS_EXCEPTION(ProjectFileError, loadProject(other, target));
    }

    void version1ProjectsLoadAsAudioTracks() {
        test::TempDir dir;
        const QString target = dir.path(QStringLiteral("old.gilproj"));
        QFile file(target);
        file.open(QIODevice::WriteOnly);
        file.write("{\"format\": \"gilstudio-project\", \"version\": 1, \"tracks\": ["
                   "{\"id\": \"t\", \"name\": \"Old\", \"color\": \"#fff\", \"clips\": []}]}");
        file.close();
        Project project;
        loadProject(project, target);
        QCOMPARE(project.tracks()[0].kind, kAudioKind);
        QVERIFY(!project.tracks()[0].isMidi());
    }

    void notesInFilesAreCleanedUp() {
        // Notes of no length go; pitch, velocity and start are kept in range; duplicates go.
        QJsonObject data{{QStringLiteral("format"), kProjectFormat},
                         {QStringLiteral("version"), 15},
                         {QStringLiteral("tracks"),
                          QJsonArray{QJsonObject{
                              {QStringLiteral("id"), QStringLiteral("t")},
                              {QStringLiteral("name"), QStringLiteral("Keys")},
                              {QStringLiteral("color"), QStringLiteral("#fff")},
                              {QStringLiteral("kind"), QStringLiteral("midi")},
                              {QStringLiteral("clips"),
                               QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("c")},
                                                      {QStringLiteral("start_beat"), 0},
                                                      {QStringLiteral("duration_beats"), 4},
                                                      {QStringLiteral("notes"),
                                                       QJsonArray{QJsonArray{200, -1, 1, 0}, QJsonArray{60, 0, 0, 100},
                                                                  QJsonArray{200, -1, 1, 0}, QJsonArray{-3, 1.5, 0.5, 300}}}}}}}}}};
        Project project;
        loadInto(project, data);
        const Clip& clip = project.tracks()[0].clips[0];
        QCOMPARE(clip.name, QStringLiteral("MIDI"));
        QVERIFY((clip.notes == std::vector<Note>{note(127, 0.0, 1.0, 1), note(0, 1.5, 0.5, 127)}));
    }
};

QTEST_GUILESS_MAIN(TestMidiModel)
#include "test_midi_model.moc"
