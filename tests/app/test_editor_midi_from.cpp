// A device's MIDI input from another track (Device::midiFrom) in the
// application layer: setting it (undo, what is refused), what deleting,
// duplicating, pasting, flattening and freezing do to it, presets and project
// files, and the engine hearing it through the session (an instrument and an
// effect with a MIDI input playing a MIDI track's notes).

#include "BridgeTestSupport.h"
#include "EditorFixture.h"
#include "SessionFixture.h"
#include "TestSupport.h"

#include "io/Serialization.h"
#include "model/Devices.h"
#include "model/Errors.h"

#include <QJsonObject>
#include <QTest>

#include <cmath>

using namespace sub::app;
using test::EditorFixture;
using test::SessionFixture;

namespace {

const Freeze kFreeze{QStringLiteral("frozen.wav"), 2.0, 120.0};

struct Setup {
    QString keys;    // a MIDI track whose notes are taken
    QString lead;    // a MIDI track with a synth
    QString synth;   // its synth
    QString audio;   // an audio track
};

Setup setUp(EditorFixture& f) {
    Setup s;
    s.keys = f.editor.addMidiTrack();
    s.lead = f.editor.addMidiTrack();
    s.synth = f.track(s.lead).devices[0].id;
    s.audio = f.editor.addAudioTrack();
    return s;
}

}  // namespace

