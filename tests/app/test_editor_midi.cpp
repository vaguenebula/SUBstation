// MIDI in the editor: MIDI tracks and their instrument, moving devices, MIDI
// clips and note edits, clips moving only onto tracks of their kind, MIDI
// inputs, and recorded MIDI takes.

#include "EditorFixture.h"
#include "TestSupport.h"

#include "model/Errors.h"

#include <QTest>

#include <cmath>

using namespace sub::app;
using test::EditorFixture;

namespace {

constexpr int C = 60, E = 64, G = 67;
constexpr double kSec = 0.5;  // seconds per beat at 120 BPM

double round6(double value) { return std::round(value * 1e6) / 1e6; }

QStringList kinds(const std::vector<Device>& devices) {
    QStringList result;
    for (const Device& d : devices) result.append(d.kind);
    return result;
}

QStringList ids(const std::vector<Device>& devices) {
    QStringList result;
    for (const Device& d : devices) result.append(d.id);
    return result;
}

struct Played {
    double start;
    double end;
    int pitch;
    int velocity;
};

// A MIDI take from `start` (beats) lasting `beats`, of notes (start, end, pitch, velocity) in beats.
RecordedTake midiTake(const QString& trackId, double start, double beats, const std::vector<Played>& played) {
    RecordedTake take{trackId, {}, start * kSec, beats * kSec, {}, true};
    for (const Played& n : played) take.notes.push_back({n.start * kSec, n.end * kSec, n.pitch, n.velocity});
    return take;
}

}  // namespace

