// The piano roll's bends (MIDI 2.0's per-note pitch bend), driven through the
// clip view the way a user would: B (or the bend button) showing the notes'
// bend curves and the bend bar, always with the Draw tool; points added on a
// curve and dragged there, clicked away, selected with Ctrl and a rubber band
// and deleted with Delete,
// a segment bent with Alt, a point double-clicked in at a pitch; the vibrato
// tool (V) drawing vibrato over a stretch, adding it to a note's end and taking
// it away, Shift and Alt changing its speed and ramp as it is drawn, Ctrl
// drawing it off the grid; the glide tool (G) sliding into the next note;
// Shift showing the notes to edit as out of bend mode; Clear; the note tools staying away in bend mode. Runs on a display
// (xvfb here).

#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <memory>

#include "UiTestSupport.h"
#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "intelligence/Harmony.h"
#include "model/Clip.h"
#include "model/Project.h"
#include "pianoroll/NoteGrid.h"
#include "pianoroll/PianoRoll.h"

using sub::app::Note;
using sub::ui::NoteGrid;
using sub::ui::PianoRoll;
namespace test = sub::app::test;

namespace {

extern const char* const kWindow;

Note note(int pitch, double start, double length) {
    Note n;
    n.pitch = pitch;
    n.start = start;
    n.length = length;
    return n;
}

}  // namespace

class TestUiPianoRollBends : public QObject {
    Q_OBJECT

    std::unique_ptr<test::UiSession> ui_;
    QQuickWindow* window_ = nullptr;
    QQuickItem* clipView_ = nullptr;
    QString trackId_;
    QString clipId_;

    sub::app::Session& session() { return ui_->session(); }
    sub::app::Project& project() { return *session().project(); }
    QUndoStack& undo() { return *session().undoStack(); }
    NoteGrid* grid() { return clipView_->findChild<NoteGrid*>(QStringLiteral("noteGrid")); }
    PianoRoll* roll() { return grid()->roll(); }
    QQuickItem* item(const char* name) { return clipView_->findChild<QQuickItem*>(QString::fromLatin1(name)); }
    std::vector<Note> clipNotes() { return project().clip(trackId_, clipId_).notes; }
    Note only() {
        const std::vector<Note> notes = clipNotes();
        return notes.size() == 1 ? notes.front() : Note{};
    }

    // On a note's curve (with no bend: the middle of its row), `semitones` above it, at a beat.
    QPoint onCurve(double beat, int pitch, double semitones = 0.0) {
        return test::at(grid(), QPointF(roll()->view().beatToX(beat),
                                        roll()->pitchTop(pitch) + roll()->rowHeight() / 2.0 - semitones * roll()->rowHeight()));
    }

    // A MIDI track (with the Synth) and a one-bar clip at beat 4 holding `notes`, open in the piano roll.
    void openClip(const std::vector<Note>& notes) {
        trackId_ = session().editor()->addMidiTrack();
        const auto ref = session().editor()->addMidiClip(trackId_, 4.0, 4.0);
        QVERIFY(ref);
        clipId_ = ref->clipId;
        session().editor()->setClipNotes(*ref, notes, QStringLiteral("Notes"));
        clipView_->setProperty("trackId", trackId_);
        clipView_->setProperty("clipIds", QVariantList{clipId_});
        QTRY_VERIFY(grid()->isVisible());
        QTRY_VERIFY(grid()->hasActiveFocus());
    }

    void bendMode() {
        QTest::keyClick(window_, Qt::Key_B);
        QVERIFY(roll()->bendMode());
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
        session().harmony()->setShown(false);
        if (roll()) {
            roll()->setBendMode(false);
            roll()->setBendTool(QStringLiteral("draw"));
            roll()->setGlideCurve(0.0);
        }
        test::moveTo(window_, QPoint(5, 5), {}, Qt::NoButton);
    }

