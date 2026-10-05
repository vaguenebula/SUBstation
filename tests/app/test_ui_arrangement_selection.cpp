// Time selections in the arrangement, driven with the mouse on the lanes: a
// group selected acts on what is in it but shows over the rows it covers; a
// drag (or a Shift-click) started in a track's own lane selects clips over
// every track it crosses, folded, frozen, groups and what is in them; stretches
// of frozen tracks dragged, with their frozen audio, and what is refused there.
// Runs on a display (xvfb here). With $SUBSTATION_SCREENS set, it saves
// screenshots there.

#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <memory>

#include "ArrangementTestSupport.h"
#include "model/Clip.h"
#include "session/ArrangementActions.h"

using sub::app::Clip;
using sub::app::ClipRef;
using sub::app::TimeRange;
namespace test = sub::app::test;
namespace arr = sub::ui::arrangement;

class TestUiArrangementSelection : public QObject {
    Q_OBJECT

    std::unique_ptr<test::UiSession> ui_;
    std::unique_ptr<test::ArrangementHarness> h_;

    sub::app::Session& session() { return ui_->session(); }
    sub::app::Project& project() { return *session().project(); }
    sub::app::ProjectEditor& editor() { return *session().editor(); }
    sub::app::Selection& selection() { return *session().selection(); }
    QUndoStack& undo() { return *session().undoStack(); }
    QQuickWindow* window() { return h_->window(); }
    sub::ui::Arrangement* arrangement() { return h_->arrangement(); }

    // An audio track named `name` with a 2 s clip ("<name>0", of a missing
    // file: 4 beats) at `start`.
    QString track(const QString& name, double start) {
        const QString id = editor().addAudioTrack(-1, name);
        Clip clip = Clip::audio(name + QStringLiteral("0"), QStringLiteral("missing.wav"), name, start, 2.0, 0.0, 2.0);
        editor().commitClips(QStringLiteral("Add"), {{id, {clip}}});
        return id;
    }
    // A point of a track's row: `y` px down from its top (< 0: its middle), at a beat.
    QPoint at(const QString& trackId, double beat, double y = -1) {
        const arr::Row& row = h_->rowOf(trackId);
        const double top = row.top - arrangement()->scrollY();
        return h_->at(QPointF(h_->x(beat), top + (y < 0 ? row.mainHeight / 2.0 : y)));
    }
    void drag(QPoint from, QPoint to, Qt::KeyboardModifiers modifiers = {}) {
        test::drag(window(), from, to, modifiers);
        h_->settle();
    }
    void click(QPoint point, Qt::KeyboardModifiers modifiers = {}) {
        test::click(window(), point, modifiers);
        h_->settle();
    }
    QColor pixel(QPoint point) {
        h_->settle();
        return window()->grabWindow().pixelColor(point);
    }
    std::vector<std::pair<double, double>> spans(const QString& trackId) {
        std::vector<std::pair<double, double>> out;
        for (const Clip& c : project().track(trackId).clips) {
            out.emplace_back(std::round(c.startBeat * 1e6) / 1e6, std::round(c.endBeat(project().tempo()) * 1e6) / 1e6);
        }
        return out;
    }
    std::vector<std::pair<double, double>> frozenSpans(const QString& trackId) {
        std::vector<std::pair<double, double>> out;
        const sub::app::Track& t = project().track(trackId);
        for (const Clip& c : t.frozen->playing(trackId)) {
            out.emplace_back(std::round(c.startBeat * 1e6) / 1e6, std::round(c.endBeat(project().tempo()) * 1e6) / 1e6);
        }
        return out;
    }

private Q_SLOTS:
    void initTestCase() {
        test::prepareApplication();
        if (!test::haveDisplay()) QSKIP("needs a display: Qt Quick's software renderer draws none of this geometry");
        sub::ui::setUpApplication();
        ui_ = std::make_unique<test::UiSession>();
        h_ = std::make_unique<test::ArrangementHarness>(*ui_);
        QVERIFY(h_->show());
    }

    void cleanupTestCase() {
        h_.reset();
        ui_.reset();
    }

    void init() {
        session().bridge()->stop();
        session().newProject();
        undo().clear();
        session().arrangement()->setClipboard({});
        arrangement()->setSnap(true);
        arrangement()->setGridLevel(0);
        h_->setZoom(sub::ui::timeline::Timeline::kDefaultPxPerBeat);
        arrangement()->setScrollBeats(0.0);
        arrangement()->setScrollY(0);
        test::moveTo(window(), QPoint(2, 2), {}, Qt::NoButton);
        h_->settle();
    }

