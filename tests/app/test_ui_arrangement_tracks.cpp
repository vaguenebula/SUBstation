// The arrangement's tracks and their headers, driven as a user would with the
// mouse on the headers, their controls and the lanes: groups (folding, the
// group's header, dragging headers into and out of groups, folded tracks'
// clips as bars, cut, copy and paste), returns and sends, following the
// playhead while scrolling by hand, the header's controls (volume, solo, the
// activator, arming, renaming in place), the input and monitoring menus with
// resampling and MIDI inputs, sidechains as routing (the send knobs they grey
// out), the track menu's freezing and flattening, resizing a track by its
// bottom edge, and screenshots of a whole project. Runs on a display (xvfb
// here). With $SUBSTATION_SCREENS set, it saves screenshots there.

#include <QFileInfo>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "ArrangementTestSupport.h"
#include "model/Automation.h"
#include "model/Clip.h"
#include "model/Device.h"
#include "session/ArrangementActions.h"
#include "session/RenderProgress.h"
#include "theme/Theme.h"

using sub::app::Clip;
using sub::app::ClipRef;
using sub::app::kMaster;
using sub::app::Selection;
using sub::ui::TrackHeaderItem;
namespace test = sub::app::test;
namespace arr = sub::ui::arrangement;
namespace automation = sub::app::automation;

namespace {

const Qt::KeyboardModifiers kCtrlAlt = Qt::ControlModifier | Qt::AltModifier;

QRectF geometryOf(const QQuickItem* item) { return QRectF(item->position(), item->size()); }

}  // namespace

class TestUiArrangementTracks : public QObject {
    Q_OBJECT

    std::unique_ptr<test::UiSession> ui_;
    std::unique_ptr<test::ArrangementHarness> h_;
    std::unique_ptr<test::TempDir> dir_;
    QStringList tones_;  // two tone files, 2 and 3 s long

    sub::app::Session& session() { return ui_->session(); }
    sub::app::Project& project() { return *session().project(); }
    sub::app::ProjectEditor& editor() { return *session().editor(); }
    Selection& selection() { return *session().selection(); }
    sub::app::EngineBridge& bridge() { return *session().bridge(); }
    QUndoStack& undo() { return *session().undoStack(); }
    QQuickWindow* window() { return h_->window(); }
    sub::ui::ArrangementLanes* lanes() { return h_->lanes(); }
    sub::ui::Arrangement* arrangement() { return h_->arrangement(); }
    TrackHeaderItem* header(const QString& id) { return h_->header(id); }
    QQuickItem* control(const QString& id, const char* name) { return h_->control(header(id), QLatin1String(name)); }

    // Audio tracks A, B, C..., a 2 s clip each (c0, c1...: of a missing file)
    // at beats 0, 4, 8...
    QStringList makeTracks(int count) {
        QStringList ids;
        for (int i = 0; i < count; ++i) {
            const QString id = editor().addAudioTrack(-1, QString(QChar(u'A' + i)));
            Clip clip;
            clip.id = QStringLiteral("c%1").arg(i);
            clip.path = QStringLiteral("missing.wav");
            clip.name = QStringLiteral("x");
            clip.startBeat = i * 4.0;
            clip.durationSec = 2.0;
            clip.sourceDurationSec = 2.0;
            editor().commitClips(QStringLiteral("Add"), {{id, {clip}}});
            ids << id;
        }
        h_->settle();
        return ids;
    }

    QStringList order() {
        QStringList ids;
        for (const auto& t : project().tracks()) ids << t.id;
        return ids;
    }
    QString parentOf(const QString& id) { return project().track(id).parent.value_or(QString()); }

