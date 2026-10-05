// Automation through the editor: undoable envelope edits, range edits across
// lanes, deleted devices' automation, touched parameters, the lanes shown,
// saving, and automation moving (or copied) with clips unless it is locked
// (the editor parts of tests/test_automation_model.py).

#include "EditorFixture.h"
#include "TestSupport.h"

#include "io/Serialization.h"
#include "model/Errors.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTest>

#include <cmath>

using namespace sub::app;
using test::EditorFixture;
using test::env;
namespace autom = sub::app::automation;

namespace {

bool near(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }

double valueAt(const Envelope& points, double beat) { return autom::valueAt(points, beat).value_or(-1.0); }

std::vector<double> beatsOf(const Envelope& points) {
    std::vector<double> beats;
    for (const AutomationPoint& point : points) beats.push_back(point.beat);
    return beats;
}

Envelope twoTracksPan() { return env({{0.0, 0.5}, {5.0, 0.0}, {7.0, 1.0}, {12.0, 0.5}}); }

// Two MIDI tracks; on the first, a clip from beat 4 to 8 and under it: pan
// from 5 to 7, the synth cutoff's only point; volume with nothing under it.
struct TwoTracks {
    QString a;
    QString b;
    QString clip;
    QString synth;  // the synth's cutoff's key
};

TwoTracks twoTracks(EditorFixture& f) {
    TwoTracks t;
    t.a = f.editor.addMidiTrack();
    t.b = f.editor.addMidiTrack();
    t.clip = f.editor.addMidiClip(t.a, 4.0, 4.0)->clipId;
    t.synth = autom::deviceKey(f.track(t.a).devices[0].id, "cutoff");
    f.editor.setEnvelope(t.a, autom::kMixerPan, twoTracksPan());
    f.editor.setEnvelope(t.a, t.synth, env({{6.0, 0.25}}));
    f.editor.setEnvelope(t.a, autom::kMixerVolume, env({{0.0, 0.8}, {2.0, 0.6}}));  // nothing under the clip
    return t;
}

}  // namespace