    void bAndTheBendButtonShowTheCurves() {
        Note scoop = note(64, 0.0, 2.0), held = note(60, 2.0, 2.0);
        scoop.bend = {{0.0, -2.0, 0.0}, {0.5, 0.0, 0.4}, {1.5, 0.0, 0.0}, {2.0, 1.0, 0.0}};
        held.vibrato = {{0.5, 1.5, 0.5, 5.5, 0.3}};
        held.bend = {{0.25, 0.0, 0.0}, {0.5, 2.0, 0.0}};
        openClip({note(60, 0.0, 2.0), scoop, held});
        QVERIFY(!item("bendTools")->isVisible());
        QTest::keyClick(window_, Qt::Key_B);
        QVERIFY(roll()->bendMode());
        QTRY_VERIFY(item("bendTools")->isVisible());
        QVERIFY(item("bendMode")->property("checked").toBool());
        test::screenshot(window_, QStringLiteral("piano-roll-bends"));
        QTest::keyClick(window_, Qt::Key_B);
        QVERIFY(!roll()->bendMode());
        QTRY_VERIFY(!item("bendTools")->isVisible());
        test::click(window_, test::centerOf(item("bendMode")));
        QVERIFY(roll()->bendMode());
        QVERIFY(grid()->hasActiveFocus());  // (the button takes no focus: the notes keep the keyboard)
        // V: the vibrato tool, and back.
        QTest::keyClick(window_, Qt::Key_V);
        QCOMPARE(roll()->bendTool(), QStringLiteral("vibrato"));
        QVERIFY(item("bendVibrato")->property("checked").toBool());
        QTest::keyClick(window_, Qt::Key_V);
        QCOMPARE(roll()->bendTool(), QStringLiteral("draw"));
        // Out of bend mode, V goes into it with the vibrato tool, G with the glide tool.
        QTest::keyClick(window_, Qt::Key_B);
        QTest::keyClick(window_, Qt::Key_V);
        QVERIFY(roll()->bendMode());
        QCOMPARE(roll()->bendTool(), QStringLiteral("vibrato"));
        // B (or the button) always comes back with Draw, whatever was used last.
        QTest::keyClick(window_, Qt::Key_B);
        QTest::keyClick(window_, Qt::Key_B);
        QCOMPARE(roll()->bendTool(), QStringLiteral("draw"));
        QTest::keyClick(window_, Qt::Key_V);
        test::click(window_, test::centerOf(item("bendMode")));
        test::click(window_, test::centerOf(item("bendMode")));
        QVERIFY(roll()->bendMode());
        QCOMPARE(roll()->bendTool(), QStringLiteral("draw"));
        QTest::keyClick(window_, Qt::Key_G);
        QCOMPARE(roll()->bendTool(), QStringLiteral("glide"));
        QVERIFY(item("bendGlide")->property("checked").toBool());
        QTest::keyClick(window_, Qt::Key_G);
        QCOMPARE(roll()->bendTool(), QStringLiteral("draw"));
        QTest::keyClick(window_, Qt::Key_B);
        QTest::keyClick(window_, Qt::Key_G);
        QVERIFY(roll()->bendMode());
        QCOMPARE(roll()->bendTool(), QStringLiteral("glide"));
    }

    void aPointAddedOnACurveIsDraggedWhereTheMouseGoes() {
        openClip({note(60, 0.0, 4.0)});
        bendMode();
        const int depth = undo().count();
        // Press on the line at beat 2, drag up two rows (and half a beat on: onto the grid).
        test::drag(window_, onCurve(2.0, 60), onCurve(2.5, 60, 2.2));
        const Note bent = only();
        QCOMPARE(bent.bend.size(), size_t(1));
        QCOMPARE(bent.bend[0].time, 2.5);
        QCOMPARE(bent.bend[0].semitones, 2.0);  // a whole semitone, the grid on
        QCOMPARE(undo().count(), depth + 1);   // added and dragged: one step
        QCOMPARE(roll()->selectedBendCount(), 1);
        undo().undo();
        QVERIFY(only().bend.empty());
        undo().redo();
        // Alt: anywhere, off the grid and between semitones. (The curve rises to the point from the note's start.)
        const double on = only().bendAt(1.0, project().tempo());
        QCOMPARE(on, 0.8);
        test::drag(window_, onCurve(1.0, 60, on), onCurve(1.1, 60, on + 0.4), Qt::AltModifier);
        QCOMPARE(only().bend.size(), size_t(2));
        QVERIFY(std::abs(only().bend[0].semitones - 1.2) < 0.15);
        QVERIFY(only().bend[0].time != 1.0);
    }

