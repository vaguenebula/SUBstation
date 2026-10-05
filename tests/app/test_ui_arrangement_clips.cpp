// Editing a clip by itself in the arrangement, driven as a user would: Alt on
// an edge stretches it, Ctrl+Shift on its body slides its audio or notes inside
// it, and with F held its fades are dragged by their handles (lengths and
// curves), heard as they are. Runs on a display (xvfb here). With
// $SUBSTATION_SCREENS set, it saves screenshots there.

#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <memory>

#include "ArrangementTestSupport.h"
#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "model/Clip.h"
#include "model/Project.h"
#include "session/ComputerKeyboard.h"
#include "session/Selection.h"

using sub::app::Clip;
using sub::app::Note;
using sub::ui::ArrangementLanes;
namespace test = sub::app::test;
namespace arr = sub::ui::arrangement;

namespace {

constexpr int kSamplesPerBeat = test::kSampleRate / 2;  // at 120 BPM

bool near(double a, double b, double tolerance = 1e-9) { return std::abs(a - b) <= tolerance; }

}  // namespace

class TestUiArrangementClips : public QObject {
    Q_OBJECT

    std::unique_ptr<test::UiSession> ui_;
    std::unique_ptr<test::ArrangementHarness> h_;
    std::unique_ptr<test::TempDir> dir_;
    QString dc_;  // two seconds of 0.5 in both channels

    sub::app::Session& session() { return ui_->session(); }
    sub::app::Project& project() { return *session().project(); }
    sub::app::ProjectEditor& editor() { return *session().editor(); }
    sub::app::Selection& selection() { return *session().selection(); }
    sub::app::EngineBridge& bridge() { return *session().bridge(); }
    QUndoStack& undo() { return *session().undoStack(); }
    QQuickWindow* window() { return h_->window(); }
    ArrangementLanes* lanes() { return h_->lanes(); }
    sub::ui::Arrangement* arrangement() { return h_->arrangement(); }
    double tempo() { return project().tempo(); }

    const Clip& clipOf(int track, int clip = 0) {
        return project().tracks()[static_cast<size_t>(track)].clips[static_cast<size_t>(clip)];
    }

    // An audio track playing `clip` (of dc_), loaded and drawn.
    QString dcTrack(const Clip& clip) {
        const QString track = editor().addAudioTrack(-1, QStringLiteral("DC"));
        editor().commitClips(QStringLiteral("Add"), {{track, {clip}}});
        bridge().requestSource(dc_);
        if (!QTest::qWaitFor([&] { return bridge().source(dc_) != nullptr; }, 10000)) return {};
        h_->settle();
        return track;
    }

    // What the engine plays at a beat (the left channel), offline.
    float heardAt(double beat) {
        const std::vector<float> out = ui_->engine().renderOffline(0.0, 10 * kSamplesPerBeat);
        return out[size_t(2 * int(beat * kSamplesPerBeat))];
    }

    // A clip's rectangle and its body in the lanes (as the lanes draw it).
    std::pair<QRectF, QRectF> clipArea(int track, const Clip& clip) {
        const auto& row = h_->row(track);
        const double top = row.top - arrangement()->scrollY();
        const double x0 = h_->timeline().beatToX(clip.startBeat), x1 = h_->timeline().beatToX(clip.endBeat(tempo()));
        const QRectF rect(x0, top + 1, std::max(2.0, x1 - x0), row.mainHeight - 3);
        return {rect, rect.adjusted(0, arr::clipTitleHeight(rect.height()), 0, 0)};
    }
    arr::FadeHandles handlesOf(int track, const Clip& clip) {
        const auto [rect, body] = clipArea(track, clip);
        return arr::fadeHandles(clip, rect, body, arrangement()->pxPerBeat(), tempo());
    }

private Q_SLOTS:
    void initTestCase() {
        test::prepareApplication();
        if (!test::haveDisplay()) QSKIP("needs a display: Qt Quick's software renderer draws none of this geometry");
        sub::ui::setUpApplication();
        ui_ = std::make_unique<test::UiSession>();
        h_ = std::make_unique<test::ArrangementHarness>(*ui_);
        QVERIFY(h_->show());
        dir_ = std::make_unique<test::TempDir>();
        dc_ = test::writeWav(dir_->path(QStringLiteral("dc.wav")), test::constant(2.0, 0.5f), 2);
    }

    void cleanupTestCase() {
        h_.reset();
        ui_.reset();
    }

