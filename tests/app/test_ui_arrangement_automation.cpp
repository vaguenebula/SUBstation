// Automation in the arrangement, driven with the mouse on the lanes, the
// master's lane and the headers' choosers: showing lanes, editing envelopes
// (breakpoints, curves, segments, steps, ranges), the lanes' menus, what the
// engine plays, overriding and re-enabling, controls following their
// automation, lanes below a track, saving, automation moving with a dragged
// clip, and lane ranges dragged (or Shift-clicked) over the lanes of several
// tracks, into the clips too. Runs on a display (xvfb here).
// With $SUBSTATION_SCREENS set, it saves screenshots there.

#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <memory>

#include "ArrangementTestSupport.h"
#include "model/Automation.h"
#include "session/ArrangementActions.h"

using sub::app::AutomationPoint;
using sub::app::Envelope;
using sub::app::kMaster;
using sub::ui::ArrangementLanes;
namespace test = sub::app::test;
namespace arr = sub::ui::arrangement;
namespace automation = sub::app::automation;

namespace {

constexpr int kSamplesPerBeat = test::kSampleRate / 2;  // at 120 BPM
const QString kVolume = automation::kMixerVolume;
const QString kPan = automation::kMixerPan;

Envelope env(std::initializer_list<AutomationPoint> points) { return Envelope(points); }

bool near(double a, double b, double tolerance = 1e-6) { return std::abs(a - b) <= tolerance; }

}  // namespace

class TestUiArrangementAutomation : public QObject {
    Q_OBJECT

    std::unique_ptr<test::UiSession> ui_;
    std::unique_ptr<test::ArrangementHarness> h_;
    std::unique_ptr<test::TempDir> dir_;
    QString a_, b_;  // two MIDI tracks

    sub::app::Session& session() { return ui_->session(); }
    sub::app::Project& project() { return *session().project(); }
    sub::app::ProjectEditor& editor() { return *session().editor(); }
    sub::app::Selection& selection() { return *session().selection(); }
    sub::app::EngineBridge& bridge() { return *session().bridge(); }
    QUndoStack& undo() { return *session().undoStack(); }
    QQuickWindow* window() { return h_->window(); }
    ArrangementLanes* lanes() { return h_->lanes(); }
    sub::ui::Arrangement* arrangement() { return h_->arrangement(); }

    // The lane showing an owner's automation (of `key`, or its first).
    arr::EnvelopeArea area(const QString& owner, const QString& key = {}) {
        const std::vector<arr::EnvelopeArea> areas =
            owner == kMaster ? h_->masterLane()->envelopeAreas() : lanes()->envelopeAreas();
        for (const arr::EnvelopeArea& a : areas) {
            if (a.owner == owner && (key.isEmpty() || a.key == key)) return a;
        }
        qWarning() << "no lane for" << owner << key;
        return {};
    }
    // A point of a lane, in the lane's item.
    QPointF local(const QString& owner, double beat, double value, const QString& key = {}) {
        const arr::EnvelopeArea lane = area(owner, key);
        return QPointF(std::round(arrangement()->view().beatToX(beat)), std::round(lane.y(value)));
    }
    // The same in the window.
    QPoint point(const QString& owner, double beat, double value, const QString& key = {}) {
        QQuickItem* item = owner == kMaster ? static_cast<QQuickItem*>(h_->masterLane()) : lanes();
        return test::at(item, local(owner, beat, value, key));
    }
    Envelope envelope(const QString& owner, const QString& key) { return project().envelope(owner, key); }
    std::vector<double> beats(const Envelope& points) {
        std::vector<double> out;
        for (const AutomationPoint& p : points) out.push_back(p.beat);
        return out;
    }
    sub::app::SelectedPoints points(const QString& owner, const QString& key, std::initializer_list<int> indices) {
        return {owner, key, QSet<int>(indices)};
    }
    void click(QPoint at, Qt::KeyboardModifiers modifiers = {}) {
        test::click(window(), at, modifiers);
        h_->settle();
    }
    void drag(QPoint from, QPoint to, Qt::KeyboardModifiers modifiers = {}) {
        test::drag(window(), from, to, modifiers);
        h_->settle();
    }
    void hover(QPoint at, Qt::KeyboardModifiers modifiers = {}) {
        test::moveTo(window(), at, modifiers, Qt::NoButton);
        lanes()->updateHover(lanes()->mapFromScene(QPointF(at)), modifiers);
    }
    QString deviceOf(const QString& trackId, int index = 0) {
        return project().track(trackId).devices[static_cast<size_t>(index)].id;
    }
    QVariantMap chooser(sub::ui::TrackHeaderItem* header, int index) { return header->choosers().value(index).toMap(); }

private Q_SLOTS:
    void initTestCase() {
        test::prepareApplication();
        if (!test::haveDisplay()) QSKIP("needs a display: Qt Quick's software renderer draws none of this geometry");
        sub::ui::setUpApplication();
        ui_ = std::make_unique<test::UiSession>();
        h_ = std::make_unique<test::ArrangementHarness>(*ui_);
        QVERIFY(h_->show());
        dir_ = std::make_unique<test::TempDir>();
    }

    void cleanupTestCase() {
        h_.reset();
        ui_.reset();
    }

    // Two MIDI tracks; the view zoomed so a beat is 40 px.
    void init() {
        bridge().stop();
        session().newProject();
        undo().clear();
        arrangement()->setSnap(true);
        arrangement()->setGridLevel(0);
        arrangement()->setScrollBeats(0.0);
        arrangement()->setScrollY(0);
        a_ = editor().addMidiTrack();
        b_ = editor().addMidiTrack();
        h_->setZoom(40.0);
        test::moveTo(window(), QPoint(2, 2), {}, Qt::NoButton);
        h_->settle();
    }

