// Track labels on the application's side (Session.trackLabels): what the
// labeller is told of a project (devices and their parameters, notes and files
// where they play, sends, the mixer; whose name a track has), presets and
// effects' amounts read from plug-ins' saved states when they aren't loaded,
// following the project once its edits settle, and what agents and tooltips get.

#include "EditorFixture.h"
#include "TestSupport.h"

#include "intelligence/TrackLabels.h"
#include "model/Devices.h"

#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <utility>

using namespace sub::app;
using test::EditorFixture;
namespace labels = sub::intelligence::labels;

namespace {

void put(QByteArray& to, quint64 value, int bytes) {
    for (int i = 0; i < bytes; ++i) to.append(static_cast<char>((value >> (8 * i)) & 0xff));
}

// A .vstpreset (as a plug-in's Device::state holds one, base64) of a component's and a controller's state.
QString savedState(const QByteArray& component, const QByteArray& controller = {}) {
    QByteArray out("VST3");
    put(out, 1, 4);
    out += QByteArray(32, 'A');
    put(out, 0, 8);
    QByteArray list("List");
    put(list, 2, 4);
    for (const auto& [id, data] : {std::pair{QByteArray("Comp"), component}, std::pair{QByteArray("Cont"), controller}}) {
        list += id;
        put(list, static_cast<quint64>(out.size()), 8);
        put(list, static_cast<quint64>(data.size()), 8);
        out += data;
    }
    const auto at = static_cast<quint64>(out.size());
    for (int i = 0; i < 8; ++i) out[40 + i] = static_cast<char>((at >> (8 * i)) & 0xff);
    return QString::fromLatin1((out + list).toBase64());
}

PluginRef pluginRef(const QString& name, bool instrument) {
    return PluginRef{QStringLiteral("VST3"), QString(32, u'0'), name, QStringLiteral("Vendor"), QString(), instrument};
}

const labels::TrackFacts& factsOf(const std::vector<labels::TrackFacts>& all, const QString& id) {
    return *std::find_if(all.begin(), all.end(), [&](const labels::TrackFacts& t) { return t.id == id.toStdString(); });
}

const labels::ParamFact* param(const labels::DeviceFact& device, const std::string& id) {
    for (const labels::ParamFact& p : device.params) {
        if (p.id == id) return &p;
    }
    return nullptr;
}

}  // namespace

class TestTrackLabels : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void whatTheLabellerIsTold() {
        EditorFixture f;
        // A MIDI track (the built-in Synth) through a Delay, its notes in a clip at beat 4; a send to a return.
        const QString midi = f.editor.addMidiTrack();
        const QString delay = f.editor.addDevice(midi, QStringLiteral("delay"));
        f.editor.setDeviceParam(midi, delay, QStringLiteral("mix"), 40.0);
        const auto clip = f.editor.addMidiClip(midi, 4, 4);
        QVERIFY(clip);
        f.editor.setClipNotes(*clip, {{60, 0, 1, 100}, {64, 0, 1, 90}, {67, 0, 1, 80}}, QStringLiteral("Notes"));
        const QString ret = f.editor.addReturnTrack();
        f.editor.setSend(midi, ret, -6.0);
        f.editor.setTrackParam(midi, TrackField::Mute, 1);
        // An audio track playing a file, a second at 120 BPM.
        const ClipRefs audio = f.editor.addClips({}, 0.0, {{QStringLiteral("C:/x/C_BellStab1.wav"), 1.0}});
        QCOMPARE(audio.size(), 1);