    void init() {
        bridge().stop();
        session().newProject();
        undo().clear();
        arrangement()->setSnap(true);
        arrangement()->setGridLevel(0);
        h_->setZoom(sub::ui::timeline::Timeline::kDefaultPxPerBeat);  // a one-beat grid
        arrangement()->setScrollBeats(0.0);
        arrangement()->setScrollY(0);
        test::moveTo(window(), QPoint(2, 2), {}, Qt::NoButton);
        h_->settle();
    }

    void cleanup() {
        QTest::keyRelease(window(), Qt::Key_F);
        session().computerKeyboard()->setEnabled(false);
        bridge().stop();
    }

    void altOnAnEdgeStretchesTheClip() {
        const QString track = dcTrack(Clip::audio(QStringLiteral("c"), dc_, QStringLiteral("DC"), 0.0, 2.0, 0.0, 2.0));
        QVERIFY(!track.isEmpty());
        const Clip clip = clipOf(0);  // beats 0-4
        const double y = h_->row(0).top - arrangement()->scrollY() + 6;  // its title bar
        const QPoint end = h_->at(QPointF(h_->x(4.0) - 2, y));
        // Over its end, holding Alt shows the stretch (without moving the mouse).
        test::moveTo(window(), end, {}, Qt::NoButton);
        QCOMPARE(lanes()->hoverEdge(), std::make_optional(std::make_pair(clip.id, false)));
        QVERIFY(!lanes()->hoverStretch());
        QTest::keyPress(window(), Qt::Key_Alt, Qt::AltModifier);
        QVERIFY(lanes()->hoverStretch());
        QCOMPARE(lanes()->cursor().shape(), Qt::BitmapCursor);
        test::screenshot(window(), QStringLiteral("arrangement_stretch_edge"));
        // Dragged to beat 8: twice as long, the same audio at half speed (warped to do it).
        test::press(window(), end, Qt::AltModifier);
        test::moveTo(window(), h_->at(QPointF(h_->x(6.0), y)), Qt::AltModifier);
        test::moveTo(window(), h_->at(QPointF(h_->x(8.0) + 3, y)), Qt::AltModifier);  // (on the grid)
        QCOMPARE(clipOf(0).endBeat(tempo()), 4.0);  // the model waits for the drop...
        QVERIFY(std::abs(heardAt(6.0)) > 0.1f);     // ...the engine plays it stretched already
        test::release(window(), h_->at(QPointF(h_->x(8.0) + 3, y)), Qt::AltModifier);
        QTest::keyRelease(window(), Qt::Key_Alt, Qt::NoModifier);
        QVERIFY(!lanes()->hoverStretch());
        const Clip stretched = clipOf(0);
        QCOMPARE(undo().undoText(), QStringLiteral("Stretch Clip"));
        QCOMPARE(stretched.startBeat, 0.0);
        QVERIFY(near(stretched.endBeat(tempo()), 8.0));
        QVERIFY(stretched.isWarped());
        QVERIFY(near(stretched.segmentBpm, 240.0));  // (it played at the project's 120)
        QCOMPARE(stretched.durationSec, clip.durationSec);
        QCOMPARE(stretched.offsetSec, clip.offsetSec);
        QVERIFY(std::abs(heardAt(7.0)) > 0.1f);
        // By its start, Shift off the grid: the end stays.
        test::drag(window(), h_->at(QPointF(h_->x(0.0) + 2, y)), h_->at(QPointF(h_->x(2.5), y)),
                   Qt::AltModifier | Qt::ShiftModifier);
        QVERIFY(std::abs(clipOf(0).startBeat - 2.5) < 0.1);
        QVERIFY(near(clipOf(0).endBeat(tempo()), 8.0, 1e-6));
        undo().undo();
        // Without Alt the edge trims, as ever.
        test::drag(window(), h_->at(QPointF(h_->x(8.0) - 2, y)), h_->at(QPointF(h_->x(6.0), y)));
        QCOMPARE(undo().undoText(), QStringLiteral("Trim Clip"));
        QVERIFY(near(clipOf(0).endBeat(tempo()), 6.0));
        QCOMPARE(clipOf(0).segmentBpm, 240.0);

        // A MIDI clip's notes stretch with it.
        const QString midi = editor().addMidiTrack();
        editor().commitClips(QStringLiteral("Add"),
                             {{midi, {Clip::midi(QStringLiteral("m"), QStringLiteral("M"), 0.0, 2.0, 0.0,
                                                 {Note{60, 0.0, 0.5, 100}, Note{64, 1.0, 0.5, 100}})}}});
        h_->settle();
        const double midiY = h_->row(1).top - arrangement()->scrollY() + 6;
        test::drag(window(), h_->at(QPointF(h_->x(2.0) - 2, midiY)), h_->at(QPointF(h_->x(4.0), midiY)),
                   Qt::AltModifier);
        const Clip notes = clipOf(1);
        QCOMPARE(notes.durationBeats, 4.0);
        QCOMPARE(notes.playedNotes()[1].start, 2.0);
        QCOMPARE(notes.playedNotes()[1].end, 3.0);
    }

