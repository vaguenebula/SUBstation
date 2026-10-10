// Multiband Dynamics' editor (ui/qml/devices/editors/MultibandEditor.qml,
// ui/src/devices/MultibandGraph): loaded as the device view loads it, over a
// real engine; its controls bound to their parameters (undoably, and the engine
// has what they set), the graph's drags, double-clicks and wheel (one undo step
// a gesture), the displays reaching the graph as the engine renders, and its
// animation easing and settling. Also the `ratio` unit and the ratios typed. With
// SUBSTATION_UI_SCREENSHOTS set to a folder, the editor is saved there.

#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <vector>

#include "EditorHarness.h"
#include "audio/EngineBridge.h"
#include "audio/MultibandResponse.h"
#include "controls/KnobItem.h"
#include "controls/ValueBoxItem.h"
#include "devices/MultibandGraph.h"
#include "editor/ProjectEditor.h"
#include "model/ParamSpec.h"

using namespace sub::app;
using namespace sub::ui;
using sub::app::test::kSampleRate;

namespace {

constexpr int kLow = MultibandGraph::Low, kMid = MultibandGraph::Mid, kHigh = MultibandGraph::High;
constexpr int kBelow = MultibandGraph::Below, kAbove = MultibandGraph::Above;

}  // namespace

class TestUiDeviceEditorsMultiband : public QObject, public sub::app::test::EditorHarness {
    Q_OBJECT

    // A track playing `mono` with a Multiband Dynamics on it, `settings` set first, its editor shown.
    struct Shown {
        QString track, device;
        QQuickItem* view = nullptr;
        MultibandGraph* graph = nullptr;
    };
    Shown showDevice(const sub::app::OrderedMap<QString, double>& settings = {},
                     const std::vector<float>& mono = tone(1000.0, kSampleRate)) {
        Shown shown;
        shown.track = audioTrackWith(mono, QStringLiteral("tone"), 1.0);
        if (shown.track.isEmpty())
            return shown;
        shown.device = editor()->addDevice(shown.track, QStringLiteral("multiband"));
        if (!settings.isEmpty())
            editor()->setDeviceParams(shown.track, shown.device, settings);
        undo()->clear();
        shown.view = show(QStringLiteral("multiband"), shown.track, shown.device);
        if (shown.view)
            shown.graph = find<MultibandGraph>(shown.view, QStringLiteral("multibandGraph"));
        return shown;
    }

    ValueBoxItem* box(QQuickItem* view, const char* name) {
        QQuickItem* item = find(view, QString::fromLatin1(name));
        return item ? qvariant_cast<ValueBoxItem*>(item->property("box")) : nullptr;
    }
    QQuickItem* button(QQuickItem* view, const char* name) {
        QQuickItem* item = find(view, QString::fromLatin1(name));
        return item ? qvariant_cast<QQuickItem*>(item->property("button")) : nullptr;
    }
    void click(QQuickItem* item) { QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(item)); }
    void ticks(int count) {
        for (int i = 0; i < count; ++i)
            refreshDisplays();
    }
    // A drag on the graph from `from` by `by`, in `steps` moves.
    void drag(MultibandGraph* graph, QPointF from, QPoint by, int steps = 3,
              Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        const QPoint at = scenePoint(graph, from);
        QTest::mousePress(window_, Qt::LeftButton, modifiers, at);
        for (int i = 1; i <= steps; ++i)
            dragTo(at + by * i / steps, modifiers);
        QTest::mouseRelease(window_, Qt::LeftButton, modifiers, at + by);
    }
    // One undo step more than `before`, nothing to redo.
    bool oneStepAfter(int before) { return undo()->index() == before + 1 && undo()->count() == undo()->index(); }

