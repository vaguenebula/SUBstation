// Group tracks through the editor: grouping, ungrouping, moving tracks into and
// out of groups (all undoable), inserting tracks in groups, deleting groups,
// folding, arming, saving, their default height, and cutting, copying and
// pasting groups.

#include "EditorFixture.h"
#include "TestSupport.h"

#include "io/Serialization.h"
#include "model/Devices.h"
#include "model/Routing.h"
#include "model/TrackNames.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace sub::app;
using test::EditorFixture;
using test::ids;

namespace {

using Names = std::vector<std::pair<QString, QString>>;

// Each track's name and its group's name ("": none), in order.
Names names(const Project& project) {
    QHash<QString, QString> byId;
    for (const Track& t : project.tracks()) byId.insert(t.id, t.name);
    Names result;
    for (const Track& t : project.tracks()) result.emplace_back(t.name, t.parent ? byId.value(*t.parent) : QString());
    return result;
}

QStringList tracks(EditorFixture& f, const QStringList& trackNames) {
    QStringList ids;
    for (const QString& name : trackNames) ids.append(f.editor.addAudioTrack(-1, name));
    return ids;
}

QStringList order(const Project& project) {
    QStringList result;
    for (const Track& t : project.tracks()) result.append(t.id);
    return result;
}

}  // namespace

class TestEditorGroups : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void groupAndUngroupRoundTrip() {
        EditorFixture f;
        Project& p = f.project;
        const QStringList abc = tracks(f, {"A", "B", "C"});
        const QString a = abc[0], c = abc[2];
        const QString group = f.editor.groupTracks({c, a});
        QVERIFY(f.track(group).isGroup() && !f.track(group).parent);
        const QString g = f.track(group).name;
        QVERIFY((names(p) == Names{{g, ""}, {"A", g}, {"C", g}, {"B", ""}}));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Group Tracks"));

        // Grouping inside a group nests.
        const QString inner = f.editor.groupTracks({c});
        const QString i = f.track(inner).name;
        QVERIFY((names(p) == Names{{g, ""}, {"A", g}, {i, g}, {"C", i}, {"B", ""}}));
        QCOMPARE(p.ancestors(c), (QStringList{inner, group}));
        QCOMPARE(p.depth(c), 2);
        QStringList inside;
        for (const Track* t : p.descendants(group)) inside.append(t->name);
        QCOMPARE(inside, (QStringList{"A", i, "C"}));

        f.stack.undo();
        f.stack.undo();
        QVERIFY((names(p) == Names{{"A", ""}, {"B", ""}, {"C", ""}}));
        f.stack.redo();
        f.stack.redo();
        QVERIFY((names(p)[3] == std::pair<QString, QString>{"C", i}));