    void cleanup() { bridge().stop(); }

    void aShowsAndHidesEveryLane() {
        QVERIFY(!h_->row(0).automation);
        editor().toggleAllAutomation();  // A (View › Automation)
        h_->settle();
        for (const auto& row : arrangement()->layout().rows()) QVERIFY(row.automation);
        QVERIFY(project().master().automationView.shown);
        const auto areas = lanes()->envelopeAreas();
        QCOMPARE(areas.size(), size_t(2));
        QCOMPARE(areas[0].owner, a_);
        QCOMPARE(areas[0].key, kVolume);
        QCOMPARE(areas[1].owner, b_);
        QVERIFY(h_->masterLane()->height() > arr::kMasterHeight);  // room for its lane and choosers
        QCOMPARE(chooser(h_->header(a_), 0).value(QStringLiteral("param")).toString(), QStringLiteral("Track Volume"));
        QCOMPARE(chooser(h_->header(a_), 0).value(QStringLiteral("device")).toString(), QStringLiteral("Mixer"));
        QQuickItem* deviceChooser = h_->control(h_->header(a_), QStringLiteral("deviceChooser"));
        QVERIFY(deviceChooser && deviceChooser->isVisible());
        QCOMPARE(chooser(h_->masterHeader(), 0).value(QStringLiteral("param")).toString(), QStringLiteral("Master Volume"));
        // A track's own lane is at least kMinAutomationRow high while its automation shows.
        QVERIFY(h_->row(0).mainHeight >= arr::kMinAutomationRow);
        editor().toggleAllAutomation();
        h_->settle();
        for (const auto& row : arrangement()->layout().rows()) QVERIFY(!row.automation);
        QVERIFY(lanes()->envelopeAreas().empty());
        QVERIFY(h_->header(a_)->choosers().isEmpty());
        QCOMPARE(h_->masterLane()->height(), double(arr::kMasterHeight));
    }

    void changingAParameterShowsItsLane() {
        selection().selectTrack(a_);
        const QString utility = editor().addDevice(a_, QStringLiteral("utility"));
        editor().setDeviceParam(a_, utility, QStringLiteral("gain"), -6.0);
        const QString key = automation::deviceKey(utility, QStringLiteral("gain"));
        QVERIFY(project().track(a_).automationView.shown);
        QCOMPARE(project().track(a_).automationView.key, std::optional<QString>(key));
        h_->settle();
        QCOMPARE(chooser(h_->header(a_), 0).value(QStringLiteral("device")).toString(), QStringLiteral("Utility"));
        // The header's controls too: dragging its pan shows the pan lane.
        QQuickItem* pan = h_->control(h_->header(a_), QStringLiteral("pan"));
        const QPoint at = test::centerOf(pan);
        drag(at, at - QPoint(0, 30));
        QCOMPARE(project().track(a_).automationView.key, std::optional<QString>(kPan));
        QVERIFY(project().track(a_).pan > 0.0);
        editor().trySetTrackParam(kMaster, QStringLiteral("volume_db"), -3.0);
        QCOMPARE(project().master().automationView, (sub::app::AutomationView{true, kVolume, {}}));
    }

    void clickingAMixerControlShowsItsLane() {
        // Pressing a control shows its automation (as in Ableton), and changes nothing.
        h_->settle();
        QQuickItem* volume = h_->control(h_->header(a_), QStringLiteral("volume"));
        const int steps = undo().count();
        click(test::centerOf(volume));
        const auto& view = project().track(a_).automationView;
        QVERIFY(view.shown);
        QCOMPARE(view.key, std::optional<QString>(kVolume));
        QCOMPARE(project().track(a_).volumeDb, 0.0);
        QCOMPARE(undo().count(), steps);
        QQuickItem* pan = h_->control(h_->header(b_), QStringLiteral("pan"));
        click(test::centerOf(pan));
        QCOMPARE(project().track(b_).automationView.key, std::optional<QString>(kPan));
    }

    void clickOnTheLineAddsBreakpointsAndDraggingMovesThem() {
        editor().showAutomation(a_, kPan);
        h_->settle();
        click(point(a_, 2.1, 0.9));  // off the line (centred pan: 0.5)
        QVERIFY(envelope(a_, kPan).empty());
        // Over the line, the cursor says a click adds a breakpoint, and where it would go shows.
        hover(point(a_, 2.1, 0.5));
        const auto hovered = lanes()->hoverPoint();
        QVERIFY(hovered && hovered->kind == arr::Hover::Kind::Add && !hovered->index);
        QVERIFY(near(hovered->beat, 2.0));
        QVERIFY(near(hovered->value, 0.5));
        QCOMPARE(lanes()->cursor().shape(), Qt::BitmapCursor);
        click(point(a_, 2.1, 0.5));  // snaps to the grid
        QCOMPARE(envelope(a_, kPan).size(), size_t(1));
        const AutomationPoint added = envelope(a_, kPan)[0];
        QVERIFY(near(added.beat, 2.0));
        QVERIFY(near(added.value, 0.5));
        QCOMPARE(selection().points(), std::make_optional(points(a_, kPan, {0})));
        click(point(a_, 6.0, 0.75));  // off the line again
        QCOMPARE(envelope(a_, kPan).size(), size_t(1));
        click(point(a_, 6.0, 0.5));
        QCOMPARE(beats(envelope(a_, kPan)), (std::vector<double>{2.0, 6.0}));
        // Drag the first one right and up: it stops at its neighbour, and it is one undo step.
        const int steps = undo().count();
        drag(point(a_, 2.0, added.value), point(a_, 9.0, 1.0));
        const AutomationPoint moved = envelope(a_, kPan)[0];
        QVERIFY(near(moved.beat, 6.0));
        QVERIFY(near(moved.value, 1.0, 0.05));
        QCOMPARE(undo().count(), steps + 1);
        undo().undo();
        QCOMPARE(envelope(a_, kPan)[0], added);
    }