        TrackLabels trackLabels(&f.project, nullptr, nullptr);
        const auto facts = trackLabels.facts();
        QCOMPARE(int(facts.size()), 4);  // the tracks, the return, the master
        const labels::TrackFacts& m = factsOf(facts, midi);
        QCOMPARE(m.kind, std::string("midi"));
        QVERIFY(m.mute);
        QCOMPARE(int(m.devices.size()), 2);
        QCOMPARE(m.devices[0].kind, std::string("synth"));
        QCOMPARE(m.devices[0].name, std::string("Synth"));
        QVERIFY(m.devices[0].instrument);
        const labels::ParamFact* attack = param(m.devices[0], "attack");
        QVERIFY(attack);
        QCOMPARE(attack->name, std::string("Attack"));
        QCOMPARE(attack->unit, std::string("ms"));
        const labels::ParamFact* mix = param(m.devices[1], "mix");
        QVERIFY(mix);
        QCOMPARE(mix->value, 40.0);
        QCOMPARE(mix->unit, std::string("%"));
        QVERIFY(std::abs(mix->normalized - 0.4) < 1e-6);  // (the engine maps in floats)
        QCOMPARE(int(m.notes.size()), 3);
        QCOMPARE(m.notes[0].start, 4.0);  // where it plays, on the timeline
        QCOMPARE(m.notes[0].end, 5.0);
        QCOMPARE(int(m.sends.size()), 1);
        QCOMPARE(m.sends[0].returnId, ret.toStdString());
        QCOMPARE(m.sends[0].levelDb, -6.0);
        const labels::TrackFacts& a = factsOf(facts, f.project.tracks().back().id);
        QCOMPARE(int(a.audio.size()), 1);
        QCOMPARE(a.audio[0].file, std::string("C:/x/C_BellStab1.wav"));
        QCOMPARE(a.audio[0].beats, 2.0);
        QCOMPARE(factsOf(facts, ret).kind, std::string("return"));
        QCOMPARE(facts.back().kind, std::string("master"));

        QCOMPARE(trackLabels.label(midi), QStringLiteral("Echoing Synth Chords"));
        QCOMPARE(trackLabels.label(f.project.tracks().back().id), QStringLiteral("Bell Stab"));
        QCOMPARE(trackLabels.label(QStringLiteral("nope")), QString());

