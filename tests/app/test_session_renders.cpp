// Renders in the background through the session: exporting and freezing,
// their progress (RenderProgress, the UI's modal dialog) and Cancel, closing
// while one runs, the window going on meanwhile; freezing and unfreezing the
// selected tracks (Ctrl+Shift+F), what the device view says of a frozen track,
// the track menu's freeze actions, flattening; and a project's plug-ins
// loading after it opens (the selected track's first; renders wait for them;
// saved meanwhile, they keep their state).

#include "BridgeTestSupport.h"
#include "SessionFixture.h"
#include "TestSupport.h"

#include "AudioSource.h"
#include "model/Clip.h"
#include "session/DeviceSelection.h"
#include "session/RenderProgress.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTest>

using namespace sub::app;
using sub::app::test::SessionFixture;
using sub::app::test::TempDir;

namespace {

constexpr int kRate = test::kSampleRate;

// The renders' progress as it goes: "title: label", each once.
struct Following {
    explicit Following(RenderProgress& progress, const QString& prefix) {
        QObject::connect(&progress, &RenderProgress::changed, [this, &progress, prefix] {
            if (!progress.label().startsWith(prefix)) return;
            const QString entry = progress.title() + QStringLiteral(": ") + progress.label();
            if (seen.isEmpty() || seen.back() != entry) seen.append(entry);
            if (entry == cancelAt) progress.cancel();
        });
    }
    QStringList seen;
    QString cancelAt;  // cancel when this shows
};

// One second of a constant 0.5 in both channels.
QString dcWav(const TempDir& dir, const QString& name = QStringLiteral("dc.wav")) {
    return test::writeWav(dir.path(name), std::vector<float>(2 * kRate, 0.5f), 2);
}

// An audio track playing `wav` from `startBeat`, with a utility.
QString freezable(SessionFixture& f, const QString& wav, const QString& name = QStringLiteral("A"),
                  double startBeat = 0.0) {
    const QString track = f.clipTrack(wav, 1.0, name, startBeat);
    f.editor().addDevice(track, QStringLiteral("utility"));
    return track;
}

QStringList freezeFiles(const TempDir& recordings) {
    return QDir(recordings.path(QStringLiteral("Freeze"))).entryList({QStringLiteral("*.wav")}, QDir::Files);
}

}  // namespace