    void pointsAreClickedAwaySelectedAndDeleted() {
        Note n = note(60, 0.0, 4.0);
        n.bend = {{1.0, 0.0, 0.0}, {2.0, 2.0, 0.0}, {3.0, 0.0, 0.0}};
        openClip({n});
        bendMode();
        // A click on a point deletes it.
        test::click(window_, onCurve(2.0, 60, 2.0));
        QCOMPARE(only().bend.size(), size_t(2));
        QCOMPARE(undo().undoText(), QStringLiteral("Delete Bend Point"));
        undo().undo();
        // Ctrl-click selects (each), Delete deletes them.
        test::click(window_, onCurve(1.0, 60), Qt::ControlModifier);
        test::click(window_, onCurve(3.0, 60), Qt::ControlModifier);
        QCOMPARE(roll()->selectedBendCount(), 2);
        QCOMPARE(only().bend.size(), size_t(3));
        QTest::keyClick(window_, Qt::Key_Delete);
        QCOMPARE(only().bend.size(), size_t(1));
        QCOMPARE(only().bend[0].semitones, 2.0);
        QCOMPARE(clipNotes().size(), size_t(1));  // (in bend mode Delete takes points, never notes)
        undo().undo();
        // A rubber band selects the points it covers; Ctrl+A selects them all.
        test::drag(window_, onCurve(0.5, 60, 3.0), onCurve(2.5, 60, -1.0));
        QCOMPARE(roll()->selectedBendCount(), 2);
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(roll()->selectedBendCount(), 3);
        // Dragged together: by as much as the one grabbed.
        test::drag(window_, onCurve(2.0, 60, 2.0), onCurve(2.0, 60, 3.0));
        QCOMPARE(only().bend[0].semitones, 1.0);
        QCOMPARE(only().bend[1].semitones, 3.0);
        QCOMPARE(only().bend[2].semitones, 1.0);
    }

    void altDragBendsASegment() {
        Note n = note(60, 0.0, 4.0);
        n.bend = {{1.0, 0.0, 0.0}, {3.0, 4.0, 0.0}};
        openClip({n});
        bendMode();
        // Between the two points, on the line: up bulges it upward.
        const QPoint middle = onCurve(2.0, 60, 2.0);
        test::drag(window_, middle, middle - QPoint(0, 60), Qt::AltModifier);
        QVERIFY(only().bend[0].curve > 0.2);
        QCOMPARE(undo().undoText(), QStringLiteral("Bend Segment"));
    }

    void aDoubleClickPutsAPointAtThePitchClicked() {
        openClip({note(60, 0.0, 4.0), note(67, 0.0, 4.0)});
        bendMode();
        // Three semitones above C3's row: C3's curve is nearest.
        test::doubleClick(window_, onCurve(2.0, 60, 3.0));
        std::vector<Note> notes = clipNotes();
        const Note& c = notes[0].pitch == 60 ? notes[0] : notes[1];
        QCOMPARE(c.bend.size(), size_t(1));
        QCOMPARE(c.bend[0].semitones, 3.0);
        QCOMPARE(c.bend[0].time, 2.0);
    }

