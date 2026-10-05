// What is selected in the arrangement (Selection): tracks (Ctrl- and
// Shift-click), clip ranges (and the rows they show over) and lane ranges,
// breakpoints, what Delete acts on,
// the insert marker, and what is left of a selection after the project
// changes (Selection's rules, without the window).

#include "EditorFixture.h"
#include "TestSupport.h"

#include "session/Selection.h"

#include <QSignalSpy>
#include <QTest>

using namespace sub::app;
using test::EditorFixture;
using test::env;
namespace autom = sub::app::automation;

namespace {

ClipRefs refsOf(const Project& project, const QString& trackId) {
    ClipRefs refs;
    for (const Clip& c : project.track(trackId).clips) refs.append({trackId, c.id});
    return refs;
}

// Three audio tracks with a clip each: beats 0-4, 2-8 and 4-12.
QStringList threeTracks(EditorFixture& f) {
    QStringList ids;
    const std::pair<double, double> spans[] = {{0.0, 2.0}, {2.0, 3.0}, {4.0, 4.0}};  // (start, seconds)
    int i = 0;
    for (const auto& [start, seconds] : spans) {
        const QString track = f.editor.addAudioTrack();
        f.editor.commitClips("Add", {{track, {test::audioClip(QStringLiteral("c%1").arg(i++), start, seconds)}}});
        ids.append(track);
    }
    return ids;
}

}  // namespace