class TestEditorMidi : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void midiTracksComeWithTheSynth() {
        EditorFixture f;
        const QString track = f.editor.addMidiTrack();
        QCOMPARE(f.track(track).kind, QStringLiteral("midi"));
        QCOMPARE(f.track(track).name, QStringLiteral("1 MIDI"));
        QCOMPARE(kinds(f.track(track).devices), QStringList{"synth"});
        const QString utility = f.editor.addDevice(track, "utility");
        QCOMPARE(f.project.device(track, utility).kind, QStringLiteral("utility"));
        QVERIFY(!f.editor.addDevice(track, "synth").isEmpty());  // replaces the instrument, stays first
        QCOMPARE(kinds(f.track(track).devices), (QStringList{"synth", "utility"}));
        const QString audio = f.editor.addAudioTrack();
        QVERIFY(f.editor.addDevice(audio, "synth").isEmpty());  // instruments need a MIDI track
        f.stack.undo();
        f.stack.undo();
        QCOMPARE(kinds(f.track(track).devices), (QStringList{"synth", "utility"}));
    }

    void moveDeviceGoesToItsNewPosition() {
        EditorFixture f;
        const QString track = f.editor.addMidiTrack();
        const QString synth = f.track(track).devices[0].id;
        const QString a = f.editor.addDevice(track, "utility");
        const QString b = f.editor.addDevice(track, "utility");
        const QString c = f.editor.addDevice(track, "utility");
        const auto chain = [&] { return ids(f.track(track).devices); };

        f.editor.moveDevice(track, a, 2);  // right
        QCOMPARE(chain(), (QStringList{synth, b, a, c}));
        f.editor.moveDevice(track, c, 1);  // left
        QCOMPARE(chain(), (QStringList{synth, c, b, a}));
        f.editor.moveDevice(track, b, 9);  // past the end
        QCOMPARE(chain(), (QStringList{synth, c, a, b}));
        f.editor.moveDevice(track, c, 0);  // not before the instrument
        f.editor.moveDevice(track, synth, 3);  // which doesn't move
        QCOMPARE(chain(), (QStringList{synth, c, a, b}));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Move Device"));
    }

    void midiClipsAndNoteEditsAreUndoable() {
        EditorFixture f;
        const QString track = f.editor.addMidiTrack();
        const auto ref = f.editor.addMidiClip(track, 4.0, 4.0);
        QVERIFY(ref);
        QCOMPARE(f.project.clip(ref->trackId, ref->clipId).name, QString());  // (MIDI clips have no name)
        for (double length : {1.0, 2.0, 3.0}) {  // one gesture: one undo step
            f.editor.setClipNotes(*ref, {Note{E, 0.0, length}, Note{C, 0.0, length}}, "Resize Notes", "gesture");
        }
        QVERIFY((f.project.clip(ref->trackId, ref->clipId).notes == std::vector<Note>{{C, 0.0, 3.0}, {E, 0.0, 3.0}}));
        f.stack.undo();
        QVERIFY(f.project.clip(ref->trackId, ref->clipId).notes.empty());
        QVERIFY(!f.editor.addMidiClip(f.editor.addAudioTrack(), 0.0, 4.0));
    }

    void clipsOnlyMoveOntoTracksOfTheirKind() {
        EditorFixture f;
        const QString audio1 = f.editor.addAudioTrack();
        const QString midi = f.editor.addMidiTrack();
        const QString audio2 = f.editor.addAudioTrack();
        const ClipRefs ref = f.editor.addClips(audio1, 0.0, {{"a.wav", 1.0}});
        QCOMPARE(f.editor.clampTrackDelta(ref, 1), 0);  // the MIDI track in between: stay
        QCOMPARE(f.editor.clampTrackDelta(ref, 2), 2);
        QCOMPARE(f.editor.moveClips(ref, 0.0, 1)[0].trackId, audio1);
        const auto midiRef = f.editor.addMidiClip(midi, 0.0, 4.0);
        QCOMPARE(f.editor.moveClips({*midiRef}, 2.0, 1)[0].trackId, midi);
        // Audio files meant for a MIDI track land on a new audio track instead.
        const ClipRefs added = f.editor.addClips(midi, 0.0, {{"b.wav", 1.0}});
        const QString newTrack = added[0].trackId;
        QVERIFY(newTrack != audio1 && newTrack != midi && newTrack != audio2);
        QVERIFY(!f.track(newTrack).isMidi());
    }

    // --- MIDI inputs and recording ---

    void midiTracksHearEveryInputAndCanBeArmed() {
        EditorFixture f;
        const QString track = f.editor.addMidiTrack();
        QVERIFY(f.track(track).midiInput == MidiInput{});
        QVERIFY(f.track(track).hasInput());
        f.editor.armTracks({track}, true);
        QVERIFY(f.track(track).armed);
        f.editor.setTrackMidiInput(track, MidiInput{"Keys", 3});
        QVERIFY((f.track(track).midiInput == MidiInput{"Keys", 3}));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Change MIDI Input"));
        f.editor.setTrackMidiInput(track, std::nullopt);
        QVERIFY(!f.track(track).midiInput && !f.track(track).hasInput());
        f.stack.undo();
        QVERIFY((f.track(track).midiInput == MidiInput{"Keys", 3}));
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackMidiInput(track, MidiInput{"", 17}));
        QVERIFY(!f.editor.trySetTrackMidiInput(track, true, "", 17));
        QVERIFY(f.editor.trySetTrackMidiInput(track, false));
        QVERIFY(!f.track(track).midiInput);
    }

    void aRecordedMidiTakeBecomesAClip() {
        EditorFixture f;
        const QString track = f.editor.addMidiTrack();
        const auto old = f.editor.addMidiClip(track, 0.0, 16.0);
        f.editor.setClipNotes(*old, {Note{C, 5.0, 1.0}}, "Add Note");
        const QString audio = f.editor.addAudioTrack();
        const std::vector<RecordedTake> takes{
            midiTake(track, 4.0, 4.0, {{4.0, 4.5, C, 100}, {5.26, 6.0, E, 80}, {7.5, 8.0, G, 127}}),
            midiTake(audio, 0.0, 4.0, {{0.0, 1.0, C, 100}})};  // not a MIDI track: ignored
        const int steps = f.stack.index();
        const ClipRefs refs = f.editor.addRecordings(takes);
        QCOMPARE(refs.size(), 1);
        QCOMPARE(f.stack.index(), steps + 1);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Record"));
        const Clip clip = f.project.clip(refs[0].trackId, refs[0].clipId);
        QCOMPARE(clip.name, QString());
        QCOMPARE(clip.startBeat, 4.0);
        QCOMPARE(clip.durationBeats, 4.0);
        struct Got {
            int pitch;
            double start;
            double length;
            int velocity;
            bool operator==(const Got&) const = default;
        };
        std::vector<Got> got;
        for (const Note& n : clip.notes) got.push_back({n.pitch, round6(n.start), round6(n.length), n.velocity});
        QVERIFY((got == std::vector<Got>{{C, 0.0, 0.5, 100}, {E, 1.26, 0.74, 80}, {G, 3.5, 0.5, 127}}));
        // It replaced what was under it (as in Arrangement recording, without overdub).
        std::vector<std::pair<double, double>> spans;
        for (const Clip& c : f.track(track).clips) spans.emplace_back(c.startBeat, c.endBeat());
        QVERIFY((spans == std::vector<std::pair<double, double>>{{0.0, 4.0}, {4.0, 8.0}, {8.0, 16.0}}));
        f.stack.undo();
        QCOMPARE(f.track(track).clips.size(), size_t(1));
        QCOMPARE(f.track(track).clips[0].id, old->clipId);
    }

    void recordQuantization() {
        EditorFixture f;
        const QString track = f.editor.addMidiTrack();
        const RecordedTake take = midiTake(track, 1.1, 3.0, {{1.1, 1.3, C, 100}, {1.6, 2.2, E, 100}, {3.95, 4.05, G, 100}});
        ClipRefs refs = f.editor.addRecordings({take}, 0.5);
        Clip clip = f.project.clip(refs[0].trackId, refs[0].clipId);
        // Starts on the arrangement's grid, lengths as played; a note the grid would
        // take out of the clip (before where recording began) stays where it was played.
        struct Got {
            int pitch;
            double start;
            double length;
            bool operator==(const Got&) const = default;
        };
        std::vector<Got> got;
        for (const Note& n : clip.notes) got.push_back({n.pitch, round6(clip.startBeat + n.start), round6(n.length)});
        QVERIFY((got == std::vector<Got>{{C, 1.1, 0.2}, {E, 1.5, 0.6}, {G, 4.0, 0.1}}));
        refs = f.editor.addRecordings({take});
        std::vector<double> starts;
        for (const Note& n : f.project.clip(refs[0].trackId, refs[0].clipId).notes) starts.push_back(round6(n.start + 1.1));
        QVERIFY((starts == std::vector<double>{1.1, 1.6, 3.95}));
    }

    // --- More of the clip editor on MIDI clips ---

    void midiClipSpansAndClipsOverARange() {
        EditorFixture f;
        const QString track = f.editor.addMidiTrack();
        const QString audio = f.editor.addAudioTrack();
        f.editor.addMidiClip(track, 0.0, 2.0);
        f.editor.addMidiClip(track, 6.0, 4.0);
        // From the grid line at or before it, not reaching back over the clip before;
        // a bar long, or up to the next clip.
        QVERIFY((f.editor.midiClipSpan(track, 2.5, 1.0) == std::pair<double, double>{2.0, 4.0}));
        QVERIFY((f.editor.midiClipSpan(track, 1.5, 1.0) == std::pair<double, double>{1.0, 4.0}));  // (in a clip)
        QVERIFY((f.editor.midiClipSpan(track, 11.0, 0.0) == std::pair<double, double>{11.0, 4.0}));
        const ClipRefs made = f.editor.addMidiClipsOver(12.0, 14.0, {track, audio});
        QCOMPARE(made.size(), 1);
        QCOMPARE(made[0].trackId, track);
        QVERIFY(!f.editor.addMidiClip(track, 0.0, 1.0 / 128));  // too short
    }

    void consolidatingJoinsMidiClipsOnEachTrack() {
        EditorFixture f;
        const QString track = f.editor.addMidiTrack();
        const auto a = f.editor.addMidiClip(track, 0.0, 2.0);
        const auto b = f.editor.addMidiClip(track, 4.0, 2.0);
        f.editor.setClipNotes(*a, {Note{C, 0.5, 1.0}}, "Add Note");
        f.editor.setClipNotes(*b, {Note{E, 0.0, 1.0}}, "Add Note");
        QVERIFY(f.editor.consolidatable({*a}).isEmpty());  // (one clip: nothing to join)
        const ClipRefs joined = f.editor.consolidateClips({*a, *b});
        QCOMPARE(joined.size(), 1);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Consolidate"));
        QCOMPARE(f.track(track).clips.size(), size_t(1));
        const Clip& clip = f.track(track).clips[0];
        QCOMPARE(clip.startBeat, 0.0);
        QCOMPARE(clip.endBeat(), 6.0);
        QCOMPARE(clip.notes.size(), size_t(2));
        QVERIFY(f.editor.consolidateClips({joined[0]}).isEmpty());
        f.stack.undo();
        QCOMPARE(f.track(track).clips.size(), size_t(2));
        QCOMPARE(f.editor.clipsAt({track}, 1.0), (ClipRefs{*a}));
        QVERIFY(f.editor.clipsAt({track}, 2.0).isEmpty());  // (at an edge)
    }
};

QTEST_GUILESS_MAIN(TestEditorMidi)
#include "test_editor_midi.moc"