    void ctrlShiftOnTheBodySlidesTheContent() {
        // Beats 4-6, playing seconds 1-2 of the file.
        const QString track = dcTrack(Clip::audio(QStringLiteral("c"), dc_, QStringLiteral("DC"), 4.0, 1.0, 1.0, 2.0));
        QVERIFY(!track.isEmpty());
        const QPoint body = h_->lane(0, 5.0);
        test::moveTo(window(), body, Qt::ControlModifier | Qt::ShiftModifier, Qt::NoButton);
        QCOMPARE(lanes()->cursor().shape(), Qt::SizeHorCursor);
        // A beat to the right: the audio half a second later on the timeline, the clip where it was.
        test::drag(window(), body, body + QPoint(int(arrangement()->pxPerBeat()), 0),
                   Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(undo().undoText(), QStringLiteral("Move Clip Content"));
        QCOMPARE(clipOf(0).startBeat, 4.0);
        QCOMPARE(clipOf(0).durationSec, 1.0);
        QVERIFY(near(clipOf(0).offsetSec, 0.5));
        QVERIFY(selection().clips().contains({track, QStringLiteral("c")}));
        // By whole grid steps (less than half a beat is none), and not past the file's start.
        test::drag(window(), body, body + QPoint(int(arrangement()->pxPerBeat() * 0.4), 0),
                   Qt::ControlModifier | Qt::ShiftModifier);
        QVERIFY(near(clipOf(0).offsetSec, 0.5));
        test::drag(window(), body, body + QPoint(int(arrangement()->pxPerBeat() * 3), 0),
                   Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(clipOf(0).offsetSec, 0.0);
        // Alt held once it drags: freely. (Ctrl+Alt at the press scrolls by hand instead.)
        const QPoint half = body - QPoint(int(arrangement()->pxPerBeat() / 2), 0);
        test::press(window(), body, Qt::ControlModifier | Qt::ShiftModifier);
        test::moveTo(window(), half, Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
        test::release(window(), half, Qt::ControlModifier | Qt::ShiftModifier | Qt::AltModifier);
        QVERIFY(near(clipOf(0).offsetSec, 0.25));
        // Without Ctrl+Shift the body selects time, as ever.
        test::drag(window(), h_->lane(0, 4.0), h_->lane(0, 6.0));
        QVERIFY(near(clipOf(0).offsetSec, 0.25));

        // A MIDI clip's notes move under it.
        const QString midi = editor().addMidiTrack();
        editor().commitClips(QStringLiteral("Add"),
                             {{midi, {Clip::midi(QStringLiteral("m"), QStringLiteral("M"), 0.0, 4.0, 0.0,
                                                 {Note{60, 1.0, 0.5, 100}})}}});
        h_->settle();
        test::drag(window(), h_->lane(1, 2.0), h_->lane(1, 4.0), Qt::ControlModifier | Qt::ShiftModifier);
        QCOMPARE(clipOf(1).startBeat, 0.0);
        QCOMPARE(clipOf(1).durationBeats, 4.0);
        QCOMPARE(clipOf(1).playedNotes().front().start, 3.0);
    }

    void holdingFDragsAClipsFades() {
        const QString track = dcTrack(Clip::audio(QStringLiteral("c"), dc_, QStringLiteral("DC"), 0.0, 2.0, 0.0, 2.0));
        QVERIFY(!track.isEmpty());
        QVERIFY(!lanes()->fadeKeyHeld());
        QTest::keyPress(window(), Qt::Key_F);
        QVERIFY(lanes()->fadeKeyHeld());
        // Its handles, at the top corners of its body while it has no fades.
        arr::FadeHandles handles = handlesOf(0, clipOf(0));
        const auto [rect, body] = clipArea(0, clipOf(0));
        QVERIFY(std::abs(handles.in.left() - rect.left()) < 1e-9);
        QVERIFY(std::abs(handles.out.right() - rect.right()) < 1e-9);
        QVERIFY(std::abs(handles.in.top() - body.top() - 1) < 1e-9);
        QVERIFY(!handles.inCurve && !handles.outCurve);
        const QPointF inSquare = handles.in.center();
        test::moveTo(window(), h_->at(inSquare), {}, Qt::NoButton);
        QCOMPARE(lanes()->cursor().shape(), Qt::SizeHorCursor);
        const auto hit = lanes()->hitFade(inSquare);
        QVERIFY(hit && !hit->out && !hit->curve && hit->clip.id == QStringLiteral("c"));

        // A beat in: half a second, off the grid as the mouse goes; heard so (a straight line up).
        const int beat = int(arrangement()->pxPerBeat());
        QPoint from = h_->at(inSquare);
        test::press(window(), from);
        test::moveTo(window(), from + QPoint(beat / 2, 0));
        test::moveTo(window(), from + QPoint(beat, 0));
        QCOMPARE(clipOf(0).fadeInSec, 0.0);  // the model waits for the drop...
        QVERIFY(std::abs(heardAt(0.5) - 0.25f) < 1e-3);  // ...the engine plays the fade already
        test::release(window(), from + QPoint(beat, 0));
        QCOMPARE(undo().undoText(), QStringLiteral("Fade In"));
        QVERIFY(near(clipOf(0).fadeInSec, 0.5));
        QVERIFY(std::abs(heardAt(0.5) - 0.25f) < 1e-3);
        QVERIFY(std::abs(heardAt(2.0) - 0.5f) < 1e-3);
        // Two beats out, from its end.
        handles = handlesOf(0, clipOf(0));
        from = h_->at(handles.out.center());
        test::drag(window(), from, from - QPoint(2 * beat, 0));
        QCOMPARE(undo().undoText(), QStringLiteral("Fade Out"));
        QVERIFY(near(clipOf(0).fadeOutSec, 1.0));
        QVERIFY(std::abs(heardAt(3.0) - 0.25f) < 1e-3);
        test::screenshot(window(), QStringLiteral("arrangement_fades"));

        // The dot on the fade out's curve, dragged up: it bulges up (louder for longer).
        handles = handlesOf(0, clipOf(0));
        QVERIFY(handles.inCurve && handles.outCurve);
        from = h_->at(*handles.outCurve);
        test::moveTo(window(), from, {}, Qt::NoButton);
        QCOMPARE(lanes()->cursor().shape(), Qt::SizeVerCursor);
        test::drag(window(), from, from - QPoint(0, int(arr::kCurvePixels / 2)));
        QCOMPARE(undo().undoText(), QStringLiteral("Fade Curve"));
        QVERIFY(near(clipOf(0).fadeOutCurve, 0.5, 0.02));
        QVERIFY(heardAt(3.0) > 0.3f);
        test::screenshot(window(), QStringLiteral("arrangement_fade_curve"));
        // Double-clicked, the dot straightens it; the square takes the fade away.
        handles = handlesOf(0, clipOf(0));
        test::doubleClick(window(), h_->at(*handles.outCurve));
        QCOMPARE(clipOf(0).fadeOutCurve, 0.0);
        QVERIFY(near(clipOf(0).fadeOutSec, 1.0));
        test::doubleClick(window(), h_->at(handlesOf(0, clipOf(0)).out.center()));
        QCOMPARE(clipOf(0).fadeOutSec, 0.0);
        QVERIFY(near(clipOf(0).fadeInSec, 0.5));

        // F let go: no handles; a press there selects time as ever.
        QTest::keyRelease(window(), Qt::Key_F);
        QVERIFY(!lanes()->fadeKeyHeld());
        handles = handlesOf(0, clipOf(0));
        test::drag(window(), h_->at(handles.in.center()), h_->at(handles.in.center()) + QPoint(beat, 0));
        QVERIFY(near(clipOf(0).fadeInSec, 0.5));
        QVERIFY(selection().timeRange().has_value());

        // While the computer MIDI keyboard is on, F plays a note instead.
        session().computerKeyboard()->setEnabled(true);
        QTest::keyPress(window(), Qt::Key_F);
        QVERIFY(!lanes()->fadeKeyHeld());
        QTest::keyRelease(window(), Qt::Key_F);
        session().computerKeyboard()->setEnabled(false);

        // MIDI clips have no fades.
        const QString midi = editor().addMidiTrack();
        editor().commitClips(QStringLiteral("Add"), {{midi, {Clip::midi(QStringLiteral("m"), QStringLiteral("M"), 0.0, 4.0)}}});
        h_->settle();
        QTest::keyPress(window(), Qt::Key_F);
        QVERIFY(!lanes()->hitFade(handlesOf(1, clipOf(1)).in.center()));
    }
};

QTEST_MAIN(TestUiArrangementClips)
#include "test_ui_arrangement_clips.moc"