class TestEditorMidiFrom : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void aDeviceTakesAMidiTracksNotes() {
        EditorFixture f;
        const Setup s = setUp(f);
        QVERIFY(f.editor.trySetDeviceMidiFrom(s.lead, s.synth, s.keys));
        QCOMPARE(f.p().device(s.lead, s.synth).midiFrom, s.keys);
        QCOMPARE(f.stack.undoText(), QStringLiteral("Change MIDI Input"));
        f.stack.undo();
        QCOMPARE(f.p().device(s.lead, s.synth).midiFrom, QString());
        f.stack.redo();
        QVERIFY(f.editor.trySetDeviceMidiFrom(s.lead, s.synth, QString()));
        QCOMPARE(f.p().device(s.lead, s.synth).midiFrom, QString());
        QCOMPARE(f.stack.undoText(), QStringLiteral("Remove MIDI Input"));
        // Its own track is what it hears without one.
        QVERIFY(f.editor.trySetDeviceMidiFrom(s.lead, s.synth, s.lead));
        QCOMPARE(f.p().device(s.lead, s.synth).midiFrom, QString());
        // The sources: the MIDI tracks but its own.
        QCOMPARE(test::ids(f.p().midiSources(s.lead)), QStringList{s.keys});
        QCOMPARE(test::ids(f.p().midiSources(s.audio)), (QStringList{s.keys, s.lead}));
    }

    void onlyMidiTracksAreSources() {
        EditorFixture f;
        const Setup s = setUp(f);
        QVERIFY(!f.editor.trySetDeviceMidiFrom(s.lead, s.synth, s.audio));
        QCOMPARE(f.messages.back(), QStringLiteral("Synth can only take its notes from a MIDI track"));
        QVERIFY(!f.editor.trySetDeviceMidiFrom(s.lead, s.synth, kMaster));
        QVERIFY(!f.editor.trySetDeviceMidiFrom(s.lead, s.synth, QStringLiteral("gone")));
        QVERIFY_THROWS_EXCEPTION(EditError, f.editor.setDeviceMidiFrom(s.lead, s.synth, s.audio));
        QVERIFY(!f.editor.trySetDeviceMidiFrom(s.lead, QStringLiteral("no device"), s.keys));
        QCOMPARE(f.p().device(s.lead, s.synth).midiFrom, QString());
        // A device on the master, a return or an audio track may take one.
        const QString onMaster = f.editor.addDevice(kMaster, QStringLiteral("utility"));
        QVERIFY(f.editor.trySetDeviceMidiFrom(kMaster, onMaster, s.keys));
        const QString onAudio = f.editor.addDevice(s.audio, QStringLiteral("utility"));
        QVERIFY(f.editor.trySetDeviceMidiFrom(s.audio, onAudio, s.keys));
    }

    void deletingTheSourceTakesItAwayInTheSameStep() {
        EditorFixture f;
        const Setup s = setUp(f);
        f.editor.setDeviceMidiFrom(s.lead, s.synth, s.keys);
        f.editor.deleteTracks({s.keys});
        QCOMPARE(f.p().device(s.lead, s.synth).midiFrom, QString());
        f.stack.undo();  // one step: the track and the MIDI input come back
        QVERIFY(f.p().hasTrack(s.keys));
        QCOMPARE(f.p().device(s.lead, s.synth).midiFrom, s.keys);
        // Deleted together, they come back together.
        f.editor.deleteTracks({s.keys, s.lead});
        f.stack.undo();
        QCOMPARE(f.p().device(s.lead, s.synth).midiFrom, s.keys);
    }

    void copiesTakeTheirSourcesCopy() {
        EditorFixture f;
        const Setup s = setUp(f);
        f.editor.setDeviceMidiFrom(s.lead, s.synth, s.keys);
        // The device's track alone: the copy takes the same track's notes.
        const QStringList one = f.editor.duplicateTracks({s.lead});
        QCOMPARE(one.size(), 1);
        QCOMPARE(f.track(one[0]).devices[0].midiFrom, s.keys);
        // Both: the copy takes the copy's.
        const QStringList both = f.editor.duplicateTracks({s.keys, s.lead});
        QCOMPARE(both.size(), 2);
        QCOMPARE(f.track(both[1]).devices[0].midiFrom, both[0]);
        // Pasted devices keep theirs while it is a MIDI track there, but onto it.
        const QString utility = f.editor.addDevice(s.lead, QStringLiteral("utility"));
        f.editor.setDeviceMidiFrom(s.lead, utility, s.keys);
        const std::vector<Device> copied = f.editor.copyDevices(s.lead, {utility});
        const QStringList pasted = f.editor.pasteDevices(s.audio, {copied[0]});
        QCOMPARE(pasted.size(), 1);
        QCOMPARE(f.p().device(s.audio, pasted[0]).midiFrom, s.keys);
        const QStringList onSource = f.editor.pasteDevices(s.keys, {copied[0]});
        QCOMPARE(onSource.size(), 1);
        QCOMPARE(f.p().device(s.keys, onSource[0]).midiFrom, QString());
    }

    void flatteningTheSourceTakesItAway() {
        EditorFixture f;
        const Setup s = setUp(f);
        f.editor.setDeviceMidiFrom(s.lead, s.synth, s.keys);
        f.editor.freezeTracks({{s.keys, kFreeze}});
        QCOMPARE(f.p().device(s.lead, s.synth).midiFrom, s.keys);  // frozen, it still plays its notes there
        f.editor.flattenTracks({s.keys});
        QCOMPARE(f.track(s.keys).kind, kAudioKind);
        QCOMPARE(f.p().device(s.lead, s.synth).midiFrom, QString());
        f.stack.undo();
        QCOMPARE(f.p().device(s.lead, s.synth).midiFrom, s.keys);
    }

    void aFrozenTracksDevicesKeepTheirMidiInputs() {
        EditorFixture f;
        const Setup s = setUp(f);
        f.editor.freezeTracks({{s.lead, kFreeze}});
        f.messages.clear();
        f.editor.trySetDeviceMidiFrom(s.lead, s.synth, s.keys);  // what is frozen is its audio
        QCOMPARE(f.p().device(s.lead, s.synth).midiFrom, QString());
        QCOMPARE(f.messages.size(), 1);
        f.editor.unfreezeTracks({s.lead});
        f.editor.setDeviceMidiFrom(s.lead, s.synth, s.keys);
        f.editor.freezeTracks({{s.lead, kFreeze}});
        f.editor.deleteTracks({s.keys});  // (its source going takes it away all the same)
        QCOMPARE(f.p().device(s.lead, s.synth).midiFrom, QString());
    }

    void midiInputsInFilesAndPresets() {
        test::TempDir dir;
        EditorFixture f;
        const Setup s = setUp(f);
        f.editor.setDeviceMidiFrom(s.lead, s.synth, s.keys);
        const QString target = dir.path(QStringLiteral("midi from.gilproj"));
        saveProject(f.project, target);
        Project loaded;
        loadProject(loaded, target);
        QCOMPARE(loaded.device(s.lead, s.synth).midiFrom, s.keys);
        // A preset names no track of its project.
        QCOMPARE(presetDevice(deviceToPreset(f.p().device(s.lead, s.synth))).midiFrom, QString());
        QJsonObject preset = deviceToPreset(f.p().device(s.lead, s.synth));  // (one written by hand)
        QJsonObject presetData = preset.value(QStringLiteral("device")).toObject();
        presetData[QStringLiteral("midi_from")] = s.keys;
        preset[QStringLiteral("device")] = presetData;
        QCOMPARE(presetDevice(preset).midiFrom, QString());
        // One from a track that isn't a MIDI track there is dropped as it loads.
        Track audio = f.track(s.audio);
        Device utility = test::makeDevice(QStringLiteral("u"), QStringLiteral("utility"));
        utility.midiFrom = s.audio;
        audio.devices = {utility};
        ProjectContents contents;
        contents.tracks = {f.track(s.keys), audio};
        Project edited;
        edited.replaceContents(std::move(contents));
        saveProject(edited, target);
        loadProject(loaded, target);
        QCOMPARE(loaded.device(s.audio, QStringLiteral("u")).midiFrom, QString());
    }

    // --- What the engine hears -------------------------------------------------------------

    void anInstrumentPlaysAnotherTracksNotes() {
        SessionFixture s;
        const QString keys = s.editor().addMidiTrack(-1, {}, QString(), std::nullopt);  // no instrument of its own
        const auto clip = s.editor().addMidiClip(keys, 0.0, 4.0);
        QVERIFY(clip);
        s.editor().setClipNotes(*clip, {Note{69, 0.0, 4.0, 127}}, QStringLiteral("Notes"));
        const QString lead = s.editor().addMidiTrack();
        const QString synth = s.project().track(lead).devices[0].id;
        QVERIFY(s.bridge().acceptsMidi(lead, synth));
        QVERIFY(std::abs(s.render(4000)[3000 * 2]) == 0.f);  // its own track has no notes
        s.editor().setDeviceMidiFrom(lead, synth, keys);
        QVERIFY(std::abs(s.level()) > 0.01f);
        s.stack().undo();
        QCOMPARE(s.level(), 0.f);
        const QString utility = s.editor().addDevice(lead, QStringLiteral("utility"));
        QVERIFY(!s.bridge().acceptsMidi(lead, utility));
        // A device that plays no notes is given none (it keeps the setting for when it could).
        QVERIFY(s.editor().trySetDeviceMidiFrom(lead, utility, keys));
        QCOMPARE(s.level(), 0.f);
    }

    void anEffectWithAMidiInputPlaysAlong() {
        const QString bundle = test::testPluginsBundle();
        if (!test::haveTestPlugins(bundle)) QSKIP("the test plug-ins are not built");
        SessionFixture s;
        const QString keys = s.editor().addMidiTrack(-1, {}, QString(), std::nullopt);
        const auto clip = s.editor().addMidiClip(keys, 0.0, 4.0);
        s.editor().setClipNotes(*clip, {Note{60, 1.0, 1.0, 127}}, QStringLiteral("Notes"));
        const QString audio = s.editor().addAudioTrack();
        const QString effect =
            s.editor().addDevice(audio, kPluginKind, -1, *test::testPlugin(bundle, QStringLiteral("SUB Test Note Effect")));
        QTRY_VERIFY(s.bridge().acceptsMidi(audio, effect));
        s.editor().setDeviceMidiFrom(audio, effect, keys);
        const std::vector<float> out = s.render(3 * test::kSampleRate / 2);
        QCOMPARE(out[2 * (test::kSampleRate / 4)], 0.f);              // before the note
        QCOMPARE(out[2 * (test::kSampleRate / 2 + 100)], 1.f);        // velocity/127 while it is held
        QCOMPARE(out[2 * (test::kSampleRate + 100)], 0.f);            // after it
    }
};

QTEST_MAIN(TestEditorMidiFrom)
#include "test_editor_midi_from.moc"