private Q_SLOTS:
    void initTestCase() {
        if (!haveDisplay())
            QSKIP("needs a display: the offscreen platform renders Qt Quick in software, without this geometry");
        startHost();
    }

    void cleanupTestCase() { stopHost(); }

    void init() { clearHost(); }

    void fitsAndShowsEveryControl() {
        const Shown s = showDevice();
        QVERIFY(s.view && s.graph);
        QVERIFY2(s.view->implicitHeight() <= bodyHeight(),
                 qPrintable(QStringLiteral("%1 > %2").arg(s.view->implicitHeight()).arg(bodyHeight())));
        QCOMPARE(s.view->implicitWidth(), 780.0);

        // The device's own controls: within the body, their rows one under the other.
        QQuickItem* globals = find(s.view, QStringLiteral("globals"));
        QVERIFY(globals);
        QVERIFY(globals->y() + globals->height() <= s.view->height() - 6);
        double bottom = 0.0;
        for (QQuickItem* row : globals->childItems()) {
            if (!row->isVisible())
                continue;
            QVERIFY(row->y() >= bottom);
            bottom = row->y() + row->height();
        }

        for (const char* name : {"highOn", "midLabel", "lowOn", "highSolo", "midSolo", "lowSolo", "xoverHigh",
                                 "xoverLow", "highIn", "midIn", "lowIn", "highOut", "midOut", "lowOut", "highAbove",
                                 "midAbove", "lowAbove", "highAboveRatio", "midAboveRatio", "lowAboveRatio",
                                 "highBelow", "midBelow", "lowBelow", "highBelowRatio", "midBelowRatio",
                                 "lowBelowRatio", "highAttack", "midAttack", "lowAttack", "highRelease", "midRelease",
                                 "lowRelease", "pageTime", "pageBelow", "pageAbove", "amount", "time", "output",
                                 "softKnee", "modePeak", "modeRms", "scGain", "scMix", "sidechainButton",
                                 "multibandGraph"})
            QVERIFY2(find(s.view, QString::fromLatin1(name)), name);
        QVERIFY(!find(s.view, QStringLiteral("midLabel"))->property("param").isValid());  // Mid can't be switched off

        // The boxes read their parameters' defaults, in their units.
        QCOMPARE(box(s.view, "midAbove")->value(), -20.0);
        QCOMPARE(box(s.view, "midAbove")->text(), QStringLiteral("-20.0 dB"));
        QCOMPARE(box(s.view, "midBelow")->value(), -40.0);
        QCOMPARE(box(s.view, "midAboveRatio")->value(), 1.0);
        QCOMPARE(box(s.view, "midAboveRatio")->text(), QStringLiteral("1.00:1"));
        QCOMPARE(box(s.view, "xoverLow")->value(), 120.0);
        QCOMPARE(box(s.view, "xoverHigh")->value(), 2500.0);
        QCOMPARE(box(s.view, "xoverHigh")->text(), QStringLiteral("2.50 kHz"));
        QCOMPARE(box(s.view, "midAttack")->text(), QStringLiteral("10 ms"));
        auto* amount = qvariant_cast<KnobItem*>(
            qvariant_cast<QQuickItem*>(find(s.view, QStringLiteral("amount"))->property("knob"))->property("knob"));
        QVERIFY(amount);
        QCOMPARE(amount->value(), 100.0);

        // The rows line up with the graph's lanes.
        for (const auto& [name, band] :
             {std::pair{"highIn", kHigh}, std::pair{"midIn", kMid}, std::pair{"lowIn", kLow}}) {
            QQuickItem* in = find(s.view, QString::fromLatin1(name));
            const double y = in->mapToScene(QPointF(0, in->height() / 2)).y();
            const QRectF lane = s.graph->lane(band);
            const double top = s.graph->mapToScene(lane.topLeft()).y(),
                         under = s.graph->mapToScene(lane.bottomLeft()).y();
            QVERIFY2(top < y && y < under, name);
        }
        QCOMPARE(s.graph->rowHeight(), (s.view->height() - 12 - 16) / 3);

        // The fields: Above's to start with, then Time's, then Below's.
        auto visible = [&](const char* name) { return find(s.view, QString::fromLatin1(name))->isVisible(); };
        QVERIFY(visible("midAbove") && visible("midAboveRatio"));
        QVERIFY(!visible("midBelow") && !visible("midAttack"));
        QVERIFY(find(s.view, QStringLiteral("pageAbove"))->property("checked").toBool());
        click(find(s.view, QStringLiteral("pageTime")));
        QVERIFY(visible("midAttack") && visible("midRelease") && visible("highAttack"));
        QVERIFY(!visible("midAbove") && !visible("midBelow"));
        QVERIFY(find(s.view, QStringLiteral("pageTime"))->property("checked").toBool());
        QVERIFY(!find(s.view, QStringLiteral("pageAbove"))->property("checked").toBool());
        click(find(s.view, QStringLiteral("pageBelow")));
        QVERIFY(visible("midBelow") && visible("lowBelowRatio"));
        QVERIFY(!visible("midAttack") && !visible("midAbove"));
        QCOMPARE(undo()->count(), 0);  // (a view, not an edit)

        // At the least height it asks for, the rows and the device's controls still fit, apart.
        QQuickItem* compact = show(QStringLiteral("multiband"), s.track, s.device, int(s.view->implicitHeight()));
        QVERIFY(compact);
        QQuickItem* low = find(compact, QStringLiteral("xoverLow"));
        QVERIFY(low->mapToItem(compact, QPointF(0, low->height())).y() <= compact->height() - 6 + 0.5);
        globals = find(compact, QStringLiteral("globals"));
        QVERIFY(globals->y() + globals->height() <= compact->height() - 6);
        auto* graph = find<MultibandGraph>(compact, QStringLiteral("multibandGraph"));
        QVERIFY(graph->rowHeight() >= MultibandGraph::kMinRowHeight);
        QTest::qWait(30);
        save(grab(), QStringLiteral("multiband-compact.png"));
    }

    void controlsAreUndoable() {
        const Shown s = showDevice();
        QVERIFY(s.view && s.graph);
        auto value = [&](const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); };

        // A box dragged: one undo step.
        {
            const int before = undo()->index();
            const QPoint at = centerOf(box(s.view, "midIn"));
            QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
            for (int dy = 20; dy <= 40; dy += 20)
                dragTo(at - QPoint(0, dy));
            QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 40));
            QVERIFY2(std::abs(value("mid_in") - 1.0) < 0.051, qPrintable(QString::number(value("mid_in"))));
            QVERIFY(oneStepAfter(before));
            undo()->undo();
            QCOMPARE(value("mid_in"), 0.0);
        }

        // High switched off: its lane and its controls dim (still editable), its solo can't be used.
        {
            const int before = undo()->index();
            click(button(s.view, "highOn"));
            QCOMPARE(value("high_on"), 0.0);
            QVERIFY(oneStepAfter(before));
            QVERIFY(!find(s.view, QStringLiteral("highOn"))->property("lit").toBool());
            QVERIFY(!s.graph->bandOn(kHigh));
            QVERIFY(!find(s.view, QStringLiteral("highSolo"))->isEnabled());
            QVERIFY(find(s.view, QStringLiteral("midSolo"))->isEnabled());
            QTRY_COMPARE(find(s.view, QStringLiteral("highAbove"))->opacity(), 0.45);
            QTRY_COMPARE(find(s.view, QStringLiteral("xoverHigh"))->opacity(), 0.45);
            QCOMPARE(find(s.view, QStringLiteral("midAbove"))->opacity(), 1.0);
            undo()->undo();
            QCOMPARE(value("high_on"), 1.0);
            QVERIFY(s.graph->bandOn(kHigh));
            QTRY_COMPARE(find(s.view, QStringLiteral("highAbove"))->opacity(), 1.0);
        }

        // Solo, Peak/RMS, Soft Knee: a click, a step each.
        int before = undo()->index();
        click(button(s.view, "midSolo"));
        QCOMPARE(value("mid_solo"), 1.0);
        QVERIFY(oneStepAfter(before));
        before = undo()->index();
        click(button(s.view, "modePeak"));
        QCOMPARE(value("mode"), 0.0);
        QVERIFY(find(s.view, QStringLiteral("modePeak"))->property("lit").toBool());
        QVERIFY(!find(s.view, QStringLiteral("modeRms"))->property("lit").toBool());
        QVERIFY(oneStepAfter(before));
        before = undo()->index();
        click(button(s.view, "softKnee"));
        QCOMPARE(value("soft_knee"), 1.0);
        QVERIFY(oneStepAfter(before));

        // Ratios typed as Live and the house print them.
        ValueBoxItem* ratio = box(s.view, "midAboveRatio");
        QVERIFY(ratio);
        before = undo()->index();
        QVERIFY(ratio->applyTyped(QStringLiteral("1:2")));
        QCOMPARE(value("mid_above_ratio"), 0.5);
        QCOMPARE(ratio->text(), QStringLiteral("1:2.00"));
        QVERIFY(oneStepAfter(before));
        before = undo()->index();
        QVERIFY(ratio->applyTyped(QStringLiteral("1:3")));
        QVERIFY(std::abs(value("mid_above_ratio") - 0.333) < 1e-9);
        QCOMPARE(ratio->text(), QStringLiteral("1:3.00"));
        QVERIFY(oneStepAfter(before));
        before = undo()->index();
        QVERIFY(ratio->applyTyped(QStringLiteral("4:1")));
        QCOMPARE(value("mid_above_ratio"), 4.0);
        QCOMPARE(ratio->text(), QStringLiteral("4.00:1"));
        QVERIFY(oneStepAfter(before));
        before = undo()->index();
        QVERIFY(!ratio->applyTyped(QStringLiteral("x")));
        QCOMPARE(value("mid_above_ratio"), 4.0);
        QCOMPARE(undo()->index(), before);
        // A crossover typed as a frequency.
        QVERIFY(box(s.view, "xoverLow")->applyTyped(QStringLiteral("0.2k")));
        QCOMPARE(value("xover_low"), 200.0);
        QCOMPARE(box(s.view, "xoverLow")->text(), QStringLiteral("200 Hz"));

        // The engine has what the editor set.
        const auto id = bridge()->engineDeviceId(s.track, s.device);
        QVERIFY(id);
        for (const char* p : {"mid_above_ratio", "mid_solo", "mode", "soft_knee", "xover_low", "high_on"})
            QCOMPARE(engine_->processorParam(*id, engine_->processorParamIndex(*id, p)), float(value(p)));
    }

    void graphDragsAThreshold() {
        const Shown s = showDevice();
        QVERIFY(s.view && s.graph);
        auto value = [&](const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); };
        MultibandGraph* graph = s.graph;

        int before = undo()->index();
        const QPoint at = scenePoint(graph, graph->aboveHandle(kMid));
        drag(graph, graph->aboveHandle(kMid), QPoint(-30, 0));
        const double expected = -20.0 - 30.0 / graph->pixelsPerDb();
        QVERIFY2(std::abs(value("mid_above") - expected) <= 0.2, qPrintable(QString::number(value("mid_above"))));
        QCOMPARE(value("mid_below"), -40.0);
        QVERIFY(oneStepAfter(before));
        QVERIFY(std::abs(scenePoint(graph, graph->aboveHandle(kMid)).x() - (at.x() - 30)) <= 1);
        QCOMPARE(value("low_above"), -20.0);  // (only the band grabbed)
        undo()->undo();
        QCOMPARE(value("mid_above"), -20.0);

        // Above dragged past Below pushes it along: still one step.
        before = undo()->index();
        const double to = graph->xOfDb(-50.0) - graph->aboveHandle(kMid).x();
        drag(graph, graph->aboveHandle(kMid), QPoint(int(std::lround(to)), 0), 4);
        QVERIFY2(std::abs(value("mid_above") + 50.0) <= 0.2, qPrintable(QString::number(value("mid_above"))));
        QCOMPARE(value("mid_below"), value("mid_above"));
        QVERIFY(oneStepAfter(before));

        // Between the thresholds the press isn't the graph's (the frame's: selecting the device).
        before = undo()->index();
        undo()->undo();
        const QPointF between((graph->xOfDb(-40.0) + graph->xOfDb(-20.0)) / 2, graph->lane(kMid).center().y());
        drag(graph, between, QPoint(-20, 20));
        QCOMPARE(value("mid_above"), -20.0);
        QCOMPARE(value("mid_below"), -40.0);
        QCOMPARE(undo()->index(), before - 1);

        // The engine has what the graph set.
        drag(graph, graph->belowHandle(kLow), QPoint(-20, 0));
        const auto id = bridge()->engineDeviceId(s.track, s.device);
        QVERIFY(id);
        QCOMPARE(engine_->processorParam(*id, engine_->processorParamIndex(*id, "low_below")),
                 float(value("low_below")));
        QVERIFY(value("low_below") < -44.0);
    }

    void graphDragsARatio() {
        const Shown s = showDevice();
        QVERIFY(s.view && s.graph);
        auto value = [&](const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); };
        MultibandGraph* graph = s.graph;

        // Above: down compresses, up expands; all one step.
        int before = undo()->index();
        {
            const QPoint at = scenePoint(graph, graph->aboveBlockPoint(kMid));
            QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
            for (int dy = 10; dy <= 30; dy += 10)
                dragTo(at + QPoint(0, dy));
            QVERIFY2(std::abs(value("mid_above_ratio") - 2.0) <= 0.02,
                     qPrintable(QString::number(value("mid_above_ratio"))));
            for (int dy = 20; dy >= -30; dy -= 10)
                dragTo(at + QPoint(0, dy));
            QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 30));
            QVERIFY2(std::abs(value("mid_above_ratio") - 0.5) <= 0.01,
                     qPrintable(QString::number(value("mid_above_ratio"))));
        }
        QVERIFY(oneStepAfter(before));

        // Below: up lifts the quiet parts (upward compression).
        before = undo()->index();
        drag(graph, graph->belowBlockPoint(kMid), QPoint(0, -30));
        QVERIFY2(std::abs(value("mid_below_ratio") - 2.0) <= 0.02,
                 qPrintable(QString::number(value("mid_below_ratio"))));
        QVERIFY(oneStepAfter(before));

        // A pixel from 1:1 is 1:1 exactly (the detent).
        drag(graph, graph->aboveBlockPoint(kLow), QPoint(0, 1), 1);
        QCOMPARE(value("low_above_ratio"), 1.0);
        // Shift: finely (a fifth as far).
        drag(graph, graph->aboveBlockPoint(kLow), QPoint(0, -30), 3, Qt::ShiftModifier);
        QVERIFY2(std::abs(value("low_above_ratio") - std::pow(2.0, -0.2)) <= 0.01,
                 qPrintable(QString::number(value("low_above_ratio"))));

        // A band switched off keeps its settings, and its lane still takes drags.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("high_on"), 0.0);
        before = undo()->index();
        drag(graph, graph->aboveBlockPoint(kHigh), QPoint(0, 30));
        QVERIFY2(std::abs(value("high_above_ratio") - 2.0) <= 0.02,
                 qPrintable(QString::number(value("high_above_ratio"))));
        QVERIFY(oneStepAfter(before));

        const auto id = bridge()->engineDeviceId(s.track, s.device);
        QVERIFY(id);
        for (const char* p : {"mid_above_ratio", "mid_below_ratio", "high_above_ratio"})
            QCOMPARE(engine_->processorParam(*id, engine_->processorParamIndex(*id, p)), float(value(p)));
    }

    void modifiedDrags() {
        const Shown s = showDevice();
        QVERIFY(s.view && s.graph);
        auto value = [&](const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); };
        MultibandGraph* graph = s.graph;

        // Ctrl: every band's Above, by as much.
        int before = undo()->index();
        drag(graph, graph->aboveHandle(kMid), QPoint(-20, 0), 2, Qt::ControlModifier);
        const double moved = -20.0 - 20.0 / graph->pixelsPerDb();
        for (const char* id : {"low_above", "mid_above", "high_above"})
            QVERIFY2(std::abs(value(id) - moved) <= 0.2, id);
        QVERIFY(oneStepAfter(before));
        undo()->undo();
        for (const char* id : {"low_above", "mid_above", "high_above"})
            QCOMPARE(value(id), -20.0);

        // Alt: both thresholds of the band, the gap kept; stopped at the edge as a pair.
        before = undo()->index();
        drag(graph, graph->aboveHandle(kMid), QPoint(20, 0), 2, Qt::AltModifier);
        QVERIFY2(std::abs(value("mid_above") - (-20.0 + 20.0 / graph->pixelsPerDb())) <= 0.2,
                 qPrintable(QString::number(value("mid_above"))));
        QVERIFY(std::abs((value("mid_above") - value("mid_below")) - 20.0) < 1e-9);
        QVERIFY(oneStepAfter(before));
        before = undo()->index();
        drag(graph, graph->aboveHandle(kMid), QPoint(120, 0), 3, Qt::AltModifier);
        QCOMPARE(value("mid_above"), 0.0);
        QCOMPARE(value("mid_below"), -20.0);
        QVERIFY(oneStepAfter(before));
        QCOMPARE(value("low_above"), -20.0);

        // Ctrl on a ratio: every band's, times as much.
        before = undo()->index();
        drag(graph, graph->belowBlockPoint(kLow), QPoint(0, -30), 3, Qt::ControlModifier);
        for (const char* id : {"low_below_ratio", "mid_below_ratio", "high_below_ratio"})
            QVERIFY2(std::abs(value(id) - 2.0) <= 0.02, id);
        QVERIFY(oneStepAfter(before));
    }

    void doubleClickAndWheel() {
        const Shown s = showDevice({{QStringLiteral("mid_above"), -35.0}, {QStringLiteral("mid_above_ratio"), 4.0}});
        QVERIFY(s.view && s.graph);
        auto value = [&](const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); };
        MultibandGraph* graph = s.graph;
        QCOMPARE(value("mid_above"), -35.0);

        int before = undo()->index();
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, scenePoint(graph, graph->aboveHandle(kMid)));
        QCOMPARE(value("mid_above"), -20.0);
        QVERIFY(oneStepAfter(before));
        before = undo()->index();
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, scenePoint(graph, graph->aboveBlockPoint(kMid)));
        QCOMPARE(value("mid_above_ratio"), 1.0);
        QVERIFY(oneStepAfter(before));
        // A threshold reset under the other pushes it along.
        editor()->setDeviceParams(s.track, s.device,
                                  {{QStringLiteral("low_below"), -10.0}, {QStringLiteral("low_above"), -5.0}});
        before = undo()->index();
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, scenePoint(graph, graph->aboveHandle(kLow)));
        QCOMPARE(value("low_above"), -20.0);
        QCOMPARE(value("low_below"), -20.0);
        QVERIFY(oneStepAfter(before));

        // The wheel over a block: louder in that region (above: towards expansion), the notches one step.
        before = undo()->index();
        const QPoint block = scenePoint(graph, graph->aboveBlockPoint(kMid));
        for (int i = 0; i < 3; ++i)
            wheel(block, 120);
        QVERIFY2(std::abs(value("mid_above_ratio") - 0.771) <= 0.005,
                 qPrintable(QString::number(value("mid_above_ratio"))));
        QVERIFY(oneStepAfter(before));
        // Over a handle: half a dB a notch.
        before = undo()->index();
        wheel(scenePoint(graph, graph->aboveHandle(kHigh)), -120);
        QCOMPARE(value("high_above"), -20.5);
        QVERIFY(oneStepAfter(before));
    }

    void displaysReachTheGraph() {
        // One band (High and Low off), Peak, Above -20 at 4:1; a tone at 0.5 (-6.02 dB).
        const Shown s = showDevice({{QStringLiteral("low_on"), 0.0},
                                    {QStringLiteral("high_on"), 0.0},
                                    {QStringLiteral("mode"), 0.0},
                                    {QStringLiteral("mid_above"), -20.0},
                                    {QStringLiteral("mid_above_ratio"), 4.0}},
                                   tone(1000.0, kSampleRate, 0.5));
        QVERIFY(s.view && s.graph);
        MultibandGraph* graph = s.graph;
        // Shown as it is, not fading in.
        QVERIFY(!graph->bandOn(kHigh) && !graph->bandOn(kLow) && graph->bandOn(kMid));
        QCOMPARE(graph->laneOpacity(kHigh), 0.35);
        QCOMPARE(graph->laneOpacity(kLow), 0.35);
        QCOMPARE(graph->laneOpacity(kMid), 1.0);
        ticks(30);
        QVERIFY(!graph->animating());  // (nothing playing: nothing moves)

        engine()->renderOffline(0.0, kSampleRate / 2);
        refreshDisplays();
        const double in = 20 * std::log10(0.5);
        const double gain = (in + 20.0) * (1.0 / 4.0 - 1.0);
        QVERIFY2(std::abs(graph->inLevel(kMid) - in) < 0.3, qPrintable(QString::number(graph->inLevel(kMid))));
        QVERIFY2(std::abs(graph->gainTarget(kMid) - gain) < 0.3, qPrintable(QString::number(graph->gainTarget(kMid))));
        QVERIFY2(std::abs(graph->outLevel(kMid) - (in + gain)) < 0.3,
                 qPrintable(QString::number(graph->outLevel(kMid))));
        QVERIFY(graph->animating());
        // The gain eases there.
        QVERIFY(graph->gainShown(kMid) < 0.0 && graph->gainShown(kMid) > graph->gainTarget(kMid));
        ticks(12);  // (no new audio: the readings held)
        QVERIFY(std::abs(graph->gainShown(kMid) - graph->gainTarget(kMid)) < 0.1);
        QVERIFY2(graph->glow(kMid, kAbove) > 0.9, qPrintable(QString::number(graph->glow(kMid, kAbove))));
        QCOMPARE(graph->glow(kMid, kBelow), 0.0);  // (1:1: doing nothing)
        QCOMPARE(graph->glow(kHigh, kAbove), 0.0);
        // The level the static curve gives is the engine's.
        QCOMPARE(graph->staticOutDb(kMid, in), in + multibandGainDb(in, -20.0, 4.0, -40.0, 1.0, false, 100.0));
        QVERIFY(std::abs(graph->staticOutDb(kMid, in) - (in + gain)) < 0.01);

        // The audio stopped: the meters fall, the gain eases home, the glow fades.
        ticks(75);
        QVERIFY2(graph->outLevel(kMid) < -46.0, qPrintable(QString::number(graph->outLevel(kMid))));
        QVERIFY(std::abs(graph->gainShown(kMid)) < 0.1);
        QVERIFY2(graph->glow(kMid, kAbove) < 0.1, qPrintable(QString::number(graph->glow(kMid, kAbove))));
        // And once all is still, it stops repainting.
        ticks(300);
        QVERIFY(!graph->animating());
        QCOMPARE(graph->inLevel(kMid), MultibandGraph::kFloorDb);
        QCOMPARE(graph->outPeak(kMid), MultibandGraph::kFloorDb);
    }

    void lanesAndHighlightsEase() {
        const Shown s = showDevice();
        QVERIFY(s.view && s.graph);
        MultibandGraph* graph = s.graph;
        QCOMPARE(graph->laneOpacity(kHigh), 1.0);

        click(button(s.view, "highOn"));
        QVERIFY(!graph->bandOn(kHigh));
        refreshDisplays();
        QVERIFY(graph->laneOpacity(kHigh) < 1.0 && graph->laneOpacity(kHigh) > 0.35);
        ticks(25);
        QVERIFY(std::abs(graph->laneOpacity(kHigh) - 0.35) < 0.01);

        click(button(s.view, "midSolo"));  // Low muted by it (High is off: it follows Mid)
        refreshDisplays();
        QVERIFY(graph->laneOpacity(kLow) < 1.0 && graph->laneOpacity(kLow) > 0.5);
        ticks(25);
        QVERIFY(std::abs(graph->laneOpacity(kLow) - 0.5) < 0.01);
        QCOMPARE(graph->laneOpacity(kMid), 1.0);

        // Under the mouse, a handle lights up and the cursor says what a drag does.
        QTest::mouseMove(window_, scenePoint(graph, graph->aboveHandle(kMid)));
        ticks(10);
        QVERIFY2(graph->highlight(kMid, kAbove, true) > 0.9,
                 qPrintable(QString::number(graph->highlight(kMid, kAbove, true))));
        QCOMPARE(graph->cursor().shape(), Qt::SizeHorCursor);
        QTest::mouseMove(window_, scenePoint(graph, graph->aboveBlockPoint(kMid)));
        QCOMPARE(graph->cursor().shape(), Qt::SizeVerCursor);
        ticks(25);
        QVERIFY(graph->highlight(kMid, kAbove, true) < 0.1);
        QVERIFY(graph->highlight(kMid, kAbove, false) > 0.9);
        QTest::mouseMove(window_, QPoint(1, 1));
        ticks(60);
        QVERIFY(graph->highlight(kMid, kAbove, false) == 0.0);
        QVERIFY(!graph->animating());
    }

    void sidechainControls() {
        const Shown s = showDevice();
        QVERIFY(s.view && s.graph);
        QVERIFY(!s.graph->sidechained());
        QVERIFY(!find(s.view, QStringLiteral("scGain"))->isEnabled());
        QVERIFY(!find(s.view, QStringLiteral("scMix"))->isEnabled());
        QVERIFY(!find(s.view, QStringLiteral("sidechainButton"))->property("checked").toBool());

        const QString other = audioTrackWith(tone(60.0, kSampleRate), QStringLiteral("kick"), 1.0);
        QVERIFY(!other.isEmpty());
        QVERIFY(editor()->trySetDeviceSidechain(s.track, s.device, other));
        QVERIFY(s.graph->sidechained());
        QVERIFY(find(s.view, QStringLiteral("scGain"))->isEnabled());
        QVERIFY(find(s.view, QStringLiteral("scMix"))->isEnabled());
        QVERIFY(find(s.view, QStringLiteral("sidechainButton"))->property("checked").toBool());

        QSignalSpy asked(s.view, SIGNAL(sidechainMenuRequested()));
        click(find(s.view, QStringLiteral("sidechainButton")));
        QCOMPARE(asked.count(), 1);
    }

    void ratioTexts() {
        QCOMPARE(formatValue(4.0, QStringLiteral("ratio")), QStringLiteral("4.00:1"));
        QCOMPARE(formatValue(66.7, QStringLiteral("ratio")), QStringLiteral("66.7:1"));
        QCOMPARE(formatValue(100.0, QStringLiteral("ratio")), QStringLiteral("100:1"));
        QCOMPARE(formatValue(1.0, QStringLiteral("ratio")), QStringLiteral("1.00:1"));
        QCOMPARE(formatValue(0.5, QStringLiteral("ratio")), QStringLiteral("1:2.00"));
        QCOMPARE(formatValue(0.25, QStringLiteral("ratio")), QStringLiteral("1:4.00"));

        QCOMPARE(multibandParseRatio(QStringLiteral("4")), 4.0);
        QCOMPARE(multibandParseRatio(QStringLiteral("4:1")), 4.0);
        QCOMPARE(multibandParseRatio(QStringLiteral("4.00:1")), 4.0);
        QCOMPARE(multibandParseRatio(QStringLiteral("1:2")), 0.5);
        QCOMPARE(multibandParseRatio(QStringLiteral(" 1 : 2.00 ")), 0.5);
        QCOMPARE(multibandParseRatio(QStringLiteral("0.5")), 0.5);
        QCOMPARE(multibandParseRatio(QStringLiteral("1000")), 100.0);  // held
        QCOMPARE(multibandParseRatio(QStringLiteral("1:8")), 0.25);
        for (const char* text : {"x", "", "1:0", "0", "-2", "1:2:3", "a:1"})
            QVERIFY2(multibandParseRatio(QString::fromLatin1(text)) == 0.0, text);
        for (const double r : {0.25, 0.3, 0.333, 0.5, 0.77, 1.0, 1.5, 3.33, 4.0, 9.99, 10.0, 66.7, 99.5, 100.0}) {
            const double back = multibandParseRatio(formatValue(r, QStringLiteral("ratio")));
            QVERIFY2(std::abs(back / r - 1.0) < 0.005, qPrintable(QString::number(r)));
        }
        // The static curve is the engine's law: past a threshold, the distance from it divided by the ratio.
        QVERIFY(std::abs(multibandGainDb(-10.0, -20.0, 4.0, -40.0, 1.0, false, 100.0) + 7.5) < 1e-5);
        QVERIFY(std::abs(multibandGainDb(-60.0, -20.0, 1.0, -40.0, 4.0, false, 100.0) - 15.0) < 1e-5);
        QVERIFY(std::abs(multibandGainDb(-60.0, -20.0, 1.0, -40.0, 4.0, false, 50.0) - 7.5) < 1e-5);
        QVERIFY(std::abs(multibandGainDb(-20.0, -20.0, 4.0, -40.0, 1.0, true, 100.0) + 0.5625) < 1e-5);
    }

    void screenshot() {
        std::vector<float> mix = tone(40.0, kSampleRate, 0.3);
        for (const double f : {1000.0, 10000.0}) {
            const std::vector<float> part = tone(f, kSampleRate, f == 1000.0 ? 0.3 : 0.15);
            for (std::size_t i = 0; i < mix.size(); ++i)
                mix[i] += part[i];
        }
        const Shown s = showDevice({{QStringLiteral("high_above"), -30.0},
                                    {QStringLiteral("high_above_ratio"), 8.0},
                                    {QStringLiteral("mid_below"), -50.0},
                                    {QStringLiteral("mid_below_ratio"), 3.0},
                                    {QStringLiteral("mid_above"), -14.0},
                                    {QStringLiteral("mid_above_ratio"), 2.0},
                                    {QStringLiteral("low_above"), -24.0},
                                    {QStringLiteral("low_above_ratio"), 0.5}},
                                   mix);
        QVERIFY(s.view && s.graph);
        // Rendered, then ticks with no new audio (within the hold, so it reads as playing on), then
        // rendered again so the meters hold what is drawn while the window paints.
        engine()->renderOffline(0.0, kSampleRate / 2);
        ticks(12);
        engine()->renderOffline(0.0, kSampleRate / 2);
        refreshDisplays();
        QVERIFY(s.graph->gainTarget(kHigh) < -6.0 && s.graph->gainTarget(kLow) > 6.0);  // cut and lifted
        QVERIFY(std::abs(s.graph->gainShown(kHigh) - s.graph->gainTarget(kHigh)) < 0.1);
        QTest::qWait(30);
        save(grab(), QStringLiteral("multiband.png"));

        // High off, the Below fields, the Mid Above handle dragged (its value over it).
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("high_on"), 0.0);
        click(find(s.view, QStringLiteral("pageBelow")));
        ticks(30);
        QTest::qWait(200);  // (the fields' fades)
        const QPoint at = scenePoint(s.graph, s.graph->aboveHandle(kMid));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at + QPoint(-12, 0));
        dragTo(at + QPoint(-24, 0));
        engine()->renderOffline(0.0, kSampleRate / 2);
        ticks(10);
        QTest::qWait(30);
        save(grab(), QStringLiteral("multiband-drag.png"));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(-24, 0));

        // The Time fields, keyed by a sidechain, Soft Knee, the static curve read under the mouse.
        const QString kick = audioTrackWith(tone(60.0, kSampleRate, 0.8), QStringLiteral("kick"), 1.0);
        QVERIFY(editor()->trySetDeviceSidechain(s.track, s.device, kick));
        editor()->setDeviceParams(s.track, s.device,
                                  {{QStringLiteral("high_on"), 1.0},
                                   {QStringLiteral("soft_knee"), 1.0},
                                   {QStringLiteral("low_below"), -60.0},
                                   {QStringLiteral("low_below_ratio"), 0.4}});
        click(find(s.view, QStringLiteral("pageTime")));
        QTest::qWait(200);
        QTest::mouseMove(window_,
                         scenePoint(s.graph, QPointF(s.graph->xOfDb(-12.0), s.graph->lane(kMid).center().y())));
        ticks(300);  // (the meters let go of what played before)
        engine()->renderOffline(0.0, kSampleRate / 2);
        ticks(10);
        QTest::qWait(30);
        save(grab(), QStringLiteral("multiband-time.png"));
    }
};

QTEST_MAIN(TestUiDeviceEditorsMultiband)
#include "test_ui_device_editors_multiband.moc"
