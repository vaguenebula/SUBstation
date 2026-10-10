// Freezing through the bridge: a frozen track plays its frozen audio without
// its devices (and gets them back, plug-ins as they were), where a time
// selection moved it (and where a drag would take it), a frozen MIDI track,
// a frozen group, nothing to freeze, renders no track plays deleted; renders in
// the background (a freeze, an export) followed through their signals and
// cancelled; reversed copies of files, written on a thread of their own and
// found again. The changes are made as the editor makes them.

#include "BridgeTestSupport.h"
#include "TestSupport.h"

#include "audio/AudioFiles.h"
#include "editor/ProjectEditor.h"
#include "io/Serialization.h"
#include "model/Ids.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <cmath>

using namespace sub::app;
using sub::app::test::Studio;

namespace {

bool allNear(const std::vector<float>& samples, float value, float tolerance, size_t from = 0) {
    for (size_t i = from; i < samples.size(); ++i) {
        if (std::abs(samples[i] - value) > tolerance) return false;
    }
    return true;
}

// The last 100 frames of 4000, both channels.
std::vector<float> level(Studio& studio) {
    const std::vector<float> out = studio.render(4000);
    return {out.end() - 200, out.end()};
}

bool allClose(const std::vector<float>& a, const std::vector<float>& b, float tolerance) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::abs(a[i] - b[i]) > tolerance) return false;
    }
    return true;
}

QString dcWav(const test::TempDir& dir, const QString& name = QStringLiteral("dc.wav")) {
    return test::writeWav(dir.path(name), std::vector<float>(2 * test::kSampleRate, 0.5f), 2);
}

}  // namespace

class TestBridgeFreeze : public QObject {
    Q_OBJECT