    void click(QPoint at, Qt::KeyboardModifiers modifiers = {}) {
        test::click(window(), at, modifiers);
        h_->settle();
    }
    void clickControl(const QString& id, const char* name, Qt::KeyboardModifiers modifiers = {}) {
        QQuickItem* item = control(id, name);
        QVERIFY2(item, name);
        click(test::centerOf(item), modifiers);
    }
    // A click on a header's fold button.
    void fold(const QString& id) {
        TrackHeaderItem* h = header(id);
        QVERIFY(h);
        click(test::at(h, h->foldRect().center()));
    }
    // A header dragged by its name to `endY` in the header column.
    void dragHeader(const QString& id, double endY) {
        TrackHeaderItem* h = header(id);
        QVERIFY(h);
        auto* column = h_->find<QQuickItem*>(QStringLiteral("headers"));
        QVERIFY(column);
        const QPoint start = test::at(h, QPointF(h->nameLeft() + 8, 8));  // (by its name bar)
        const QPoint end = test::at(column, QPointF(h->nameLeft() + 8, endY));
        test::press(window(), start);
        test::moveTo(window(), start + QPoint(0, 12));
        test::moveTo(window(), end);
        test::release(window(), end);
        h_->settle();
    }
    // A header's send knobs (and their letters), left to right.
    std::vector<QQuickItem*> sendItems(TrackHeaderItem* h, const char* name) {
        std::vector<QQuickItem*> items = test::findAllIn<QQuickItem*>(h, QLatin1String(name));
        std::sort(items.begin(), items.end(),
                  [](QQuickItem* a, QQuickItem* b) { return a->mapToScene(QPointF()).x() < b->mapToScene(QPointF()).x(); });
        return items;
    }
    std::vector<QQuickItem*> knobs(TrackHeaderItem* h) { return sendItems(h, "sendKnob"); }
    // `knob` dragged up by `pixels`.
    void dragKnob(QQuickItem* knob, int pixels) {
        const QPoint start = test::centerOf(knob);
        test::press(window(), start);
        test::moveTo(window(), start - QPoint(0, pixels / 2));
        test::moveTo(window(), start - QPoint(0, pixels));
        test::release(window(), start - QPoint(0, pixels));
        h_->settle();
    }
    // Whether a strip's knob for its first send can be used.
    bool usable(const QString& owner) {
        TrackHeaderItem* h = project().hasReturn(owner) ? h_->returnHeader(owner) : header(owner);
        if (!h) return false;
        const std::vector<QQuickItem*> items = knobs(h);
        const bool enabled = h->sends().value(0).toMap().value(QStringLiteral("enabled")).toBool();
        return !items.empty() && items[0]->isEnabled() && enabled;
    }
    QStringList entryTexts(const arr::MenuEntries& menu) {
        QStringList texts = menu.texts();
        texts.removeAll(QString());
        return texts;
    }
    // Typed into the window (what has the focus), a key at a time.
    void typeText(const QString& text) {
        for (const QChar ch : text) QTest::keyClick(window(), ch.toLatin1());
    }
    QObject* popup() { return h_->view()->findChild<QObject*>(QStringLiteral("arrangementMenu")); }
    void closePopup() {
        QObject* menu = popup();
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("visible").toBool());
    }
    bool waitForSource(const QString& path) {
        bridge().requestSource(path);
        return QTest::qWaitFor([&] { return bridge().source(path) != nullptr; }, 10000);
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
        for (int i = 0; i < 2; ++i) {
            tones_ << test::writeWav(dir_->path(QStringLiteral("tone%1.wav").arg(i)), test::tone(2.0 + i, 220.0 * (i + 1)), 2);
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

    // --- Groups -------------------------------------------------------------------------------

    void groupingTheSelectedTracksAndFoldingHidesThem() {
        const QStringList ids = makeTracks(3);
        const QString a = ids[0], b = ids[1], c = ids[2];
        selection().selectTrack(a, true);
        selection().selectTrack(b, true, Selection::Mode::Toggle);
        session().groupSelected();  // Ctrl+G
        h_->settle();
        const QString group = project().tracks()[0].id;
        QVERIFY(project().track(group).isGroup());
        QCOMPARE(parentOf(a), group);
        QCOMPARE(parentOf(b), group);
        QCOMPARE(parentOf(c), QString());
        QCOMPARE(selection().trackId(), group);
        // The group's header: no arm or input; its tracks indented under it.
        QVERIFY(control(group, "arm") && !control(group, "arm")->isVisible());
        QVERIFY(!control(group, "input")->isVisible());
        QVERIFY(header(a)->indent() > header(group)->indent());
        QCOMPARE(header(a)->depth(), 1);
        QCOMPARE(header(group)->kind(), QStringLiteral("group"));

        // Folding hides its tracks; the tracks below move up.
        const int topOfC = h_->rowOf(c).top;
        fold(group);
        QVERIFY(project().track(group).folded);
        QVERIFY(h_->rowOf(a).hidden && h_->rowOf(b).hidden);
        QVERIFY(!header(a)->isVisible());
        QVERIFY(h_->rowOf(c).top < topOfC);
        QCOMPARE(undo().undoText(), QStringLiteral("Group Tracks"));  // folding isn't undone

        // Clicking in the lanes where the hidden tracks were finds the track below.
        const double y = h_->rowOf(c).top + 5 - arrangement()->scrollY();
        const auto index = lanes()->rowIndexAt(y);
        QVERIFY(index);
        QCOMPARE(h_->row(*index).trackId, c);

        fold(group);
        QVERIFY(header(a)->isVisible());

        // Ctrl+Shift+G ungroups.
        selection().selectTrack(group, true);
        session().ungroupSelected();
        h_->settle();
        QCOMPARE(order(), (QStringList{a, b, c}));
        for (const auto& t : project().tracks()) QVERIFY(!t.parent);
        undo().undo();
        QVERIFY(project().tracks()[0].isGroup());
    }

    void draggingHeadersMovesTracksIntoAndOutOfGroups() {
        const QStringList ids = makeTracks(3);
        const QString a = ids[0], b = ids[1], c = ids[2];
        const QString group = editor().groupTracks({a});
        h_->settle();
        const int scroll = arrangement()->scrollY();

        // Onto the middle of the group's header: into the group, last.
        {
            const arr::Row row = h_->rowOf(group);
            selection().selectTrack(c, true);
            dragHeader(c, row.top + row.mainHeight / 2 - scroll);
        }
        QCOMPARE(order(), (QStringList{group, a, c, b}));
        QCOMPARE(parentOf(c), group);
        QCOMPARE(undo().undoText(), QStringLiteral("Move Track"));

        // Below the last track: out of the group, last.
        dragHeader(a, arrangement()->totalHeight() + 10 - scroll);
        QCOMPARE(order(), (QStringList{group, c, b, a}));
        QCOMPARE(parentOf(a), QString());

        // Dropped amid its own tracks, a group stays where it is.
        {
            const arr::Row row = h_->rowOf(c);
            dragHeader(group, row.top + row.mainHeight - 4 - scroll);
        }
        QCOMPARE(order(), (QStringList{group, c, b, a}));

        undo().undo();
        undo().undo();
        QCOMPARE(order(), (QStringList{group, a, b, c}));
    }

    void whileHeadersAreDraggedALineShowsWhereTheyGo() {
        const QStringList ids = makeTracks(3);
        auto* column = h_->find<QQuickItem*>(QStringLiteral("headers"));
        auto* line = h_->find<QQuickItem*>(QStringLiteral("dropLine"));
        QVERIFY(column && line);
        TrackHeaderItem* h = header(ids[0]);
        const QPoint start = test::at(h, QPointF(h->nameLeft() + 8, 8));  // (by its name bar)
        const arr::Row last = h_->rowOf(ids[2]);
        const QPoint end = test::at(column, QPointF(h->nameLeft() + 8, last.top + last.mainHeight - 6));
        test::press(window(), start);
        test::moveTo(window(), start + QPoint(0, 12));
        test::moveTo(window(), end);
        h_->settle();
        QVERIFY(line->isVisible());
        QCOMPARE(line->y(), double(last.top + last.height()) - 1);  // between the last track and below
        test::screenshot(window(), QStringLiteral("arrangement_header_drag"));
        test::release(window(), end);
        h_->settle();
        QVERIFY(!line->isVisible());
        QCOMPARE(order(), (QStringList{ids[1], ids[2], ids[0]}));
    }

    void foldingATrackOrGroupHidesItsAutomation() {
        const QStringList ids = makeTracks(2);
        const QString a = ids[0], b = ids[1];
        const QString group = editor().groupTracks({b});
        h_->settle();
        for (const QString& id : {a, group}) {
            editor().showAutomation(id);
            editor().addAutomationLane(id);
        }
        h_->settle();
        QVERIFY(!h_->rowOf(a).lanes.empty());
        QVERIFY(control(a, "deviceChooser") && control(a, "deviceChooser")->isVisible());
        const int height = h_->rowOf(a).mainHeight;

        // A folded track is its name row: no automation lanes or choosers, and it doesn't resize.
        fold(a);
        {
            const arr::Row row = h_->rowOf(a);
            QVERIFY(project().track(a).folded && row.folded && row.lanes.empty() && !row.automation);
            QVERIFY(row.mainHeight < height);
            QCOMPARE(header(a)->height(), double(row.height()));
            QQuickItem* chooser = control(a, "deviceChooser");
            QVERIFY(!chooser || !chooser->isVisible());
            QVERIFY(!control(a, "volume")->isVisible());
            QVERIFY(!header(a)->inResizeZone(row.mainHeight - 2));
            for (const arr::EnvelopeArea& area : lanes()->envelopeAreas()) QVERIFY(area.owner != a);
        }

        // A folded group is two rows, taller than a folded track (as in Ableton:
        // its name row, its volume and pan under it), and hides its tracks.
        fold(group);
        {
            const arr::Row row = h_->rowOf(group);
            QCOMPARE(row.mainHeight, arr::kFoldedGroupHeight);
            QVERIFY(h_->rowOf(a).mainHeight < row.mainHeight && row.mainHeight < height);
            QVERIFY(row.lanes.empty() && !row.automation);
            QVERIFY(control(group, "volume")->isVisible() && control(group, "pan")->isVisible());
            QQuickItem* chooser = control(group, "deviceChooser");
            QVERIFY(!chooser || !chooser->isVisible());
            QVERIFY(h_->rowOf(b).hidden);
        }

        // Unfolded, both show their automation as before.
        fold(a);
        fold(group);
        QVERIFY(!h_->rowOf(a).lanes.empty());
        QCOMPARE(h_->rowOf(a).mainHeight, height);
        QVERIFY(!h_->rowOf(group).lanes.empty());
        QVERIFY(control(group, "deviceChooser") && control(group, "deviceChooser")->isVisible());
    }

    // As in Ableton: a folded track shows its clips as bars with their names, a
    // click selects one and a drag moves it; beside them its lane is a grid to
    // select time on, as any track's.
    void aFoldedTracksClipsAreBarsToClickAndDrag() {
        const QStringList ids = makeTracks(2);
        const QString a = ids[0], b = ids[1];
        editor().setFolded(a, true);
        h_->settle();
        const auto& view = arrangement()->view();
        const arr::Row row = h_->rowOf(a);
        QVERIFY(row.bars && row.mainHeight < h_->rowOf(b).mainHeight);
        const double lowY = row.top - arrangement()->scrollY() + row.mainHeight - 5;  // low in the bar: still the clip's
        const auto point = [&](double beat, double y) { return h_->at(QPointF(std::round(view.beatToX(beat)), y)); };
        const auto hit = lanes()->hitClip(QPointF(std::round(view.beatToX(1.0)), lowY));  // inside c0 (beats 0-4)
        QVERIFY(hit && hit->zone == sub::ui::ArrangementLanes::Zone::Title);

        // A click on a bar selects its clip.
        click(point(1.0, lowY));
        QCOMPARE(selection().clips(), (QSet<ClipRef>{{a, QStringLiteral("c0")}}));
        QVERIFY(selection().timeRange());
        QCOMPARE(selection().rangeStart(), 0.0);
        QCOMPARE(selection().rangeEnd(), 4.0);
        QCOMPARE(selection().rangeTrackIds(), QStringList{a});
        // A drag moves it.
        test::drag(window(), point(1.0, lowY), point(3.0, lowY));
        h_->settle();
        QCOMPARE(project().track(a).clips[0].startBeat, view.snapBeat(2.0));
        QCOMPARE(undo().undoText(), QStringLiteral("Move Time Selection"));
        undo().undo();

        // Beside its clips its lane is a grid like any other: a click moves the
        // insert marker, a drag selects time there.
        selection().clear();
        click(point(5.0, lowY));
        QVERIFY(!selection().timeRange());
        QCOMPARE(selection().insertBeat(), view.snapBeat(5.0));
        QCOMPARE(selection().trackId(), a);
        QCOMPARE(lanes()->cursor().shape(), Qt::IBeamCursor);
        test::drag(window(), point(7.0, lowY), point(3.0, lowY));  // (from beside the bar, over it)
        h_->settle();
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{3.0, 7.0, {a}}));
        QVERIFY(selection().clipRange());
        QCOMPARE(selection().clips(), (QSet<ClipRef>{{a, QStringLiteral("c0")}}));
        QCOMPARE(selection().insertBeat(), 3.0);
        // What is selected acts on its clips as on any track's: Delete cuts out the range.
        session().deleteSelection();
        QCOMPARE(project().track(a).clips.size(), size_t(1));
        QCOMPARE(project().track(a).clips[0].endBeat(project().tempo()), 3.0);
        undo().undo();

        // Started on it, a drag takes in the tracks it crosses...
        selection().clear();
        const arr::Row other = h_->rowOf(b);
        const double otherY = other.top - arrangement()->scrollY() + other.mainHeight / 2;
        test::drag(window(), point(6.0, lowY), point(1.0, otherY));
        h_->settle();
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{1.0, 6.0, {a, b}}));
        // ...and a selection made on the grid of other tracks takes in the folded one, and its clips.
        test::drag(window(), point(1.0, otherY), point(6.0, lowY));
        h_->settle();
        QCOMPARE(selection().rangeStart(), 1.0);
        QCOMPARE(selection().rangeEnd(), 6.0);
        QCOMPARE(selection().rangeTrackIds(), (QStringList{a, b}));
        QCOMPARE(selection().clips(), (QSet<ClipRef>{{a, QStringLiteral("c0")}, {b, QStringLiteral("c1")}}));
        test::screenshot(window(), QStringLiteral("arrangement_folded_track_selection"));
        // Ctrl+D duplicates it on both, the folded track's clip too.
        session().duplicate();
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{6.0, 11.0, {a, b}}));
        QCOMPARE(project().track(a).clips.size(), size_t(2));
        QCOMPARE(project().track(a).clips[1].startBeat, 6.0);
    }

    void foldingLeavesTheNameAndButtonsInPlace() {
        const QStringList ids = makeTracks(1);
        const QString group = editor().groupTracks({ids[0]});
        h_->settle();
        for (const QString& id : {ids[0], group}) {
            const QRectF foldBefore = header(id)->foldRect();
            const QRectF soloBefore = geometryOf(control(id, "solo"));
            const QRectF activatorBefore = geometryOf(control(id, "activator"));
            const qreal nameBefore = header(id)->nameLeft();
            fold(id);
            QVERIFY(project().track(id).folded);
            QCOMPARE(header(id)->foldRect(), foldBefore);
            QCOMPARE(geometryOf(control(id, "solo")), soloBefore);
            QCOMPARE(geometryOf(control(id, "activator")), activatorBefore);
            QCOMPARE(header(id)->nameLeft(), nameBefore);
            QVERIFY(control(id, "solo")->isVisible());
            fold(id);
        }
    }

    // A folded track's name row sits in the middle of what it has: as much room
    // above the buttons, the meter, the first In/Out chooser and the fold button
    // as below them, selected or not. A folded group has two rows (as in
    // Ableton: as tall as its colour while it is open), volume, pan and the In/Out
    // column's second chooser in the second, as evenly.
    void foldedHeadersPadTheirRowsEvenly() {
        const QStringList ids = makeTracks(2);
        const QString group = editor().groupTracks({ids[1]});
        h_->settle();
        editor().setFolded(ids[0], true);
        editor().setFolded(group, true);
        h_->settle();
        QCOMPARE(h_->rowOf(ids[0]).mainHeight, arr::kFoldedHeight);
        QCOMPARE(h_->rowOf(group).mainHeight, arr::kFoldedGroupHeight);
        for (const QString& id : {ids[0], group}) {
            const bool isGroup = id == group;
            TrackHeaderItem* h = header(id);
            const double top = 0.0;
            const double bottom = h->mainHeight() - 1;  // (the line under it)
            const char* first = isGroup ? "output" : "input";  // (the In/Out column's first row)
            const char* second = isGroup ? "outputChannel" : "inputChannel";
            QVERIFY(control(id, first)->isVisible());
            QCOMPARE(control(id, second)->isVisible(), isGroup);
            QCOMPARE(control(id, "volume")->isVisible(), isGroup);
            QCOMPARE(control(id, "pan")->isVisible(), isGroup);
            // The first row as far from the top as the last from the line under it.
            const double pad = geometryOf(control(id, "activator")).top() - top;
            for (const char* name : {"activator", "solo", first})
                QVERIFY2(geometryOf(control(id, name)).top() - top == pad, name);
            const auto lastRow = isGroup ? std::vector<const char*>{"volume", "pan", second}
                                         : std::vector<const char*>{"activator", "solo", first};
            for (const char* name : lastRow) QVERIFY2(bottom - geometryOf(control(id, name)).bottom() == pad, name);
            const QRectF meter = geometryOf(control(id, "meter"));
            QCOMPARE(meter.top() - top, bottom - meter.bottom());
            // The fold button is on the name row.
            QCOMPARE(h->foldRect().center().y(), geometryOf(control(id, "activator")).center().y());
            for (bool selected : {false, true}) {
                selection().selectTrack(selected ? id : QString());
                h_->settle();
                test::screenshot(window(), QStringLiteral("arrangement_folded_header_%1%2")
                                               .arg(id == group ? QStringLiteral("group") : QStringLiteral("track"),
                                                    selected ? QStringLiteral("_selected") : QString()),
                                 h->mapRectToScene(h->boundingRect()).toAlignedRect());
            }
        }
    }

    // The bands of the groups down the headers: a group's colour from the top
    // of its header to the bottom of its last track, unbroken across the lines
    // between its tracks and the automation lanes they show, and across the
    // top of its header; a band per level for nested groups; a folded group's
    // just its header's. A track in no group keeps its own colour at the left.
    void groupBandsRunDownTheirGroups() {
        const QStringList ids = makeTracks(5);
        const QString a = ids[0], b = ids[1], c = ids[2], d = ids[3], e = ids[4];
        const QString outer = editor().groupTracks({a, b, c});
        const QString inner = editor().groupTracks({b, c});
        const QString folded = editor().groupTracks({d});
        editor().showAutomation(c);
        editor().addAutomationLane(c);
        editor().setFolded(folded, true);
        h_->settle();
        QCOMPARE(order(), (QStringList{outer, a, inner, b, c, folded, d, e}));
        QCOMPARE(h_->rowOf(c).lanes.size(), size_t(1));
        auto* column = h_->find<QQuickItem*>(QStringLiteral("headers"));
        QVERIFY(column && h_->find<QQuickItem*>(QStringLiteral("groupBands")));
        const QImage image = window()->grabWindow();
        test::screenshot(window(), QStringLiteral("arrangement_group_bands"));
        const qreal dpr = window()->effectiveDevicePixelRatio();
        const int scroll = arrangement()->scrollY();
        // The colour at a point of the header column (content y).
        auto at = [&](double x, double y) {
            const QPointF p = column->mapToScene(QPointF(x, y - scroll));
            return image.pixelColor(int(p.x() * dpr), int(p.y() * dpr));
        };
        auto color = [&](const QString& id) { return QColor(project().track(id).color); };
        auto band = [](int depth) { return depth * arr::kGroupIndent + arr::kGroupBand / 2.0; };  // a band's middle

        // The outer group's band: from its header's top to C's lane's bottom, across every line in between.
        const arr::Row first = h_->rowOf(outer), last = h_->rowOf(c);
        for (double y = first.top; y < last.bottom() - 1; y += 1) QCOMPARE(at(band(0), y), color(outer));
        QVERIFY(at(band(0), last.bottom() - 1) != color(outer));  // the line under the group shows
        // Its name column, in its colour (its name bar), and the inner group's.
        QCOMPARE(at(60, first.top + 1), color(outer));
        QCOMPARE(at(60, h_->rowOf(inner).top + 1), color(inner));
        // A track's name bar is in its colour; below it, a dark shade of it.
        QCOMPARE(at(60, h_->rowOf(a).top + 1), color(a));
        QVERIFY(at(60, h_->rowOf(a).top + 40) != color(a));
        // The inner group's: from its header to C's bottom, beside the outer one's.
        for (double y = h_->rowOf(inner).top; y < last.bottom() - 1; y += 1) QCOMPARE(at(band(1), y), color(inner));
        QVERIFY(at(band(1), h_->rowOf(a).top + 10) != color(inner));
        // The folded group: its band and its name bar, on its header only.
        const arr::Row foldedRow = h_->rowOf(folded);
        QCOMPARE(at(band(0), foldedRow.top + foldedRow.mainHeight / 2), color(folded));
        QCOMPARE(at(60, foldedRow.top + 1), color(folded));
        QVERIFY(at(band(0), foldedRow.bottom() - 1) != color(folded));
        // E, in no group: its name bar in its own colour, from the column's edge.
        QCOMPARE(at(3, h_->rowOf(e).top + 19), color(e));  // (under its fold button)
        QCOMPARE(header(e)->indent(), 0);
        QCOMPARE(header(c)->indent(), 2 * arr::kGroupIndent);
    }

    // A group's lane shows its tracks' clips (as bars in their colours, as in
    // Ableton) only while it is folded: open, its tracks show them below it and
    // its lane is a grid like any other.
    void aGroupsLaneShowsItsClipsOnlyWhileFolded() {
        const QStringList ids = makeTracks(2);
        const QString group = editor().groupTracks({ids[0], ids[1]});
        editor().setTrackColor(ids[0], QStringLiteral("#ff2020"));
        h_->settle();
        const auto& view = arrangement()->view();
        // The colour at a beat in the group's lane, near its top (where a bar's solid edge is).
        auto colorAt = [&](double beat) {
            const double y = h_->rowOf(group).top - arrangement()->scrollY() + 4;
            const QPoint p = h_->at(QPointF(std::round(view.beatToX(beat)) + 0.5, y));
            const qreal dpr = window()->effectiveDevicePixelRatio();
            return window()->grabWindow().pixelColor(int(p.x() * dpr), int(p.y() * dpr));
        };
        auto red = [](const QColor& c) { return c.red() > 150 && c.green() < 100 && c.blue() < 100; };
        QVERIFY(!red(colorAt(1.5)));  // open: A's clip (beats 0-4) isn't in the group's lane
        editor().setFolded(group, true);
        h_->settle();
        test::screenshot(window(), QStringLiteral("arrangement_folded_group"));
        QVERIFY(red(colorAt(1.5)));  // folded: it is
        QVERIFY(!red(colorAt(10.0)));  // (where no clip is)
    }

    // As in Ableton, a group's lane has a row per track in it, and five at
    // least: a group of one track shows its clips in the top fifth of the lane,
    // empty rows below them. Its fold button is the same folded as open (three
    // bars in a circle, not filled in).
    void aGroupsLaneHasFiveRowsAtLeast() {
        const QStringList ids = makeTracks(1);
        const QString group = editor().groupTracks({ids[0]});
        editor().setTrackColor(ids[0], QStringLiteral("#ff2020"));
        h_->settle();
        auto pixel = [&](const QPoint& p) {
            const qreal dpr = window()->effectiveDevicePixelRatio();
            return window()->grabWindow().pixelColor(int(p.x() * dpr), int(p.y() * dpr));
        };
        // The fold button, drawn the same folded as open.
        auto foldButton = [&] {
            TrackHeaderItem* h = header(group);
            const qreal dpr = window()->effectiveDevicePixelRatio();
            const QRectF r = h->mapRectToScene(h->foldRect());
            return window()->grabWindow().copy(QRectF(r.topLeft() * dpr, r.size() * dpr).toAlignedRect());
        };
        const QImage open = foldButton();
        editor().setFolded(group, true);
        h_->settle();
        QCOMPARE(foldButton(), open);

        const arr::Row row = h_->rowOf(group);
        QCOMPARE(row.mainHeight, arr::kFoldedGroupHeight);
        const double x = std::round(arrangement()->view().beatToX(1.5)) + 0.5;
        auto red = [&](double offset) {
            const QColor c = pixel(h_->at(QPointF(x, row.top - arrangement()->scrollY() + offset)));
            return c.red() > 150 && c.green() < 100 && c.blue() < 100;
        };
        // The lane's rows: a fifth each of what is from 2 px under its top to 3 px over its bottom.
        const double share = (arr::kFoldedGroupHeight - 5) / double(arr::kGroupSummaryRows);
        QVERIFY(red(2 + share / 2));  // its track's clip, in the first
        for (int i = 1; i < arr::kGroupSummaryRows; ++i) QVERIFY(!red(2 + (i + 0.5) * share));  // the rest empty
        test::screenshot(window(), QStringLiteral("arrangement_folded_group_rows"));
    }

    void foldingOneOfTheSelectedTracksFoldsThemAll() {
        const QStringList ids = makeTracks(3);
        const QString a = ids[0], b = ids[1], c = ids[2];
        const QString group = editor().groupTracks({c});
        h_->settle();
        selection().selectTrack(a, true);
        selection().selectTrack(group, true, Selection::Mode::Toggle);
        editor().setFolded(group, true);  // one folded already: they take the clicked one's new state
        h_->settle();

        fold(a);
        QVERIFY(project().track(a).folded && project().track(group).folded && !project().track(b).folded);
        QStringList selected = selection().trackIds();
        std::sort(selected.begin(), selected.end());
        QStringList expected{a, group};
        std::sort(expected.begin(), expected.end());
        QCOMPARE(selected, expected);  // the click doesn't change the selection
        fold(group);
        QVERIFY(!project().track(a).folded && !project().track(group).folded);

        // An unselected track's fold button folds just it.
        fold(b);
        QVERIFY(project().track(b).folded && !project().track(a).folded);
    }

    void groupsCanBeCutCopiedAndPasted() {
        const QStringList ids = makeTracks(3);
        const QString a = ids[0], b = ids[1], c = ids[2];
        const QString group = editor().groupTracks({a, b});
        selection().selectTrack(group, true);
        session().copy();
        selection().selectTrack(c, true);
        session().paste();  // after C
        h_->settle();
        QCOMPARE(project().tracks().size(), size_t(7));  // the group, A, B, C, then the copies
        const QString pasted = project().tracks()[4].id;
        QVERIFY(project().track(pasted).isGroup() && !project().track(pasted).parent);
        QCOMPARE(selection().trackIds(), QStringList{pasted});
        const auto inside = project().descendants(pasted);
        QCOMPARE(inside.size(), size_t(2));
        for (const auto* t : inside) {
            QCOMPARE(t->clips.size(), size_t(1));
            QVERIFY(t->id != a && t->id != b);
            QVERIFY(header(t->id) && header(t->id)->isVisible());
        }
        QCOMPARE(undo().undoText(), QStringLiteral("Paste Track"));

        selection().selectTrack(group, true);
        session().cut();
        h_->settle();
        QVERIFY(!project().hasTrack(group) && !project().hasTrack(a));
        QCOMPARE(project().tracks().size(), size_t(4));
        QVERIFY(!header(a));
        selection().selectTrack(c, true);
        session().paste();
        h_->settle();
        QCOMPARE(project().tracks().size(), size_t(7));
        QVERIFY(project().tracks()[1].isGroup());  // after C, before the first copy
        undo().undo();
        undo().undo();
        h_->settle();
        QVERIFY(project().hasTrack(group));
        QStringList inGroup;
        for (const auto* t : project().descendants(group)) inGroup << t->id;
        QCOMPARE(inGroup, (QStringList{a, b}));
        QVERIFY(header(a) && header(a)->isVisible());
    }

    void aRefusedCutLeavesTheClipboardAndTheHeaderMenuActsOnTracks() {
        const QStringList ids = makeTracks(3);
        const QString a = ids[0], b = ids[1], c = ids[2];
        const QString group = editor().groupTracks({a, b});
        h_->settle();
        // Cutting a track in a frozen group is refused: nothing is cut, the clipboard keeps what it had.
        selection().setTimeRange(8.0, 12.0, {c}, editor().clipsInRange(8.0, 12.0, {c}));
        session().copy();
        QCOMPARE(session().arrangement()->clipboardKind(), QStringLiteral("clips"));
        sub::app::Freeze frozen;
        frozen.path = dir_->path(QStringLiteral("frozen.wav"));
        frozen.durationSec = 2.0;
        frozen.tempo = project().tempo();
        sub::app::OrderedMap<QString, sub::app::Freeze> freezes;
        freezes.insert(group, frozen);
        QCOMPARE(editor().freezeTracks(freezes), QStringList{group});
        selection().selectTrack(a, true);
        session().cut();
        h_->settle();
        QVERIFY(project().hasTrack(a));
        QCOMPARE(session().arrangement()->clipboardKind(), QStringLiteral("clips"));
        undo().undo();  // (unfrozen)

        // Right-clicking a selected track's header: its Cut and Copy act on the selected
        // tracks, not on a clip range selected since.
        selection().selectTrack(c, true);
        selection().setTimeRange(0.0, 4.0, {c}, editor().clipsInRange(0.0, 4.0, {c}));
        QCOMPARE(selection().trackId(), c);
        QCOMPARE(selection().focus(), Selection::Focus::Clips);
        QSignalSpy requested(header(c), &TrackHeaderItem::menuRequested);
        QTest::mouseClick(window(), Qt::RightButton, {}, test::at(header(c), QPointF(20, 8)));
        h_->settle();
        QCOMPARE(requested.count(), 1);
        QCOMPARE(selection().focus(), Selection::Focus::Track);
        QVERIFY(!selection().timeRange());
        QCOMPARE(selection().trackIds(), QStringList{c});
        QCOMPARE(session().whatIsCopied(QStringLiteral("copied")), QStringLiteral("tracks"));
        QTRY_VERIFY(popup()->property("visible").toBool());
        const arr::MenuEntries menu = header(c)->contextMenu();
        QCOMPARE(popup()->property("count").toInt(), int(menu.entries().size()));
        QVERIFY(menu.find(QStringLiteral("Rename")) && menu.find(QStringLiteral("Color/") + sub::app::kTrackColors[0]));
        QCOMPARE(menu.find(QStringLiteral("Group Tracks"))->shortcut, QStringLiteral("Ctrl+G"));
        QVERIFY(menu.find(QStringLiteral("Fold Track")));
        test::screenshot(window(), QStringLiteral("arrangement_header_menu"));
        closePopup();

        // Ctrl+R doesn't rename a track hidden in a folded group.
        editor().setFolded(group, true);
        h_->settle();
        QVERIFY(!arrangement()->renameTrack(a));
        QVariant renamed;
        QVERIFY(QMetaObject::invokeMethod(h_->view(), "renameTrack", Q_RETURN_ARG(QVariant, renamed),
                                          Q_ARG(QVariant, QVariant(group))));
        QVERIFY(renamed.toBool());
        QVERIFY(header(group)->renaming());
        header(group)->finishRename(QString());  // (blank: no change)
        QVERIFY(!header(group)->renaming());
        QCOMPARE(project().track(group).name.isEmpty(), false);
    }

    void clickingHeadersSelectsTracks() {
        const QStringList ids = makeTracks(3);
        click(test::at(header(ids[0]), QPointF(60, 8)));
        QCOMPARE(selection().trackIds(), QStringList{ids[0]});
        QVERIFY(header(ids[0])->selected());
        click(test::at(header(ids[2]), QPointF(60, 8)), Qt::ControlModifier);
        QCOMPARE(selection().trackIds(), (QStringList{ids[0], ids[2]}));
        click(test::at(header(ids[0]), QPointF(60, 8)), Qt::ControlModifier);  // takes it out
        QCOMPARE(selection().trackIds(), QStringList{ids[2]});
        click(test::at(header(ids[0]), QPointF(60, 8)), Qt::ShiftModifier);  // from the last one clicked
        QCOMPARE(selection().trackIds(), (QStringList{ids[0], ids[1], ids[2]}));
        // A plain click on one of several selected selects just it (no drag followed).
        click(test::at(header(ids[1]), QPointF(60, 8)));
        QCOMPARE(selection().trackIds(), QStringList{ids[1]});
        // Below the headers: no track.
        auto* column = h_->find<QQuickItem*>(QStringLiteral("headers"));
        click(test::at(column, QPointF(60, arrangement()->totalHeight() + 20)));
        QCOMPARE(selection().trackId(), QString());
    }

    // --- Returns and sends --------------------------------------------------------------------

    void aReturnGoesAboveTheMaster() {
        const QString track = editor().addAudioTrack();
        const QString ret = session().insertReturnTrack();  // Ctrl+Alt+T
        h_->settle();
        QCOMPARE(project().returns().size(), size_t(1));
        QCOMPARE(project().returns()[0].name, QStringLiteral("A Return"));
        QCOMPARE(order(), QStringList{track});
        QCOMPARE(selection().trackId(), ret);
        TrackHeaderItem* returnHeader = h_->returnHeader(ret);
        sub::ui::BusLane* lane = h_->returnLane(ret);
        QVERIFY(returnHeader && lane && returnHeader->isVisible() && lane->isVisible());
        QCOMPARE(h_->control(returnHeader, QStringLiteral("activator"))->property("text").toString(), QStringLiteral("A"));
        QCOMPARE(returnHeader->kind(), QStringLiteral("return"));
        // Above the master, beside its lane.
        QVERIFY(returnHeader->mapToScene(QPointF()).y() < h_->masterHeader()->mapToScene(QPointF()).y());
        QCOMPARE(lane->mapToScene(QPointF()).y(), returnHeader->mapToScene(QPointF()).y());
        QCOMPARE(lane->height(), returnHeader->height());
        QCOMPARE(arrangement()->returnsHeight(), int(lane->height()));
        QVERIFY(!window()->grabWindow().isNull());  // the rows paint
        // A click on its header selects it.
        selection().selectTrack(track, true);
        click(test::at(returnHeader, QPointF(40, 10)));
        QCOMPARE(selection().trackId(), ret);
        undo().undo();
        h_->settle();
        QVERIFY(project().returns().empty());
        QVERIFY(!h_->returnHeader(ret) && !h_->returnLane(ret));
        QCOMPARE(arrangement()->returnsHeight(), 0);
    }

    void sendKnobsOnTrackHeaders() {
        const QString track = editor().addAudioTrack();
        const QString group = editor().groupTracks({track});
        const QString a = editor().addReturnTrack(), b = editor().addReturnTrack();
        h_->settle();
        TrackHeaderItem* h = header(track);
        QCOMPARE(h->sends().size(), 2);
        QCOMPARE(h->sends()[0].toMap().value(QStringLiteral("returnId")).toString(), a);
        QCOMPARE(h->sends()[1].toMap().value(QStringLiteral("returnId")).toString(), b);
        std::vector<QQuickItem*> letters = sendItems(h, "sendLetter");
        std::vector<QQuickItem*> trackKnobs = knobs(h);
        QCOMPARE(trackKnobs.size(), size_t(2));
        QCOMPARE(letters[0]->property("text").toString(), QStringLiteral("A"));
        QVERIFY(trackKnobs[0]->isVisible() && trackKnobs[0]->isEnabled());
        QVERIFY(knobs(header(group))[1]->isVisible());  // groups send too
        QCOMPARE(trackKnobs[0]->property("value").toDouble(), 0.0);  // no send yet: silent
        // Below volume and pan.
        QVERIFY(trackKnobs[0]->mapToScene(QPointF()).y() >
                control(track, "volume")->mapToScene(QPointF()).y() + control(track, "volume")->height());
        dragKnob(trackKnobs[0], 120);
        QVERIFY(project().track(track).sends.contains(a));
        const sub::app::Send send = project().track(track).sends.value(a);
        QVERIFY(send.levelDb > automation::kMinVolumeDb && !send.preFader);
        QVERIFY(std::abs(knobs(header(track))[0]->property("value").toDouble() - automation::volumeToNormalized(send.levelDb)) < 1e-9);
        QCOMPARE(undo().undoText(), QStringLiteral("Change Send"));
        undo().undo();  // one drag, one step
        QVERIFY(project().track(track).sends.isEmpty());
        // Pre-fader: the letter shows in the accent colour.
        editor().setSend(track, a, -6.0, true);
        h_->settle();
        letters = sendItems(header(track), "sendLetter");
        QCOMPARE(letters[0]->property("color").value<QColor>(), sub::ui::Theme::kAccent);
        QCOMPARE(letters[1]->property("color").value<QColor>() == sub::ui::Theme::kAccent, false);
        QVERIFY(std::abs(knobs(header(track))[0]->property("value").toDouble() - automation::volumeToNormalized(-6.0)) < 1e-9);
        // Its menu: Pre-Fader (checked), Remove Send, Show Automation.
        arr::MenuEntries menu = header(track)->sendMenu(a);
        QCOMPARE(entryTexts(menu), (QStringList{QStringLiteral("Pre-Fader"), QStringLiteral("Remove Send"),
                                                QStringLiteral("Show Automation")}));
        QVERIFY(menu.find(QStringLiteral("Pre-Fader"))->checked);
        QTest::mouseClick(window(), Qt::RightButton, {}, test::centerOf(knobs(header(track))[0]));
        QTRY_VERIFY(popup()->property("visible").toBool());
        QCOMPARE(popup()->property("count").toInt(), 4);  // (and a separator)
        closePopup();
        // The send knobs sit in the mixer column, below volume and pan; with
        // automation shown, the choosers are in the name column, left of them.
        editor().showAutomation(track);
        h_->settle();
        QQuickItem* chooser = control(track, "deviceChooser");
        QVERIFY(chooser && chooser->isVisible());
        QVERIFY(chooser->mapToScene(QPointF(chooser->width(), 0)).x() <= knobs(header(track))[0]->mapToScene(QPointF()).x());
        QVERIFY(menu.triggerText(QStringLiteral("Remove Send")));
        QVERIFY(project().track(track).sends.isEmpty());
    }

    void returnsSendToReturnsButNotIntoACycle() {
        const QString a = editor().addReturnTrack(), b = editor().addReturnTrack();
        h_->settle();
        TrackHeaderItem* headerA = h_->returnHeader(a);
        QVERIFY(!knobs(headerA)[0]->isEnabled());  // not into itself
        QVERIFY(knobs(headerA)[1]->isEnabled());
        QVERIFY(headerA->sendMenu(a).isEmpty());  // (greyed out: no menu)
        dragKnob(knobs(headerA)[1], 120);
        QVERIFY(project().track(a).sends.contains(b));
        h_->settle();
        QVERIFY(!knobs(h_->returnHeader(b))[0]->isEnabled());  // a feeds b: b can't send into a
    }

    void sendAutomationLanes() {
        const QString track = editor().addAudioTrack();
        const QString ret = editor().addReturnTrack();
        const QString key = automation::sendKey(ret);
        const auto names = [&](const QString& owner) {
            QStringList out;
            for (const auto& group : bridge().paramGroups(owner)) {
                if (group.id != QStringLiteral("mixer")) continue;
                for (const auto& spec : group.specs) out << spec.name;
            }
            return out;
        };
        QCOMPARE(names(track), (QStringList{QStringLiteral("Track Volume"), QStringLiteral("Track Pan"),
                                            QStringLiteral("Track Activator"), QStringLiteral("Send A")}));
        QCOMPARE(bridge().paramSpec(track, key)->name, QStringLiteral("Send A"));
        QCOMPARE(names(ret), (QStringList{QStringLiteral("Track Volume"), QStringLiteral("Track Pan"),
                                          QStringLiteral("Track Activator")}));  // not to itself
        editor().showAutomation(track, key);
        editor().addAutomationPoint(track, key, 0.0, 0.5);
        h_->settle();
        QVERIFY(bridge().isAutomated(track, key));
        QCOMPARE(header(track)->sends()[0].toMap().value(QStringLiteral("automation")).toString(), QStringLiteral("on"));
        QQuickItem* knob = knobs(header(track))[0];
        QCOMPARE(knob->property("automation").toString(), QStringLiteral("on"));
        bool shown = false;
        for (const arr::EnvelopeArea& area : lanes()->envelopeAreas()) shown = shown || (area.owner == track && area.key == key);
        QVERIFY(shown);
        // Turning the knob by hand overrides the automation, as for volume.
        dragKnob(knob, 60);
        QVERIFY(bridge().isOverridden(track, key));
        QCOMPARE(knobs(header(track))[0]->property("automation").toString(), QStringLiteral("off"));
    }

    void deletingAReturn() {
        const QString track = editor().addAudioTrack();
        const QString ret = editor().addReturnTrack();
        editor().setSend(track, ret, -3.0);
        h_->settle();
        click(test::at(h_->returnHeader(ret), QPointF(40, 10)));
        QCOMPARE(selection().trackId(), ret);
        session().deleteSelection();
        h_->settle();
        QVERIFY(project().returns().empty());
        QVERIFY(project().track(track).sends.isEmpty());
        QVERIFY(header(track)->sends().isEmpty());
        QVERIFY(knobs(header(track)).empty());
        undo().undo();
        h_->settle();
        QCOMPARE(project().returns().size(), size_t(1));
        QCOMPARE(project().returns()[0].id, ret);
        QCOMPARE(project().track(track).sends.value(ret), (sub::app::Send{-3.0, false}));
        QVERIFY(h_->returnHeader(ret));
    }

    void aLoadedProjectShowsItsReturns() {
        const QString track = editor().addAudioTrack();
        const QString ret = editor().addReturnTrack();
        editor().setSend(track, ret, -3.0);
        const QString path =
            dir_->path(QStringLiteral("returns.") + QFileInfo(session().suggestedSavePath()).suffix());
        QVERIFY(session().saveProjectAs(path));
        session().newProject();
        h_->settle();
        QVERIFY(!h_->returnLane(ret));
        QVERIFY(session().openProject(path));
        h_->settle();
        QVERIFY(h_->returnLane(ret) && h_->returnHeader(ret));
        QCOMPARE(header(track)->sends().size(), 1);
        QCOMPARE(header(track)->sends()[0].toMap().value(QStringLiteral("returnId")).toString(), ret);
    }

    // --- Following the playhead ---------------------------------------------------------------

    void scrollingByHandStopsFollowingThePlayheadUntilPlaybackRestarts() {
        QVERIFY(arrangement()->follow());
        bridge().play();
        QVERIFY(bridge().isPlaying());
        const double far = arrangement()->view().xToBeat(lanes()->width() * 2);
        arrangement()->onPosition(far);  // off the right edge: the view follows it
        QVERIFY(arrangement()->scrollBeats() > 0);
        const QPoint start = test::at(lanes(), QPointF(200, 20));
        const QPoint end = test::at(lanes(), QPointF(260, 20));
        test::press(window(), start, kCtrlAlt);
        test::moveTo(window(), end, kCtrlAlt);  // pans back toward the start
        const double scrolled = arrangement()->scrollBeats();
        arrangement()->onPosition(far + 64);  // the playhead off screen: the view stays where it was put
        QCOMPARE(arrangement()->scrollBeats(), scrolled);
        QVERIFY(arrangement()->followPaused());
        test::release(window(), end, kCtrlAlt);
        arrangement()->onPosition(far + 128);
        QCOMPARE(arrangement()->scrollBeats(), scrolled);
        bridge().stop();
        QTRY_VERIFY(!arrangement()->followPaused());  // stopping (or starting) playback follows again
        bridge().play();
        arrangement()->onPosition(far + 128);
        QVERIFY(arrangement()->scrollBeats() > scrolled);
        bridge().stop();
        // Following off (the view's property): the view stays.
        h_->view()->setProperty("follow", false);
        QVERIFY(!arrangement()->follow());
        bridge().play();
        const double now = arrangement()->scrollBeats();
        arrangement()->onPosition(far + 512);
        QCOMPARE(arrangement()->scrollBeats(), now);
        bridge().stop();
    }

    // --- The header's controls ----------------------------------------------------------------

    // The mixer column's controls fill it in two columns as wide as each other,
    // as Ableton's: the activator over volume; solo and arm over pan.
    void theMixerControlsFillTheirColumn() {
        const QString track = editor().addAudioTrack();
        h_->settle();
        const QRectF activator = geometryOf(control(track, "activator")), volume = geometryOf(control(track, "volume"));
        const QRectF solo = geometryOf(control(track, "solo")), arm = geometryOf(control(track, "arm"));
        const QRectF pan = geometryOf(control(track, "pan"));
        QCOMPARE(volume.width(), pan.width());
        QCOMPARE(activator.width(), volume.width());
        QCOMPARE(activator.left(), volume.left());
        QCOMPARE(solo.left(), pan.left());
        QCOMPARE(arm.right(), pan.right());
        QCOMPARE(solo.width(), arm.width());
        // From the column's left edge to the meter, as far from each.
        const double meter = geometryOf(control(track, "meter")).left();
        QCOMPARE(volume.left() - header(track)->mixerLeft(), meter - pan.right());
    }

    void headerControls() {
        const QString track = editor().addAudioTrack();
        h_->settle();
        QQuickItem* volume = control(track, "volume");
        const QPoint start = test::centerOf(volume);
        test::press(window(), start);
        test::moveTo(window(), start - QPoint(0, 60));
        test::moveTo(window(), start - QPoint(0, 120));  // up 120 px
        test::release(window(), start - QPoint(0, 120));
        h_->settle();
        QCOMPARE(project().track(track).volumeDb, 6.0);  // held at +6 dB
        QCOMPARE(header(track)->volume(), 6.0);
        undo().undo();
        QCOMPARE(project().track(track).volumeDb, 0.0);
        clickControl(track, "solo");
        QVERIFY(project().track(track).solo);
        QVERIFY(control(track, "solo")->property("checked").toBool());
        clickControl(track, "activator");
        QVERIFY(project().track(track).mute);
        QVERIFY(!control(track, "activator")->property("checked").toBool());
        QCOMPARE(control(track, "activator")->property("text").toString(), QStringLiteral("1"));

        // Renaming in place (Ctrl+R): the name's field, with its template (#:
        // its number) all selected; Enter takes it.
        QQuickItem* field = control(track, "rename");
        QVERIFY(field && !field->isVisible());
        QVERIFY(arrangement()->renameTrack(track));
        h_->settle();
        QVERIFY(field->isVisible() && field->hasActiveFocus());
        QCOMPARE(field->property("text").toString(), project().track(track).nameTemplate);
        QVERIFY(field->property("text").toString().startsWith(QStringLiteral("# ")));
        QCOMPARE(field->property("selectedText").toString(), field->property("text").toString());
        typeText(QStringLiteral("# Lead"));
        QTest::keyClick(window(), Qt::Key_Return);
        h_->settle();
        QCOMPARE(project().track(track).name, QStringLiteral("1 Lead"));
        QCOMPARE(header(track)->property("name").toString(), QStringLiteral("1 Lead"));
        QVERIFY(arrangement()->renameTrack(track));
        h_->settle();
        QCOMPARE(field->property("text").toString(), QStringLiteral("# Lead"));
        typeText(QStringLiteral("Drums"));
        QTest::keyClick(window(), Qt::Key_Return);
        h_->settle();
        QCOMPARE(project().track(track).name, QStringLiteral("Drums"));
        QVERIFY(!field->isVisible() && !header(track)->renaming());
        // Escape-free: a rename left (focus elsewhere) takes what was typed too.
        QVERIFY(arrangement()->renameTrack(track));
        h_->settle();
        typeText(QStringLiteral("Beat"));
        QMetaObject::invokeMethod(h_->view(), "focusLanes");
        h_->settle();
        QCOMPARE(project().track(track).name, QStringLiteral("Beat"));
        QVERIFY(lanes()->hasActiveFocus());
    }

    void soloAndArmRules() {
        const QString a = editor().addAudioTrack(), b = editor().addAudioTrack();
        h_->settle();
        // Soloing a track unsoloes the others; Ctrl keeps them.
        clickControl(a, "solo");
        clickControl(b, "solo");
        QVERIFY(!project().track(a).solo && project().track(b).solo);
        clickControl(a, "solo", Qt::ControlModifier);
        QVERIFY(project().track(a).solo && project().track(b).solo);
        // Unsoloing one unsoloes them all.
        clickControl(b, "solo");
        QVERIFY(!project().track(a).solo && !project().track(b).solo);
        // A selected track's solo acts on the selected tracks.
        selection().selectTrack(a, true);
        selection().selectTrack(b, true, Selection::Mode::Toggle);
        clickControl(a, "solo");
        QVERIFY(project().track(a).solo && project().track(b).solo);
        selection().selectTrack(QString());

        // Arming one disarms the others; a track with no input says so.
        QSignalSpy messages(&session(), &sub::app::Session::statusMessage);
        clickControl(a, "arm");
        QVERIFY(project().track(a).armed);
        QVERIFY(std::any_of(messages.begin(), messages.end(), [](const QList<QVariant>& m) {
            return m.value(0).toString().contains(QStringLiteral("has no input"));
        }));
        clickControl(b, "arm");
        QVERIFY(!project().track(a).armed && project().track(b).armed);
        QVERIFY(control(b, "arm")->property("checked").toBool() && !control(a, "arm")->property("checked").toBool());
        clickControl(a, "arm", Qt::ControlModifier);
        QVERIFY(project().track(a).armed && project().track(b).armed);
    }

    void theInputMenuListsTracks() {
        const QString a = editor().addAudioTrack(-1, QStringLiteral("Drums"));
        const QString b = editor().addAudioTrack(-1, QStringLiteral("Bass"));
        const QString group = editor().groupTracks({b});
        const QString ret = editor().addReturnTrack();
        h_->settle();
        const QString groupName = project().track(group).name, retName = project().track(ret).name;
        TrackHeaderItem* h = header(b);
        QCOMPARE(h->inputText(), QStringLiteral("No Input"));
        QCOMPARE(h->inputChannelText(), QString());  // (an empty box: nothing to choose)
        QCOMPARE(h->monitor(), QStringLiteral("auto"));
        arr::MenuEntries menu = h->inputMenu();
        const QString cycle = groupName + QStringLiteral(" (it takes this track's output)");
        // As Ableton's Audio From.
        QCOMPARE(entryTexts(menu), (QStringList{QStringLiteral("Ext. In"), QStringLiteral("Configure…"),
                                                QStringLiteral("Resampling"), QStringLiteral("Drums"), cycle, retName,
                                                QStringLiteral("No Input")}));
        QVERIFY(!menu.find(cycle)->enabled);  // its own group
        QVERIFY(menu.find(QStringLiteral("Drums"))->enabled && menu.find(QStringLiteral("No Input"))->checked);
        // Configure... asks for the preferences' Audio page.
        QSignalSpy preferences(arrangement(), &sub::ui::Arrangement::preferencesRequested);
        QVERIFY(menu.triggerText(QStringLiteral("Configure…")));
        QCOMPARE(preferences.size(), 1);
        QCOMPARE(preferences.at(0).at(0).toInt(), 0);
        // Ext. In: the device's channels (here, none open: the first pair), and their choices below.
        QVERIFY(h->inputMenu().triggerText(QStringLiteral("Ext. In")));
        h_->settle();
        QCOMPARE(project().track(b).input, (std::vector<int>{0, 1}));
        QCOMPARE(header(b)->inputText(), QStringLiteral("Ext. In"));
        QCOMPARE(header(b)->inputChannelText(), QStringLiteral("1/2"));
        undo().undo();
        h_->settle();

        // The input button shows the menu.
        clickControl(b, "input");
        QTRY_VERIFY(popup()->property("visible").toBool());
        QCOMPARE(popup()->property("count").toInt(), int(menu.entries().size()));
        test::screenshot(window(), QStringLiteral("arrangement_input_menu"));
        closePopup();

        QVERIFY(menu.triggerText(QStringLiteral("Drums")));
        h_->settle();
        QCOMPARE(project().track(b).inputTrack, std::optional<QString>(a));
        QCOMPARE(header(b)->inputText(), QStringLiteral("Drums"));
        QVERIFY(header(b)->inputMenu().find(QStringLiteral("Drums"))->checked);
        // Below it, where Drums's signal is taken: Post Mixer, or Pre FX, Post FX.
        QCOMPARE(header(b)->inputChannelText(), QStringLiteral("Post Mixer"));
        QVERIFY(header(b)->inputChannelToolTip().contains(QStringLiteral("Drums")));
        QCOMPARE(entryTexts(header(b)->inputChannelMenu()),
                 (QStringList{QStringLiteral("Pre FX"), QStringLiteral("Post FX"), QStringLiteral("Post Mixer")}));
        QVERIFY(header(b)->inputChannelMenu().triggerText(QStringLiteral("Pre FX")));
        h_->settle();
        QCOMPARE(project().track(b).inputTap, sub::app::kPreFx);
        QCOMPARE(header(b)->inputChannelText(), QStringLiteral("Pre FX"));
        clickControl(b, "inputChannel");
        QTRY_VERIFY(popup()->property("visible").toBool());
        closePopup();
        undo().undo();
        h_->settle();
        editor().renameTrack(a, QStringLiteral("Beat"));
        h_->settle();
        QCOMPARE(header(b)->inputText(), QStringLiteral("Beat"));  // it follows its source's name
        QVERIFY(header(b)->inputMenu().triggerText(QStringLiteral("Resampling")));
        h_->settle();
        QCOMPARE(project().track(b).inputTrack, std::optional<QString>(kMaster));
        QCOMPARE(header(b)->inputText(), QStringLiteral("Resampling"));
        QCOMPARE(header(b)->inputChannelText(), QString());  // (the mix, as it is heard)
        undo().undo();
        undo().undo();
        QCOMPARE(project().track(b).inputTrack, std::optional<QString>(a));
        undo().undo();
        h_->settle();
        QVERIFY(!project().track(b).inputTrack);
        QCOMPARE(header(b)->inputText(), QStringLiteral("No Input"));

        // A return the track sends to can't be its source.
        editor().setSend(b, ret, 0.0);
        h_->settle();
        for (const arr::MenuEntry& entry : header(b)->inputMenu().entries()) {
            if (entry.text.startsWith(retName)) QVERIFY(!entry.enabled);
        }
    }

    // In, Auto and Off side by side, as Ableton's: the one chosen lit.
    void theMonitoringButtons() {
        const QString track = editor().addAudioTrack();
        h_->settle();
        const auto lit = [&](const char* mode) {
            return control(track, (QStringLiteral("monitor:") + QLatin1String(mode)).toLatin1().constData())
                ->property("lit")
                .toBool();
        };
        QVERIFY(control(track, "monitor")->isVisible());
        QVERIFY(lit("auto") && !lit("in") && !lit("off"));
        clickControl(track, "monitor:off");
        h_->settle();
        QCOMPARE(project().track(track).monitor, QStringLiteral("off"));
        QCOMPARE(header(track)->monitor(), QStringLiteral("off"));
        QVERIFY(lit("off") && !lit("auto"));
        clickControl(track, "monitor:in");
        QCOMPARE(project().track(track).monitor, QStringLiteral("in"));
        undo().undo();
        undo().undo();
        h_->settle();
        QCOMPARE(project().track(track).monitor, QStringLiteral("auto"));
        QVERIFY(lit("auto"));
        QVERIFY(header(track)->monitorToolTip(QStringLiteral("in"), false).contains(QStringLiteral("always")));
    }

    // Audio To, as Ableton's: Main, its group, the tracks it can go into (an
    // audio track's input, a device taking a sidechain), Sends Only; below,
    // where in that track it goes.
    void theOutputChoosers() {
        const QString a = editor().addAudioTrack(-1, QStringLiteral("Drums"));
        const QString bus = editor().addAudioTrack(-1, QStringLiteral("Bus"));
        const QString keys = editor().addMidiTrack(-1, QStringLiteral("Keys"));
        const QString inGroup = editor().addAudioTrack(-1, QStringLiteral("Inner"));
        const QString group = editor().groupTracks({inGroup});
        const QString compressor = editor().addDevice(keys, QStringLiteral("compressor"));
        h_->settle();
        const QString groupName = project().track(group).name;
        TrackHeaderItem* h = header(a);
        QCOMPARE(h->outputText(), QStringLiteral("Main"));
        QCOMPARE(h->outputChannelText(), QString());  // (an empty box)
        arr::MenuEntries menu = h->outputMenu();
        // Bus (an audio track: its input), Keys (a MIDI track with a compressor:
        // its sidechain), the track in the group; not the group itself.
        QCOMPARE(entryTexts(menu), (QStringList{QStringLiteral("Ext. Out"), QStringLiteral("Configure…"),
                                                QStringLiteral("Main"), QStringLiteral("Bus"), QStringLiteral("Keys"),
                                                QStringLiteral("Inner"), QStringLiteral("Sends Only")}));
        QVERIFY(!menu.find(QStringLiteral("Ext. Out"))->enabled);
        QVERIFY(menu.find(QStringLiteral("Main"))->checked);
        QVERIFY(menu.triggerText(QStringLiteral("Bus")));
        h_->settle();
        QCOMPARE(project().track(a).output, sub::app::Output::track(bus));
        QCOMPARE(header(a)->outputText(), QStringLiteral("Bus"));
        QCOMPARE(header(a)->outputChannelText(), QStringLiteral("Track In"));
        QCOMPARE(entryTexts(header(a)->outputChannelMenu()), QStringList{QStringLiteral("Track In")});
        // A MIDI track: into its compressor's sidechain; the name follows the device's.
        QVERIFY(header(a)->outputMenu().triggerText(QStringLiteral("Keys")));
        h_->settle();
        QCOMPARE(project().track(a).output, sub::app::Output::sidechain(compressor));
        QCOMPARE(header(a)->outputText(), QStringLiteral("Keys"));
        QCOMPARE(header(a)->outputChannelText(), QStringLiteral("Sidechain-Compressor"));
        QVERIFY(header(a)->outputChannelMenu().find(QStringLiteral("Sidechain-Compressor"))->checked);
        clickControl(a, "outputChannel");
        QTRY_VERIFY(popup()->property("visible").toBool());
        test::screenshot(window(), QStringLiteral("arrangement_output_channel_menu"));
        closePopup();
        // Into a sidechain on an audio track, its name chosen again: it stays in the sidechain (not its input).
        const QString busCompressor = editor().addDevice(bus, QStringLiteral("compressor"));
        h_->settle();
        QVERIFY(header(a)->outputMenu().triggerText(QStringLiteral("Bus")));
        QVERIFY(header(a)->outputChannelMenu().triggerText(QStringLiteral("Sidechain-Compressor")));
        h_->settle();
        QCOMPARE(project().track(a).output, sub::app::Output::sidechain(busCompressor));
        QVERIFY(header(a)->outputMenu().find(QStringLiteral("Bus"))->checked);
        QVERIFY(header(a)->outputMenu().triggerText(QStringLiteral("Bus")));
        h_->settle();
        QCOMPARE(project().track(a).output, sub::app::Output::sidechain(busCompressor));
        // Sends Only; then back to Main.
        QVERIFY(header(a)->outputMenu().triggerText(QStringLiteral("Sends Only")));
        h_->settle();
        QCOMPARE(header(a)->outputText(), QStringLiteral("Sends Only"));
        QVERIFY(header(a)->outputMenu().triggerText(QStringLiteral("Main")));
        h_->settle();
        QVERIFY(project().track(a).output.isDefault());

        // In a group: its group's name, Main past it; a track that feeds it is greyed out.
        TrackHeaderItem* inner = header(inGroup);
        QCOMPARE(inner->outputText(), groupName);
        arr::MenuEntries innerMenu = inner->outputMenu();
        QVERIFY(innerMenu.find(groupName)->checked && !innerMenu.find(QStringLiteral("Main"))->checked);
        editor().setTrackOutput(bus, sub::app::Output::track(inGroup));
        h_->settle();
        const arr::MenuEntry* cycle = header(inGroup)->outputMenu().find(QStringLiteral("Bus (it feeds this track)"));
        QVERIFY(cycle && !cycle->enabled);
        QVERIFY(header(inGroup)->outputMenu().triggerText(QStringLiteral("Main")));
        h_->settle();
        QCOMPARE(project().track(inGroup).output, sub::app::Output::master());
        QCOMPARE(header(inGroup)->outputText(), QStringLiteral("Main"));
        // A group's Audio To is its In/Out column's first row.
        QVERIFY(control(group, "output")->isVisible() && !control(group, "input")->isVisible());
        QCOMPARE(control(group, "output")->y(), control(a, "input")->y());
        // The button shows the menu.
        clickControl(a, "output");
        QTRY_VERIFY(popup()->property("visible").toBool());
        test::screenshot(window(), QStringLiteral("arrangement_output_menu"));
        closePopup();
    }

    // A set routed as Ableton users route theirs: groups in a premaster group,
    // two of them into a track that ducks them (Track In), a track keying that
    // track's device (Sidechain-...), a MIDI track monitoring Auto, folded
    // groups, automation shown on the groups. What each header's In/Out shows.
    void anAbletonLikeSet() {
        auto& e = editor();
        const QString chords = e.addMidiTrack(-1, QStringLiteral("chords"));
        const QString pad = e.addAudioTrack(-1, QStringLiteral("pad"));
        const QString kit = e.addMidiTrack(-1, QStringLiteral("DRUMS"));
        const QString ride = e.addAudioTrack(-1, QStringLiteral("ride"));
        const QString shaker = e.addAudioTrack(-1, QStringLiteral("shaker"));
        const QString sidechain = e.addAudioTrack(-1, QStringLiteral("SIDECHAIN"));
        const QString kick = e.addAudioTrack(-1, QStringLiteral("kick"));
        const QString intro = e.groupTracks({chords});
        const QString fx = e.groupTracks({pad});
        const QString cymbals = e.groupTracks({ride});
        const QString percs = e.groupTracks({shaker});
        const QString drums = e.groupTracks({kit, cymbals, percs});
        const QString premaster = e.groupTracks({intro, fx, drums});
        const QString shaper = e.addDevice(sidechain, QStringLiteral("compressor"));
        const QStringList names{QStringLiteral("Premaster"), QStringLiteral("intro"), QStringLiteral("fx"),
                                QStringLiteral("drums"), QStringLiteral("cymbals"), QStringLiteral("percs")};
        const QStringList groups{premaster, intro, fx, drums, cymbals, percs};
        const QStringList colors{QStringLiteral("#1a2f96"), QStringLiteral("#a9a9a9"), QStringLiteral("#1aff2f"),
                                 QStringLiteral("#ffa529"), QStringLiteral("#ffa529"), QStringLiteral("#ffa529")};
        for (qsizetype i = 0; i < groups.size(); ++i) {
            e.renameTrack(groups[i], names[i]);
            e.setTrackColor(groups[i], colors[i]);
        }
        e.setTrackColor(chords, QStringLiteral("#a9a9a9"));
        e.setTrackColor(kit, QStringLiteral("#ffa529"));
        e.setTrackOutput(intro, sub::app::Output::track(sidechain));
        e.setTrackOutput(fx, sub::app::Output::track(sidechain));
        e.setTrackOutput(kick, sub::app::Output::sidechain(shaper));
        e.setTrackParam(intro, sub::app::TrackField::VolumeDb, -15.0);
        e.setTrackParam(fx, sub::app::TrackField::VolumeDb, -15.0);
        e.setTrackParam(drums, sub::app::TrackField::VolumeDb, -15.0);
        e.setTrackMonitor(sidechain, QStringLiteral("in"));
        for (const QString& id : {premaster, intro}) e.showAutomation(id, automation::kMixerOn);
        e.showAutomation(drums, automation::kMixerVolume);
        e.showAutomation(chords);
        e.setFolded(cymbals, true);
        e.setFolded(percs, true);
        h_->settle();
        QCOMPARE(header(premaster)->outputText(), QStringLiteral("Main"));
        QCOMPARE(header(intro)->outputText(), QStringLiteral("SIDECHAIN"));
        QCOMPARE(header(intro)->outputChannelText(), QStringLiteral("Track In"));
        QCOMPARE(header(chords)->outputText(), QStringLiteral("intro"));
        QCOMPARE(header(chords)->inputText(), QStringLiteral("All Ins"));
        QCOMPARE(header(drums)->outputText(), QStringLiteral("Premaster"));
        QCOMPARE(header(cymbals)->outputText(), QStringLiteral("drums"));
        QCOMPARE(header(kick)->outputText(), QStringLiteral("SIDECHAIN"));
        QCOMPARE(header(kick)->outputChannelText(), QStringLiteral("Sidechain-Compressor"));
        QCOMPARE(entryTexts(header(kick)->outputChannelMenu()),
                 (QStringList{QStringLiteral("Track In"), QStringLiteral("Sidechain-Compressor")}));
        QCOMPARE(control(intro, "volume")->property("text").toString(), QStringLiteral("-15.0"));
        QCOMPARE(control(premaster, "volume")->property("text").toString(), QStringLiteral("0"));
        auto* column = h_->find<QQuickItem*>(QStringLiteral("headers"));
        test::screenshot(window(), QStringLiteral("arrangement_ableton_set"),
                         column->mapRectToScene(column->boundingRect()).toAlignedRect());
        arrangement()->scrollToY(h_->rowOf(sidechain).top);
        h_->settle();
        clickControl(kick, "outputChannel");
        QTRY_VERIFY(popup()->property("visible").toBool());
        test::screenshot(window(), QStringLiteral("arrangement_ableton_set_sidechain_menu"));
        closePopup();
    }

    // View › In/Out hides the In/Out column: the headers narrow by it.
    void hidingTheInOutColumn() {
        const QString track = editor().addAudioTrack();
        h_->settle();
        QQuickItem* view = h_->view();
        QVERIFY(view->property("ioShown").toBool());
        const double wide = header(track)->width();
        QVERIFY(control(track, "input")->isVisible());
        view->setProperty("ioShown", false);
        h_->settle();
        QVERIFY(header(track)->width() < wide);
        QVERIFY(!control(track, "input")->isVisible() && !control(track, "output")->isVisible());
        QVERIFY(control(track, "activator")->isVisible());
        view->setProperty("ioShown", true);
        h_->settle();
        QCOMPARE(header(track)->width(), wide);
    }

    void midiHeaderControls() {
        const QString track = editor().addMidiTrack();
        h_->settle();
        TrackHeaderItem* h = header(track);
        QVERIFY(h->midi());
        QVERIFY(control(track, "input")->isVisible() && control(track, "monitor")->isVisible());
        QCOMPARE(h->inputText(), QStringLiteral("All Ins"));
        QCOMPARE(h->inputChannelText(), QStringLiteral("All Channels"));
        // As Ableton's MIDI From: All Ins, the computer keyboard (and each MIDI input), Configure..., No Input.
        arr::MenuEntries menu = h->inputMenu();
        QCOMPARE(entryTexts(menu).mid(0, 2), (QStringList{QStringLiteral("All Ins"), QStringLiteral("Computer Keyboard")}));
        QCOMPARE(entryTexts(menu).mid(entryTexts(menu).size() - 2),
                 (QStringList{QStringLiteral("Configure…"), QStringLiteral("No Input")}));
        QSignalSpy preferences(arrangement(), &sub::ui::Arrangement::preferencesRequested);
        QVERIFY(menu.triggerText(QStringLiteral("Configure…")));
        QCOMPARE(preferences.at(0).at(0).toInt(), 1);  // the MIDI page
        QVERIFY(h->inputChannelMenu().triggerText(QStringLiteral("Ch. 10")));
        h_->settle();
        QCOMPARE(project().track(track).midiInput, (std::optional<sub::app::MidiInput>(sub::app::MidiInput{QString(), 10})));
        QCOMPARE(header(track)->inputText(), QStringLiteral("All Ins"));
        QCOMPARE(header(track)->inputChannelText(), QStringLiteral("Ch. 10"));
        QVERIFY(header(track)->inputMenu().triggerText(QStringLiteral("No Input")));
        h_->settle();
        QVERIFY(!project().track(track).midiInput);
        QCOMPARE(header(track)->inputText(), QStringLiteral("No Input"));
        QSignalSpy messages(&session(), &sub::app::Session::statusMessage);
        clickControl(track, "arm");
        QVERIFY(project().track(track).armed);
        QVERIFY(std::any_of(messages.begin(), messages.end(), [](const QList<QVariant>& m) {
            return m.value(0).toString().contains(QStringLiteral("has no MIDI input"));
        }));
        undo().undo();
        QCOMPARE(project().track(track).midiInput, (std::optional<sub::app::MidiInput>(sub::app::MidiInput{QString(), 10})));
        // An input that isn't connected (now) can still be chosen, and shows as such.
        editor().setTrackMidiInput(track, sub::app::MidiInput{QStringLiteral("Old Keyboard"), 0});
        h_->settle();
        const arr::MenuEntry* old = header(track)->inputMenu().find(QStringLiteral("Old Keyboard (not connected)"));
        QVERIFY(old && old->checked);
    }

    // --- Routing: inputs and sidechains grey out send knobs -----------------------------------

    void sendKnobsFollowInputs() {
        const QString track = editor().addAudioTrack();
        const QString other = editor().addAudioTrack();
        const QString group = editor().groupTracks({track});
        const QString ret = editor().addReturnTrack();
        h_->settle();
        QVERIFY(usable(group) && usable(other));
        editor().setTrackInputTrack(track, ret);  // ret -> track -> group
        h_->settle();
        QVERIFY(!usable(group) && !usable(track) && usable(other));
        undo().undo();
        h_->settle();
        QVERIFY(usable(group));
        editor().setTrackInputTrack(other, ret);
        h_->settle();
        QVERIFY(!usable(other) && usable(group));
        editor().moveTracks({other}, project().trackIndex(track) + 1, group);
        h_->settle();
        QCOMPARE(parentOf(other), group);
        QCOMPARE(project().track(other).inputTrack, std::optional<QString>(ret));
        QVERIFY(!usable(group));  // moved into the group: ret -> other -> group
    }

    void aSidechainIsRoutingTheSendKnobsKnow() {
        const QString bass = editor().addAudioTrack(-1, QStringLiteral("Bass"));
        const QString ret = editor().addReturnTrack();
        const QString keyed = editor().addDevice(bass, QStringLiteral("utility"));
        h_->settle();
        QVERIFY(usable(bass));
        editor().setDeviceSidechain(bass, keyed, sub::app::Sidechain{ret, sub::app::kPostFader});
        h_->settle();
        QVERIFY(!usable(bass));  // the return keys it: the bass can't send to it
        QVERIFY(header(bass)->sendMenu(ret).isEmpty());
        editor().setDeviceSidechain(bass, keyed, std::nullopt);
        h_->settle();
        QVERIFY(usable(bass));
    }

    // --- Freezing -----------------------------------------------------------------------------

    void theTrackMenuFreezesAndFlattens() {
        test::ScopedEnv recordings("SUBSTATION_RECORDINGS", dir_->path(QStringLiteral("Recordings")));
        const QString wav = test::writeWav(dir_->path(QStringLiteral("dc.wav")), test::constant(1.0, 0.5f), 2);
        const auto refs = editor().addClips(QString(), 0.0, {{wav, 1.0}});
        QCOMPARE(refs.size(), 1);
        const QString track = refs[0].trackId;
        editor().addDevice(track, QStringLiteral("utility"));
        QVERIFY(waitForSource(wav));
        h_->settle();
        const qreal nameLeft = header(track)->nameLeft();
        arr::MenuEntries menu = header(track)->contextMenu();
        QVERIFY(menu.find(QStringLiteral("Freeze Track")) && menu.find(QStringLiteral("Freeze Track"))->enabled);
        QCOMPARE(menu.find(QStringLiteral("Freeze Track"))->shortcut, QStringLiteral("Ctrl+Shift+F"));
        QVERIFY(!menu.find(QStringLiteral("Flatten Track"))->enabled);
        QVERIFY(menu.triggerText(QStringLiteral("Freeze Track")));
        QVERIFY(QTest::qWaitFor([&] { return project().track(track).frozen.has_value() && !session().render()->active(); },
                                30000));
        h_->settle();
        QCOMPARE(header(track)->nameLeft(), nameLeft);  // no mark before its name: it stays where it was
        test::screenshot(window(), QStringLiteral("arrangement_frozen_track"));
        menu = header(track)->contextMenu();
        QVERIFY(menu.find(QStringLiteral("Unfreeze Track")));
        QVERIFY(menu.find(QStringLiteral("Flatten Track"))->enabled);
        QVERIFY(menu.triggerText(QStringLiteral("Unfreeze Track")));
        QVERIFY(!project().track(track).frozen);
        h_->settle();
        QCOMPARE(header(track)->nameLeft(), nameLeft);
    }

    void flatteningAMidiTrack() {
        test::ScopedEnv recordings("SUBSTATION_RECORDINGS", dir_->path(QStringLiteral("Recordings")));
        const QString track = editor().addMidiTrack(-1, QStringLiteral("Keys"));
        const auto ref = editor().addMidiClip(track, 0.0, 2.0);
        QVERIFY(ref);
        editor().setClipNotes(*ref, {{60, 0.0, 1.0, 100}}, QStringLiteral("setup"));
        selection().selectTrack(track, true);
        h_->settle();
        QVERIFY(header(track)->midi());
        QVERIFY(header(track)->contextMenu().triggerText(QStringLiteral("Freeze Track")));
        QVERIFY(QTest::qWaitFor([&] { return project().track(track).frozen.has_value() && !session().render()->active(); },
                                30000));
        h_->settle();
        QVERIFY(header(track)->contextMenu().triggerText(QStringLiteral("Flatten Track")));
        h_->settle();
        const auto& flat = project().track(track);
        QVERIFY(flat.isAudio() && flat.devices.empty() && flat.clips.size() == 1);
        QCOMPARE(selection().trackId(), track);  // selected again
        QVERIFY(!header(track)->midi());
        QVERIFY(!control(track, "input")->property("text").toString().startsWith(QStringLiteral("All Ins")));
        QVERIFY(!window()->grabWindow().isNull());
        undo().undo();
        h_->settle();
        QVERIFY(project().track(track).isMidi());
        QVERIFY(header(track)->midi());
    }

    // --- Resizing -----------------------------------------------------------------------------

    void draggingAHeadersBottomEdgeResizesItsTrack() {
        const QString track = editor().addAudioTrack();
        h_->settle();
        TrackHeaderItem* h = header(track);
        const int height = project().track(track).height;
        const QPoint edge = test::at(h, QPointF(40, h->mainHeight() - 2));
        QVERIFY(h->inResizeZone(h->mainHeight() - 2));
        QVERIFY(!h->inResizeZone(h->mainHeight() / 2));
        test::moveTo(window(), edge, {}, Qt::NoButton);
        h_->settle();
        QCOMPARE(h->cursor().shape(), Qt::SplitVCursor);
        test::drag(window(), edge, edge + QPoint(0, 30));
        h_->settle();
        QCOMPARE(project().track(track).height, height + 30);
        QCOMPARE(h_->rowOf(track).mainHeight, height + 30);
        QCOMPARE(header(track)->height(), double(height + 30));
        test::drag(window(), edge + QPoint(0, 30), edge - QPoint(0, 400));  // no smaller than the least
        h_->settle();
        QCOMPARE(project().track(track).height, sub::app::kMinTrackHeight);
        QVERIFY(!project().track(track).folded);
    }

    // --- A whole project ------------------------------------------------------------------------

    void aProjectWithEverything() {
        // An audio track in a group, a MIDI track with its automation shown, another audio track,
        // two returns (sent to) and the master.
        const auto kick = editor().addClips(QString(), 0.0, {{tones_[0], 2.0}});
        const QString drums = kick[0].trackId;
        editor().renameTrack(drums, QStringLiteral("Kick"));
        const QString group = editor().groupTracks({drums});
        editor().renameTrack(group, QStringLiteral("Drums"));
        const QString keys = editor().addMidiTrack(-1, QStringLiteral("Keys"));
        const auto clip = editor().addMidiClip(keys, 4.0, 8.0);
        QVERIFY(clip);
        editor().setClipNotes(*clip, {{60, 0.0, 1.0, 100}, {64, 1.0, 1.0, 90}, {67, 2.0, 2.0, 80}, {72, 4.0, 3.0, 110}},
                              QStringLiteral("setup"));
        const auto vox = editor().addClips(QString(), 6.0, {{tones_[1], 3.0}});
        editor().renameTrack(vox[0].trackId, QStringLiteral("Vox"));
        const QString a = editor().addReturnTrack(), b = editor().addReturnTrack();
        editor().setSend(keys, a, -12.0);
        editor().setSend(vox[0].trackId, b, -6.0, true);
        editor().showAutomation(keys, automation::kMixerVolume);
        editor().addAutomationPoint(keys, automation::kMixerVolume, 0.0, 0.4);
        editor().addAutomationPoint(keys, automation::kMixerVolume, 8.0, 0.8);
        editor().addAutomationPoint(keys, automation::kMixerVolume, 12.0, 0.6);
        for (const QString& path : tones_) QVERIFY(waitForSource(path));
        selection().selectTrack(keys, true);
        h_->setZoom(40.0);
        h_->settle();
        QVERIFY(h_->returnHeader(a) && h_->returnHeader(b) && header(group) && header(keys));
        test::screenshot(window(), QStringLiteral("arrangement_project"));

        fold(group);
        QVERIFY(project().track(group).folded && h_->rowOf(drums).hidden);
        test::screenshot(window(), QStringLiteral("arrangement_folded_group"));
        fold(group);
    }
};

QTEST_MAIN(TestUiArrangementTracks)
#include "test_ui_arrangement_tracks.moc"
