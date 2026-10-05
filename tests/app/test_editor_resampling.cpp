// Resampling through the editor: an audio track taking its input from another
// track's output (a track, a group, a return) or the master's; cycles refused,
// inputs that would close one dropped when tracks move into groups, a source
// going takes the inputs from it along (one undo step), and saving (the editor
// parts of tests/test_resampling_model.py).

#include "EditorFixture.h"
#include "TestSupport.h"

#include "io/Serialization.h"
#include "model/Errors.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QTest>

using namespace sub::app;
using test::EditorFixture;

namespace {

QStringList ids(const std::vector<const Track*>& tracks) {
    QStringList result;
    for (const Track* t : tracks) result.append(t->id);
    return result;
}

std::optional<QString> some(const QString& id) { return id; }

}  // namespace

class TestEditorResampling : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void anInputFromATrackAndItsUndo() {
        EditorFixture f;
        Project& p = f.project;
        const QString source = f.editor.addMidiTrack(-1, "Synth");
        const QString track = f.editor.addAudioTrack(-1, "Bounce");
        f.editor.setTrackInput(track, {0, 1});
        const int steps = f.stack.count();
        f.editor.setTrackInputTrack(track, source);
        QVERIFY(f.track(track).input.empty() && f.track(track).inputTrack == some(source) && f.track(track).hasInput());
        QCOMPARE(f.stack.count(), steps + 1);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Change Track Input"));
        f.editor.setTrackInput(track, {2});  // back to the device's channels
        QVERIFY(f.track(track).input == std::vector<int>{2} && !f.track(track).inputTrack);
        f.stack.undo();
        QVERIFY(f.track(track).input.empty() && f.track(track).inputTrack == some(source));
        f.stack.undo();
        QVERIFY((f.track(track).input == std::vector<int>{0, 1}) && !f.track(track).inputTrack);
        f.editor.setTrackInputTrack(track, kMaster);  // resampling
        QVERIFY(f.track(track).inputTrack == some(kMaster));
        QCOMPARE(p.inputName(kMaster), QStringLiteral("Resampling"));
        f.editor.setTrackInputTrack(track, std::nullopt);
        QVERIFY(f.track(track).input.empty() && !f.track(track).inputTrack && !f.track(track).hasInput());
        QCOMPARE(p.inputName(source), QStringLiteral("Synth"));
        QCOMPARE(ids(p.inputSources(track)), QStringList{source});
        // From QML.
        QVERIFY(f.editor.trySetTrackInputTrack(track, source));
        QVERIFY(f.track(track).inputTrack == some(source));
        QVERIFY(f.editor.trySetTrackInput(track, {1}));
        QVERIFY(f.track(track).input == std::vector<int>{1} && !f.track(track).inputTrack);
        QVERIFY(!f.editor.trySetTrackInput(track, {0, 1, 2}));
        QVERIFY(f.editor.trySetTrackInputTrack(track, {}));
        QVERIFY(f.track(track).input.empty() && !f.track(track).inputTrack);
    }

    void whatCanBeASource() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = f.editor.addAudioTrack();
        const QString b = f.editor.addAudioTrack();
        const QString midi = f.editor.addMidiTrack();
        const QString group = f.editor.groupTracks({b});
        const QString ret = f.editor.addReturnTrack();
        f.editor.setSend(a, ret, 0.0);
        QCOMPARE(ids(p.inputSources(b)), (QStringList{a, group, midi, ret}));
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackInputTrack(b, b));  // itself
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackInputTrack(b, group));  // its own group
        QVERIFY(p.inputWouldCycle(b, group) && p.inputWouldCycle(a, ret));
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackInputTrack(a, ret));  // a return it sends to
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackInputTrack(midi, a));  // MIDI tracks take MIDI
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackInputTrack(group, kMaster));  // groups record nothing
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackInputTrack(a, QStringLiteral("nothing")));
        f.editor.setTrackInputTrack(b, ret);
        QVERIFY(p.wouldCycle(b, ret));  // a send closes cycles through inputs too: b -> ret -> b
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setSend(b, ret, 0.0));
        QVERIFY(p.inputWouldCycle(a, b));  // a -> ret -> b
        QVERIFY(!p.inputWouldCycle(b, a) && !p.inputWouldCycle(b, kMaster));
        QVERIFY(p.sendTargets(b).empty());
    }

    // In the group it takes its input from (or a group fed by it), a track's
    // input would close a cycle: it goes, in the same undo step.
    void movingATrackIntoItsSourceDropsTheInput() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = f.editor.addAudioTrack();
        const QString b = f.editor.addAudioTrack();
        const QString group = f.editor.groupTracks({a});
        f.editor.setTrackInputTrack(b, group);
        const int steps = f.stack.count();
        QVERIFY(f.editor.moveTracks({b}, p.trackIndex(a) + 1, group));
        QVERIFY(f.track(b).parent == some(group) && !f.track(b).inputTrack);
        QCOMPARE(f.stack.count(), steps + 1);
        f.stack.undo();
        QVERIFY(!f.track(b).parent && f.track(b).inputTrack == some(group));
        // Out of the group, or into another one, it keeps its input.
        const QString other = f.editor.groupTracks({b});
        QVERIFY(f.track(b).parent == some(other) && f.track(b).inputTrack == some(group));
    }

    void aSourceGoingTakesTheInputsFromIt() {
        EditorFixture f;
        Project& p = f.project;
        const QString source = f.editor.addAudioTrack();
        const QString group = f.editor.groupTracks({source});
        const QString ret = f.editor.addReturnTrack();
        const QString a = f.editor.addAudioTrack();
        const QString b = f.editor.addAudioTrack();
        const QString c = f.editor.addAudioTrack();
        f.editor.setTrackInputTrack(a, source);
        f.editor.setTrackInputTrack(b, group);
        f.editor.setTrackInputTrack(c, ret);
        const int steps = f.stack.count();
        f.editor.deleteTracks({source});
        QVERIFY(!f.track(a).inputTrack && f.track(b).inputTrack == some(group));
        QCOMPARE(f.stack.count(), steps + 1);
        f.stack.undo();
        QVERIFY(p.hasTrack(source) && f.track(a).inputTrack == some(source));
        f.editor.ungroup({group});
        QVERIFY(!f.track(b).inputTrack && !f.track(source).parent);
        f.stack.undo();
        QVERIFY(f.track(b).inputTrack == some(group));
        f.editor.deleteTracks({group});  // with what is in it
        QVERIFY(!f.track(a).inputTrack && !f.track(b).inputTrack);
        f.editor.deleteTracks({ret});
        QVERIFY(!f.track(c).inputTrack);
        f.stack.undo();
        f.stack.undo();
        QVERIFY(f.track(a).inputTrack == some(source) && f.track(b).inputTrack == some(group) &&
                f.track(c).inputTrack == some(ret));
    }

    void inputsAreSavedAndLoaded() {
        EditorFixture f;
        Project& p = f.project;
        const QString a = f.editor.addAudioTrack();
        const QString b = f.editor.addAudioTrack();
        const QString c = f.editor.addAudioTrack();
        const QString ret = f.editor.addReturnTrack();
        f.editor.setTrackInputTrack(a, c);  // a track listed after it
        f.editor.setTrackInputTrack(b, kMaster);
        f.editor.setTrackInputTrack(c, ret);
        const QJsonObject data = projectToJson(p);
        QStringList saved;
        for (const QJsonValue& t : data["tracks"].toArray()) saved.append(t.toObject()["input_track"].toString());
        QCOMPARE(saved, (QStringList{c, kMaster, ret}));
        Project loaded;
        loadInto(loaded, data);
        QVERIFY(loaded.track(a).inputTrack == some(c) && loaded.track(b).inputTrack == some(kMaster) &&
                loaded.track(c).inputTrack == some(ret));
    }
};

QTEST_GUILESS_MAIN(TestEditorResampling)
#include "test_editor_resampling.moc"