        // A deactivated clip plays nothing: its notes aren't the track's.
        f.editor.setRangeActive(4, 8, {midi}, false);
        QVERIFY(factsOf(trackLabels.facts(), midi).notes.empty());
    }

    void whoNamedATrack() {
        EditorFixture f;
        const QString midi = f.editor.addMidiTrack();  // "# Synth": named by its instrument
        const auto named = [&](const QString& id) { return TrackLabels::namedByUser(f.project.track(id)); };
        QVERIFY(!named(midi));
        f.editor.renameTrack(midi, QStringLiteral("vocals"));
        QVERIFY(named(midi));
        f.editor.renameTrack(midi, QStringLiteral("# Lead"));
        QVERIFY(named(midi));
        for (const char* given : {"3 MIDI", "# MIDI", "Synth", "# Synth 2", "4 MIDI 2"}) {
            f.editor.renameTrack(midi, QString::fromLatin1(given));
            QVERIFY2(!named(midi), given);
        }
        const ClipRefs clips = f.editor.addClips({}, 0.0, {{QStringLiteral("C:/x/kick_01.wav"), 1.0}});
        const QString audio = clips.front().trackId;
        QVERIFY(!named(audio));  // "# kick_01"
        f.editor.renameTrack(audio, QStringLiteral("kick_01 2"));  // (a copy's)
        QVERIFY(!named(audio));
        f.editor.renameTrack(audio, QStringLiteral("Kick In"));
        QVERIFY(named(audio));
        const QString group = f.editor.groupTracks({audio});
        QVERIFY(!named(group));
        f.editor.renameTrack(group, QStringLiteral("drums"));
        QVERIFY(named(group));
        QVERIFY(!TrackLabels::namedByUser(f.project.master()));
    }

    void presetsAndAmountsFromSavedStates() {
        // Plug-ins that aren't loaded (no engine here): what their saved states say.
        EditorFixture f;
        const QString midi = f.editor.addMidiTrack(-1, {}, QString());
        const QString serum = f.editor.addDevice(midi, kPluginKind, -1, pluginRef(QStringLiteral("Serum 2"), true));
        const QString verb = f.editor.addDevice(midi, kPluginKind, -1, pluginRef(QStringLiteral("ValhallaVintageVerb"), false));
        QVERIFY(!serum.isEmpty() && !verb.isEmpty());
        f.project.storePluginState(midi, serum, savedState("binary", R"(XferJson{"presetName":"PLUCK - Dynasty"})"), {});
        f.project.storePluginState(midi, verb, savedState(R"(<ValhallaVintageVerb presetName="Default" Mix="0.5"/>)"), {});

        TrackLabels trackLabels(&f.project, nullptr, nullptr);
        const labels::TrackFacts& facts = factsOf(trackLabels.facts(), midi);
        QCOMPARE(facts.devices[0].preset, std::string("PLUCK - Dynasty"));
        QCOMPARE(facts.devices[1].preset, std::string("Default"));
        QCOMPARE(int(facts.devices[1].params.size()), 1);
        QCOMPARE(facts.devices[1].params[0].name, std::string("Mix"));
        QCOMPARE(trackLabels.label(midi), QStringLiteral("Washed Out Serum Pluck"));
        const QString tip = trackLabels.toolTip(midi);
        QVERIFY2(tip.startsWith(QStringLiteral("Washed Out Serum Pluck\n")), qPrintable(tip));
        QVERIFY2(tip.contains(QStringLiteral("preset \u201CPLUCK - Dynasty\u201D")), qPrintable(tip));
        QVERIFY2(tip.contains(QStringLiteral("ValhallaVintageVerb: reverb, 50% wet")), qPrintable(tip));

        // Another state saved: read again.
        f.project.storePluginState(midi, serum, savedState({}, R"({"presetName":"BS - Bass Line"})"), {});
        f.editor.setTrackParam(midi, TrackField::VolumeDb, -1.0);  // (an edit: labelled again)
        QCOMPARE(trackLabels.label(midi), QStringLiteral("Washed Out Serum Bass Line"));
    }

    void followsTheProjectOnceItSettles() {
        EditorFixture f;
        TrackLabels trackLabels(&f.project, nullptr, nullptr);
        QSignalSpy changed(&trackLabels, &TrackLabels::changed);
        const QString midi = f.editor.addMidiTrack();
        f.editor.renameTrack(midi, QStringLiteral("piano"));
        f.editor.setTrackParam(midi, TrackField::VolumeDb, -3.0);
        QCOMPARE(changed.count(), 0);  // not yet: once, when the edits settle
        QCOMPARE(trackLabels.label(midi), QStringLiteral("Synth Piano"));  // (asked before: labelled now)
        QTRY_COMPARE(changed.count(), 1);
        QCOMPARE(trackLabels.revision(), 1);
        QTest::qWait(TrackLabels::kSettleMs * 2);
        QCOMPARE(changed.count(), 1);

        f.editor.addDevice(midi, QStringLiteral("delay"));
        QTRY_COMPARE(changed.count(), 2);
        QCOMPARE(trackLabels.label(midi), QStringLiteral("Echoing Synth Piano"));
        f.stack.undo();
        QCOMPARE(trackLabels.label(midi), QStringLiteral("Synth Piano"));
    }

    void whatAgentsAndTooltipsGet() {
        EditorFixture f;
        const ClipRefs kick = f.editor.addClips({}, 0.0, {{QStringLiteral("C:/x/Kick_Deep.wav"), 0.5}});
        const ClipRefs snare = f.editor.addClips({}, 0.0, {{QStringLiteral("C:/x/Snare_Tight.wav"), 0.5}});
        const QString group = f.editor.groupTracks({kick.front().trackId, snare.front().trackId});
        const QString ret = f.editor.addReturnTrack();
        f.editor.addDevice(ret, QStringLiteral("delay"));

        TrackLabels trackLabels(&f.project, nullptr, nullptr);
        const QVariantList all = trackLabels.describe();
        QCOMPARE(all.size(), 5);  // the group, its two tracks, the return, the master: in that order
        const QVariantMap first = all[0].toMap();
        QCOMPARE(first.value(QStringLiteral("id")).toString(), group);
        QCOMPARE(first.value(QStringLiteral("kind")).toString(), QStringLiteral("group"));
        QCOMPARE(first.value(QStringLiteral("label")).toString(), QStringLiteral("Drum Group"));
        QCOMPARE(first.value(QStringLiteral("family")).toString(), QStringLiteral("drums"));
        QCOMPARE(first.value(QStringLiteral("role")).toString(), QStringLiteral("group"));
        const QVariantMap second = all[1].toMap();
        QCOMPARE(second.value(QStringLiteral("label")).toString(), QStringLiteral("Deep Kick"));
        QCOMPARE(second.value(QStringLiteral("parent")).toString(), group);
        QCOMPARE(second.value(QStringLiteral("role")).toString(), QStringLiteral("kick"));
        QVERIFY(second.value(QStringLiteral("details")).toStringList().contains(QStringLiteral("Audio: \u201CKick_Deep\u201D")));
        QCOMPARE(all[3].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Delay Return"));
        QCOMPARE(all[4].toMap().value(QStringLiteral("label")).toString(), QStringLiteral("Master"));

        QVERIFY(trackLabels.toolTip(group).startsWith(QStringLiteral("Drum Group\nHolds: Deep Kick, Tight Snare")));
        QCOMPARE(trackLabels.toolTip(QStringLiteral("nope")), QString());
    }
};

QTEST_GUILESS_MAIN(TestTrackLabels)
#include "test_track_labels.moc"
