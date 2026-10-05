// The clip view with audio clips (tests/test_ui_smoke.py's clip view tests
// and test_ui_clip_edits.py's clip gain), driven the way a user would: one
// clip opened, its settings and waveform; several clips edited in unison;
// warping and transposing reaching the audio; the clip gain making the
// waveform taller; which clips open with a MIDI clip among them; going back
// (Esc, ×, the clips deleted, the project reset). Runs on a display (xvfb
// here). With $SUBSTATION_UI_SCREENSHOTS set, it saves screenshots there.

#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <memory>
#include <numbers>

#include "UiTestSupport.h"
#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "model/Clip.h"
#include "model/Project.h"
#include "pianoroll/ClipViewController.h"
#include "pianoroll/ClipWaveform.h"
#include "pianoroll/NoteGrid.h"
#include "session/Selection.h"

using sub::app::Clip;
using sub::app::ClipRef;
using sub::app::ClipRefs;
using sub::ui::ClipViewController;
namespace test = sub::app::test;

namespace {

constexpr int kRate = test::kSampleRate;

// The window (at the end of the file: moc skips what follows a raw string).
extern const char* const kWindow;

// A stereo tone: `freq` on the left, 1.5 times it on the right, quieter.
std::vector<float> tone(double seconds, double freq) {
    const int frames = static_cast<int>(seconds * kRate);
    std::vector<float> samples(static_cast<size_t>(2 * frames));
    for (int i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / kRate;
        samples[size_t(2 * i)] = static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * freq * t));
        samples[size_t(2 * i + 1)] = static_cast<float>(0.3 * std::sin(2 * std::numbers::pi * freq * 1.5 * t));
    }
    return samples;
}

// The power of `freq` in the left channel of interleaved stereo (Goertzel).
double power(const std::vector<float>& out, size_t from, size_t frames, double freq) {
    const double k = 2 * std::cos(2 * std::numbers::pi * freq / kRate);
    double s1 = 0, s2 = 0;
    for (size_t i = from; i < from + frames; ++i) {
        const double s = out[2 * i] + k * s1 - s2;
        s2 = s1;
        s1 = s;
    }
    return s1 * s1 + s2 * s2 - k * s1 * s2;
}

float peakOf(const std::vector<float>& out, size_t fromFrame, size_t toFrame) {
    float most = 0;
    for (size_t i = fromFrame; i < toFrame && 2 * i < out.size(); ++i) most = std::max(most, std::abs(out[2 * i]));
    return most;
}

}  // namespace

class TestUiClipView : public QObject {
    Q_OBJECT

    std::unique_ptr<test::UiSession> ui_;
    std::unique_ptr<test::TempDir> dir_;
    QQuickWindow* window_ = nullptr;
    QQuickItem* clipView_ = nullptr;
    QStringList tracks_;  // the three tracks' ids
    ClipRefs clips_;      // their clips

    sub::app::Session& session() { return ui_->session(); }
    sub::app::Project& project() { return *session().project(); }
    sub::app::ProjectEditor& editor() { return *session().editor(); }
    QUndoStack& undo() { return *session().undoStack(); }
    ClipViewController* controller() { return clipView_->property("controller").value<ClipViewController*>(); }
    QQuickItem* item(const char* name) { return clipView_->findChild<QQuickItem*>(QString::fromLatin1(name)); }
    QQuickItem* knob(const char* name) { return item(name)->findChild<QQuickItem*>(QStringLiteral("knob")); }
    QString readout(const char* name) {
        return item(name)->findChild<QObject*>(QStringLiteral("readout"))->property("text").toString();
    }
    const Clip& clip(int index) { return project().clip(clips_[index].trackId, clips_[index].clipId); }

    // Three tracks with a tone each (tone0: 2 s at 220 Hz from beat 0, tone1:
    // 3 s at 440 Hz from beat 2, tone2: 4 s at 660 Hz from beat 4), decoded.
    void threeTracks() {
        for (int i = 0; i < 3; ++i) {
            const double seconds = 2.0 + i;
            const QString path =
                test::writeWav(dir_->path(QStringLiteral("tone%1.wav").arg(i)), tone(seconds, 220.0 * (i + 1)), 2);
            const ClipRefs refs = editor().addClips(QString(), i * 2.0, {{path, seconds}},
                                                    static_cast<int>(project().tracks().size()));
            QCOMPARE(refs.size(), 1);
            clips_.append(refs.front());
            tracks_.append(refs.front().trackId);
        }
        for (int i = 0; i < 3; ++i) QTRY_VERIFY(!session().bridge()->waveform(clip(i).path).isNull());
    }

