// Notes' bends (MIDI 2.0's per-note pitch bend) in the application layer: the
// model's bend edits and how note edits carry bends along, the rules agreeing
// with the engine's, bends in project files, recorded bends becoming points,
// and bent notes reaching the engine through the session.

#include "EditorFixture.h"
#include "SessionFixture.h"
#include "TestSupport.h"

#include "io/Serialization.h"
#include "model/Errors.h"
#include "model/Notes.h"
#include "model/Project.h"

#include "NoteBend.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

#include <cmath>

using namespace sub::app;
using test::EditorFixture;
using test::SessionFixture;

namespace {

constexpr double kTempo = 120.0;

Note note(int pitch, double start, double length, std::vector<BendPoint> bend = {}, std::vector<Vibrato> vibrato = {}) {
    Note n{pitch, start, length, 100};
    n.bend = std::move(bend);
    n.vibrato = std::move(vibrato);
    return n;
}

// An array holding `inner`. (Not QJsonArray{inner}: MSVC takes a braced array
// of one array as a copy of it, not an array holding it.)
QJsonArray holding(const QJsonArray& inner) { return QJsonArray{QJsonValue(inner)}; }

std::vector<double> times(const Note& n) {
    std::vector<double> out;
    for (const BendPoint& p : n.bend) out.push_back(test::round6(p.time));
    return out;
}

// The frequency of a steady tone, from its zero crossings (rising) over `samples`.
double frequency(const std::vector<float>& interleaved, int64_t from, int64_t frames) {
    int crossings = 0;
    double first = -1.0, last = -1.0;
    for (int64_t i = from + 1; i < from + frames; ++i) {
        const float a = interleaved[static_cast<size_t>(2 * (i - 1))], b = interleaved[static_cast<size_t>(2 * i)];
        if (a < 0.f && b >= 0.f) {
            const double at = static_cast<double>(i - 1) + a / (a - b);
            if (first < 0.0) first = at;
            last = at;
            ++crossings;
        }
    }
    return crossings > 1 ? (crossings - 1) * test::kSampleRate / (last - first) : 0.0;
}

}  // namespace