    void pressingOnTheLineAddsABreakpointThatADragPlaces() {
        editor().showAutomation(a_, kPan);
        h_->settle();
        const int steps = undo().count();
        drag(point(a_, 2.0, 0.5), point(a_, 3.0, 0.9));
        QCOMPARE(envelope(a_, kPan).size(), size_t(1));
        const AutomationPoint placed = envelope(a_, kPan)[0];
        QVERIFY(near(placed.beat, 3.0));
        QVERIFY(near(placed.value, 0.9, 0.05));
        QCOMPARE(selection().points(), std::make_optional(points(a_, kPan, {0})));
        QCOMPARE(undo().count(), steps + 1);  // adding and placing: one step
        undo().undo();
        QVERIFY(envelope(a_, kPan).empty());
    }

    void lanesShowParametersChangedByHand() {
        editor().showAutomation(a_);
        h_->settle();
        const QString synth = deviceOf(a_);
        int frames = lanes()->lastStats().frames;
        editor().setDeviceParam(a_, synth, QStringLiteral("attack"), 50.0);
        QTRY_VERIFY(lanes()->lastStats().frames > frames);  // drawn again: a lane without an envelope shows its value
        h_->settle();
        frames = lanes()->lastStats().frames;
        Q_EMIT bridge().pluginParamsChanged(b_, QStringLiteral("x"));  // (a plug-in's values) on a track not showing any
        QTest::qWait(60);
        QCOMPARE(lanes()->lastStats().frames, frames);
    }

    void aNewBreakpointGoesOnASlopedLine() {
        editor().showAutomation(a_, kPan);
        editor().setEnvelope(a_, kPan, env({{0.0, 0.0}, {8.0, 1.0}}));
        h_->settle();
        click(point(a_, 4.0, 0.2));  // well below the line
        QCOMPARE(envelope(a_, kPan).size(), size_t(2));
        click(point(a_, 4.0, 0.5) + QPoint(0, 2));  // a little off it counts
        const Envelope points = envelope(a_, kPan);
        QCOMPARE(beats(points), (std::vector<double>{0.0, 4.0, 8.0}));
        QVERIFY(near(points[1].value, 0.5));
    }

    void altDragBendsASegment() {
        editor().showAutomation(a_, kPan);
        editor().setEnvelope(a_, kPan, env({{0.0, 0.0}, {8.0, 1.0}}));
        h_->settle();
        const QPoint start = point(a_, 4.0, 0.5);
        hover(start, Qt::AltModifier);
        QCOMPARE(lanes()->cursor().shape(), Qt::SizeVerCursor);
        drag(start, start - QPoint(0, 60), Qt::AltModifier);
        const Envelope points = envelope(a_, kPan);
        QVERIFY(points[0].curve > 0.2);
        QCOMPARE(points.size(), size_t(2));  // bent, not a new point
        QVERIFY(*automation::valueAt(points, 4.0) > 0.5);  // bulging upward
        undo().undo();
        QCOMPARE(envelope(a_, kPan)[0].curve, 0.0);
    }

    void deletingBreakpoints() {
        editor().showAutomation(a_, kPan);
        editor().setEnvelope(a_, kPan, env({{1.0, 0.2}, {3.0, 0.8}, {5.0, 0.4}, {7.0, 0.6}}));
        h_->settle();
        click(point(a_, 3.0, 0.8));  // a click deletes a breakpoint
        QCOMPARE(beats(envelope(a_, kPan)), (std::vector<double>{1.0, 5.0, 7.0}));
        undo().undo();
        h_->settle();
        // Each click of a double-click counts: the first deletes, the second (off the line now) does nothing.
        test::doubleClick(window(), point(a_, 3.0, 0.8));
        h_->settle();
        QCOMPARE(beats(envelope(a_, kPan)), (std::vector<double>{1.0, 5.0, 7.0}));
        // Shift- (or Ctrl-) clicks select instead; Delete deletes the selected ones.
        click(point(a_, 1.0, 0.2), Qt::ShiftModifier);
        click(point(a_, 7.0, 0.6), Qt::ShiftModifier);
        QCOMPARE(selection().points(), std::make_optional(points(a_, kPan, {0, 2})));
        QCOMPARE(envelope(a_, kPan).size(), size_t(3));
        session().deleteSelection();
        QCOMPARE(beats(envelope(a_, kPan)), (std::vector<double>{5.0}));
    }

