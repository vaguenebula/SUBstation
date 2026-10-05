// The editor's clip edits and undo: adding, moving, duplicating, splitting
// clips, tempo and warping trims, gesture merging, inputs and arming, recorded
// takes, copy and paste with the automation under clips, time selections, and
// reversing (the editor parts of tests/test_edits.py and tests/test_keys.py).

#include "EditorFixture.h"
#include "TestSupport.h"

#include "editor/ProjectEditor.h"
#include "io/Serialization.h"
#include "model/Edits.h"
#include "model/Errors.h"
#include "model/Ids.h"

#include <QJsonObject>
#include <QTest>

#include <cmath>

using namespace sub::app;
using test::EditorFixture;

namespace {

constexpr double kTempo = 120.0;  // 1 beat = 0.5 s

double round6(double value) { return std::round(value * 1e6) / 1e6; }

using Spans = std::vector<std::pair<double, double>>;

Spans spans(const std::vector<Clip>& clips, double tempo = kTempo) {
    Spans result;
    for (const Clip& c : clips) result.emplace_back(round6(c.startBeat), round6(c.endBeat(tempo)));
    return result;
}

bool near(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }

Clip clip(double start, double beats, double offset = 0.0, double source = 100.0, const QString& id = {}) {
    return Clip::audio(id.isEmpty() ? newId() : id, QStringLiteral("a.wav"), QStringLiteral("a"), start, beats * 0.5,
                       offset, source);
}

// A warped clip `beats` long; at 60 BPM a beat is one second of audio.
Clip warped(double start, double beats, double segmentBpm = 60.0, const QString& id = {}, double source = 100.0) {
    Clip c = Clip::audio(id.isEmpty() ? newId() : id, QStringLiteral("a.wav"), QStringLiteral("a"), start,
                         beats * 60.0 / segmentBpm, 0.0, source);
    c.warp = true;
    c.segmentBpm = segmentBpm;
    return c;
}

struct Row {
    QString id;
    QString name;
    std::vector<Clip> clips;
    friend bool operator==(const Row&, const Row&) = default;
};

std::vector<Row> snapshot(const Project& project) {
    std::vector<Row> rows;
    for (const Track& t : project.tracks()) rows.push_back({t.id, t.name, t.clips});
    return rows;
}

Clip withBpm(const Clip& c, double bpm) {
    Clip changed = c;
    changed.segmentBpm = bpm;
    return changed;
}

}  // namespace

