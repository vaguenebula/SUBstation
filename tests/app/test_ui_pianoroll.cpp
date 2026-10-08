// The piano roll, driven through the clip view the way a user would: a MIDI
// track with the Synth and a one-bar clip at beat 4 opened in it; notes drawn
// and heard, dragged to move, resize and copy; the notes' keys taking
// precedence over the window's shortcuts; the rubber band, the keys and the
// velocity lane; Alt+wheel and Ctrl+Alt drags; the note tools floating by notes
// selected by dragging; the song's chords along the top, notes out of its key
// in red, and Generate writing chords and a bass line; keys dragged over, a
// rubber band's notes heard, Ctrl+D by the stretch dragged over, copy and paste
// at the clicked beat, and several clips edited together. Runs on a display (xvfb here). With
// $SUBSTATION_UI_SCREENSHOTS set, it saves screenshots there.

#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <memory>

#include "UiTestSupport.h"
#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "intelligence/Harmony.h"
#include "model/Clip.h"
#include "model/Project.h"
#include "pianoroll/ChordLane.h"
#include "pianoroll/ClipViewController.h"
#include "pianoroll/NoteGrid.h"
#include "pianoroll/PianoKeys.h"
#include "pianoroll/PianoRoll.h"
#include "pianoroll/PianoRuler.h"
#include "pianoroll/VelocityLane.h"
#include "session/Selection.h"

using sub::app::Note;
using sub::ui::NoteGrid;
using sub::ui::PianoRoll;
namespace test = sub::app::test;

namespace {

constexpr int kSamplesPerBeat = test::kSampleRate / 2;  // at 120 BPM
constexpr int kShowMs = 180;  // the note tools' fade in

// The window (at the end of the file: moc skips what follows a raw string).
extern const char* const kWindow;

Note note(int pitch, double start, double length, int velocity = 100) {
    Note n;
    n.pitch = pitch;
    n.start = start;
    n.length = length;
    n.velocity = velocity;
    return n;
}

// Notes sounding together.
std::vector<Note> chord(std::initializer_list<int> pitches, double start, double length) {
    std::vector<Note> notes;
    for (const int p : pitches) notes.push_back(note(p, start, length));
    return notes;
}

std::vector<Note> operator+(std::vector<Note> a, const std::vector<Note>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

// How red a colour is.
int redness(const QColor& c) { return c.red() - (c.green() + c.blue()) / 2; }

std::vector<Note> sortedBy(std::vector<Note> notes, bool byPitch = false) {
    std::sort(notes.begin(), notes.end(), [byPitch](const Note& a, const Note& b) {
        return byPitch ? a.pitch < b.pitch : a.start < b.start;
    });
    return notes;
}

}  // namespace

class TestUiPianoRoll : public QObject {
    Q_OBJECT

    std::unique_ptr<test::UiSession> ui_;
    QQuickWindow* window_ = nullptr;
    QQuickItem* clipView_ = nullptr;
    QString trackId_;
    QString clipId_;

    sub::app::Session& session() { return ui_->session(); }
    sub::app::Project& project() { return *session().project(); }
    QUndoStack& undo() { return *session().undoStack(); }
    sub::ui::ClipViewController* controller() {
        return clipView_->property("controller").value<sub::ui::ClipViewController*>();
    }
    NoteGrid* grid() { return clipView_->findChild<NoteGrid*>(QStringLiteral("noteGrid")); }
    PianoRoll* roll() { return grid()->roll(); }
    QQuickItem* tools() { return clipView_->findChild<QQuickItem*>(QStringLiteral("noteTools")); }
    QQuickItem* toolButton(const char* name) { return tools()->findChild<QQuickItem*>(QString::fromLatin1(name)); }
    std::vector<Note> clipNotes() { return project().clip(trackId_, clipId_).notes; }
    int shortcut(const char* name) { return ui_->root()->property(name).toInt(); }

    // Just inside the grid cell of a beat and key, in the window.
    QPoint cell(double beat, int pitch) {
        return test::at(grid(), QPointF(int(roll()->view().beatToX(beat) + 3),
                                        int(roll()->pitchTop(pitch) + roll()->rowHeight() / 2)));
    }

    // A MIDI track (with the Synth) and a one-bar clip at beat 4, open in the
    // piano roll (as by inserting a MIDI clip on its lane).
    void openMidiClip() {
        trackId_ = session().editor()->addMidiTrack();
        const auto ref = session().editor()->addMidiClip(trackId_, 4.0, 4.0);
        QVERIFY(ref);
        clipId_ = ref->clipId;
        clipView_->setProperty("trackId", trackId_);
        clipView_->setProperty("clipIds", QVariantList{clipId_});
        QTRY_VERIFY(grid()->isVisible());
    }

    void waitForTools() { QTest::qWait(kShowMs + 60); }  // let the bar's animation finish

    void clickButton(const char* name) { test::click(window_, test::centerOf(toolButton(name))); }

    QQuickItem* chordLane() { return clipView_->findChild<QQuickItem*>(QStringLiteral("chordLane")); }
    // "C 0-2, G 2-4": the chords the roll shows, in content beats.
    QString chordNames() {
        QStringList names;
        for (const PianoRoll::RollChord& c : roll()->chords())
            names.append(QStringLiteral("%1 %2-%3").arg(c.name).arg(c.start).arg(c.end));
        return names.join(QStringLiteral(", "));
    }
    // A MIDI track "Keys" playing `notes` from the start of the song.
    void keysTrack(const std::vector<Note>& notes) {
        const QString keys = session().editor()->addMidiTrack(-1, QStringLiteral("Keys"));
        const auto ref = session().editor()->addMidiClip(keys, 0.0, 8.0);
        QVERIFY(ref);
        session().editor()->setClipNotes(*ref, notes, QStringLiteral("Notes"));
    }
    // A pixel of the note grid, in the window's image.
    QColor gridPixel(const QImage& image, double beat, int pitch, double dy = 3) {
        return image.pixelColor(test::at(grid(), QPointF(roll()->view().beatToX(beat), roll()->pitchTop(pitch) + dy)));
    }
    // Generate › Chords or › Bass, through the clip view's header.
    void generate(const char* entry) {
        auto* button = clipView_->findChild<QQuickItem*>(QStringLiteral("generate"));
        QVERIFY(button && button->isVisible());
        test::click(window_, test::centerOf(button));
        QObject* menu = clipView_->findChild<QObject*>(QStringLiteral("generateMenu"));
        QTRY_VERIFY(menu->property("opened").toBool());
        auto* item = clipView_->findChild<QQuickItem*>(QString::fromLatin1(entry));
        QVERIFY(item);
        test::click(window_, test::centerOf(item));
        QTRY_VERIFY(!menu->property("visible").toBool());
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
        // The ruler's clicks play from there, as the main window's locate does.
        connect(clipView_, SIGNAL(locateRequested(double)), this, SLOT(locate(double)));
    }

    void cleanupTestCase() { ui_.reset(); }

    void init() {
        clipView_->setProperty("clipIds", QVariantList());
        project().clear();
        undo().clear();
        session().bridge()->stop();
        session().harmony()->setShown(true);
        test::moveTo(window_, QPoint(5, 5), {}, Qt::NoButton);
        QTRY_COMPARE(tools()->property("progress").toDouble(), 0.0);  // (the last test's note tools faded out)
    }

    void locate(double beat) {
        session().selection()->setInsert(beat);
        session().bridge()->locate(beat);
    }

    void midiClipOpensInThePianoRoll() {
        openMidiClip();
        const sub::app::Track& track = project().track(trackId_);
        QCOMPARE(track.isMidi(), true);
        QCOMPARE(track.devices.size(), size_t(1));
        QCOMPARE(track.devices.front().kind, QStringLiteral("synth"));
        // Just the piano roll: no audio controls.
        QVERIFY(controller()->midi());
        QVERIFY(clipView_->property("midi").toBool());
        QVERIFY(!clipView_->findChild<QQuickItem*>(QStringLiteral("audioPage"))->isVisible());
        QVERIFY(grid()->hasActiveFocus());
        QCOMPARE(clipView_->findChild<QObject*>(QStringLiteral("clipName"))->property("text").toString(), track.name);
        QVERIFY(controller()->info().contains(QStringLiteral("0 notes")));
        // Fitted to the clip: its four beats fill the width.
        QVERIFY(std::abs(roll()->view().beatToX(4.0) - grid()->width() * 0.96) <= 1.0);
        // And centred on C3, as a clip without notes is.
        const double middle = grid()->height() / 2;
        QVERIFY(roll()->pitchAt(middle) == 60 || roll()->pitchAt(middle) == 59);
        QVERIFY(!window_->grabWindow().isNull());
    }