    void open(const ClipRefs& refs, const QString& lead = {}) {
        QVariantList ids;
        for (const ClipRef& ref : refs)
            ids.append(QVariantMap{{QStringLiteral("trackId"), ref.trackId}, {QStringLiteral("clipId"), ref.clipId}});
        clipView_->setProperty("leadClipId", lead);
        clipView_->setProperty("clipIds", ids);
    }

    // Turns a knob by dragging it up `pixels` (600 for its whole range), in two moves.
    void turn(const char* name, int pixels) {
        const QPoint center = test::centerOf(knob(name));
        test::press(window_, center);
        test::moveTo(window_, center - QPoint(0, pixels / 2));
        test::moveTo(window_, center - QPoint(0, pixels));
        test::release(window_, center - QPoint(0, pixels));
    }

    void chooseWarpMode(const QString& mode) {
        QMetaObject::invokeMethod(item("warpMode"), "activated", Q_ARG(int, int(sub::app::kWarpModes.indexOf(mode))));
    }

    // The rows of a column of the waveform drawn in the track's colour.
    int drawnRows(const Clip& shown, int x) {
        auto* waveform = item("clipWaveform");
        const QImage image = window_->grabWindow();
        const QColor color(project().track(clips_.front().trackId).color);
        int rows = 0;
        const QPoint top = test::at(waveform, QPointF(x, sub::ui::ClipWaveform::kRulerHeight));
        for (int y = top.y(); y < top.y() + int(waveform->height()) - sub::ui::ClipWaveform::kRulerHeight; ++y) {
            if (image.pixelColor(top.x(), y).rgb() == color.rgb()) ++rows;
        }
        Q_UNUSED(shown);
        return rows;
    }

private Q_SLOTS:
    void initTestCase() {
        test::prepareApplication();
        if (!test::haveDisplay()) QSKIP("needs a display: Qt Quick's software renderer draws none of this geometry");
        sub::ui::setUpApplication();
        ui_ = std::make_unique<test::UiSession>();
        window_ = ui_->show(kWindow);
        QVERIFY(window_);
        clipView_ = window_->findChild<QQuickItem*>(QStringLiteral("clipView"));
        QVERIFY(clipView_);
    }

    void cleanupTestCase() { ui_.reset(); }

    void init() {
        clipView_->setProperty("clipIds", QVariantList());
        project().clear();
        undo().clear();
        dir_ = std::make_unique<test::TempDir>();
        tracks_.clear();
        clips_.clear();
        test::moveTo(window_, QPoint(5, 5), {}, Qt::NoButton);
    }

    void openingAnAudioClip() {
        threeTracks();
        QVERIFY(!controller()->count());
        clipView_->setProperty("trackId", tracks_[0]);
        clipView_->setProperty("clipIds", QVariantList{clips_[0].clipId});
        QCOMPARE(controller()->refs(), ClipRefs{clips_[0]});
        QVERIFY(!controller()->midi());
        QVERIFY(item("audioPage")->isVisible());
        QVERIFY(!item("pianoRollView")->isVisible());
        QVERIFY(clipView_->hasActiveFocus());  // for Esc
        QCOMPARE(clipView_->findChild<QObject*>(QStringLiteral("clipName"))->property("text").toString(), clip(0).name);
        QCOMPARE(controller()->info(), QStringLiteral("2.00 s  ·  4.00 beats"));
        QCOMPARE(controller()->audioClips().size(), size_t(1));
        QCOMPARE(controller()->audioClips().front().first, clip(0));
        QCOMPARE(controller()->audioClips().front().second, QColor(project().track(tracks_[0]).color));
        QVERIFY(!window_->grabWindow().isNull());
        test::screenshot(window_, QStringLiteral("clip-view-audio"));
        // Esc (or ×) goes back to the devices.
        QSignalSpy closing(clipView_, SIGNAL(closeRequested()));
        QTest::keyClick(window_, Qt::Key_Escape);
        QCOMPARE(closing.count(), 1);
        test::click(window_, test::centerOf(item("closeButton")));
        QCOMPARE(closing.count(), 2);
        // Deleting the clip closes the view.
        editor().deleteClips({clips_[0]});
        QCOMPARE(closing.count(), 3);
        QVERIFY(!controller()->count());
        QVERIFY(!item("audioPage")->isVisible());
    }