    void aTimeSelectionInALaneClearsAndDuplicates() {
        editor().showAutomation(a_, kPan);
        editor().setEnvelope(a_, kPan, env({{0.0, 0.0}, {2.0, 1.0}, {4.0, 0.0}}));
        h_->settle();
        drag(point(a_, 1.0, 0.9), point(a_, 3.0, 0.9));
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{1.0, 3.0, {a_}}));
        QCOMPARE(selection().lanes(), (QList<sub::app::LaneRef>{{a_, kPan}}));
        test::screenshot(window(), QStringLiteral("arrangement_automation_range"));
        session().duplicate();  // Ctrl+D: after the range, over what was there
        QCOMPARE(selection().rangeStart(), 3.0);
        QCOMPARE(selection().rangeEnd(), 5.0);
        Envelope points = envelope(a_, kPan);
        QVERIFY(near(*automation::valueAt(points, 4.0), *automation::valueAt(points, 2.0)));
        undo().undo();
        selection().setTimeRange(1.0, 3.0, {a_}, std::nullopt, {{a_, kPan}});
        session().deleteSelection();
        points = envelope(a_, kPan);
        QVERIFY(near(*automation::valueAt(points, 2.0), 0.5));  // straight across the range
        QVERIFY(near(*automation::valueAt(points, 0.5), 0.25));  // the rest as it was
    }

    void cutCopyAndPasteAutomationFromTheLanesMenu() {
        editor().showAutomation(a_, kPan);
        editor().showAutomation(b_, kPan);
        editor().setEnvelope(a_, kPan, env({{0.0, 0.0}, {4.0, 1.0}}));
        h_->settle();
        drag(point(a_, 1.0, 0.95), point(a_, 3.0, 0.95));
        // Outside the range, Cut and Copy have nothing; inside, they do.
        arr::MenuEntries menu = lanes()->contextMenu(local(a_, 6.0, 0.95));
        QVERIFY(!menu.find(QStringLiteral("Copy"))->enabled);
        QVERIFY(!menu.find(QStringLiteral("Paste"))->enabled);
        menu = lanes()->contextMenu(local(a_, 2.0, 0.95));
        QVERIFY(menu.find(QStringLiteral("Copy"))->enabled);
        QVERIFY(menu.triggerText(QStringLiteral("Copy")));
        QCOMPARE(session().arrangement()->clipboardKind(), QStringLiteral("automation"));
        // Paste at the insert marker, onto the lane right-clicked (not one of the selected ones).
        selection().setInsert(8.0);
        menu = lanes()->contextMenu(local(b_, 2.0, 0.95, kPan));
        QVERIFY(menu.find(QStringLiteral("Paste"))->enabled);
        QVERIFY(menu.triggerText(QStringLiteral("Paste")));
        const Envelope pasted = envelope(b_, kPan);
        QVERIFY(!pasted.empty());
        QVERIFY(near(*automation::valueAt(pasted, 9.0), 0.5));
        QCOMPARE(selection().lanes(), (QList<sub::app::LaneRef>{{b_, kPan}}));
        // Cut takes it out of the lane.
        drag(point(a_, 1.0, 0.95), point(a_, 3.0, 0.95));
        menu = lanes()->contextMenu(local(a_, 2.0, 0.95));
        QVERIFY(menu.triggerText(QStringLiteral("Cut")));
        QVERIFY(near(*automation::valueAt(envelope(a_, kPan), 2.0), 0.5));  // straight across (from 0.25 to 0.75)
    }

    void theLanesOwnMenu() {
        editor().showAutomation(a_, kPan);
        editor().setEnvelope(a_, kPan, env({{1.0, 0.2}, {3.0, 0.8}, {5.0, 0.4}}));
        editor().addAutomationLane(a_);
        h_->settle();
        // On a breakpoint: Delete Breakpoint; then the lane's own entries.
        arr::MenuEntries menu = lanes()->contextMenu(local(a_, 3.0, 0.8, kPan));
        QVERIFY(menu.find(QStringLiteral("Delete Breakpoint")));
        QVERIFY(!menu.find(QStringLiteral("Remove Lane")));  // (its own lane)
        QVERIFY(menu.find(QStringLiteral("Show Automation in New Lane")));
        QVERIFY(menu.find(QStringLiteral("Hide Automation")));
        QVERIFY(menu.find(QStringLiteral("Delete Envelope"))->enabled);
        QVERIFY(menu.triggerText(QStringLiteral("Delete Breakpoint")));
        QCOMPARE(beats(envelope(a_, kPan)), (std::vector<double>{1.0, 5.0}));
        // Several selected: Delete Selected Breakpoints.
        click(point(a_, 1.0, 0.2, kPan), Qt::ShiftModifier);
        click(point(a_, 5.0, 0.4, kPan), Qt::ShiftModifier);
        menu = lanes()->contextMenu(local(a_, 7.0, 0.1, kPan));
        QVERIFY(menu.triggerText(QStringLiteral("Delete Selected Breakpoints")));
        QVERIFY(envelope(a_, kPan).empty());
        // A lane below: Remove Lane.
        const QString below = project().track(a_).automationView.lanes.value(0);
        menu = lanes()->contextMenu(local(a_, 2.0, 0.5, below));
        QVERIFY(!menu.find(QStringLiteral("Delete Envelope"))->enabled);
        QVERIFY(menu.triggerText(QStringLiteral("Remove Lane")));
        QVERIFY(project().track(a_).automationView.lanes.isEmpty());
        menu = lanes()->contextMenu(local(a_, 2.0, 0.5, kPan));
        QVERIFY(menu.triggerText(QStringLiteral("Hide Automation")));
        QVERIFY(!project().track(a_).automationView.shown);
    }

    void automationPlaysUntilChangedByHand() {
        const QString path = test::writeWav(dir_->path(QStringLiteral("t.wav")), test::constant(4.0, 0.5f), 2);
        const QString track = editor().addClips(QString(), 0.0, {{path, 4.0}})[0].trackId;
        bridge().requestSource(path);
        QVERIFY(QTest::qWaitFor([&] { return bridge().source(path) != nullptr; }, 10000));
        const Envelope silentThenLoud =
            env({{0.0, 0.0}, {1.0, 0.0}, {1.0, automation::volumeToNormalized(0.0)}});
        editor().setEnvelope(track, kVolume, silentThenLoud);
        std::vector<float> out = ui_->engine().renderOffline(0.0, 2 * kSamplesPerBeat);
        QCOMPARE(out[size_t(2 * (kSamplesPerBeat - 20))], 0.0f);
        QVERIFY(near(out[size_t(2 * (kSamplesPerBeat + 100))], 0.5, 1e-3));
        QVERIFY(bridge().isAutomated(track, kVolume));
        h_->settle();
        QCOMPARE(h_->header(track)->volumeAutomation(), QStringLiteral("on"));
        // Setting the volume by hand overrides the automation...
        editor().trySetTrackParam(track, QStringLiteral("volume_db"), -6.0);
        QVERIFY(bridge().isOverridden(track, kVolume));
        QVERIFY(session().automationOverridden());
        h_->settle();
        QCOMPARE(h_->header(track)->volumeAutomation(), QStringLiteral("off"));
        QCOMPARE(h_->control(h_->header(track), QStringLiteral("volume"))->property("automation").toString(),
                 QStringLiteral("off"));
        out = ui_->engine().renderOffline(0.0, 2 * kSamplesPerBeat);
        QVERIFY(near(out[2000], 0.5 * std::pow(10.0, -6 / 20.0), 1e-3));
        // The lane's menu and the header's offer Re-Enable Automation; it plays again.
        editor().showAutomation(track, kVolume);
        h_->settle();
        QVERIFY(h_->header(track)->contextMenu().find(QStringLiteral("Re-Enable Automation")));
        arr::MenuEntries menu = lanes()->contextMenu(local(track, 2.0, 0.5, kVolume));
        QVERIFY(menu.triggerText(QStringLiteral("Re-Enable Automation")));
        QVERIFY(!bridge().hasOverrides());
        out = ui_->engine().renderOffline(0.0, kSamplesPerBeat - 10);
        QCOMPARE(*std::max_element(out.begin(), out.end()), 0.0f);
        // Its envelope deleted, the track plays at its own volume again.
        editor().clearEnvelope(track, kVolume);
        out = ui_->engine().renderOffline(0.0, 2000);
        QVERIFY(near(out[2 * 1000], 0.5 * std::pow(10.0, -6 / 20.0), 1e-3));
    }

    void controlsFollowTheirAutomation() {
        selection().selectTrack(a_);
        editor().setEnvelope(a_, kVolume, env({{0.0, 0.0}, {4.0, 1.0}}));
        bridge().locate(2.0);
        h_->settle();
        sub::ui::TrackHeaderItem* header = h_->header(a_);
        QCOMPARE(header->volumeAutomation(), QStringLiteral("on"));
        QVERIFY(near(header->volume(), automation::normalizedToVolume(0.5), 0.05));
        QQuickItem* volume = h_->control(header, QStringLiteral("volume"));
        QVERIFY(near(volume->property("value").toDouble(), automation::normalizedToVolume(0.5), 0.05));
        bridge().locate(4.0);
        h_->settle();
        QVERIFY(near(header->volume(), automation::normalizedToVolume(1.0), 0.05));
        editor().clearEnvelope(a_, kVolume);  // back to its own value
        h_->settle();
        QCOMPARE(header->volumeAutomation(), QString());
        QCOMPARE(header->volume(), 0.0);
    }

    void masterAutomation() {
        const QString path = test::writeWav(dir_->path(QStringLiteral("m.wav")), test::constant(2.0, 0.5f), 2);
        editor().addClips(QString(), 0.0, {{path, 2.0}});
        bridge().requestSource(path);
        QVERIFY(QTest::qWaitFor([&] { return bridge().source(path) != nullptr; }, 10000));
        editor().showAutomation(kMaster, kPan);
        h_->settle();
        click(point(kMaster, 0.0, 0.5));  // on its line: pan centred
        drag(point(kMaster, 0.0, 0.5), point(kMaster, 0.0, 0.0));
        QCOMPARE(envelope(kMaster, kPan).size(), size_t(1));
        QVERIFY(near(envelope(kMaster, kPan)[0].value, 0.0, 0.03));
        const std::vector<float> out = ui_->engine().renderOffline(0.0, 1000);
        QVERIFY(near(out[2 * 500], 0.5, 1e-3));
        QVERIFY(std::abs(out[2 * 500 + 1]) < 0.02);  // panned left
        h_->settle();
        QCOMPARE(h_->masterHeader()->panAutomation(), QStringLiteral("on"));
        // Its lane's menu, off the lane: Hide Automation.
        arr::MenuEntries menu = h_->masterLane()->contextMenu(local(kMaster, 4.0, 0.5));
        QVERIFY(menu.find(QStringLiteral("Hide Automation")));
        // The master's header's menu.
        menu = h_->masterHeader()->contextMenu();
        QCOMPARE(selection().trackId(), kMaster);
        QVERIFY(menu.triggerText(QStringLiteral("Show Automation in New Lane")));
        QCOMPARE(project().master().automationView.lanes.size(), 1);
        h_->settle();
        QVERIFY(h_->masterLane()->height() >= arr::kMasterHeight + arr::kAutomationLaneHeight);
    }

    void lanesBelowATrack() {
        editor().showAutomation(a_);
        h_->settle();
        sub::ui::TrackHeaderItem* header = h_->header(a_);
        const int before = h_->row(1).top;
        QQuickItem* add = h_->control(header, QStringLiteral("addLane"));
        QVERIFY(add);
        click(test::centerOf(add));  // "+"
        const auto& row = h_->row(0);
        QCOMPARE(row.lanes.size(), size_t(1));
        QCOMPARE(row.lanes[0].key, kPan);
        QCOMPARE(h_->row(1).top, before + row.lanes[0].height);
        const arr::EnvelopeArea lane = area(a_, kPan);
        QCOMPARE(lane.lane, 0);
        QVERIFY(lane.rect.top() >= row.mainHeight - arrangement()->scrollY());
        click(point(a_, 1.0, 0.5, kPan));
        QVERIFY(near(envelope(a_, kPan)[0].value, 0.5));
        QVERIFY(envelope(a_, kVolume).empty());
        // Its choosers: the parameter chooser's menu sets the lane's parameter.
        QCOMPARE(header->choosers().size(), 2);
        const arr::MenuEntries params = header->paramMenu(0);
        QVERIFY(params.find(QStringLiteral("Track Pan"))->checked);
        QVERIFY(params.find(QStringLiteral("Track Pan"))->dot.isValid());  // automated
        QVERIFY(params.triggerText(QStringLiteral("Track Volume")));
        QCOMPARE(project().track(a_).automationView.lanes, QStringList{kVolume});
        // The device chooser's menu: the mixer and each device (its automated parameter first).
        const arr::MenuEntries devices = header->deviceMenu(-1);
        QCOMPARE(devices.texts(), (QStringList{QStringLiteral("Mixer"), QStringLiteral("Synth")}));
        QVERIFY(devices.find(QStringLiteral("Mixer"))->checked);
        QVERIFY(devices.triggerText(QStringLiteral("Synth")));
        QVERIFY(automation::keyDevice(*project().track(a_).automationView.key) == deviceOf(a_));
        h_->settle();
        QCOMPARE(chooser(header, 0).value(QStringLiteral("device")).toString(), QStringLiteral("Synth"));
        test::screenshot(window(), QStringLiteral("arrangement_automation_lanes"));
        QQuickItem* remove = h_->control(header, QStringLiteral("removeLane"));
        QVERIFY(remove);
        click(test::centerOf(remove));  // "−"
        QVERIFY(h_->row(0).lanes.empty());
        QCOMPARE(h_->row(1).top, before);
    }

    void automationIsSaved() {
        editor().setEnvelope(a_, kPan, env({{0.0, 0.1}, {4.0, 0.9, 0.3}}));
        editor().trySetTrackParam(kMaster, QStringLiteral("pan"), 0.25);
        editor().showAutomation(a_, kPan);
        const QString target = dir_->path(QStringLiteral("auto.gilproj"));
        QVERIFY(session().saveProjectAs(target));
        session().newProject();
        QVERIFY(session().openProject(target));
        h_->settle();
        const auto& reopened = project().tracks()[0];
        QCOMPARE(reopened.automation.value(kPan), env({{0.0, 0.1}, {4.0, 0.9, 0.3}}));
        QVERIFY(reopened.automationView.shown);
        QCOMPARE(project().master().pan, 0.25);
        QVERIFY(bridge().isAutomated(reopened.id, kPan));
        const auto areas = lanes()->envelopeAreas();
        QCOMPARE(areas.size(), size_t(1));
        QCOMPARE(areas[0].owner, reopened.id);
        QCOMPARE(areas[0].key, kPan);
    }

    void unlockedAutomationMovesWithADraggedClip() {
        editor().addMidiClip(a_, 0.0, 4.0);
        editor().setEnvelope(a_, kPan, env({{0.0, 0.0}, {4.0, 1.0}}));
        h_->settle();
        const int title = h_->row(0).top - arrangement()->scrollY() + 5;  // the clip's title bar
        const auto dragClip = [&](double start, double end) {
            drag(h_->at(QPointF(std::round(arrangement()->view().beatToX(start + 0.5)), title)),
                 h_->at(QPointF(std::round(arrangement()->view().beatToX(end + 0.5)), title)));
        };
        dragClip(0.0, 8.0);
        QCOMPARE(project().track(a_).clips[0].startBeat, 8.0);
        QCOMPARE(envelope(a_, kPan), env({{8.0, 0.0}, {12.0, 1.0}}));
        undo().undo();  // clip and automation together
        QCOMPARE(project().track(a_).clips[0].startBeat, 0.0);
        QCOMPARE(envelope(a_, kPan), env({{0.0, 0.0}, {4.0, 1.0}}));
        // Locked, the automation stays.
        editor().setAutomationLocked(true);
        dragClip(0.0, 8.0);
        QCOMPARE(project().track(a_).clips[0].startBeat, 8.0);
        QCOMPARE(envelope(a_, kPan), env({{0.0, 0.0}, {4.0, 1.0}}));
        editor().setAutomationLocked(false);
    }

    void laneScreenshot() {
        // Everything paints: lanes, curves, steps, readouts.
        selection().selectTrack(a_);
        const QString wave = automation::deviceKey(deviceOf(a_), QStringLiteral("wave"));
        editor().setEnvelope(a_, wave, env({{0.0, 0.0}, {8.0, 1.0}}));  // discrete: steps
        editor().setEnvelope(b_, kVolume, env({{0.0, 0.2, 0.8}, {8.0, 0.9}}));
        editor().addMidiClip(b_, 1.0, 6.0);
        editor().toggleAllAutomation();
        editor().showAutomation(a_, wave);
        editor().addAutomationLane(b_);
        h_->settle();
        const QPoint start = point(b_, 8.0, 0.9, kVolume);
        test::press(window(), start);
        test::moveTo(window(), start + QPoint(0, 20));  // a readout shows while dragging
        h_->settle();
        test::screenshot(window(), QStringLiteral("arrangement_automation_readout"));
        QVERIFY(!window()->grabWindow().isNull());
        test::release(window(), start + QPoint(0, 20));
    }

    void aDragFromALaneSelectsEveryLaneItCrosses() {
        // Where a drag starts decides what it selects: started on automation it
        // selects automation, up into the clips (or past the top track) and down
        // over other tracks: every automation lane it crosses, of every track.
        editor().addMidiClip(a_, 0.0, 8.0);
        editor().showAutomation(a_, kPan);
        editor().addAutomationLane(a_);
        editor().setAutomationLane(a_, 0, kVolume);  // a lane below a_
        editor().showAutomation(b_, kVolume);
        h_->settle();
        const QList<sub::app::LaneRef> all{{a_, kPan}, {a_, kVolume}, {b_, kVolume}};
        const auto x = [&](double beat) { return std::round(arrangement()->view().beatToX(beat)); };
        const QPoint start = point(b_, 1.0, 0.1, kVolume);
        const double clipsY = h_->row(0).top - arrangement()->scrollY() + 5;  // a_'s clips
        for (const QPoint end : {h_->at(QPointF(x(3.0), -10)),  // past the top track
                                 h_->at(QPointF(x(3.0), clipsY))}) {
            selection().clear();
            drag(start, end);
            QCOMPARE(selection().timeRange(), (sub::app::TimeRange{1.0, 3.0, {a_, b_}}));
            QCOMPARE(selection().lanes(), all);
            QVERIFY(!selection().clipRange() && selection().clips().isEmpty());
        }
        // Kept to its lane, it is a range on that lane.
        selection().clear();
        drag(start, point(b_, 3.0, 0.1, kVolume));
        QCOMPARE(selection().lanes(), (QList<sub::app::LaneRef>{{b_, kVolume}}));
        // Down from the top track's lane over the next track's.
        selection().clear();
        drag(point(a_, 1.0, 0.1, kPan), point(b_, 3.0, 0.9, kVolume));
        QCOMPARE(selection().lanes(), all);
        QCOMPARE(selection().rangeTrackIds(), (QStringList{a_, b_}));
        test::screenshot(window(), QStringLiteral("arrangement_lane_range_over_tracks"));
    }

    void shiftClickExtendsALaneRangeOverTheLanesBetween() {
        // Shift-click: a lane range goes on to where the click is, over every
        // automation lane between (and the time between); a click on the clips
        // takes in the lanes up to there, as a drag would have.
        editor().addMidiClip(a_, 0.0, 8.0);
        editor().showAutomation(a_, kPan);
        editor().showAutomation(b_, kVolume);
        h_->settle();
        drag(point(b_, 2.0, 0.1, kVolume), point(b_, 3.0, 0.1, kVolume));
        QCOMPARE(selection().lanes(), (QList<sub::app::LaneRef>{{b_, kVolume}}));
        click(point(a_, 6.0, 0.1, kPan), Qt::ShiftModifier);
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{2.0, 6.0, {a_, b_}}));
        QCOMPARE(selection().lanes(), (QList<sub::app::LaneRef>{{a_, kPan}, {b_, kVolume}}));
        QVERIFY(!selection().clipRange());
        QCOMPARE(selection().insertBeat(), 2.0);
        // Shift-clicked in the clips' band (beside the clip), it stays automation:
        // the lanes up to there.
        const auto band = [&](double beat) {
            return h_->at(QPointF(std::round(arrangement()->view().beatToX(beat)),
                                  h_->row(0).top + 5 - arrangement()->scrollY()));
        };
        selection().clear();
        drag(point(b_, 2.0, 0.1, kVolume), point(b_, 3.0, 0.1, kVolume));
        click(band(10.0), Qt::ShiftModifier);
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{2.0, 10.0, {a_, b_}}));
        QCOMPARE(selection().lanes(), (QList<sub::app::LaneRef>{{a_, kPan}, {b_, kVolume}}));
        // A clip range Shift-clicked on a lane goes on as a clip range, over the tracks to there.
        drag(band(9.0), band(10.0));
        QVERIFY(selection().clipRange());
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{9.0, 10.0, {a_}}));
        click(point(b_, 4.0, 0.1, kVolume), Qt::ShiftModifier);
        QCOMPARE(selection().timeRange(), (sub::app::TimeRange{4.0, 10.0, {a_, b_}}));
        QVERIFY(selection().clipRange() && selection().lanes().isEmpty());
        QCOMPARE(project().track(a_).clips.size(), size_t(1));  // (nothing moved)
    }

    void draggingNearASegmentMovesItsTwoBreakpoints() {
        editor().showAutomation(a_, kPan);
        editor().setEnvelope(a_, kPan, env({{0.0, 0.2}, {2.0, 0.4}, {6.0, 0.4}, {8.0, 0.8}}));
        h_->settle();
        const QPoint near_ = point(a_, 4.0, 0.4) + QPoint(0, 10);  // below the line, not on it
        hover(near_);
        QVERIFY(lanes()->hoverPoint() && lanes()->hoverPoint()->kind == arr::Hover::Kind::Segment);
        QCOMPARE(lanes()->hoverPoint()->index, std::optional<int>(1));
        QCOMPARE(lanes()->cursor().shape(), Qt::ArrowCursor);  // only the segment lights up
        hover(point(a_, 4.0, 0.4) + QPoint(0, 30));
        QVERIFY(!lanes()->hoverPoint());
        // A click selects the two; a drag moves them together, as one undo step.
        click(near_);
        QCOMPARE(selection().points(), std::make_optional(points(a_, kPan, {1, 2})));
        QCOMPARE(envelope(a_, kPan).size(), size_t(4));
        const int steps = undo().count();
        const double height = area(a_).values().height();
        drag(near_, near_ + QPoint(0, -int(std::round(height * 0.25))));
        const Envelope points = envelope(a_, kPan);
        QCOMPARE(beats(points), (std::vector<double>{0.0, 2.0, 6.0, 8.0}));
        QVERIFY(near(points[1].value, 0.65, 0.02));
        QVERIFY(near(points[2].value, 0.65, 0.02));
        QCOMPARE(points[0].value, 0.2);
        QCOMPARE(points[3].value, 0.8);
        QCOMPARE(undo().count(), steps + 1);
    }

    void draggingASelectedRangeMovesItsAutomation() {
        editor().showAutomation(a_, kPan);
        const Envelope ramp = env({{0.0, 0.0}, {8.0, 1.0}});
        editor().setEnvelope(a_, kPan, ramp);
        h_->settle();
        drag(point(a_, 2.0, 0.9), point(a_, 4.0, 0.9));  // select 2..4
        QCOMPARE(selection().rangeStart(), 2.0);
        QCOMPARE(selection().rangeEnd(), 4.0);
        const QPoint inside = point(a_, 3.0, 0.8);
        hover(inside);
        QVERIFY(lanes()->hoverPoint() && lanes()->hoverPoint()->kind == arr::Hover::Kind::Range);
        QCOMPARE(lanes()->cursor().shape(), Qt::ArrowCursor);
        // Up: the inside moves, with steps at the edges; the outside stays.
        const double height = area(a_).values().height();
        const int steps = undo().count();
        drag(inside, inside + QPoint(0, -int(std::round(height * 0.25))));
        Envelope points = envelope(a_, kPan);
        QVERIFY(near(*automation::valueAt(points, 3.0), 0.625, 0.02));
        QVERIFY(near(*automation::valueAt(points, 1.0), 0.125));
        QVERIFY(near(*automation::valueAt(points, 5.0), 0.625));
        const std::vector<double> at = beats(points);
        QCOMPARE(std::count(at.begin(), at.end(), 2.0), 2);
        QCOMPARE(std::count(at.begin(), at.end(), 4.0), 2);
        QCOMPARE(selection().rangeStart(), 2.0);
        QCOMPARE(undo().count(), steps + 1);
        undo().undo();
        QCOMPARE(envelope(a_, kPan), ramp);
        // Right: it lands 2 beats later (over what was there); the range goes along.
        drag(inside, point(a_, 5.0, 0.8));
        points = envelope(a_, kPan);
        QVERIFY(near(*automation::valueAt(points, 5.0), 0.375));  // what was at 3
        QVERIFY(near(*automation::valueAt(points, 7.0), 0.875));  // the rest as it was
        QCOMPARE(selection().rangeStart(), 4.0);
        QCOMPARE(selection().rangeEnd(), 6.0);
    }

    void aStepIsASegmentToDrag() {
        editor().showAutomation(a_, kPan);
        editor().setEnvelope(a_, kPan, env({{0.0, 0.2}, {4.0, 0.2}, {4.0, 0.8}, {8.0, 0.8}}));
        h_->settle();
        for (const QPoint at : {point(a_, 4.0, 0.5), point(a_, 4.0, 0.5) + QPoint(8, 0)}) {  // on it, and beside
            hover(at);
            QVERIFY(lanes()->hoverPoint() && lanes()->hoverPoint()->kind == arr::Hover::Kind::Segment);
            QCOMPARE(lanes()->hoverPoint()->index, std::optional<int>(1));
        }
        drag(point(a_, 4.0, 0.5), point(a_, 6.0, 0.5));
        QCOMPARE(envelope(a_, kPan), env({{0.0, 0.2}, {6.0, 0.2}, {6.0, 0.8}, {8.0, 0.8}}));
    }

    void pointsInASelectedRangeMoveOnTheirOwn() {
        editor().showAutomation(a_, kPan);
        editor().setEnvelope(a_, kPan, env({{0.0, 0.5}, {2.5, 0.8}, {3.5, 0.2}, {6.0, 0.5}}));
        h_->settle();
        drag(point(a_, 2.0, 0.02), point(a_, 4.0, 0.02));  // select 2..4
        const double height = area(a_).values().height();
        drag(point(a_, 3.0, 0.95), point(a_, 3.0, 0.95) + QPoint(0, int(std::round(height * 0.1))));
        const Envelope moved = envelope(a_, kPan);
        QCOMPARE(selection().rangeStart(), 2.0);
        QCOMPARE(selection().rangeEnd(), 4.0);
        const auto peak = std::find_if(moved.begin(), moved.end(), [](const AutomationPoint& p) { return p.beat == 2.5; });
        QVERIFY(peak != moved.end());
        // A breakpoint in the range: just it moves (the range doesn't).
        drag(point(a_, 2.5, peak->value), point(a_, 2.5, peak->value - 0.2));
        const Envelope after = envelope(a_, kPan);
        QCOMPARE(beats(after), beats(moved));
        int changed = 0;
        for (size_t i = 0; i < moved.size(); ++i) {
            if (moved[i] != after[i]) {
                ++changed;
                QCOMPARE(moved[i].beat, 2.5);
            }
        }
        QCOMPARE(changed, 1);
    }

    void draggedBreakpointsOverrideWhereTheyLand() {
        editor().showAutomation(a_, kPan);
        editor().setEnvelope(a_, kPan,
                             env({{0.0, 0.5}, {1.0, 0.5}, {2.0, 0.5}, {4.0, 0.9}, {5.0, 0.1}, {6.0, 0.5}}));
        h_->settle();
        const QPoint start = point(a_, 1.5, 0.5) + QPoint(0, 10);  // the segment from 1 to 2
        drag(start, start + QPoint(int(std::round(arrangement()->pxPerBeat() * 3.5)), 0));
        QCOMPARE(beats(envelope(a_, kPan)), (std::vector<double>{0.0, 4.0, 4.5, 5.5, 6.0}));  // the one at 5 went
        QCOMPARE(selection().points(), std::make_optional(points(a_, kPan, {2, 3})));  // still the pair
    }
};

QTEST_MAIN(TestUiArrangementAutomation)
#include "test_ui_arrangement_automation.moc"