    void notesDrawnInThePianoRollAreHeard() {
        openMidiClip();
        // A3 at the clip start, E4 two beats in.
        test::doubleClick(window_, cell(0.0, 69));
        test::doubleClick(window_, cell(2.0, 76));
        const std::vector<Note> notes = clipNotes();
        const double step = roll()->view().gridStep();
        QCOMPARE(notes, (std::vector<Note>{note(69, 0.0, step), note(76, 2.0, step)}));
        QCOMPARE(roll()->selected(), std::vector<Note>{notes[1]});  // the new note is selected
        QVERIFY(controller()->info().contains(QStringLiteral("2 notes")));
        QVERIFY(!roll()->auditioned());  // let go: no key sounding

        const std::vector<float> out = ui_->engine().renderOffline(0.0, 8 * kSamplesPerBeat);
        int first = -1;
        for (int i = 0; i < 8 * kSamplesPerBeat; ++i) {
            if (std::abs(out[size_t(2 * i)]) > 1e-6f) {
                first = i;
                break;
            }
        }
        QVERIFY2(first >= 4 * kSamplesPerBeat && first < 4 * kSamplesPerBeat + 10, qPrintable(QString::number(first)));
        float loudest = 0.0f;
        for (int i = 6 * kSamplesPerBeat; i < 6 * kSamplesPerBeat + 2000; ++i)
            loudest = std::max(loudest, std::abs(out[size_t(2 * i)]));
        QVERIFY(loudest > 0.02f);  // the second note, two beats later

        // Clicking the ruler plays from there: two beats into the clip is beat 6 of the song.
        auto* ruler = clipView_->findChild<QQuickItem*>(QStringLiteral("pianoRuler"));
        QSignalSpy located(clipView_, SIGNAL(locateRequested(double)));
        test::click(window_, test::at(ruler, QPointF(int(roll()->view().beatToX(2.0)), 10)));
        QCOMPARE(located.count(), 1);
        QCOMPARE(located.first().first().toDouble(), 6.0);
        QCOMPARE(session().bridge()->position(), 6.0);
        QVERIFY(!roll()->playhead());  // stopped: no playhead, just the start position
        QCOMPARE(roll()->startBeat(), std::optional<double>(2.0));
        QCOMPARE(undo().undoText(), QStringLiteral("Add Note"));
        undo().undo();
        QCOMPARE(clipNotes().size(), size_t(1));
        QVERIFY(roll()->selected().empty());  // the selected note is gone
    }

    void draggingMovesResizesAndCopiesNotes() {
        openMidiClip();
        test::doubleClick(window_, cell(1.0, 60));
        const std::vector<Note> notes = clipNotes();
        QCOMPARE(notes.size(), size_t(1));
        const Note original = notes.front();
        const int depth = undo().count();
        // Move: one beat later and a fifth up, as one undo step.
        const QPoint start = cell(1.0, 60) + QPoint(8, 0);
        test::drag(window_, start, start + QPoint(int(roll()->pxPerBeat()), -7 * roll()->rowHeight()));
        const Note moved = clipNotes().front();
        QCOMPARE(clipNotes().size(), size_t(1));
        QCOMPARE(moved.start, 2.0);
        QCOMPARE(moved.pitch, 67);
        QCOMPARE(moved.length, original.length);
        QCOMPARE(undo().count(), depth + 1);
        QCOMPARE(undo().undoText(), QStringLiteral("Move Notes"));
        // Resize the end by one beat.
        const QRectF rect = roll()->noteRect(moved);
        const QPointF edge(int(rect.right()) - 2, int(rect.center().y()));
        QVERIFY(grid()->noteAt(edge) && grid()->noteAt(edge)->zone == NoteGrid::Zone::End);
        test::drag(window_, test::at(grid(), edge), test::at(grid(), edge + QPointF(int(roll()->pxPerBeat()), 0)));
        const Note resized = clipNotes().front();
        QVERIFY(std::abs(resized.length - (original.length + 1.0)) < 1e-9);
        QCOMPARE(undo().undoText(), QStringLiteral("Resize Notes"));
        // Ctrl-drag copies (the selected note: Ctrl-clicking it would deselect it).
        const QPoint body = test::at(grid(), QPointF(int(rect.center().x()), int(rect.center().y())));
        test::drag(window_, body, body + QPoint(0, 2 * roll()->rowHeight()), Qt::ControlModifier);
        std::vector<int> pitches;
        for (const Note& n : clipNotes()) pitches.push_back(n.pitch);
        QCOMPARE(pitches, (std::vector<int>{65, 67}));
        QCOMPARE(roll()->selected().size(), size_t(1));
        QCOMPARE(roll()->selected().front().pitch, 65);  // the copy
        QCOMPARE(undo().undoText(), QStringLiteral("Copy Notes"));
        undo().undo();
        undo().undo();
        undo().undo();
        QCOMPARE(clipNotes(), std::vector<Note>{original});
    }

    void dragsBelowTheThresholdChangeNothing() {
        openMidiClip();
        test::doubleClick(window_, cell(1.0, 60));
        const std::vector<Note> notes = clipNotes();
        const int depth = undo().count();
        const QPoint body = cell(1.0, 60) + QPoint(8, 0);
        test::drag(window_, body, body + QPoint(1, 1));  // under DRAG_THRESHOLD (3 px)
        QCOMPARE(clipNotes(), notes);
        QCOMPARE(undo().count(), depth);
        // A click on one of several selected notes selects just it.
        test::doubleClick(window_, cell(2.0, 62));
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(roll()->selected().size(), size_t(2));
        test::click(window_, body);
        QCOMPARE(roll()->selected(), std::vector<Note>{clipNotes().front()});
        // Double-clicking a note deletes it.
        test::doubleClick(window_, body);
        QCOMPARE(clipNotes().size(), size_t(1));
        QCOMPARE(undo().undoText(), QStringLiteral("Delete Note"));
        QVERIFY(roll()->selected().empty());
    }

    void pianoRollKeysTakePrecedenceOverWindowShortcuts() {
        openMidiClip();
        for (double beat : {0.0, 1.0}) test::doubleClick(window_, cell(beat, 60));
        session().selection()->selectClips(*session().editor(), {{trackId_, clipId_}});  // the clip is selected too
        grid()->forceActiveFocus();
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        QCOMPARE(roll()->selected().size(), size_t(2));
        QTest::keyClick(window_, Qt::Key_Up, Qt::ShiftModifier);  // an octave up
        for (const Note& n : clipNotes()) QCOMPARE(n.pitch, 72);
        QCOMPARE(undo().undoText(), QStringLiteral("Transpose Notes"));
        QTest::keyClick(window_, Qt::Key_D, Qt::ControlModifier);  // duplicate after them
        std::vector<double> starts;
        for (const Note& n : clipNotes()) starts.push_back(n.start);
        QCOMPARE(starts, (std::vector<double>{0.0, 1.0, 1.25, 2.25}));
        QCOMPARE(undo().undoText(), QStringLiteral("Duplicate Notes"));
        QTest::keyClick(window_, Qt::Key_Right);  // a grid step later
        QCOMPARE(sortedBy(clipNotes()).back().start, 2.25 + roll()->view().gridStep());
        QCOMPARE(undo().undoText(), QStringLiteral("Move Notes"));
        QTest::keyClick(window_, Qt::Key_Delete);  // deletes the selected notes, not the clip
        QCOMPARE(clipNotes().size(), size_t(2));
        QCOMPARE(project().track(trackId_).clips.size(), size_t(1));
        QTest::keyClick(window_, Qt::Key_U, Qt::ControlModifier);  // quantizes every note
        // 0: the selected notes deactivated (not the clip); again, activated.
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(window_, Qt::Key_0);
        for (const Note& n : clipNotes()) QVERIFY(n.muted);
        QCOMPARE(roll()->selected().size(), size_t(2));  // (still selected, as they are now)
        QCOMPARE(undo().undoText(), QStringLiteral("Deactivate Notes"));
        QVERIFY(!project().track(trackId_).clips[0].muted);
        QTest::keyClick(window_, Qt::Key_0);
        for (const Note& n : clipNotes()) QVERIFY(!n.muted);
        QCOMPARE(undo().undoText(), QStringLiteral("Activate Notes"));
        // None of the window's shortcuts fired.
        QCOMPARE(shortcut("deletes"), 0);
        QCOMPARE(shortcut("selectAlls"), 0);
        QCOMPARE(shortcut("duplicates"), 0);
        QCOMPARE(shortcut("quantizes"), 0);
        QCOMPARE(shortcut("deactivates"), 0);
        // (They do away from the notes.)
        auto* elsewhere = window_->findChild<QQuickItem*>(QStringLiteral("elsewhere"));
        elsewhere->forceActiveFocus();
        QTest::keyClick(window_, Qt::Key_Delete);
        QCOMPARE(shortcut("deletes"), 1);
        QCOMPARE(clipNotes().size(), size_t(2));
        // Esc goes back.
        grid()->forceActiveFocus();
        QSignalSpy closing(clipView_, SIGNAL(closeRequested()));
        QTest::keyClick(window_, Qt::Key_Escape);
        QCOMPARE(closing.count(), 1);
    }

