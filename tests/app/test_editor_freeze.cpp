// Freezing through the editor: what can be frozen, freezing and unfreezing
// (undoable), what a frozen track (and what is in a frozen group) refuses,
// flattening, and saving.

#include "EditorFixture.h"
#include "TestSupport.h"

#include "io/Serialization.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace sub::app;
using test::EditorFixture;
using test::env;
namespace autom = sub::app::automation;

namespace {

const Freeze kFreeze{QStringLiteral("frozen.wav"), 2.0, 120.0};

// An audio track with one clip ("<name>c", beat 0, a second long).
QString audioTrack(EditorFixture& f, const QString& name = QStringLiteral("A")) {
    const QString track = f.editor.addAudioTrack(-1, name);
    f.editor.commitClips("Add", {{track, {test::audioClip(name + "c", 0.0, 1.0)}}});
    return track;
}

QStringList clipIds(const Track& track) {
    QStringList ids;
    for (const Clip& c : track.clips) ids.append(c.id);
    return ids;
}

}  // namespace

class TestEditorFreeze : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void whatCanBeFrozen() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f, "A");
        const QString b = audioTrack(f, "B");
        QVERIFY(!p.freezeProblem(a));
        QVERIFY(p.freezeProblem(kMaster));
        // A sidechain taking A's signal after its fader or before it: in the frozen audio.
        const QString comp = f.editor.addDevice(b, "compressor");
        f.editor.setDeviceSidechain(b, comp, Sidechain{a, kPreFader});
        QVERIFY(!p.freezeProblem(a));
        // Before its devices (or after one of them): not.
        f.editor.setDeviceSidechain(b, comp, Sidechain{a, kPreFx});
        QVERIFY(p.freezeProblem(a)->contains("sidechain"));
        QVERIFY(f.editor.freezeTracks({{a, kFreeze}}).isEmpty());
        f.editor.setDeviceSidechain(b, comp, std::nullopt);
        f.editor.freezeTracks({{a, kFreeze}});
        QVERIFY(p.freezeProblem(a)->contains("frozen already"));
    }

    void freezingAndUnfreezingAreUndoable() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f);
        f.editor.armTracks({a}, true);
        QCOMPARE(f.editor.freezeTracks({{a, kFreeze}}), QStringList{a});
        QVERIFY(f.track(a).frozen == kFreeze && p.isFrozen(a) && p.frozenBy(a) == a);
        QVERIFY(!f.track(a).armed);  // a frozen track doesn't record
        QCOMPARE(f.stack.undoText(), QStringLiteral("Freeze Track"));
        f.stack.undo();
        QVERIFY(!f.track(a).frozen);
        f.stack.redo();
        QCOMPARE(f.editor.unfreezeTracks({a}), QStringList{a});
        QVERIFY(!f.track(a).frozen);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Unfreeze Track"));
        f.stack.undo();
        QVERIFY(f.track(a).frozen == kFreeze);
    }

    void aFrozenTrackRefusesChangesToItsClipsDevicesAndTheirAutomation() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f);
        const QString device = f.editor.addDevice(a, "utility");
        f.editor.freezeTracks({{a, kFreeze}});
        const int count = f.stack.count();
        f.editor.deleteClips({{a, "Ac"}});
        f.editor.addDevice(a, "utility");
        f.editor.removeDevice(a, device);
        f.editor.setDeviceEnabled(a, device, false);
        f.editor.setDeviceParam(a, device, "gain", 0.5);
        f.editor.setEnvelope(a, autom::deviceKey(device, "gain"), env({{0.0, 0.5}}));
        QCOMPARE(f.stack.count(), count);
        QCOMPARE(clipIds(f.track(a)), QStringList{"Ac"});
        QCOMPARE(f.track(a).devices.size(), size_t(1));
        QVERIFY(!f.messages.isEmpty() && f.messages.back().contains("frozen"));
        // Its mixer stays live, and so does its automation.
        f.editor.setTrackParam(a, TrackField::VolumeDb, -6.0);
        f.editor.setEnvelope(a, autom::kMixerVolume, env({{0.0, 0.5}}));
        QCOMPARE(f.track(a).volumeDb, -6.0);
        QVERIFY(!p.envelope(a, autom::kMixerVolume).empty());
        // Nor is it armed.
        f.editor.armTracks({a}, true);
        QVERIFY(!f.track(a).armed);
        // Nor does it take clips.
        QVERIFY(f.editor.addClips(a, 4.0, {{"b.wav", 1.0}}).isEmpty());
        QVERIFY(f.messages.back().contains("unfreeze it to change its clips"));
    }

    void whatIsInAFrozenGroupCantChange() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f, "A");
        const QString b = audioTrack(f, "B");
        const QString outside = audioTrack(f, "C");
        const QString group = f.editor.groupTracks({a, b});
        const QString ret = f.editor.addReturnTrack();
        f.editor.freezeTracks({{group, kFreeze}});
        QVERIFY(p.isFrozen(a) && p.frozenBy(a) == group && !f.track(a).frozen);
        QVERIFY(p.freezeProblem(a)->contains("frozen"));
        const int count = f.stack.count();
        f.editor.commitClips("Edit", {{a, {}}});  // its clips
        f.editor.setEnvelope(a, autom::kMixerVolume, env({{0.0, 0.5}}));  // its mixer's automation: baked
        QVERIFY(!f.editor.moveTracks({outside}, 1, group));  // into the group
        QVERIFY(!f.editor.moveTracks({a}, static_cast<int>(p.tracks().size()), {}));  // out of it
        f.editor.deleteTracks({a});
        QCOMPARE(f.stack.count(), count);
        QVERIFY(!p.tracks()[0].parent && p.tracks()[1].parent == group && p.tracks()[2].parent == group);
        QVERIFY(f.editor.laneFrozen(a, autom::kMixerVolume) && !f.editor.laneFrozen(a, autom::sendKey(ret)));
        QVERIFY(!f.editor.laneFrozen(group, autom::kMixerVolume));  // (the group's own mixer: live)
        // A send from what is in it to a return outside it stays live.
        f.editor.setSend(a, ret, -6.0);
        QVERIFY(!f.track(a).sends.isEmpty());
        // A new track after one in it goes after the group, not into it.
        const InsertionPoint point = f.editor.insertionPoint(b);
        const QString added = f.editor.addAudioTrack(*point.index, {}, point.parent);
        QVERIFY(!f.track(added).parent);
        QCOMPARE(p.trackIndex(added), 3);
        // Nor does a group of tracks in it.
        QVERIFY(f.editor.groupTracks({a}).isEmpty());
        // The group itself goes, with what is in it.
        f.editor.deleteTracks({group});
        QVERIFY(!p.hasTrack(a));
    }

    void ungroupingAFrozenGroup() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f, "A");
        const QString group = f.editor.groupTracks({a});
        f.editor.freezeTracks({{group, kFreeze}});
        f.editor.ungroup({group});  // it goes: what was in it plays again
        QVERIFY(!p.hasTrack(group) && !p.isFrozen(a));
    }

    void aGroupAndATrackInItFreezeAsTheGroup() {
        EditorFixture f;
        const QString a = audioTrack(f, "A");
        const QString group = f.editor.groupTracks({a});
        QCOMPARE(f.editor.freezeTracks({{group, kFreeze}, {a, kFreeze}}), QStringList{group});
        QVERIFY(!f.track(a).frozen);
        QVERIFY(f.editor.unfreezeTracks({a}).isEmpty());  // (its group holds it)
    }

    // --- Flattening ---

    void flatteningAFrozenMidiTrack() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addMidiTrack(-1, "Keys");
        const QString synth = f.track(track).devices[0].id;
        f.editor.commitClips("Add", {{track, {Clip::midi("m", "m", 0.0, 4.0, 0.0, {Note{60, 0.0, 1.0}})}}});
        f.editor.setEnvelope(track, autom::deviceKey(synth, "volume"), env({{0.0, 0.3}}));
        f.editor.setEnvelope(track, autom::kMixerPan, env({{0.0, 0.2}}));
        f.editor.setTrackParam(track, TrackField::VolumeDb, -3.0);
        f.editor.showAutomation(track, autom::deviceKey(synth, "volume"));
        QVERIFY(p.flattenProblem(track));  // not frozen yet
        f.editor.freezeTracks({{track, kFreeze}});
        QCOMPARE(f.editor.flattenTracks({track}), QStringList{track});
        const Track& flat = f.track(track);
        QVERIFY(flat.isAudio() && !flat.frozen && flat.devices.empty() && flat.volumeDb == -3.0);
        QCOMPARE(flat.clips.size(), size_t(1));
        QVERIFY(flat.clips[0].path == kFreeze.path && flat.clips[0].isWarped());
        QCOMPARE(flat.clips[0].name, QStringLiteral("Keys"));
        QCOMPARE(flat.automation.keys(), QList<QString>{autom::kMixerPan});  // its devices' automation went with them
        QVERIFY(!flat.automationView.key);  // (it showed a device's)
        QCOMPARE(f.stack.undoText(), QStringLiteral("Flatten Track"));
        f.stack.undo();
        const Track& back = f.track(track);
        QVERIFY(back.isMidi() && back.frozen == kFreeze && back.devices[0].id == synth);
        QCOMPARE(back.automation.size(), 2);
    }

    void onlyFrozenAudioAndMidiTracksFlatten() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f);
        const QString group = f.editor.groupTracks({a});
        f.editor.freezeTracks({{group, kFreeze}});
        QVERIFY(p.flattenProblem(group));
        QVERIFY(f.editor.flattenTracks({group, a}).isEmpty());
    }

    void frozenTracksAreSaved() {
        EditorFixture f;
        Project& p = f.project;
        test::TempDir dir;
        const QString a = audioTrack(f);
        const QString ret = f.editor.addReturnTrack();
        const Freeze frozen{dir.path("Freeze/a.wav"), 3.5, 98.0};
        f.editor.freezeTracks({{a, frozen}, {ret, kFreeze}});
        const QJsonObject data =
            QJsonDocument::fromJson(QJsonDocument(projectToJson(p, dir.path("x.gilproj"))).toJson()).object();
        QCOMPARE(data["tracks"].toArray()[0].toObject()["frozen"].toObject()["relative_path"].toString(),
                 QStringLiteral("Freeze/a.wav"));
        Project loaded;
        loadInto(loaded, data, dir.path("x.gilproj"));
        QVERIFY(loaded.track(a).frozen == frozen && loaded.track(ret).frozen == kFreeze);
    }

    void automationDoesntMoveWithoutTheFrozenClipsUnderIt() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f);
        f.editor.setEnvelope(a, autom::kMixerVolume, env({{0.5, 0.2}, {1.5, 0.8}}));
        f.editor.freezeTracks({{a, kFreeze}});
        const Envelope envelope = p.envelope(a, autom::kMixerVolume);
        f.editor.moveClips({{a, "Ac"}}, 4.0);  // (its mixer's automation would go with them)
        QCOMPARE(f.track(a).clips[0].startBeat, 0.0);
        QVERIFY(p.envelope(a, autom::kMixerVolume) == envelope);
        QVERIFY(f.messages.back().contains("frozen"));
    }

    void aFrozenTracksDevicesDontMove() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = audioTrack(f, "A");
        const QString b = audioTrack(f, "B");
        const QString device = f.editor.addDevice(a, "utility");
        f.editor.setEnvelope(a, autom::deviceKey(device, "gain"), env({{0.0, 0.5}}));
        f.editor.freezeTracks({{a, kFreeze}});
        const int count = f.stack.count();
        QVERIFY(!f.editor.moveDevicesToTrack(a, {device}, b));
        QVERIFY(f.editor.groupDevices(a, {device}).isEmpty());
        QCOMPARE(f.stack.count(), count);
        QVERIFY(p.hasDevice(a, device) && !p.envelope(a, autom::deviceKey(device, "gain")).empty());
    }
};

QTEST_GUILESS_MAIN(TestEditorFreeze)
#include "test_editor_freeze.moc"
