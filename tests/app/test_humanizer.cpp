// Humanizing on the application's side (Session.humanizer): the velocity model
// shipped next to the application, each track judged as one part with the notes
// it plays around the notes humanized (where its clips play them, none
// deactivated), each track keeping its own level, and the amount.

#include "EditorFixture.h"
#include "TestSupport.h"

#include "humanize/VelocityModel.h"
#include "intelligence/Humanizer.h"
#include "model/Project.h"

#include <QSignalSpy>
#include <QTest>

#include <numeric>

using namespace sub::app;
using test::EditorFixture;
namespace humanize = sub::intelligence::humanize;

namespace {

// `bars` bars of 4/4 from content beat 0: a chord on every beat, a melody of eighths above.
std::vector<Note> piece(int bars, int velocity = 100) {
    std::vector<Note> notes;
    const int chords[4][3] = {{48, 52, 55}, {48, 53, 57}, {47, 50, 55}, {48, 52, 55}};
    const int melody[8] = {72, 74, 76, 79, 77, 76, 74, 72};
    for (int bar = 0; bar < bars; ++bar) {
        for (int beat = 0; beat < 4; ++beat) {
            for (const int pitch : chords[bar % 4]) notes.push_back({pitch, bar * 4.0 + beat, 0.9, velocity});
        }
        for (int eighth = 0; eighth < 8; ++eighth)
            notes.push_back({melody[(eighth + bar) % 8], bar * 4.0 + eighth * 0.5, 0.45, velocity});
    }
    return notes;
}

ClipRef midiClip(EditorFixture& f, const QString& track, double start, double length, const std::vector<Note>& notes) {
    const auto ref = f.editor.addMidiClip(track, start, length);
    f.editor.setClipNotes(*ref, notes, QStringLiteral("Notes"));
    return *ref;
}

// The clip's notes (as it keeps them), as targets.
std::vector<Humanizer::Target> targetsOf(const EditorFixture& f, const ClipRef& ref) {
    std::vector<Humanizer::Target> targets;
    for (const Note& note : f.project.findClip(ref.trackId, ref.clipId)->notes) targets.push_back({ref.trackId, ref.clipId, note});
    return targets;
}

// What the model makes of `context` (not humanized) and `targets` (humanized), in timeline beats.
std::vector<int> expected(const std::vector<Note>& context, const std::vector<Note>& targets, double amount = 1.0) {
    std::vector<humanize::Note> part;
    for (const Note& n : context) part.push_back({n.pitch, n.start, n.length, n.velocity, false});
    for (const Note& n : targets) part.push_back({n.pitch, n.start, n.length, n.velocity, true});
    const humanize::VelocityModel model(Humanizer::velocityModelPath().toStdString());
    const std::vector<int> all = model.humanize(part, humanize::Meter{}, amount);
    return std::vector<int>(all.begin() + static_cast<std::ptrdiff_t>(context.size()), all.end());
}

std::vector<Note> shifted(std::vector<Note> notes, double beats) {
    for (Note& n : notes) n.start += beats;
    return notes;
}

double mean(const std::vector<int>& values) {
    return std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
}

}  // namespace

