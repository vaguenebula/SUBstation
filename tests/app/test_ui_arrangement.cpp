// The arrangement, driven as a user would, with mouse, wheel and key events on
// its items in a window: clips dragged, copied, trimmed, selected and heard
// where a drag takes them; time selections; the ruler's loop brace and scrub
// area; zoom, scroll and follow; Alt+wheel resizing and folding; Ctrl+Alt
// drags; files, devices and presets dropped; clips opened in the clip view; the
// lanes' menus; MIDI clips; reversing; the takes drawn while recording. Runs on
// a display (xvfb here). With $SUBSTATION_SCREENS set, it saves screenshots
// there.

#include <QDragMoveEvent>
#include <QFileInfo>
#include <QDropEvent>
#include <QMimeData>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <memory>

#include "ArrangementTestSupport.h"
#include "browser/BrowserController.h"
#include "browser/BrowserMime.h"
#include "theme/Theme.h"
#include "audio/EngineBridge.h"
#include "editor/ProjectEditor.h"
#include "files/FileManager.h"
#include "files/HotSwap.h"
#include "model/Clip.h"
#include "model/Project.h"
#include "session/ArrangementActions.h"
#include "session/Selection.h"

using sub::app::Clip;
using sub::app::ClipRef;
using sub::ui::ArrangementLanes;
namespace test = sub::app::test;
namespace arr = sub::ui::arrangement;

namespace {

constexpr int kSamplesPerBeat = test::kSampleRate / 2;  // at 120 BPM

using Span = std::pair<double, double>;

}  // namespace

class TestUiArrangement : public QObject {
    Q_OBJECT

    std::unique_ptr<test::UiSession> ui_;
    std::unique_ptr<test::ArrangementHarness> h_;
    std::unique_ptr<test::TempDir> dir_;
    QStringList tones_;  // three tone files: 2, 3 and 4 s long

    sub::app::Session& session() { return ui_->session(); }
    sub::app::Project& project() { return *session().project(); }
    sub::app::ProjectEditor& editor() { return *session().editor(); }
    sub::app::Selection& selection() { return *session().selection(); }
    sub::app::EngineBridge& bridge() { return *session().bridge(); }
    QUndoStack& undo() { return *session().undoStack(); }
    QQuickWindow* window() { return h_->window(); }
    ArrangementLanes* lanes() { return h_->lanes(); }
    sub::ui::Arrangement* arrangement() { return h_->arrangement(); }

    bool waitForSource(const QString& path) {
        bridge().requestSource(path);
        return QTest::qWaitFor([&] { return bridge().source(path) != nullptr; }, 10000);
    }

    // The Python tests' three_tracks: three audio tracks of tones, their clips at beats 0-4, 2-8 and 4-12.
    void threeTracks() {
        for (int i = 0; i < 3; ++i) {
            editor().addClips(QString(), i * 2.0, {{tones_[i], 2.0 + i}}, static_cast<int>(project().tracks().size()));
        }
        for (const QString& path : tones_) QVERIFY(waitForSource(path));
        h_->settle();
    }

    std::vector<Span> spans(const QString& trackId) {
        std::vector<Span> out;
        const double tempo = project().tempo();
        for (const Clip& c : project().track(trackId).clips) {
            out.emplace_back(std::round(c.startBeat * 1e6) / 1e6, std::round(c.endBeat(tempo) * 1e6) / 1e6);
        }
        return out;
    }

    QString trackId(int index) { return project().tracks()[static_cast<size_t>(index)].id; }
    const Clip& clipOf(int index, int clip = 0) {
        return project().tracks()[static_cast<size_t>(index)].clips[static_cast<size_t>(clip)];
    }
    QSet<ClipRef> refs(std::initializer_list<ClipRef> list) { return QSet<ClipRef>(list.begin(), list.end()); }

private Q_SLOTS:
    void initTestCase() {
        test::prepareApplication();
        if (!test::haveDisplay()) QSKIP("needs a display: Qt Quick's software renderer draws none of this geometry");
        sub::ui::setUpApplication();
        ui_ = std::make_unique<test::UiSession>();
        h_ = std::make_unique<test::ArrangementHarness>(*ui_);
        QVERIFY(h_->show());
        dir_ = std::make_unique<test::TempDir>();
        for (int i = 0; i < 3; ++i) {
            tones_ << test::writeWav(dir_->path(QStringLiteral("tone%1.wav").arg(i)),
                                     test::tone(2.0 + i, 220.0 * (i + 1)), 2);
        }
    }

    void cleanupTestCase() {
        h_.reset();
        ui_.reset();
    }

    void init() {
        bridge().stop();
        session().newProject();
        undo().clear();
        session().arrangement()->setClipboard({});
        arrangement()->setSnap(true);
        arrangement()->setGridLevel(0);
        arrangement()->setFollow(true);
        h_->setZoom(sub::ui::timeline::Timeline::kDefaultPxPerBeat);
        arrangement()->setScrollBeats(0.0);
        arrangement()->setScrollY(0);
        test::moveTo(window(), QPoint(2, 2), {}, Qt::NoButton);
        h_->settle();
    }

    void cleanup() { bridge().stop(); }

    void arrangementRendersAndMirrorsTheEngine() {
        threeTracks();
        QCOMPARE(project().tracks().size(), size_t(3));
        QCOMPARE(project().tracks()[0].name, QStringLiteral("1 tone0"));  // (named by its clips, numbered by its place)
        for (int i = 0; i < 3; ++i) QVERIFY(h_->header(trackId(i)));
        QVERIFY(!window()->grabWindow().isNull());
        test::screenshot(window(), QStringLiteral("arrangement_three_tracks"));
        // The engine plays what the model says: clip 2 starts at beat 4 (2 s at 120 BPM).
        std::vector<float> out = ui_->engine().renderOffline(0.0, 3 * test::kSampleRate);
        float loudest = 0.0f;
        for (int i = 0; i < test::kSampleRate / 2; ++i) loudest = std::max(loudest, std::abs(out[size_t(2 * i)]));
        QVERIFY(loudest > 0.1f);  // clip 0 at beat 0
        editor().trySetTrackParam(trackId(0), QStringLiteral("mute"), 1.0);
        editor().trySetTrackParam(trackId(1), QStringLiteral("mute"), 1.0);
        out = ui_->engine().renderOffline(0.0, 3 * test::kSampleRate);
        loudest = 0.0f;
        for (int i = 0; i < 2 * test::kSampleRate; ++i) loudest = std::max(loudest, std::abs(out[size_t(2 * i)]));
        QCOMPARE(loudest, 0.0f);  // clip 2 only starts at 2 s
        // The activators show it.
        QVERIFY(!h_->control(h_->header(trackId(0)), QStringLiteral("activator"))->property("checked").toBool());
        QVERIFY(h_->control(h_->header(trackId(2)), QStringLiteral("activator"))->property("checked").toBool());
    }

    void mouseDragMovesClipAndUndoRestores() {
        threeTracks();
        const QString track = trackId(0);
        const Clip clip = clipOf(0);
        const QPointF start(h_->x(clip.startBeat) + 30, h_->row(0).top - arrangement()->scrollY() + 6);  // title bar
        const QPointF end = start + QPointF(4 * arrangement()->pxPerBeat(), 0);
        test::press(window(), h_->at(start));
        test::moveTo(window(), h_->at(start + QPointF(10, 0)));
        test::moveTo(window(), h_->at(end));
        test::release(window(), h_->at(end));
        const Clip& moved = clipOf(0);
        QCOMPARE(moved.id, clip.id);
        QCOMPARE(moved.startBeat, 4.0);
        QCOMPARE(selection().clips(), refs({{track, clip.id}}));
        QCOMPARE(selection().insertBeat(), 4.0);  // playback follows the moved clip
        undo().undo();
        QCOMPARE(clipOf(0).startBeat, 0.0);
    }