class TestSessionRenders : public QObject {
    Q_OBJECT

private:
    std::unique_ptr<TempDir> recordings_;
    std::unique_ptr<test::ScopedEnv> recordingsEnv_;

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }
    void init() {
        QSettings().clear();
        recordings_ = std::make_unique<TempDir>();
        recordingsEnv_ = std::make_unique<test::ScopedEnv>("SUBSTATION_RECORDINGS", recordings_->path());
    }
    void cleanup() {
        recordingsEnv_.reset();
        recordings_.reset();
    }

    // --- Exporting ---

    void exportingShowsItsProgressAndWritesTheFile() {
        SessionFixture f(true);
        Session& s = f.s();
        TempDir dir;
        f.clipTrack(dcWav(dir));
        Following following(*s.render(), QStringLiteral("Exporting"));
        const QString target = dir.path(QStringLiteral("mix.wav"));
        QVERIFY(s.exportAudio(target, QStringLiteral("arrangement"), 24));
        QVERIFY(s.render()->active() && s.rendering());
        QCOMPARE(s.render()->title(), QStringLiteral("Export Audio"));
        QVERIFY(f.waitForRender());
        QCOMPARE(following.seen, QStringList{QStringLiteral("Export Audio: Exporting mix.wav…")});
        QVERIFY(QFileInfo::exists(target));
        QCOMPARE(f.lastMessage(), QStringLiteral("Exported mix.wav"));
        QVERIFY(!s.rendering() && !f.engine.isRendering());
        const auto source = f.engine.loadSource(target.toStdString());
        QCOMPARE(source->frames(), int64_t{kRate});
        QVERIFY(std::abs(source->channelData(1)[1000] - 0.5f) < 1e-4f);
    }

    // A time selection, as an MP3: the whole mix over its time (whichever tracks
    // it is on), at the bitrate asked for, LAME's info tag first.
    void exportingATimeSelectionAsMp3() {
        SessionFixture f(true);
        Session& s = f.s();
        TempDir dir;
        f.clipTrack(dcWav(dir));  // a second: two beats at 120 BPM
        f.editor().addMidiTrack();
        QCOMPARE(s.exportProblem(QStringLiteral("selection")),
                 QStringLiteral("There is no time selection to export: select a time range in the arrangement first."));
        f.selection().setTimeRange(0.5, 1.5, {f.project().tracks().back().id});  // (on the other track: it is time)
        QCOMPARE(s.exportProblem(QStringLiteral("selection")), QString());
        QCOMPARE(QFileInfo(s.suggestedExportPath(QStringLiteral("mp3"))).fileName(), QStringLiteral("Untitled.mp3"));
        const QString target = dir.path(QStringLiteral("part.mp3"));
        QVERIFY(s.exportAudio(target, QStringLiteral("selection"), 24, QStringLiteral("mp3"), 192));
        QVERIFY(f.waitForRender());
        QCOMPARE(f.lastMessage(), QStringLiteral("Exported part.mp3"));

        QFile file(target);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QVERIFY(bytes.size() > 1000);
        QCOMPARE(quint8(bytes[0]), quint8(0xFF));  // an MPEG audio frame's sync,
        QCOMPARE(quint8(bytes[1]) & 0xFE, 0xFA);   // MPEG-1 Layer III,
        QCOMPARE((quint8(bytes[2]) >> 4) & 0xF, 0xB);  // 192 kbps
        QVERIFY(bytes.left(200).contains("Info") && bytes.left(400).contains("LAME"));  // (CBR: "Info")

        // Half a second of the mix (a beat at 120 BPM), as an MP3 holds it: up to
        // a frame or two more for the encoder's delay and padding.
        const auto source = f.engine.loadSource(target.toStdString());
        QVERIFY2(source->frames() >= kRate / 2 && source->frames() <= kRate / 2 + 3 * 1152,
                 qPrintable(QString::number(source->frames())));
        double sum = 0.0;
        const int64_t middle = source->frames() / 2;
        for (int64_t i = middle - 1000; i < middle + 1000; ++i) sum += source->channelData(0)[i];
        QVERIFY2(std::abs(sum / 2000.0 - 0.5) < 0.05, qPrintable(QString::number(sum / 2000.0)));
    }

    void playingStopsBeforeExporting() {
        SessionFixture f(true);
        Session& s = f.s();
        TempDir dir;
        f.clipTrack(dcWav(dir));
        f.selection().setInsert(1.0);
        s.togglePlay();
        QVERIFY(f.bridge().isPlaying());
        QVERIFY(s.exportAudio(dir.path(QStringLiteral("mix.wav")), QStringLiteral("arrangement"), 16));
        QVERIFY(!f.bridge().isPlaying());
        QCOMPARE(f.bridge().position(), 1.0);  // (where playback started)
        QVERIFY(f.waitForRender());
    }

    void theWindowGoesOnWhileItRendersAndCancelStopsIt() {
        SessionFixture f(true);
        Session& s = f.s();
        TempDir dir;
        f.clipTrack(dcWav(dir), 1.0, QStringLiteral("A"), 10'000.0);  // over an hour to render
        const QString target = dir.path(QStringLiteral("mix.wav"));
        QVERIFY(s.exportAudio(target, QStringLiteral("arrangement"), 24));
        QTest::qWait(50);
        QVERIFY(f.engine.isRendering() && s.render()->active());
        BridgeTestAccess::pollMeters(f.bridge());  // what the window polls doesn't wait for the render
        QVERIFY(!s.exportAudio(dir.path(QStringLiteral("other.wav")), QStringLiteral("arrangement"), 24));  // one at a time
        s.render()->cancel();  // Cancel
        QVERIFY(s.render()->cancelled());
        QCOMPARE(s.render()->label(), QStringLiteral("Cancelling…"));
        QVERIFY(f.waitForRender());
        QVERIFY(!QFileInfo::exists(target));  // (what was written of it is gone)
        QCOMPARE(f.lastMessage(), QStringLiteral("Export cancelled"));
        QVERIFY(!f.engine.isRendering());
        QCOMPARE(f.engine.renderOffline(0.0, 100).size(), size_t{200});
    }

    void closingTheWindowWhileItRendersCancelsTheRender() {
        SessionFixture f(true);
        Session& s = f.s();
        TempDir dir;
        f.clipTrack(dcWav(dir), 1.0, QStringLiteral("A"), 10'000.0);
        const QString target = dir.path(QStringLiteral("mix.wav"));
        QVERIFY(s.exportAudio(target, QStringLiteral("arrangement"), 24));
        QVERIFY(!s.requestClose());  // (the window stays: the render is cancelled)
        QVERIFY(f.waitForRender());
        QCOMPARE(f.lastMessage(), QStringLiteral("Export cancelled"));
        QVERIFY(!QFileInfo::exists(target));
        QVERIFY(s.requestClose());
    }

    // --- Freezing ---

    void freezingShowsItsProgressAndCancelFreezesNothing() {
        SessionFixture f(true);
        Session& s = f.s();
        TempDir dir;
        const QString wav = dcWav(dir);
        const QString a = f.clipTrack(wav, 1.0, QStringLiteral("A"));
        const QString b = f.clipTrack(wav, 1.0, QStringLiteral("B"));
        f.selection().selectTrack(a, true);
        f.selection().selectTrack(b, false, Selection::Mode::Toggle);
        Following following(*s.render(), QStringLiteral("Freezing"));
        following.cancelAt = QStringLiteral("Freeze Tracks: Freezing B (2 of 2)…");  // A's is done
        const QString undoText = f.stack().undoText();
        s.toggleFreeze();  // Ctrl+Shift+F
        QVERIFY(f.waitForRender());
        QCOMPARE(following.seen, (QStringList{QStringLiteral("Freeze Tracks: Freezing A (1 of 2)…"),
                                              QStringLiteral("Freeze Tracks: Freezing B (2 of 2)…")}));
        QVERIFY(!f.project().track(a).frozen && !f.project().track(b).frozen);
        QCOMPARE(f.stack().undoText(), undoText);
        QVERIFY(freezeFiles(*recordings_).isEmpty());  // A's render too: no track plays it

        following.cancelAt.clear();
        s.toggleFreeze();
        QVERIFY(f.waitForRender());
        QVERIFY(f.project().track(a).frozen && f.project().track(b).frozen);
        QCOMPARE(f.stack().undoText(), QStringLiteral("Freeze Tracks"));
        QCOMPARE(f.lastMessage(), QStringLiteral("Froze A, B"));
    }

    void ctrlShiftFFreezesAndUnfreezesTheSelectedTrack() {
        SessionFixture f(true);
        Session& s = f.s();
        TempDir dir;
        const QString track = freezable(f, dcWav(dir));
        f.selection().selectTrack(track, true);
        QCOMPARE(s.deviceSelection()->shownDevices().size(), 1);  // its utility
        s.toggleFreeze();
        QVERIFY(f.waitForRender());
        QVERIFY(f.project().track(track).frozen);
        const QStringList files = freezeFiles(*recordings_);
        QCOMPARE(files.size(), 1);
        QVERIFY(files[0].startsWith(QStringLiteral("A Freeze ")));
        QVERIFY(s.deviceSelection()->shownDevices().isEmpty());
        QVERIFY(s.deviceSelection()->hint().contains(QStringLiteral("frozen")));
        QVERIFY(f.lastMessage().startsWith(QStringLiteral("Froze A")));
        s.toggleFreeze();
        QVERIFY(!f.project().track(track).frozen);
        QCOMPARE(s.deviceSelection()->shownDevices().size(), 1);
        QCOMPARE(f.lastMessage(), QStringLiteral("Unfroze A"));
        f.stack().undo();
        QVERIFY(f.project().track(track).frozen);
    }

    void aFrozenTrackSaysWhyAnEditIsntMade() {
        SessionFixture f(true);
        Session& s = f.s();
        TempDir dir;
        const QString track = freezable(f, dcWav(dir));
        f.selection().selectTrack(track, true);
        s.toggleFreeze();
        QVERIFY(f.waitForRender());
        f.editor().deleteClips({{track, QStringLiteral("Ac")}});
        QVERIFY(f.lastMessage().contains(QStringLiteral("unfreeze")));
        QCOMPARE(f.project().track(track).clips.size(), size_t{1});
    }

    void whatIsInAFrozenGroupShowsNoDevices() {
        SessionFixture f(true);
        Session& s = f.s();
        TempDir dir;
        const QString track = freezable(f, dcWav(dir));
        const QString group = f.editor().groupTracks({track});
        f.selection().selectTrack(group, true);
        s.toggleFreeze();
        QVERIFY(f.waitForRender());
        QVERIFY(f.project().track(group).frozen);
        f.selection().selectTrack(track, true);
        QVERIFY(s.deviceSelection()->shownDevices().isEmpty());
        QVERIFY(s.deviceSelection()->hint().contains(f.project().track(group).name));
    }

    void theTrackMenuFreezesAndFlattens() {
        SessionFixture f(true);
        Session& s = f.s();
        TempDir dir;
        const QString track = freezable(f, dcWav(dir));
        QVariantMap actions = s.freezeActions({track});
        QCOMPARE(actions.value(QStringLiteral("freezeText")).toString(), QStringLiteral("Freeze Track"));
        QVERIFY(actions.value(QStringLiteral("freezeEnabled")).toBool());
        QVERIFY(!actions.value(QStringLiteral("flattenEnabled")).toBool());
        s.freezeTracks({track});
        QVERIFY(f.waitForRender());
        QVERIFY(f.project().track(track).frozen);
        actions = s.freezeActions({track});
        QCOMPARE(actions.value(QStringLiteral("freezeText")).toString(), QStringLiteral("Unfreeze Track"));
        QVERIFY(actions.value(QStringLiteral("unfreeze")).toBool() && actions.value(QStringLiteral("flattenEnabled")).toBool());
        QCOMPARE(s.unfreezeTracks({track}), QStringList{track});
        QVERIFY(!f.project().track(track).frozen);
        // For several tracks.
        const QString other = f.editor().addAudioTrack();
        actions = s.freezeActions({track, other});
        QCOMPARE(actions.value(QStringLiteral("freezeText")).toString(), QStringLiteral("Freeze Tracks"));
        QCOMPARE(actions.value(QStringLiteral("flattenText")).toString(), QStringLiteral("Flatten Tracks"));
    }

    void flatteningAMidiTrack() {
        SessionFixture f(true);
        Session& s = f.s();
        const QString track = f.editor().addMidiTrack(-1, QStringLiteral("Keys"));
        f.editor().commitClips(QStringLiteral("Add"),
                               {{track, {Clip::midi(QStringLiteral("m"), QStringLiteral("m"), 0.0, 2.0, 0.0, {Note{60, 0.0, 1.0}})}}});
        f.selection().selectTrack(track, true);
        s.toggleFreeze();
        QVERIFY(f.waitForRender());
        s.flattenSelectedTracks();
        const Track& flat = f.project().track(track);
        QVERIFY(flat.isAudio() && flat.devices.empty() && flat.clips.size() == 1);
        QCOMPARE(f.selection().trackId(), track);  // selected again
        QCOMPARE(f.lastMessage(), QStringLiteral("Flattened Keys"));
        f.stack().undo();
        QVERIFY(f.project().track(track).isMidi());
        // Only frozen audio and MIDI tracks flatten: said so.
        f.stack().undo();  // (unfrozen)
        s.flattenSelectedTracks();
        QVERIFY(f.project().track(track).isMidi());
        QVERIFY(!f.lastMessage().isEmpty());
    }

    void nothingToFreezeIsSaid() {
        SessionFixture f(true);
        Session& s = f.s();
        const QString keys = f.editor().addMidiTrack();  // nothing to freeze yet: an empty arrangement
        f.selection().selectTrack(keys, true);
        s.toggleFreeze();
        QVERIFY(f.waitForRender());
        QVERIFY(!f.project().track(keys).frozen);
        QVERIFY(f.lastMessage().contains(QStringLiteral("could not be frozen")));
    }

    void rendersNoTrackPlaysAreDeleted() {
        SessionFixture f;
        Session& s = f.s();
        TempDir dir;
        const QString wav = dcWav(dir);
        const QString a = f.clipTrack(wav, 1.0, QStringLiteral("A"));
        const QString b = f.clipTrack(wav, 1.0, QStringLiteral("B"));
        std::vector<Freeze> rendered;
        s.setFreezeFinisher([&](FreezeRender& render) -> std::optional<Freeze> {
            if (render.trackId() == b) {
                render.cancel();
                f.bridge().finishFreeze(render);
                throw EditError(QStringLiteral("Could not write it"));
            }
            const auto freeze = f.bridge().finishFreeze(render);
            rendered.push_back(*freeze);
            return freeze;
        });
        s.freezeTracks({a, b});
        QVERIFY(f.waitForRender());
        QCOMPARE(rendered.size(), size_t{1});
        QVERIFY(!QFileInfo::exists(rendered[0].path));  // A's render, played by no track
        QVERIFY(!f.project().track(a).frozen);
        QVERIFY(f.lastMessage().contains(QStringLiteral("could not be frozen")));
    }

    void closingWhileFreezingFreezesNothing() {
        SessionFixture f;
        Session& s = f.s();
        TempDir dir;
        const QString a = f.clipTrack(dcWav(dir), 1.0, QStringLiteral("A"), 10'000.0);
        s.freezeTracks({a});
        QVERIFY(s.rendering());
        s.shutdown();  // (the application ends: what it rendered goes)
        QVERIFY(!s.rendering() && !s.render()->active());
        QVERIFY(!f.project().track(a).frozen);
        QVERIFY(freezeFiles(*recordings_).isEmpty());
    }

    // --- A project's plug-ins loading ---

    void aProjectsPluginsLoadAfterItOpensTheSelectedTracksFirst() {
        const QString bundle = test::testPluginsBundle();
        if (!test::haveTestPlugins(bundle)) QSKIP("test plug-ins not built");
        SessionFixture f;
        Session& s = f.s();
        TempDir dir;
        const QString path = savedWithPlugins(f, dir, {QStringLiteral("A"), QStringLiteral("B"), QStringLiteral("C")});
        QVERIFY(s.openProject(path));
        BridgeTestAccess::stopPluginTimer(f.bridge());  // (one at a time, by hand here)
        const QString a = f.project().tracks()[0].id, b = f.project().tracks()[1].id, c = f.project().tracks()[2].id;
        const auto loaded = [&](const QString& track) {
            return f.bridge().engineDeviceId(track, f.project().track(track).devices[0].id).has_value();
        };
        QCOMPARE(f.bridge().pluginsPending(), 3);
        QCOMPARE(s.pluginsLoadingText(), QStringLiteral("Loading plug-ins: 0 of 3"));
        f.selection().selectTrack(c);
        BridgeTestAccess::loadNextPlugin(f.bridge());
        QVERIFY(!loaded(a) && !loaded(b) && loaded(c));
        QCOMPARE(s.pluginsLoadingText(), QStringLiteral("Loading plug-ins: 1 of 3"));
        // Its editor asked for: that one now.
        f.bridge().requestPluginEditor(b, f.project().track(b).devices[0].id);
        QVERIFY(loaded(b));
        QCOMPARE(f.bridge().pluginsPending(), 1);
        BridgeTestAccess::loadNextPlugin(f.bridge());
        QVERIFY(loaded(a));
        QCOMPARE(f.bridge().pluginsPending(), 0);
        QCOMPARE(s.pluginsTotal(), 0);
        QVERIFY(s.pluginsLoadingText().isEmpty());  // (hidden)
    }

    void aPluginWaitingToLoadMayMoveOrGo() {
        const QString bundle = test::testPluginsBundle();
        if (!test::haveTestPlugins(bundle)) QSKIP("test plug-ins not built");
        SessionFixture f;
        Session& s = f.s();
        TempDir dir;
        const QString path = savedWithPlugins(f, dir, {QStringLiteral("A"), QStringLiteral("B")}, true);
        QVERIFY(s.openProject(path));
        BridgeTestAccess::stopPluginTimer(f.bridge());
        const QString a = f.project().tracks()[0].id, b = f.project().tracks()[1].id;
        const QString fxA = f.project().track(a).devices[0].id, fxB = f.project().track(b).devices[0].id;
        f.editor().removeDevice(b, fxB);
        QVERIFY(f.editor().moveDevicesToTrack(a, {fxA}, b));
        f.bridge().loadPendingPlugins();  // A's loads where it is now; B's went
        QVERIFY(f.bridge().engineDeviceId(b, fxA).has_value());
        QCOMPARE(f.bridge().pluginsPending(), 0);
        QVERIFY(!f.bridge().engineDeviceId(b, fxB).has_value());
        f.stack().undo();  // (A's goes back, its processor along)
        QVERIFY(f.bridge().engineDeviceId(a, fxA).has_value());
        f.stack().undo();  // B's is back: it loads now, as any device added
        QVERIFY(f.bridge().engineDeviceId(b, fxB).has_value());
    }

    void rendersWaitForThePluginsStillLoading() {
        const QString bundle = test::testPluginsBundle();
        if (!test::haveTestPlugins(bundle)) QSKIP("test plug-ins not built");
        SessionFixture f;
        Session& s = f.s();
        TempDir dir;
        const QString path = savedWithPlugins(f, dir, {QStringLiteral("A")});
        QVERIFY(s.openProject(path));
        QCOMPARE(f.bridge().pluginsPending(), 1);
        QStringList labels;  // while it waits (a busy bar)
        connect(s.render(), &RenderProgress::changed, this, [&] {
            if (s.render()->busy() && !labels.contains(s.render()->label())) labels.append(s.render()->label());
        });
        const QString target = dir.path(QStringLiteral("mix.wav"));
        QVERIFY(s.exportAudio(target, QStringLiteral("arrangement"), 24));
        QVERIFY(f.waitForRender());
        QVERIFY(!labels.isEmpty());
        QCOMPARE(labels.front(), QStringLiteral("Loading plug-ins (1 to go)…"));
        QCOMPARE(f.bridge().pluginsPending(), 0);
        QCOMPARE(f.lastMessage(), QStringLiteral("Exported mix.wav"));
        const auto exported = f.engine.loadSource(target.toStdString());
        float most = 0.f;
        for (int64_t i = 0; i < kRate / 2; ++i) most = std::max(most, std::abs(exported->channelData(0)[i]));
        QVERIFY(most > 0.01f);  // the synth played
    }

    void savedBeforeItsPluginsLoadAProjectKeepsTheirState() {
        const QString bundle = test::testPluginsBundle();
        if (!test::haveTestPlugins(bundle)) QSKIP("test plug-ins not built");
        SessionFixture f;
        Session& s = f.s();
        TempDir dir;
        const QString path = savedWithPlugins(f, dir, {QStringLiteral("A")});
        const auto stateIn = [](const QString& file) {
            QFile json(file);
            json.open(QIODevice::ReadOnly);
            const QJsonObject data = QJsonDocument::fromJson(json.readAll()).object();
            return data.value(QStringLiteral("tracks")).toArray().at(0).toObject().value(QStringLiteral("devices"))
                .toArray().at(0).toObject().value(QStringLiteral("state")).toString();
        };
        const QString saved = stateIn(path);
        QVERIFY(!saved.isEmpty());
        QVERIFY(s.openProject(path));
        BridgeTestAccess::stopPluginTimer(f.bridge());
        const QString again = dir.path(QStringLiteral("again.gilproj"));
        QVERIFY(s.saveProjectAs(again));
        QCOMPARE(stateIn(again), saved);
    }

private:
    // A project with a track per name, each with SUB Test Synth playing a note
    // (or an audio track with SUB Test Effect), saved; then a new project.
    QString savedWithPlugins(SessionFixture& f, const TempDir& dir, const QStringList& names, bool effects = false) {
        const QString bundle = test::testPluginsBundle();
        f.bridge().setKnownPlugins(test::testPluginInfos(bundle));
        for (const QString& name : names) {
            if (effects) {
                const QString track = f.editor().addAudioTrack(-1, name);
                f.editor().addDevice(track, kPluginKind, -1, test::testPlugin(bundle, QStringLiteral("SUB Test Effect")));
                continue;
            }
            const QString track =
                f.editor().addMidiTrack(-1, name, QString(), test::testPlugin(bundle, QStringLiteral("SUB Test Synth")));
            f.editor().commitClips(QStringLiteral("Add"),
                                   {{track, {Clip::midi(name + QStringLiteral("m"), name, 0.0, 2.0, 0.0, {Note{60, 0.0, 1.0}})}}});
        }
        const QString path = dir.path(QStringLiteral("plugins.gilproj"));
        if (!f.s().saveProjectAs(path)) return {};
        f.s().newProject();
        return path;
    }
};

QTEST_GUILESS_MAIN(TestSessionRenders)
#include "test_session_renders.moc"