    void aGroupSelectedShowsOverItsRowButActsOnWhatIsInIt() {
        const QString a = track("A", 0.0), b = track("B", 4.0), c = track("C", 0.0);
        const QString group = editor().groupTracks({a, b});
        h_->settle();
        const QPoint inA = at(a, 2.0), inB = at(b, 2.0), inGroup = at(group, 2.0);
        const QColor a0 = pixel(inA), b0 = pixel(inB), group0 = pixel(inGroup);
        // Along the group's row: the group and what is in it, shown over its row.
        drag(at(group, 1.0), at(group, 3.0));
        QCOMPARE(selection().timeRange(), (TimeRange{1.0, 3.0, {group, a, b}}));
        QCOMPARE(selection().rangeRows(), QStringList{group});
        QCOMPARE(selection().clips(), (QSet<ClipRef>{{a, QStringLiteral("A0")}}));
        test::screenshot(window(), QStringLiteral("arrangement_group_row_selected"));
        QVERIFY(pixel(inGroup) != group0);  // tinted
        QCOMPARE(pixel(inA), a0);  // not: the drag didn't reach its row
        // Grabbed on its row, it moves, with what is in the group.
        drag(at(group, 2.0, 5), at(group, 10.0, 5));
        QCOMPARE(selection().timeRange(), (TimeRange{9.0, 11.0, {group, a, b}}));
        QCOMPARE(selection().rangeRows(), QStringList{group});
        QCOMPARE(spans(a), (std::vector<std::pair<double, double>>{{0, 1}, {3, 4}, {9, 11}}));
        undo().undo();
        // On a row it doesn't show over, a press in the clips' band is outside
        // it: a new selection, nothing moves.
        selection().clear();
        drag(at(group, 9.0), at(group, 11.0));
        const int steps = undo().count();
        drag(at(a, 10.0, 5), at(a, 12.0, 5));
        QCOMPARE(selection().timeRange(), (TimeRange{10.0, 12.0, {a}}));
        QCOMPARE(undo().count(), steps);
        // Down from the group's row to its first track: shown over both, acting on all.
        drag(at(group, 1.0), at(a, 3.0));
        QCOMPARE(selection().rangeTrackIds(), (QStringList{group, a, b}));
        QCOMPARE(selection().rangeRows(), (QStringList{group, a}));
        QVERIFY(pixel(inA) != a0);
        QCOMPARE(pixel(inB), b0);
        // Delete acts on all of it.
        session().deleteSelection();
        QCOMPARE(spans(a), (std::vector<std::pair<double, double>>{{0, 1}, {3, 4}}));
        QCOMPARE(selection().rangeRows(), (QStringList{group, a}));  // (still shown where it was)
        QCOMPARE(spans(c), (std::vector<std::pair<double, double>>{{0, 4}}));
    }

    void aDragFromATracksLaneSelectsClipsOnEveryTrackItCrosses() {
        // Folded, frozen, groups and what is in them (folded away or not).
        const QString x = track("X", 0.0);
        const QString folded = track("F", 0.0);
        const QString frozen = track("Z", 0.0);
        const QString inner = track("H", 0.0);
        const QString group = editor().groupTracks({inner});
        const QString last = track("L", 0.0);
        editor().setFolded(folded, true);
        editor().setFolded(group, true);
        editor().freezeTracks({{frozen, sub::app::Freeze{QStringLiteral("frozen.wav"), 8.0, 120.0}}});
        h_->settle();
        drag(at(x, 1.0), at(last, 3.0));
        QCOMPARE(selection().timeRange(), (TimeRange{1.0, 3.0, {x, folded, frozen, group, inner, last}}));
        QVERIFY(selection().clipRange());
        QCOMPARE(selection().clips().size(), 5);
        test::screenshot(window(), QStringLiteral("arrangement_selection_over_every_kind"));
        // Up from the bottom, the same.
        drag(at(last, 3.0), at(x, 1.0));
        QCOMPARE(selection().timeRange(), (TimeRange{1.0, 3.0, {x, folded, frozen, group, inner, last}}));
        // Started on the folded group's row, its hidden track comes along.
        selection().clear();
        drag(at(group, 1.0), at(group, 3.0));
        QCOMPARE(selection().timeRange(), (TimeRange{1.0, 3.0, {group, inner}}));
        // Delete acts on every one of them, the frozen track's frozen audio too.
        drag(at(x, 1.0), at(last, 3.0));
        session().deleteSelection();
        for (const QString& id : {x, folded, frozen, inner, last}) {
            QCOMPARE(spans(id), (std::vector<std::pair<double, double>>{{0, 1}, {3, 4}}));
        }
        QCOMPARE(frozenSpans(frozen), (std::vector<std::pair<double, double>>{{0, 1}, {3, 16}}));
    }