    void cutCopyPasteClips() {
        threeTracks();
        const QString first = trackId(0), second = trackId(1), third = trackId(2);  // clips at 0-4, 2-8 and 4-12
        // Nothing copied yet: pasting says so and changes nothing.
        QSignalSpy messages(&session(), &sub::app::Session::statusMessage);
        session().paste();
        QCOMPARE(undo().count(), 3);
        QVERIFY(!messages.isEmpty());

        selection().selectClips(editor(), {{first, clipOf(0).id}});
        session().copy();
        // Pasted at the insert marker on the selected track, and selected.
        selection().selectTrack(third);
        selection().setInsert(20.0);
        session().paste();
        QCOMPARE(spans(third), (std::vector<Span>{{4, 12}, {20, 24}}));
        const Clip pasted = clipOf(2, 1);
        QCOMPARE(pasted.path, clipOf(0).path);
        QVERIFY(pasted.id != clipOf(0).id);
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{20.0, 24.0, {third}}));
        QCOMPARE(selection().clips(), refs({{third, pasted.id}}));
        // The insert marker moves to its end, so pasting again appends.
        QCOMPARE(selection().insertBeat(), 24.0);
        session().paste();
        QCOMPARE(spans(third).size(), size_t(3));
        QCOMPARE(spans(third)[2].first, 24.0);

        // Cut takes just the selected stretch out; the empty area stays selected.
        selection().setTimeRange(5.0, 7.0, {second}, editor().clipsInRange(5.0, 7.0, {second}));
        session().cut();
        QCOMPARE(spans(second), (std::vector<Span>{{2, 5}, {7, 8}}));
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{5.0, 7.0, {second}}));
        QVERIFY(selection().clips().isEmpty());
        QCOMPARE(undo().undoText(), QStringLiteral("Cut"));
        selection().setInsert(12.0);
        session().paste();
        QCOMPARE(spans(second), (std::vector<Span>{{2, 5}, {7, 8}, {12, 14}}));
        undo().undo();
        undo().undo();
        QCOMPARE(spans(second), (std::vector<Span>{{2, 8}}));
    }

    void zoomScrollAndFollow() {
        threeTracks();
        const double before = arrangement()->pxPerBeat();
        QMetaObject::invokeMethod(h_->view(), "zoom", Q_ARG(QVariant, 2.0));
        QCOMPARE(arrangement()->pxPerBeat(), before * 2.0);
        QMetaObject::invokeMethod(h_->view(), "zoomToArrangement");
        QCOMPARE(arrangement()->scrollBeats(), 0.0);
        // The whole arrangement (12 beats, at least 8 bars) fits.
        QCOMPARE(arrangement()->pxPerBeat(), lanes()->width() / (32 * 1.05));
        arrangement()->setScrollBeats(8.0);
        QCOMPARE(arrangement()->hScrollValue(), std::trunc(8.0 * arrangement()->pxPerBeat()));
        arrangement()->scrollToY(1e6);
        QCOMPARE(arrangement()->scrollY(), arrangement()->view().maxScrollY());
        // The vertical scroll bar reaches over the tracks and the drop zone below them.
        QCOMPARE(arrangement()->vScrollTotal(),
                 std::max(arrangement()->totalHeight() + arr::kDropZone, int(lanes()->height())) + 0.0);
        QVERIFY(!window()->grabWindow().isNull());
    }

    void gridLevelsAndSnapping() {
        QCOMPARE(arrangement()->gridStep(), 1.0);  // 24 px a beat: quarters
        QCOMPARE(h_->view()->property("gridStep").toDouble(), 1.0);
        QMetaObject::invokeMethod(h_->view(), "narrowGrid");
        QCOMPARE(arrangement()->gridLevel(), -1);
        QCOMPARE(arrangement()->gridStep(), 0.5);
        QCOMPARE(arrangement()->gridLabel(), QStringLiteral("Grid 1/8"));
        QMetaObject::invokeMethod(h_->view(), "widenGrid");
        QMetaObject::invokeMethod(h_->view(), "widenGrid");
        QCOMPARE(arrangement()->gridStep(), 2.0);
        for (int i = 0; i < 5; ++i) QMetaObject::invokeMethod(h_->view(), "widenGrid");
        QCOMPARE(arrangement()->gridLevel(), 2);  // the widest
        QCOMPARE(arrangement()->gridLabel(), QStringLiteral("Grid 1 Bar"));
        h_->view()->setProperty("gridLevel", 0);
        // A click on the grid's label toggles snapping.
        QQuickItem* info = h_->find<QQuickItem*>(QStringLiteral("gridInfo"));
        test::click(window(), test::centerOf(info));
        QVERIFY(!h_->view()->property("snap").toBool());
        QCOMPARE(arrangement()->gridLabel(), QStringLiteral("Grid 1/4 (off)"));
        QCOMPARE(arrangement()->view().snapBeat(1.3), 1.3);
        h_->view()->setProperty("snap", true);
        QCOMPARE(arrangement()->view().snapBeat(1.3), 1.0);
    }

    void rulerLoopBraceAndScrubZoom() {
        sub::ui::ArrangementRuler* ruler = h_->ruler();
        const auto at = [&](double beat, double y) { return test::at(ruler, QPointF(h_->x(beat), y)); };
        const double y = 6;  // the loop strip
        QCOMPARE(project().loopStart(), 0.0);  // the default brace
        QCOMPARE(project().loopEnd(), 16.0);
        // Dragging outside the brace draws a new loop and enables it.
        test::drag(window(), at(20.0, y), at(28.0, y));
        QVERIFY(project().loopEnabled());
        QCOMPARE(project().loopStart(), 20.0);
        QCOMPARE(project().loopEnd(), 28.0);
        // Dragging the brace body moves it; the whole gesture is one undo step.
        test::drag(window(), at(24.0, y), at(22.0, y));
        QCOMPARE(project().loopStart(), 18.0);
        QCOMPARE(project().loopEnd(), 26.0);
        undo().undo();
        QCOMPARE(project().loopStart(), 20.0);
        QCOMPARE(project().loopEnd(), 28.0);
        // Dragging an edge resizes it.
        test::drag(window(), at(28.0, y), at(32.0, y));
        QCOMPARE(project().loopStart(), 20.0);
        QCOMPARE(project().loopEnd(), 32.0);
        // Double-clicking it turns it off (and on).
        test::doubleClick(window(), at(24.0, y));
        QVERIFY(!project().loopEnabled());
        test::doubleClick(window(), at(24.0, y));
        QVERIFY(project().loopEnabled());

        const double before = arrangement()->pxPerBeat();
        const double scrubY = ruler->height() - 8;
        test::drag(window(), test::at(ruler, QPointF(300, scrubY)), test::at(ruler, QPointF(300, scrubY + 60)));
        QVERIFY(arrangement()->pxPerBeat() > before);
        arrangement()->setScrollBeats(0.0);
        test::click(window(), at(3.0, scrubY));
        QCOMPARE(bridge().position(), arrangement()->view().snapBeat(3.0));
        QCOMPARE(selection().insertBeat(), arrangement()->view().snapBeat(3.0));
        // Sideways, it scrolls (and stops following the playhead).
        arrangement()->setScrollBeats(8.0);
        const double scrolled = arrangement()->scrollBeats();
        test::drag(window(), test::at(ruler, QPointF(300, scrubY)), test::at(ruler, QPointF(200, scrubY)));
        QCOMPARE(arrangement()->scrollBeats(), scrolled + 100 / arrangement()->pxPerBeat());
        QVERIFY(arrangement()->followPaused());
    }

    void mouseTrimAndSelectingBelowTheTracks() {
        threeTracks();
        const QString track = trackId(0);
        const Clip clip = clipOf(0);
        const auto& row = h_->row(0);
        const double y = row.top - arrangement()->scrollY() + 6;  // trim handles are at the ends of the title bar
        const double bodyY = row.top - arrangement()->scrollY() + row.height() / 2;
        const double tempo = project().tempo();
        const double rightEdge = h_->x(clip.endBeat(tempo)) - 2;
        QCOMPARE(lanes()->hitClip(QPointF(rightEdge, bodyY))->zone, ArrangementLanes::Zone::Body);  // lower down: no trimming
        test::drag(window(), h_->at(QPointF(rightEdge, y)), h_->at(QPointF(h_->x(2.0), y)));
        QVERIFY(std::abs(clipOf(0).durationSec - 1.0) < 1e-9);  // 2 beats at 120 BPM
        const double leftEdge = h_->x(0.0) + 2;
        test::drag(window(), h_->at(QPointF(leftEdge, y)), h_->at(QPointF(h_->x(1.0), y)));
        Clip trimmed = clipOf(0);
        QCOMPARE(trimmed.startBeat, 1.0);
        QVERIFY(std::abs(trimmed.offsetSec - 0.5) < 1e-9);
        QCOMPARE(selection().insertBeat(), 1.0);  // playback starts from the trimmed clip
        QCOMPARE(undo().undoText(), QStringLiteral("Trim Clip"));
        selection().setInsert(6.0);
        test::click(window(), h_->at(QPointF(h_->x(trimmed.endBeat(tempo)) - 2, y)));
        QCOMPARE(selection().insertBeat(), 1.0);  // a click on its edge selects it, as on its body

        // Trim handles are only just inside a clip's ends, shown with bracket cursors.
        const double startX = h_->x(trimmed.startBeat);
        test::moveTo(window(), h_->at(QPointF(startX + 2, y)), {}, Qt::NoButton);
        QCOMPARE(lanes()->cursor().shape(), Qt::BitmapCursor);
        QCOMPARE(lanes()->hoverEdge(), std::make_optional(std::make_pair(trimmed.id, true)));
        QVERIFY(!window()->grabWindow().isNull());
        test::moveTo(window(), h_->at(QPointF(startX - 2, y)), {}, Qt::NoButton);  // just outside: not a trim
        QCOMPARE(lanes()->cursor().shape(), Qt::IBeamCursor);
        QVERIFY(!lanes()->hoverEdge());
        test::drag(window(), h_->at(QPointF(startX - 2, y)), h_->at(QPointF(h_->x(0.0), y)));
        QCOMPARE(clipOf(0).startBeat, 1.0);  // a time selection, not a trim
        // Where two clips touch, each side of the boundary trims its own clip.
        editor().addClips(track, 2.0, {{tones_[0], 1.0}});
        const Clip first = clipOf(0, 0), second = clipOf(0, 1);
        const double boundary = arrangement()->view().beatToX(2.0);
        auto hit = lanes()->hitClip(QPointF(boundary - 2, y));
        QVERIFY(hit && hit->clip.id == first.id && hit->zone == ArrangementLanes::Zone::Right);
        hit = lanes()->hitClip(QPointF(boundary + 2, y));
        QVERIFY(hit && hit->clip.id == second.id && hit->zone == ArrangementLanes::Zone::Left);

        // Below the tracks is grid too: a drag from there selects time on it, from the last track up,
        // snapped like anywhere else (here ending in the first track's title band: the clips it touches).
        const double emptyY = arrangement()->totalHeight() - arrangement()->scrollY() + 40;
        QVERIFY(!lanes()->rowIndexAt(emptyY));
        test::moveTo(window(), h_->at(QPointF(h_->x(3.0), emptyY)), {}, Qt::NoButton);
        QCOMPARE(lanes()->cursor().shape(), Qt::IBeamCursor);
        test::drag(window(), h_->at(QPointF(h_->x(20.3), emptyY)), h_->at(QPointF(h_->x(0.6), 2)));
        QCOMPARE(arrangement()->view().snapBeat(0.6), 1.0);  // (a one-beat grid at this zoom)
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{1.0, 20.0, {trackId(0), trackId(1), trackId(2)}}));
        QVERIFY(selection().clipRange());
        QCOMPARE(selection().clips().size(), 4);  // including the clip added above
        // A click there places the insert marker on the grid and deselects.
        test::click(window(), h_->at(QPointF(h_->x(6.1), emptyY)));
        QVERIFY(!selection().timeRange());
        QVERIFY(selection().clips().isEmpty());
        QCOMPARE(selection().insertBeat(), 6.0);
    }

    void clipBodySetsInsertAndSelectsTime() {
        threeTracks();
        const auto& rows = arrangement()->layout().rows();
        const double bodyY = rows[0].top - arrangement()->scrollY() + rows[0].height() / 2;
        // A click in a clip body places the insert marker instead of selecting the clip.
        test::click(window(), h_->at(QPointF(h_->x(1.0), bodyY)));
        QVERIFY(selection().clips().isEmpty());
        QCOMPARE(selection().insertBeat(), 1.0);
        QCOMPARE(selection().trackId(), trackId(0));
        // Dragging selects a time range across the tracks it covers; play starts at its start.
        // It selects everything in it, the clips it touches too, whether the drag was in their bodies or titles.
        const double endY = rows[1].top - arrangement()->scrollY() + rows[1].height() / 2;
        test::drag(window(), h_->at(QPointF(h_->x(3.0), bodyY)), h_->at(QPointF(h_->x(1.0), endY)));
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{1.0, 3.0, {trackId(0), trackId(1)}}));
        QCOMPARE(selection().insertBeat(), 1.0);
        QVERIFY(selection().clipRange());
        QCOMPARE(selection().clips(), refs({{trackId(0), clipOf(0).id}, {trackId(1), clipOf(1).id}}));
        editor().deleteTracks({trackId(1)});
        QCOMPARE(selection().timeRange()->trackIds, QStringList{trackId(0)});
    }

    void altWheelResizesTracks() {
        threeTracks();
        const QString track = trackId(1);
        const int before = project().track(track).height;
        int clock = 0;
        arrangement()->setClock([&clock] { return clock * 1.0; });  // (each wheel a turn of its own)
        const auto& rows = arrangement()->layout().rows();
        clock += 1;
        test::wheel(window(), h_->at(QPointF(200, rows[1].top - arrangement()->scrollY() + 10)), 2, Qt::AltModifier);
        QCOMPARE(project().track(track).height, before + 24);
        QCOMPARE(arrangement()->layout().rows()[2].top, arrangement()->layout().rows()[1].top + before + 24);
        h_->settle();
        sub::ui::TrackHeaderItem* header = h_->header(track);
        QQuickItem* pan = h_->control(header, QStringLiteral("pan"));
        clock += 1;
        test::wheel(window(), test::centerOf(pan), -1, Qt::AltModifier);  // over a control, too
        QCOMPARE(project().track(track).height, before + 12);
        QCOMPARE(project().track(track).pan, 0.0);
        clock += 1;
        test::wheel(window(), test::at(header, QPointF(20, 5)), -100, Qt::AltModifier);
        QCOMPARE(project().track(track).height, sub::app::kMinTrackHeight);
        QVERIFY(!project().track(track).folded);  // (it got there in this turn)
        arrangement()->setClock({});
        arrangement()->setClock([] { return 0.0; });
    }

    void altWheelFoldsATrackAtItsSmallestAndUnfoldsIt() {
        threeTracks();
        double clock = 100.0;
        arrangement()->setClock([&clock] { return clock; });
        const QString second = trackId(1), third = trackId(2);
        const auto overLane = [&](const QString& id) {
            const auto& r = h_->rowOf(id);
            return h_->at(QPointF(200, r.top - arrangement()->scrollY() + 5));
        };
        const auto turn = [&](QPoint pos, int notches) {
            clock += 0.1;  // one turn of the wheel, a notch at a time
            test::wheel(window(), pos, notches, Qt::AltModifier);
            h_->settle();
        };
        const int height = project().track(second).height;
        const int notches = (height - sub::app::kMinTrackHeight + 11) / 12;
        for (int i = 0; i < notches; ++i) {  // down: it shrinks...
            QVERIFY(!project().track(second).folded);
            turn(overLane(second), -1);
        }
        QCOMPARE(project().track(second).height, sub::app::kMinTrackHeight);
        QVERIFY(!project().track(second).folded);
        turn(overLane(second), -1);  // ...and at its smallest, folds
        QVERIFY(project().track(second).folded);
        // The same turn goes on with that track, though the next is under the mouse now.
        turn(overLane(third), -1);
        QVERIFY(project().track(second).folded);
        QVERIFY(!project().track(third).folded);
        QCOMPARE(project().track(third).height, height);
        clock += 1.0;  // a turn of its own, over a control of its header
        sub::ui::TrackHeaderItem* header = h_->header(second);
        turn(test::at(header, QPointF(header->width() - 30, 8)), 1);  // up: unfolds it...
        QVERIFY(!project().track(second).folded);
        QCOMPARE(project().track(second).height, sub::app::kMinTrackHeight);
        turn(test::at(header, QPointF(20, 5)), 1);  // ...then makes it taller
        QCOMPARE(project().track(second).height, sub::app::kMinTrackHeight + 12);
        arrangement()->setClock([] { return 0.0; });
    }

    void ctrlAltDragScrollsBothWays() {
        threeTracks();
        for (const auto& track : project().tracks()) editor().setTrackHeight(track.id, 400);
        QVERIFY(arrangement()->view().maxScrollY() > 100);
        arrangement()->setScrollBeats(8.0);
        std::vector<std::vector<Clip>> before;
        for (const auto& track : project().tracks()) before.push_back(track.clips);
        test::drag(window(), h_->at(QPointF(400, 300)), h_->at(QPointF(300, 200)), Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(arrangement()->scrollY(), 100);
        QVERIFY(std::abs(arrangement()->scrollBeats() - (8.0 + 100 / arrangement()->pxPerBeat())) < 1e-9);
        for (size_t i = 0; i < before.size(); ++i) QVERIFY(project().tracks()[i].clips == before[i]);  // nothing moved
        // Holding Ctrl+Alt shows the open hand, without moving the mouse.
        test::moveTo(window(), h_->at(QPointF(300, 150)), {}, Qt::NoButton);
        QTest::keyPress(window(), Qt::Key_Control, Qt::ControlModifier);
        QTest::keyPress(window(), Qt::Key_Alt, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(lanes()->cursor().shape(), Qt::OpenHandCursor);
        QTest::keyRelease(window(), Qt::Key_Alt, Qt::ControlModifier);
        QTest::keyRelease(window(), Qt::Key_Control, Qt::NoModifier);
        QCOMPARE(lanes()->cursor().shape(), Qt::IBeamCursor);
    }

    void titleClickSetsPlaybackStart() {
        threeTracks();
        const QString track = trackId(2);
        const Clip clip = clipOf(2);  // starts at 2 s = beat 4
        const QPointF title(h_->x(clip.startBeat) + 30, h_->row(2).top - arrangement()->scrollY() + 14);
        test::moveTo(window(), h_->at(title), {}, Qt::NoButton);
        QCOMPARE(lanes()->cursor().shape(), Qt::PointingHandCursor);
        test::click(window(), h_->at(title));
        QCOMPARE(selection().clips(), refs({{track, clip.id}}));
        QCOMPARE(selection().insertBeat(), 4.0);
        session().togglePlay();
        QVERIFY(std::abs(bridge().position() - 4.0) < 0.1);
        session().togglePlay();
    }

    void ctrlDragCopiesSelectedClip() {
        threeTracks();
        const QString track = trackId(0);
        const Clip clip = clipOf(0);  // beats 0-4
        const auto title = [&](const Clip& c) {
            return h_->at(QPointF(h_->x(c.startBeat) + 30, h_->row(0).top - arrangement()->scrollY() + 6));
        };
        test::click(window(), title(clip));
        // Selecting a clip selects the area it covers on the grid.
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{0.0, 4.0, {track}}));
        QVERIFY(selection().clipRange());
        QCOMPARE(selection().clips(), refs({{track, clip.id}}));
        // Ctrl-dragging the selected clip copies it, leaving the original where it was.
        test::drag(window(), title(clip), title(clip) + QPoint(int(8 * arrangement()->pxPerBeat()), 0), Qt::ControlModifier);
        QCOMPARE(project().track(track).clips.size(), size_t(2));
        const Clip original = clipOf(0, 0), copy = clipOf(0, 1);
        QCOMPARE(original, clip);
        QVERIFY(copy.id != clip.id);
        QCOMPARE(copy.startBeat, 8.0);
        QCOMPARE(selection().clips(), refs({{track, copy.id}}));
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{8.0, 12.0, {track}}));
        // A Ctrl-click (no drag) just selects the clip's area.
        test::click(window(), title(original), Qt::ControlModifier);
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{0.0, 4.0, {track}}));
        QCOMPARE(project().track(track).clips.size(), size_t(2));  // clicks copy nothing
    }

    void shiftClickSelectsTheClipsInBetween() {
        threeTracks();
        editor().addClips(trackId(1), 20.0, {{tones_[0], 2.0}});  // outside the span
        const QString far = clipOf(1, 1).id;
        const auto title = [&](int index, const Clip& c) {
            return h_->at(QPointF(h_->x(c.startBeat) + 10, h_->row(index).top - arrangement()->scrollY() + 6));
        };
        test::click(window(), title(0, clipOf(0)));
        test::click(window(), title(2, clipOf(2)), Qt::ShiftModifier);
        // The area fully containing both clips, on the tracks from one to the other.
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{0.0, 12.0, {trackId(0), trackId(1), trackId(2)}}));
        QCOMPARE(selection().clips(),
                 refs({{trackId(0), clipOf(0).id}, {trackId(1), clipOf(1).id}, {trackId(2), clipOf(2).id}}));
        QVERIFY(!selection().clips().contains({trackId(1), far}));
        // A plain click on one of the selected clips selects just it (a drag would move them all).
        test::click(window(), title(1, clipOf(1)));
        QCOMPARE(selection().clips(), refs({{trackId(1), clipOf(1).id}}));
        test::click(window(), title(0, clipOf(0)));
        test::click(window(), title(2, clipOf(2)), Qt::ShiftModifier);
        // Shift-clicking again extends from the same anchor rather than adding to the selection.
        test::click(window(), title(0, clipOf(0)), Qt::ShiftModifier);
        QCOMPARE(selection().clips(), refs({{trackId(0), clipOf(0).id}}));
    }

    void doubleClickClipOpensClipView() {
        threeTracks();
        const QString track = trackId(0);
        const Clip clip = clipOf(0);
        QSignalSpy opened(session().arrangement(), &sub::app::ArrangementActions::clipViewRequested);
        test::doubleClick(window(), h_->lane(0, 1.0));
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.first().at(0).toList().size(), 1);
        QCOMPARE(opened.first().at(1).toString(), track);
        QCOMPARE(opened.first().at(2).toString(), clip.id);
        QCOMPARE(selection().clips(), refs({{track, clip.id}}));
        // Shift+Tab: the selected clips open again.
        QMetaObject::invokeMethod(h_->view(), "openClipView");
        QCOMPARE(opened.count(), 2);
        QCOMPARE(opened.last().at(0).toList().first().toMap().value(QStringLiteral("clipId")).toString(), clip.id);
        // Several selected clips open together.
        selection().selectClips(editor(), {{trackId(0), clipOf(0).id}, {trackId(1), clipOf(1).id}});
        QMetaObject::invokeMethod(h_->view(), "openClipView");
        QCOMPARE(opened.count(), 3);
        QCOMPARE(opened.last().at(0).toList().size(), 2);
        // A double-click's first click selects the clip clicked (as a click does): it opens alone.
        test::doubleClick(window(), h_->lane(1, 3.0, true));
        QCOMPARE(opened.count(), 4);
        QCOMPARE(opened.last().at(0).toList().size(), 1);
        QCOMPARE(opened.last().at(2).toString(), clipOf(1).id);
        // Nothing selected: nothing to open.
        selection().clear();
        QMetaObject::invokeMethod(h_->view(), "openClipView");
        QCOMPARE(opened.count(), 4);
    }

    void aDragOnTheLanesSelectsEverythingInItsRange() {
        threeTracks();
        const auto& rows = arrangement()->layout().rows();
        const int scroll = arrangement()->scrollY();
        const double bodyY = rows[0].top - scroll + rows[0].height() / 2;
        const double laneY = rows[1].top - scroll + rows[1].height() - 8;
        const double bandY = rows[1].top - scroll + 5;
        const QString a = trackId(0), b = trackId(1);
        const QSet<ClipRef> touched = refs({{a, clipOf(0).id}, {b, clipOf(1).id}});
        // Wherever in the lanes it ends (low in a lane, or in the clips' top band), a drag
        // selects everything in its range: it persists, and it knows the clips it touches.
        for (const auto& [startY, endY] : {std::pair{bodyY, laneY}, std::pair{laneY, bodyY}, std::pair{bodyY, bandY}}) {
            selection().clear();
            test::drag(window(), h_->at(QPointF(h_->x(1.0), startY)), h_->at(QPointF(h_->x(3.0), endY)));
            QCOMPARE(selection().timeRange(), (sub::app::TimeRange{1.0, 3.0, {a, b}}));
            QVERIFY(selection().clipRange());
            QCOMPARE(selection().clips(), touched);
        }
        test::screenshot(window(), QStringLiteral("arrangement_time_selection"));
        // Delete cuts out only the range: clip 0 (beats 0-4) keeps its start and end,
        // clip 1 (beats 2-8) loses its first beat, track 3 is outside the range.
        session().deleteSelection();
        QCOMPARE(spans(a), (std::vector<Span>{{0, 1}, {3, 4}}));
        QCOMPARE(spans(b), (std::vector<Span>{{3, 8}}));
        QCOMPARE(project().tracks()[2].clips.size(), size_t(1));
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{1.0, 3.0, {a, b}}));  // still selected
        QVERIFY(selection().clips().isEmpty());
        undo().undo();
        // Ctrl+D copies just the selected area to right after it, then selects the copy.
        selection().setTimeRange(1.0, 3.0, {a, b}, QSet<ClipRef>());
        session().duplicate();
        QCOMPARE(spans(a), (std::vector<Span>{{0, 3}, {3, 5}}));
        QCOMPARE(spans(b), (std::vector<Span>{{2, 4}, {4, 5}, {5, 8}}));
        QVERIFY(std::abs(clipOf(1, 1).offsetSec) < 1e-9);  // the copy plays clip 1's first beat
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{3.0, 5.0, {a, b}}));
        QCOMPARE(selection().insertBeat(), 3.0);
        undo().undo();
        // Clicking a clip's title inside the range selects just that clip's area...
        const QPointF title(h_->x(5.0), rows[1].top - scroll + 6);
        const QPointF body(h_->x(5.0), rows[1].top - scroll + rows[1].height() / 2);
        test::click(window(), h_->at(title));
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{2.0, 8.0, {b}}));
        QCOMPARE(selection().clips(), refs({{b, clipOf(1).id}}));
        // ...and draws it highlighted, but for its title bar.
        h_->settle();
        QImage image = window()->grabWindow();
        const QColor highlighted = image.pixelColor(h_->at(body)), titleColor = image.pixelColor(h_->at(title));
        selection().clear();
        h_->settle();
        image = window()->grabWindow();
        QVERIFY(image.pixelColor(h_->at(body)) != highlighted);
        QCOMPARE(image.pixelColor(h_->at(title)), titleColor);
    }

    void draggingAClipRangeMovesIt() {
        threeTracks();
        const auto& rows = arrangement()->layout().rows();
        const QString a = trackId(0), b = trackId(1), c = trackId(2);
        const auto band = [&](int row, double beat) {
            return h_->at(QPointF(h_->x(beat), rows[size_t(row)].top - arrangement()->scrollY() + 5));
        };
        const auto selectRange = [&] {
            const QPoint body = h_->at(QPointF(h_->x(1.0), rows[0].top - arrangement()->scrollY() + rows[0].height() / 2));
            test::drag(window(), body, band(1, 3.0));
            QCOMPARE(selection().timeRange(), (sub::app::TimeRange{1.0, 3.0, {a, b}}));
            QVERIFY(selection().clipRange());
        };
        // Clips: track 0 beats 0-4, track 1 beats 2-8, track 2 beats 4-12.
        selectRange();
        const int depth = undo().count();
        test::drag(window(), band(0, 2.0), band(0, 6.0));  // grabbing any selected clip moves the whole range
        QCOMPARE(spans(a), (std::vector<Span>{{0, 1}, {3, 4}, {5, 7}}));
        QCOMPARE(spans(b), (std::vector<Span>{{3, 6}, {6, 7}, {7, 8}}));  // the moved beat replaces what it lands on
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{5.0, 7.0, {a, b}}));
        QCOMPARE(undo().count(), depth + 1);
        undo().undo();
        QCOMPARE(spans(a).size() + spans(b).size() + spans(c).size(), size_t(3));

        // Down a track, and Ctrl copies instead.
        selectRange();
        test::drag(window(), band(0, 2.0), band(1, 2.0), Qt::ControlModifier);
        QCOMPARE(spans(a), (std::vector<Span>{{0, 4}}));
        QCOMPARE(spans(b), (std::vector<Span>{{1, 3}, {3, 8}}));
        QCOMPARE(spans(c), (std::vector<Span>{{2, 3}, {4, 12}}));
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{1.0, 3.0, {b, c}}));
        undo().undo();

        // A click inside the range, without dragging, still selects just that clip's area.
        selectRange();
        test::click(window(), band(1, 2.5));
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{2.0, 8.0, {b}}));
        QCOMPARE(selection().clips(), refs({{b, clipOf(1).id}}));
        QVERIFY(!lanes()->gestureActive());

        // Dragging a selected clip moves the very clip (same id), unchanged.
        const Clip clip = clipOf(1);
        test::drag(window(), band(1, 2.5), band(1, 4.5));
        QCOMPARE(project().track(b).clips.size(), size_t(1));
        QCOMPARE(clipOf(1).id, clip.id);
        QCOMPARE(clipOf(1).startBeat, 4.0);
        QCOMPARE(clipOf(1).durationSec, clip.durationSec);
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{4.0, 10.0, {b}}));
    }

    void aDragInProgressShowsWhereTheClipsGo() {
        threeTracks();
        const auto& rows = arrangement()->layout().rows();
        const QPoint start = h_->at(QPointF(h_->x(1.0), rows[0].top - arrangement()->scrollY() + 6));
        const QPoint end = h_->at(QPointF(h_->x(9.0), rows[1].top - arrangement()->scrollY() + 6));
        test::press(window(), start);
        test::moveTo(window(), start + QPoint(10, 0));
        test::moveTo(window(), end);
        h_->settle();
        QVERIFY(lanes()->gestureActive());
        test::screenshot(window(), QStringLiteral("arrangement_drag_in_progress"));
        test::release(window(), end);
        QCOMPARE(clipOf(1, 1).startBeat, 8.0);  // onto the next audio track, at beat 8
    }

    void dropFilesFromTheBrowser() {
        const QString path = test::writeWav(dir_->path(QStringLiteral("dropped.wav")), test::tone(1.0, 330.0), 2);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path)});
        const QPointF pos(arrangement()->view().beatToX(4.0) + 1, 20);
        // Over the lanes: a dashed box shows where it would go.
        QVERIFY(lanes()->dragOver(&mime, pos));
        QVERIFY(lanes()->dropPreview());
        QCOMPARE(lanes()->dropPreview()->beat, 4.0);
        QCOMPARE(lanes()->dropPreview()->sources.size(), size_t(1));
        QCOMPARE(lanes()->dropPreview()->sources[0].name, QStringLiteral("dropped"));
        h_->settle();
        QVERIFY(!window()->grabWindow().isNull());
        QVERIFY(lanes()->drop(&mime, pos));
        QVERIFY(!lanes()->dropPreview());
        QCOMPARE(project().tracks().size(), size_t(1));
        QCOMPARE(project().tracks()[0].name, QStringLiteral("1 dropped"));
        QCOMPARE(clipOf(0).startBeat, 4.0);
        QVERIFY(std::abs(clipOf(0).durationSec - 1.0) < 1e-9);
        QVERIFY(waitForSource(path));
        QCOMPARE(selection().clips().size(), 1);
        // Dropped through the window, as a drag from the file manager comes: onto the audio track.
        const QPoint target = h_->lane(0, 8.0, true);
        QDragEnterEvent enter(target, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window(), &enter);
        QDragMoveEvent move(target, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window(), &move);
        QVERIFY(move.isAccepted());
        QDropEvent drop(target, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(window(), &drop);
        QCOMPARE(project().tracks().size(), size_t(1));
        QCOMPARE(spans(trackId(0)).size(), size_t(2));
        QCOMPARE(spans(trackId(0))[1].first, 8.0);
    }

    void aDroppedLoopIsSetUpInTheSameUndoStep() {
        editor().setKeyByName(QStringLiteral("C"));
        const QString path = test::writeWav(dir_->path(QStringLiteral("Bass_Loop_100_D.wav")), test::tone(1.0, 330.0), 2);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path)});
        const QPointF pos(arrangement()->view().beatToX(0.0) + 1, 20);
        const int steps = undo().index();
        lanes()->dragOver(&mime, pos);
        QVERIFY(lanes()->drop(&mime, pos));
        const Clip clip = clipOf(0);
        QVERIFY(clip.isWarped());
        QCOMPARE(clip.segmentBpm, 100.0);
        QCOMPARE(clip.transpose, -2);
        QCOMPARE(undo().index(), steps + 1);
        undo().undo();
        QVERIFY(project().tracks().empty());
        QCOMPARE(project().keyName(), QStringLiteral("C"));
    }

    void devicesDroppedOnATrack() {
        threeTracks();
        QMimeData utility;
        utility.setData(QString::fromLatin1(sub::app::kDeviceMime), QByteArray("[\"utility\"]"));
        const QPointF onThird(40, h_->row(2).top - arrangement()->scrollY() + 10);
        QVERIFY(lanes()->dragOver(&utility, onThird));
        QVERIFY(lanes()->drop(&utility, onThird));
        QCOMPARE(project().tracks()[2].devices.size(), size_t(1));
        QCOMPARE(project().tracks()[2].devices[0].kind, QStringLiteral("utility"));
        QCOMPARE(selection().trackId(), trackId(2));  // shows its devices
        // An instrument dropped on an audio track is refused; below the tracks it makes a MIDI track.
        QMimeData synth;
        synth.setData(QString::fromLatin1(sub::app::kDeviceMime), QByteArray("[\"synth\"]"));
        QSignalSpy messages(&session(), &sub::app::Session::statusMessage);
        const QPointF onFirst(40, h_->row(0).top - arrangement()->scrollY() + 10);
        lanes()->drop(&synth, onFirst);
        QVERIFY(project().tracks()[0].devices.empty());
        QVERIFY(!messages.isEmpty() && messages.last().first().toString().contains(QStringLiteral("MIDI")));
        const QPointF below(40, arrangement()->totalHeight() - arrangement()->scrollY() + 20);
        QVERIFY(!lanes()->dragOver(&utility, below));  // an effect needs a track
        QVERIFY(lanes()->dragOver(&synth, below));
        QVERIFY(lanes()->drop(&synth, below));
        QCOMPARE(project().tracks().size(), size_t(4));
        QVERIFY(project().tracks()[3].isMidi());
        QCOMPARE(project().tracks()[3].devices[0].kind, QStringLiteral("synth"));
        // Devices dragged from a track's chain: onto another track's row, moved there.
        const QString from = trackId(2), deviceId = project().tracks()[2].devices[0].id;
        QMimeData moved;
        moved.setData(QString::fromLatin1(sub::app::kDeviceMoveMime), sub::app::movedDevicesData(from, {deviceId}));
        QVERIFY(!lanes()->dragOver(&moved, onThird));  // its own track
        QVERIFY(lanes()->dragOver(&moved, onFirst));
        QVERIFY(lanes()->drop(&moved, onFirst));
        QVERIFY(project().tracks()[2].devices.empty());
        QCOMPARE(project().tracks()[0].devices[0].id, deviceId);
        QCOMPARE(selection().trackId(), trackId(0));
    }

    void insertingAMidiClip() {
        const QString first = session().insertMidiTrack();
        const QString second = session().insertMidiTrack();
        h_->settle();
        QSignalSpy opened(session().arrangement(), &sub::app::ArrangementActions::clipViewRequested);
        // Double-clicking empty space makes no clip.
        test::click(window(), h_->lane(0, 4.5));
        test::doubleClick(window(), h_->lane(0, 4.5));
        QVERIFY(spans(first).empty());
        QCOMPARE(opened.count(), 0);
        // Insert MIDI Clip inside a time selection fills it, and opens it in the piano roll.
        test::drag(window(), h_->lane(0, 2.0), h_->lane(0, 7.0));
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{2.0, 7.0, {first}}));
        QVERIFY(lanes()->insertMidiClip(first, h_->lanePoint(0, 4.5).x()));
        QCOMPARE(spans(first), (std::vector<Span>{{2, 7}}));
        QCOMPARE(opened.count(), 1);
        QCOMPARE(opened.last().at(1).toString(), first);
        // Outside a selection: a bar at the grid line (here from an empty lane's menu).
        arr::MenuEntries menu = lanes()->contextMenu(h_->lanePoint(1, 0.5));
        QVERIFY(menu.triggerText(QStringLiteral("Insert MIDI Clip")));
        QCOMPARE(spans(second), (std::vector<Span>{{0, 4}}));
        QCOMPARE(opened.count(), 2);
        QCOMPARE(opened.last().at(2).toString(), clipOf(1).id);
        QCOMPARE(selection().clips(), refs({{second, clipOf(1).id}}));
    }

    void midiClipsInTheArrangement() {
        const QString track = session().insertMidiTrack();
        h_->settle();
        const auto ref = lanes()->insertMidiClip(track, h_->lanePoint(0, 4.5).x());
        QVERIFY(ref);
        editor().setClipNotes(*ref, {{60, 0.0, 1.0, 100}, {64, 2.0, 1.0, 100}}, QStringLiteral("setup"));
        h_->settle();
        QVERIFY(!window()->grabWindow().isNull());  // draws the notes inside the clip
        // Split at beat 6: each piece plays its own note; the notes stay in both.
        selection().selectClips(editor(), {*ref});
        selection().setInsert(6.0);
        session().split();
        QCOMPARE(project().track(track).clips.size(), size_t(2));
        // Ctrl+J joins the two pieces back into one clip, which plays both notes, as one undo step.
        selection().selectClips(editor(), {{track, clipOf(0, 0).id}, {track, clipOf(0, 1).id}});
        arr::MenuEntries menu = lanes()->contextMenu(h_->lanePoint(0, 5.0, true));
        QVERIFY(menu.find(QStringLiteral("Consolidate"))->enabled);
        QVERIFY(!menu.find(QStringLiteral("Reverse"))->enabled);  // no audio
        QVERIFY(menu.triggerText(QStringLiteral("Consolidate")));
        QCOMPARE(project().track(track).clips.size(), size_t(1));
        QCOMPARE(spans(track), (std::vector<Span>{{4, 8}}));
        QCOMPARE(clipOf(0).playedNotes().size(), size_t(2));
        QCOMPARE(undo().undoText(), QStringLiteral("Consolidate"));
        QCOMPARE(selection().clips(), refs({{track, clipOf(0).id}}));
        undo().undo();
        // An audio clip can't be dragged onto the MIDI track.
        const QString path = test::writeWav(dir_->path(QStringLiteral("a.wav")), test::tone(1.0, 220.0), 2);
        const auto audio = editor().addClips(QString(), 0.0, {{path, 1.0}}, 1);
        QCOMPARE(audio.size(), 1);
        h_->settle();
        const QPoint start = h_->lane(1, 0.5, true);
        test::drag(window(), start, start - QPoint(0, h_->row(0).height()));
        QVERIFY(project().findClip(audio[0].trackId, audio[0].clipId));  // still on its audio track
    }

    void rReversesTheSelectedAudioClipsAndAgainPutsThemBack() {
        test::ScopedEnv recordings("SUBSTATION_RECORDINGS", dir_->path(QStringLiteral("Recordings")));
        std::vector<float> ramp(size_t(2 * test::kSampleRate) * 2);
        for (size_t i = 0; i < ramp.size() / 2; ++i) ramp[2 * i] = ramp[2 * i + 1] = float(i) / float(2 * test::kSampleRate);
        const QString path = test::writeWav(dir_->path(QStringLiteral("ramp.wav")), ramp, 2);
        const auto added = editor().addClips(QString(), 0.0, {{path, 2.0}});
        QVERIFY(waitForSource(path));
        const QString track = added[0].trackId, clipId = added[0].clipId;
        selection().selectClips(editor(), {added[0]});
        session().arrangement()->setReverseInPlaceSeconds(1000.0);
        // The clip's menu reverses it (as R does).
        arr::MenuEntries menu = lanes()->contextMenu(h_->lanePoint(0, 1.0, true));
        QVERIFY(menu.find(QStringLiteral("Reverse"))->enabled);
        QVERIFY(menu.triggerText(QStringLiteral("Reverse")));
        Clip clip = clipOf(0);
        QCOMPARE(QFileInfo(clip.path).fileName(), QStringLiteral("ramp R.wav"));  // a reversed copy, as Ableton makes
        QCOMPARE(clip.id, clipId);
        QCOMPARE(clip.reversedFrom, path);
        QCOMPARE(undo().undoText(), QStringLiteral("Reverse Clip"));
        // Again: it plays its own file again (forwards).
        session().reverseClips();
        clip = clipOf(0);
        QCOMPARE(clip.path, path);
        QVERIFY(clip.reversedFrom.isEmpty());
        // A time range over part of it: just that part, split off and reversed (from the same copy).
        undo().undo();
        undo().undo();
        selection().setTimeRange(1.0, 2.0, {track}, QSet<ClipRef>());
        session().reverseClips();
        QCOMPARE(project().track(track).clips.size(), size_t(3));
        QCOMPARE(QFileInfo(clipOf(0, 1).path).fileName(), QStringLiteral("ramp R.wav"));
        QCOMPARE(QDir(dir_->path(QStringLiteral("Recordings/Reversed"))).entryList(QDir::Files).size(), 1);
        QVERIFY(std::abs(clipOf(0, 1).offsetSec - 1.0) < 1e-9);  // it played seconds 0.5-1: 1-1.5 of the copy
        // The menu's Reverse follows the selected area: with the clip back in it (deleted, then undone).
        undo().undo();
        selection().setTimeRange(1.0, 2.0, {track}, refs({{track, clipId}}));
        session().deleteSelection();
        undo().undo();
        QVERIFY(selection().clipRange() && selection().clips().isEmpty());
        menu = lanes()->contextMenu(QPointF(h_->x(1.5), h_->row(0).top + h_->row(0).mainHeight - 4));
        QVERIFY(menu.find(QStringLiteral("Reverse")) && menu.find(QStringLiteral("Reverse"))->enabled);
    }

    void reversingNeedsAudioClips() {
        const QString track = editor().addMidiTrack();
        const auto ref = editor().addMidiClip(track, 0.0, 4.0);
        selection().selectClips(editor(), {*ref});
        QSignalSpy messages(&session(), &sub::app::Session::statusMessage);
        session().reverseClips();
        QCOMPARE(messages.count(), 1);
        QCOMPARE(messages.first().first().toString(), QStringLiteral("There are no audio clips in the selection to reverse."));
        QCOMPARE(undo().undoText(), QStringLiteral("Insert MIDI Clip"));
    }

    void theMenuDeactivatesClipsAndActivatesThem() {
        threeTracks();
        // The clip's menu deactivates it (as 0 does); then it offers Activate.
        arr::MenuEntries menu = lanes()->contextMenu(h_->lanePoint(0, 1.2, true));
        QCOMPARE(menu.find(QStringLiteral("Deactivate"))->shortcut, QStringLiteral("0"));
        QVERIFY(menu.find(QStringLiteral("Deactivate"))->enabled);
        QVERIFY(menu.triggerText(QStringLiteral("Deactivate")));
        QVERIFY(clipOf(0).muted);
        QCOMPARE(undo().undoText(), QStringLiteral("Deactivate Clip"));
        menu = lanes()->contextMenu(h_->lanePoint(0, 1.2, true));
        QVERIFY(!menu.find(QStringLiteral("Deactivate")));
        QVERIFY(menu.triggerText(QStringLiteral("Activate")));
        QVERIFY(!clipOf(0).muted);
        // A time selection with no clips in it: nothing to deactivate.
        selection().setTimeRange(40.0, 44.0, {trackId(0)}, QSet<ClipRef>());
        menu = lanes()->contextMenu(QPointF(h_->x(42.0), h_->row(0).top + h_->row(0).mainHeight - 4));
        QVERIFY(menu.find(QStringLiteral("Deactivate")) && !menu.find(QStringLiteral("Deactivate"))->enabled);
    }

    void clipGainMakesTheWaveformTaller() {
        std::vector<float> square(size_t(test::kSampleRate) * 2);
        for (size_t i = 0; i < square.size() / 2; ++i) square[2 * i] = square[2 * i + 1] = (i % 2) ? 0.25f : -0.25f;
        const QString path = test::writeWav(dir_->path(QStringLiteral("quiet.wav")), square, 2);
        const auto added = editor().addClips(QString(), 0.0, {{path, 1.0}});
        QVERIFY(waitForSource(path));
        const sub::app::Waveform source = bridge().waveform(path);
        // A band from -0.25 to 0.25: a quarter of the lane each way; twice as loud, twice as tall;
        // too loud for the lane, cut off at its edges.
        const auto drawnRows = [&](double gain) {
            const auto tile = arr::WaveformCache::renderTile(source, 100.0, 0, 64, false, gain);
            return tile ? tile->bottoms[0][17] - tile->tops[0][17] : 0.0f;  // (column 16's centre)
        };
        QVERIFY(std::abs(drawnRows(1.0) - 16) <= 2);
        QVERIFY(std::abs(drawnRows(2.0) - 32) <= 2);
        QVERIFY(drawnRows(8.0) >= 62);
        // Drawn at 0.1 dB steps: the same tiles for gains that look the same, a quiet one not flat.
        QCOMPARE(arr::quantizedGain(1.004), arr::quantizedGain(1.0));
        QVERIFY(std::abs(arr::quantizedGain(1.0) - 1.0) < 1e-9);
        QVERIFY(std::abs(arr::quantizedGain(std::pow(10.0, -60 / 20.0)) - 0.001) < 1e-6);
        QCOMPARE(arr::quantizedGain(0.0), 0.0);
        // The arrangement draws it with its gain.
        editor().updateClips({added[0]}, [](const Clip& c) {
            Clip louder = c;
            louder.gainDb = 6.0;
            return louder;
        }, QStringLiteral("Change Clip Gain"));
        h_->settle();
        QVERIFY(!window()->grabWindow().isNull());
        QVERIFY(lanes()->waveforms().size() > 0);
    }

    void aWaveformIsOutlinedAsAbletonDrawsIt() {
        // A click in silence: the outline through the columns' centres, each
        // the minimum and maximum of two columns' worth of frames about it (the
        // click's column and the one before it reach it, at its full height:
        // never averaged down), and the line through silence 1.5 pixels thick
        // about the middle of a pixel.
        std::vector<float> click(size_t(test::kSampleRate) * 2);
        const size_t at = 10000;  // frame 10000: column 100 at 100 frames a pixel
        click[2 * at] = click[2 * at + 1] = 0.9f;
        const QString path = test::writeWav(dir_->path(QStringLiteral("click.wav")), click, 2);
        editor().addClips(QString(), 0.0, {{path, 1.0}});
        QVERIFY(waitForSource(path));
        const sub::app::Waveform source = bridge().waveform(path);
        const auto tile = arr::WaveformCache::renderTile(source, 100.0, 0, 64, false);
        QVERIFY(tile);
        QCOMPARE(tile->count, arr::WaveformCache::kTile + 2);
        // (point x + 1 is column x's centre)
        const auto top = [&](int x) { return tile->tops[0][size_t(x + 1)]; };
        const auto bottom = [&](int x) { return tile->bottoms[0][size_t(x + 1)]; };
        const float peak = 32.5f - 0.9f * 31.0f;
        QVERIFY2(std::abs(top(100) - peak) < 0.5f, qPrintable(QString::number(top(100))));
        QCOMPARE(top(99), top(100));
        for (int x : {97, 98, 101, 102}) {
            QCOMPARE(top(x), 31.75f);
            QCOMPARE(bottom(x), 33.25f);
        }
        // The last point is the centre of the last column with its centre in the source.
        const int columns = int(std::ceil(double(source.frames()) / 100.0 - 0.5));
        const int lastTile = (columns - 1) / arr::WaveformCache::kTile;
        const auto last = arr::WaveformCache::renderTile(source, 100.0, lastTile, 64, false);
        QVERIFY(last);
        QCOMPARE(last->count, columns - lastTile * arr::WaveformCache::kTile + 1);
        QVERIFY(!arr::WaveformCache::renderTile(source, 100.0, lastTile + 1, 64, false));
    }

    void aDraggedClipIsHeardWhereItGoesBeforeItIsDropped() {
        const QString path = test::writeWav(dir_->path(QStringLiteral("dc.wav")), test::constant(2.0, 0.5f), 2);
        const auto added = editor().addClips(QString(), 0.0, {{path, 2.0}});  // beats 0-4
        QVERIFY(waitForSource(path));
        h_->settle();
        const QString track = added[0].trackId;
        const double titleY = h_->row(0).top - arrangement()->scrollY() + 6;
        const auto heardAt = [&](double beat) {
            const std::vector<float> out = ui_->engine().renderOffline(0.0, 10 * kSamplesPerBeat);
            return out[size_t(2 * int(beat * kSamplesPerBeat))];
        };
        const QPoint start = h_->at(QPointF(h_->x(1.0), titleY));
        test::press(window(), start);
        test::moveTo(window(), start + QPoint(10, 0));
        test::moveTo(window(), h_->at(QPointF(h_->x(5.0), titleY)));  // four beats on
        QCOMPARE(clipOf(0).startBeat, 0.0);  // the model waits for the drop...
        QCOMPARE(heardAt(1.0), 0.0f);  // ...the engine plays it there now
        QVERIFY(std::abs(heardAt(6.0) - 0.5f) < 1e-3);
        // Back where it was, and dropped: nothing changed, and it plays there again.
        test::moveTo(window(), start);
        test::release(window(), start);
        QCOMPARE(undo().undoText(), QStringLiteral("Add Clip"));
        QVERIFY(std::abs(heardAt(1.0) - 0.5f) < 1e-3);
        QCOMPARE(heardAt(6.0), 0.0f);

        test::drag(window(), start, h_->at(QPointF(h_->x(5.0), titleY)));
        QCOMPARE(clipOf(0).startBeat, 4.0);
        QCOMPARE(heardAt(1.0), 0.0f);
        QVERIFY(std::abs(heardAt(6.0) - 0.5f) < 1e-3);
        undo().undo();
        QVERIFY(std::abs(heardAt(1.0) - 0.5f) < 1e-3);

        // Trimming too: the clip plays trimmed while its edge is dragged.
        const QPoint edge = h_->at(QPointF(h_->x(4.0) - 2, titleY));
        test::press(window(), edge);
        test::moveTo(window(), h_->at(QPointF(h_->x(2.0), titleY)));
        QVERIFY(std::abs(heardAt(1.0) - 0.5f) < 1e-3);
        QCOMPARE(heardAt(3.0), 0.0f);
        test::release(window(), h_->at(QPointF(h_->x(2.0), titleY)));
        QCOMPARE(clipOf(0).endBeat(project().tempo()), 2.0);
        Q_UNUSED(track);
    }

    void aTimeSelectionOverAGroupTakesInItsTracks() {
        QStringList ids;
        for (const char* name : {"A", "B", "C"}) {
            const QString id = editor().addAudioTrack(-1, QString::fromLatin1(name));
            editor().commitClips(QStringLiteral("Add"),
                                 {{id, {Clip::audio(QStringLiteral("c") + QString::number(ids.size()), QStringLiteral("missing.wav"),
                                                    QStringLiteral("x"), 0.0, 2.0, 0.0, 2.0)}}});
            ids << id;
        }
        const QString group = editor().groupTracks({ids[0], ids[1]});
        editor().setFolded(group, true);  // (its tracks hidden: still in it)
        h_->settle();
        const auto& row = h_->rowOf(group);
        const double y = row.top - arrangement()->scrollY() + row.mainHeight / 2;
        // Dragged along the group's lane, a selection takes in everything in the group.
        test::drag(window(), h_->at(QPointF(h_->x(1.0), y)), h_->at(QPointF(h_->x(3.0), y)));
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{1.0, 3.0, {group, ids[0], ids[1]}}));
        QCOMPARE(selection().clips(), refs({{ids[0], QStringLiteral("c0")}, {ids[1], QStringLiteral("c1")}}));
        test::screenshot(window(), QStringLiteral("arrangement_folded_group_selection"));
        session().deleteSelection();
        for (const QString& id : {ids[0], ids[1]}) QCOMPARE(spans(id), (std::vector<Span>{{0, 1}, {3, 4}}));
        QCOMPARE(spans(ids[2]), (std::vector<Span>{{0, 4}}));
        undo().undo();
        // Copy and paste it further on: onto the group's tracks again.
        session().copy();
        selection().setInsert(8.0);
        session().paste();
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{8.0, 10.0, {ids[0], ids[1]}}));
        for (const QString& id : {ids[0], ids[1]}) QCOMPARE(spans(id).back().first, 8.0);
        undo().undo();
        // Ctrl+A: from the first clip to the last, on every track.
        session().selectAll();
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{0.0, 4.0, {group, ids[0], ids[1], ids[2]}}));
    }

    void aTakeIsDrawnUpToThePlayhead() {
        const QString track = editor().addAudioTrack();
        h_->settle();
        const auto& row = h_->rowOf(track);
        const double y = row.top - arrangement()->scrollY() + 6;  // its red title bar
        // Half a beat has come in (the input lags): the take still reaches the playhead, at beat 2.
        sub::app::LiveTake take;
        take.trackId = track;
        take.started = true;
        take.frames = kSamplesPerBeat / 2;
        take.peaks = std::vector<float>(size_t(take.frames / sub::app::LiveTake::kPeakFrames) * 2, 0.0f);
        for (size_t i = 0; i < take.peaks.size(); i += 2) {
            take.peaks[i] = -0.5f;
            take.peaks[i + 1] = 0.5f;
        }
        h_->liveTakes()->setTakes(QMap<QString, sub::app::LiveTake>{{track, take}});
        bridge().play();
        arrangement()->onPosition(2.0);
        h_->settle();
        const QImage image = window()->grabWindow();
        QCOMPARE(image.pixelColor(h_->at(QPointF(h_->x(1.5), y))), sub::ui::Theme::recordOn());
        QVERIFY(image.pixelColor(h_->at(QPointF(h_->x(2.5), y))) != sub::ui::Theme::recordOn());
        test::screenshot(window(), QStringLiteral("arrangement_live_take"), QRect(0, 0, 400, 200));
        // A MIDI take's notes so far (a held one reaches the take's end).
        take.midi = true;
        take.peaks.clear();
        take.notes = {{0, kSamplesPerBeat / 4, 60, 100, 0}, {kSamplesPerBeat / 2, -1, 64, 90, 0}};
        h_->liveTakes()->setTakes(QMap<QString, sub::app::LiveTake>{{track, take}});
        h_->settle();
        QVERIFY(!window()->grabWindow().isNull());
        bridge().stop();
        h_->liveTakes()->setTakes(std::nullopt);
        arrangement()->onPosition(0.0);
        QVERIFY(!arrangement()->playhead());
    }

    void theLanesMenus() {
        threeTracks();
        // An empty part of a lane: Paste (at that point, enabled once something was copied),
        // Insert Audio Track and Insert MIDI Track (after it, in its group), Delete Track.
        const QPointF empty = h_->lanePoint(0, 20.0);
        arr::MenuEntries menu = lanes()->contextMenu(empty);
        QCOMPARE(menu.texts(), (QStringList{QStringLiteral("Paste"), QString(), QStringLiteral("Insert Audio Track"),
                                            QStringLiteral("Insert MIDI Track"), QStringLiteral("Delete Track")}));
        QVERIFY(!menu.find(QStringLiteral("Paste"))->enabled);
        QVERIFY(menu.triggerText(QStringLiteral("Insert MIDI Track")));
        QCOMPARE(project().tracks().size(), size_t(4));
        QVERIFY(project().tracks()[1].isMidi());  // after it
        // A MIDI track's lane offers Insert MIDI Clip.
        h_->settle();
        menu = lanes()->contextMenu(h_->lanePoint(1, 20.0));
        QVERIFY(menu.find(QStringLiteral("Insert MIDI Clip")));
        // A clip's: what acts on it (selected first), and Split Here at the snapped beat under the mouse.
        menu = lanes()->contextMenu(h_->lanePoint(0, 1.2, true));
        QCOMPARE(menu.texts(),
                 (QStringList{QStringLiteral("Cut"), QStringLiteral("Copy"), QStringLiteral("Paste"), QString(),
                              QStringLiteral("Split Here"), QStringLiteral("Duplicate"), QStringLiteral("Consolidate"),
                              QStringLiteral("Reverse"), QStringLiteral("Deactivate"), QString(),
                              QStringLiteral("Find Similar Sounds"), QStringLiteral("Hot-Swap Sample"),
                              QStringLiteral("Show in File Manager"), QString(), QStringLiteral("Delete")}));
        QCOMPARE(selection().clips(), refs({{trackId(0), clipOf(0).id}}));
        QCOMPARE(menu.find(QStringLiteral("Duplicate"))->shortcut, QStringLiteral("Ctrl+D"));
        // Find Similar Sounds: the browser lists the sounds most like the clip's.
        QVERIFY(menu.find(QStringLiteral("Find Similar Sounds"))->enabled);
        QVERIFY(menu.triggerText(QStringLiteral("Find Similar Sounds")));
        QCOMPARE(session().browser()->similarTo(), clipOf(0).path);
        session().browser()->clearSimilar();
        QCOMPARE(session().browser()->similarTo(), QString());
        // Hot-Swap Sample: every clip playing its file, the browser listing the sounds most like it.
        QVERIFY(menu.triggerText(QStringLiteral("Hot-Swap Sample")));
        sub::app::HotSwap& hotSwap = *session().hotSwap();
        QVERIFY(hotSwap.active());
        QCOMPARE(hotSwap.uses().clips, (sub::app::ClipRefs{{trackId(0), clipOf(0).id}}));
        QCOMPARE(session().browser()->similarTo(), clipOf(0).path);
        hotSwap.stop();
        session().browser()->clearSimilar();
        // Show in File Manager: the panel is asked to show its row.
        QSignalSpy revealed(session().files(), &sub::app::FileManager::revealRequested);
        QVERIFY(menu.triggerText(QStringLiteral("Show in File Manager")));
        QCOMPARE(revealed.count(), 1);
        QVERIFY(menu.triggerText(QStringLiteral("Split Here")));
        QCOMPARE(spans(trackId(0)), (std::vector<Span>{{0, 1}, {1, 4}}));
        // Copy (the area still selected: both pieces), then paste where the empty lane was right-clicked.
        menu = lanes()->contextMenu(h_->lanePoint(0, 0.5, true));
        QVERIFY(menu.triggerText(QStringLiteral("Copy")));
        menu = lanes()->contextMenu(h_->lanePoint(0, 20.0));
        QVERIFY(menu.find(QStringLiteral("Paste"))->enabled);
        QVERIFY(menu.triggerText(QStringLiteral("Paste")));
        QCOMPARE(spans(trackId(0)), (std::vector<Span>{{0, 1}, {1, 4}, {20, 21}, {21, 24}}));
        // Delete Track.
        menu = lanes()->contextMenu(h_->lanePoint(0, 30.0));
        QVERIFY(menu.triggerText(QStringLiteral("Delete Track")));
        QCOMPARE(project().tracks().size(), size_t(3));
        // The menu shows (as ArrangementMenu.qml), and its entries run.
        QSignalSpy requested(lanes(), &ArrangementLanes::menuRequested);
        QTest::mouseClick(window(), Qt::RightButton, {}, h_->lane(0, 30.0));
        QCOMPARE(requested.count(), 1);
        QObject* popup = h_->view()->findChild<QObject*>(QStringLiteral("arrangementMenu"));
        QVERIFY(popup);
        QTRY_VERIFY(popup->property("visible").toBool());
        QCOMPARE(popup->property("count").toInt(), 7);  // a MIDI track's: Paste, Insert MIDI Clip, the inserts, Delete Track
        test::screenshot(window(), QStringLiteral("arrangement_lane_menu"));
        QMetaObject::invokeMethod(popup, "close");
        QTRY_VERIFY(!popup->property("visible").toBool());
    }
};

QTEST_MAIN(TestUiArrangement)
#include "test_ui_arrangement.moc"