    void rubberBandKeysAndVelocityLane() {
        openMidiClip();
        const std::pair<double, int> placed[] = {{0.0, 60}, {1.0, 62}, {2.0, 64}};
        for (const auto& [beat, pitch] : placed) test::doubleClick(window_, cell(beat, pitch));
        const std::vector<Note> notes = clipNotes();
        // Rubber band around the first two.
        test::drag(window_, cell(0.0, 63) - QPoint(2, 0), cell(1.5, 59));
        QCOMPARE(roll()->selected(), (std::vector<Note>{notes[0], notes[1]}));
        // Ctrl-clicking a selected note deselects it.
        test::click(window_, cell(0.0, 60), Qt::ControlModifier);
        QCOMPARE(roll()->selected(), std::vector<Note>{notes[1]});
        // Clicking a key selects the notes on it (and plays it while held).
        auto* keys = clipView_->findChild<QQuickItem*>(QStringLiteral("pianoKeys"));
        const QPoint key = test::at(keys, QPointF(10, int(roll()->pitchTop(64) + 4)));
        test::press(window_, key);
        QCOMPARE(roll()->auditioned(), std::optional<int>(64));
        test::release(window_, key);
        QVERIFY(!roll()->auditioned());
        QCOMPARE(roll()->selected(), std::vector<Note>{notes[2]});
        // Shift adds.
        test::click(window_, test::at(keys, QPointF(10, int(roll()->pitchTop(60) + 4))), Qt::ShiftModifier);
        QCOMPARE(roll()->selected(), (std::vector<Note>{notes[0], notes[2]}));
        test::click(window_, test::at(keys, QPointF(10, int(roll()->pitchTop(64) + 4))));
        // Dragging a stem down changes the velocity (of the selected notes, when it is one of theirs).
        auto* lane = clipView_->findChild<sub::ui::VelocityLane*>(QStringLiteral("velocityLane"));
        const QPointF top(int(roll()->view().beatToX(2.0)), int(lane->velocityY(100)));
        QVERIFY(lane->stemAt(top.x()));
        test::drag(window_, test::at(lane, top), test::at(lane, top + QPointF(0, int(lane->span() / 2))));
        const std::vector<Note> after = clipNotes();
        QCOMPARE(after[0].velocity, 100);
        QCOMPARE(after[1].velocity, 100);
        QVERIFY2(std::abs(after[2].velocity - 37) <= 2, qPrintable(QString::number(after[2].velocity)));
        QCOMPARE(undo().undoText(), QStringLiteral("Change Velocity"));
        // Several selected notes change together, by the same amount, as one step.
        const int depth = undo().count();
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        const QPointF stem(int(roll()->view().beatToX(1.0)), int(lane->velocityY(100)));
        test::drag(window_, test::at(lane, stem), test::at(lane, stem + QPointF(0, 10)));
        const std::vector<Note> together = clipNotes();
        const int delta = together[1].velocity - 100;
        QVERIFY(delta < 0);
        QCOMPARE(together[0].velocity, 100 + delta);
        QCOMPARE(together[2].velocity, after[2].velocity + delta);
        QCOMPARE(undo().count(), depth + 1);
    }

    void draggingOverTheKeysSelectsTheirNotes() {
        openMidiClip();
        roll()->commit({note(60, 0, 1), note(62, 1, 1), note(64, 2, 1), note(67, 3, 1)}, QStringLiteral("Notes"));
        auto* keys = clipView_->findChild<QQuickItem*>(QStringLiteral("pianoKeys"));
        const auto key = [&](int pitch) { return test::at(keys, QPointF(10, int(roll()->pitchTop(pitch) + 4))); };
        // From C3 down... up to E3: the notes on every key between, each key heard in turn.
        test::press(window_, key(60));
        test::moveTo(window_, key(62));
        test::moveTo(window_, key(64));
        QCOMPARE(roll()->auditioned(), std::optional<int>(64));
        test::release(window_, key(64));
        QCOMPARE(roll()->selected(), (std::vector<Note>{note(60, 0, 1), note(62, 1, 1), note(64, 2, 1)}));
        // The other way, from G3 down to E3, with Shift: added to them.
        test::press(window_, key(67), Qt::ShiftModifier);
        test::moveTo(window_, key(64), Qt::ShiftModifier);
        test::release(window_, key(64), Qt::ShiftModifier);
        QCOMPARE(roll()->selectedCount(), 4);
        // A press alone: that key's notes.
        test::click(window_, key(62));
        QCOMPARE(roll()->selected(), std::vector<Note>{note(62, 1, 1)});
    }

    void aRubberBandPlaysTheNotesItSelects() {
        openMidiClip();
        roll()->commit(chord({60, 64, 67}, 0, 1) + std::vector<Note>{note(72, 2, 1)}, QStringLiteral("Notes"));
        const auto sounding = [this] {
            std::vector<int> pitches;
            for (const auto& [track, pitch] : roll()->previewing()) {
                if (track == trackId_) pitches.push_back(pitch);
            }
            std::sort(pitches.begin(), pitches.end());
            return pitches;
        };
        // The band reaches the chord: its three keys sound together at once, before it is let go.
        test::press(window_, cell(0.0, 74) - QPoint(2, 0));
        test::moveTo(window_, cell(0.75, 59));
        QCOMPARE(roll()->selectedCount(), 3);
        QCOMPARE(sounding(), (std::vector<int>{60, 64, 67}));
        // On to the next note: it sounds too, and the chord's keys aren't struck again.
        test::moveTo(window_, cell(2.5, 59));
        QCOMPARE(roll()->selectedCount(), 4);
        const std::vector<int> now = sounding();
        QVERIFY(std::count(now.begin(), now.end(), 72) == 1);
        QVERIFY(std::adjacent_find(now.begin(), now.end()) == now.end());
        test::release(window_, cell(2.5, 59));
        QTRY_VERIFY_WITH_TIMEOUT(roll()->previewing().empty(), PianoRoll::kChordPreviewMs + 1000);
        // Clicking a note plays just it, while held; a rubber band over nothing plays nothing.
        test::press(window_, cell(2.0, 72));
        QVERIFY(roll()->previewing().empty());
        QCOMPARE(roll()->auditioned(), std::optional<int>(72));
        test::release(window_, cell(2.0, 72));
        test::drag(window_, cell(3.0, 63), cell(3.5, 61));
        QVERIFY(roll()->previewing().empty());
        // Deactivated notes stay silent.
        roll()->setSelection(chord({60, 64, 67}, 0, 1));
        QTest::keyClick(window_, Qt::Key_0);
        test::drag(window_, cell(0.0, 69) - QPoint(2, 0), cell(0.75, 59));
        QCOMPARE(roll()->selectedCount(), 3);
        QVERIFY(roll()->previewing().empty());
    }

