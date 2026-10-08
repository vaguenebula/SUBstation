// The session's Edit and Create commands, driven as the menus and keys drive
// them, against the project, the selection and the clipboard: splitting,
// selecting all, duplicating and deleting the selected area, cutting, copying
// and pasting clips, automation and tracks (one clipboard), groups, return
// tracks, duplicating tracks, inserting tracks after the selected one, reversing
// clips (at once, and in the background), and what the engine plays.

#include "EditorFixture.h"
#include "SessionFixture.h"
#include "TestSupport.h"

#include "audio/Waveform.h"
#include "browser/BrowserController.h"
#include "session/ArrangementActions.h"
#include "session/DeviceSelection.h"
#include "session/RenderProgress.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>

#include <cmath>
#include <numbers>

using namespace sub::app;
using sub::app::test::SessionFixture;
using sub::app::test::TempDir;

namespace {

constexpr int kRate = test::kSampleRate;
constexpr int kSamplesPerBeat = kRate / 2;  // at 120 BPM

// Stereo: a sine at `freq` (0.5) and one at 1.5 times it (0.3).
std::vector<float> tone(double seconds, double freq) {
    const auto frames = static_cast<size_t>(seconds * kRate);
    std::vector<float> samples(frames * 2);
    for (size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        samples[2 * i] = static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * freq * t));
        samples[2 * i + 1] = static_cast<float>(0.3 * std::sin(2 * std::numbers::pi * freq * 1.5 * t));
    }
    return samples;
}

// The same samples in both channels.
std::vector<float> stereo(const std::vector<float>& mono) {
    std::vector<float> samples(mono.size() * 2);
    for (size_t i = 0; i < mono.size(); ++i) samples[2 * i] = samples[2 * i + 1] = mono[i];
    return samples;
}

// Three audio tracks (tone0..2, 2, 3 and 4 s long) at beats 0, 2 and 4: clips at beats 0-4, 2-8 and 4-12.
QStringList threeTracks(SessionFixture& f, const TempDir& dir) {
    QStringList paths;
    for (int i = 0; i < 3; ++i) {
        const QString path =
            test::writeWav(dir.path(QStringLiteral("tone%1.wav").arg(i)), tone(2.0 + i, 220.0 * (i + 1)), 2);
        f.editor().addClips({}, i * 2.0, {{path, f.bridge().fileInfo(path)->duration}},
                            static_cast<int>(f.project().tracks().size()));
        paths.append(path);
    }
    for (const QString& path : paths) f.waitForSource(path);
    return paths;
}

QStringList trackIds(const Project& project) {
    QStringList ids;
    for (const Track& track : project.tracks()) ids.append(track.id);
    return ids;
}

int clipCount(const Project& project) {
    int count = 0;
    for (const Track& track : project.tracks()) count += static_cast<int>(track.clips.size());
    return count;
}

// Each clip's (start, end) in beats, rounded to 1e-6.
QList<QPair<double, double>> spans(const Project& project, const QString& trackId) {
    QList<QPair<double, double>> list;
    for (const Clip& clip : project.track(trackId).clips) {
        list.append({std::round(clip.startBeat * 1e6) / 1e6, std::round(clip.endBeat(project.tempo()) * 1e6) / 1e6});
    }
    return list;
}

QSet<ClipRef> clipSet(std::initializer_list<ClipRef> refs) { return QSet<ClipRef>(refs.begin(), refs.end()); }

double leftAt(const std::vector<float>& out, int64_t frame) { return out.at(static_cast<size_t>(2 * frame)); }

double maxAbs(const std::vector<float>& out, int64_t fromFrame, int64_t toFrame) {
    double most = 0.0;
    for (int64_t i = 2 * fromFrame; i < std::min<int64_t>(2 * toFrame, static_cast<int64_t>(out.size())); ++i) {
        most = std::max(most, std::abs(static_cast<double>(out[static_cast<size_t>(i)])));
    }
    return most;
}

// Three audio tracks A, B, C with a clip each ("c0" at 0, "c1" at 4, "c2" at
// 8, of a file that isn't there).
QStringList makeTracks(SessionFixture& f, int count = 3) {
    QStringList ids;
    for (int i = 0; i < count; ++i) {
        const QString id = f.editor().addAudioTrack(-1, QString(QChar(u'A' + i)));
        f.editor().commitClips(QStringLiteral("Add"),
                               {{id, {Clip::audio(QStringLiteral("c%1").arg(i), QStringLiteral("missing.wav"),
                                                  QStringLiteral("x"), i * 4.0, 2.0, 0.0, 2.0)}}});
        ids.append(id);
    }
    return ids;
}

}  // namespace