class TestHumanizer : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void velocitiesAreTheModelsAtTheNotesOwnLevel() {
        EditorFixture f;
        Humanizer humanizer(&f.project);
        QVERIFY(humanizer.velocityAvailable());  // (the build puts the model next to the tests)
        const QString track = f.editor.addMidiTrack(-1, QStringLiteral("Piano"));
        const ClipRef clip = midiClip(f, track, 8, 8, piece(2));  // (at bar 3)
        const auto targets = targetsOf(f, clip);
        const auto velocities = humanizer.velocities(targets, 1.0);
        QVERIFY(velocities.has_value());
        QCOMPARE(velocities->size(), targets.size());
        std::vector<Note> onTimeline;
        for (const auto& t : targets) onTimeline.push_back(t.note);
        QCOMPARE(*velocities, expected({}, shifted(onTimeline, 8)));
        QVERIFY(std::abs(mean(*velocities) - 100.0) < 0.5);
        double melody = 0, chords = 0;
        for (size_t i = 0; i < targets.size(); ++i) (targets[i].note.pitch >= 72 ? melody : chords) += (*velocities)[i];
        QVERIFY(melody / 16 > chords / 24 + 5);  // (16 melody notes, 24 chord notes)
        // None of the way: as they were.
        QCOMPARE(*humanizer.velocities(targets, 0.0), std::vector<int>(targets.size(), 100));
    }

    void aTrackIsJudgedWithTheNotesItPlaysAroundThem() {
        EditorFixture f;
        Humanizer humanizer(&f.project);
        const QString track = f.editor.addMidiTrack(-1, QStringLiteral("Piano"));
        const std::vector<Note> all = piece(4);
        std::vector<Note> first, second;
        for (const Note& n : all) (n.start < 8 ? first : second).push_back(n);
        midiClip(f, track, 0, 8, first);
        // The second clip's content starts 8 beats in: its notes play where they did.
        const ClipRef later = midiClip(f, track, 8, 8, shifted(second, -8));
        const auto targets = targetsOf(f, later);
        std::vector<Note> played;  // (on the timeline, in the clip's order)
        for (const auto& t : targets) played.push_back(t.note);
        played = shifted(played, 8);
        const auto velocities = humanizer.velocities(targets, 1.0);
        QVERIFY(velocities.has_value());
        QCOMPARE(*velocities, expected(first, played));
        QVERIFY(*velocities != expected({}, played));  // (the first clip's notes mattered)

        // A deactivated note, or a deactivated clip, is no context.
        Note silent{84, 7.0, 1.0, 100};
        silent.muted = true;
        const ClipRef deactivated = midiClip(f, track, 16, 4, piece(1));
        f.editor.setRangeActive(16, 20, {track}, false);
        QVERIFY(f.project.findClip(track, deactivated.clipId)->muted);
        std::vector<Note> withSilent = first;
        withSilent.push_back(silent);
        f.editor.setClipNotes({track, f.project.track(track).clips.front().id}, withSilent, QStringLiteral("Notes"));
        QCOMPARE(*humanizer.velocities(targetsOf(f, later), 1.0), *velocities);
    }

    void eachTrackKeepsItsOwnLevel() {
        EditorFixture f;
        Humanizer humanizer(&f.project);
        const ClipRef loud = midiClip(f, f.editor.addMidiTrack(-1, QStringLiteral("Keys")), 0, 8, piece(2, 90));
        const ClipRef soft = midiClip(f, f.editor.addMidiTrack(-1, QStringLiteral("Piano")), 0, 8, piece(2, 50));
        auto targets = targetsOf(f, loud);
        const auto softTargets = targetsOf(f, soft);
        targets.insert(targets.end(), softTargets.begin(), softTargets.end());
        const auto velocities = humanizer.velocities(targets, 1.0);
        QVERIFY(velocities.has_value());
        const std::vector<int> a(velocities->begin(), velocities->begin() + 40), b(velocities->begin() + 40, velocities->end());
        QVERIFY(std::abs(mean(a) - 90.0) < 0.5);
        QVERIFY(std::abs(mean(b) - 50.0) < 0.5);
        std::vector<Note> softNotes;
        for (const auto& t : softTargets) softNotes.push_back(t.note);
        QCOMPARE(b, expected({}, softNotes));  // (each track alone)

        // A target whose clip is gone keeps its velocity.
        const std::vector<Humanizer::Target> gone{{loud.trackId, QStringLiteral("no such clip"), Note{60, 0, 1, 77}}};
        QCOMPARE(*humanizer.velocities(gone, 1.0), std::vector<int>{77});
        QCOMPARE(*humanizer.velocities({}, 1.0), std::vector<int>());
    }
};

QTEST_GUILESS_MAIN(TestHumanizer)
#include "test_humanizer.moc"
