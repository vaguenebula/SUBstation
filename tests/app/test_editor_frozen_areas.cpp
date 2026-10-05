// Time selections over frozen tracks through the editor: deleting, moving,
// copying (Ctrl-drag), duplicating, cutting and pasting a stretch of a frozen
// track (or of a frozen group) takes its frozen audio along (Freeze::segments),
// in one undo step with the clips and the automation baked into it; what can't
// be done that way is refused, and says why: moves between a frozen track and
// another, a stretch of only some of a frozen group, pastes into a frozen track
// of what wasn't copied from it; the other edits of a frozen track's clips stay
// refused. Unfreezing and flattening after such edits.

#include "EditorFixture.h"
#include "TestSupport.h"

#include "model/Commands.h"

#include <QTest>

#include <cmath>
#include <memory>
#include <tuple>

using namespace sub::app;
using test::EditorFixture;
using test::env;
namespace autom = sub::app::automation;

namespace {

// 8 s of frozen audio: 16 beats at 120 BPM.
const Freeze kFreeze{QStringLiteral("frozen.wav"), 8.0, 120.0};

double rounded(double value) { return std::round(value * 1e6) / 1e6; }

// Clips (or segments) as (start beat, end beat, offset in seconds).
using Piece = std::tuple<double, double, double>;
std::vector<Piece> pieces(const std::vector<Clip>& clips) {
    std::vector<Piece> out;
    for (const Clip& c : clips) out.emplace_back(rounded(c.startBeat), rounded(c.endBeat(120.0)), rounded(c.offsetSec));
    return out;
}

// What of a frozen track's audio plays.
std::vector<Piece> playing(const Project& p, const QString& id) { return pieces(p.track(id).frozen->playing(id)); }
std::vector<Piece> clipsOf(const Project& p, const QString& id) { return pieces(p.track(id).clips); }

// An audio track with clips ("<name>0", "<name>1"...) at these (start beat, seconds).
QString audioTrack(EditorFixture& f, const QString& name, std::initializer_list<std::pair<double, double>> clips) {
    const QString track = f.editor.addAudioTrack(-1, name);
    std::vector<Clip> list;
    int i = 0;
    for (const auto& [start, seconds] : clips) list.push_back(test::audioClip(name + QString::number(i++), start, seconds));
    f.editor.commitClips("Add", {{track, list}});
    return track;
}

QStringList clipIds(const Track& track) {
    QStringList ids;
    for (const Clip& c : track.clips) ids.append(c.id);
    return ids;
}

}  // namespace