    void theVibratoToolDrawsVibrato() {
        openClip({note(60, 0.0, 4.0)});
        bendMode();
        QTest::keyClick(window_, Qt::Key_V);
        // Dragged across from beat 1 to beat 3, on the note's row.
        test::drag(window_, onCurve(1.0, 60), onCurve(3.0, 60));
        Note n = only();
        QCOMPARE(n.vibrato.size(), size_t(1));
        QCOMPARE(n.vibrato[0].start, 1.0);
        QCOMPARE(n.vibrato[0].length, 2.0);
        QCOMPARE(n.vibrato[0].depth, roll()->vibratoDepth());
        QCOMPARE(n.vibrato[0].rate, roll()->vibratoRate());
        QCOMPARE(undo().undoText(), QStringLiteral("Draw Vibrato"));
        // A click on it takes it away; a click on the note adds it from there to its end.
        test::click(window_, onCurve(2.0, 60, n.bendAt(2.0, project().tempo())));
        QVERIFY(only().vibrato.empty());
        roll()->setVibratoRate(7.0);
        test::click(window_, onCurve(3.0, 60));
        n = only();
        QCOMPARE(n.vibrato.size(), size_t(1));
        QCOMPARE(n.vibrato[0].start, 3.0);
        QCOMPARE(n.vibrato[0].length, 1.0);
        QCOMPARE(n.vibrato[0].rate, 7.0);
        // Dragged up as it is drawn: deeper.
        undo().undo();
        test::drag(window_, onCurve(1.0, 60), onCurve(3.0, 60, 1.0));
        QVERIFY(only().vibrato[0].depth > roll()->vibratoDepth() + 0.5);
        // It goes with the curve drawn by hand: a point added on it later bends what it swings around.
        QTest::keyClick(window_, Qt::Key_V);
        test::doubleClick(window_, onCurve(0.5, 60, 2.0));
        n = only();
        QCOMPARE(n.bend.size(), size_t(1));
        QCOMPARE(n.vibrato.size(), size_t(1));
        QCOMPARE(n.bendAt(3.5, project().tempo()), 2.0);  // past the vibrato: the curve, held
    }

    void shiftAndAltSetAVibratosSpeedAndRampAsItIsDrawn() {
        openClip({note(60, 0.0, 4.0)});
        bendMode();
        QTest::keyClick(window_, Qt::Key_V);
        const double rate = roll()->vibratoRate(), fade = roll()->vibratoFade() / 100.0;
        const auto drawn = [&] {
            const Note n = only();
            return n.vibrato.size() == 1 ? n.vibrato.front() : sub::app::Vibrato{};
        };
        test::press(window_, onCurve(1.0, 60));
        test::moveTo(window_, onCurve(2.0, 60));
        test::moveTo(window_, onCurve(3.0, 60));
        QCOMPARE(drawn().length, 2.0);
        QCOMPARE(drawn().rate, rate);
        QCOMPARE(drawn().fade, fade);
        // Shift: sideways is its speed (100 px doubles it); the stretch stays.
        const QPoint shifted = onCurve(3.0, 60) + QPoint(int(NoteGrid::kVibratoRatePixels), 0);
        test::moveTo(window_, shifted, Qt::ShiftModifier);
        QCOMPARE(drawn().rate, std::round(rate * 2.0 * 10.0) / 10.0);
        QCOMPARE(drawn().start, 1.0);
        QCOMPARE(drawn().length, 2.0);
        // Alt: sideways is its ramp (200 px from none to all of it).
        const int rampStep = int(NoteGrid::kVibratoRampPixels * 0.3);
        test::moveTo(window_, shifted + QPoint(rampStep, 0), Qt::AltModifier);
        QCOMPARE(drawn().fade, std::round((fade + 0.3) * 100.0) / 100.0);
        QCOMPARE(drawn().rate, std::round(rate * 2.0 * 10.0) / 10.0);
        QCOMPARE(drawn().length, 2.0);
        // Let go of them: the stretch goes on from where it was (a beat left: to beat 2).
        const int held = int(NoteGrid::kVibratoRatePixels) + rampStep;
        test::moveTo(window_, onCurve(2.0, 60) + QPoint(held, 0));
        QCOMPARE(drawn().length, 1.0);
        QCOMPARE(drawn().depth, roll()->vibratoDepth());  // (the depth stayed too)
        test::release(window_, onCurve(2.0, 60) + QPoint(held, 0));
        QCOMPARE(drawn().rate, std::round(rate * 2.0 * 10.0) / 10.0);
        QCOMPARE(undo().undoText(), QStringLiteral("Draw Vibrato"));
        QCOMPARE(roll()->vibratoRate(), rate);  // (the bend bar keeps its own for the next one)
        undo().undo();
        QVERIFY(only().vibrato.empty());  // one undo step
        // Ctrl: off the grid.
        test::drag(window_, onCurve(1.0, 60), onCurve(2.3, 60), Qt::ControlModifier);
        const double sixteenths = drawn().length * 16.0;
        QVERIFY2(std::abs(sixteenths - std::round(sixteenths)) > 1e-6, qPrintable(QString::number(drawn().length)));
        QVERIFY(std::abs(drawn().length - 1.3) < 0.05);
    }