    QString freezeDir() const { return QDir(qEnvironmentVariable("SUBSTATION_RECORDINGS")).filePath("Freeze"); }

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }

    void aFrozenTrackPlaysItsFrozenAudioWithoutItsDevices() {
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString track = studio.clipTrack(dcWav(dir), 1.0, QStringLiteral("A"));
        const QString utility = studio.edit.addDevice(track, QStringLiteral("utility"));
        studio.edit.setDeviceParam(track, utility, QStringLiteral("gain"), -6.0206);  // halves it
        bridge.waitForDeviceStates();
        level(studio);
        QVERIFY(allNear(level(studio), 0.25f, 1e-4f));

        const Freeze freeze = bridge.renderFreeze(track);
        QCOMPARE(freeze.tempo, studio.project.tempo());
        QVERIFY(freeze.durationSec >= studio.project.endBeat() / 2);
        QVERIFY(QFileInfo(freeze.path).fileName().startsWith("A Freeze "));
        QCOMPARE(QFileInfo(freeze.path).absolutePath(), QFileInfo(freezeDir()).absoluteFilePath());
        QVERIFY(bridge.source(freeze.path));  // decoded at once: no gap while it loads
        studio.edit.freeze(track, freeze);
        QVERIFY(!bridge.engineDeviceId(track, utility));  // unloaded
        QVERIFY(studio.engine.trackFrozen(*bridge.engineTrackId(track)));
        QVERIFY(allNear(level(studio), 0.25f, 1e-4f));
        studio.edit.setTrack(track, TrackField::VolumeDb, -6.0206);  // its fader stays live
        QVERIFY(allNear(level(studio), 0.125f, 1e-4f));

        studio.edit.unfreeze(track);  // its devices come back as they were
        QVERIFY(bridge.engineDeviceId(track, utility));
        QVERIFY(!studio.engine.trackFrozen(*bridge.engineTrackId(track)));
        bridge.waitForDeviceStates();
        level(studio);
        QVERIFY(allNear(level(studio), 0.125f, 1e-4f));

        studio.stack.undo();  // frozen again, then flattened: its frozen audio as a clip
        const Track& frozen = studio.project.track(track);
        Track flat = frozen;
        Clip clip = frozen.frozen->clip(track, frozen.name);
        clip.id = newId();
        flat.kind = kAudioKind;
        flat.clips = {clip};
        flat.devices.clear();
        flat.frozen.reset();
        studio.stack.push(new ReplaceTrackCommand(&studio.project, frozen, flat, QStringLiteral("Flatten Track")));
        QVERIFY(studio.project.track(track).devices.empty());
        QVERIFY(allNear(level(studio), 0.125f, 1e-4f));
    }

    void aFrozenTrackPlaysItsFrozenAudioWhereATimeSelectionTookIt() {
        // Moving a stretch of a frozen track moves what of its frozen audio
        // plays (its segments) with its clips; a drag's preview plays them where
        // it would take them; undoing puts them back.
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        ProjectEditor editor(&studio.project, &studio.stack);
        const QString track = studio.clipTrack(dcWav(dir), 1.0, QStringLiteral("A"));  // beats 0-2
        const QString utility = studio.edit.addDevice(track, QStringLiteral("utility"));
        studio.edit.setDeviceParam(track, utility, QStringLiteral("gain"), -6.0206);  // halves it
        bridge.waitForDeviceStates();
        QCOMPARE(editor.freezeTracks({{track, bridge.renderFreeze(track)}}), QStringList{track});
        const auto heard = [&](double beat) {
            const std::vector<float> out = studio.render(4000, beat);
            return std::vector<float>(out.end() - 200, out.end());
        };
        heard(0.5);
        QVERIFY(allNear(heard(0.5), 0.25f, 1e-4f));
        editor.moveRange(0.0, 2.0, {track}, 4.0);
        QCOMPARE(studio.project.track(track).clips[0].startBeat, 4.0);
        QVERIFY(allNear(heard(0.5), 0.0f, 1e-6f));
        QVERIFY(allNear(heard(4.5), 0.25f, 1e-4f));
        // While a drag goes on, it plays where the drag would take it.
        const MovedRange moved = editor.movedRange(4.0, 6.0, {track}, 4.0);
        bridge.previewClips(moved.clips, moved.frozen);
        QVERIFY(allNear(heard(4.5), 0.0f, 1e-6f));
        QVERIFY(allNear(heard(8.5), 0.25f, 1e-4f));
        bridge.endClipPreview();
        QVERIFY(allNear(heard(4.5), 0.25f, 1e-4f));
        QVERIFY(allNear(heard(8.5), 0.0f, 1e-6f));
        studio.stack.undo();  // (one step: back where it was)
        QVERIFY(allNear(heard(0.5), 0.25f, 1e-4f));
        QVERIFY(allNear(heard(4.5), 0.0f, 1e-6f));
    }

    void aFrozenMidiTrackPlaysItsFrozenAudio() {
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString track = studio.edit.addMidiTrack(QStringLiteral("Keys"));
        studio.edit.setClips(track, {Clip::midi(QStringLiteral("m"), QStringLiteral("m"), 0.0, 2.0, 0.0,
                                                {Note{60, 0.0, 1.0, 100}})});
        bridge.waitForDeviceStates();
        const std::vector<float> before = studio.render(test::kSampleRate);
        QVERIFY(test::peak(before) > 0.01f);
        studio.edit.freeze(track, bridge.renderFreeze(track));
        QVERIFY(allClose(studio.render(test::kSampleRate), before, 1e-5f));
        studio.edit.unfreeze(track);  // its notes again, not its frozen audio as well
        bridge.waitForDeviceStates();
        QVERIFY(allClose(studio.render(test::kSampleRate), before, 1e-5f));
    }

    void whatIsInAFrozenGroupKeepsItsDevices() {
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString track = studio.clipTrack(dcWav(dir));
        const QString utility = studio.edit.addDevice(track, QStringLiteral("utility"));
        const QString group = studio.edit.groupTracks({track});
        studio.edit.freeze(group, bridge.renderFreeze(group));
        QVERIFY(studio.engine.trackFrozen(*bridge.engineTrackId(group)));
        QVERIFY(!studio.engine.trackFrozen(*bridge.engineTrackId(track)));
        QVERIFY(bridge.engineDeviceId(track, utility));  // (the engine just doesn't render it)
        QVERIFY(allNear(level(studio), 0.5f, 1e-4f));
    }

    void nothingToFreeze() {
        Studio studio;
        const QString track = studio.edit.addAudioTrack();
        try {
            studio.bridge->renderFreeze(track);
            QFAIL("froze an empty arrangement");
        } catch (const EditError& error) {
            QVERIFY(error.message().contains("nothing to freeze"));
        }
        try {
            studio.bridge->startFreeze(kMaster);
            QFAIL("froze the master");
        } catch (const EditError& error) {
            QVERIFY(error.message().contains("can't be frozen"));
        }
    }

    void aPluginComesBackAsItWas() {
        const auto ref = test::testPlugin(test::testPluginsBundle(), QStringLiteral("SUB Test Effect"));
        if (!ref) QSKIP("test plug-ins not built");
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString track = studio.clipTrack(dcWav(dir), 1.0, QStringLiteral("A"));
        const QString effect = studio.edit.addDevice(track, kPluginKind, ref);
        studio.engine.setProcessorParam(*bridge.engineDeviceId(track, effect), 0, 0.25f);  // only the plug-in knows
        studio.edit.freeze(track, bridge.renderFreeze(track));
        QVERIFY(!bridge.engineDeviceId(track, effect));
        QVERIFY(studio.project.device(track, effect).state.has_value());  // (stored with the project meanwhile)
        studio.edit.unfreeze(track);
        const auto processor = bridge.engineDeviceId(track, effect);
        QVERIFY(processor && std::abs(studio.engine.processorParam(*processor, 0) - 0.25f) < 1e-6f);
    }

    void rendersNoTrackPlaysAreDeleted() {
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString track = studio.clipTrack(dcWav(dir));
        const Freeze freeze = bridge.renderFreeze(track);
        QVERIFY(QFileInfo::exists(freeze.path) && bridge.source(freeze.path));
        bridge.discardFreeze(freeze);
        QVERIFY(!QFileInfo::exists(freeze.path) && !bridge.source(freeze.path));
    }

    void freezingInTheBackground() {
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        const QString track = studio.clipTrack(dcWav(dir), 1.0, QStringLiteral("A"));
        QVERIFY(bridge.devicesReady());
        std::unique_ptr<FreezeRender> render = bridge.startFreeze(track);
        QCOMPARE(render->trackId(), track);
        QVERIFY(studio.engine.isRendering());
        QSignalSpy ended(render.get(), &RenderTask::ended);
        QVERIFY(ended.wait(10000) || render->done());
        QCOMPARE(render->progress(), 1.0);
        const std::optional<Freeze> freeze = bridge.finishFreeze(*render);
        QVERIFY(freeze && QFileInfo::exists(freeze->path));
        QVERIFY(!studio.engine.isRendering());
        studio.edit.freeze(track, *freeze);
        QVERIFY(allNear(level(studio), 0.5f, 1e-4f));
        studio.edit.unfreeze(track);

        // Over an hour to render: cancelled, it freezes nothing and leaves no file.
        studio.edit.setClips(track, {test::audioClip(QStringLiteral("late"), dcWav(dir, "late.wav"), 10000.0)});
        std::unique_ptr<FreezeRender> slow = bridge.startFreeze(track);
        QSignalSpy cancelled(slow.get(), &RenderTask::cancelledChanged);
        BridgeTestAccess::pollMeters(bridge);  // what the UI polls doesn't wait for the render
        slow->cancel();
        QVERIFY(slow->cancelled() && cancelled.count() == 1);
        QSignalSpy slowEnded(slow.get(), &RenderTask::ended);
        QVERIFY(slowEnded.wait(10000) || slow->done());
        QVERIFY(!bridge.finishFreeze(*slow));
        QVERIFY(!QFileInfo::exists(slow->path()));
        QVERIFY(!studio.engine.isRendering());
        QCOMPARE(studio.render(100).size(), size_t{200});
    }

    void exportingInTheBackground() {
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        studio.clipTrack(dcWav(dir));
        const QString target = dir.path("mix.wav");
        std::unique_ptr<EngineRender> render = bridge.startExport(target, 0.0, studio.project.endBeat(), 24);
        QSignalSpy progressed(render.get(), &RenderTask::progressChanged);
        QSignalSpy ended(render.get(), &RenderTask::ended);
        QVERIFY(ended.wait(10000) || render->done());
        QVERIFY(!progressed.isEmpty());
        const std::optional<qint64> frames = bridge.finishExport(*render);
        QVERIFY(frames && *frames > 0);
        QVERIFY(render->finished());
        const auto exported = studio.engine.loadSource(target.toStdString());
        QVERIFY(std::abs(exported->channelData(0)[1000] - 0.5f) < 1e-3f);

        // Cancelled: no file.
        studio.edit.setClips(studio.project.tracks().front().id,
                             {test::audioClip(QStringLiteral("late"), dcWav(dir, "late.wav"), 10000.0)});
        std::unique_ptr<EngineRender> slow = bridge.startExport(dir.path("long.wav"), 0.0, studio.project.endBeat(), 16);
        slow->cancel();
        QSignalSpy slowEnded(slow.get(), &RenderTask::ended);
        QVERIFY(slowEnded.wait(10000) || slow->done());
        QVERIFY(!bridge.finishExport(*slow));
        QVERIFY(!QFileInfo::exists(dir.path("long.wav")));
        try {
            bridge.finishExport(*slow);
            QFAIL("finished twice");
        } catch (const EditError&) {
        }
    }

    void aFileReversedMustBeDecoded() {
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        std::vector<float> ramp(2 * test::kSampleRate);
        for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i % 32768) / 32768.f;
        const QString path = test::writeWav(dir.path("ramp.wav"), ramp, 1);
        try {
            bridge.startReversed(path);
            QFAIL("reversed a file that isn't decoded");
        } catch (const EditError& error) {
            QCOMPARE(error.message(), QStringLiteral("ramp.wav is still loading: it can't be reversed"));
        }
        QSignalSpy failed(&bridge, &EngineBridge::sourceFailed);
        const QString missing = dir.path("missing.wav");
        bridge.requestSource(missing);
        QVERIFY(failed.wait(5000));
        try {
            bridge.startReversed(missing);
            QFAIL("reversed a file that can't be decoded");
        } catch (const EditError& error) {
            QCOMPARE(error.message(), QStringLiteral("missing.wav can't be found: it can't be reversed"));
        }
        QVERIFY(!bridge.reversedCopy(path));  // none made yet
    }

    void reversedCopiesAreWrittenAndFoundAgain() {
        test::TempDir dir;
        Studio studio;
        EngineBridge& bridge = *studio.bridge;
        std::vector<float> ramp(2 * test::kSampleRate);
        for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i % 32768) / 32768.f;
        const QString path = test::writeWav(dir.path("ramp.wav"), ramp, 1);
        QSignalSpy ready(&bridge, &EngineBridge::sourceReady);
        bridge.requestSource(path);
        QVERIFY(ready.wait(5000));

        std::unique_ptr<ReverseJob> job = bridge.startReversed(path);
        QCOMPARE(QFileInfo(job->path()).fileName(), QStringLiteral("ramp R.wav"));
        QCOMPARE(job->frames(), qint64(ramp.size()));
        QCOMPARE(job->seconds(), 2.0);
        QSignalSpy ended(job.get(), &RenderTask::ended);
        QVERIFY(ended.wait(10000) || job->done());
        const auto reversed = bridge.finishReversed(path, *job);
        QVERIFY(reversed);
        QCOMPARE(reversed->first, job->path());
        QCOMPARE(reversed->second, 2.0);
        const auto copy = bridge.source(reversed->first);  // decoded: it plays without a gap
        QVERIFY(copy && copy->frames() == static_cast<int64_t>(ramp.size()));
        const float* samples = copy->channelData(0);
        bool backwards = true;
        for (size_t i = 0; i < ramp.size(); i += 997) {
            const float expected = static_cast<float>(std::nearbyint(ramp[ramp.size() - 1 - i] * 32768.0)) / 32768.f;
            backwards = backwards && std::abs(samples[i] - expected) < 1e-6f;
        }
        QVERIFY(backwards);
        QCOMPARE(*bridge.reversedCopy(path), reversed->first);  // given again for this file
        QCOMPARE(bridge.renderReversed(path).first, reversed->first);

        // Written a chunk at a time (that one was one chunk): the same copy. (Not
        // started, finishing it starts it.)
        ReverseJob chunked(studio.engine, bridge.source(path), dir.path("chunked.wav"), 1000);
        const std::shared_ptr<sub::AudioSource> chunks = chunked.finish();
        QVERIFY(chunks && chunks->frames() == copy->frames());
        QVERIFY(std::equal(chunks->data(), chunks->data() + chunks->frames(), copy->data()));

        // Cancelled before it starts (started at once, a short copy could be
        // written before the cancel reached it): its file goes.
        auto cancelled = std::make_unique<ReverseJob>(studio.engine, bridge.source(path),
                                                      reversedPath(reversedFolder(studio.project), path));
        const QString gone = cancelled->path();
        QCOMPARE(QFileInfo(gone).fileName(), QStringLiteral("ramp R 2.wav"));
        cancelled->cancel();
        cancelled->start();
        QVERIFY(!bridge.finishReversed(path, *cancelled));
        QVERIFY(!QFileInfo::exists(gone));

        // After reopening, a clip of the project playing the copy finds it again.
        Clip clip = test::audioClip(QStringLiteral("r"), reversed->first);
        clip.reversedFrom = path;
        const QString track = studio.edit.addAudioTrack();
        studio.edit.setClips(track, {clip});
        loadInto(studio.project, projectToJson(studio.project));  // (forgets the copies made this session)
        QCOMPARE(*bridge.reversedCopy(path), reversed->first);
    }
};

QTEST_GUILESS_MAIN(TestBridgeFreeze)
#include "test_bridge_freeze.moc"