class TestEditorFrozenAreas : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void deletingAStretchOfAFrozenTrackDeletesItsFrozenAudioToo() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f, "A", {{0.0, 1.0}, {4.0, 1.0}});  // beats 0-2 and 4-6
        f.editor.freezeTracks({{a, kFreeze}});
        QCOMPARE(playing(p, a), (std::vector<Piece>{{0, 16, 0}}));  // at first: all of it
        const int count = f.stack.count();
        QVERIFY(f.editor.deleteRange(1.0, 5.0, {a}));
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{0, 1, 0}, {5, 6, 0.5}}));
        QCOMPARE(playing(p, a), (std::vector<Piece>{{0, 1, 0}, {5, 16, 2.5}}));
        QVERIFY(p.isFrozen(a) && f.messages.isEmpty());
        QCOMPARE(f.stack.count(), count + 1);  // one undo step, both
        QCOMPARE(f.stack.undoText(), QStringLiteral("Delete Time Selection"));
        f.stack.undo();
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{0, 2, 0}, {4, 6, 0}}));
        QVERIFY(f.track(a).frozen == kFreeze);  // as it was: all of it plays
        f.stack.redo();
        QCOMPARE(playing(p, a), (std::vector<Piece>{{0, 1, 0}, {5, 16, 2.5}}));
    }

    void movingAStretchMovesTheFrozenAudioAndTheAutomationBakedIntoIt() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f, "A", {{0.0, 1.0}, {8.0, 1.0}});  // beats 0-2 and 8-10
        const QString utility = f.editor.addDevice(a, "utility");
        const QString gain = autom::deviceKey(utility, "gain");
        f.editor.setEnvelope(a, gain, env({{1.0, 0.2}, {3.0, 0.8}}));
        f.editor.freezeTracks({{a, kFreeze}});
        const int count = f.stack.count();
        const auto [start, tracks] = f.editor.moveRange(0.0, 4.0, {a}, 8.0);
        QCOMPARE(start, 8.0);
        QCOMPARE(tracks, QStringList{a});
        // The stretch replaces everything where it lands, as its frozen audio does:
        // the clip at beat 8 goes.
        QCOMPARE(clipIds(f.track(a)), QStringList{"A0"});
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{8, 10, 0}}));
        QCOMPARE(playing(p, a), (std::vector<Piece>{{4, 8, 2}, {8, 12, 0}, {12, 16, 6}}));
        // (A device's automation: baked into the frozen audio, so it goes along.)
        QVERIFY(p.envelope(a, gain).front().beat >= 8.0);
        QCOMPARE(autom::valueAt(p.envelope(a, gain), 9.0), std::optional<double>(0.2));
        QCOMPARE(autom::valueAt(p.envelope(a, gain), 11.0), std::optional<double>(0.8));
        QCOMPARE(f.stack.count(), count + 1);
        f.stack.undo();
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{0, 2, 0}, {8, 10, 0}}));
        QVERIFY(f.track(a).frozen == kFreeze);
        QCOMPARE(p.envelope(a, gain), env({{1.0, 0.2}, {3.0, 0.8}}));
        // Ctrl-drag: a copy, over what is where it lands.
        f.editor.moveRange(0.0, 4.0, {a}, 8.0, 0, true);
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{0, 2, 0}, {8, 10, 0}}));
        QVERIFY(f.track(a).clips[1].id != "A1");  // (the copy, not the clip that was there)
        QCOMPARE(playing(p, a), (std::vector<Piece>{{0, 8, 0}, {8, 12, 0}, {12, 16, 6}}));
        QVERIFY(f.messages.isEmpty());
    }

    void clipsDontMoveBetweenAFrozenTrackAndAnother() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f, "A", {{0.0, 1.0}});
        const QString b = audioTrack(f, "B", {{0.0, 1.0}});
        f.editor.freezeTracks({{a, kFreeze}});
        const int count = f.stack.count();
        const auto [start, tracks] = f.editor.moveRange(0.0, 4.0, {a}, 2.0, 1);  // off it
        QCOMPARE(start, 0.0);
        QCOMPARE(tracks, QStringList{a});  // (where it still is)
        QVERIFY(f.messages.back().contains("A is frozen"));
        f.editor.moveRange(0.0, 4.0, {b}, 2.0, -1);  // onto it
        QCOMPARE(f.stack.count(), count);
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{0, 2, 0}}));
        QCOMPARE(clipsOf(p, b), (std::vector<Piece>{{0, 2, 0}}));
        QVERIFY(f.messages.back().contains("A is frozen: clips can't move between it and other tracks"));
        // In time only, it moves.
        f.editor.moveRange(0.0, 4.0, {a, b}, 2.0);
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{2, 4, 0}}));
        QCOMPARE(clipsOf(p, b), (std::vector<Piece>{{2, 4, 0}}));
        QCOMPARE(playing(p, a), (std::vector<Piece>{{2, 6, 0}, {6, 16, 3}}));
        // What a drag would make of it says where the frozen audio would go (the drag's preview).
        const MovedRange moved = f.editor.movedRange(2.0, 6.0, {a}, 4.0);
        QCOMPARE(pieces(moved.frozen.value(a)), (std::vector<Piece>{{6, 10, 0}, {10, 16, 5}}));
        QVERIFY(f.track(a).frozen->playing(a) != moved.frozen.value(a));  // (nothing changed yet)
    }

    void pastingIntoAFrozenTrackTakesWhatWasCopiedFromIt() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f, "A", {{0.0, 1.0}});
        const QString b = audioTrack(f, "B", {});
        const QString c = audioTrack(f, "C", {{0.0, 1.0}});
        f.editor.freezeTracks({{a, kFreeze}, {c, Freeze{QStringLiteral("c.wav"), 8.0, 120.0}}});
        const auto content = f.editor.copyRange(0.0, 4.0, {a});
        QVERIFY(content);
        QCOMPARE(content->frozen.size(), size_t(1));
        QCOMPARE(content->frozen[0].trackId, a);
        QCOMPARE(content->frozen[0].path, kFreeze.path);
        QCOMPARE(pieces(content->frozen[0].segments), (std::vector<Piece>{{0, 4, 0}}));

        // Onto a track that isn't frozen: the clips as they are.
        QVERIFY(f.editor.paste(*content, 8.0, b));
        QCOMPARE(clipsOf(p, b), (std::vector<Piece>{{8, 10, 0}}));
        QVERIFY(!f.track(b).frozen);
        QVERIFY(f.track(a).frozen == kFreeze);
        // Back into the frozen track it came from: the clips, with their frozen
        // audio, over everything where they land.
        const auto area = f.editor.paste(*content, 8.0, a);
        QCOMPARE(area, std::optional<TimeRange>(TimeRange{8.0, 12.0, {a}}));
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{0, 2, 0}, {8, 10, 0}}));
        QCOMPARE(playing(p, a), (std::vector<Piece>{{0, 8, 0}, {8, 12, 0}, {12, 16, 6}}));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Paste"));

        // Into another frozen track: refused, nothing pasted.
        const int count = f.stack.count();
        QVERIFY(!f.editor.paste(*content, 8.0, c));
        QVERIFY(f.messages.back().contains("C is frozen: only what was copied from it can be pasted into it"));
        // Nor what was copied from a track that isn't frozen.
        const auto unfrozen = f.editor.copyRange(8.0, 12.0, {b});
        QVERIFY(unfrozen && unfrozen->frozen.empty());
        QVERIFY(!f.editor.paste(*unfrozen, 4.0, a));
        QCOMPARE(f.stack.count(), count);
        // Nor from it before it was frozen again (another render).
        f.editor.unfreezeTracks({a});
        f.editor.freezeTracks({{a, Freeze{QStringLiteral("again.wav"), 8.0, 120.0}}});
        QVERIFY(!f.editor.paste(*content, 4.0, a));
        QVERIFY(f.messages.back().contains("A is frozen"));
    }

    void cuttingAndPastingBack() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f, "A", {{0.0, 1.0}, {4.0, 1.0}});
        f.editor.freezeTracks({{a, kFreeze}});
        const auto content = f.editor.cutRange(0.0, 4.0, {a});
        QVERIFY(content);
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{4, 6, 0}}));
        QCOMPARE(playing(p, a), (std::vector<Piece>{{4, 16, 2}}));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Cut"));
        QVERIFY(f.editor.paste(*content, 0.0, a));
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{0, 2, 0}, {4, 6, 0}}));
        QCOMPARE(playing(p, a), (std::vector<Piece>{{0, 4, 0}, {4, 16, 2}}));
        // Frozen audio there without clips is copied too (a tail), to paste back onto it.
        const auto tail = f.editor.copyRange(12.0, 14.0, {a});
        QVERIFY(tail);
        QCOMPARE(tail->tracks.size(), size_t(1));
        QVERIFY(tail->tracks[0].clips.empty());
        QCOMPARE(pieces(tail->frozen[0].segments), (std::vector<Piece>{{0, 2, 6}}));
    }

    void duplicatingAStretchOfAFrozenTrack() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f, "A", {{0.0, 1.0}, {3.0, 1.0}});  // beats 0-2 and 3-5
        f.editor.freezeTracks({{a, kFreeze}});
        const auto copies = f.editor.duplicateRange(0.0, 4.0, {a});
        QVERIFY(copies);
        QCOMPARE(copies->size(), 2);
        // The copy (4-8) replaces what was there: the second clip's last beat.
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{0, 2, 0}, {3, 4, 0}, {4, 6, 0}, {7, 8, 0}}));
        QCOMPARE(playing(p, a), (std::vector<Piece>{{0, 4, 0}, {4, 8, 0}, {8, 16, 4}}));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Duplicate Time Selection"));
    }

    void aFrozenGroupsAudioGoesWithAStretchOfAllOfIt() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f, "A", {{0.0, 1.0}});  // beats 0-2
        const QString b = audioTrack(f, "B", {{2.0, 1.0}});  // beats 2-4
        const QString group = f.editor.groupTracks({a, b});
        f.editor.renameTrack(group, "G");
        f.editor.setEnvelope(a, autom::kMixerVolume, env({{1.0, 0.5}}));  // (baked into the group's audio)
        f.editor.freezeTracks({{group, kFreeze}});
        const int count = f.stack.count();
        // Only some of what is in it: refused.
        QVERIFY(!f.editor.deleteRange(0.0, 3.0, {a}));
        QVERIFY(f.messages.back().contains("G is frozen: select the whole group to edit what is in it"));
        QVERIFY(!f.editor.deleteRange(0.0, 3.0, {a, b}));  // (its tracks, not the group)
        QVERIFY(!f.editor.duplicateRange(0.0, 3.0, {a}));
        QVERIFY(!f.editor.cutRange(0.0, 3.0, {a}));
        QCOMPARE(f.editor.moveRange(0.0, 3.0, {a}, 4.0).first, 0.0);
        QCOMPARE(f.stack.count(), count);
        // All of it: the group's frozen audio goes along (its tracks are no frozen tracks themselves).
        const QStringList all{group, a, b};
        QVERIFY(f.editor.deleteRange(0.0, 3.0, all));
        QCOMPARE(clipsOf(p, a), std::vector<Piece>());
        QCOMPARE(clipsOf(p, b), (std::vector<Piece>{{3, 4, 0.5}}));
        QCOMPARE(playing(p, group), (std::vector<Piece>{{3, 16, 1.5}}));
        QVERIFY(!f.track(a).frozen && !f.track(b).frozen);
        f.stack.undo();
        // Moved, with the mixer automation of what is in it (baked in too).
        f.editor.moveRange(0.0, 4.0, all, 8.0);
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{8, 10, 0}}));
        QCOMPARE(clipsOf(p, b), (std::vector<Piece>{{10, 12, 0}}));
        QCOMPARE(playing(p, group), (std::vector<Piece>{{4, 8, 2}, {8, 12, 0}, {12, 16, 6}}));
        QVERIFY(p.envelope(a, autom::kMixerVolume).front().beat >= 8.0);
        QCOMPARE(autom::valueAt(p.envelope(a, autom::kMixerVolume), 9.0), std::optional<double>(0.5));
        f.stack.undo();
        // Copied from all of it, it pastes back onto its tracks, its audio with it;
        // a track of it with nothing copied is cleared where it lands too.
        const auto content = f.editor.copyRange(0.0, 2.0, all);
        QVERIFY(content);
        QCOMPARE(content->tracks.size(), size_t(1));  // (A's clip; the group has nothing of its own there)
        QCOMPARE(content->frozen.size(), size_t(1));
        const auto area = f.editor.paste(*content, 2.0, a);
        QVERIFY(area);
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{0, 2, 0}, {2, 4, 0}}));
        QCOMPARE(clipsOf(p, b), std::vector<Piece>());
        QCOMPARE(playing(p, group), (std::vector<Piece>{{0, 2, 0}, {2, 4, 0}, {4, 16, 2}}));
        // Not onto the group's other track.
        QVERIFY(!f.editor.paste(*content, 2.0, b));
        QVERIFY(f.messages.back().contains("G is frozen: only what was copied from it"));
        // Copied from some of it, it has none of its audio: it pastes elsewhere, not into it.
        const auto some = f.editor.copyRange(0.0, 2.0, {a});
        QVERIFY(some && some->frozen.empty());
        QVERIFY(!f.editor.paste(*some, 6.0, a));
    }

    void aFrozenTrackInAFrozenGroupKeepsItsOwnAudioInStepToo() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f, "A", {{0.0, 1.0}});
        f.editor.freezeTracks({{a, Freeze{QStringLiteral("a.wav"), 8.0, 120.0}}});
        const QString group = f.editor.groupTracks({a});
        f.editor.freezeTracks({{group, kFreeze}});
        QVERIFY(f.editor.deleteRange(0.0, 1.0, {group, a}));
        QCOMPARE(playing(p, group), (std::vector<Piece>{{1, 16, 0.5}}));
        QCOMPARE(playing(p, a), (std::vector<Piece>{{1, 16, 0.5}}));  // (it plays again once the group is unfrozen)
    }

    void otherEditsOfAFrozenTracksClipsStayRefused() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f, "A", {{0.0, 1.0}});
        f.editor.freezeTracks({{a, kFreeze}});
        const int count = f.stack.count();
        f.editor.splitClips({{a, "A0"}}, 1.0);
        f.editor.deleteClips({{a, "A0"}});
        f.editor.moveClips({{a, "A0"}}, 4.0);
        f.editor.duplicateClips({{a, "A0"}});
        Clip trimmed = f.track(a).clips[0];
        trimmed.durationSec = 0.5;
        f.editor.replaceClip(a, trimmed, "Trim Clip");
        f.editor.updateClips({{a, "A0"}}, [](const Clip& c) {
            Clip louder = c;
            louder.gainDb = 6.0;
            return louder;
        }, "Change Gain");
        QMap<QString, std::pair<QString, double>> reversed;
        reversed.insert(QStringLiteral("a.wav"), {QStringLiteral("a R.wav"), 1.0});
        f.editor.reverseRange(0.0, 2.0, {a}, reversed);
        f.editor.commitClips("Edit", {{a, {}}});
        QCOMPARE(f.stack.count(), count);
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{0, 2, 0}}));
        QVERIFY(f.track(a).frozen == kFreeze);
        QVERIFY(f.messages.back().contains("A is frozen: unfreeze it to change its clips"));
        // The command taking the frozen audio along changes both, and undoes both.
        const ClipLists before{{a, f.track(a).clips}};
        const ClipLists after{{a, {}}};
        f.stack.push(std::make_unique<SetClipsCommand>(&p, "Edit", before, after, QString(),
                                                       FrozenSegments{{a, std::nullopt}},
                                                       FrozenSegments{{a, std::vector<Clip>()}})
                         .release());
        QVERIFY(f.track(a).clips.empty() && f.track(a).frozen->segments->empty());
        QVERIFY(f.track(a).frozen->playing(a).empty());  // (nothing of it plays)
        f.stack.undo();
        QVERIFY(f.track(a).frozen == kFreeze);
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{0, 2, 0}}));
    }

    void unfreezingAfterwardsLeavesTheClipsAsEdited() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f, "A", {{0.0, 1.0}, {4.0, 1.0}});
        f.editor.freezeTracks({{a, kFreeze}});
        f.editor.moveRange(4.0, 6.0, {a}, 4.0);
        QCOMPARE(f.editor.unfreezeTracks({a}), QStringList{a});
        QVERIFY(!f.track(a).frozen);
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{0, 2, 0}, {8, 10, 0}}));
        f.stack.undo();  // frozen again, its audio as edited
        QCOMPARE(playing(p, a), (std::vector<Piece>{{0, 4, 0}, {6, 8, 3}, {8, 10, 2}, {10, 16, 5}}));
        // Flattened: an audio track playing what of its frozen audio played.
        QCOMPARE(f.editor.flattenTracks({a}), QStringList{a});
        QCOMPARE(clipsOf(p, a), (std::vector<Piece>{{0, 4, 0}, {6, 8, 3}, {8, 10, 2}, {10, 16, 5}}));
        for (const Clip& clip : f.track(a).clips) {
            QVERIFY(clip.path == kFreeze.path && clip.isWarped() && clip.name == "A");
        }
    }
};

QTEST_GUILESS_MAIN(TestEditorFrozenAreas)
#include "test_editor_frozen_areas.moc"