    void ctrlDDuplicatesTheStretchARubberBandSelected() {
        openMidiClip();
        QVERIFY(roll()->view().snap());
        // A rest, then two notes: dragged over from the bar's start, they repeat with the rest.
        roll()->commit({note(60, 0.5, 0.5), note(62, 1.5, 0.5)}, QStringLiteral("Notes"));
        const double y = roll()->pitchTop(64);
        test::drag(window_, test::at(grid(), QPointF(roll()->view().beatToX(0.0) + 2, y)),
                   test::at(grid(), QPointF(roll()->view().beatToX(2.0) - 2, roll()->pitchTop(58))));
        QCOMPARE(roll()->selectedCount(), 2);
        QCOMPARE(roll()->selectedSpan(), std::optional<PianoRoll::Span>(PianoRoll::Span{0.0, 2.0}));
        QTest::keyClick(window_, Qt::Key_D, Qt::ControlModifier);
        std::vector<double> starts;
        for (const Note& n : sortedBy(clipNotes())) starts.push_back(n.start);
        QCOMPARE(starts, (std::vector<double>{0.5, 1.5, 2.5, 3.5}));
        QCOMPARE(roll()->selected(), (std::vector<Note>{note(60, 2.5, 0.5), note(62, 3.5, 0.5)}));
        // Again: on after the stretch duplicated.
        QCOMPARE(roll()->selectedSpan(), std::optional<PianoRoll::Span>(PianoRoll::Span{2.0, 4.0}));
        QTest::keyClick(window_, Qt::Key_D, Qt::ControlModifier);
        QCOMPARE(sortedBy(clipNotes()).back().start, 5.5);
        QCOMPARE(shortcut("duplicates"), 0);
        // Notes chosen without a rubber band: right after the last of them, as before.
        undo().undo();
        undo().undo();
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        QVERIFY(!roll()->selectedSpan());
        QTest::keyClick(window_, Qt::Key_D, Qt::ControlModifier);
        starts.clear();
        for (const Note& n : sortedBy(clipNotes())) starts.push_back(n.start);
        QCOMPARE(starts, (std::vector<double>{0.5, 1.5, 2.0, 3.0}));
    }

