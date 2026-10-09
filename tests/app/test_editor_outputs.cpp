// Tracks' outputs through the editor (Ableton's Audio To): into their group by
// default, the master past it, an audio track's input (Track In), a device's
// sidechain, or nowhere (Sends Only); each one undo step; cycles refused;
// outputs going into their groups when tracks go into another group (as in
// Ableton), when what they went into goes, or when they would close a cycle;
// copies going into copies; where an input from a track is tapped; saving and
// loading, and repairing what a project can't have.

#include "EditorFixture.h"
#include "TestSupport.h"

#include "io/Serialization.h"
#include "model/Errors.h"
#include "model/Routing.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

using namespace sub::app;
using test::EditorFixture;

namespace {

// A device to take a sidechain (the model doesn't ask whether it has a sidechain input).
QString device(EditorFixture& f, const QString& trackId) { return f.editor.addDevice(trackId, "utility"); }

QJsonObject trackJson(const QJsonObject& project, const QString& id) {
    for (const QJsonValue& t : project.value(QStringLiteral("tracks")).toArray()) {
        if (t.toObject().value(QStringLiteral("id")).toString() == id) return t.toObject();
    }
    return {};
}

}  // namespace

class TestEditorOutputs : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void anOutputAndItsUndo() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = f.editor.addAudioTrack(-1, "A");
        const QString bus = f.editor.addAudioTrack(-1, "Bus");
        QVERIFY(f.track(a).output.isDefault());
        QCOMPARE(p.outputTarget(a), std::optional<QString>());  // (the master)
        const int steps = f.stack.count();
        f.editor.setTrackOutput(a, Output::track(bus));
        QCOMPARE(f.track(a).output, Output::track(bus));
        QCOMPARE(p.outputTarget(a), std::optional<QString>(bus));
        QCOMPARE(f.stack.count(), steps + 1);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Change Track Output"));
        QVERIFY(feeds(routingGraph(p.tracks(), p.returns()), a, bus));
        f.editor.setTrackOutput(a, Output::none());
        QCOMPARE(f.track(a).output, Output::none());
        f.stack.undo();
        QCOMPARE(f.track(a).output, Output::track(bus));
        f.stack.undo();
        QVERIFY(f.track(a).output.isDefault());
        // The master from outside a group is its default.
        const int index = f.stack.index();
        f.editor.setTrackOutput(a, Output::master());
        QVERIFY(f.track(a).output.isDefault());
        QCOMPARE(f.stack.index(), index);  // (nothing changed)
        // From QML.
        QVERIFY(f.editor.trySetTrackOutput(a, QStringLiteral("track"), bus));
        QCOMPARE(f.track(a).output, Output::track(bus));
        QVERIFY(f.editor.trySetTrackOutput(a, QStringLiteral("none")));
        QCOMPARE(f.track(a).output, Output::none());
        QVERIFY(f.editor.trySetTrackOutput(a, QStringLiteral("group")));
        QVERIFY(f.track(a).output.isDefault());
        QVERIFY(!f.editor.trySetTrackOutput(a, QStringLiteral("elsewhere")));
        QVERIFY(!f.messages.isEmpty());
    }

    void whatAnOutputCanGoInto() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = f.editor.addAudioTrack(-1, "A");
        const QString b = f.editor.addAudioTrack(-1, "B");
        const QString midi = f.editor.addMidiTrack(-1, "Keys");
        const QString group = f.editor.groupTracks({b});
        const QString ret = f.editor.addReturnTrack();
        // Only an audio track's input (not a MIDI track's, a group's or a return's), not itself.
        for (const QString& target : {midi, group, ret, a, QStringLiteral("gone")}) {
            QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackOutput(a, Output::track(target)));
        }
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackOutput(a, Output::sidechain(QStringLiteral("gone"))));
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackOutput(kMaster, Output::none()));
        // Its own group stands for its default.
        f.editor.setTrackOutput(b, Output::track(group));
        QVERIFY(f.track(b).output.isDefault());
        // Past its group, into the master.
        f.editor.setTrackOutput(b, Output::master());
        QCOMPARE(f.track(b).output, Output::master());
        QVERIFY(!feeds(routingGraph(p.tracks(), p.returns()), b, group));
        // Cycles: a track into one that goes into it; a group into a track in it; a
        // device on itself, or on one it feeds.
        f.editor.setTrackOutput(a, Output::track(b));
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackOutput(b, Output::track(a)));
        f.editor.setTrackOutput(b, Output::group());
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackOutput(group, Output::track(b)));
        const QString onA = device(f, a);
        const QString onGroup = device(f, group);
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackOutput(a, Output::sidechain(onA)));
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackOutput(group, Output::sidechain(onA)));  // a -> b -> group
        f.editor.setTrackOutput(midi, Output::sidechain(onGroup));
        QCOMPARE(p.outputTarget(midi), std::optional<QString>(group));
        // A return's output, and a device on the master.
        const QString onMaster = device(f, kMaster);
        f.editor.setTrackOutput(ret, Output::sidechain(onMaster));
        QCOMPARE(f.track(ret).output, Output::sidechain(onMaster));
        QCOMPARE(p.outputTarget(ret), std::optional<QString>());
        f.editor.setTrackOutput(ret, Output::track(a));
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setSend(a, ret, 0.0));  // it feeds the return
        QVERIFY(f.messages.isEmpty());
    }

    void goingIntoAnotherGroupGoesIntoIt() {
        // As in Ableton: grouping tracks (or moving them into a group) sends
        // them into it, wherever they went; leaving a group, they keep theirs.
        EditorFixture f;
        Project& p = f.project;
        const QString a = f.editor.addAudioTrack(-1, "A");
        const QString b = f.editor.addAudioTrack(-1, "B");
        const QString bus = f.editor.addAudioTrack(-1, "Bus");
        f.editor.setTrackOutput(a, Output::track(bus));
        f.editor.setTrackOutput(b, Output::none());
        const QString group = f.editor.groupTracks({a, b});
        QVERIFY(f.track(a).output.isDefault() && f.track(b).output.isDefault());
        QCOMPARE(p.outputTarget(a), std::optional<QString>(group));
        f.stack.undo();  // (one step)
        QCOMPARE(f.track(a).output, Output::track(bus));
        QCOMPARE(f.track(b).output, Output::none());
        f.stack.redo();
        f.editor.setTrackOutput(a, Output::track(bus));
        QVERIFY(f.editor.moveTracks({a}, p.trackIndex(bus) + 1, QString()));  // out of the group
        QCOMPARE(f.track(a).output, Output::track(bus));
        // Within its group, it keeps its output.
        f.editor.setTrackOutput(b, Output::master());
        const QString c = f.editor.addAudioTrack(-1, "C");
        QVERIFY(f.editor.moveTracks({c}, p.trackIndex(b) + 1, group));
        QVERIFY(f.editor.moveTracks({b}, p.trackIndex(c) + 1, group));
        QCOMPARE(f.track(b).output, Output::master());
        // Out to the top, an output into the master is the default again.
        QVERIFY(f.editor.moveTracks({b}, static_cast<int>(p.tracks().size()), QString()));
        QCOMPARE(p.outputTarget(b), std::optional<QString>());
    }

    void movingAGroupDropsAnOutputThatWouldCloseACycle() {
        EditorFixture f;
        Project& p = f.project;
        const QString t = f.editor.addAudioTrack(-1, "T");
        const QString inner = f.editor.addAudioTrack(-1, "Inner");
        const QString group = f.editor.groupTracks({inner});
        f.editor.setTrackOutput(group, Output::track(t));  // the group into T
        // T going into the group would make T -> group -> T.
        QVERIFY(f.editor.moveTracks({t}, p.trackIndex(inner) + 1, group));
        QCOMPARE(p.track(t).parent, std::optional<QString>(group));
        QVERIFY(f.track(group).output.isDefault());
        QCOMPARE(f.stack.undoText(), QStringLiteral("Move Track"));
        f.stack.undo();
        QCOMPARE(f.track(group).output, Output::track(t));
        QVERIFY(!p.track(t).parent);
    }

    void whatAnOutputWentIntoGoing() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = f.editor.addAudioTrack(-1, "A");
        const QString b = f.editor.addAudioTrack(-1, "B");
        const QString bus = f.editor.addAudioTrack(-1, "Bus");
        const QString keyed = f.editor.addAudioTrack(-1, "Keyed");
        const QString comp = device(f, keyed);
        f.editor.setTrackOutput(a, Output::track(bus));
        f.editor.setTrackOutput(b, Output::sidechain(comp));
        // The device going: what went into it goes into its group (one step).
        f.editor.removeDevice(keyed, comp);
        QVERIFY(f.track(b).output.isDefault());
        QCOMPARE(f.stack.undoText(), QStringLiteral("Delete Device"));
        f.stack.undo();
        QCOMPARE(f.track(b).output, Output::sidechain(comp));
        // The track going (with its devices).
        f.editor.deleteTracks({bus, keyed});
        QVERIFY(f.track(a).output.isDefault() && f.track(b).output.isDefault());
        f.stack.undo();
        QCOMPARE(f.track(a).output, Output::track(bus));
        QCOMPARE(f.track(b).output, Output::sidechain(comp));
        // The device moving to another track keeps it going there, unless that closes a cycle.
        const QString other = f.editor.addAudioTrack(-1, "Other");
        QVERIFY(f.editor.moveDevicesToTrack(keyed, {comp}, other));
        QCOMPARE(f.track(b).output, Output::sidechain(comp));
        QCOMPARE(p.outputTarget(b), std::optional<QString>(other));
        f.editor.setTrackOutput(a, Output::track(b));  // a goes into b, which goes into the device
        QVERIFY(f.editor.moveDevicesToTrack(other, {comp}, a));  // onto a: b -> a -> b
        QVERIFY(f.track(b).output.isDefault());
        QVERIFY(f.messages.isEmpty());
    }

    void copiesGoIntoCopies() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = f.editor.addAudioTrack(-1, "A");
        const QString bus = f.editor.addAudioTrack(-1, "Bus");
        const QString outside = f.editor.addAudioTrack(-1, "Outside");
        const QString comp = device(f, bus);
        f.editor.setTrackOutput(a, Output::track(bus));
        f.editor.setTrackOutput(outside, Output::sidechain(comp));
        const QStringList copies = f.editor.duplicateTracks({a, bus});
        QCOMPARE(copies.size(), 2);
        QCOMPARE(f.track(copies[0]).output, Output::track(copies[1]));
        // A copy alone keeps going into the original.
        const QStringList one = f.editor.duplicateTracks({outside});
        QCOMPARE(f.track(one[0]).output, Output::sidechain(comp));
        // ... and into the copy of its device, copied with it.
        f.editor.setTrackOutput(a, Output::sidechain(comp));
        const QStringList both = f.editor.duplicateTracks({a, bus});
        const QString copiedComp = p.track(both[1]).devices.front().id;
        QVERIFY(copiedComp != comp);
        QCOMPARE(f.track(both[0]).output, Output::sidechain(copiedComp));
    }

    void anInputsTap() {
        EditorFixture f;
        const QString source = f.editor.addMidiTrack(-1, "Synth");
        const QString track = f.editor.addAudioTrack(-1, "Bounce");
        f.editor.setTrackInputTrack(track, source);
        QCOMPARE(f.track(track).inputTap, kPostFader);
        f.editor.setTrackInputTap(track, kPreFx);
        QCOMPARE(f.track(track).inputTap, kPreFx);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Change Track Input"));
        QVERIFY(f.editor.trySetTrackInputTap(track, kPreFader));
        QCOMPARE(f.track(track).inputTap, kPreFader);
        QVERIFY(!f.editor.trySetTrackInputTap(track, QStringLiteral("somewhere")));
        f.stack.undo();
        QCOMPARE(f.track(track).inputTap, kPreFx);
    }

    void savingAndLoading() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = f.editor.addAudioTrack(-1, "A");
        const QString bus = f.editor.addAudioTrack(-1, "Bus");
        const QString keyed = f.editor.addAudioTrack(-1, "Keyed");
        const QString inGroup = f.editor.addAudioTrack(-1, "In");
        const QString group = f.editor.groupTracks({inGroup});
        const QString comp = device(f, keyed);
        const QString ret = f.editor.addReturnTrack();
        f.editor.setTrackOutput(a, Output::track(bus));
        f.editor.setTrackOutput(keyed, Output::none());
        f.editor.setTrackOutput(inGroup, Output::master());
        f.editor.setTrackOutput(ret, Output::sidechain(comp));
        f.editor.setTrackInputTrack(bus, a);
        f.editor.setTrackInputTap(bus, kPreFader);
        const QJsonObject saved = projectToJson(p);
        QCOMPARE(saved.value(QStringLiteral("version")).toInt(), kProjectVersion);
        QVERIFY(!trackJson(saved, bus).contains(QStringLiteral("output")));  // (into its group: the default)
        QCOMPARE(trackJson(saved, a).value(QStringLiteral("output")).toObject(),
                 (QJsonObject{{QStringLiteral("to"), QStringLiteral("track")}, {QStringLiteral("track"), bus}}));
        QCOMPARE(trackJson(saved, bus).value(QStringLiteral("input_tap")).toString(), kPreFader);
        Project loaded;
        loadInto(loaded, saved);
        for (const QString& id : {a, bus, keyed, inGroup, group}) {
            QCOMPARE(loaded.track(id).output, p.track(id).output);
            QCOMPARE(loaded.track(id).inputTap, p.track(id).inputTap);
        }
        QCOMPARE(loaded.track(ret).output, Output::sidechain(comp));

        // Older files: every track into its group, inputs after the fader.
        QJsonObject older = saved;
        QJsonArray tracks;
        for (const QJsonValue& t : saved.value(QStringLiteral("tracks")).toArray()) {
            QJsonObject track = t.toObject();
            track.remove(QStringLiteral("output"));
            track.remove(QStringLiteral("input_tap"));
            tracks.append(track);
        }
        older.insert(QStringLiteral("tracks"), tracks);
        older.insert(QStringLiteral("version"), 20);
        Project old;
        loadInto(old, older);
        QVERIFY(old.track(a).output.isDefault());
        QCOMPARE(old.track(bus).inputTap, kPostFader);
    }

    void loadingRepairsOutputsAProjectCantHave() {
        // Into a track that isn't there or isn't an audio track, into a device
        // that isn't there, into its own group, or closing a cycle: into its group.
        EditorFixture f;
        Project& p = f.project;
        const QString a = f.editor.addAudioTrack(-1, "A");
        const QString b = f.editor.addAudioTrack(-1, "B");
        const QString midi = f.editor.addMidiTrack(-1, "Keys");
        const QString inGroup = f.editor.addAudioTrack(-1, "In");
        const QString group = f.editor.groupTracks({inGroup});
        QJsonObject data = projectToJson(p);
        const auto into = [](const QString& to, const QString& key = {}, const QString& id = {}) {
            QJsonObject output{{QStringLiteral("to"), to}};
            if (!key.isEmpty()) output.insert(key, id);
            return output;
        };
        const QMap<QString, QJsonObject> outputs{
            {a, into("track", "track", b)},        // kept (the first)
            {b, into("track", "track", a)},        // a cycle with the one before
            {midi, into("track", "track", group)}, // not an audio track
            {inGroup, into("track", "track", group)},  // its own group
            {group, into("sidechain", "device", "gone")},
        };
        QJsonArray tracks;
        for (const QJsonValue& t : data.value(QStringLiteral("tracks")).toArray()) {
            QJsonObject track = t.toObject();
            const QString id = track.value(QStringLiteral("id")).toString();
            if (outputs.contains(id)) track.insert(QStringLiteral("output"), outputs.value(id));
            tracks.append(track);
        }
        data.insert(QStringLiteral("tracks"), tracks);
        Project loaded;
        loadInto(loaded, data);
        QCOMPARE(loaded.track(a).output, Output::track(b));
        for (const QString& id : {b, midi, inGroup, group}) QVERIFY2(loaded.track(id).output.isDefault(), qPrintable(id));
    }
};

QTEST_GUILESS_MAIN(TestEditorOutputs)
#include "test_editor_outputs.moc"