class TestEditorEdits : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void addClipsCreatesTrackAndPlacesSequentially() {
        EditorFixture f;
        const ClipRefs refs = f.editor.addClips({}, 2.0, {{"C:/x/kick.wav", 1.0}, {"C:/x/snare.wav", 0.5}});
        QCOMPARE(f.project.tracks().size(), size_t(1));
        QCOMPARE(f.project.tracks()[0].name, QStringLiteral("kick"));
        QVERIFY((spans(f.project.tracks()[0].clips) == Spans{{2, 4}, {4, 5}}));
        QCOMPARE(refs.size(), 2);
        f.stack.undo();
        QVERIFY(f.project.tracks().empty());
    }

    void moveClipsAcrossTracksWithOverlap() {
        EditorFixture f;
        const QString t1 = f.editor.addAudioTrack();
        const QString t2 = f.editor.addAudioTrack();
        const ClipRefs a = f.editor.addClips(t1, 0.0, {{"a.wav", 2.0}});
        f.editor.addClips(t2, 0.0, {{"b.wav", 4.0}});
        const auto before = snapshot(f.project);

        const ClipRefs moved = f.editor.moveClips(a, 2.0, 1);
        QCOMPARE(moved.size(), 1);
        QCOMPARE(moved[0].trackId, t2);
        QVERIFY(f.track(t1).clips.empty());
        QVERIFY((spans(f.track(t2).clips) == Spans{{0, 2}, {2, 6}, {6, 8}}));

        f.stack.undo();
        QVERIFY(snapshot(f.project) == before);
        f.stack.redo();
        QVERIFY((spans(f.track(t2).clips) == Spans{{0, 2}, {2, 6}, {6, 8}}));
    }

    void moveIsClampedToTimelineAndTracks() {
        EditorFixture f;
        const QString t1 = f.editor.addAudioTrack();
        const ClipRefs a = f.editor.addClips(t1, 1.0, {{"a.wav", 1.0}});
        const ClipRefs moved = f.editor.moveClips(a, -10.0, 5);
        QCOMPARE(moved[0].trackId, t1);
        QCOMPARE(f.track(t1).clips[0].startBeat, 0.0);
    }

    void duplicatePlacesCopiesAfterSelection() {
        EditorFixture f;
        const QString t = f.editor.addAudioTrack();
        const ClipRefs refs = f.editor.addClips(t, 0.0, {{"a.wav", 1.0}, {"b.wav", 1.0}});
        const ClipRefs copies = f.editor.duplicateClips(refs);
        QVERIFY((spans(f.track(t).clips) == Spans{{0, 2}, {2, 4}, {4, 6}, {6, 8}}));
        for (const ClipRef& copy : copies) QVERIFY(!refs.contains(copy));
    }

    void splitAndDelete() {
        EditorFixture f;
        const QString t = f.editor.addAudioTrack();
        const ClipRefs refs = f.editor.addClips(t, 0.0, {{"a.wav", 2.0}});
        f.editor.splitClips(refs, 1.0);
        QVERIFY((spans(f.track(t).clips) == Spans{{0, 1}, {1, 4}}));
        ClipRefs all;
        for (const Clip& c : f.track(t).clips) all.append({t, c.id});
        f.editor.deleteClips(all);
        QVERIFY(f.track(t).clips.empty());
        f.stack.undo();
        f.stack.undo();
        QVERIFY((spans(f.track(t).clips) == Spans{{0, 4}}));
    }

    void trackParamChangesMergeDuringAGesture() {
        EditorFixture f;
        const QString t = f.editor.addAudioTrack();
        for (double db : {-1.0, -2.0, -3.0}) f.editor.setTrackParam(t, TrackField::VolumeDb, db, QStringLiteral("gesture"));
        QCOMPARE(f.track(t).volumeDb, -3.0);
        f.stack.undo();
        QCOMPARE(f.track(t).volumeDb, 0.0);
        f.stack.redo();
        f.editor.setTrackParam(t, TrackField::VolumeDb, -6.0, QStringLiteral("another"));
        f.stack.undo();
        QCOMPARE(f.track(t).volumeDb, -3.0);
    }

    void deleteTrackUndoRestoresEverything() {
        EditorFixture f;
        const QString t = f.editor.addAudioTrack();
        f.editor.addClips(t, 0.0, {{"a.wav", 1.0}});
        f.editor.addDevice(t, QStringLiteral("utility"));
        const auto before = snapshot(f.project);
        f.editor.deleteTracks({t});
        QVERIFY(f.project.tracks().empty());
        f.stack.undo();
        QVERIFY(snapshot(f.project) == before);
        QCOMPARE(f.track(t).devices[0].kind, QStringLiteral("utility"));
    }

    void fasterTempoTrimsClipsInsteadOfOverlapping() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        f.editor.addClips(track, 0.0, {{"a.wav", 2.0}, {"b.wav", 1.0}});  // 4 beats, then 2 at 120 BPM
        QCOMPARE(p.track(track).clips[1].startBeat, 4.0);
        const auto ends = [&] { return spans(p.track(track).clips, p.tempo()); };

        // Dragging the tempo up trims the first clip at the second one's start...
        const QString drag = QStringLiteral("drag");
        f.editor.setTempo(150.0, drag);
        QVERIFY((ends() == Spans{{0.0, 4.0}, {4.0, 6.5}}));
        QVERIFY(near(p.track(track).clips[0].durationSec, 1.6));
        // ...and back down within the same drag restores it: steps fit from the start of the drag.
        f.editor.setTempo(130.0, drag);
        f.editor.setTempo(100.0, drag);
        QVERIFY(near(p.track(track).clips[0].durationSec, 2.0));
        f.editor.setTempo(180.0, drag);
        QVERIFY((ends() == Spans{{0.0, 4.0}, {4.0, 7.0}}));
        // The whole drag is one undo step that restores tempo and clips.
        f.stack.undo();
        QCOMPARE(p.tempo(), 120.0);
        QCOMPARE(p.track(track).clips[0].durationSec, 2.0);
        QCOMPARE(p.track(track).clips[1].durationSec, 1.0);
        f.stack.redo();
        QCOMPARE(p.tempo(), 180.0);
        QVERIFY((ends()[0] == std::pair<double, double>{0.0, 4.0}));
    }

    void settings() {
        EditorFixture f;
        f.editor.setTempo(140.0);
        f.editor.setLoop(true, 4.0, 8.0);
        QCOMPARE(f.project.tempo(), 140.0);
        QVERIFY(f.project.loopEnabled());
        QCOMPARE(f.project.loopStart(), 4.0);
        f.stack.undo();
        f.stack.undo();
        QCOMPARE(f.project.tempo(), 120.0);
        QVERIFY(!f.project.loopEnabled());
        // (Held to 20..999 BPM, to 0.01; a loop at least a sixteenth long, not before the start.)
        f.editor.setTempo(1500.0);
        QCOMPARE(f.project.tempo(), 999.0);
        f.editor.setTempo(123.456);
        QCOMPARE(f.project.tempo(), 123.46);
        f.editor.setLoop(true, -2.0, -1.0);
        QCOMPARE(f.project.loopStart(), 0.0);
        QCOMPARE(f.project.loopEnd(), 0.25);
        f.editor.setTimeSignature(6, 8);
        QCOMPARE(f.project.timeSignature(), (TimeSignature{6, 8}));
        f.editor.setTimeSignature(5, 3);  // (not one)
        QCOMPARE(f.project.timeSignature(), (TimeSignature{6, 8}));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Change Time Signature"));
        f.editor.setLoopEnabled(false);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Toggle Loop"));
    }

    // --- Warping ---

    void tempoChangeLeavesWarpedClipsAlone() {
        EditorFixture f;
        const QString t = f.editor.addAudioTrack();
        f.project.setClips(t, {warped(0.0, 4.0, 60.0, "w"), clip(4.0, 2.0, 0.0, 100.0, "u")});
        f.editor.setTempo(60.0);  // the unwarped clip grows, the warped one doesn't
        const auto& clips = f.track(t).clips;
        QVERIFY(near(clips[0].lengthBeats(60.0), 4.0));
        QVERIFY(near(clips[1].lengthBeats(60.0), 1.0));
    }

    void segmentBpmChangesTrimAtTheNextClipAndDragsRecover() {
        EditorFixture f;
        const QString t = f.editor.addAudioTrack();
        const std::vector<Clip> original{warped(0.0, 2.0, 60.0, "w"), clip(3.0, 2.0, 0.0, 100.0, "next")};
        f.project.setClips(t, original);
        const ClipRefs refs{{t, "w"}};
        const QString key = QStringLiteral("drag");
        const int depth = f.stack.count();
        // A higher segment BPM plays the audio slower, so the clip grows (2 s at
        // 180 BPM is 6 beats) until it meets the next clip, which keeps its place.
        f.editor.updateClips(refs, [](const Clip& c) { return withBpm(c, 180.0); }, "Change Segment BPM", key);
        const auto& clips = f.track(t).clips;
        QVERIFY(near(clips[0].endBeat(kTempo), 3.0));
        QVERIFY(near(clips[0].durationSec, 1.0));  // 3 beats at 180 BPM
        QVERIFY(clips[1] == original[1]);
        // ...and dragging back in the same gesture restores it untrimmed.
        f.editor.updateClips(refs, [](const Clip& c) { return withBpm(c, 60.0); }, "Change Segment BPM", key);
        QVERIFY(f.track(t).clips == original);
        QCOMPARE(f.stack.count(), depth + 1);
        // Separate edits don't merge: a lower BPM (faster) leaves the trimmed clip short.
        f.editor.updateClips(refs, [](const Clip& c) { return withBpm(c, 180.0); }, "Change Segment BPM");
        f.editor.updateClips(refs, [](const Clip& c) { return withBpm(c, 90.0); }, "Change Segment BPM");
        QVERIFY(near(f.project.clip(t, "w").lengthBeats(kTempo), 1.5));
        f.stack.undo();
        f.stack.undo();
        QVERIFY(f.track(t).clips == original);
    }

    void turningWarpOffTrimsLikeATempoChange() {
        EditorFixture f;
        const QString t = f.editor.addAudioTrack();
        f.project.setClips(t, {warped(0.0, 2.0, 60.0, "w"), clip(3.0, 2.0, 0.0, 100.0, "next")});
        // Unwarped, 2 s of audio is 4 beats at 120 BPM: cut where the next clip starts.
        f.editor.updateClips(
            {{t, "w"}},
            [](const Clip& c) {
                Clip changed = c;
                changed.warp = false;
                return changed;
            },
            "Toggle Warp");
        QVERIFY(near(f.project.clip(t, "w").endBeat(kTempo), 3.0));
    }

    // --- Inputs, monitoring, arming ---

    void trackInputAndMonitoringAreUndoableArmingIsNot() {
        EditorFixture f;
        const QString a = f.editor.addAudioTrack();
        const QString b = f.editor.addAudioTrack();
        const QString midi = f.editor.addMidiTrack();
        const int steps = f.stack.count();
        f.editor.setTrackInput(a, {2, 3});
        f.editor.setTrackMonitor(a, "in");
        QVERIFY((f.track(a).input == std::vector<int>{2, 3}));
        QCOMPARE(f.track(a).monitor, QStringLiteral("in"));
        QCOMPARE(f.stack.count(), steps + 2);
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackMonitor(a, "loud"));
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackInput(a, {0, 1, 2}));
        QVERIFY(!f.editor.trySetTrackMonitor(a, "loud"));  // (from QML: said on `refused`)
        QCOMPARE(f.messages.size(), 1);
        f.stack.undo();
        f.stack.undo();
        QVERIFY(f.track(a).input.empty());
        QCOMPARE(f.track(a).monitor, QStringLiteral("auto"));

        const auto armed = [&] {
            return std::vector<bool>{f.track(a).armed, f.track(b).armed, f.track(midi).armed};
        };
        f.editor.armTracks({a}, true);
        f.editor.armTracks({b, midi}, true, true);  // the others are disarmed
        QVERIFY((armed() == std::vector<bool>{false, true, true}));
        f.editor.armTracks({a}, true, false);
        QVERIFY((armed() == std::vector<bool>{true, true, true}));
        f.editor.armTracks({a, b, midi}, false);
        QVERIFY((armed() == std::vector<bool>{false, false, false}));
        QCOMPARE(f.stack.index(), steps);
        QCOMPARE(f.stack.count(), steps + 2);  // still redoable
    }

    void recordedTakesBecomeClipsReplacingWhatWasUnderThem() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        f.editor.addClips(track, 0.0, {{"C:/x/loop.wav", 4.0}});  // 8 beats at 120 BPM
        const std::vector<Clip> before = f.track(track).clips;
        const std::vector<RecordedTake> takes{
            {track, "C:/rec/take 1.wav", 1.0, 1.5, {}, false},  // beats 2..5
            {"gone", "C:/rec/other.wav", 0.0, 1.0, {}, false}};  // its track was deleted meanwhile
        ClipRefs refs = f.editor.addRecordings(takes);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Record"));
        QCOMPARE(refs.size(), 1);
        QCOMPARE(refs[0].trackId, track);
        Clip take = p.clip(refs[0].trackId, refs[0].clipId);
        QCOMPARE(take.path, QStringLiteral("C:/rec/take 1.wav"));
        QCOMPARE(take.startBeat, 2.0);
        QCOMPARE(take.durationSec, 1.5);
        QCOMPARE(take.offsetSec, 0.0);
        QVERIFY((spans(f.track(track).clips) == Spans{{0, 2}, {2, 5}, {5, 8}}));  // overdubbed: the take wins
        f.stack.undo();  // the take goes, and with it the project's reference to its file
        QVERIFY(f.track(track).clips == before);
        for (const Track& t : p.tracks()) {
            for (const Clip& c : t.clips) QVERIFY(c.path != u"C:/rec/take 1.wav");
        }
        f.stack.redo();
        QVERIFY((spans(f.track(track).clips) == Spans{{0, 2}, {2, 5}, {5, 8}}));

        // A take that began before the timeline (recorded from the start, heard late) starts at 0.
        refs = f.editor.addRecordings({{track, "C:/rec/take 2.wav", -0.25, 1.0, {}, false}});
        take = p.clip(refs[0].trackId, refs[0].clipId);
        QCOMPARE(take.startBeat, 0.0);
        QCOMPARE(take.offsetSec, 0.25);
        QCOMPARE(take.durationSec, 0.75);
        QCOMPARE(take.sourceDurationSec, 1.0);
        QVERIFY(f.editor.addRecordings({{track, "C:/rec/empty.wav", -2.0, 1.0, {}, false}}).isEmpty());
    }

    // --- Copy and paste ---

    void copyPasteTakesJustTheRangeAndPastesAtATrack() {
        EditorFixture f;
        const QString t1 = f.editor.addAudioTrack();
        const QString t2 = f.editor.addAudioTrack();
        const QString t3 = f.editor.addAudioTrack();
        f.editor.addClips(t1, 0.0, {{"a.wav", 2.0}});  // beats 0-4
        f.editor.addClips(t2, 2.0, {{"b.wav", 1.0}});  // beats 2-4
        const auto content = f.editor.copyRange(1.0, 3.0, {t1, t2});
        QVERIFY(content);
        QCOMPARE(content->length, 2.0);
        QCOMPARE(content->tracks.size(), size_t(2));
        QCOMPARE(content->tracks[0].row, 0);
        QCOMPARE(content->tracks[1].row, 1);
        QVERIFY((spans(content->tracks[0].clips) == Spans{{0, 2}}));
        QVERIFY((spans(content->tracks[1].clips) == Spans{{1, 2}}));
        QVERIFY(!f.editor.copyRange(10.0, 12.0, {t1}));  // nothing there

        // Onto the second track: the content's second row lands on the third.
        const auto area = f.editor.paste(*content, 8.0, t2);
        QVERIFY(area);
        QVERIFY((*area == TimeRange{8.0, 10.0, {t2, t3}}));
        QVERIFY((spans(f.track(t2).clips) == Spans{{2, 4}, {8, 10}}));
        QVERIFY((spans(f.track(t3).clips) == Spans{{9, 10}}));
        // Pasting again makes new clips; one undo step each.
        f.editor.paste(*content, 10.0, t2);
        QSet<QString> ids;
        for (const Clip& c : f.track(t2).clips) ids.insert(c.id);
        QCOMPARE(ids.size(), 3);
        f.stack.undo();
        f.stack.undo();
        QVERIFY(f.track(t3).clips.empty());
    }

    void pasteReplacesWhatIsThereAndCutTakesItOut() {
        EditorFixture f;
        const QString t = f.editor.addAudioTrack();
        f.editor.addClips(t, 0.0, {{"a.wav", 4.0}});  // beats 0-8
        const auto content = f.editor.cutRange(2.0, 4.0, {t});
        QVERIFY((spans(f.track(t).clips) == Spans{{0, 2}, {4, 8}}));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Cut"));
        f.editor.paste(*content, 5.0, t);
        QVERIFY((spans(f.track(t).clips) == Spans{{0, 2}, {4, 5}, {5, 7}, {7, 8}}));
        f.stack.undo();
        f.stack.undo();
        QVERIFY((spans(f.track(t).clips) == Spans{{0, 8}}));
    }

    void pasteOntoAnotherKindOfTrackGoesBackWhereItCameFrom() {
        EditorFixture f;
        const QString audio = f.editor.addAudioTrack();
        const QString midi = f.editor.addMidiTrack();
        f.editor.addClips(audio, 0.0, {{"a.wav", 1.0}});
        const auto content = f.editor.copyRange(0.0, 2.0, {audio});
        QCOMPARE(f.editor.pasteTargets(*content, midi), std::optional<QStringList>(QStringList{audio}));
        QCOMPARE(f.editor.paste(*content, 4.0, midi), std::optional<TimeRange>(TimeRange{4.0, 6.0, {audio}}));
        QVERIFY(f.track(midi).clips.empty());
        f.editor.deleteTracks({audio});
        QVERIFY(!f.editor.paste(*content, 4.0, midi));
    }

    void copiedAutomationComesAlongUnlessLocked() {
        EditorFixture f;
        const QString t1 = f.editor.addAudioTrack();
        const QString t2 = f.editor.addAudioTrack();
        f.editor.addClips(t1, 0.0, {{"a.wav", 1.0}});
        f.editor.setEnvelope(t1, automation::kMixerVolume, test::env({{0.0, 0.2}, {2.0, 0.6}}));
        const auto content = f.editor.copyRange(0.0, 2.0, {t1});
        f.editor.paste(*content, 4.0, t2);
        QVERIFY(f.project.envelope(t2, automation::kMixerVolume) == test::env({{4.0, 0.2}, {6.0, 0.6}}));

        f.editor.setAutomationLocked(true);
        QVERIFY(f.editor.copyRange(0.0, 2.0, {t1})->tracks[0].automation.empty());
        f.editor.cutRange(0.0, 2.0, {t1});
        QCOMPARE(f.project.envelope(t1, automation::kMixerVolume).size(), size_t(2));  // locked: it stays
    }

    // Delete, Copy, Duplicate and moving a time selection act on the clips and the
    // automation of every track in it: a group's too, and tracks without clips there.
    void aTimeSelectionTakesEverythingInItOnEveryTrack() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = f.editor.addAudioTrack();
        const QString b = f.editor.addAudioTrack();
        const QString group = f.editor.groupTracks({a, b});
        QCOMPARE(p.withContents({group}), (QStringList{group, a, b}));
        QCOMPARE(p.withContents({b, group}), (QStringList{group, a, b}));  // (in the arrangement's order, once)
        f.editor.addClips(a, 0.0, {{"a.wav", 2.0}});  // beats 0-4
        const Envelope rise = test::env({{1.0, 0.2}, {3.0, 0.8}});
        for (const QString& id : {group, b}) f.editor.setEnvelope(id, automation::kMixerVolume, rise);  // automation, no clips
        const QStringList tracks = p.withContents({group});

        const auto content = f.editor.copyRange(0.0, 4.0, tracks);
        QCOMPARE(content->tracks.size(), size_t(3));
        const auto described = [&](std::size_t i) {
            const CopiedTrack& c = content->tracks[i];
            return std::make_tuple(c.trackId, c.row, !c.clips.empty(), !c.automation.empty());
        };
        QVERIFY(described(0) == std::make_tuple(group, 0, false, true));
        QVERIFY(described(1) == std::make_tuple(a, 1, true, false));
        QVERIFY(described(2) == std::make_tuple(b, 2, false, true));

        f.editor.duplicateRange(0.0, 4.0, tracks);
        QVERIFY((spans(p.track(a).clips) == Spans{{0, 4}, {4, 8}}));
        for (const QString& id : {group, b}) {
            const Envelope e = p.envelope(id, automation::kMixerVolume);
            for (const auto& point : test::env({{5.0, 0.2}, {7.0, 0.8}})) {
                QVERIFY(std::find_if(e.begin(), e.end(), [&](const AutomationPoint& q) {
                            return q.beat == point.beat && q.value == point.value;
                        }) != e.end());
            }
        }
        f.stack.undo();

        const auto [start, moved] = f.editor.moveRange(0.0, 4.0, tracks, 8.0);
        QCOMPARE(start, 8.0);
        QCOMPARE(moved, tracks);
        QVERIFY((spans(p.track(a).clips) == Spans{{8, 12}}));
        for (const QString& id : {group, b}) {
            std::vector<std::pair<double, double>> after;
            for (const AutomationPoint& point : p.envelope(id, automation::kMixerVolume)) {
                if (point.beat > 8.0) after.emplace_back(point.beat, point.value);
            }
            QVERIFY((after == std::vector<std::pair<double, double>>{{9.0, 0.2}, {11.0, 0.8}}));
        }
        f.stack.undo();

        f.editor.deleteRange(0.0, 4.0, tracks);
        QVERIFY(p.track(a).clips.empty());
        QCOMPARE(f.stack.undoText(), QStringLiteral("Delete Time Selection"));
        QVERIFY(p.envelope(group, automation::kMixerVolume).empty() && p.envelope(b, automation::kMixerVolume).empty());
        f.stack.undo();
        QVERIFY(p.envelope(group, automation::kMixerVolume) == rise);
        QCOMPARE(p.track(a).clips.size(), size_t(1));

        f.editor.setAutomationLocked(true);  // locked: the automation stays where it is
        f.editor.deleteRange(0.0, 4.0, tracks);
        QVERIFY(p.track(a).clips.empty());
        QVERIFY(p.envelope(group, automation::kMixerVolume) == rise);
    }

    void movedRangeIsWhatMoveRangeMakes() {
        EditorFixture f;
        const QString t1 = f.editor.addAudioTrack();
        const QString t2 = f.editor.addAudioTrack();
        f.editor.addClips(t1, 0.0, {{"a.wav", 2.0}});  // beats 0-4
        const auto before = snapshot(f.project);
        const MovedRange moved = f.editor.movedRange(1.0, 3.0, {t1}, 4.0, 1);
        QCOMPARE(moved.deltaBeats, 4.0);
        QCOMPARE(moved.trackDelta, 1);
        QVERIFY((spans(moved.clips.value(t1)) == Spans{{0, 1}, {3, 4}}));
        QVERIFY((spans(moved.clips.value(t2)) == Spans{{5, 7}}));
        QVERIFY(snapshot(f.project) == before);  // nothing changed yet
        f.editor.moveRange(1.0, 3.0, {t1}, 4.0, 1);
        for (auto it = moved.clips.constBegin(); it != moved.clips.constEnd(); ++it) {
            QVERIFY(spans(f.track(it.key()).clips) == spans(it.value()));
        }
        QCOMPARE(f.editor.movedRange(1.0, 3.0, {t1}, -9.0).deltaBeats, -1.0);  // (not before the timeline's start)
    }

    void reversingPlaysAStretchBackwardsAndAgainForwards() {
        EditorFixture f;
        const QString t = f.editor.addAudioTrack();
        f.editor.addClips(t, 0.0, {{"a.wav", 4.0}});  // beats 0-8, all of a 4 s file
        Clip original = f.track(t).clips[0];
        Clip part = original;
        part.offsetSec = 0.5;
        part.durationSec = 2.0;
        const Clip flipped = edits::reverseClip(part, "a R.wav", 4.0);
        // It plays seconds 0.5-2.5 of the file backwards: seconds 1.5-3.5 of the reversed copy.
        QCOMPARE(flipped.path, QStringLiteral("a R.wav"));
        QCOMPARE(flipped.offsetSec, 1.5);
        QCOMPARE(flipped.durationSec, 2.0);
        QCOMPARE(flipped.reversedFrom, QStringLiteral("a.wav"));
        QCOMPARE(flipped.startBeat, original.startBeat);

        // A time range inside a clip: it is split there, and just the part inside is reversed.
        const ClipRefs refs = f.editor.reverseRange(2.0, 4.0, {t}, {{"a.wav", {"a R.wav", 4.0}}});
        const auto& clips = f.track(t).clips;
        QVERIFY((spans(clips) == Spans{{0, 2}, {2, 4}, {4, 8}}));
        QCOMPARE(refs, (ClipRefs{{t, clips[1].id}}));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Reverse Clip"));
        QCOMPARE(clips[1].path, QStringLiteral("a R.wav"));
        QVERIFY(near(clips[1].offsetSec, 2.0));
        QCOMPARE(clips[1].reversedFrom, QStringLiteral("a.wav"));
        QCOMPARE(clips[0].path, QStringLiteral("a.wav"));
        QCOMPARE(clips[2].path, QStringLiteral("a.wav"));
        // Reversed again, it plays its file again (forwards), as it did.
        f.editor.reverseRange(2.0, 4.0, {t}, {{"a R.wav", {"a.wav", 4.0}}});
        const Clip middle = f.track(t).clips[1];
        QCOMPARE(middle.path, QStringLiteral("a.wav"));
        QVERIFY(near(middle.offsetSec, 1.0));
        QCOMPARE(middle.reversedFrom, QString());
        f.stack.undo();
        f.stack.undo();
        QVERIFY((spans(f.track(t).clips) == Spans{{0, 8}}));
        // Clips whose files have no reversed copy stay as they are.
        QVERIFY(f.editor.reverseRange(0.0, 8.0, {t}, {}).isEmpty());
    }

    // --- From QML ---

    void theQmlOperationsAreInvokable() {
        // (QML calls them through the meta-object system, by name.)
        EditorFixture f;
        QString track;
        QVERIFY(QMetaObject::invokeMethod(&f.editor, "addAudioTrack", Q_RETURN_ARG(QString, track), Q_ARG(int, -1),
                                          Q_ARG(QString, QStringLiteral("Vox"))));
        QCOMPARE(f.track(track).name, QStringLiteral("Vox"));
        bool done = false;
        QVERIFY(QMetaObject::invokeMethod(&f.editor, "trySetTrackParam", Q_RETURN_ARG(bool, done), Q_ARG(QString, track),
                                          Q_ARG(QString, QStringLiteral("volume_db")), Q_ARG(double, -6.0),
                                          Q_ARG(QString, QString())));
        QVERIFY(done);
        QCOMPARE(f.track(track).volumeDb, -6.0);
        const QMetaObject* meta = f.editor.metaObject();
        for (const char* signature :
             {"deleteTracks(QStringList)", "groupTracks(QStringList)", "moveTracks(QStringList,int,QString)",
              "setTempo(double,QString)", "deleteRange(double,double,QStringList)", "addDevice(QString,QString,int,QString)",
              "moveDevicesToTrack(QString,QStringList,QString,int,QString)",
              "setChainParam(QString,QString,QString,double,QString)", "setMacro(QString,QString,int,double,QString)",
              "setDeviceParam(QString,QString,QString,double,QString)", "addAutomationPoint(QString,QString,double,double,QString)",
              "deleteAutomationPoints(QString,QString,QList<int>)", "toggleAllAutomation()", "unfreezeTracks(QStringList)",
              "trySetDeviceSidechain(QString,QString,QString,QString)", "tryMapMacro(QString,QString,int,QString,QString,double,double)"}) {
            QVERIFY2(meta->indexOfMethod(QMetaObject::normalizedSignature(signature)) >= 0, signature);
        }
        QVERIFY(meta->indexOfSignal("refused(QString)") >= 0);
        QVERIFY(meta->indexOfSignal("pluginAdded(QString,QString)") >= 0);
        QVERIFY(meta->indexOfSignal("parameterTouched(QString,QString)") >= 0);
    }

    // --- Keys and file names (the editor parts of tests/test_keys.py) ---

    void addedClipsFollowNameAndProjectKey() {
        EditorFixture f;
        f.editor.setKey(Key{7, false});  // G major
        const ClipRefs refs = f.editor.addClips({}, 0.0, {{"C:/x/Bass_Loop_100_Am.wav", 4.8}});
        QCOMPARE(refs.size(), 1);
        const Clip& c = f.project.clip(refs[0].trackId, refs[0].clipId);
        QVERIFY(c.isWarped());
        QCOMPARE(c.segmentBpm, 100.0);
        QCOMPARE(c.transpose, -5);  // C -> G
        QVERIFY(near(c.lengthBeats(f.project.tempo()), 8.0));  // 4.8 s at 100 BPM
    }

    void keyIsUndoableAndSaved() {
        EditorFixture f;
        const Key aMinor{9, true};
        f.editor.setKey(aMinor);
        QVERIFY(f.project.key() == aMinor);
        const QJsonObject data = projectToJson(f.project);
        f.stack.undo();
        QVERIFY(!f.project.key());
        loadInto(f.project, data);
        QVERIFY(f.project.key() == aMinor);
        f.editor.setKeyByName("F#m");  // (as QML has it)
        QCOMPARE(f.project.keyName(), QStringLiteral("F#m"));
        f.editor.setKeyByName({});
        QVERIFY(!f.project.key());
    }
};

QTEST_GUILESS_MAIN(TestEditorEdits)
#include "test_editor_edits.moc"
