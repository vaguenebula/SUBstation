// The engine taking the project's tracks through the bridge: clips and notes as
// the engine has them, groups as buses, returns and sends (made silent when only
// automated), inputs from other tracks' outputs (resampling), the master's
// mixer, the project's settings, a drag's preview, and automation overridden by
// hand. The changes are made as the editor makes them (the model's commands).

#include "BridgeTestSupport.h"
#include "TestSupport.h"

#include "audio/EngineDescs.h"
#include "io/Serialization.h"
#include "model/Notes.h"

#include <QSignalSpy>
#include <QTest>

#include <cmath>

using namespace sub::app;
using sub::app::test::Studio;

namespace {

bool near(double a, double b, double tolerance = 1e-6) { return std::abs(a - b) <= tolerance; }

QString dcWav(const test::TempDir& dir, const QString& name = QStringLiteral("dc.wav"), float value = 0.5f) {
    return test::writeWav(dir.path(name), std::vector<float>(2 * test::kSampleRate, value), 2);
}

}  // namespace

class TestBridgeTracks : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void clipsAsTheEngineHasThem() {
        Clip clip = Clip::audio(QStringLiteral("c1"), QStringLiteral("/a/b.wav"), QStringLiteral("b"), 2.0, 1.5, 0.25);
        clip.gainDb = -6.020599913279624;
        clip.pan = -0.5;
        sub::ClipDesc desc = clipDesc(clip);
        QCOMPARE(desc.path, std::string("/a/b.wav"));
        QCOMPARE(desc.startBeat, 2.0);
        QCOMPARE(desc.durationSec, 1.5);
        QCOMPARE(desc.offsetSec, 0.25);
        QVERIFY(near(desc.gain, 0.5));
        QCOMPARE(desc.pan, -0.5f);
        QVERIFY(!desc.warp);  // not warped: no segment BPM
        QCOMPARE(desc.warpMode, sub::WarpMode::Standard);
        QCOMPARE(desc.id, std::string("c1"));
        // Re-Pitch, transposed: the engine has it as the clip says.
        clip.warp = true;
        clip.segmentBpm = 120.0;
        clip.warpMode = QStringLiteral("Re-Pitch");
        clip.transpose = 12;
        desc = clipDesc(clip);
        QVERIFY(desc.warp);
        QCOMPARE(desc.segmentBpm, 120.0);
        QCOMPARE(desc.warpMode, sub::WarpMode::RePitch);
        QCOMPARE(desc.transpose, 12.0);
        clip.detune = 50.0;
        clip.warpMode = QStringLiteral("Formants");
        desc = clipDesc(clip);
        QCOMPARE(desc.transpose, 12.5);
        QCOMPARE(desc.warpMode, sub::WarpMode::Formants);
        clip.warpMode = QStringLiteral("Beats");  // (an unknown mode plays as Standard)
        QCOMPARE(clipDesc(clip).warpMode, sub::WarpMode::Standard);
    }

    void aMidiTracksNotesAreTheOnesItsClipsPlay() {
        const int C = 60, E = 64, G = 67;
        const Clip clip = Clip::midi(QStringLiteral("m"), QStringLiteral("m"), 8.0, 2.0, 1.0,
                                     notes::normalize({
                                         {C, 0.0, 4.0, 100},  // starts before the window: not played (no chasing)
                                         {E, 1.0, 0.5, 100},  // at the window start: plays at the clip start
                                         {G, 2.5, 1.0, 100},  // runs past the window: cut at the clip end
                                         {C, 3.0, 1.0, 100},  // at the window end: not played
                                     }));
        Track track = test::makeTrack(QStringLiteral("t"), QStringLiteral("t"), kMidiKind);
        track.clips = {clip};
        const std::vector<sub::NoteDesc> notes = noteDescs(track);
        QCOMPARE(notes.size(), size_t{2});
        QCOMPARE(notes[0].startBeat, 8.0);
        QCOMPARE(notes[0].lengthBeats, 0.5);
        QCOMPARE(notes[0].key, E);
        QCOMPARE(notes[0].velocity, 100);
        QCOMPARE(notes[1].startBeat, 9.5);
        QCOMPARE(notes[1].lengthBeats, 0.5);
        QCOMPARE(clipNoteDescs({clip}).size(), size_t{2});
        QVERIFY(clipNoteDescs({test::makeTrack("a", "a").clips}).empty());
    }

    void theEngineHearsGroupsAsBuses() {
        test::TempDir dir;
        Studio studio;
        const QString wav = dcWav(dir);
        const QString a = studio.clipTrack(wav, 1.0, QStringLiteral("A"));
        const QString b = studio.clipTrack(wav, 1.0, QStringLiteral("B"));
        QVERIFY(near(studio.level(), 1.0));
        const QString group = studio.edit.groupTracks({a, b});
        studio.edit.setTrack(group, TrackField::VolumeDb, -6.0206);
        QVERIFY(near(studio.level(), 0.5, 1e-3));
        studio.edit.setTrack(group, TrackField::Mute, true);
        QVERIFY(near(studio.level(), 0.0));
        studio.edit.moveTracks({b}, static_cast<int>(studio.project.tracks().size()), std::nullopt);  // out of it
        QVERIFY(near(studio.level(), 0.5));
        QCOMPARE(studio.engine.trackOutput(*studio.bridge->engineTrackId(b)), sub::Engine::kMaster);
        QCOMPARE(studio.engine.trackOutput(*studio.bridge->engineTrackId(a)), *studio.bridge->engineTrackId(group));
        studio.stack.undo();
        QVERIFY(near(studio.level(), 0.0));
        studio.stack.undo();  // unmute
        const QString inner = studio.edit.groupTracks({a});  // a group in the group
        QVERIFY(near(studio.level(), 0.5, 1e-3));
        QCOMPARE(studio.engine.trackOutput(*studio.bridge->engineTrackId(inner)), *studio.bridge->engineTrackId(group));
        studio.edit.deleteTracks({group});  // with what is in it
        QVERIFY(near(studio.level(), 0.0));
        QVERIFY(!studio.bridge->engineTrackId(a) && !studio.bridge->engineTrackId(inner));
        studio.stack.undo();
        QVERIFY(near(studio.level(), 0.5, 1e-3));
        QCOMPARE(*studio.project.track(inner).parent, group);
    }

    void theEngineHearsReturnsAndSends() {
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString track = studio.clipTrack(dcWav(dir));
        QVERIFY(near(studio.level(), 0.5));
        const QString ret = studio.edit.addReturnTrack();
        QVERIFY(near(studio.level(), 0.5));  // no send yet
        studio.edit.setSend(track, ret, -6.0206);
        QVERIFY(near(studio.level(), 0.75, 1e-3));
        studio.edit.setTrack(track, TrackField::VolumeDb, -70.0);  // silent: only a pre-fader send is heard
        QVERIFY(near(studio.level(), 0.0));
        studio.edit.setSend(track, ret, std::nullopt, true);
        QVERIFY(near(studio.level(), 0.25, 1e-3));
        const QString other = studio.edit.addReturnTrack();
        studio.edit.setSend(ret, other, 0.0);
        QVERIFY(near(studio.level(), 0.5, 1e-3));
        studio.edit.setTrack(ret, TrackField::Mute, true);
        QVERIFY(near(studio.level(), 0.0));
        studio.stack.undo();
        studio.edit.setTrack(track, TrackField::VolumeDb, 0.0);
        QVERIFY(near(studio.level(), 1.0, 1e-3));  // the track, ret, other
        // Automating a send that wasn't set makes it, silent, so that the automation plays.
        const QString third = studio.edit.addReturnTrack();
        studio.edit.setEnvelope(track, automation::sendKey(third), {{0.0, 1.0, 0.0}});
        const auto sends = studio.engine.trackSends(*bridge.engineTrackId(track));
        QCOMPARE(sends.size(), size_t{2});
        QCOMPARE(sends[0].trackId, *bridge.engineTrackId(ret));
        QCOMPARE(sends[1].trackId, *bridge.engineTrackId(third));
        QVERIFY(near(studio.level(), 1.0 + 0.5 * std::pow(10.0, 6.0 / 20.0), 1e-3));  // +6 dB at the lane's top
        QVERIFY(bridge.isAutomated(track, automation::sendKey(third)));
        studio.edit.setEnvelope(track, automation::sendKey(third), {});
        QVERIFY(near(studio.level(), 1.0, 1e-3));
        // Deleting a return takes the sends into it along; undo brings them back.
        studio.edit.deleteTracks({ret});
        QVERIFY(near(studio.level(), 0.5));
        QVERIFY(studio.engine.trackSends(*bridge.engineTrackId(track)).empty());
        studio.stack.undo();
        QVERIFY(near(studio.level(), 1.0, 1e-3));
        // A project loaded with returns and sends plays them.
        loadInto(studio.project, projectToJson(studio.project));
        QVERIFY(near(studio.level(), 1.0, 1e-3));
    }

    void aSendLevelChangedByHandOverridesItsAutomation() {
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString track = studio.clipTrack(dcWav(dir));
        const QString ret = studio.edit.addReturnTrack();
        studio.edit.setSend(track, ret, -6.0206);
        const QString key = automation::sendKey(ret);
        studio.edit.setEnvelope(track, key, {{0.0, 0.0, 0.0}});  // silent
        QVERIFY(bridge.isAutomated(track, key));
        QVERIFY(near(studio.level(), 0.5, 1e-3));
        QSignalSpy state(&bridge, &EngineBridge::automationStateChanged);
        studio.edit.setSend(track, ret, -12.0);  // by hand: its automation stops
        QVERIFY(bridge.isOverridden(track, key) && bridge.hasOverrides() && !bridge.isAutomated(track, key));
        QVERIFY(!state.isEmpty() && state.last().first().toString() == track);
        QVERIFY(near(studio.level(), 0.5 + 0.5 * std::pow(10.0, -12.0 / 20.0), 1e-3));  // the send's own level
        bridge.reEnableAutomation(track);
        QVERIFY(!bridge.hasOverrides() && bridge.isAutomated(track, key));
        QVERIFY(near(studio.level(), 0.5, 1e-3));
    }

    void theEngineTakesInputsFromTracks() {
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const auto engineInput = [&](const QString& trackId) {
            return studio.engine.trackInputTrack(*bridge.engineTrackId(trackId));
        };
        const QString source = studio.edit.addAudioTrack();
        const QString group = studio.edit.groupTracks({source});
        const QString track = studio.edit.addAudioTrack();
        studio.edit.setInput(track, {}, group);
        QCOMPARE(engineInput(track), bridge.engineTrackId(group));
        studio.edit.setInput(track, {}, kMaster);
        QCOMPARE(engineInput(track), std::optional<uint32_t>(sub::Engine::kMaster));
        studio.edit.setInput(track, {0}, std::nullopt);
        QCOMPARE(engineInput(track), std::nullopt);
        studio.edit.setInput(track, {}, source);
        QCOMPARE(engineInput(track), bridge.engineTrackId(source));
        // Into the source's group: the input goes before the route that would close the cycle.
        studio.edit.setInput(track, {}, group);
        studio.edit.moveTracks({track}, studio.project.trackIndex(source) + 1, group);
        QCOMPARE(engineInput(track), std::nullopt);
        QCOMPARE(studio.engine.trackOutput(*bridge.engineTrackId(track)), *bridge.engineTrackId(group));
        studio.stack.undo();
        QCOMPARE(engineInput(track), bridge.engineTrackId(group));
        // Deleting the source, and undoing it.
        studio.edit.deleteTracks({group});
        QCOMPARE(engineInput(track), std::nullopt);
        studio.stack.undo();
        QCOMPARE(engineInput(track), bridge.engineTrackId(group));
        // A project loaded with a track taking the output of one listed after it.
        const QString later = studio.edit.addAudioTrack();
        studio.edit.setInput(track, {}, later);
        loadInto(studio.project, projectToJson(studio.project));
        QCOMPARE(engineInput(track), bridge.engineTrackId(later));
    }

    void theMastersMixerAndTheProjectsSettings() {
        test::TempDir dir;
        Studio studio;
        studio.clipTrack(dcWav(dir));
        studio.edit.setTrack(kMaster, TrackField::VolumeDb, -6.0206);
        QVERIFY(near(studio.level(), 0.25, 1e-3));
        studio.edit.setEnvelope(kMaster, automation::kMixerVolume, {{0.0, 0.0, 0.0}});  // silent
        QVERIFY(near(studio.level(), 0.0));
        studio.edit.setEnvelope(kMaster, automation::kMixerVolume, {});  // back to its own value
        QVERIFY(!studio.bridge->isAutomated(kMaster, automation::kMixerVolume));
        QVERIFY(near(studio.level(), 0.25, 1e-3));
        studio.edit.setSettings({{SettingsField::Tempo, 90.0},
                                 {SettingsField::TimeSignature, TimeSignature{6, 8}},
                                 {SettingsField::LoopEnabled, true},
                                 {SettingsField::LoopStart, 2.0},
                                 {SettingsField::LoopEnd, 6.0}});
        QCOMPARE(studio.engine.tempo(), 90.0);
        studio.stack.undo();
        QCOMPARE(studio.engine.tempo(), 120.0);
    }

    void aDragIsHeardWhereItGoes() {
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString wav = dcWav(dir);
        const QString track = studio.clipTrack(wav);
        QVERIFY(near(studio.level(), 0.5));
        Clip moved = test::audioClip(track + QStringLiteral("c"), wav, 100.0);  // dragged far away
        bridge.previewClips({{track, {moved}}});
        QVERIFY(near(studio.level(), 0.0));
        QCOMPARE(studio.project.track(track).clips.front().startBeat, 0.0);  // the model hasn't changed
        bridge.previewClips({});  // previewed before but not now: its own clips again
        QVERIFY(near(studio.level(), 0.5));
        bridge.previewClips({{track, {moved}}});
        bridge.endClipPreview();  // the drop changed nothing
        QVERIFY(near(studio.level(), 0.5));
        // A frozen track plays on as it is.
        Freeze freeze;
        freeze.path = wav;
        freeze.durationSec = 1.0;
        studio.edit.freeze(track, freeze);
        bridge.previewClips({{track, {moved}}});
        QVERIFY(near(studio.level(), 0.5));
        bridge.endClipPreview();
    }

    void theTransportAndPreviews() {
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        QSignalSpy moved(&bridge, &EngineBridge::positionChanged);
        QSignalSpy transport(&bridge, &EngineBridge::transportChanged);
        bridge.locate(4.0);
        QCOMPARE(bridge.position(), 4.0);
        QCOMPARE(moved.last().first().toDouble(), 4.0);
        bridge.locate(-2.0);  // never before the timeline's start
        QCOMPARE(bridge.position(), 0.0);
        bridge.play();
        QVERIFY(bridge.isPlaying() && bridge.property("playing").toBool());
        QCOMPARE(transport.last().first().toBool(), true);
        bridge.stop();
        QVERIFY(!bridge.isPlaying());
        QCOMPARE(transport.last().first().toBool(), false);
        QCOMPARE(transport.count(), 2);  // (only when it changed)
        QSignalSpy metronome(&bridge, &EngineBridge::metronomeChanged);
        bridge.setProperty("metronome", true);
        QVERIFY(bridge.metronome() && studio.engine.metronome() && metronome.count() == 1);
        bridge.setMetronome(true);
        QCOMPARE(metronome.count(), 1);
        bridge.setMetronome(false);
        // A preview decodes the file first; stopped (or another asked for) meanwhile, it isn't played.
        const QString wav = dcWav(dir, QStringLiteral("preview.wav"));
        QSignalSpy ready(&bridge, &EngineBridge::sourceReady);
        bridge.previewFile(wav);
        bridge.stopPreview();
        QVERIFY(ready.wait(5000));
        QVERIFY(!studio.engine.isPreviewing());
        bridge.previewFile(wav);  // decoded: at once (without a device it doesn't sound)
        bridge.stopPreview();
        const QString keys = studio.edit.addMidiTrack();
        bridge.previewNote(keys, 60, 100);  // (heard only while a device runs)
        bridge.previewNote(keys, 60, 0);
        bridge.previewNote(QStringLiteral("nope"), 60, 100);
    }

    void clipsAreDecodedForTheWaveforms() {
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString wav = dcWav(dir, QStringLiteral("decoded.wav"));
        QSignalSpy ready(&bridge, &EngineBridge::sourceReady);
        const QString track = studio.edit.addAudioTrack();
        studio.edit.setClips(track, {test::audioClip(QStringLiteral("c"), wav)});
        QVERIFY(bridge.isLoading(wav) || bridge.source(wav));
        QVERIFY(ready.wait(5000) || bridge.source(wav));
        QCOMPARE(ready.first().first().toString(), wav);
        const Waveform waveform = bridge.waveform(wav);
        QVERIFY(!waveform.isNull());
        QCOMPARE(waveform.frames(), qint64{test::kSampleRate});
        QCOMPARE(waveform.channels(), 2);
        QCOMPARE(waveform.sampleRate(), test::kSampleRate);
        QVERIFY(waveform.peakLevels() > 0 && waveform.peakCount(0) > 0);
        QCOMPARE(waveform.peaks(0)[1], 0.5f);  // the first peak's max
        QCOMPARE(waveform.channelData(0)[100], 0.5f);
        QVERIFY(bridge.waveform(dir.path("missing.wav")).isNull());
        // A file that can't be decoded: said so, and known.
        QSignalSpy failed(&bridge, &EngineBridge::sourceFailed);
        const QString missing = dir.path("missing.wav");
        bridge.requestSource(missing);
        QVERIFY(failed.wait(5000));
        QVERIFY(!bridge.loadError(missing).isEmpty() && !bridge.isLoading(missing));
        // Its header: length and format.
        const auto info = bridge.fileInfo(wav);
        QVERIFY(info && info->frames == test::kSampleRate && info->channels == 2 && near(info->duration, 1.0));
        QVERIFY(!bridge.fileInfo(missing));
        // A file asked for again: once decoded, at once.
        bool called = false;
        bridge.requestSource(wav, [&] { called = true; });
        QVERIFY(called);
        // Opening another project forgets what it doesn't use.
        studio.project.clear();
        QVERIFY(!bridge.source(wav));
    }
};

QTEST_GUILESS_MAIN(TestBridgeTracks)
#include "test_bridge_tracks.moc"