class TestEditorAutomation : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void envelopeEditsAreUndoable() {
        EditorFixture f;
        const QString track = f.editor.addMidiTrack();
        QSignalSpy changes(&f.project, &Project::automationChanged);
        f.editor.addAutomationPoint(track, autom::kMixerVolume, 1.0, 0.5);
        const int index = f.editor.addAutomationPoint(track, autom::kMixerVolume, 3.0, 0.8);
        QCOMPARE(index, 1);
        QCOMPARE(changes.size(), 2);
        for (const auto& change : changes) {
            QCOMPARE(change[0].toString(), track);
            QCOMPARE(change[1].toString(), autom::kMixerVolume);
        }
        const Envelope original = f.project.envelope(track, autom::kMixerVolume);
        for (int step = 1; step < 5; ++step) {  // one drag: one undo step
            f.editor.moveAutomationPoints(track, autom::kMixerVolume, original, {1}, 0.25 * step, -0.1 * step, "gesture");
        }
        const AutomationPoint moved = f.project.envelope(track, autom::kMixerVolume)[1];
        QCOMPARE(moved.beat, 4.0);
        QVERIFY(near(moved.value, 0.4));
        f.stack.undo();
        QVERIFY(f.project.envelope(track, autom::kMixerVolume) == original);
        f.editor.setAutomationCurve(track, autom::kMixerVolume, original, 0, 0.5);
        f.editor.deleteAutomationPoints(track, autom::kMixerVolume, {0, 1});
        QVERIFY(!f.track(track).automation.contains(autom::kMixerVolume));  // empty: no automation
        f.stack.undo();
        QCOMPARE(f.project.envelope(track, autom::kMixerVolume)[0].curve, 0.5);
    }

    void rangeEditsAcrossLanes() {
        EditorFixture f;
        const QString a = f.editor.addAudioTrack();
        const QString b = f.editor.addAudioTrack();
        for (const QString& owner : {a, b, kMaster}) {
            f.editor.setEnvelope(owner, autom::kMixerPan, env({{0.0, 0.0}, {2.0, 1.0}, {4.0, 0.0}}));
        }
        const QList<LaneRef> lanes{{a, autom::kMixerPan}, {kMaster, autom::kMixerPan}};
        f.editor.deleteAutomationRange(1.0, 3.0, lanes);
        f.stack.undo();
        f.stack.redo();
        QCOMPARE(f.project.envelope(a, autom::kMixerPan).size(), size_t(4));  // the peak went; edges keep the outside
        QCOMPARE(f.project.envelope(kMaster, autom::kMixerPan).size(), size_t(4));
        QCOMPARE(f.project.envelope(b, autom::kMixerPan).size(), size_t(3));
        f.stack.undo();  // both lanes in one step
        QCOMPARE(f.project.envelope(kMaster, autom::kMixerPan).size(), size_t(3));
        f.editor.duplicateAutomationRange(0.0, 2.0, {{b, autom::kMixerPan}});
        QVERIFY(near(valueAt(f.project.envelope(b, autom::kMixerPan), 3.0), 0.5));
    }

    void movingARangeOfLanesIsOneStepPerDrag() {
        EditorFixture f;
        const QString a = f.editor.addAudioTrack();
        f.editor.setEnvelope(a, autom::kMixerPan, env({{0.0, 0.2}, {2.0, 0.6}, {4.0, 0.2}}));
        const QMap<LaneRef, Envelope> originals{{{a, autom::kMixerPan}, f.project.envelope(a, autom::kMixerPan)},
                                               {{a, autom::kMixerVolume}, {}}};  // (nothing to move)
        const int steps = f.stack.count();
        for (int step = 1; step <= 3; ++step) {
            f.editor.moveAutomationRange(1.0, 3.0, originals, 0.0, 0.1 * step, "drag");
        }
        QCOMPARE(f.stack.count(), steps + 1);
        QVERIFY(near(valueAt(f.project.envelope(a, autom::kMixerPan), 2.0), 0.9));
        QVERIFY(!f.track(a).automation.contains(autom::kMixerVolume));
        f.stack.undo();
        QVERIFY(f.project.envelope(a, autom::kMixerPan) == originals.value({a, autom::kMixerPan}));
    }

    void deletingADeviceDeletesItsAutomation() {
        EditorFixture f;
        const QString track = f.editor.addMidiTrack();
        const QString synth = f.track(track).devices[0].id;
        const QString utility = f.editor.addDevice(track, "utility");
        const QString key = autom::deviceKey(utility, "gain");
        f.editor.setEnvelope(track, key, env({{0.0, 0.5}}));
        f.editor.setEnvelope(track, autom::deviceKey(synth, "cutoff"), env({{0.0, 0.5}}));
        f.editor.removeDevice(track, utility);
        QCOMPARE(f.track(track).automation.keys(), QList<QString>{autom::deviceKey(synth, "cutoff")});
        f.stack.undo();  // the device and its automation come back together
        QVERIFY(f.track(track).automation.contains(key));
        QCOMPARE(f.track(track).devices.size(), size_t(2));
        QCOMPARE(f.track(track).devices[0].id, synth);
        QCOMPARE(f.track(track).devices[1].id, utility);
        f.editor.addDevice(track, "synth");  // replaces the instrument, and its automation goes
        QCOMPARE(f.track(track).automation.keys(), QList<QString>{key});
    }

    void touchingAParameterIsReported() {
        EditorFixture f;
        const QString track = f.editor.addMidiTrack();
        const QString synth = f.track(track).devices[0].id;
        QSignalSpy touched(&f.editor, &ProjectEditor::parameterTouched);
        f.editor.setTrackParam(track, TrackField::VolumeDb, -3.0);
        f.editor.setTrackParam(track, TrackField::Solo, true);  // not automatable
        f.editor.setDeviceParam(track, synth, "cutoff", 1000.0);
        f.editor.setTrackParam(kMaster, TrackField::Pan, 0.5);
        f.editor.touchParameter(track, autom::kMixerPan);  // clicked, not changed
        const QList<QStringList> expected{{track, autom::kMixerVolume},
                                          {track, autom::deviceKey(synth, "cutoff")},
                                          {kMaster, autom::kMixerPan},
                                          {track, autom::kMixerPan}};
        QCOMPARE(touched.size(), expected.size());
        for (qsizetype i = 0; i < expected.size(); ++i) {
            QCOMPARE((QStringList{touched[i][0].toString(), touched[i][1].toString()}), expected[i]);
        }
        QCOMPARE(f.project.master().pan, 0.5);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Change Master Pan"));
        f.stack.undo();
        QCOMPARE(f.project.master().pan, 0.0);
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setTrackParam(kMaster, TrackField::Mute, true));  // always heard
        QVERIFY(!f.editor.trySetTrackParam(kMaster, "mute", 1.0));
        QCOMPARE(f.messages.size(), 1);
        QVERIFY(f.editor.trySetTrackParam(track, "pan", -0.25));
        QCOMPARE(f.track(track).pan, -0.25);
    }

    void automationView() {
        EditorFixture f;
        const QString a = f.editor.addAudioTrack();
        const QString b = f.editor.addAudioTrack();
        f.editor.setEnvelope(b, autom::kMixerPan, env({{0.0, 0.5}}));
        f.editor.showAutomation(a);
        QVERIFY((f.project.automationView(a) == AutomationView{true, autom::kMixerVolume, {}}));
        QVERIFY(f.editor.toggleAllAutomation());  // not all showed: now all do
        QCOMPARE(f.project.automationView(b).key, std::optional<QString>(autom::kMixerPan));  // its automated target first
        QVERIFY(f.project.automationView(kMaster).shown);
        QVERIFY(!f.editor.toggleAllAutomation());
        for (const QString& owner : f.project.owners()) QVERIFY(!f.project.automationView(owner).shown);
        QCOMPARE(f.project.automationView(b).key, std::optional<QString>(autom::kMixerPan));  // remembered
        f.editor.addAutomationLane(b);
        f.editor.addAutomationLane(b);
        QVERIFY((f.project.automationView(b) ==
                 AutomationView{true, autom::kMixerPan, {autom::kMixerVolume, autom::kMixerPan}}));
        f.editor.setAutomationLane(b, 1, autom::kMixerVolume);
        f.editor.removeAutomationLane(b, 0);
        QCOMPARE(f.project.automationView(b).lanes, QStringList{autom::kMixerVolume});
        QCOMPARE(f.stack.count(), 3);  // two tracks and the envelope: view changes are not undo steps
        f.editor.setAutomationLane(b, -1, autom::kMixerVolume);
        QCOMPARE(f.project.automationView(b).key, std::optional<QString>(autom::kMixerVolume));
        f.editor.resetAutomationView(b);
        QVERIFY(f.project.automationView(b) == AutomationView{});
        QCOMPARE(f.editor.defaultAutomationKey(b), autom::kMixerPan);
        QCOMPARE(f.editor.defaultAutomationKey(a), autom::kMixerVolume);
    }

    void saveAndLoad() {
        EditorFixture f;
        const QString track = f.editor.addMidiTrack();
        const QString deviceKey = autom::deviceKey(f.track(track).devices[0].id, "cutoff");
        Envelope curved = env({{0.0, 0.2}, {2.0, 0.9}});
        curved[0].curve = 0.3;
        f.editor.setEnvelope(track, deviceKey, curved);
        f.editor.setEnvelope(kMaster, autom::kMixerVolume, env({{1.0, 0.5}}));
        f.editor.setTrackParam(kMaster, TrackField::Pan, -0.25);
        f.editor.showAutomation(track, deviceKey);
        f.editor.addAutomationLane(track);
        f.editor.setAutomationLocked(true);
        QJsonObject data = QJsonDocument::fromJson(QJsonDocument(projectToJson(f.project)).toJson()).object();
        QCOMPARE(data["version"].toInt(), kProjectVersion);
        QJsonArray tracks = data["tracks"].toArray();
        QJsonObject saved = tracks[0].toObject();
        QJsonObject automation = saved["automation"].toObject();
        automation["mixer:unknown"] = QJsonArray{QJsonArray{0, 1, 0}};  // from a later version: dropped
        saved["automation"] = automation;
        tracks[0] = saved;
        data["tracks"] = tracks;
        Project loaded;
        loadInto(loaded, data);
        const Track& restored = loaded.tracks()[0];
        QVERIFY(restored.automation == f.track(track).automation);
        QVERIFY(restored.automationView == f.track(track).automationView);
        QVERIFY((loaded.master().automation == EnvelopeMap{{autom::kMixerVolume, env({{1.0, 0.5}})}}));
        QCOMPARE(loaded.master().pan, -0.25);
        QVERIFY(loaded.automationLocked());
    }

    // --- Automation moving with clips (unless locked) ---

    void movingAClipMovesItsAutomation() {
        EditorFixture f;
        const TwoTracks t = twoTracks(f);
        const int steps = f.stack.count();
        f.editor.moveRange(4.0, 8.0, {t.a}, 8.0);
        QCOMPARE(f.project.clip(t.a, t.clip).startBeat, 12.0);
        const Envelope pan = f.project.envelope(t.a, autom::kMixerPan);
        // Moved: the stretch under the clip, what it replaced at 12..16, and a straight line where it was.
        QVERIFY(near(valueAt(pan, 13.0), 0.0) && near(valueAt(pan, 15.0), 1.0));
        QVERIFY(near(valueAt(pan, 5.0), 0.3));  // (4, 0.1) to (8, 0.9)
        QVERIFY(near(valueAt(pan, 2.0), valueAt(twoTracksPan(), 2.0)));
        QVERIFY((beatsOf(f.project.envelope(t.a, t.synth)) == std::vector<double>{14.0}));
        QVERIFY(f.project.envelope(t.a, autom::kMixerVolume) == env({{0.0, 0.8}, {2.0, 0.6}}));  // untouched
        QCOMPARE(f.stack.count(), steps + 1);
        f.stack.undo();
        QVERIFY(f.project.envelope(t.a, autom::kMixerPan) == twoTracksPan());
        QCOMPARE(f.project.clip(t.a, t.clip).startBeat, 4.0);
    }

    void lockedAutomationStays() {
        EditorFixture f;
        const TwoTracks t = twoTracks(f);
        f.editor.setAutomationLocked(true);
        QCOMPARE(f.stack.count(), 6);  // a setting, not an edit
        f.editor.moveRange(4.0, 8.0, {t.a}, 8.0);
        f.editor.duplicateRange(12.0, 16.0, {t.a});
        QVERIFY(f.project.envelope(t.a, autom::kMixerPan) == twoTracksPan());
        QVERIFY(f.project.envelope(t.a, t.synth) == env({{6.0, 0.25}}));
    }

    void copiesOfClipsCopyTheirAutomation() {
        EditorFixture f;
        const TwoTracks t = twoTracks(f);
        f.editor.duplicateRange(4.0, 8.0, {t.a});  // Ctrl+D
        const Envelope pan = f.project.envelope(t.a, autom::kMixerPan);
        QVERIFY(near(valueAt(pan, 5.0), 0.0) && near(valueAt(pan, 9.0), 0.0));
        QVERIFY(near(valueAt(pan, 11.0), 1.0));
        QVERIFY((beatsOf(f.project.envelope(t.a, t.synth)) == std::vector<double>{6.0, 10.0}));
        f.editor.moveRange(4.0, 8.0, {t.a}, 16.0, 0, true);  // Ctrl-drag
        QVERIFY(f.project.envelope(t.a, t.synth) == env({{6.0, 0.25}, {10.0, 0.25}, {22.0, 0.25}}));
    }

    void acrossTracksOnlyTheMixerAutomationGoesAlong() {
        EditorFixture f;
        const TwoTracks t = twoTracks(f);
        f.editor.moveRange(4.0, 8.0, {t.a}, 0.0, 1);
        QCOMPARE(f.project.clip(t.b, t.clip).startBeat, 4.0);
        const Envelope pan = f.project.envelope(t.b, autom::kMixerPan);
        QVERIFY(near(valueAt(pan, 5.0), 0.0) && near(valueAt(pan, 7.0), 1.0));
        for (const AutomationPoint& point : f.project.envelope(t.a, autom::kMixerPan)) {
            QVERIFY(!(4.0 < point.beat && point.beat < 8.0));  // gone from a
        }
        QVERIFY(f.project.envelope(t.a, t.synth) == env({{6.0, 0.25}}));  // a device's stays on its track
        QCOMPARE(f.track(t.b).automation.keys(), QList<QString>{autom::kMixerPan});
    }

    void movingClipsByReferenceMovesTheirAutomation() {
        EditorFixture f;
        const TwoTracks t = twoTracks(f);
        f.editor.moveClips({{t.a, t.clip}}, -4.0);
        QVERIFY((beatsOf(f.project.envelope(t.a, t.synth)) == std::vector<double>{2.0}));
        f.editor.duplicateClips({{t.a, t.clip}});
        QVERIFY((beatsOf(f.project.envelope(t.a, t.synth)) == std::vector<double>{2.0, 6.0}));
    }
};

QTEST_GUILESS_MAIN(TestEditorAutomation)
#include "test_editor_automation.moc"