    void copiedNotesPasteWhereTheGridWasClicked() {
        openMidiClip();
        roll()->commit(chord({60, 64}, 0, 1) + std::vector<Note>{note(67, 1, 0.5)}, QStringLiteral("Notes"));
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        QTest::keyClick(window_, Qt::Key_C, Qt::ControlModifier);
        QVERIFY(roll()->hasCopiedNotes());
        // A click on an empty part of the grid places the paste marker; playback still starts where it did.
        locate(5.0);  // (the start marker one beat into the clip)
        test::click(window_, cell(2.0, 55));
        QCOMPARE(roll()->pasteBeat(), std::optional<double>(2.0));
        QCOMPARE(session().selection()->insertBeat(), 5.0);
        QCOMPARE(session().bridge()->position(), 5.0);
        QCOMPARE(roll()->startBeat(), std::optional<double>(1.0));
        test::screenshot(window_, QStringLiteral("piano-roll-paste-marker"));
        QVERIFY(roll()->selected().empty());
        QTest::keyClick(window_, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(undo().undoText(), QStringLiteral("Paste Notes"));
        const std::vector<Note> pasted{note(60, 2, 1), note(64, 2, 1), note(67, 3, 0.5)};
        QCOMPARE(roll()->selected(), pasted);
        QCOMPARE(clipNotes().size(), size_t(6));
        // The paste marker went to their end: pasting again appends.
        QCOMPARE(roll()->pasteBeat(), std::optional<double>(3.5));
        QTest::keyClick(window_, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(sortedBy(clipNotes()).back().start, 4.5);
        // Cut: copied, and taken out; pasted back where clicked.
        roll()->setSelection({note(67, 1, 0.5)});
        QTest::keyClick(window_, Qt::Key_X, Qt::ControlModifier);
        QCOMPARE(undo().undoText(), QStringLiteral("Cut Note"));
        QCOMPARE(clipNotes().size(), size_t(8));
        test::click(window_, cell(0.5, 55));
        QTest::keyClick(window_, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(roll()->selected(), std::vector<Note>{note(67, 0.5, 0.5)});
        // The window's Copy, Cut and Paste never fired.
        QCOMPARE(shortcut("copies"), 0);
        QCOMPARE(shortcut("cuts"), 0);
        QCOMPARE(shortcut("pastes"), 0);
    }

    void severalMidiClipsAreEditedTogether() {
        openMidiClip();  // A: beats 4-8
        sub::app::ProjectEditor& editor = *session().editor();
        const auto later = editor.addMidiClip(trackId_, 12.0, 4.0);  // A again: 12-16
        const QString other = editor.addMidiTrack();
        const auto between = editor.addMidiClip(other, 8.0, 4.0);  // B: 8-12
        QVERIFY(later && between);
        editor.setClipNotes({trackId_, clipId_}, {note(60, 0, 1)}, QStringLiteral("Notes"));
        editor.setClipNotes(*between, {note(64, 0, 1)}, QStringLiteral("Notes"));
        const auto entry = [](const sub::app::ClipRef& ref) {
            return QVariantMap{{QStringLiteral("trackId"), ref.trackId}, {QStringLiteral("clipId"), ref.clipId}};
        };
        clipView_->setProperty("clipIds", QVariantList{entry(*between), entry(*later), entry({trackId_, clipId_})});
        clipView_->setProperty("leadClipId", clipId_);
        QTRY_COMPARE(roll()->clipCount(), 3);
        QVERIFY(controller()->midi());
        QCOMPARE(controller()->refs().front(), (sub::app::ClipRef{trackId_, clipId_}));  // the lead first
        QCOMPARE(controller()->name(), QStringLiteral("3 MIDI Clips"));
        // In the song's beats: each clip where it is.
        QCOMPARE(roll()->origin(), 0.0);
        QCOMPARE(roll()->shift(0), 4.0);
        const auto local = [&](double beat, int pitch) {
            return QPointF(roll()->view().beatToX(beat) + 3, roll()->pitchTop(pitch) + 4);
        };
        QVERIFY(grid()->noteAt(local(4.0, 60)) && grid()->noteAt(local(8.0, 64)));
        QCOMPARE(grid()->noteAt(local(8.0, 64))->note.clip, 2);  // (B's clip: the lead, then the others in order)
        // A rubber band over both clips' notes, moved up a semitone: each in its own clip, one undo step.
        test::drag(window_, cell(4.0, 66), cell(8.5, 58));
        QCOMPARE(roll()->selectedCount(), 2);
        const int depth = undo().count();
        QTest::keyClick(window_, Qt::Key_Up);
        QCOMPARE(project().clip(trackId_, clipId_).notes, std::vector<Note>{note(61, 0, 1)});
        QCOMPARE(project().clip(other, between->clipId).notes, std::vector<Note>{note(65, 0, 1)});
        QCOMPARE(undo().count(), depth + 1);
        // A new note goes into the clip playing where it is drawn.
        test::doubleClick(window_, cell(13.0, 70));
        QCOMPARE(project().clip(trackId_, later->clipId).notes.size(), size_t(1));
        QCOMPARE(project().clip(trackId_, later->clipId).notes.front().start, 1.0);
        test::doubleClick(window_, cell(9.0, 70));
        QCOMPARE(project().clip(other, between->clipId).notes.size(), size_t(2));
        // Copied notes paste into the clip of their track that plays there.
        roll()->selectNotes({{0, note(61, 0, 1)}});
        QTest::keyClick(window_, Qt::Key_C, Qt::ControlModifier);
        test::click(window_, cell(14.0, 56));
        QTest::keyClick(window_, Qt::Key_V, Qt::ControlModifier);
        QCOMPARE(project().clip(trackId_, later->clipId).notes.size(), size_t(2));
        undo().undo();
        undo().undo();
        undo().undo();
        undo().undo();
        QCOMPARE(project().clip(trackId_, clipId_).notes, std::vector<Note>{note(60, 0, 1)});
        QCOMPARE(project().clip(other, between->clipId).notes, std::vector<Note>{note(64, 0, 1)});
        test::screenshot(window_, QStringLiteral("piano-roll-several-clips"));
    }

    void altWheelResizesTheKeysAndCtrlAltDragScrolls() {
        openMidiClip();
        const QPointF at(int(roll()->view().beatToX(1.0) + 3), int(roll()->pitchTop(64) + roll()->rowHeight() / 2));
        const double scrollBeats = roll()->scrollBeats();
        const int height = roll()->rowHeight();
        // Alt+wheel over the grid (or the keys) makes the rows taller, keeping the key under the mouse there.
        test::wheel(window_, test::at(grid(), at), 4, Qt::AltModifier);
        QVERIFY(roll()->rowHeight() > height);
        QCOMPARE(roll()->scrollBeats(), scrollBeats);
        QCOMPARE(roll()->pitchAt(at.y()), 64);
        const int taller = roll()->rowHeight();
        auto* keys = clipView_->findChild<QQuickItem*>(QStringLiteral("pianoKeys"));
        test::wheel(window_, test::at(keys, QPointF(10, at.y())), -8, Qt::AltModifier);
        QVERIFY(roll()->rowHeight() < taller);
        QCOMPARE(roll()->pitchAt(at.y()), 64);
        // (Qt may report Alt+wheel as horizontal.)
        const int before = roll()->rowHeight();
        test::wheel(window_, test::at(grid(), at), 2, Qt::AltModifier, true);
        QVERIFY(roll()->rowHeight() > before);
        for (int i = 0; i < 50; ++i) test::wheel(window_, test::at(grid(), at), -1, Qt::AltModifier);
        QCOMPARE(roll()->rowHeight(), 5);  // no shorter than this
        // The wheel scrolls three rows a notch; Shift sideways; Ctrl zooms time around the mouse.
        roll()->setScrollY(200);
        test::wheel(window_, test::at(grid(), at), -1);
        QCOMPARE(roll()->scrollY(), 200 + 3 * 5);
        const double beats = roll()->scrollBeats();
        test::wheel(window_, test::at(grid(), at), -1, Qt::ShiftModifier);
        QVERIFY(std::abs(roll()->scrollBeats() - (beats + 80 / roll()->pxPerBeat())) < 1e-9);
        const double ppb = roll()->pxPerBeat();
        const double under = roll()->view().xToBeat(at.x());
        test::wheel(window_, test::at(grid(), at), 1, Qt::ControlModifier);
        QVERIFY(std::abs(roll()->pxPerBeat() - ppb * 1.2) < 1e-9);
        QVERIFY(std::abs(roll()->view().xToBeat(at.x()) - under) < 1e-9);
        // Ctrl+Alt drag scrolls both ways, and draws no note or rubber band.
        const Qt::KeyboardModifiers ctrlAlt = Qt::ControlModifier | Qt::AltModifier;
        const QPoint start = cell(2.0, roll()->pitchAt(grid()->height() / 2));
        const double beat = roll()->scrollBeats();
        const int y = roll()->scrollY();
        test::drag(window_, start, start + QPoint(-40, 30), ctrlAlt);
        QVERIFY(std::abs(roll()->scrollBeats() - (beat + 40 / roll()->pxPerBeat())) < 1e-9);
        QVERIFY(roll()->scrollY() > 0);
        QCOMPARE(roll()->scrollY(), y - 30);
        QVERIFY(clipNotes().empty());
        QVERIFY(roll()->selected().empty());
        // The ruler: dragged sideways it scrolls, up and down it zooms.
        const double position = session().bridge()->position();
        auto* ruler = clipView_->findChild<QQuickItem*>(QStringLiteral("pianoRuler"));
        const double scrolled = roll()->scrollBeats();
        test::drag(window_, test::at(ruler, QPointF(300, 10)), test::at(ruler, QPointF(250, 11)));
        QVERIFY(std::abs(roll()->scrollBeats() - (scrolled + 50 / roll()->pxPerBeat())) < 1e-9);
        const double zoom = roll()->pxPerBeat();
        test::drag(window_, test::at(ruler, QPointF(300, 5)), test::at(ruler, QPointF(301, 20)));
        QVERIFY(roll()->pxPerBeat() > zoom);
        QCOMPARE(session().bridge()->position(), position);  // neither drag played from there
        // The scroll bars: dragging the vertical one's handle scrolls the rows, the
        // horizontal one's time.
        auto* vbar = clipView_->findChild<QQuickItem*>(QStringLiteral("vbar"));
        roll()->setScrollY(0);
        const QPoint handle = test::at(vbar, QPointF(vbar->width() / 2, 10));
        test::drag(window_, handle, handle + QPoint(0, 40));
        QVERIFY(roll()->scrollY() > 0);
        const double perPixel = roll()->vScrollTotal() / vbar->height();
        QVERIFY(std::abs(roll()->scrollY() - 40 * perPixel) <= perPixel + 1);
        auto* hbar = clipView_->findChild<QQuickItem*>(QStringLiteral("hbar"));
        roll()->setScrollBeats(0);
        const QPoint thumb = test::at(hbar, QPointF(10, hbar->height() / 2));
        test::drag(window_, thumb, thumb + QPoint(30, 0));
        QVERIFY(roll()->scrollBeats() > 0);
        QCOMPARE(roll()->hScrollValue(), std::trunc(roll()->scrollBeats() * roll()->pxPerBeat()));
    }

    void noteToolsFloatByNotesSelectedByDragging() {
        openMidiClip();
        QQuickItem* bar = tools();
        const auto shown = [bar] { return bar->property("shown").toBool(); };
        const auto count = [bar] {
            return bar->findChild<QObject*>(QStringLiteral("noteCount"))->property("text").toString();
        };
        const sub::app::ClipRef ref{trackId_, clipId_};
        const std::vector<Note> played{note(60, 0.1, 0.25), note(64, 1.05, 0.25), note(67, 1.9, 0.25),
                                       note(72, 3.2, 0.25)};
        session().editor()->setClipNotes(ref, played, QStringLiteral("setup"));
        QVERIFY(!bar->isVisible());

        // Clicking a note, or drawing one, selects it without bringing up the tools.
        test::click(window_, cell(1.1, 64));
        QCOMPARE(roll()->selected(), std::vector<Note>{clipNotes()[1]});
        QVERIFY(!shown());
        test::doubleClick(window_, cell(3.0, 50));
        QCOMPARE(roll()->selected().size(), size_t(1));
        QVERIFY(!shown());
        undo().undo();

        // A rubber band brings them up (fading in as they rise), just above the notes it caught.
        test::drag(window_, cell(0.0, 66) - QPoint(2, 0), cell(1.5, 59));
        QCOMPARE(roll()->selected(), (std::vector<Note>{clipNotes()[0], clipNotes()[1]}));
        QVERIFY(shown());
        QCOMPARE(count(), QStringLiteral("2 notes"));
        waitForTools();
        QVERIFY(bar->isVisible());
        QTRY_COMPARE(bar->opacity(), 1.0);  // (on a slow machine the fade may still be ending)
        QVERIFY(bar->y() + bar->height() - 1 < roll()->noteRect(clipNotes()[1]).top());
        test::screenshot(window_, QStringLiteral("piano-roll-tools"));
        // They hide while the group is dragged, and come back where it ends up.
        const QPoint grab = cell(1.1, 64);
        test::press(window_, grab);
        test::moveTo(window_, grab + QPoint(0, roll()->rowHeight() * 2));
        QVERIFY(!shown());
        test::release(window_, grab + QPoint(0, roll()->rowHeight() * 2));
        std::vector<int> pitches;
        for (const Note& n : roll()->selected()) pitches.push_back(n.pitch);
        std::sort(pitches.begin(), pitches.end());
        QCOMPARE(pitches, (std::vector<int>{58, 62}));
        QVERIFY(shown());
        undo().undo();
        // Clicking one note of the group (without dragging it) selects just that note; the tools go.
        test::drag(window_, cell(0.0, 66) - QPoint(2, 0), cell(1.5, 59));
        QCOMPARE(roll()->selected().size(), size_t(2));
        QVERIFY(shown());
        test::click(window_, cell(1.1, 64));
        QCOMPARE(roll()->selected(), std::vector<Note>{clipNotes()[1]});
        QVERIFY(!shown());
        // Clicking empty space deselects, and the tools fade away.
        test::click(window_, cell(3.0, 50));
        QVERIFY(roll()->selected().empty());
        QVERIFY(!shown());
        waitForTools();
        QVERIFY(!bar->isVisible());

        // Ctrl+A brings them up too, inside the grid.
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        QVERIFY(shown());
        QCOMPARE(count(), QStringLiteral("4 notes"));
        waitForTools();
        QVERIFY(bar->x() >= 0 && bar->y() >= 0 && bar->x() + bar->width() <= grid()->width() &&
                bar->y() + bar->height() <= grid()->height());
        // Quantize (to 1/16 by default) moves the selected notes, as one undo step, and the tools stay.
        const int depth = undo().index();
        clickButton("quantize");
        std::vector<double> starts;
        for (const Note& n : clipNotes()) starts.push_back(n.start);
        QCOMPARE(starts, (std::vector<double>{0.0, 1.0, 2.0, 3.25}));
        QCOMPARE(undo().index(), depth + 1);
        QCOMPARE(undo().undoText(), QStringLiteral("Quantize"));
        QCOMPARE(roll()->selected(), clipNotes());
        QVERIFY(shown());
        QVERIFY(grid()->hasActiveFocus());  // the buttons never take it
        undo().undo();
        // With nothing selected, Ctrl+U quantizes every note, here to 1/4 at half strength.
        roll()->setSelection({});
        auto* grids = bar->findChild<QObject*>(QStringLiteral("quantizeGrid"));
        QCOMPARE(grids->property("currentText").toString(), QStringLiteral("1/16"));
        roll()->setQuantizeGrid(QStringLiteral("1/4"));
        roll()->setQuantizeAmount(50.0);
        QCOMPARE(grids->property("currentText").toString(), QStringLiteral("1/4"));
        QCOMPARE(bar->findChild<QObject*>(QStringLiteral("quantizeAmount"))->property("value").toDouble(), 50.0);
        QTest::keyClick(window_, Qt::Key_U, Qt::ControlModifier);
        const double expected[] = {0.05, 1.025, 1.95, 3.1};
        for (size_t i = 0; i < 4; ++i) QVERIFY(std::abs(clipNotes()[i].start - expected[i]) < 1e-9);
        undo().undo();

        // Legato acts on the selected notes: each reaches the next, the last the next
        // note after it (unselected here), not the clip's end.
        roll()->setSelection({clipNotes()[0], clipNotes()[1], clipNotes()[2]});
        QMetaObject::invokeMethod(toolButton("legato"), "clicked");
        const std::vector<Note> legato = clipNotes();
        const double lengths[] = {0.95, 0.85, 1.3, 0.25};
        for (size_t i = 0; i < 4; ++i) {
            QCOMPARE(legato[i].start, played[i].start);
            QVERIFY(std::abs(legato[i].length - lengths[i]) < 1e-9);
        }
        QCOMPARE(roll()->selected(), (std::vector<Note>{legato[0], legato[1], legato[2]}));  // still selected
        QCOMPARE(undo().undoText(), QStringLiteral("Legato"));
        undo().undo();

        // ×2 and ÷2 scale the selected notes' timing from the first one, each one undo step.
        roll()->setSelection({clipNotes()[0], clipNotes()[1]});
        QMetaObject::invokeMethod(toolButton("doubleTime"), "clicked");
        QCOMPARE(undo().undoText(), QStringLiteral("Timing ×2"));
        const std::vector<Note> doubled = sortedBy(roll()->selected());
        QCOMPARE(doubled[0].start, 0.1);
        QVERIFY(std::abs(doubled[0].length - 0.5) < 1e-9);
        QVERIFY(std::abs(doubled[1].start - 2.0) < 1e-9);
        QVERIFY(std::abs(doubled[1].length - 0.5) < 1e-9);
        QMetaObject::invokeMethod(toolButton("halfTime"), "clicked");
        QCOMPARE(undo().undoText(), QStringLiteral("Timing ÷2"));
        const std::vector<Note> halved = sortedBy(roll()->selected());
        for (size_t i = 0; i < 2; ++i) {
            QVERIFY(std::abs(halved[i].start - played[i].start) < 1e-9);
            QVERIFY(std::abs(halved[i].length - played[i].length) < 1e-9);
        }
        undo().undo();
        undo().undo();

        // Humanize is a menu: Timing moves starts a little, lengths and velocities stay.
        roll()->setSelection(clipNotes());
        roll()->seedRandom(3);
        QMetaObject::invokeMethod(toolButton("humanize"), "clicked");
        QObject* menu = bar->findChild<QObject*>(QStringLiteral("humanizeMenu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        QObject* timing = menu->findChild<QObject*>(QStringLiteral("humanizeTiming"));
        QObject* velocity = menu->findChild<QObject*>(QStringLiteral("humanizeVelocity"));
        QVERIFY(timing && velocity);
        QVERIFY(velocity->property("enabled").toBool());  // (the build puts the model next to the tests)
        // Each has its own amount.
        QCOMPARE(roll()->humanizeVelocityAmount(), PianoRoll::kDefaultHumanizeVelocity);
        QCOMPARE(roll()->humanizeTimingAmount(), PianoRoll::kDefaultHumanizeTiming);
        roll()->setHumanizeTimingAmount(100.0);
        QCOMPARE(timing->findChild<QObject*>(QStringLiteral("humanizeTimingAmount"))->property("value").toDouble(), 100.0);
        QCOMPARE(velocity->findChild<QObject*>(QStringLiteral("humanizeVelocityAmount"))->property("value").toDouble(),
                 PianoRoll::kDefaultHumanizeVelocity);
        const std::vector<Note> before = sortedBy(clipNotes(), true);
        QMetaObject::invokeMethod(timing, "triggered");
        const std::vector<Note> after = sortedBy(clipNotes(), true);
        QVERIFY(after != before);
        for (size_t i = 0; i < before.size(); ++i) {
            QVERIFY(std::abs(after[i].start - before[i].start) <= 0.125 + 1e-9);
            QCOMPARE(after[i].velocity, before[i].velocity);
            QCOMPARE(after[i].length, before[i].length);
        }
        QCOMPARE(undo().undoText(), QStringLiteral("Humanize Timing"));
        QTRY_VERIFY(!menu->property("visible").toBool());
        QTRY_VERIFY(grid()->hasActiveFocus());  // (the notes take the keyboard back from the menu)
        undo().undo();
        // Velocity gives the model's velocities, at the notes' own level; timing stays.
        QMetaObject::invokeMethod(toolButton("humanize"), "clicked");
        QMetaObject::invokeMethod(velocity, "triggered");
        QCOMPARE(undo().undoText(), QStringLiteral("Humanize Velocity"));
        const std::vector<Note> shaped = sortedBy(clipNotes(), true);
        QCOMPARE(shaped.size(), before.size());
        for (size_t i = 0; i < before.size(); ++i) {
            QCOMPARE(shaped[i].start, before[i].start);
            QCOMPARE(shaped[i].length, before[i].length);
            QVERIFY(shaped[i].velocity >= 1 && shaped[i].velocity <= 127);
        }
        QVERIFY(!window_->grabWindow().isNull());
        roll()->setHumanizeTimingAmount(PianoRoll::kDefaultHumanizeTiming);
    }

    void toolsClickNeverReachesTheNotes() {
        openMidiClip();
        const std::vector<Note> played{note(60, 0.5, 0.5), note(62, 1.0, 0.5)};
        session().editor()->setClipNotes({trackId_, clipId_}, played, QStringLiteral("setup"));
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        waitForTools();
        // A click on the bar's background (between its buttons) stays on the bar.
        QQuickItem* bar = tools();
        test::click(window_, test::at(bar, QPointF(3, bar->height() / 2)));
        QCOMPARE(roll()->selected().size(), size_t(2));
        QVERIFY(bar->property("shown").toBool());
        // The headphones button turns hearing notes off.
        auto* preview = clipView_->findChild<QQuickItem*>(QStringLiteral("preview"));
        test::click(window_, test::centerOf(preview));
        QVERIFY(!roll()->preview());
        test::press(window_, cell(0.6, 60));
        QVERIFY(!roll()->auditioned());
        test::release(window_, cell(0.6, 60));
        test::click(window_, test::centerOf(preview));
        QVERIFY(roll()->preview());
        QVERIFY(grid()->hasActiveFocus());
    }

    void notesOutsideTheClipAreKeptAndTheViewFollowsUndo() {
        openMidiClip();
        // A note past the clip's end is kept (trimming hides notes, never deletes them).
        const std::vector<Note> notes{note(60, 0.0, 1.0), note(62, 5.0, 1.0)};
        session().editor()->setClipNotes({trackId_, clipId_}, notes, QStringLiteral("setup"));
        QVERIFY(controller()->info().contains(QStringLiteral("1 note")));
        QVERIFY(!controller()->info().contains(QStringLiteral("notes")));
        test::screenshot(window_, QStringLiteral("piano-roll-outside"));
        // Deleting the clip closes the view.
        QSignalSpy closing(clipView_, SIGNAL(closeRequested()));
        session().editor()->deleteClips({{trackId_, clipId_}});
        QCOMPARE(closing.count(), 1);
        QVERIFY(!roll()->hasClip());
        undo().undo();
    }

    void cursorsSayWhatADragWouldDo() {
        openMidiClip();
        test::doubleClick(window_, cell(1.0, 60));
        const QRectF rect = roll()->noteRect(clipNotes().front());
        const auto hover = [this](QPointF pos) {
            test::moveTo(window_, test::at(grid(), pos), {}, Qt::NoButton);
            return grid()->cursor().shape();
        };
        QCOMPARE(hover(rect.center()), Qt::PointingHandCursor);  // moves it
        QCOMPARE(hover(QPointF(rect.right() - 1, rect.center().y())), Qt::SizeHorCursor);  // resizes it
        QCOMPARE(hover(QPointF(rect.left() + 1, rect.center().y())), Qt::SizeHorCursor);
        QCOMPARE(hover(QPointF(rect.right() + 40, rect.center().y())), Qt::ArrowCursor);
        // The hand as soon as Ctrl+Alt is held, without moving the mouse.
        QTest::keyPress(window_, Qt::Key_Alt, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(grid()->cursor().shape(), Qt::OpenHandCursor);
        QTest::keyRelease(window_, Qt::Key_Alt, Qt::ControlModifier);
        QCOMPARE(grid()->cursor().shape(), Qt::ArrowCursor);
        // The velocity lane: up and down by a stem.
        auto* lane = clipView_->findChild<sub::ui::VelocityLane*>(QStringLiteral("velocityLane"));
        test::moveTo(window_, test::at(lane, QPointF(int(roll()->view().beatToX(1.0)) + 4, 30)), {}, Qt::NoButton);
        QCOMPARE(lane->cursor().shape(), Qt::SizeVerCursor);
        test::moveTo(window_, test::at(lane, QPointF(int(roll()->view().beatToX(1.0)) + 20, 30)), {}, Qt::NoButton);
        QCOMPARE(lane->cursor().shape(), Qt::ArrowCursor);
    }

    void thePlayheadFollowsPlaybackInsideTheClip() {
        openMidiClip();
        sub::app::EngineBridge* bridge = session().bridge();
        bridge->locate(5.0);  // a beat into the clip
        QVERIFY(!roll()->playhead());
        bridge->play();
        if (!bridge->isPlaying()) QSKIP("the engine doesn't play without an audio device here");
        QCOMPARE(roll()->playhead(), std::optional<double>(1.0));
        session().selection()->setInsert(4.5);
        test::screenshot(window_, QStringLiteral("piano-roll-playhead"), QRect(0, 0, 400, 120));
        bridge->stop();
        QVERIFY(!roll()->playhead());  // stopped: only the start marker
        bridge->locate(9.0);  // past the clip
        bridge->play();
        QVERIFY(!roll()->playhead());
        bridge->stop();
    }

    void fittedOnceShown() {
        // Opened while hidden (the device view showing), the clip is fitted when it shows.
        clipView_->setVisible(false);
        trackId_ = session().editor()->addMidiTrack();
        clipId_ = session().editor()->addMidiClip(trackId_, 4.0, 2.0)->clipId;
        session().editor()->setClipNotes({trackId_, clipId_}, {note(84, 0.0, 1.0), note(88, 1.0, 1.0)},
                                         QStringLiteral("setup"));
        clipView_->setProperty("trackId", trackId_);
        clipView_->setProperty("clipIds", QVariantList{clipId_});
        QVERIFY(!grid()->isVisible());
        clipView_->setVisible(true);
        QTRY_VERIFY(grid()->isVisible());
        QVERIFY(std::abs(roll()->view().beatToX(2.0) - grid()->width() * 0.96) <= 1.0);
        QCOMPARE(roll()->scrollBeats(), 0.0);
        // Its notes centred.
        QCOMPARE(roll()->pitchAt(grid()->height() / 2), 86);
        QVERIFY(grid()->hasActiveFocus());
        // Hidden (back to the devices) it forgets the clip, a key sounding stops;
        // shown again, it opens it afresh, fitted, nothing selected.
        QTest::keyClick(window_, Qt::Key_A, Qt::ControlModifier);
        roll()->zoomAt(0, 3.0);
        test::press(window_, cell(0.5, 84));
        QCOMPARE(roll()->auditioned(), std::optional<int>(84));
        clipView_->setVisible(false);
        QVERIFY(!roll()->auditioned());
        QVERIFY(!roll()->hasClip());
        QVERIFY(!grid()->hasFocus());
        test::release(window_, cell(0.5, 84));
        clipView_->setVisible(true);
        QTRY_VERIFY(grid()->isVisible());
        QVERIFY(roll()->hasClip());
        QVERIFY(roll()->selected().empty());
        QVERIFY(std::abs(roll()->view().beatToX(2.0) - grid()->width() * 0.96) <= 1.0);
        QVERIFY(grid()->hasActiveFocus());
    }

    void screenshots() {
        openMidiClip();
        std::vector<Note> notes;
        // A bass line, chords and a melody, louder on the beat.
        const int chord[][3] = {{60, 64, 67}, {57, 60, 64}, {53, 57, 60}, {55, 59, 62}};
        for (int i = 0; i < 4; ++i) {
            for (int pitch : chord[i]) notes.push_back(note(pitch, i * 1.0, 0.9, 90));
            notes.push_back(note(chord[i][0] - 12, i * 1.0, 0.45, 120));
            notes.push_back(note(chord[i][0] - 12, i * 1.0 + 0.5, 0.45, 60));
            notes.push_back(note(chord[i][2] + 5, i * 1.0 + 0.25, 0.25, 40 + 20 * i));
        }
        notes.push_back(note(72, 4.5, 1.0, 100));  // past the clip's end: dimmed
        session().editor()->setClipNotes({trackId_, clipId_}, notes, QStringLiteral("setup"));
        // As a new piano roll has it (other tests changed it).
        roll()->zoomRows((PianoRoll::kRowHeight - roll()->rowHeight()) / PianoRoll::kRowHeightStep, 0);
        QCOMPARE(roll()->rowHeight(), PianoRoll::kRowHeight);
        roll()->setQuantizeGrid(QStringLiteral("1/16"));
        roll()->setQuantizeAmount(100);
        roll()->setHumanizeVelocityAmount(PianoRoll::kDefaultHumanizeVelocity);
        roll()->setHumanizeTimingAmount(PianoRoll::kDefaultHumanizeTiming);
        roll()->setScrollY(roll()->pitchTop(79) + roll()->scrollY());
        // A group selected by dragging: the tools float by it.
        test::drag(window_, cell(0.0, 68) - QPoint(2, 0), cell(1.8, 56));
        waitForTools();
        session().selection()->setInsert(4.5);  // the start marker, half a beat into the clip
        test::screenshot(window_, QStringLiteral("piano-roll"));
        auto* lane = clipView_->findChild<QQuickItem*>(QStringLiteral("velocityLane"));
        test::screenshot(window_, QStringLiteral("piano-roll-velocity"),
                         QRect(test::at(lane, QPointF(-64, -40)), QSize(int(lane->width()) + 64, 112)));
        QQuickItem* bar = tools();
        test::screenshot(window_, QStringLiteral("piano-roll-note-tools"),
                         QRect(test::at(bar, QPointF(-20, -20)),
                               QSize(int(bar->width()) + 40, int(bar->height()) + 40)));
        QVERIFY(!window_->grabWindow().isNull());
    }

    // The song's chords along the top of the notes, over the part the clip
    // plays, in its own beats; hidden (C) with the key; clicks going through
    // them to the notes.
    void theSongsChordsShowAlongTheTop() {
        openMidiClip();  // beats 4 to 8
        keysTrack(chord({48, 52, 55}, 0, 6) + chord({43, 47, 50}, 6, 2));
        QTRY_COMPARE(chordNames(), QStringLiteral("C 0-2, G 2-4"));
        QQuickItem* lane = chordLane();
        QVERIFY(lane->isVisible());
        QCOMPARE(lane->height(), double(sub::ui::ChordLane::kHeight));
        QCOMPARE(lane->width(), grid()->width());
        QCOMPARE(clipView_->findChild<QObject*>(QStringLiteral("keyLabel"))->property("text").toString(),
                 QStringLiteral("Key: C Major (inferred)"));
        const QPoint inC = test::at(lane, QPointF(roll()->view().beatToX(1.5), lane->height() - 3));
        const QColor shown = window_->grabWindow().pixelColor(inC);
        // (With a melody, an F# out of the key among it, to look at.)
        roll()->commit({note(72, 0, 1), note(74, 1, 0.5), note(76, 1.5, 0.5), note(78, 2, 1), note(79, 3, 1)},
                       QStringLiteral("Notes"));
        QTRY_VERIFY(roll()->outOfKey(78));
        const int scrolled = roll()->scrollY();
        roll()->setScrollY(roll()->pitchTop(84) + roll()->scrollY());
        test::screenshot(window_, QStringLiteral("piano-roll-chords"));
        roll()->setScrollY(scrolled);
        roll()->commit({}, QStringLiteral("Notes"));

        session().harmony()->setShown(false);
        QVERIFY(!lane->isVisible());
        QVERIFY(roll()->chords().empty());
        QVERIFY(!roll()->scaleKey());
        QVERIFY(!clipView_->findChild<QQuickItem*>(QStringLiteral("keyLabel"))->isVisible());
        QTRY_VERIFY(window_->grabWindow().pixelColor(inC) != shown);  // the grid, untinted
        session().harmony()->setShown(true);
        QCOMPARE(chordNames(), QStringLiteral("C 0-2, G 2-4"));

        // A double-click in the lane adds a note under it: the lane takes no clicks.
        test::doubleClick(window_, test::at(lane, QPointF(roll()->view().beatToX(3.0) + 3, 9)));
        QCOMPARE(clipNotes().size(), size_t(1));
        QCOMPARE(clipNotes().front().pitch, roll()->pitchAt(9));
    }

    // Notes out of the key are tinted red: the project's key, or the one the
    // song is in; none while the harmony is hidden.
    void notesOutOfTheKeyShowRed() {
        openMidiClip();
        session().editor()->setKeyByName(QStringLiteral("C"));
        roll()->commit({note(60, 0, 1), note(66, 1, 1)}, QStringLiteral("Notes"));
        QTRY_VERIFY(roll()->scaleKey().has_value());
        QVERIFY(!roll()->outOfKey(60));
        QVERIFY(roll()->outOfKey(66));
        QVERIFY(roll()->outOfKey(66 + 12));
        const QImage image = window_->grabWindow();
        const QColor c = gridPixel(image, 0.9, 60), fSharp = gridPixel(image, 1.9, 66);
        QVERIFY2(redness(fSharp) > redness(c) + 20,
                 qPrintable(QStringLiteral("%1 vs %2").arg(fSharp.name(), c.name())));

        // No key set: the one the song's notes are in.
        session().editor()->setKeyByName(QString());
        roll()->commit(chord({60, 64, 67}, 0, 4) + std::vector<Note>{note(65, 0, 1), note(62, 1, 1), note(66, 2, 0.5)},
                       QStringLiteral("Notes"));
        QTRY_COMPARE(session().harmony()->keyLabel(), QStringLiteral("C Major"));
        QTRY_VERIFY(roll()->outOfKey(66));
        QVERIFY(!roll()->outOfKey(65));

        session().harmony()->setShown(false);
        QVERIFY(!roll()->outOfKey(66));
    }

    // Generate › Chords and › Bass: from a progression in the key where the
    // song has no chords, else from the song's; one undo step each, selected.
    void generateWritesChordsAndABass() {
        openMidiClip();  // an empty song: I V vi IV in C major, from the clip's bar
        generate("generateChords");
        std::vector<Note> notes = sortedBy(clipNotes(), true);
        QCOMPARE(notes.size(), size_t(3));
        QCOMPARE(notes[0].pitch, 60);
        QCOMPARE(notes[1].pitch, 64);
        QCOMPARE(notes[2].pitch, 67);
        for (const Note& n : notes) {
            QCOMPARE(n.start, 0.0);
            QCOMPARE(n.length, 4.0);
        }
        QCOMPARE(roll()->selectedCount(), 3);
        QCOMPARE(undo().undoText(), QStringLiteral("Generate Chords"));

        generate("generateBass");  // under the chords it just wrote (the song's now)
        notes = sortedBy(clipNotes(), true);
        QCOMPARE(notes.size(), size_t(4));
        QCOMPARE(notes[0].pitch, 36);  // C1
        QCOMPARE(roll()->selectedCount(), 1);
        QCOMPARE(undo().undoText(), QStringLiteral("Generate Bass"));
        generate("generateBass");  // again: nothing more to write
        QCOMPARE(clipNotes().size(), size_t(4));
        QCOMPARE(undo().undoText(), QStringLiteral("Generate Bass"));
        undo().undo();
        undo().undo();
        QVERIFY(clipNotes().empty());

        // Another track plays A minor over the clip, then F: the bass follows it.
        keysTrack(chord({45, 57, 60, 64}, 4, 2) + chord({41, 57, 60, 65}, 6, 2));
        QTRY_COMPARE(chordNames(), QStringLiteral("Am 0-2, F 2-4"));
        generate("generateBass");
        notes = sortedBy(clipNotes());
        QCOMPARE(notes.size(), size_t(2));
        QCOMPARE(notes[0].pitch, 45);  // A1
        QCOMPARE(notes[0].length, 2.0);
        QCOMPARE(notes[1].pitch, 41);  // F1
        QCOMPARE(notes[1].start, 2.0);
    }
};

namespace {

const char* const kWindow = R"(
import QtQuick
import SUBstation

Window {
    width: 1100
    height: 560

    // The main window's shortcuts for clips, counting when they fire.
    property int deletes: 0
    property int selectAlls: 0
    property int duplicates: 0
    property int quantizes: 0
    property int deactivates: 0
    property int copies: 0
    property int cuts: 0
    property int pastes: 0

    Shortcut { sequences: [StandardKey.Delete]; onActivated: deletes++ }
    Shortcut { sequences: [StandardKey.SelectAll]; onActivated: selectAlls++ }
    Shortcut { sequence: "Ctrl+D"; onActivated: duplicates++ }
    Shortcut { sequence: "Ctrl+U"; onActivated: quantizes++ }
    Shortcut { sequence: "0"; onActivated: deactivates++ }
    Shortcut { sequences: [StandardKey.Copy]; onActivated: copies++ }
    Shortcut { sequences: [StandardKey.Cut]; onActivated: cuts++ }
    Shortcut { sequences: [StandardKey.Paste]; onActivated: pastes++ }

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

QTEST_MAIN(TestUiPianoRoll)
#include "test_ui_pianoroll.moc"