    void shiftClickExtendsAClipRangeOverEverythingInThatDirection() {
        const QString a = track("A", 0.0), b = track("B", 0.0), c = track("C", 0.0);
        const QString inner = track("H", 0.0);
        const QString group = editor().groupTracks({inner});
        h_->settle();
        const int steps = undo().count();
        drag(at(b, 2.0), at(b, 3.0));
        QCOMPARE(selection().timeRange(), (TimeRange{2.0, 3.0, {b}}));
        // Down to the group's row: the tracks between, and what is in the group.
        click(at(group, 5.0), Qt::ShiftModifier);
        QCOMPARE(selection().timeRange(), (TimeRange{2.0, 5.0, {b, c, group, inner}}));
        QCOMPARE(selection().rangeRows(), (QStringList{b, c, group}));
        QVERIFY(selection().clipRange());
        QCOMPARE(selection().insertBeat(), 2.0);
        // Then up to the top, and back in time.
        click(at(a, 1.0), Qt::ShiftModifier);
        QCOMPARE(selection().timeRange(), (TimeRange{1.0, 5.0, {a, b, c, group, inner}}));
        QCOMPARE(selection().clips().size(), 4);
        // A Shift-drag goes on extending it while it moves.
        drag(at(a, 6.0), at(a, 7.0), Qt::ShiftModifier);
        QCOMPARE(selection().timeRange(), (TimeRange{1.0, 7.0, {a, b, c, group, inner}}));
        // From just the insert marker (a click on a track), a Shift-click selects from there.
        click(at(c, 3.0));
        QVERIFY(!selection().timeRange());
        click(at(a, 1.0), Qt::ShiftModifier);
        QCOMPARE(selection().timeRange(), (TimeRange{1.0, 3.0, {a, b, c}}));
        QCOMPARE(undo().count(), steps);  // (selecting changes nothing)
    }

    void aStretchOfAFrozenTrackDragsWithItsFrozenAudio() {
        const QString a = track("A", 0.0), b = track("B", 8.0);
        editor().freezeTracks({{a, sub::app::Freeze{QStringLiteral("frozen.wav"), 8.0, 120.0}}});
        h_->settle();
        QSignalSpy messages(&session(), &sub::app::Session::statusMessage);
        drag(at(a, 1.0), at(a, 3.0));
        QCOMPARE(selection().timeRange(), (TimeRange{1.0, 3.0, {a}}));
        const int steps = undo().count();
        // Grabbed in the clips' band, 4 beats on: while it drags, the frozen
        // track's clips where it lands are drawn cut away (its audio is replaced there).
        test::press(window(), at(a, 2.0, 5));
        test::moveTo(window(), at(a, 3.0, 5));
        test::moveTo(window(), at(a, 6.0, 5));
        h_->settle();
        test::screenshot(window(), QStringLiteral("arrangement_frozen_stretch_dragged"));
        test::release(window(), at(a, 6.0, 5));
        h_->settle();
        QCOMPARE(selection().timeRange(), (TimeRange{5.0, 7.0, {a}}));
        QCOMPARE(spans(a), (std::vector<std::pair<double, double>>{{0, 1}, {3, 4}, {5, 7}}));
        QCOMPARE(frozenSpans(a), (std::vector<std::pair<double, double>>{{0, 1}, {3, 5}, {5, 7}, {7, 16}}));
        QCOMPARE(undo().count(), steps + 1);
        // Onto another track: refused, and the status line says why.
        messages.clear();
        drag(at(a, 6.0, 5), at(b, 6.0, 5));
        QVERIFY(std::any_of(messages.begin(), messages.end(), [](const QList<QVariant>& message) {
            return message.at(0).toString().startsWith(QStringLiteral("A is frozen: clips can't move"));
        }));
        QCOMPARE(spans(b), (std::vector<std::pair<double, double>>{{8, 12}}));
        QCOMPARE(selection().timeRange(), (TimeRange{5.0, 7.0, {a}}));  // (where it still is)
        QCOMPARE(undo().count(), steps + 1);
        undo().undo();
        QVERIFY(!project().track(a).frozen->segments);
        QCOMPARE(spans(a), (std::vector<std::pair<double, double>>{{0, 4}}));
    }
};

QTEST_MAIN(TestUiArrangementSelection)
#include "test_ui_arrangement_selection.moc"