        // Ungrouping: what was in the group takes its place, in its group.
        f.editor.ungroup({group});
        const QString moved = f.track(inner).name;  // (renumbered: a row up)
        QCOMPARE(moved, numberedName(QStringLiteral("# Group"), 2));
        QVERIFY((names(p) == Names{{"A", ""}, {moved, ""}, {"C", moved}, {"B", ""}}));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Ungroup Tracks"));
        f.stack.undo();
        QVERIFY((names(p)[0] == std::pair<QString, QString>{g, ""}));
        QVERIFY((names(p)[3] == std::pair<QString, QString>{"C", i}));
        QVERIFY(!treeProblem(p.tracks()));
    }

    void movingTracksIntoAndOutOfGroups() {
        EditorFixture f;
        Project& p = f.project;
        const QStringList abc = tracks(f, {"A", "B", "C"});
        const QString a = abc[0], b = abc[1], c = abc[2];
        const QString group = f.editor.groupTracks({a});
        const QString g = f.track(group).name;
        // Into the group, after A.
        QVERIFY(f.editor.moveTracks({c}, p.trackIndex(a) + 1, group));
        QVERIFY((names(p) == Names{{g, ""}, {"A", g}, {"C", g}, {"B", ""}}));
        // Out of it, to the end.
        QVERIFY(f.editor.moveTracks({a}, static_cast<int>(p.tracks().size()), {}));
        QVERIFY((names(p) == Names{{g, ""}, {"C", g}, {"B", ""}, {"A", ""}}));
        // Not amid another group's tracks without being in it, nor a group into itself.
        const QString other = f.editor.groupTracks({b});
        const QString o = f.track(other).name;
        QVERIFY(!f.editor.moveTracks({a}, p.trackIndex(c), {}));
        QVERIFY(!f.editor.moveTracks({group}, p.trackIndex(c) + 1, group));
        f.editor.moveTracks({other}, p.trackIndex(c) + 1, group);  // a group with its tracks
        QVERIFY((names(p) == Names{{g, ""}, {"C", g}, {o, g}, {"B", o}, {"A", ""}}));
        QVERIFY(!f.editor.moveTracks({group}, p.trackIndex(b) + 1, other));  // into a group in it
        QVERIFY(!f.editor.canMoveTracks({a}, 0, a));  // (into what isn't a group)
        for (int i = 0; i < 4; ++i) f.stack.undo();
        QVERIFY((names(p) == Names{{g, ""}, {"A", g}, {"B", ""}, {"C", ""}}));
    }

    void insertedTracksGoIntoTheGroupTheyAreInsertedIn() {
        EditorFixture f;
        Project& p = f.project;
        const QStringList ab = tracks(f, {"A", "B"});
        const QString a = ab[0], b = ab[1];
        const QString group = f.editor.groupTracks({a, b});
        InsertionPoint point = f.editor.insertionPoint(a);
        const QString added = f.editor.addMidiTrack(*point.index, "M", kDefaultInstrument, std::nullopt, point.parent);
        QCOMPARE(f.track(added).parent, std::optional<QString>(group));
        QCOMPARE(p.trackIndex(added), p.trackIndex(a) + 1);
        QCOMPARE(f.track(added).color, f.track(group).color);  // a new track takes its group's colour
        point = f.editor.insertionPoint(group);
        const QString afterGroup = f.editor.addAudioTrack(*point.index, "After", point.parent);
        QVERIFY(!f.track(afterGroup).parent);
        QVERIFY(f.track(afterGroup).color != f.track(group).color);  // (in none: a colour of its own)
        QCOMPARE(p.trackIndex(afterGroup), static_cast<int>(p.tracks().size()) - 1);
        // Between a group's tracks, a track can only be in that group.
        const QString amid = f.editor.addAudioTrack(p.trackIndex(b), "Amid", std::nullopt);
        QCOMPARE(f.track(amid).parent, std::optional<QString>(group));
        QCOMPARE(f.track(amid).color, f.track(group).color);  // (the group it was let into)
        // Clips dropped on a group go on a new audio track.
        const ClipRefs refs = f.editor.addClips(group, 0.0, {{"x.wav", 1.0}}, static_cast<int>(p.tracks().size()));
        QVERIFY(!f.track(refs[0].trackId).isGroup());
        QVERIFY(f.track(group).clips.empty());
        QVERIFY(f.editor.insertionPoint({}) == InsertionPoint{});
    }

    void deletingAGroupDeletesItsTracksAndUndoBringsThemBack() {
        EditorFixture f;
        Project& p = f.project;
        const QStringList abc = tracks(f, {"A", "B", "C"});
        const QString group = f.editor.groupTracks({abc[0], abc[1]});
        const QString inner = f.editor.groupTracks({abc[1]});
        const Names before = names(p);
        f.editor.deleteTracks({group});
        QVERIFY((names(p) == Names{{"C", ""}}));
        f.stack.undo();
        QVERIFY(names(p) == before);
        QCOMPARE(f.track(inner).parent, std::optional<QString>(group));
    }

    void foldingIsViewState() {
        EditorFixture f;
        Project& p = f.project;
        const QStringList ab = tracks(f, {"A", "B"});
        const QString group = f.editor.groupTracks({ab[0]});
        const int steps = f.stack.count();
        f.editor.setFolded(group, true);
        QVERIFY(f.track(group).folded && p.isHidden(ab[0]) && !p.isHidden(ab[1]));
        QCOMPARE(f.stack.count(), steps);
        f.editor.setFolded(ab[1], true);  // a track folds too (to a thin row), hiding nothing else
        QVERIFY(f.track(ab[1]).folded && !p.isHidden(ab[1]));
        QCOMPARE(f.stack.count(), steps);
    }

    void groupsArentArmed() {
        EditorFixture f;
        const QString a = tracks(f, {"A"})[0];
        const QString group = f.editor.groupTracks({a});
        f.editor.armTracks({a, group}, true);
        QVERIFY(f.track(a).armed && !f.track(group).armed);
    }

    void groupsAreSavedAndLoaded() {
        EditorFixture f;
        Project& p = f.project;
        const QStringList abc = tracks(f, {"A", "B", "C"});
        const QString group = f.editor.groupTracks({abc[0], abc[1]});
        const QString inner = f.editor.groupTracks({abc[1]});
        f.editor.setFolded(inner, true);
        f.editor.setFolded(abc[0], true);
        QJsonObject data = QJsonDocument::fromJson(QJsonDocument(projectToJson(p)).toJson()).object();
        Project loaded;
        loadInto(loaded, data);
        QVERIFY(names(loaded) == names(p));
        QVERIFY(loaded.track(inner).folded && loaded.track(group).isGroup());
        QVERIFY(loaded.track(abc[0]).folded && !loaded.track(group).folded);
        // A file whose groups don't hold together (edited by hand) loads with the strays out of them.
        QJsonArray saved = data["tracks"].toArray();
        QJsonObject stray = saved.takeAt(saved.size() - 1).toObject();  // C, put first and in the group that comes after it
        stray["parent"] = group;
        saved.insert(0, stray);
        data["tracks"] = saved;
        loadInto(loaded, data);
        QVERIFY((names(loaded)[0] == std::pair<QString, QString>{"C", ""}));
        QVERIFY(!treeProblem(loaded.tracks()));
    }

    // A new group is a little taller than a new track; heights saved are kept,
    // and a file without them gets the defaults (a group's for a group).
    void groupsAreALittleTallerThanTracks() {
        EditorFixture f;
        Project& p = f.project;
        const QStringList ab = tracks(f, {"A", "B"});
        const QString group = f.editor.groupTracks({ab[0]});
        QCOMPARE(f.track(ab[0]).height, kDefaultTrackHeight);
        QCOMPARE(f.track(group).height, kDefaultGroupHeight);
        QVERIFY(kDefaultGroupHeight > kDefaultTrackHeight);
        f.editor.setTrackHeight(ab[1], 80);  // (as saved before the defaults grew)
        QJsonObject data = QJsonDocument::fromJson(QJsonDocument(projectToJson(p)).toJson()).object();
        Project loaded;
        loadInto(loaded, data);
        QCOMPARE(loaded.track(ab[0]).height, kDefaultTrackHeight);
        QCOMPARE(loaded.track(ab[1]).height, 80);
        QCOMPARE(loaded.track(group).height, kDefaultGroupHeight);
        QJsonArray saved = data["tracks"].toArray();
        for (qsizetype i = 0; i < saved.size(); ++i) {
            QJsonObject t = saved[i].toObject();
            t.remove("height");
            saved[i] = t;
        }
        data["tracks"] = saved;
        loadInto(loaded, data);
        QCOMPARE(loaded.track(ab[0]).height, kDefaultTrackHeight);
        QCOMPARE(loaded.track(ab[1]).height, kDefaultTrackHeight);
        QCOMPARE(loaded.track(group).height, kDefaultGroupHeight);
    }

    // --- Cut, copy and paste of tracks ---

    void groupsCanBeCutCopiedAndPasted() {
        EditorFixture f;
        Project& p = f.project;
        const QStringList abc = tracks(f, {"A", "B", "C"});
        const QString a = abc[0], b = abc[1], c = abc[2];
        for (int i = 0; i < 3; ++i) {
            f.editor.commitClips("Add", {{abc[i], {test::audioClip(QStringLiteral("c%1").arg(i), i * 4.0, 2.0, "missing.wav")}}});
        }
        const QString group = f.editor.groupTracks({a, b});
        const auto copied = f.editor.copyTracks({group});
        QVERIFY(copied);
        QCOMPARE(copied->roots, QStringList{group});
        QCOMPARE(copied->tracks.size(), size_t(3));
        const QStringList pasted = f.editor.pasteTracks(*copied, c);  // after C
        QCOMPARE(p.tracks().size(), size_t(7));  // the group, A, B, C, then the copies
        QCOMPARE(pasted, QStringList{p.tracks()[4].id});
        QVERIFY(p.tracks()[4].isGroup() && !p.tracks()[4].parent);
        const auto inside = p.descendants(pasted[0]);
        QCOMPARE(inside.size(), size_t(2));
        for (const Track* t : inside) {
            QCOMPARE(t->clips.size(), size_t(1));
            QVERIFY(t->id != a && t->id != b);
        }
        QCOMPARE(f.stack.undoText(), QStringLiteral("Paste Track"));

        const auto cut = f.editor.cutTracks({group});
        QVERIFY(cut);
        QVERIFY(!p.hasTrack(group) && !p.hasTrack(a));
        QCOMPARE(p.tracks().size(), size_t(4));
        f.editor.pasteTracks(*cut, c);
        QCOMPARE(p.tracks().size(), size_t(7));
        QVERIFY(p.tracks()[1].isGroup());  // after C, before the first copy
        f.stack.undo();
        f.stack.undo();
        QVERIFY(p.hasTrack(group));
        QCOMPARE(ids(p.descendants(group)), (QStringList{a, b}));
        QVERIFY(!f.editor.copyTracks({kMaster}));  // (not a track of the arrangement)
    }

    void aRefusedCutCutsNothing() {
        EditorFixture f;
        Project& p = f.project;
        const QStringList ab = tracks(f, {"A", "B"});
        const QString group = f.editor.groupTracks({ab[0], ab[1]});
        f.editor.freezeTracks({{group, Freeze{"frozen.wav", 2.0, 120.0}}});
        // Cutting a track in a frozen group is refused: nothing is cut.
        QVERIFY(!f.editor.cutTracks({ab[0]}));
        QVERIFY(p.hasTrack(ab[0]));
        QVERIFY(!f.messages.isEmpty());
    }

    void movingTracksKeepsTheirOrder() {
        EditorFixture f;
        Project& p = f.project;
        const QStringList abcd = tracks(f, {"A", "B", "C", "D"});
        QVERIFY(f.editor.moveTracks({abcd[3], abcd[0]}, 2, {}));  // in their order, before C
        QCOMPARE(order(p), (QStringList{abcd[1], abcd[0], abcd[3], abcd[2]}));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Move Tracks"));
        QVERIFY(!f.editor.moveTracks({abcd[1]}, 0, {}));  // (where it is)
    }
};

QTEST_GUILESS_MAIN(TestEditorGroups)
#include "test_editor_groups.moc"