class TestNoteBends : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    // --- The model -------------------------------------------------------------------------

    void bendPointsStayInOrder() {
        int at = -1;
        Note n = notes::withBendPoint(note(60, 0.0, 2.0), {1.0, 2.0, 0.0}, &at);
        QCOMPARE(at, 0);
        n = notes::withBendPoint(n, {0.5, -1.0, 0.0}, &at);
        QCOMPARE(at, 0);
        n = notes::withBendPoint(n, {1.0, 5.0, 2.0}, &at);  // after the one at its time; its curve held to 1
        QCOMPARE(at, 2);
        QCOMPARE(times(n), (std::vector<double>{0.5, 1.0, 1.0}));
        QCOMPARE(n.bend[2].curve, 1.0);
        QCOMPARE(notes::withBendPoint(n, {1.5, 100.0, 0.0}).bend.back().semitones, notes::kMaxBendSemitones);
        QVERIFY(n.bent());
        QCOMPARE(notes::withoutBendPoints(n, {0, 2}).bend, (std::vector<BendPoint>{{1.0, 2.0, 0.0}}));
        QCOMPARE(notes::withBendCurve(n, 1, -0.5).bend[1].curve, -0.5);
    }

    void movedPointsStayBetweenTheirNeighboursAndInTheNote() {
        const Note n = note(60, 0.0, 2.0, {{0.25, 0.0, 0.0}, {0.5, 1.0, 0.0}, {1.0, 2.0, 0.0}, {1.5, 0.0, 0.0}});
        // One point can't pass the next one.
        QCOMPARE(times(notes::withBendPointsMoved(n, {1}, 0.75, 0.0)), (std::vector<double>{0.25, 1.0, 1.0, 1.5}));
        // Two together move as one, as far as the first one can go.
        const Note both = notes::withBendPointsMoved(n, {1, 2}, -1.0, 3.0);
        QCOMPARE(times(both), (std::vector<double>{0.25, 0.25, 0.75, 1.5}));
        QCOMPARE(both.bend[2].semitones, 5.0);
        // Not past the note's end, nor before its start.
        QCOMPARE(times(notes::withBendPointsMoved(n, {3}, 9.0, 0.0)).back(), 2.0);
        QCOMPARE(times(notes::withBendPointsMoved(n, {0}, -9.0, 0.0)).front(), 0.0);
        QCOMPARE(notes::withBendPointsMoved(n, {2}, 0.0, 100.0).bend[2].semitones, notes::kMaxBendSemitones);
    }

    void aVibratoTakesTheStretchItCovers() {
        Note n = notes::withVibrato(note(60, 0.0, 4.0), {0.5, 3.0, 0.5, 5.5, 0.3});
        n = notes::withVibrato(n, {1.0, 1.0, 1.0, 6.0, 0.0});  // in the middle: splits it
        QCOMPARE(n.vibrato.size(), size_t{3});
        QCOMPARE(n.vibrato[0].start, 0.5);
        QCOMPARE(n.vibrato[0].length, 0.5);
        QCOMPARE(n.vibrato[1].depth, 1.0);
        QCOMPARE(n.vibrato[2].start, 2.0);
        QCOMPARE(n.vibrato[2].length, 1.5);
        // Held to the note.
        const Note held = notes::withVibrato(note(60, 0.0, 1.0), {0.5, 3.0, 0.5, 5.5, 0.3});
        QCOMPARE(held.vibrato[0].length, 0.5);
        QCOMPARE(notes::withoutVibrato(n, 1).vibrato.size(), size_t{2});
        // Covered whole: it goes.
        QCOMPARE(notes::withVibrato(n, {0.0, 4.0, 0.25, 5.0, 0.3}).vibrato.size(), size_t{1});
    }

    void aSlideGoesToTheNextNote() {
        const Note b = note(71, 0.0, 2.0);
        Note muted = note(70, 1.0, 1.0);
        muted.muted = true;
        // The next to start after it (not with it); of a chord, the nearest in pitch.
        const std::vector<Note> notes{b, note(60, 0.0, 2.0), muted, note(64, 2.0, 1.0), note(68, 2.0, 2.0), note(72, 3.0, 1.0)};
        QCOMPARE(notes::nextNote(notes, b)->pitch, 68);
        QCOMPARE(notes::nextNote({b, note(69, 2.0, 1.0), note(73, 2.0, 1.0)}, b)->pitch, 69);  // (as near: the lower)
        QVERIFY(!notes::nextNote({b, note(60, 0.0, 1.0)}, b));  // nothing after it
        QVERIFY(!notes::nextNote({b, muted}, b));                // (deactivated)
        // A slide over 1..2: from where its curve is to the interval, bent; points it covers go.
        Note bent = note(71, 0.0, 2.0, {{0.5, 1.0, 0.0}, {1.25, 3.0, 0.0}, {1.75, 0.0, 0.0}});
        bent = notes::withSlide(bent, 1.0, 2.0, -3.0, 0.4);
        QCOMPARE(times(bent), (std::vector<double>{0.5, 1.0, 2.0}));
        QCOMPARE(bent.bend[1].semitones, 1.0 + 2.0 * (0.5 / 0.75));  // (where the curve was at 1: no step)
        QCOMPARE(bent.bend[1].curve, 0.4);
        QCOMPARE(bent.bend[2].semitones, -3.0);
        QCOMPARE(bent.bendAt(2.0, 120.0), -3.0);
        // Points after it stay; held to the note.
        const Note later = notes::withSlide(note(71, 0.0, 2.0, {{1.9, 2.0, 0.0}}), 0.5, 1.5, 5.0, 0.0);
        QCOMPARE(times(later), (std::vector<double>{0.5, 1.5, 1.9}));
        QCOMPARE(times(notes::withSlide(note(71, 0.0, 2.0), 1.0, 9.0, 2.0, 0.0)), (std::vector<double>{1.0, 2.0}));
    }

    void noteEditsCarryTheirBends() {
        const Note bent = note(60, 1.0, 2.0, {{0.5, 2.0, 0.0}}, {{1.0, 1.0, 0.5, 5.5, 0.3}});
        // Moved: it goes along.
        QCOMPARE(notes::shifted({bent}, 1.0, 2)[0].bend, bent.bend);
        // Its start trimmed: it stays where it was in time.
        const Note trimmed = notes::resized({bent}, notes::Edge::Start, 0.25)[0];
        QCOMPARE(trimmed.start, 1.25);
        QCOMPARE(trimmed.bend[0].time, 0.25);
        QCOMPARE(trimmed.vibrato[0].start, 0.75);
        const Note lengthened = notes::resized({bent}, notes::Edge::Start, -0.5)[0];
        QCOMPARE(lengthened.bend[0].time, 1.0);
        // Cut by a note placed over its start.
        const std::vector<Note> placed = notes::place({bent}, {}, {note(60, 0.0, 1.5)});
        QCOMPARE(placed.size(), size_t{2});
        QCOMPARE(placed[1].start, 1.5);
        QCOMPARE(placed[1].bend.size(), size_t{1});
        QCOMPARE(placed[1].bend[0].time, 0.0);  // where it was: half a beat in
        // ×2: in time with the note.
        const std::vector<Note> scaled = notes::timeScaled({bent, note(62, 0.0, 1.0)}, 2.0);
        QCOMPARE(scaled.size(), size_t{2});
        const Note doubled = scaled[0].pitch == 60 ? scaled[0] : scaled[1];
        QCOMPARE(doubled.bend.size(), size_t{1});
        QCOMPARE(doubled.bend[0].time, 1.0);
        QCOMPARE(doubled.vibrato[0].start, 2.0);
        QCOMPARE(doubled.vibrato[0].length, 2.0);
        QCOMPARE(doubled.vibrato[0].rate, 5.5);  // (its rate is in time)
        // Two notes alike but for their bends are two notes.
        QCOMPARE(notes::normalize({bent, note(60, 1.0, 2.0)}).size(), size_t{2});
        QCOMPARE(notes::normalize({bent, bent}).size(), size_t{1});
    }

    void theModelBendsAsTheEngineDoes() {
        const Note n = note(60, 0.0, 4.0, {{1.0, 2.0, 0.4}, {3.0, -1.0, 0.0}}, {{2.0, 1.0, 0.5, 4.0, 0.25}});
        const std::vector<sub::BendPoint> points{{1.0, 2.0, 0.4}, {3.0, -1.0, 0.0}};
        // 4 cycles a second, at 120 BPM: 2 a beat.
        const std::vector<sub::VibratoSpan> vibratos{{2.0, 1.0, 0.5, 2.0, 0.25}};
        for (double t = 0.0; t <= 4.0; t += 0.0625) QCOMPARE(n.bendAt(t, kTempo), sub::bend::at(points, vibratos, t));
        QCOMPARE(n.bendAt(0.5, kTempo), 1.0);
        QCOMPARE(n.bendAt(3.5, kTempo), -1.0);
    }

    void recordedBendsAreDrawnWithFewPoints() {
        // A glide up two semitones over a beat, as a controller sends it: a value every few milliseconds.
        std::vector<BendPoint> many;
        for (int i = 0; i <= 100; ++i) many.push_back({i / 100.0, 2.0 * i / 100.0, 0.0});
        for (int i = 1; i <= 50; ++i) many.push_back({1.0 + i / 100.0, 2.0, 0.0});
        const std::vector<BendPoint> few = notes::simplifiedBend(many);
        QCOMPARE(few.size(), size_t{3});
        QCOMPARE(few[1].time, 1.0);
        QCOMPARE(few[1].semitones, 2.0);
    }

    // --- Files ----------------------------------------------------------------------------

    void bendsRoundTripInProjectFiles() {
        test::TempDir dir;
        Note silent = note(64, 2.0, 1.0, {{0.0, -1.0, 0.0}});
        silent.muted = true;
        const Clip clip = Clip::midi(QStringLiteral("m"), QString(), 0.0, 4.0, 0.0,
                                     notes::normalize({note(60, 0.0, 1.0, {{0.5, 2.0, -0.25}}, {{0.25, 0.5, 0.75, 6.0, 0.5}}),
                                                       note(62, 1.0, 1.0), silent}));
        Track keys = test::makeTrack(QStringLiteral("t1"), QStringLiteral("Keys"), kMidiKind);
        keys.clips = {clip};
        Project project;
        ProjectContents contents;
        contents.tracks = {keys};
        project.replaceContents(std::move(contents));
        const QString target = dir.path(QStringLiteral("bends.gilproj"));
        saveProject(project, target);
        QFile file(target);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QJsonObject data = QJsonDocument::fromJson(file.readAll()).object();
        QCOMPARE(data[QStringLiteral("version")].toInt(), 22);
        const QJsonArray saved = data[QStringLiteral("tracks")].toArray()[0].toObject()[QStringLiteral("clips")]
                                     .toArray()[0].toObject()[QStringLiteral("notes")].toArray();
        QCOMPARE(saved[0].toArray().size(), 6);  // (its fifth value written: false)
        QCOMPARE(saved[0].toArray()[4].toBool(), false);
        QCOMPARE(saved[0].toArray()[5].toObject()[QStringLiteral("bend")].toArray(),
                 holding(QJsonArray{0.5, 2.0, -0.25}));
        QCOMPARE(saved[0].toArray()[5].toObject()[QStringLiteral("vibrato")].toArray(),
                 holding(QJsonArray{0.25, 0.5, 0.75, 6.0, 0.5}));
        QCOMPARE(saved[1].toArray().size(), 4);  // unbent notes as they were
        Project loaded;
        loadProject(loaded, target);
        QVERIFY(loaded.tracks()[0].clips == std::vector<Clip>{clip});
    }

    void damagedBendsAreRefused() {
        test::TempDir dir;
        const QString target = dir.path(QStringLiteral("damaged.gilproj"));
        const auto write = [&](const QJsonArray& noteData) {
            const QJsonObject data{
                {QStringLiteral("format"), kProjectFormat},
                {QStringLiteral("version"), kProjectVersion},
                {QStringLiteral("tracks"),
                 QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("t")},
                                        {QStringLiteral("name"), QStringLiteral("Keys")},
                                        {QStringLiteral("color"), QStringLiteral("#fff")},
                                        {QStringLiteral("kind"), QStringLiteral("midi")},
                                        {QStringLiteral("clips"),
                                         QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("c")},
                                                                {QStringLiteral("start_beat"), 0},
                                                                {QStringLiteral("duration_beats"), 4},
                                                                {QStringLiteral("notes"), holding(noteData)}}}}}}}};
            QFile file(target);
            QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
            file.write(QJsonDocument(data).toJson());
        };
        Project project;
        write(QJsonArray{60, 0, 1, 100, false, 3});
        QVERIFY_THROWS_EXCEPTION(ProjectFileError, loadProject(project, target));
        write(QJsonArray{60, 0, 1, 100, false, QJsonObject{{QStringLiteral("bend"), holding(QJsonArray{0.5, 1})}}});
        QVERIFY_THROWS_EXCEPTION(ProjectFileError, loadProject(project, target));
        // Numbers as text may say "nan" or "inf": not in a bend (the pitch would be).
        write(QJsonArray{60, 0, 1, 100, false,
                         QJsonObject{{QStringLiteral("bend"), holding(QJsonArray{0.5, QStringLiteral("nan"), 0})}}});
        QVERIFY_THROWS_EXCEPTION(ProjectFileError, loadProject(project, target));
        write(QJsonArray{60, 0, 1, 100, false,
                         QJsonObject{{QStringLiteral("vibrato"),
                                      holding(QJsonArray{QStringLiteral("inf"), 1, 0.5, 5.5, 0.3})}}});
        QVERIFY_THROWS_EXCEPTION(ProjectFileError, loadProject(project, target));
        // Points out of order load in order; out of range, held to it.
        write(QJsonArray{60, 0, 1, 100, false,
                         QJsonObject{{QStringLiteral("bend"), QJsonArray{QJsonArray{0.75, 99, 0}, QJsonArray{0.25, 1, 0}}}}});
        loadProject(project, target);
        const Note& loaded = project.tracks()[0].clips[0].notes[0];
        QCOMPARE(times(loaded), (std::vector<double>{0.25, 0.75}));
        QCOMPARE(loaded.bend[1].semitones, notes::kMaxBendSemitones);
    }

    // --- Recording ---------------------------------------------------------------------------

    void recordedBendsBecomeTheNotesBends() {
        EditorFixture f;
        const QString track = f.editor.addMidiTrack();
        RecordedTake take{track, {}, 0.0, 2.0, {}, true};  // two seconds: four beats at 120 BPM
        RecordedTakeNote played{0.5, 1.5, 60, 100, {}};
        for (int i = 0; i <= 50; ++i) played.bend.emplace_back(i * 0.01, 2.0 * i / 50.0);  // up a tone over a beat (half a second)
        played.bend.emplace_back(0.75, 2.0);
        take.notes = {played};
        f.editor.addRecordings({take});
        const Note& n = f.track(track).clips[0].notes[0];
        QCOMPARE(n.start, 1.0);
        QCOMPARE(n.bend.size(), size_t{3});  // its start, the top of the glide, the end
        QCOMPARE(test::round6(n.bend[1].time), 1.0);
        QCOMPARE(n.bend[1].semitones, 2.0);
    }

    // --- Through the session to the engine ---------------------------------------------------

    void bentNotesPlayAtTheirBentPitch() {
        SessionFixture s;
        const QString track = s.editor().addMidiTrack();  // with the Synth (a saw)
        const QString synth = s.project().track(track).devices[0].id;
        s.editor().setDeviceParam(track, synth, QStringLiteral("wave"), 0.0);  // a sine
        s.editor().setDeviceParam(track, synth, QStringLiteral("cutoff"), 20000.0);
        const auto ref = s.editor().addMidiClip(track, 0.0, 4.0);
        QVERIFY(ref);
        s.editor().setClipNotes(*ref, {note(69, 0.0, 4.0, {{0.0, 12.0, 0.0}})}, QStringLiteral("Bend"));
        std::vector<float> out = s.render(test::kSampleRate);
        QVERIFY(std::abs(frequency(out, 4800, 24000) - 880.0) < 2.0);
        s.stack().undo();  // unbent again
        s.editor().setClipNotes(*ref, {note(69, 0.0, 4.0)}, QStringLiteral("Unbend"));
        out = s.render(test::kSampleRate);
        QVERIFY(std::abs(frequency(out, 4800, 24000) - 440.0) < 1.0);
    }
};

QTEST_MAIN(TestNoteBends)
#include "test_note_bends.moc"