class TestSelection : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void selectingClipsSelectsTheAreaTheyCover() {
        EditorFixture f;
        const QStringList tracks = threeTracks(f);
        Selection selection;
        QSignalSpy changed(&selection, &Selection::changed);
        // From the earliest start to the latest end, on every track from the first clip's to the last's.
        selection.selectClips(f.editor, {refsOf(f.project, tracks[0])[0], refsOf(f.project, tracks[2])[0]});
        QVERIFY((selection.timeRange() == TimeRange{0.0, 12.0, tracks}));
        QVERIFY(selection.clipRange());
        QCOMPARE(selection.clips().size(), 3);  // (the one in between too: it is in the area)
        QCOMPARE(selection.trackId(), tracks[0]);
        QCOMPARE(selection.focus(), Selection::Focus::Clips);
        QVERIFY(changed.size() >= 1);
        // On a track of them, if given.
        selection.selectClips(f.editor, refsOf(f.project, tracks[1]), tracks[1]);
        QVERIFY((selection.timeRange() == TimeRange{2.0, 8.0, {tracks[1]}}));
        QCOMPARE(selection.clips(), (QSet<ClipRef>{refsOf(f.project, tracks[1])[0]}));
        QCOMPARE(selection.trackId(), tracks[1]);
        // Clips that aren't there: nothing (on the track given).
        selection.selectClips(f.editor, {{tracks[0], "gone"}}, tracks[2]);
        QVERIFY(!selection.timeRange() && selection.clips().isEmpty());
        QCOMPARE(selection.trackId(), tracks[2]);
    }

    void aTimeRangeIsAClipRangeOrALaneRange() {
        EditorFixture f;
        const QStringList tracks = threeTracks(f);
        Selection selection;
        // With the clips it touches (possibly none): a clip range.
        selection.setTimeRange(1.0, 3.0, tracks.mid(0, 2), f.editor.clipsInRange(1.0, 3.0, tracks.mid(0, 2)));
        QVERIFY(selection.clipRange());
        QCOMPARE(selection.clips().size(), 2);
        QCOMPARE(selection.trackId(), tracks[0]);
        QVERIFY(selection.lanes().isEmpty());
        selection.setTimeRange(5.0, 7.0, {tracks[1]}, QSet<ClipRef>());
        QVERIFY(selection.clipRange() && selection.clips().isEmpty());
        // Without: a lane range, of automation lanes if given (the master's have no track).
        selection.setTimeRange(1.0, 3.0, {}, std::nullopt, {{kMaster, autom::kMixerVolume}});
        QVERIFY(selection.hasTimeRange() && !selection.clipRange());
        QCOMPARE(selection.lanes(), (QList<LaneRef>{{kMaster, autom::kMixerVolume}}));
        QCOMPARE(selection.trackId(), tracks[1]);  // (unchanged: the master has no track)
        // Lanes go with a clip range: it has none.
        selection.setTimeRange(1.0, 3.0, {tracks[0]}, QSet<ClipRef>(), {{tracks[0], autom::kMixerPan}});
        QVERIFY(selection.lanes().isEmpty());
        // Nothing selected: an empty range, or one on no tracks and no lanes.
        selection.setTimeRange(3.0, 3.0, {tracks[0]}, QSet<ClipRef>{refsOf(f.project, tracks[0])[0]});
        QVERIFY(!selection.hasTimeRange() && !selection.clipRange() && selection.clips().isEmpty());
        selection.setLaneRange(1.0, 2.0, {});
        QVERIFY(!selection.hasTimeRange());
        selection.setLaneRange(1.0, 2.0, {tracks[2]});
        QVERIFY(selection.hasTimeRange() && !selection.clipRange());
        QCOMPARE(selection.rangeTrackIds(), QStringList{tracks[2]});
        QCOMPARE(selection.rangeStart(), 1.0);
        QCOMPARE(selection.rangeEnd(), 2.0);
        // Clearing it can never leave a clip range behind.
        selection.setTimeRange(1.0, 3.0, {tracks[0]}, QSet<ClipRef>());
        selection.clear();
        QVERIFY(!selection.clipRange());
        QCOMPARE(selection.trackId(), tracks[0]);  // (where the insert marker shows)
        selection.clear(tracks[2]);
        QCOMPARE(selection.trackId(), tracks[2]);
    }

    void tracksAreSelectedByClickCtrlClickAndShiftClick() {
        Selection selection;
        const QStringList order{"a", "b", "c", "d"};
        selection.selectTrack("b", true);
        QCOMPARE(selection.trackIds(), QStringList{"b"});
        QCOMPARE(selection.focus(), Selection::Focus::Track);
        selection.selectTrack("d", true, Selection::Mode::Toggle);
        QCOMPARE(selection.trackIds(), (QStringList{"b", "d"}));
        QCOMPARE(selection.trackId(), QStringLiteral("d"));  // the last one clicked
        selection.selectTrack("d", true, Selection::Mode::Toggle);  // out again
        QCOMPARE(selection.trackIds(), QStringList{"b"});
        QCOMPARE(selection.trackId(), QStringLiteral("b"));
        selection.selectTrack("d", true, Selection::Mode::Range, order);  // from the last one clicked
        QCOMPARE(selection.trackIds(), (QStringList{"b", "c", "d"}));
        QCOMPARE(selection.trackId(), QStringLiteral("d"));
        selection.selectTrack("a", true, Selection::Mode::Range, order);  // (still from b)
        QCOMPARE(selection.trackIds(), (QStringList{"a", "b"}));
        selection.selectTrack("c");  // a plain click: just it
        QCOMPARE(selection.trackIds(), QStringList{"c"});
        selection.selectTrack("c", false, Selection::Mode::Toggle);  // the only one, out: none
        QVERIFY(selection.trackIds().isEmpty() && selection.trackId().isEmpty());
        selection.selectTrack({});
        QVERIFY(selection.trackIds().isEmpty());
        // A range without a track clicked before it: just it.
        Selection fresh;
        fresh.selectTrack("c", false, Selection::Mode::Range, order);
        QCOMPARE(fresh.trackIds(), QStringList{"c"});
    }

    void whatDeleteActsOn() {
        EditorFixture f;
        const QStringList tracks = threeTracks(f);
        Selection selection;
        QSignalSpy changed(&selection, &Selection::changed);
        selection.selectTrack(tracks[2], true);
        selection.setTimeRange(0.0, 4.0, {tracks[2]}, f.editor.clipsInRange(0.0, 4.0, {tracks[2]}));
        QCOMPARE(selection.trackId(), tracks[2]);
        QCOMPARE(selection.focus(), Selection::Focus::Clips);
        // Right-clicking a selected track's header: its Cut and Copy act on the
        // selected tracks, not on a clip range selected since.
        selection.focusTracks();
        QCOMPARE(selection.focus(), Selection::Focus::Track);
        QVERIFY(!selection.hasTimeRange() && selection.clips().isEmpty());
        QCOMPARE(selection.trackIds(), QStringList{tracks[2]});
        const auto count = changed.size();
        selection.focusTracks();  // (already)
        QCOMPARE(changed.size(), count);
        selection.focusDevices();
        QCOMPARE(selection.focus(), Selection::Focus::Devices);
        selection.focusDevices();
        QCOMPARE(changed.size(), count + 1);
        selection.clear();  // selecting something else deselects the devices
        QCOMPARE(selection.focus(), Selection::Focus::Clips);
    }

    void breakpointsAreSelectedOnOneEnvelope() {
        Selection selection;
        selection.setTimeRange(1.0, 3.0, {"t"}, std::nullopt, {{"t", autom::kMixerPan}});
        selection.selectPoints("t", autom::kMixerPan, QSet<int>{0, 2});
        QCOMPARE(selection.focus(), Selection::Focus::Automation);
        QVERIFY(!selection.hasTimeRange() && selection.lanes().isEmpty());  // breakpoints aren't a range
        QCOMPARE(selection.selectedPoints("t", autom::kMixerPan), (QSet<int>{0, 2}));
        QVERIFY(selection.selectedPoints("t", autom::kMixerVolume).isEmpty());
        QCOMPARE(selection.trackId(), QStringLiteral("t"));
        selection.selectPoints(kMaster, autom::kMixerVolume, QList<int>{1});  // (as QML has it)
        QCOMPARE(selection.trackId(), QStringLiteral("t"));  // the master has no track
        QCOMPARE(selection.selectedPoints(kMaster, autom::kMixerVolume), QSet<int>{1});
        selection.selectPoints(kMaster, autom::kMixerVolume, QSet<int>());  // none: nothing
        QVERIFY(!selection.points());
        QCOMPARE(selection.focus(), Selection::Focus::Clips);
    }

    void theInsertMarker() {
        Selection selection;
        QSignalSpy moved(&selection, &Selection::insertChanged);
        selection.setInsert(6.0);
        QCOMPARE(selection.insertBeat(), 6.0);
        selection.setInsert(-2.0);  // not before the timeline's start
        QCOMPARE(selection.insertBeat(), 0.0);
        QCOMPARE(moved.size(), 2);
    }

    // After an edit (or its undo), what is selected and gone is let go of: the
    // area stays selected, without the clips that went.
    void pruningLetsGoOfWhatIsGone() {
        EditorFixture f;
        Project& p = f.project;
        const QStringList tracks = threeTracks(f);
        const QString ret = f.editor.addReturnTrack();
        Selection selection;
        selection.setTimeRange(1.0, 3.0, tracks.mid(0, 2), f.editor.clipsInRange(1.0, 3.0, tracks.mid(0, 2)));
        QCOMPARE(selection.clips().size(), 2);
        f.editor.deleteRange(1.0, 3.0, tracks.mid(0, 2));  // (trimmed and cut, the clips keep their ids)
        QSignalSpy changed(&selection, &Selection::changed);
        selection.prune(p);
        QCOMPARE(selection.clips().size(), 2);
        QCOMPARE(changed.size(), 0);
        f.editor.deleteClips(refsOf(p, tracks[1]));
        selection.prune(p);
        QVERIFY((selection.timeRange() == TimeRange{1.0, 3.0, tracks.mid(0, 2)}));  // still selected
        QCOMPARE(selection.clips(), QSet<ClipRef>{refsOf(p, tracks[0])[0]});
        QVERIFY(selection.clipRange());
        QCOMPARE(changed.size(), 1);
        selection.prune(p);  // (nothing more to let go of)
        QCOMPARE(changed.size(), 1);

        // Tracks that go: out of the range and of the selected tracks.
        selection.setTimeRange(0.0, 4.0, tracks, QSet<ClipRef>());
        f.editor.deleteTracks({tracks[0]});
        selection.prune(p);
        QCOMPARE(selection.rangeTrackIds(), tracks.mid(1));
        f.editor.deleteTracks(tracks.mid(1));
        selection.prune(p);
        QVERIFY(!selection.hasTimeRange());  // on no tracks: none
        f.stack.undo();
        f.stack.undo();
        selection.selectTrack(tracks[1], true);
        selection.selectTrack(ret, true, Selection::Mode::Toggle);  // (a return is selectable)
        f.editor.deleteTracks({ret});
        selection.prune(p);
        QCOMPARE(selection.trackIds(), QStringList{tracks[1]});
        QCOMPARE(selection.trackId(), tracks[1]);  // the last of those left
        selection.selectTrack(kMaster);
        selection.prune(p);
        QCOMPARE(selection.trackId(), kMaster);  // (the master is always there)

        // Breakpoints that go; lanes whose owner goes.
        f.editor.setEnvelope(tracks[1], autom::kMixerPan, env({{0.0, 0.1}, {1.0, 0.2}, {2.0, 0.3}}));
        selection.selectPoints(tracks[1], autom::kMixerPan, QSet<int>{1, 2});
        f.editor.deleteAutomationPoints(tracks[1], autom::kMixerPan, {2});
        selection.prune(p);
        QCOMPARE(selection.selectedPoints(tracks[1], autom::kMixerPan), QSet<int>{1});
        QCOMPARE(selection.focus(), Selection::Focus::Automation);
        f.editor.clearEnvelope(tracks[1], autom::kMixerPan);
        selection.prune(p);
        QVERIFY(!selection.points());
        QCOMPARE(selection.focus(), Selection::Focus::Clips);
        selection.setTimeRange(0.0, 1.0, {}, std::nullopt, {{tracks[1], autom::kMixerPan}, {kMaster, autom::kMixerVolume}});
        f.editor.deleteTracks({tracks[1]});
        selection.prune(p);
        QCOMPARE(selection.lanes(), (QList<LaneRef>{{kMaster, autom::kMixerVolume}}));
        QVERIFY(selection.hasTimeRange());  // (the master's lane is still there)
    }

    void aTimeSelectionOverAGroupTakesInItsTracks() {
        // As the arrangement selects it: the group's lane takes in what is in it.
        EditorFixture f;
        const QStringList tracks = threeTracks(f);
        const QString group = f.editor.groupTracks(tracks.mid(0, 2));
        const QStringList over = f.project.withContents({group});
        Selection selection;
        selection.setTimeRange(1.0, 3.0, over, f.editor.clipsInRange(1.0, 3.0, over));
        QCOMPARE(selection.rangeTrackIds(), (QStringList{group, tracks[0], tracks[1]}));
        QCOMPARE(selection.clips(), (QSet<ClipRef>{refsOf(f.project, tracks[0])[0], refsOf(f.project, tracks[1])[0]}));
        f.editor.deleteRange(selection.rangeStart(), selection.rangeEnd(), selection.rangeTrackIds());
        std::vector<std::pair<double, double>> spans;
        for (const Clip& c : f.track(tracks[0]).clips) spans.emplace_back(c.startBeat, c.endBeat(f.project.tempo()));
        QVERIFY((spans == std::vector<std::pair<double, double>>{{0.0, 1.0}, {3.0, 4.0}}));
    }

    void aTimeSelectionShowsOverTheRowsItCovers() {
        // Made over a group's row alone, it acts on what is in the group too
        // (rangeTrackIds), but shows over the group's row only (rangeRows).
        EditorFixture f;
        const QStringList tracks = threeTracks(f);
        const QString group = f.editor.groupTracks(tracks.mid(0, 2));
        const QStringList over = f.project.withContents({group});
        Selection selection;
        selection.setTimeRange(1.0, 3.0, over, f.editor.clipsInRange(1.0, 3.0, over), {}, {group});
        QCOMPARE(selection.rangeTrackIds(), (QStringList{group, tracks[0], tracks[1]}));
        QCOMPARE(selection.rangeRows(), QStringList{group});
        QCOMPARE(selection.clips().size(), 2);
        // Without rows (or with none of its tracks): it shows over all its tracks.
        selection.setTimeRange(1.0, 3.0, over, QSet<ClipRef>());
        QCOMPARE(selection.rangeRows(), over);
        selection.setTimeRange(1.0, 3.0, over, QSet<ClipRef>(), {}, {tracks[2]});
        QCOMPARE(selection.rangeRows(), over);
        // Rows go with the tracks they are of.
        selection.setTimeRange(1.0, 3.0, over, QSet<ClipRef>(), {}, {group, tracks[0]});
        f.editor.ungroup({group});  // (the group goes, what was in it stays)
        selection.prune(f.project);
        QCOMPARE(selection.rangeTrackIds(), (QStringList{tracks[0], tracks[1]}));
        QCOMPARE(selection.rangeRows(), QStringList{tracks[0]});
        selection.clear();
        QVERIFY(selection.rangeRows().isEmpty());
        // A selection of tracks has none.
        selection.setTimeRange(1.0, 3.0, over, QSet<ClipRef>(), {}, {group});
        selection.selectTrack(tracks[2], true);
        QVERIFY(selection.rangeRows().isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestSelection)
#include "test_selection.moc"