    void editingSeveralClipsInUnison() {
        threeTracks();
        const ClipRefs refs{clips_[0], clips_[1]};
        editor().updateClips(
            {clips_[1]},
            [](const Clip& c) {
                Clip changed = c;
                changed.transpose = 5;
                changed.warpMode = QStringLiteral("Smooth");
                return changed;
            },
            QStringLiteral("setup"));
        open({clips_[1], clips_[0]});  // shown top track first
        QCOMPARE(controller()->refs(), refs);
        QCOMPARE(controller()->name(), QStringLiteral("2 Clips"));
        QCOMPARE(controller()->info(), QStringLiteral("on 2 tracks  ·  changes apply to every selected clip"));
        QCOMPARE(controller()->audioClips().size(), size_t(2));
        QCOMPARE(controller()->warpModeIndex(), -1);  // modes differ
        QCOMPARE(item("warpMode")->property("displayText").toString(), QStringLiteral("Mixed"));
        QCOMPARE(readout("transpose"), QStringLiteral("+0 st … +5 st"));
        QCOMPARE(knob("transpose")->property("value").toDouble(), 0.0);  // the first clip's
        test::screenshot(window_, QStringLiteral("clip-view-several"));

        // A knob drag moves every clip by the same amount, as one undo step.
        const int depth = undo().count();
        turn("transpose", 14);  // 2.24 semitones (600 px for 96)
        QCOMPARE(clip(0).transpose, 2);
        QCOMPARE(clip(1).transpose, 7);
        QCOMPARE(undo().count(), depth + 1);
        QCOMPARE(undo().undoText(), QStringLiteral("Transpose Clips"));
        QCOMPARE(readout("transpose"), QStringLiteral("+2 st … +7 st"));
        // Choosing a warp mode sets it on all of them.
        chooseWarpMode(QStringLiteral("Transients"));
        QCOMPARE(clip(0).warpMode, QStringLiteral("Transients"));
        QCOMPARE(clip(1).warpMode, QStringLiteral("Transients"));
        QCOMPARE(item("warpMode")->property("currentText").toString(), QStringLiteral("Transients"));
        QCOMPARE(undo().undoText(), QStringLiteral("Change Warp Mode"));
        test::click(window_, test::centerOf(item("warp")));
        QVERIFY(clip(0).warp && clip(1).warp);
        QVERIFY(item("warp")->property("checked").toBool());
        undo().undo();
        undo().undo();
        undo().undo();
        QCOMPARE(clip(0).transpose, 0);
        QCOMPARE(clip(1).transpose, 5);
        QCOMPARE(clip(2).transpose, 0);  // not open, untouched
        QVERIFY(!item("warp")->property("checked").toBool());
        // A clip held at a limit keeps its offset to the others when the knob comes back.
        const QPoint center = test::centerOf(knob("transpose"));
        test::press(window_, center);
        test::moveTo(window_, center - QPoint(0, 275));  // +44 semitones: the second clip stops at +48
        QCOMPARE(clip(1).transpose, 48);
        test::moveTo(window_, center);
        test::release(window_, center);
        QCOMPARE(clip(0).transpose, 0);
        QCOMPARE(clip(1).transpose, 5);
        QCOMPARE(undo().undoText(), QStringLiteral("Transpose Clips"));
        undo().undo();
        // Deleting one clip keeps the view open on the other.
        QSignalSpy closing(clipView_, SIGNAL(closeRequested()));
        editor().deleteClips({clips_[0]});
        QCOMPARE(closing.count(), 0);
        QCOMPARE(controller()->refs(), ClipRefs{clips_[1]});
        QCOMPARE(controller()->name(), clip(1).name);
    }

