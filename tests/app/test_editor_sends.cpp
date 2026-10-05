// Return tracks and sends through the editor: making and deleting returns
// (with the sends into them, as one undo step), sends' levels and taps,
// cycles among returns, solo, and saving (the editor parts of
// tests/test_sends_model.py).

#include "EditorFixture.h"
#include "TestSupport.h"

#include "io/Serialization.h"
#include "model/Errors.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>

using namespace sub::app;
using test::EditorFixture;
using test::env;
namespace autom = sub::app::automation;

namespace {

QStringList ids(const std::vector<const Track*>& tracks) {
    QStringList result;
    for (const Track* t : tracks) result.append(t->id);
    return result;
}

QStringList ids(const std::vector<Track>& tracks) {
    QStringList result;
    for (const Track& t : tracks) result.append(t.id);
    return result;
}

}  // namespace

class TestEditorSends : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void returnsAreTracksApartFromTheArrangement() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack(-1, "Vox");
        const QString a = f.editor.addReturnTrack();
        const QString b = f.editor.addReturnTrack();
        QCOMPARE(p.returns()[0].name, QStringLiteral("A Return"));
        QCOMPARE(p.returns()[1].name, QStringLiteral("B Return"));
        QVERIFY(f.track(a).isReturn() && !f.track(a).hasInput());
        QCOMPARE(ids(p.tracks()), QStringList{track});
        QVERIFY(p.hasOwner(a) && !p.hasTrack(a));
        QCOMPARE(p.returnLetter(b), QStringLiteral("B"));
        QCOMPARE(p.owners(), (QStringList{track, a, b, kMaster}));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Insert Return Track"));
        f.stack.undo();
        QCOMPARE(ids(p.returns()), QStringList{a});
    }

    void sendsAndTheirUndo() {
        EditorFixture f;
        const QString track = f.editor.addAudioTrack();
        const QString ret = f.editor.addReturnTrack();
        f.editor.setSend(track, ret, -12.0);
        QVERIFY((f.track(track).sends == SendMap{{ret, Send{-12.0, false}}}));
        f.editor.setSend(track, ret, std::nullopt, true);
        QVERIFY((f.track(track).sends == SendMap{{ret, Send{-12.0, true}}}));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Toggle Pre-Fader Send"));
        // A knob drag is one undo step.
        for (double level : {-10.0, -8.0, -6.0}) f.editor.setSend(track, ret, level, std::nullopt, "drag");
        QCOMPARE(f.track(track).sends.value(ret).levelDb, -6.0);
        f.stack.undo();
        QVERIFY((f.track(track).sends.value(ret) == Send{-12.0, true}));
        f.stack.undo();
        f.stack.undo();
        QVERIFY(f.track(track).sends.isEmpty());
        f.editor.setSend(track, ret, 20.0);  // held to the fader's range
        QCOMPARE(f.track(track).sends.value(ret).levelDb, autom::kMaxVolumeDb);
        f.editor.removeSend(track, ret);
        QVERIFY(f.track(track).sends.isEmpty());
        // From QML.
        QVERIFY(f.editor.trySetSendLevel(track, ret, -3.0));
        QVERIFY(f.editor.trySetSendPreFader(track, ret, true));
        QVERIFY((f.track(track).sends.value(ret) == Send{-3.0, true}));
    }

    void cyclesAmongReturnsAreRefused() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = f.editor.addReturnTrack();
        const QString b = f.editor.addReturnTrack();
        const QString c = f.editor.addReturnTrack();
        const QString group = f.editor.addAudioTrack();
        f.editor.setSend(a, b, 0.0);
        f.editor.setSend(b, c, 0.0);
        QVERIFY(p.wouldCycle(c, a) && p.wouldCycle(a, a));
        QVERIFY(!p.wouldCycle(a, c) && !p.wouldCycle(group, a));
        QVERIFY(p.sendTargets(c).empty());
        QCOMPARE(ids(p.sendTargets(b)), QStringList{c});
        QCOMPARE(ids(p.sendTargets(group)), (QStringList{a, b, c}));
        const std::vector<std::pair<QString, QString>> refused{{c, a}, {b, a}, {a, a}, {group, group}, {kMaster, a}};
        for (const auto& [from, to] : refused) QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setSend(from, to, 0.0));
        QVERIFY(!f.editor.trySetSendLevel(c, a, 0.0));
        QCOMPARE(f.messages.size(), 1);
    }

    void deletingAReturnTakesTheSendsToItAlong() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QString a = f.editor.addReturnTrack();
        const QString b = f.editor.addReturnTrack();
        f.editor.setSend(track, a, -3.0);
        f.editor.setSend(track, b, -6.0);
        f.editor.setSend(b, a, 0.0);
        const QString key = autom::sendKey(a);
        f.editor.setEnvelope(track, key, env({{0.0, 0.5}}));
        f.editor.deleteTracks({a});
        QCOMPARE(ids(p.returns()), QStringList{b});
        QVERIFY((f.track(track).sends == SendMap{{b, Send{-6.0}}}));
        QVERIFY(f.track(b).sends.isEmpty());
        QVERIFY(!f.track(track).automation.contains(key));
        QCOMPARE(f.stack.undoText(), QStringLiteral("Delete Return Track"));
        f.stack.undo();  // one step brings it all back
        QCOMPARE(ids(p.returns()), (QStringList{a, b}));
        QVERIFY((f.track(track).sends == SendMap{{a, Send{-3.0}}, {b, Send{-6.0}}}));
        QVERIFY((f.track(b).sends == SendMap{{a, Send{0.0}}}));
        QVERIFY(p.envelope(track, key) == env({{0.0, 0.5}}));
        // With tracks: one step too.
        f.editor.deleteTracks({track, b});
        QVERIFY(p.tracks().empty());
        QCOMPARE(ids(p.returns()), QStringList{a});
        QCOMPARE(f.stack.undoText(), QStringLiteral("Delete Tracks"));
    }

    void soloTakesReturnsIn() {
        EditorFixture f;
        const QString track = f.editor.addAudioTrack();
        const QString ret = f.editor.addReturnTrack();
        f.editor.soloTracks({track}, true);
        f.editor.soloTracks({ret}, true, true);  // the others are unsoloed, the tracks too
        QVERIFY(f.track(ret).solo && !f.track(track).solo);
    }

    void returnsAndSendsAreSavedAndLoaded() {
        EditorFixture f;
        Project& p = f.project;
        const QString track = f.editor.addAudioTrack();
        const QString group = f.editor.groupTracks({track});
        const QString a = f.editor.addReturnTrack();
        const QString b = f.editor.addReturnTrack();
        f.editor.setSend(track, a, -6.0, true);
        f.editor.setSend(group, b, -3.0);
        f.editor.setSend(a, b, -1.5);
        f.editor.setTrackParam(b, TrackField::VolumeDb, -2.0);
        f.editor.setEnvelope(track, autom::sendKey(a), env({{1.0, 0.25}}));
        QJsonObject data = QJsonDocument::fromJson(QJsonDocument(projectToJson(p)).toJson()).object();
        Project loaded;
        loadInto(loaded, data);
        QCOMPARE(ids(loaded.returns()), ids(p.returns()));
        for (std::size_t i = 0; i < p.returns().size(); ++i) {
            QCOMPARE(loaded.returns()[i].name, p.returns()[i].name);
            QCOMPARE(loaded.returns()[i].kind, kReturnKind);
        }
        QVERIFY((loaded.track(track).sends == SendMap{{a, Send{-6.0, true}}}));
        QVERIFY((loaded.track(group).sends == SendMap{{b, Send{-3.0}}}));
        QVERIFY((loaded.track(a).sends == SendMap{{b, Send{-1.5}}}));
        QCOMPARE(loaded.track(b).volumeDb, -2.0);
        QVERIFY(loaded.envelope(track, autom::sendKey(a)) == env({{1.0, 0.25}}));
    }

    void soloingIsNotUndone() {
        EditorFixture f;
        const QString a = f.editor.addAudioTrack();
        const QString b = f.editor.addAudioTrack();
        const int steps = f.stack.count();
        f.editor.soloTracks({a}, true, true);
        f.editor.setTrackParam(b, TrackField::Solo, true);
        QVERIFY(f.track(a).solo && f.track(b).solo);
        QCOMPARE(f.stack.count(), steps);
        f.editor.soloTracks({b}, true, true);  // the others unsoloed
        QVERIFY(!f.track(a).solo);
        QCOMPARE(f.stack.count(), steps);
    }

    void severalFadersAtOnce() {
        EditorFixture f;
        const QString a = f.editor.addAudioTrack();
        const QString b = f.editor.addAudioTrack();
        for (double db : {-3.0, -6.0, -100.0}) f.editor.setTracksParam({{a, db}, {b, db / 2}}, TrackField::VolumeDb, "drag");
        QCOMPARE(f.track(a).volumeDb, autom::kMinVolumeDb);  // held to the faders' range
        QCOMPARE(f.track(b).volumeDb, -50.0);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Change Volume"));
        f.stack.undo();  // one drag: one step
        QCOMPARE(f.track(a).volumeDb, 0.0);
        QCOMPARE(f.track(b).volumeDb, 0.0);
        f.editor.setTracksParam({{a, 3.0}}, TrackField::Pan);  // (one: as setTrackParam)
        QCOMPARE(f.track(a).pan, 1.0);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Change Pan"));
    }
};

QTEST_GUILESS_MAIN(TestEditorSends)
#include "test_editor_sends.moc"