    void theGlideToolSlidesIntoTheNextNote() {
        // B, then G# and E together after it: G# is the nearer.
        openClip({note(71, 0.0, 2.0), note(68, 2.0, 2.0), note(64, 2.0, 1.0)});
        const auto b = [&] {
            for (const Note& n : clipNotes())
                if (n.pitch == 71) return n;
            return Note{};
        };
        bendMode();
        QTest::keyClick(window_, Qt::Key_G);
        QCOMPARE(roll()->glideCurve(), 0.0);  // (straight at first)
        QTRY_VERIFY(item("glideCurve")->isVisible());  // (the bend bar shows it with the glide tool)
        // Dragged from beat 1 to beat 2: down three semitones over that stretch.
        test::drag(window_, onCurve(1.0, 71), onCurve(2.0, 71));
        QCOMPARE(b().bend.size(), size_t(2));
        QCOMPARE(b().bend[0].time, 1.0);
        QCOMPARE(b().bend[0].semitones, 0.0);
        QCOMPARE(b().bend[1].time, 2.0);
        QCOMPARE(b().bend[1].semitones, -3.0);
        QCOMPARE(undo().undoText(), QStringLiteral("Add Slide"));
        undo().undo();
        QVERIFY(b().bend.empty());
        // Alt: sideways bends it (right: it arrives later), the stretch staying.
        test::press(window_, onCurve(1.0, 71));
        test::moveTo(window_, onCurve(1.25, 71));
        test::moveTo(window_, onCurve(1.5, 71));
        const int half = int(NoteGrid::kGlideCurvePixels / 2);
        test::moveTo(window_, onCurve(1.5, 71) + QPoint(half, 0), Qt::AltModifier);
        test::screenshot(window_, QStringLiteral("piano-roll-glide"));
        QCOMPARE(b().bend[1].time, 1.5);
        QVERIFY(std::abs(b().bend[0].curve - 0.5) < 0.02);  // (going down: bulging up arrives later)
        QVERIFY(b().bendAt(1.25, project().tempo()) > -1.5);  // later than halfway, halfway along
        test::release(window_, onCurve(1.5, 71) + QPoint(half, 0), Qt::AltModifier);
        QCOMPARE(roll()->glideCurve(), 50.0);  // the curve new slides start with now
        undo().undo();
        QVERIFY(b().bend.empty());  // (one undo step)
        // A click: from there to the note's end, with that curve.
        test::click(window_, onCurve(0.5, 71));
        QCOMPARE(b().bend.size(), size_t(2));
        QCOMPARE(b().bend[0].time, 0.5);
        QCOMPARE(b().bend[1].time, 2.0);
        QCOMPARE(b().bend[0].curve, 0.5);
        // Until it is changed: straight again (as the bend bar's box sets it).
        roll()->setGlideCurve(0.0);
        // Ctrl: from where it is pressed, off the grid.
        undo().undo();
        test::drag(window_, onCurve(0.3, 71), onCurve(1.0, 71), Qt::ControlModifier);
        QCOMPARE(b().bend[0].curve, 0.0);
        QVERIFY(std::abs(b().bend[0].time - 0.3) < 0.03);
        QVERIFY(std::abs(b().bend[0].time * 16.0 - std::round(b().bend[0].time * 16.0)) > 1e-6);
        // Nothing after G#: no slide.
        const auto before = clipNotes();
        test::drag(window_, onCurve(2.5, 68), onCurve(3.5, 68));
        QCOMPARE(clipNotes(), before);
    }