    void warpingReachesTheAudio() {
        threeTracks();
        for (int i = 1; i < 3; ++i) editor().setTrackParam(tracks_[i], sub::app::TrackField::Mute, 1.0);
        open({clips_[0]});  // tone0: 2 s at 220 Hz, beats 0..4

        // Warp on: the segment BPM takes the project tempo, so nothing moves yet.
        test::click(window_, test::centerOf(item("warp")));
        QVERIFY(clip(0).warp);
        QCOMPARE(clip(0).segmentBpm, 120.0);
        QCOMPARE(controller()->segmentBpm(), 120.0);
        QVERIFY(std::abs(clip(0).endBeat(project().tempo()) - 4.0) < 1e-9);
        QCOMPARE(undo().undoText(), QStringLiteral("Toggle Warp"));
        // Doubling the tempo halves its duration and keeps it on the beat grid.
        editor().setTempo(240.0);
        QVERIFY(std::abs(clip(0).endBeat(project().tempo()) - 4.0) < 1e-9);
        QVERIFY(controller()->info().contains(QStringLiteral("1.00 s")));
        std::vector<float> out = ui_->engine().renderOffline(0.0, 2 * kRate);
        QVERIFY(peakOf(out, kRate / 2, kRate - 2000) > 0.1f);
        QCOMPARE(peakOf(out, kRate + 10, 2 * kRate), 0.0f);

        // Transpose is heard (an octave up).
        turn("transpose", 75);  // 12 semitones
        QCOMPARE(clip(0).transpose, 12);
        out = ui_->engine().renderOffline(0.0, kRate);
        QVERIFY(power(out, kRate / 4, 16384, 440.0) > 20 * power(out, kRate / 4, 16384, 220.0));

        // Re-Pitch ignores transposition, so its knobs are greyed out, their tooltips saying why.
        chooseWarpMode(QStringLiteral("Re-Pitch"));
        QVERIFY(controller()->repitch());
        QVERIFY(!item("transpose")->property("active").toBool() && !item("detune")->property("active").toBool());
        QVERIFY(!knob("transpose")->isEnabled());
        QVERIFY(controller()->knobs().value(QStringLiteral("transpose")).toMap().value(QStringLiteral("tooltip"))
                    .toString().startsWith(QStringLiteral("Re-Pitch: the pitch follows the speed")));
        QCOMPARE(clip(0).warpMode, QStringLiteral("Re-Pitch"));
        QCOMPARE(clip(0).transpose, 12);
        chooseWarpMode(QStringLiteral("Formants"));
        QVERIFY(item("transpose")->property("active").toBool());
        QVERIFY(knob("transpose")->isEnabled());

        // :2 halves the segment BPM: the clip plays twice as fast, half as long; ×2 doubles it.
        const double before = clip(0).endBeat(project().tempo());
        test::click(window_, test::centerOf(item("halveBpm")));
        QCOMPARE(clip(0).segmentBpm, 60.0);
        QVERIFY(std::abs(clip(0).endBeat(project().tempo()) - before / 2) < 1e-9);
        QCOMPARE(undo().undoText(), QStringLiteral("Change Segment BPM"));
        test::click(window_, test::centerOf(item("doubleBpm")));
        QCOMPARE(clip(0).segmentBpm, 120.0);
        QCOMPARE(item("segmentBpm")->property("value").toDouble(), 120.0);
        // Dragging the segment BPM box sets it (one undo step a drag).
        const int depth = undo().count();
        const QPoint box = test::centerOf(item("segmentBpm"));
        test::press(window_, box);
        test::moveTo(window_, box - QPoint(0, 20));
        test::moveTo(window_, box - QPoint(0, 40));
        test::release(window_, box - QPoint(0, 40));
        QVERIFY(clip(0).segmentBpm > 120.0);
        QCOMPARE(clip(0).segmentBpm, item("segmentBpm")->property("value").toDouble());
        QCOMPARE(undo().count(), depth + 1);
        // ...kept within 20 to 999 BPM by :2 and ×2.
        for (int i = 0; i < 6; ++i) test::click(window_, test::centerOf(item("halveBpm")));
        QCOMPARE(clip(0).segmentBpm, 20.0);
    }