class TestSessionEdit : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase() { test::prepareApplication(); }
    void init() { QSettings().clear(); }

    // --- Editing, the clipboard and the transport ---

    void theEngineMirrorsTheArrangement() {
        SessionFixture f(true);
        TempDir dir;
        threeTracks(f, dir);
        QCOMPARE(f.project().tracks().size(), size_t{3});
        QCOMPARE(f.project().tracks()[0].name, QStringLiteral("1 tone0"));
        QCOMPARE(f.project().tracks()[2].name, QStringLiteral("3 tone2"));
        // The engine plays what the model says: clip 2 starts at beat 4 (2 s at 120 BPM).
        const auto out = f.render(3 * kRate);
        QVERIFY(maxAbs(out, 0, kRate / 2) > 0.1);  // clip 0 at beat 0
        const QStringList ids = trackIds(f.project());
        f.editor().setTrackParam(ids[0], TrackField::Mute, 1.0);
        f.editor().setTrackParam(ids[1], TrackField::Mute, 1.0);
        const auto quiet = f.render(3 * kRate);
        QCOMPARE(maxAbs(quiet, 0, 2 * kRate), 0.0);  // clip 2 only starts at 2 s
        QVERIFY(maxAbs(quiet, 2 * kRate + 1000, 3 * kRate) > 0.1);
    }

    void editCommands() {
        SessionFixture f(true);
        TempDir dir;
        threeTracks(f, dir);
        Session& s = f.s();
        const QString track = f.project().tracks()[0].id;
        f.selection().selectClips(f.editor(), {{track, f.project().track(track).clips[0].id}}, track);
        f.selection().setInsert(1.0);
        s.split();  // Ctrl+E
        QCOMPARE(spans(f.project(), track).size(), 2);
        QCOMPARE(spans(f.project(), track)[1].first, 1.0);

        s.selectAll();  // Ctrl+A
        QCOMPARE(f.selection().clips().size(), 4);
        s.duplicate();  // Ctrl+D
        QCOMPARE(clipCount(f.project()), 8);
        s.deleteSelection();  // Delete
        QCOMPARE(clipCount(f.project()), 4);

        s.insertAudioTrack();  // Ctrl+T
        QCOMPARE(f.project().tracks().size(), size_t{4});
        s.deleteSelectedTracks();
        QCOMPARE(f.project().tracks().size(), size_t{3});
        QVERIFY(s.title().startsWith(QStringLiteral("Untitled*")));
    }

    void cutCopyAndPasteClips() {
        SessionFixture f(true);
        TempDir dir;
        threeTracks(f, dir);
        Session& s = f.s();
        Selection& selection = f.selection();
        const QStringList ids = trackIds(f.project());
        const QString first = ids[0], second = ids[1], third = ids[2];  // clips at beats 0-4, 2-8 and 4-12

        // Nothing copied yet: pasting says so and changes nothing.
        s.paste();
        QCOMPARE(f.stack().count(), 3);
        QVERIFY(f.lastMessage().startsWith(QStringLiteral("Nothing to paste")));

        selection.selectClips(f.editor(), {{first, f.project().track(first).clips[0].id}});
        s.copy();
        QCOMPARE(s.arrangement()->clipboardKind(), QStringLiteral("clips"));
        // Pasted at the insert marker on the selected track, and selected.
        selection.selectTrack(third);
        selection.setInsert(20.0);
        s.paste();
        QCOMPARE(spans(f.project(), third), (QList<QPair<double, double>>{{4, 12}, {20, 24}}));
        const Clip pasted = f.project().track(third).clips[1];
        QCOMPARE(pasted.path, f.project().track(first).clips[0].path);
        QVERIFY(pasted.id != f.project().track(first).clips[0].id);
        QCOMPARE(*selection.timeRange(), (TimeRange{20.0, 24.0, {third}}));
        QCOMPARE(selection.clips(), clipSet({{third, pasted.id}}));
        // The insert marker moves to its end, so pasting again appends.
        QCOMPARE(selection.insertBeat(), 24.0);
        s.paste();
        QCOMPARE(spans(f.project(), third).size(), 3);
        QCOMPARE(spans(f.project(), third)[2].first, 24.0);

        // Cut takes just the selected stretch out; the empty area stays selected.
        selection.setTimeRange(5.0, 7.0, {second}, f.editor().clipsInRange(5.0, 7.0, {second}));
        s.cut();
        QCOMPARE(spans(f.project(), second), (QList<QPair<double, double>>{{2, 5}, {7, 8}}));
        QCOMPARE(*selection.timeRange(), (TimeRange{5.0, 7.0, {second}}));
        QVERIFY(selection.clips().isEmpty());
        QCOMPARE(f.stack().undoText(), QStringLiteral("Cut"));
        selection.setInsert(12.0);
        s.paste();
        QCOMPARE(spans(f.project(), second).size(), 3);
        QCOMPARE(spans(f.project(), second)[2].first, 12.0);
        f.stack().undo();
        f.stack().undo();
        QCOMPARE(spans(f.project(), second), (QList<QPair<double, double>>{{2, 8}}));
    }

    void transportAndLocate() {
        SessionFixture f(true);
        TempDir dir;
        threeTracks(f, dir);
        Session& s = f.s();
        s.locate(6.0);
        QCOMPARE(f.bridge().position(), 6.0);
        QCOMPARE(f.selection().insertBeat(), 6.0);
        s.togglePlay();
        QVERIFY(f.engine.isPlaying());
        s.togglePlay();
        QVERIFY(!f.engine.isPlaying());
        QCOMPARE(f.bridge().position(), 6.0);  // returned to where playback started
        s.stop();
        QCOMPARE(f.bridge().position(), 0.0);
        // Playback starts from the insert marker; stopping goes back there.
        f.selection().setInsert(4.0);
        s.togglePlay();
        QCOMPARE(f.bridge().position(), 4.0);
        s.stop();
        QVERIFY(!f.engine.isPlaying());
        QCOMPARE(f.bridge().position(), 4.0);
    }

    void theBrowserAddsToTheSelectedTrack() {
        SessionFixture f(true);
        TempDir dir;
        threeTracks(f, dir);
        Session& s = f.s();
        const QStringList ids = trackIds(f.project());
        // Double-click on a built-in device adds it to the selected track...
        f.selection().selectTrack(ids[0]);
        Q_EMIT s.browser()->deviceActivated(QStringLiteral("utility"));
        QCOMPARE(f.project().track(ids[0]).devices.size(), size_t{1});
        QCOMPARE(f.project().track(ids[0]).devices[0].kind, QStringLiteral("utility"));
        // ...dropping it on the device view adds it to the track shown there...
        s.deviceSelection()->dropDevices({QStringLiteral("utility")}, {}, {}, 0);
        QCOMPARE(f.project().track(ids[0]).devices.size(), size_t{2});
        QCOMPARE(s.deviceSelection()->shownDevices().size(), 2);
        // ...and dropping it on a track in the arrangement adds it to that track, which is selected.
        QVERIFY(s.arrangement()->dropDevices({QStringLiteral("utility")}, {}, ids[2]));
        QCOMPARE(f.project().track(ids[2]).devices.size(), size_t{1});
        QCOMPARE(f.selection().trackId(), ids[2]);
        QCOMPARE(s.deviceSelection()->shownDevices().size(), 1);
        // An instrument with no MIDI track selected: on a new MIDI track, which is selected.
        Q_EMIT s.browser()->deviceActivated(QStringLiteral("synth"));
        QCOMPARE(f.project().tracks().size(), size_t{4});
        const Track& keys = f.project().track(f.selection().trackId());
        QVERIFY(keys.isMidi() && keys.devices.size() == 1 && keys.devices[0].kind == QStringLiteral("synth"));
        QCOMPARE(f.project().trackIndex(keys.id), 3);  // after the selected track
        // With nothing selected, an effect goes nowhere.
        f.selection().selectTrack(QString());
        s.addDeviceToSelectedTrack(QStringLiteral("utility"));
        QCOMPARE(f.lastMessage(), QStringLiteral("Select a track to add the device to."));
        // A file activated in the browser goes at the insert marker, on a new track.
        const QString kick = test::writeWav(dir.path(QStringLiteral("Kick Deep.wav")), tone(0.2, 60.0), 2);
        Q_EMIT s.browser()->fileActivated(kick);
        QCOMPARE(f.project().tracks().back().clips[0].name, QStringLiteral("Kick Deep"));
    }

    void aDragOnTheLanesSelectsEverythingInItsRange() {
        SessionFixture f(true);
        TempDir dir;
        threeTracks(f, dir);
        Session& s = f.s();
        Selection& selection = f.selection();
        const QStringList ids = trackIds(f.project());
        // What a drag from beat 1 to 3 over the first two tracks selects.
        selection.setTimeRange(1.0, 3.0, {ids[0], ids[1]}, f.editor().clipsInRange(1.0, 3.0, {ids[0], ids[1]}));
        // Delete cuts out only the range: clip 0 (beats 0-4) keeps its start and end,
        // clip 1 (beats 2-8) loses its first beat, track 3 is outside the range.
        s.deleteSelection();
        QCOMPARE(spans(f.project(), ids[0]), (QList<QPair<double, double>>{{0, 1}, {3, 4}}));
        QCOMPARE(spans(f.project(), ids[1]), (QList<QPair<double, double>>{{3, 8}}));
        QCOMPARE(f.project().track(ids[2]).clips.size(), size_t{1});
        QCOMPARE(*selection.timeRange(), (TimeRange{1.0, 3.0, {ids[0], ids[1]}}));  // still selected
        QVERIFY(selection.clips().isEmpty());
        f.stack().undo();
        QCOMPARE(clipCount(f.project()), 3);
        // Ctrl+D copies just the selected area to right after it, then selects the copy.
        selection.setTimeRange(1.0, 3.0, {ids[0], ids[1]}, QSet<ClipRef>());
        s.duplicate();
        QCOMPARE(spans(f.project(), ids[0]), (QList<QPair<double, double>>{{0, 3}, {3, 5}}));
        QCOMPARE(spans(f.project(), ids[1]), (QList<QPair<double, double>>{{2, 4}, {4, 5}, {5, 8}}));
        QCOMPARE(f.project().track(ids[1]).clips[1].offsetSec, 0.0);  // the copy plays clip 1's first beat
        QCOMPARE(*selection.timeRange(), (TimeRange{3.0, 5.0, {ids[0], ids[1]}}));
        QVERIFY(selection.clipRange());
        QCOMPARE(selection.insertBeat(), 3.0);
        f.stack().undo();
        QCOMPARE(clipCount(f.project()), 3);
    }

    void theTitleSaysWhetherThereAreChanges() {
        SessionFixture f;
        Session& s = f.s();
        QSignalSpy titles(&s, &Session::titleChanged);
        QCOMPARE(s.title(), QStringLiteral("Untitled - SUBstation"));
        QVERIFY(s.clean());
        s.insertAudioTrack();
        QCOMPARE(s.title(), QStringLiteral("Untitled* - SUBstation"));
        QVERIFY(!s.clean() && !titles.isEmpty());
        f.stack().undo();
        QCOMPARE(s.title(), QStringLiteral("Untitled - SUBstation"));
    }

    void insertedTracksGoAfterTheSelectedOne() {
        SessionFixture f;
        Session& s = f.s();
        const QString a = s.insertAudioTrack();
        const QString b = s.insertMidiTrack();
        QCOMPARE(trackIds(f.project()), (QStringList{a, b}));
        QVERIFY(f.project().track(b).isMidi() && f.project().track(b).devices.size() == 1);  // with its synth
        QCOMPARE(f.selection().trackId(), b);
        QCOMPARE(f.selection().focus(), Selection::Focus::Track);
        f.selection().selectTrack(a);
        const QString c = s.insertAudioTrack();
        QCOMPARE(trackIds(f.project()), (QStringList{a, c, b}));
        // In the selected track's group, after what is in it.
        const QString group = f.editor().groupTracks({a, c});
        f.selection().selectTrack(a);
        const QString d = s.insertAudioTrack();
        QCOMPARE(trackIds(f.project()), (QStringList{group, a, d, c, b}));
        QCOMPARE(f.project().track(d).parent, std::optional<QString>(group));
        f.selection().selectTrack(group);
        const QString e = s.insertAudioTrack();
        QCOMPARE(trackIds(f.project()), (QStringList{group, a, d, c, e, b}));
        QVERIFY(!f.project().track(e).parent);
        // The lanes' menu: after the track under the mouse, not selected; below the tracks: last.
        const QString g = s.arrangement()->insertTrackAfter(a, true);
        QCOMPARE(trackIds(f.project()), (QStringList{group, a, g, d, c, e, b}));
        QVERIFY(f.project().track(g).isMidi() && f.project().track(g).parent == std::optional<QString>(group));
        QCOMPARE(f.selection().trackId(), e);
        const QString h = s.arrangement()->insertTrackAfter({}, false);
        QCOMPARE(trackIds(f.project()).back(), h);
        QCOMPARE(s.statusTimeout(), 8000);
    }

    // --- Reversing clips, time selections ---

    void rReversesTheSelectedAudioClipsAndAgainPutsThemBack() {
        TempDir dir;
        const test::ScopedEnv recordings("SUBSTATION_RECORDINGS", dir.path(QStringLiteral("Recordings")));
        SessionFixture f(true);
        Session& s = f.s();
        std::vector<float> ramp(2 * kRate);  // rises from 0 to 1 over 2 s (4 beats)
        for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) / static_cast<float>(ramp.size());
        const QString path = test::writeWav(dir.path(QStringLiteral("ramp.wav")), stereo(ramp), 2);
        const ClipRefs refs = f.editor().addClips({}, 0.0, {{path, 2.0}});
        const QString track = refs[0].trackId;
        const QString clipId = refs[0].clipId;
        QVERIFY(f.waitForSource(path));
        f.selection().selectClips(f.editor(), refs);

        s.reverseClips();  // R
        Clip clip = f.project().track(track).clips[0];
        QCOMPARE(QFileInfo(clip.path).absoluteFilePath(),
                 QFileInfo(dir.path(QStringLiteral("Recordings/Reversed/ramp R.wav"))).absoluteFilePath());
        QCOMPARE(clip.id, clipId);
        QCOMPARE(clip.reversedFrom, path);
        QCOMPARE(clip.offsetSec, 0.0);
        QCOMPARE(f.stack().undoText(), QStringLiteral("Reverse Clip"));
        const Waveform source = f.bridge().waveform(path);
        const Waveform backwards = f.bridge().waveform(clip.path);
        QVERIFY(!backwards.isNull() && backwards.frames() == source.frames());
        for (qint64 i = 0; i < source.frames(); i += 997) {
            QCOMPARE(backwards.channelData(0)[i], source.channelData(0)[source.frames() - 1 - i]);
        }
        auto out = f.render(4 * kSamplesPerBeat);
        QVERIFY(std::abs(leftAt(out, kSamplesPerBeat) - 0.75) < 1e-3);  // half a second in: falling, from 1
        QVERIFY(std::abs(leftAt(out, 3 * kSamplesPerBeat) - 0.25) < 1e-3);

        // Again: it plays its own file again (forwards).
        s.reverseClips();
        clip = f.project().track(track).clips[0];
        QCOMPARE(clip.path, path);
        QVERIFY(clip.reversedFrom.isEmpty());
        QCOMPARE(clip.offsetSec, 0.0);
        out = f.render(2 * kSamplesPerBeat);
        QVERIFY(std::abs(leftAt(out, kSamplesPerBeat) - 0.25) < 1e-3);
        f.stack().undo();
        QCOMPARE(f.project().track(track).clips[0].reversedFrom, path);

        // A time range over part of it: just that part, split off and reversed (from the same copy).
        f.stack().undo();
        f.selection().setTimeRange(1.0, 2.0, {track}, QSet<ClipRef>());
        s.reverseClips();
        const std::vector<Clip>& clips = f.project().track(track).clips;
        QCOMPARE(clips.size(), size_t{3});
        QCOMPARE(QFileInfo(clips[0].path).fileName(), QStringLiteral("ramp.wav"));
        QCOMPARE(QFileInfo(clips[1].path).fileName(), QStringLiteral("ramp R.wav"));
        QCOMPARE(QFileInfo(clips[2].path).fileName(), QStringLiteral("ramp.wav"));
        QCOMPARE(clips[1].startBeat, 1.0);
        QCOMPARE(QDir(dir.path(QStringLiteral("Recordings/Reversed"))).entryList(QDir::Files).size(), 1);
        QVERIFY(std::abs(clips[1].offsetSec - 1.0) < 1e-9);  // it played seconds 0.5-1: 1-1.5 of the copy
    }

    void aReversedCopySavedWithTheProjectIsUsedAgain() {
        TempDir dir;
        const test::ScopedEnv recordings("SUBSTATION_RECORDINGS", dir.path(QStringLiteral("Recordings")));
        SessionFixture f(true);
        Session& s = f.s();
        std::vector<float> ramp(kRate);
        for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) / kRate;
        const QString path = test::writeWav(dir.path(QStringLiteral("ramp.wav")), stereo(ramp), 2);
        const ClipRefs refs = f.editor().addClips({}, 0.0, {{path, 1.0}});
        QVERIFY(f.waitForSource(path));
        f.selection().selectClips(f.editor(), refs);
        s.reverseClips();
        const QString copy = f.project().track(refs[0].trackId).clips[0].path;
        const QString saved = dir.path(QStringLiteral("song.gilproj"));
        QVERIFY(s.saveProjectAs(saved));
        QVERIFY(s.openProject(saved));  // (what this session wrote is forgotten)
        QVERIFY(f.waitForSource(path));
        // Another clip of the same file, reversed: the copy the project has is used again.
        const ClipRefs other = f.editor().addClips({}, 8.0, {{path, 1.0}});
        f.selection().selectClips(f.editor(), other);
        s.reverseClips();
        QCOMPARE(f.project().track(other[0].trackId).clips[0].path, copy);
        QCOMPARE(QDir(QFileInfo(copy).path()).entryList(QDir::Files), QStringList{QStringLiteral("ramp R.wav")});
    }

    void longClipsReverseInTheBackground() {
        TempDir dir;
        const test::ScopedEnv recordings("SUBSTATION_RECORDINGS", dir.path(QStringLiteral("Recordings")));
        SessionFixture f(true);
        Session& s = f.s();
        s.arrangement()->setReverseInPlaceSeconds(0.0);  // (any clip is "long" here)
        std::vector<float> ramp(20 * kRate);
        for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) / static_cast<float>(ramp.size());
        const QString path = test::writeWav(dir.path(QStringLiteral("ramp.wav")), stereo(ramp), 2);
        const ClipRefs refs = f.editor().addClips({}, 0.0, {{path, 20.0}});
        const QString track = refs[0].trackId;
        QVERIFY(f.waitForSource(path));
        f.selection().selectClips(f.editor(), refs);

        RenderProgress& progress = *s.render();
        QStringList seen;
        bool cancel = true;
        connect(&progress, &RenderProgress::activeChanged, this, [&] {
            if (!progress.active()) return;
            if (cancel) progress.cancel();  // Cancel at once
        });
        connect(&progress, &RenderProgress::changed, this, [&] {
            if (progress.label().startsWith(QStringLiteral("Reversing"))) {
                const QString entry = progress.title() + QStringLiteral(": ") + progress.label();
                if (!seen.contains(entry)) seen.append(entry);
            }
        });
        const QString undoText = f.stack().undoText();
        s.reverseClips();  // cancelled: nothing changes, no copy is left
        QVERIFY(f.waitForRender());
        QCOMPARE(f.project().track(track).clips[0].path, path);
        QCOMPARE(f.stack().undoText(), undoText);
        QVERIFY(QDir(dir.path(QStringLiteral("Recordings/Reversed"))).entryList(QDir::Files).isEmpty());

        cancel = false;
        s.reverseClips();
        QVERIFY(progress.active());  // in the background
        QVERIFY(f.waitForRender());
        QCOMPARE(seen, QStringList{QStringLiteral("Reverse Clips: Reversing ramp.wav…")});
        const Clip clip = f.project().track(track).clips[0];
        QCOMPARE(QFileInfo(clip.path).fileName(), QStringLiteral("ramp R.wav"));
        QCOMPARE(f.stack().undoText(), QStringLiteral("Reverse Clip"));
        const Waveform source = f.bridge().waveform(path);
        const Waveform backwards = f.bridge().waveform(clip.path);
        QVERIFY(!backwards.isNull() && backwards.frames() == source.frames());
        for (qint64 i = 0; i < source.frames(); i += 9973) {
            QCOMPARE(backwards.channelData(1)[i], source.channelData(1)[source.frames() - 1 - i]);
        }
    }

    void severalFilesReverseOneAfterAnother() {
        TempDir dir;
        const test::ScopedEnv recordings("SUBSTATION_RECORDINGS", dir.path(QStringLiteral("Recordings")));
        SessionFixture f(true);
        Session& s = f.s();
        s.arrangement()->setReverseInPlaceSeconds(0.0);
        // A long file and a short one: the short one's copy is done before it is followed.
        std::vector<float> ramp(10 * kRate);
        for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = static_cast<float>(i) / static_cast<float>(ramp.size());
        const QString longFile = test::writeWav(dir.path(QStringLiteral("long.wav")), stereo(ramp), 2);
        const QString shortFile = test::writeWav(dir.path(QStringLiteral("short.wav")),
                                                 stereo(std::vector<float>(kRate / 10, 0.25f)), 2);
        const ClipRefs first = f.editor().addClips({}, 0.0, {{longFile, 10.0}});
        const ClipRefs second = f.editor().addClips({}, 0.0, {{shortFile, 0.1}});
        QVERIFY(f.waitForSource(longFile) && f.waitForSource(shortFile));
        f.selection().selectClips(f.editor(), {first[0], second[0]});
        QStringList labels;
        connect(s.render(), &RenderProgress::changed, this, [&] {
            if (s.render()->label().startsWith(QStringLiteral("Reversing")) && !labels.contains(s.render()->label())) {
                labels.append(s.render()->label());
            }
        });
        const int steps = f.stack().count();
        s.reverseClips();
        QVERIFY(f.waitForRender());
        QCOMPARE(labels, (QStringList{QStringLiteral("Reversing long.wav…"), QStringLiteral("Reversing short.wav…")}));
        QCOMPARE(f.stack().count(), steps + 1);  // one undo step
        QCOMPARE(QFileInfo(f.project().track(first[0].trackId).clips[0].path).fileName(), QStringLiteral("long R.wav"));
        QCOMPARE(QFileInfo(f.project().track(second[0].trackId).clips[0].path).fileName(), QStringLiteral("short R.wav"));
    }

    void reverseInTheMenuFollowsTheSelectedArea() {
        SessionFixture f(true);
        TempDir dir;
        const QString path = test::writeWav(dir.path(QStringLiteral("dc.wav")),
                                            stereo(std::vector<float>(2 * kRate, 0.5f)), 2);
        const ClipRefs refs = f.editor().addClips({}, 0.0, {{path, 2.0}});
        const QString track = refs[0].trackId;
        f.selection().setTimeRange(1.0, 2.0, {track}, clipSet({refs[0]}));
        f.s().deleteSelection();
        f.stack().undo();  // the clip is back in the (still selected) area
        QVERIFY(f.selection().clipRange() && f.selection().clips().isEmpty());
        QVERIFY(f.s().arrangement()->canReverse());
        QVERIFY(!f.s().arrangement()->canConsolidate());
    }

    void reversingNeedsAudioClips() {
        SessionFixture f;
        const QString track = f.editor().addMidiTrack();
        const auto ref = f.editor().addMidiClip(track, 0.0, 4.0);
        f.selection().selectClips(f.editor(), {*ref});
        f.s().reverseClips();
        QCOMPARE(f.messages, QStringList{QStringLiteral("There are no audio clips in the selection to reverse.")});
        QCOMPARE(f.stack().undoText(), QStringLiteral("Insert MIDI Clip"));
    }

    void aTimeSelectionOverAGroupTakesInItsTracks() {
        SessionFixture f;
        Session& s = f.s();
        QStringList ids;
        for (const char* name : {"A", "B", "C"}) {
            const QString id = f.editor().addAudioTrack(-1, QString::fromLatin1(name));
            f.editor().commitClips(QStringLiteral("Add"),
                                   {{id, {Clip::audio(QStringLiteral("c%1").arg(ids.size()), QStringLiteral("missing.wav"),
                                                      QStringLiteral("x"), 0.0, 2.0, 0.0, 2.0)}}});
            ids.append(id);
        }
        const QString a = ids[0], b = ids[1], c = ids[2];
        const QString group = f.editor().groupTracks({a, b});
        f.editor().setFolded(group, true);  // (its tracks hidden: still in it)
        // Dragged along the group's lane, a selection takes in everything in the group.
        const QStringList range{group, a, b};
        f.selection().setTimeRange(1.0, 3.0, range, f.editor().clipsInRange(1.0, 3.0, range));
        QCOMPARE(f.selection().clips(), clipSet({{a, QStringLiteral("c0")}, {b, QStringLiteral("c1")}}));
        s.deleteSelection();
        for (const QString& id : {a, b}) QCOMPARE(spans(f.project(), id), (QList<QPair<double, double>>{{0, 1}, {3, 4}}));
        QCOMPARE(spans(f.project(), c), (QList<QPair<double, double>>{{0, 4}}));
        f.stack().undo();

        // Copy and paste it further on: onto the group's tracks again.
        s.copy();
        f.selection().setInsert(8.0);
        s.paste();
        QCOMPARE(*f.selection().timeRange(), (TimeRange{8.0, 10.0, {a, b}}));
        for (const QString& id : {a, b}) {
            QCOMPARE(f.project().track(id).clips.size(), size_t{2});
            QCOMPARE(f.project().track(id).clips[1].startBeat, 8.0);
        }
        f.stack().undo();

        // Ctrl+A: from the first clip to the last, on every track.
        s.selectAll();
        QCOMPARE(*f.selection().timeRange(), (TimeRange{0.0, 4.0, {group, a, b, c}}));
    }

    // --- Groups ---

    void ctrlGGroupsTheSelectedTracksAndCtrlShiftGUngroups() {
        SessionFixture f;
        Session& s = f.s();
        const QStringList ids = makeTracks(f);
        const QString a = ids[0], b = ids[1], c = ids[2];
        f.selection().selectTrack(a, true);
        f.selection().selectTrack(b, true, Selection::Mode::Toggle);
        s.groupSelected();  // Ctrl+G
        const Track group = f.project().tracks()[0];
        QVERIFY(group.isGroup());
        QCOMPARE(f.project().track(a).parent, std::optional<QString>(group.id));
        QCOMPARE(f.project().track(b).parent, std::optional<QString>(group.id));
        QVERIFY(!f.project().track(c).parent);
        QCOMPARE(f.selection().trackId(), group.id);
        QCOMPARE(f.stack().undoText(), QStringLiteral("Group Tracks"));
        f.editor().setFolded(group.id, true);
        QCOMPARE(f.stack().undoText(), QStringLiteral("Group Tracks"));  // folding isn't undone
        f.editor().setFolded(group.id, false);

        // Ctrl+Shift+G ungroups.
        f.selection().selectTrack(group.id, true);
        s.ungroupSelected();
        QCOMPARE(trackIds(f.project()), (QStringList{a, b, c}));
        for (const Track& track : f.project().tracks()) QVERIFY(!track.parent);
        f.stack().undo();
        QVERIFY(f.project().tracks()[0].isGroup());

        // Nothing to group or ungroup: said so.
        f.selection().selectTrack(c, true);
        s.ungroupSelected();
        QCOMPARE(f.lastMessage(), QStringLiteral("Select a group track to ungroup."));
        f.selection().selectTrack(QString());
        s.groupSelected();
        QCOMPARE(f.lastMessage(), QStringLiteral("Select the tracks to group."));
    }

    void groupsCanBeCutCopiedAndPasted() {
        SessionFixture f;
        Session& s = f.s();
        const QStringList ids = makeTracks(f);
        const QString a = ids[0], b = ids[1], c = ids[2];
        const QString group = f.editor().groupTracks({a, b});
        f.selection().selectTrack(group, true);
        s.copy();
        QCOMPARE(s.arrangement()->clipboardKind(), QStringLiteral("tracks"));
        f.selection().selectTrack(c, true);
        s.paste();  // after C
        QCOMPARE(f.project().tracks().size(), size_t{7});  // the group, A, B, C, then the copies
        const Track pasted = f.project().tracks()[4];
        QVERIFY(pasted.isGroup() && !pasted.parent);
        QCOMPARE(f.selection().trackIds(), QStringList{pasted.id});
        const auto inside = f.project().descendants(pasted.id);
        QCOMPARE(inside.size(), size_t{2});
        for (const Track* track : inside) {
            QCOMPARE(track->clips.size(), size_t{1});
            QVERIFY(track->id != a && track->id != b);
        }
        QCOMPARE(f.stack().undoText(), QStringLiteral("Paste Track"));

        f.selection().selectTrack(group, true);
        s.cut();
        QVERIFY(!f.project().hasTrack(group) && !f.project().hasTrack(a));
        QCOMPARE(f.project().tracks().size(), size_t{4});
        f.selection().selectTrack(c, true);
        s.paste();
        QCOMPARE(f.project().tracks().size(), size_t{7});
        QVERIFY(f.project().tracks()[1].isGroup());  // after C, before the first copy
        f.stack().undo();
        f.stack().undo();
        QVERIFY(f.project().hasTrack(group));
        QStringList inGroup;
        for (const Track* track : f.project().descendants(group)) inGroup.append(track->id);
        QCOMPARE(inGroup, (QStringList{a, b}));
    }

    void aRefusedCutLeavesTheClipboard() {
        SessionFixture f;
        Session& s = f.s();
        const QStringList ids = makeTracks(f);
        const QString a = ids[0], b = ids[1], c = ids[2];
        const QString group = f.editor().groupTracks({a, b});
        // Cutting a track in a frozen group is refused: nothing is cut, the clipboard keeps what it had.
        CopiedAutomation before;
        before.length = 1.0;
        s.arrangement()->setClipboard(before);
        Freeze freeze;
        freeze.path = QStringLiteral("frozen.wav");
        freeze.durationSec = 2.0;
        freeze.tempo = 120.0;
        OrderedMap<QString, Freeze> freezes;
        freezes.insert(group, freeze);
        QCOMPARE(f.editor().freezeTracks(freezes), QStringList{group});
        f.selection().selectTrack(a, true);
        s.cut();
        QVERIFY(f.project().hasTrack(a));
        QCOMPARE(s.arrangement()->clipboardKind(), QStringLiteral("automation"));
        f.stack().undo();  // (unfrozen)

        // The selected tracks, not a clip range selected since: what Cut and Copy act on.
        f.selection().selectTrack(c, true);
        QCOMPARE(s.whatIsCopied(), QStringLiteral("tracks"));
        f.selection().setTimeRange(0.0, 4.0, {c}, f.editor().clipsInRange(0.0, 4.0, {c}));
        QCOMPARE(s.whatIsCopied(), QStringLiteral("clips"));
        f.selection().focusTracks();  // (a track header's menu)
        QCOMPARE(s.whatIsCopied(), QStringLiteral("tracks"));

        // Ctrl+R doesn't rename a track hidden in a folded group.
        f.editor().setFolded(group, true);
        f.selection().selectTrack(a, true);
        QVERIFY(s.renameTarget().isEmpty());
        QCOMPARE(f.lastMessage(), QStringLiteral("Select a track, a rack chain or a preset to rename."));
        f.selection().selectTrack(group, true);
        QCOMPARE(s.renameTarget().value(QStringLiteral("kind")).toString(), QStringLiteral("track"));
        QCOMPARE(s.renameTarget().value(QStringLiteral("trackId")).toString(), group);
    }

    void breakpointsAndReturnsArentCopied() {
        SessionFixture f;
        Session& s = f.s();
        const QString ret = f.editor().addReturnTrack();
        f.selection().selectTrack(ret, true);
        QCOMPARE(s.whatIsCopied(QStringLiteral("cut")), QString());
        QCOMPARE(f.lastMessage(), QStringLiteral("Only the arrangement's tracks can be cut, not returns or the master."));
        s.duplicate();
        QCOMPARE(f.lastMessage(),
                 QStringLiteral("Only the arrangement's tracks can be duplicated, not returns or the master."));
        const QString track = f.editor().addAudioTrack();
        f.editor().setEnvelope(track, automation::kMixerPan, test::env({{0.0, 0.0}, {2.0, 1.0}}));
        f.selection().selectPoints(track, automation::kMixerPan, QSet<int>{0});
        QCOMPARE(s.whatIsCopied(), QString());
        QCOMPARE(f.lastMessage(),
                 QStringLiteral("Breakpoints can't be copied: select a time range on the automation lane."));
        // Delete deletes them.
        s.deleteSelection();
        QCOMPARE(f.project().envelope(track, automation::kMixerPan).size(), size_t{1});
        QVERIFY(!f.selection().points());
    }

    // --- Return tracks and solo ---

    void ctrlAltTInsertsAReturn() {
        SessionFixture f;
        Session& s = f.s();
        const QString track = f.editor().addAudioTrack();
        const QString ret = s.insertReturnTrack();
        QCOMPARE(f.project().returns().size(), size_t{1});
        QCOMPARE(f.project().returns()[0].name, QStringLiteral("A Return"));
        QCOMPARE(trackIds(f.project()), QStringList{track});
        QCOMPARE(f.selection().trackId(), ret);
        QCOMPARE(s.deviceSelection()->trackId(), ret);  // the device view shows its effects
        // Another after the selected one.
        const QString second = s.insertReturnTrack();
        f.selection().selectTrack(ret, true);
        const QString between = s.insertReturnTrack();
        QCOMPARE(f.project().returnIndex(between), 1);
        QCOMPARE(f.project().returnIndex(second), 2);
        f.stack().undo();
        f.stack().undo();
        f.stack().undo();
        QVERIFY(f.project().returns().empty());
    }

    void deletingAReturnWithDelete() {
        SessionFixture f;
        Session& s = f.s();
        const QString track = f.editor().addAudioTrack();
        const QString ret = f.editor().addReturnTrack();
        f.editor().setSend(track, ret, -3.0);
        f.selection().selectTrack(ret, true);  // (its header clicked)
        s.deleteSelection();
        QVERIFY(f.project().returns().empty() && f.project().track(track).sends.isEmpty());
        f.stack().undo();
        QCOMPARE(f.project().returns().size(), size_t{1});
        QCOMPARE(f.project().track(track).sends.value(ret).levelDb, -3.0);
    }

    void soloSelectedTracks() {
        SessionFixture f;
        Session& s = f.s();
        const QString a = f.editor().addAudioTrack();
        const QString b = f.editor().addAudioTrack();
        const QString ret = f.editor().addReturnTrack();
        f.selection().selectTrack(a, true);
        s.soloSelectedTracks();  // S
        QVERIFY(f.project().track(a).solo && !f.project().track(b).solo);
        f.selection().selectTrack(b, true);
        s.soloSelectedTracks();  // the others unsoloed
        QVERIFY(!f.project().track(a).solo && f.project().track(b).solo);
        s.soloSelectedTracks();  // all of them soloed already: every track unsoloed
        QVERIFY(!f.project().track(a).solo && !f.project().track(b).solo && !f.project().track(ret).solo);
    }

    // --- Duplicating tracks, copying automation ---

    void ctrlDDuplicatesTheSelectedTracks() {
        SessionFixture f;
        Session& s = f.s();
        const QString first = s.insertAudioTrack();
        const QString second = s.insertMidiTrack();
        f.selection().selectTrack(first, true);
        f.selection().selectTrack(second, true, Selection::Mode::Toggle);
        s.duplicate();
        const QStringList ids = trackIds(f.project());
        QCOMPARE(ids.size(), 4);
        QCOMPARE(ids.mid(0, 2), (QStringList{first, second}));
        const Track& audioCopy = f.project().track(ids[2]);
        const Track& midiCopy = f.project().track(ids[3]);
        QVERIFY(audioCopy.isAudio() && midiCopy.isMidi());
        QVERIFY(f.bridge().engineDeviceId(midiCopy.id, midiCopy.devices[0].id).has_value());  // its synth plays
        const QStringList selected = f.selection().trackIds();
        QCOMPARE(QSet<QString>(selected.begin(), selected.end()), (QSet<QString>{ids[2], ids[3]}));
        QCOMPARE(f.selection().focus(), Selection::Focus::Track);
        f.stack().undo();
        QCOMPARE(trackIds(f.project()), (QStringList{first, second}));
    }

    void ctrlCAndCtrlVCopyAutomationBetweenLanes() {
        SessionFixture f;
        Session& s = f.s();
        Selection& selection = f.selection();
        const QString a = s.insertAudioTrack();
        const QString b = s.insertAudioTrack();
        f.editor().setEnvelope(a, automation::kMixerPan, test::env({{0.0, 0.0}, {2.0, 1.0}, {4.0, 0.0}}));
        selection.setTimeRange(1.0, 3.0, {a}, std::nullopt, {{a, automation::kMixerPan}});
        s.copy();
        const auto* copied = std::get_if<CopiedAutomation>(&s.arrangement()->clipboard());
        QVERIFY(copied != nullptr);
        QCOMPARE(copied->lanes[0].first, (LaneRef{a, automation::kMixerPan}));
        // Select a range on b's lane: the copy goes there, at its start, and is selected.
        selection.setTimeRange(4.0, 5.0, {b}, std::nullopt, {{b, automation::kMixerVolume}});
        selection.setInsert(4.0);
        s.paste();
        QVERIFY(std::abs(*automation::valueAt(f.project().envelope(b, automation::kMixerVolume), 5.0) - 1.0) < 1e-9);
        QCOMPARE(*selection.timeRange(), (TimeRange{4.0, 6.0, {b}}));
        QCOMPARE(selection.lanes(), (QList<LaneRef>{{b, automation::kMixerVolume}}));
        QCOMPARE(selection.insertBeat(), 6.0);
        // Cut: out of the range, which stays selected.
        selection.setTimeRange(1.0, 3.0, {a}, std::nullopt, {{a, automation::kMixerPan}});
        s.cut();
        QVERIFY(std::abs(*automation::valueAt(f.project().envelope(a, automation::kMixerPan), 2.0) - 0.5) < 1e-9);
        QCOMPARE(selection.lanes(), (QList<LaneRef>{{a, automation::kMixerPan}}));
        // Breakpoints aren't a range: nothing is copied.
        selection.selectPoints(a, automation::kMixerPan, QSet<int>{0});
        s.copy();
        copied = std::get_if<CopiedAutomation>(&s.arrangement()->clipboard());
        QVERIFY(copied != nullptr && copied->lanes[0].first == (LaneRef{a, automation::kMixerPan}));
        // An automation lane's menu's Paste: at the insert marker, onto that lane.
        selection.setInsert(8.0);
        s.arrangement()->pasteAutomationAt(b, automation::kMixerPan);
        QVERIFY(automation::valueAt(f.project().envelope(b, automation::kMixerPan), 9.0).has_value());
        QCOMPARE(selection.lanes(), (QList<LaneRef>{{b, automation::kMixerPan}}));
    }

    // --- Inserting MIDI clips -------------------------------------------------------------------

    void insertMidiClip() {
        SessionFixture f;
        Session& s = f.s();
        QSignalSpy opened(s.arrangement(), &ArrangementActions::clipViewRequested);
        s.insertMidiClip(1.0);  // nothing selected: said so
        QCOMPARE(f.lastMessage(), QStringLiteral("Select a MIDI track (or a time range on one) to insert a MIDI clip."));
        const QString keys = s.insertMidiTrack();
        f.selection().setInsert(2.5);
        s.insertMidiClip(1.0);  // at the insert marker, on the grid: a bar long
        QCOMPARE(f.project().track(keys).clips.size(), size_t{1});
        const Clip clip = f.project().track(keys).clips[0];
        QCOMPARE(clip.startBeat, 2.0);
        QCOMPARE(clip.endBeat(f.project().tempo()), 6.0);
        QCOMPARE(f.selection().clips(), clipSet({{keys, clip.id}}));
        QCOMPARE(opened.count(), 1);  // straight into the piano roll
        QCOMPARE(opened.first().at(2).toString(), clip.id);
        // Over a time selection, on each MIDI track in it.
        const QString other = s.insertMidiTrack();
        const QString audio = s.insertAudioTrack();
        f.selection().setTimeRange(8.0, 10.0, {keys, other, audio}, QSet<ClipRef>());
        s.insertMidiClip(1.0);
        QCOMPARE(f.project().track(keys).clips.size(), size_t{2});
        QCOMPARE(f.project().track(other).clips.size(), size_t{1});
        QVERIFY(f.project().track(audio).clips.empty());
        // The arrangement's: at a beat on a MIDI track.
        const QVariantMap made = s.arrangement()->insertMidiClip(other, 13.3, 0.0);
        QCOMPARE(made.value(QStringLiteral("trackId")).toString(), other);
        QCOMPARE(f.project().clip(other, made.value(QStringLiteral("clipId")).toString()).startBeat, 13.3);
        QCOMPARE(f.selection().insertBeat(), 13.3);
        QVERIFY(s.arrangement()->insertMidiClip(audio, 1.0).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestSessionEdit)
#include "test_session_edit.moc"