    void shiftShowsTheNotesToEditInBendMode() {
        openClip({note(60, 0.0, 1.0), note(64, 2.0, 1.0)});
        bendMode();
        QTRY_VERIFY(item("bendTools")->isVisible());
        QTest::keyPress(window_, Qt::Key_Shift, Qt::ShiftModifier);
        QVERIFY(roll()->bendMode());
        QVERIFY(!roll()->bendView());
        QTRY_VERIFY(!item("bendTools")->isVisible());
        // The notes, as out of bend mode: moved (on the grid: Shift is the view's), lengthened, added.
        test::drag(window_, onCurve(0.5, 60), onCurve(1.5, 60), Qt::ShiftModifier);
        QCOMPARE(clipNotes()[0].start, 1.0);
        QVERIFY(clipNotes()[0].bend.empty());
        const QPoint end = test::at(grid(), QPointF(roll()->view().beatToX(2.0) - 2, roll()->pitchTop(60) + 6));
        test::drag(window_, end, end + QPoint(int(roll()->view().pxPerBeat()), 0), Qt::ShiftModifier);
        QCOMPARE(clipNotes()[0].length, 2.0);
        test::doubleClick(window_, onCurve(3.5, 67), Qt::ShiftModifier);
        QCOMPARE(clipNotes().size(), size_t(3));
        // A rubber band brings up the note tools (Legato and the rest).
        test::drag(window_, onCurve(0.2, 70), onCurve(3.9, 58), Qt::ShiftModifier);
        QTRY_VERIFY(roll()->toolsShown());
        test::screenshot(window_, QStringLiteral("piano-roll-shift-view"));
        QTest::keyRelease(window_, Qt::Key_Shift);
        QVERIFY(roll()->bendView());
        QVERIFY(!roll()->toolsShown());
        QTRY_VERIFY(item("bendTools")->isVisible());
        // Not with the vibrato tool (Shift is its speed); with the glide tool, yes.
        QTest::keyClick(window_, Qt::Key_V);
        QTest::keyPress(window_, Qt::Key_Shift, Qt::ShiftModifier);
        QVERIFY(roll()->bendView());
        QTest::keyRelease(window_, Qt::Key_Shift);
        QTest::keyClick(window_, Qt::Key_V);
        QTest::keyClick(window_, Qt::Key_G);
        QTest::keyPress(window_, Qt::Key_Shift, Qt::ShiftModifier);
        QVERIFY(!roll()->bendView());
        QTest::keyRelease(window_, Qt::Key_Shift);
        QTest::keyClick(window_, Qt::Key_G);
        // Shift taken up while a curve is dragged leaves the curves (its drag goes on).
        test::press(window_, onCurve(1.5, 60));
        test::moveTo(window_, onCurve(1.6, 60, 1.0));
        QTest::keyPress(window_, Qt::Key_Shift, Qt::ShiftModifier);
        QVERIFY(roll()->bendView());
        test::moveTo(window_, onCurve(1.6, 60, 2.0), Qt::ShiftModifier);
        test::release(window_, onCurve(1.6, 60, 2.0), Qt::ShiftModifier);
        QCOMPARE(clipNotes()[0].bend.size(), size_t(1));
        QTest::keyRelease(window_, Qt::Key_Shift);
        QVERIFY(roll()->bendView());
    }

    void clearTakesTheBendsAway() {
        Note a = note(60, 0.0, 2.0), b = note(64, 2.0, 2.0);
        a.bend = {{1.0, 2.0, 0.0}};
        b.vibrato = {{0.0, 1.0, 0.5, 5.5, 0.3}};
        openClip({a, b});
        bendMode();
        test::click(window_, test::centerOf(item("clearBends")));
        for (const Note& n : clipNotes()) QVERIFY(!n.bent());
        QCOMPARE(undo().undoText(), QStringLiteral("Clear Bends"));
    }

    void theNoteToolsStayAwayInBendMode() {
        openClip({note(60, 0.0, 1.0), note(64, 1.0, 1.0)});
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        QTRY_VERIFY(roll()->toolsShown());
        bendMode();
        QVERIFY(!roll()->toolsShown());
        QTest::keyClick(window_, Qt::Key_B);
        roll()->placeTools();
        QVERIFY(roll()->toolsShown());  // (the notes are still selected)
    }
};

namespace {

const char* const kWindow = R"(
import QtQuick
import SUBstation

Window {
    width: 1100
    height: 560

    Item {
        objectName: "elsewhere"
        width: 10
        height: 10
        focus: true
    }

    ClipView {
        objectName: "clipView"
        y: 10
        width: parent.width
        height: parent.height - 10
    }
}
)";

}  // namespace

QTEST_MAIN(TestUiPianoRollBends)
#include "test_ui_pianoroll_bends.moc"