    void clipGainIsCalledGainAndMakesTheWaveformTaller() {
        // A square wave from -0.25 to 0.25: its waveform a band a quarter of the lane each way.
        std::vector<float> square(kRate);
        for (int i = 0; i < kRate; ++i) square[size_t(i)] = i % 2 ? 0.25f : -0.25f;
        const QString path = test::writeWav(dir_->path(QStringLiteral("quiet.wav")), square, 1);
        const ClipRefs refs = editor().addClips(QString(), 0.0, {{path, 1.0}});
        clips_ = refs;
        QTRY_VERIFY(!session().bridge()->waveform(path).isNull());
        open(refs);
        QCOMPARE(item("gain")->findChild<QObject*>(QStringLiteral("caption"))->property("text").toString(),
                 QStringLiteral("Gain"));
        QCOMPARE(readout("gain"), QStringLiteral("0.0 dB"));
        QTest::qWait(50);
        const int lane = int(item("clipWaveform")->height()) - sub::ui::ClipWaveform::kRulerHeight;
        const int quiet = drawnRows(clip(0), 300);
        QVERIFY2(std::abs(quiet - lane / 4) <= 3, qPrintable(QStringLiteral("%1 of %2").arg(quiet).arg(lane)));
        // Turned up 6 dB (twice as loud): twice as tall.
        turn("gain", 38);  // 94 dB over 600 px: 5.95 dB
        QVERIFY(std::abs(clip(0).gainDb - 38 * 94.0 / 600) < 1e-9);
        QCOMPARE(undo().undoText(), QStringLiteral("Change Clip Gain"));
        QTest::qWait(50);
        const int louder = drawnRows(clip(0), 300);
        QVERIFY2(std::abs(louder - 2 * quiet) <= 4, qPrintable(QStringLiteral("%1 then %2").arg(quiet).arg(louder)));
        // Too loud for the lane: cut off at its edges.
        turn("gain", 150);
        QTest::qWait(50);
        QVERIFY(drawnRows(clip(0), 300) >= lane - 3);
        test::screenshot(window_, QStringLiteral("clip-view-gain"));
    }

    void aMidiLeadOpensAloneInThePianoRoll() {
        threeTracks();
        const QString midiTrack = editor().addMidiTrack();
        const auto midiClip = editor().addMidiClip(midiTrack, 0.0, 4.0);
        QVERIFY(midiClip);
        // Led by an audio clip: the audio clips among them.
        open({*midiClip, clips_[1], clips_[0]}, clips_[1].clipId);
        QVERIFY(!controller()->midi());
        QCOMPARE(controller()->refs(), (ClipRefs{clips_[0], clips_[1]}));
        // Led by the MIDI clip: it alone, in the piano roll, which has the keyboard.
        open({clips_[0], *midiClip, clips_[1]}, midiClip->clipId);
        QVERIFY(controller()->midi());
        QCOMPARE(controller()->refs(), ClipRefs{*midiClip});
        QTRY_VERIFY(item("pianoRollView")->isVisible());
        QVERIFY(!item("audioPage")->isVisible());
        auto* grid = clipView_->findChild<sub::ui::NoteGrid*>(QStringLiteral("noteGrid"));
        QVERIFY(grid->hasActiveFocus());
        QCOMPARE(grid->roll()->clipId(), midiClip->clipId);
        // No lead: the first, top track first (a MIDI track added last is lowest).
        open({*midiClip, clips_[2]});
        QVERIFY(!controller()->midi());
        QCOMPARE(controller()->refs(), ClipRefs{clips_[2]});
        QVERIFY(!grid->roll()->hasClip());
        // Refs as QML writes them, on several tracks.
        clipView_->setProperty("leadClipId", QString());
        QMetaObject::invokeMethod(ui_->root(), "showRefs", Q_ARG(QVariant, clips_[1].trackId),
                                  Q_ARG(QVariant, clips_[1].clipId), Q_ARG(QVariant, clips_[0].trackId),
                                  Q_ARG(QVariant, clips_[0].clipId));
        QCOMPARE(controller()->refs(), (ClipRefs{clips_[0], clips_[1]}));
        // Plain ids are clips on `trackId`; unknown ones are left out.
        clipView_->setProperty("leadClipId", QString());
        clipView_->setProperty("trackId", midiTrack);
        clipView_->setProperty("clipIds", QVariantList{midiClip->clipId, QStringLiteral("gone")});
        QCOMPARE(controller()->refs(), ClipRefs{*midiClip});
    }

    void theWindowsShortcutsWorkOverAudioClips() {
        threeTracks();
        const QString midiTrack = editor().addMidiTrack();
        const auto midiClip = editor().addMidiClip(midiTrack, 0.0, 4.0);
        open({*midiClip});
        auto* grid = clipView_->findChild<sub::ui::NoteGrid*>(QStringLiteral("noteGrid"));
        QTRY_VERIFY(grid->hasActiveFocus());
        // The notes take Delete before the window...
        QTest::keyClick(window_, Qt::Key_Delete);
        QCOMPARE(ui_->root()->property("deletes").toInt(), 0);
        // ...but not once audio clips show: the view has the keyboard, for Esc.
        open({clips_[0]});
        QVERIFY(!grid->hasActiveFocus());
        QVERIFY(clipView_->hasActiveFocus());
        QTest::keyClick(window_, Qt::Key_Delete);
        QCOMPARE(ui_->root()->property("deletes").toInt(), 1);
        QSignalSpy closing(clipView_, SIGNAL(closeRequested()));
        QTest::keyClick(window_, Qt::Key_Escape);
        QCOMPARE(closing.count(), 1);
    }

    void theProjectResetClosesTheView() {
        threeTracks();
        open({clips_[0]});
        QSignalSpy closing(clipView_, SIGNAL(closeRequested()));
        project().clear();
        QCOMPARE(closing.count(), 1);
        QVERIFY(!controller()->count());
        // Removing a track takes its clips out.
        undo().clear();
        clips_.clear();
        tracks_.clear();
        threeTracks();
        open({clips_[0], clips_[1]});
        editor().deleteTracks({clips_[0].trackId});
        QCOMPARE(controller()->refs(), ClipRefs{clips_[1]});
        QCOMPARE(closing.count(), 1);
    }

    void severalClipsAreStackedInBands() {
        threeTracks();
        open({clips_[0], clips_[1], clips_[2]});
        QCOMPARE(controller()->name(), QStringLiteral("3 Clips"));
        QCOMPARE(controller()->info(), QStringLiteral("on 3 tracks  ·  changes apply to every selected clip"));
        test::screenshot(window_, QStringLiteral("clip-view-bands"));
        QCOMPARE(sub::ui::ClipWaveform::formatTime(0.5, 0.25), QStringLiteral("0.5"));
        QCOMPARE(sub::ui::ClipWaveform::formatTime(0.02, 0.01), QStringLiteral("0.02"));
        QCOMPARE(sub::ui::ClipWaveform::formatTime(2.0, 1.0), QStringLiteral("2"));
        QCOMPARE(sub::ui::ClipWaveform::formatTime(75.0, 15.0), QStringLiteral("1:15"));
        QCOMPARE(sub::ui::ClipWaveform::formatTime(61.5, 0.5), QStringLiteral("1:01.5"));
        QCOMPARE(sub::ui::ClipWaveform::formatTime(60.005, 0.005), QStringLiteral("1:00.005"));
    }
};

namespace {

const char* const kWindow = R"(
import QtQuick
import SUBstation

Window {
    width: 1000
    height: 420

    // The main window's Delete (for clips), counting when it fires.
    property int deletes: 0
    Shortcut { sequences: [StandardKey.Delete]; onActivated: deletes++ }

    function showRefs(track1, clip1, track2, clip2) {
        clipView.clipIds = [{trackId: track1, clipId: clip1}, {trackId: track2, clipId: clip2}]
    }

    ClipView {
        id: clipView
        objectName: "clipView"
        anchors.fill: parent
    }
}
)";

}  // namespace

QTEST_MAIN(TestUiClipView)
#include "test_ui_clipview.moc"
