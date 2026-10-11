// The Gate's editor (ui/qml/devices/editors/GateEditor.qml, GateGraph,
// GateKeyGraph) loaded as the device view loads it, over a real engine: it fits
// the view's height, and is as wide as its texts need in any font (every
// caption, readout and list whole); every control is bound to its parameter
// (undoably, its value as it is now); the threshold and return lines (either
// taken when they are one) and the key filter's dot drag in one undo step each,
// the lines give their parameters' menus; what the engine renders reaches the
// display, which scrolls and stops, goes idle when nothing comes, and repaints
// only for what shows; the key dot is blue for the key's level now, falls
// quickly, and the blue where the gate passes shows over the levels; listening,
// the label pulses only while sound comes; the sidechain section folds and
// unfolds, follows the sidechain and its EQ (what is set for later dimmed but
// settable), its cells as wide as their texts; the key filter's curve is the
// engine's, its dot follows the mouse, Ctrl and the wheel set the bell's Q.
// With SUBSTATION_UI_SCREENSHOTS set, device-editors-gate*.png are saved there.

#include <QFontMetricsF>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <vector>

#include "EditorHarness.h"
#include "audio/GateResponse.h"
#include "controls/KnobItem.h"
#include "devices/EqGraph.h"
#include "devices/EqTypeIcon.h"
#include "devices/GateGraph.h"
#include "devices/GateKeyGraph.h"
#include "model/Device.h"
#include "theme/Theme.h"

using namespace sub::app;
using namespace sub::ui;
using sub::app::test::kSampleRate;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Bursts of a 220 Hz tone, 100 ms at 0.5, then 150 ms of near silence (0.003), for `seconds`.
std::vector<float> bursts(double seconds) {
    std::vector<float> samples(static_cast<size_t>(seconds * kSampleRate));
    for (size_t i = 0; i < samples.size(); ++i) {
        const double t = double(i) / kSampleRate;
        const double level = std::fmod(t, 0.25) < 0.1 ? 0.5 : 0.003;
        samples[i] = float(level * std::sin(2 * kPi * 220.0 * t));
    }
    return samples;
}

}  // namespace

class TestUiDeviceEditorsGate : public QObject, public sub::app::test::EditorHarness {
    Q_OBJECT

    struct Shown {
        QString track;
        QString device;
        QQuickItem* view = nullptr;
        GateGraph* graph = nullptr;
    };

    // A Gate on a track playing `mono` for `seconds`, its editor shown.
    Shown gate(const std::vector<float>& mono, double seconds) {
        Shown shown;
        shown.track = audioTrackWith(mono, QStringLiteral("gated"), seconds);
        if (shown.track.isEmpty())
            return shown;
        shown.device = editor()->addDevice(shown.track, QStringLiteral("gate"));
        shown.view = show(QStringLiteral("gate"), shown.track, shown.device);
        shown.graph = shown.view ? find<GateGraph>(shown.view, QStringLiteral("gateGraph")) : nullptr;
        return shown;
    }

    // An EditorKnob's dial.
    KnobItem* knob(QQuickItem* view, const char* name) {
        QQuickItem* cell = find(view, QString::fromLatin1(name));
        auto* paramKnob = cell ? qvariant_cast<QQuickItem*>(cell->property("knob")) : nullptr;
        return paramKnob ? qvariant_cast<KnobItem*>(paramKnob->property("knob")) : nullptr;
    }

    // An EditorKnob's readout (its last child: caption, knob, readout).
    QString readout(QQuickItem* view, const char* name) {
        QQuickItem* cell = find(view, QString::fromLatin1(name));
        return cell && !cell->childItems().isEmpty() ? cell->childItems().last()->property("text").toString()
                                                     : QString();
    }

    // A ParamButton's (or ParamChoice's) button.
    QQuickItem* button(QQuickItem* view, const char* name) {
        QQuickItem* control = find(view, QString::fromLatin1(name));
        return control ? qvariant_cast<QQuickItem*>(control->property("button")) : nullptr;
    }

    void click(QQuickItem* item) { QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(item)); }

    QString textOf(QQuickItem* view, const char* name) {
        QQuickItem* item = find(view, QString::fromLatin1(name));
        return item ? item->property("text").toString() : QString();
    }

    // The displays' clock ticked `count` times, `ms` apart.
    void tick(int count, int ms = 16) {
        for (int i = 0; i < count; ++i) {
            QTest::qWait(ms);
            refreshDisplays();
        }
    }

    // The mouse off the editor (where init() leaves it), then `count` ticks: the wait after a gesture on the
    // graph. Resting on the graph, the mouse would open its tooltip once 0.7 s pass (tick(30) takes half a
    // second, and up to twice that where the timers are coarse, as Windows'), and in the host window, too
    // short for the tooltip to sit clear of the graph, it opens over the plot and takes the next press.
    void tickAway(int count) {
        QTest::mouseMove(window_, QPoint(1, 1));
        tick(count);
    }

    // Ticks until the graph has asked for no repaint three ticks running (at most `most` ticks): a meter
    // falling repaints only once it has moved a twentieth of a pixel, so a tick that comes close after
    // another may ask for none while it still falls.
    void settle(GateGraph* graph, int most = 400) {
        for (int i = 0, still = 0; i < most && still < 3; ++i) {
            tick(1);
            still = graph->animating() ? 0 : still + 1;
        }
    }

    // `item`'s rect in `in`'s coordinates.
    static QRectF rectIn(QQuickItem* item, QQuickItem* in) {
        return item->mapRectToItem(in, QRectF(0, 0, item->width(), item->height()));
    }

    // The EditorKnobs named, in `view`.
    QList<QQuickItem*> cells(QQuickItem* view, std::initializer_list<const char*> names) {
        QList<QQuickItem*> found;
        for (const char* name : names)
            found << find(view, QString::fromLatin1(name));
        return found;
    }

    double latencyOf(const Shown& shown) {
        const auto id = bridge()->engineDeviceId(shown.track, shown.device);
        return id ? engine()->processorInfo(*id).latency : -1;
    }

private Q_SLOTS:
    void initTestCase() {
        if (!haveDisplay())
            QSKIP("needs a display: the offscreen platform renders Qt Quick in software, without this geometry");
        startHost();
    }

    void cleanupTestCase() { stopHost(); }

    void init() { clearHost(); }

    void registry() {
        QVariant url;
        QMetaObject::invokeMethod(root_.get(), "editorFor", Q_RETURN_ARG(QVariant, url),
                                  Q_ARG(QVariant, QStringLiteral("gate")));
        QVERIFY(url.toString().endsWith(QStringLiteral("GateEditor.qml")));
    }

    void fitsAndBinds() {
        const Shown shown = gate(tone(220.0, kSampleRate), 1.0);
        QVERIFY(shown.view && shown.graph);
        QQuickItem* view = shown.view;
        QVERIFY2(view->implicitHeight() <= bodyHeight(),
                 qPrintable(QStringLiteral("%1 > %2").arg(view->implicitHeight()).arg(bodyHeight())));
        // As wide as it lays itself out (each part as its texts need: checked below).
        const double folded = view->property("foldedWidth").toDouble();
        QCOMPARE(view->implicitWidth(), folded);

        // Every control, each at its parameter's default.
        const char* names[] = {"threshold", "return", "floor", "attack", "hold", "release", "flip", "lookahead",
                               "gateGraph", "sidechainFold"};
        for (const char* name : names)
            QVERIFY2(find(view, QString::fromLatin1(name)), name);
        const std::pair<const char*, double> defaults[] = {{"threshold", -12.0}, {"return", 3.0}, {"floor", -40.0},
                                                           {"attack", 3.5},      {"hold", 10.0},  {"release", 15.0}};
        for (const auto& [name, value] : defaults) {
            KnobItem* dial = knob(view, name);
            QVERIFY2(dial, name);
            QVERIFY2(std::abs(dial->value() - value) < 1e-4, name);
        }
        for (const char* name : {"attack", "hold", "release"})
            QVERIFY2(knob(view, name)->logScale(), name);
        QCOMPARE(button(view, "lookahead")->property("text").toString(), QStringLiteral("1 ms"));
        // The Lookahead list as wide as its longest choice and the arrow need (its face's implicit width).
        QQuickItem* lookahead = find(view, QStringLiteral("lookahead"));
        for (const double choice : {0.0, 2.0, 1.0}) {  // (back to the default last)
            editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("lookahead"), choice);
            QVERIFY2(button(view, "lookahead")->implicitWidth() <= lookahead->width(),
                     qPrintable(button(view, "lookahead")->property("text").toString()));
        }
        // Only a knob whose range is symmetric about 0 is drawn from the middle (the key EQ's Gain).
        for (const char* name : {"threshold", "return", "floor", "attack", "hold", "release"})
            QVERIFY2(!knob(view, name)->bipolar(), name);

        // Floor at its bottom reads as silence (its readout and its knob's tooltip).
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("floor"), -75.0);
        QCOMPARE(readout(view, "floor"), QStringLiteral("−inf dB"));
        QCOMPARE(knob(view, "floor")->text(), QStringLiteral("−inf dB"));
        undo()->undo();
        QCOMPARE(readout(view, "floor"), QStringLiteral("-40.0 dB"));
        // Attack under a millisecond with two decimals.
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("attack"), 0.02);
        QCOMPARE(readout(view, "attack"), QStringLiteral("0.02 ms"));
        undo()->undo();
        QCOMPARE(readout(view, "attack"), QStringLiteral("3.5 ms"));

        // Inside the body, its margins kept, nothing over the display.
        const QRectF body(0, 0, view->width(), view->height());
        const QRectF graph = rectIn(shown.graph, view);
        QCOMPARE(graph.top(), 6.0);
        QCOMPARE(graph.bottom(), view->height() - 6);
        for (const char* name : {"threshold", "return", "floor", "attack", "hold", "release", "flip", "lookahead",
                                 "lookaheadCaption", "sidechainFold"}) {
            const QRectF rect = rectIn(find(view, QString::fromLatin1(name)), view);
            QVERIFY2(body.adjusted(8, 6, -8, -6).contains(rect), name);
            QVERIFY2(!rect.intersects(graph), name);
        }
        // The display at its own width, 28 px in (after the sidechain's strip); the knobs in a grid of equal
        // cells 4 px apart, 10 px after it; the column beside them 8 px after it, Flip, the Lookahead list and its
        // caption spanning it, and the editor ending 8 px after it.
        QCOMPARE(graph.left(), 28.0);
        QCOMPARE(graph.width(), double(GateGraph::kWidth));
        const double cell = rectIn(find(view, QStringLiteral("threshold")), view).width();
        const char* grid[2][3] = {{"threshold", "return", "floor"}, {"attack", "hold", "release"}};
        for (const auto& row : grid) {
            double left = graph.right() + 10;
            for (const char* name : row) {
                const QRectF rect = rectIn(find(view, QString::fromLatin1(name)), view);
                QCOMPARE(rect.left(), left);
                QCOMPARE(rect.width(), cell);
                left = rect.right() + 4;
            }
        }
        const QRectF side = rectIn(find(view, QStringLiteral("gateSide")), view);
        QCOMPARE(side.left(), rectIn(find(view, QStringLiteral("floor")), view).right() + 8);
        for (const char* name : {"flip", "lookahead", "lookaheadCaption"}) {
            const QRectF rect = rectIn(find(view, QString::fromLatin1(name)), view);
            QCOMPARE(rect.left(), side.left());
            QCOMPARE(rect.right(), side.right());
        }
        QCOMPARE(side.right() + 8, folded);
        // Every text whole, as a Text lays it out in its font: the knobs' captions and readouts (whatever their
        // values), the column's caption, Flip's name (with the room round it a button's face has), the list's
        // choices beside its arrow.
        const QList<QQuickItem*> knobs = cells(view, {"threshold", "return", "floor", "attack", "hold", "release"});
        for (QQuickItem* knobCell : knobs) {
            const QString problem = knobTextsProblem(knobCell);
            QVERIFY2(problem.isEmpty(), qPrintable(problem));
        }
        QQuickItem* lookaheadCaption = find(view, QStringLiteral("lookaheadCaption"));
        const double captionNeeds = textWidth(QStringLiteral("Lookahead"), lookaheadCaption->property("font"));
        QVERIFY2(captionNeeds <= lookaheadCaption->width() && !lookaheadCaption->property("truncated").toBool(),
                 qPrintable(QString::number(captionNeeds)));
        QQuickItem* flip = button(view, "flip");
        QVERIFY(flip->implicitWidth() <= flip->width());
        QQuickItem* choices = button(view, "lookahead");
        const double listNeeds = widestOf(lookahead->property("names").toStringList(), choices->property("font"))
                                 + choices->property("leftPadding").toDouble()
                                 + choices->property("rightPadding").toDouble();
        QVERIFY(listNeeds <= choices->width());
        // And no wider than that: the cells as their widest text (56 px at least), the column as its caption,
        // Flip or its list (56 px at least).
        QCOMPARE(cell, std::max(56.0, std::ceil(knobsNeed(knobs))));
        QCOMPARE(side.width(), std::ceil(std::max({56.0, captionNeeds, flip->implicitWidth(), listNeeds})));
        QTest::qWait(50);
        const QImage shot = grab();
        save(shot, QStringLiteral("gate-idle.png"));
        // The In meter is the house's level meter in a well of the panel's grey (on the display's black).
        const QRectF well = shown.graph->inMeter();
        const QPoint inWell = scenePoint(shown.graph, QPointF(well.center().x(), well.top() + well.height() / 12));
        QCOMPARE(shot.pixelColor(inWell * shot.devicePixelRatio()), QColor(0x25, 0x25, 0x25));

        // The meters' captions inside the display and right of the plot, each centred over its meter (the Gate's
        // unless that would take it within 3 px of the right edge), and "In" ending two of their font's word spaces
        // and a pixel before "Gate" begins (any nearer, the two read as one phrase, "In Gate"). As drawn too: the
        // caption row blank for two word spaces between the last of In's ink and the first of Gate's.
        GateGraph* display = shown.graph;
        const QRectF inCaption = display->inCaption(), gateCaption = display->gateCaption();
        const double space = QFontMetricsF(GateGraph::textFont()).horizontalAdvance(QLatin1Char(' '));
        QVERIFY(inCaption.left() > display->plot().right() + 6 && inCaption.top() >= 1.0 && gateCaption.top() >= 1.0);
        QVERIFY(gateCaption.right() <= display->width() - 3 + 1e-9);
        QCOMPARE(inCaption.center().x(), display->inMeter().center().x());
        if (gateCaption.right() < display->width() - 3 - 1e-9)
            QCOMPARE(gateCaption.center().x(), display->gateMeter().center().x());
        else  // held off the edge: left of its meter's centre, or on it when the clamp lands there exactly
            QVERIFY(gateCaption.center().x() <= display->gateMeter().center().x() + 1e-9);
        const double apart = gateCaption.left() - inCaption.right();
        QVERIFY2(apart >= 2 * space + 1 - 1e-9,
                 qPrintable(QStringLiteral("%1 px apart, a space %2").arg(apart).arg(space)));
        const double dpr = shot.devicePixelRatio();
        const QRectF row(display->mapToScene(inCaption.topLeft()) * dpr,
                         display->mapToScene(gateCaption.bottomRight()) * dpr);
        const double between = display->mapToScene(QPointF((inCaption.right() + gateCaption.left()) / 2, 0)).x() * dpr;
        int lastIn = -1, firstGate = -1;
        for (int x = int(std::floor(row.left() - dpr)); x <= int(std::ceil(row.right() + dpr)); ++x) {
            bool ink = false;  // (well off the display's black)
            for (int y = int(std::ceil(row.top())); y < int(std::floor(row.bottom())) && !ink; ++y) {
                const QColor c = shot.pixelColor(x, y);
                ink = std::max({c.red(), c.green(), c.blue()}) > Theme::kMeterBg.red() + 40;
            }
            if (ink && x < between)
                lastIn = x;
            else if (ink && firstGate < 0)
                firstGate = x;
        }
        QVERIFY(lastIn >= 0 && firstGate >= 0);
        const double blank = (firstGate - lastIn - 1) / dpr;
        QVERIFY2(blank >= 2 * space, qPrintable(QStringLiteral("%1 px blank, a space %2").arg(blank).arg(space)));
    }

    void controlsUndoable() {
        const Shown shown = gate(tone(220.0, kSampleRate), 1.0);
        QVERIFY(shown.view && shown.graph);
        GateGraph* graph = shown.graph;
        auto value = [&](const char* id) { return param(shown.track, shown.device, QString::fromLatin1(id)); };

        // A change from elsewhere: the knob at once, the line easing to it.
        refreshDisplays();
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("threshold"), -30.0);
        QCOMPARE(knob(shown.view, "threshold")->value(), -30.0);
        refreshDisplays();  // (a moment later)
        QVERIFY(std::abs(graph->thresholdY() - graph->yOf(-30.0)) > 0.5);  // on its way, not there at once
        for (int i = 0; i < 40 && std::abs(graph->thresholdY() - graph->yOf(-30.0)) > 0.5; ++i)
            tick(1);
        QVERIFY2(std::abs(graph->thresholdY() - graph->yOf(-30.0)) <= 0.5,
                 qPrintable(QString::number(graph->thresholdY())));
        undo()->undo();
        QCOMPARE(knob(shown.view, "threshold")->value(), -12.0);

        // Flip: a switch, lit while on.
        QQuickItem* flip = find(shown.view, QStringLiteral("flip"));
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(button(shown.view, "flip")));
        QCOMPARE(value("flip"), 1.0);
        QVERIFY(flip->property("lit").toBool());
        undo()->undo();
        QCOMPARE(value("flip"), 0.0);
        QVERIFY(!flip->property("lit").toBool());

        // Lookahead: one undo step, and the engine's latency follows.
        const int steps = undo()->index();
        QMetaObject::invokeMethod(find(shown.view, QStringLiteral("lookahead")), "choose", Q_ARG(QVariant, 2));
        QCOMPARE(value("lookahead"), 2.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(button(shown.view, "lookahead")->property("text").toString(), QStringLiteral("10 ms"));
        QTRY_COMPARE(latencyOf(shown), 480.0);
        undo()->undo();
        QTRY_COMPARE(latencyOf(shown), 48.0);
    }

    void thresholdDragIsOneStep() {
        const Shown shown = gate(tone(220.0, kSampleRate), 1.0);
        QVERIFY(shown.view && shown.graph);
        GateGraph* graph = shown.graph;
        auto value = [&] { return param(shown.track, shown.device, QStringLiteral("threshold")); };
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("threshold"), -40.0);
        tick(30);  // (the line where it is)
        const QRectF plot = graph->plot();
        const double perPixel = 78.0 / plot.height();

        // On the line, three moves up: one step, the line following.
        int steps = undo()->index();
        QPoint at = scenePoint(graph, QPointF(plot.center().x(), graph->thresholdY()));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        QVERIFY(graph->dragging());
        for (int dy = 10; dy <= 30; dy += 10)
            dragTo(at - QPoint(0, dy));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 30));
        QVERIFY(!graph->dragging());
        QVERIFY2(std::abs(value() - (-40.0 + 30 * perPixel)) < 0.2, qPrintable(QString::number(value())));
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(undo()->text(undo()->index() - 1), QStringLiteral("Change Gate Threshold"));
        QVERIFY(std::abs(graph->thresholdY() - graph->yOf(value())) < 1e-6);  // at once, not eased
        undo()->undo();
        QCOMPARE(value(), -40.0);
        tickAway(30);

        // Anywhere else in the plot: the threshold too, relative (no jump at the press).
        at = scenePoint(graph, QPointF(plot.center().x(), plot.top() + 12));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(value(), -40.0);
        dragTo(at + QPoint(0, 20));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(0, 20));
        QVERIFY2(std::abs(value() - (-40.0 - 20 * perPixel)) < 0.2, qPrintable(QString::number(value())));
        undo()->undo();
        tickAway(30);

        // A press 5 px off the line and a release: nothing changes, nothing to undo.
        steps = undo()->index();
        at = scenePoint(graph, QPointF(plot.center().x(), graph->thresholdY() - 5));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(value(), -40.0);
        QCOMPARE(undo()->index(), steps);

        // Shift: a tenth as far.
        at = scenePoint(graph, QPointF(plot.center().x(), graph->thresholdY()));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at - QPoint(0, 30), Qt::ShiftModifier);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::ShiftModifier, at - QPoint(0, 30));
        QVERIFY2(std::abs(value() - (-40.0 + 3 * perPixel)) < 0.05, qPrintable(QString::number(value())));
        QCOMPARE(undo()->index(), steps + 1);
    }

    void returnDrag() {
        const Shown shown = gate(tone(220.0, kSampleRate), 1.0);
        QVERIFY(shown.view && shown.graph);
        GateGraph* graph = shown.graph;
        auto value = [&](const char* id) { return param(shown.track, shown.device, QString::fromLatin1(id)); };
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("threshold"), -30.0);
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("return"), 6.0);
        tick(30);
        const QRectF plot = graph->plot();
        const double perPixel = 78.0 / plot.height();
        QVERIFY(std::abs(graph->returnY() - graph->yOf(-36.0)) < 0.5);

        // The return line dragged down: it closes lower (more Return), one step.
        const int steps = undo()->index();
        QPoint at = scenePoint(graph, QPointF(plot.center().x(), graph->returnY()));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at + QPoint(0, 8));
        dragTo(at + QPoint(0, 15));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(0, 15));
        QVERIFY2(std::abs(value("return") - (6.0 + 15 * perPixel)) < 0.2, qPrintable(QString::number(value("return"))));
        QCOMPARE(value("threshold"), -30.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(undo()->text(undo()->index() - 1), QStringLiteral("Change Gate Return"));

        // Far up: Return no less than 0.
        at = scenePoint(graph, QPointF(plot.center().x(), graph->returnY()));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at - QPoint(0, 200));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 200));
        QCOMPARE(value("return"), 0.0);
        QCOMPARE(undo()->index(), steps + 2);

        // Return 0: the lines are one. From on or below it, the return line; from above, the threshold.
        tick(30);
        QCOMPARE(graph->returnY(), graph->thresholdY());
        at = scenePoint(graph, QPointF(plot.center().x(), graph->returnY() + 2));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at + QPoint(0, 10));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(0, 10));
        QVERIFY2(std::abs(value("return") - 10 * perPixel) < 0.2, qPrintable(QString::number(value("return"))));
        QCOMPARE(value("threshold"), -30.0);
        undo()->undo();
        tickAway(30);
        at = scenePoint(graph, QPointF(plot.center().x(), graph->thresholdY() - 2));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at + QPoint(0, 10));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(0, 10));
        QVERIFY2(std::abs(value("threshold") - (-30.0 - 10 * perPixel)) < 0.2,
                 qPrintable(QString::number(value("threshold"))));
        QCOMPARE(value("return"), 0.0);
        undo()->undo();
        tickAway(30);
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier,
                           scenePoint(graph, QPointF(plot.center().x() + 20, graph->returnY() + 1)));
        QCOMPARE(value("return"), 3.0);
        QCOMPARE(value("threshold"), -30.0);
        undo()->undo();
        QCOMPARE(value("return"), 0.0);
        QCOMPARE(undo()->index(), steps + 2);

        undo()->undo();
        tickAway(30);

        // A double-click on the threshold line puts it back (one step); on the return line, Return.
        at = scenePoint(graph, QPointF(plot.center().x() + 30, graph->thresholdY()));
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(value("threshold"), -12.0);
        QCOMPARE(undo()->index(), steps + 2);
        tickAway(30);
        at = scenePoint(graph, QPointF(plot.center().x() - 30, graph->returnY()));
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(value("return"), 3.0);
        QCOMPARE(undo()->index(), steps + 3);

        // Undone, the return line eases back down from where it is drawn (Return 3's place, the threshold's
        // line 3 dB above it): double-clicked there before it moves, Return is still what is put back, one
        // step (the first press puts the lines where their values are; the double-click resets the line
        // that press was on, not what is under the mouse once the lines have moved).
        tickAway(30);
        undo()->undo();
        QVERIFY(value("return") > 12.0);
        QCOMPARE(graph->returnY(), graph->yOf(-15.0));
        at = scenePoint(graph, QPointF(plot.center().x() - 30, graph->returnY()));
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(value("return"), 3.0);
        QCOMPARE(value("threshold"), -12.0);
        QCOMPARE(undo()->index(), steps + 3);
    }

    void lineMenusAndTheStrip() {
        const Shown shown = gate(tone(220.0, kSampleRate), 1.0);
        QVERIFY(shown.view && shown.graph);
        GateGraph* graph = shown.graph;
        auto value = [&](const char* id) { return param(shown.track, shown.device, QString::fromLatin1(id)); };
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("threshold"), -30.0);
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("return"), 6.0);
        tick(30);
        const QRectF plot = graph->plot();

        // Right-click a line: its parameter's menu; elsewhere, nothing of the graph's (the frame's menu).
        auto* menu = qvariant_cast<QObject*>(shown.view->property("lineMenu"));
        QVERIFY(menu);
        QSignalSpy asked(graph, &GateGraph::paramMenuRequested);
        for (const auto& [y, id] : {std::pair{graph->thresholdY(), QStringLiteral("threshold")},
                                    std::pair{graph->returnY(), QStringLiteral("return")}}) {
            QTest::mouseClick(window_, Qt::RightButton, Qt::NoModifier,
                              scenePoint(graph, QPointF(plot.center().x(), y)));
            QCOMPARE(asked.count(), id == QStringLiteral("threshold") ? 1 : 2);
            QCOMPARE(asked.last().at(0).toString(), id);
            QTRY_VERIFY(menu->property("opened").toBool());
            auto* param = qvariant_cast<QObject*>(menu->property("param"));
            QVERIFY(param);
            QCOMPARE(param->property("paramId").toString(), id);
            QMetaObject::invokeMethod(menu, "close");
            QTRY_VERIFY(!menu->property("opened").toBool());
        }
        QTest::mouseClick(window_, Qt::RightButton, Qt::NoModifier,
                          scenePoint(graph, QPointF(plot.center().x(), plot.top() + 12)));
        QCOMPARE(asked.count(), 2);

        // A press on the meters (the strip right of the plot) is not a drag of the threshold.
        const int steps = undo()->index();
        const QPoint meter = scenePoint(graph, graph->inMeter().center());
        QVERIFY(!plot.contains(graph->inMeter().center()));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, meter);
        QVERIFY(!graph->dragging());
        dragTo(meter + QPoint(0, 20));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, meter + QPoint(0, 20));
        QCOMPARE(value("threshold"), -30.0);
        QCOMPARE(undo()->index(), steps);
        // The meters' wells: inside the graph, beside each other, apart from the plot.
        QVERIFY(graph->inMeter().left() > plot.right() + 20);
        QVERIFY(graph->gateMeter().left() >= graph->inMeter().right() + 6);
        QVERIFY(graph->gateMeter().right() <= graph->width() - 4);
    }

    void displaysReachTheGraph() {
        const Shown shown = gate(std::vector<float>(kSampleRate, 0.5f), 1.0);
        QVERIFY(shown.view && shown.graph);
        GateGraph* graph = shown.graph;
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("threshold"), -40.0);

        // Open: the input passes as it is.
        engine()->renderOffline(0.0, kSampleRate / 2);
        tick(3, 20);
        QVERIFY2(std::abs(graph->levelIn() - 20 * std::log10(0.5)) < 0.1,
                 qPrintable(QString::number(graph->levelIn())));
        QVERIFY2(std::abs(graph->levelOut() - 20 * std::log10(0.5)) < 0.1,
                 qPrintable(QString::number(graph->levelOut())));
        QVERIFY(graph->passing() > 0.99);
        QVERIFY(graph->property("open").toBool());
        QVERIFY2(graph->historySize() > 50, qPrintable(QString::number(graph->historySize())));
        const qint64 newest = graph->newest();
        QCOMPARE(graph->historyAt(GateGraph::Input, newest - 1), float(20 * std::log10(0.5)));
        QCOMPARE(graph->historyAt(GateGraph::Open, newest - 1), 1.0f);
        QVERIFY(graph->scroll() <= double(graph->newest()));
        // The rings hold all the plot can draw at the engine's rate, as they would at any other (ASIO's
        // 384 kHz too, where 2048 values were 1.4 s): its 2.5 s, up to 0.1 s behind the newest value, and a
        // column drawn in part at either end.
        QCOMPARE(graph->capacity(), GateGraph::ringCapacity(kSampleRate / double(gateDisplaySamples())));
        for (const double rate : {8000.0, 44100.0, 48000.0, 96000.0, 192000.0, 384000.0}) {
            const double perSecond = rate / gateDisplaySamples();
            const double column = std::ceil(GateGraph::kHistorySeconds * perSecond / graph->plot().width());
            const double drawn = (GateGraph::kHistorySeconds + 0.1) * perSecond + 2 * column + 2;
            const qint64 capacity = GateGraph::ringCapacity(perSecond);
            QVERIFY2(double(capacity) >= drawn, qPrintable(QStringLiteral("%1 Hz: %2").arg(rate).arg(capacity)));
            QCOMPARE(capacity & (capacity - 1), qint64(0));  // (an index's low bits are its place)
        }

        // More audio: the drawing scrolls on, never past the newest value.
        const double before = graph->scroll();
        engine()->renderOffline(0.0, kSampleRate / 4);
        tick(1);
        QVERIFY(graph->scroll() > before);
        QVERIFY(graph->scroll() <= double(graph->newest()));
        QVERIFY(graph->animating());
        // No more audio: it stops where the values stop, then everything settles (the meters fall).
        tick(20, 16);
        QCOMPARE(graph->scroll(), double(graph->newest()));
        settle(graph);
        QVERIFY(!graph->animating());
        const double settled = graph->scroll();
        tick(3);
        QCOMPARE(graph->scroll(), settled);
        QVERIFY(!graph->animating());
        // Nothing has come for a while (as with the device off): idle, nothing passing, the LED out.
        QVERIFY(graph->idle());
        QCOMPARE(graph->passing(), 0.0);
        QVERIFY(!graph->property("open").toBool());

        // Closed: the floor's -40 dB under the input.
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("threshold"), 0.0);
        engine()->renderOffline(0.0, kSampleRate / 2);
        tick(3, 20);
        QVERIFY2(graph->passing() < 0.01, qPrintable(QString::number(graph->passing())));
        QVERIFY2(std::abs(graph->levelOut() - (20 * std::log10(0.5) - 40.0)) < 0.2,
                 qPrintable(QString::number(graph->levelOut())));
        QVERIFY(!graph->property("open").toBool());
        QVERIFY(!graph->idle());
    }

    void silenceDrawsNothing() {
        // Silence flowing in (as from a running audio device): the history scrolls on, but nothing it
        // draws changes, so the graph doesn't repaint.
        const Shown shown = gate(std::vector<float>(size_t(kSampleRate) * 6, 0.f), 6.0);
        QVERIFY(shown.view && shown.graph);
        GateGraph* graph = shown.graph;
        const double beatsPerSecond = project()->tempo() / 60.0;
        engine()->renderOffline(0.0, kSampleRate * 3);
        double seconds = 3.0;  // (values keep coming, as from a running audio device: never idle)
        for (int i = 0; i < 100 && (i < 3 || graph->animating()); ++i) {
            engine()->renderOffline(seconds * beatsPerSecond, kSampleRate / 60);
            seconds += 1.0 / 60;
            tick(1);
        }
        QVERIFY(!graph->animating());
        QVERIFY(!graph->idle());
        for (int i = 0; i < 4; ++i) {
            const double before = graph->scroll();
            const qint64 newest = graph->newest();
            engine()->renderOffline((seconds + 0.1 * i) * beatsPerSecond, kSampleRate / 10);
            tick(1);
            QVERIFY(graph->newest() > newest);  // values came,
            QVERIFY(graph->scroll() > before);  // it scrolled on,
            QVERIFY(!graph->animating());       // and asked for no repaint
        }
        // Something that moves: it repaints again.
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("threshold"), -40.0);
        tick(1);
        QVERIFY(graph->animating());  // (the line eases to it)
    }

    void steadyToneDrawsNothing() {
        // A steady tone through an open gate (always above the threshold, so it never closes), values
        // coming every tick: once the history in sight is all of it, nothing visible moves, so the graph
        // doesn't repaint. (A 192 Hz triangle, 0.45..0.5, a peak in every value's 256 samples: the period
        // isn't a whole number of samples and the peak is sharp, so the peak each value catches, and each
        // tick's level, differ by a few thousandths of a dB, a few thousandths of a pixel. The clip is
        // 16-bit, which would flatten a sine's crest to one value.)
        std::vector<float> steady(size_t(kSampleRate) * 8);
        for (size_t i = 0; i < steady.size(); ++i) {
            const double phase = std::fmod(192.3 * double(i) / kSampleRate, 1.0);
            steady[i] = float(0.45 + 0.05 * (1.0 - std::abs(2.0 * phase - 1.0)));
        }
        const Shown shown = gate(steady, 8.0);
        QVERIFY(shown.view && shown.graph);
        GateGraph* graph = shown.graph;
        for (const auto& [id, value] : {std::pair{"threshold", -70.0}, std::pair{"lookahead", 0.0},
                                        std::pair{"attack", 0.02}, std::pair{"floor", 0.0}})
            editor()->setDeviceParam(shown.track, shown.device, QString::fromLatin1(id), value);
        const double beatsPerSecond = project()->tempo() / 60.0;
        engine()->renderOffline(0.0, kSampleRate * 3);
        double seconds = 3.0;
        // A tick's worth more, read at once (the display clock also ticks on its own while the test
        // waits: had it read them first, the tick checked would be one without values).
        auto more = [&] {
            QTest::qWait(16);
            engine()->renderOffline(seconds * beatsPerSecond, kSampleRate / 60);
            seconds += 1.0 / 60;
            refreshDisplays();
        };
        for (int i = 0; i < 150 && (i < 3 || graph->animating()); ++i)
            more();
        QVERIFY(!graph->animating());
        QVERIFY(graph->isOpen() && !graph->idle());
        for (int i = 0; i < 10; ++i) {
            const qint64 newest = graph->newest();
            more();
            QVERIFY(graph->newest() > newest);
            QVERIFY(!graph->animating());
        }
    }

    void keyDotAndShade() {
        // A second of a tone at -6 dB through a -30 dB threshold, then -50 dB.
        std::vector<float> burst(size_t(kSampleRate) * 2);
        for (size_t i = 0; i < burst.size(); ++i) {
            const double level = i < size_t(kSampleRate) ? 0.5 : 0.003;
            burst[i] = float(level * std::sin(2 * kPi * 220.0 * double(i) / kSampleRate));
        }
        const Shown shown = gate(burst, 2.0);
        QVERIFY(shown.view && shown.graph);
        GateGraph* graph = shown.graph;
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("threshold"), -30.0);
        tick(30);  // (the lines where they are, not easing across where the shade is looked at)
        engine()->renderOffline(0.0, kSampleRate);
        tick(1);
        QVERIFY2(std::abs(graph->levelKey() - 20 * std::log10(0.5)) < 0.1,
                 qPrintable(QString::number(graph->levelKey())));
        QVERIFY(graph->keyAbove() && graph->isOpen());
        QCOMPARE(graph->keyDotDb(), graph->levelKey());  // (it rises at once)

        // Where the gate is open, the blue shade shows over the levels too, not only above them: in the
        // output's band (the dark grey) a little to the left of the newest values, between the grid's
        // -12 and -24 dB lines (well above the threshold's and Return's).
        QTest::qWait(50);
        const QImage shot = grab();
        const QPoint inBand = scenePoint(graph, QPointF(graph->plot().right() - 20, graph->yOf(-18.0)));
        const QColor shaded = shot.pixelColor(inBand * shot.devicePixelRatio());
        QVERIFY2(shaded.blue() - shaded.red() > 12, qPrintable(shaded.name()));

        // Then the quiet: the dot is grey from the first tick that has only quiet values (one may still
        // hold the tone's last ones), not once the falling dot gets below the line; and the dot falls
        // below the line itself within a few ticks (it falls quickly: moving, not lagging).
        const double beatsPerSecond = project()->tempo() / 60.0;
        double seconds = 1.0;
        int grey = -1, below = -1;
        for (int i = 0; i < 30 && below < 0; ++i) {
            QTest::qWait(16);
            engine()->renderOffline(seconds * beatsPerSecond, kSampleRate / 60);
            seconds += 1.0 / 60;
            refreshDisplays();
            if (grey < 0 && !graph->keyAbove())
                grey = i;
            if (grey >= 0)
                QVERIFY2(!graph->keyAbove(), qPrintable(QString::number(i)));
            if (graph->keyDotDb() < -30.0)
                below = i;
        }
        QVERIFY2(grey >= 0 && grey <= 1, qPrintable(QString::number(grey)));
        QVERIFY2(below >= 0 && below <= 10, qPrintable(QString::number(below)));
        QVERIFY(graph->levelKey() < -45.0);
    }

    void listeningIdleIsStill() {
        // Listening, the label pulses while sound comes (the graph repaints every tick); once nothing
        // comes (the device off, the engine stopped) it holds still, dimmed, and nothing repaints.
        const Shown shown = gate(tone(220.0, kSampleRate), 1.0);
        QVERIFY(shown.view && shown.graph);
        GateGraph* graph = shown.graph;
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("sc_listen"), 1.0);
        engine()->renderOffline(0.0, kSampleRate / 2);
        tick(2);
        QVERIFY(!graph->idle());
        QVERIFY(graph->animating());
        settle(graph);
        QVERIFY(graph->idle());
        QVERIFY(!graph->animating());
        tick(5);
        QVERIFY(!graph->animating());
    }

    void sidechainSection() {
        const Shown shown = gate(tone(220.0, kSampleRate), 1.0);
        QVERIFY(shown.view && shown.graph);
        QQuickItem* view = shown.view;
        auto value = [&](const char* id) { return param(shown.track, shown.device, QString::fromLatin1(id)); };
        QQuickItem* section = find(view, QStringLiteral("sidechainSection"));
        QVERIFY(section);
        QCOMPARE(section->width(), 0.0);
        QVERIFY(!section->isVisible());

        // Unfolded: wider (the section, and the divider with the space round it), as tall as ever.
        const double sectionWidth = view->property("sectionWidth").toDouble();
        click(find(view, QStringLiteral("sidechainFold")));
        QTRY_COMPARE_WITH_TIMEOUT(section->width(), sectionWidth, 300);
        const double folded = view->property("foldedWidth").toDouble();
        QTRY_COMPARE(view->implicitWidth(), folded + sectionWidth + 13);
        QVERIFY(view->implicitHeight() <= bodyHeight());
        QVERIFY(fitted());
        const QRectF inside = rectIn(section, view);
        for (const char* name : {"sc_gain", "sc_mix", "sc_eq_freq", "sc_eq_q", "sc_eq_gain", "sc_listen", "sc_eq",
                                 "eqType0", "eqType1", "eqType2", "eqType3", "eqType4", "eqType5", "sidechainSource",
                                 "keyGraph"}) {
            QQuickItem* item = find(view, QString::fromLatin1(name));
            QVERIFY2(item, name);
            QVERIFY2(inside.contains(rectIn(item, view)), name);
        }
        // Along its top, the source, Listen and EQ from its left edge, 6 px apart, the types side by side at its
        // right edge, after them; under them the knobs spanning it, each cell as wide as its texts need.
        double left = inside.left() - 6;
        for (const char* name : {"sidechainSource", "sc_listen", "sc_eq"}) {
            const QRectF rect = rectIn(find(view, QString::fromLatin1(name)), view);
            QCOMPARE(rect.left(), left + 6);
            left = rect.right();
        }
        QVERIFY(rectIn(find(view, QStringLiteral("eqType0")), view).left() >= left + 2);
        QCOMPARE(rectIn(find(view, QStringLiteral("eqType5")), view).right(), inside.right());
        QCOMPARE(rectIn(find(view, QStringLiteral("sc_gain")), view).left(), inside.left());
        QCOMPARE(rectIn(find(view, QStringLiteral("sc_eq_gain")), view).right(), inside.right());
        const QList<QQuickItem*> scCells = cells(view, {"sc_gain", "sc_mix"});
        const QList<QQuickItem*> eqCells = cells(view, {"sc_eq_freq", "sc_eq_q", "sc_eq_gain"});
        for (QQuickItem* knobCell : scCells + eqCells) {
            const QString problem = knobTextsProblem(knobCell);
            QVERIFY2(problem.isEmpty(), qPrintable(problem));
        }
        // And no wider: Gain's and Dry/Wet's as their widest text (46 px at least), the EQ's three as theirs (52
        // at least) or as the buttons above them need.
        const double scCell = scCells.first()->width(), buttons = left + 2 + 6 * 17 - inside.left();
        QCOMPARE(scCell, std::max(46.0, std::ceil(knobsNeed(scCells))));
        QCOMPARE(eqCells.first()->width(),
                 std::max({52.0, std::ceil(knobsNeed(eqCells)), std::ceil((buttons - 2 * scCell - 6) / 3)}));
        // "No Sidechain" whole beside its arrow (the button 96 px, or as wide as that needs); "EQ" inside its
        // button, 2 px clear of either side (22 px at least).
        QQuickItem* source = find(view, QStringLiteral("sidechainSource"));
        QQuickItem* sourceLabel = source->property("contentItem").value<QQuickItem*>()->childItems().first();
        const double nameNeeds = textWidth(QStringLiteral("No Sidechain"), sourceLabel->property("font"));
        QVERIFY(!sourceLabel->property("truncated").toBool() && nameNeeds <= sourceLabel->width());
        QVERIFY2(source->width() == 96 || sourceLabel->width() == std::ceil(nameNeeds),
                 qPrintable(QStringLiteral("%1 in %2").arg(nameNeeds).arg(sourceLabel->width())));
        QQuickItem* eq = button(view, "sc_eq");
        QVERIFY(eq->property("implicitContentWidth").toDouble() + 4 <= eq->width());
        QCOMPARE(eq->width(), std::max(22.0, std::ceil(eq->property("implicitContentWidth").toDouble()) + 6));
        QVERIFY(rectIn(shown.graph, view).left() >= inside.right() + 6);
        auto enabled = [&](const char* name) { return find(view, QString::fromLatin1(name))->isEnabled(); };
        auto checked = [&](int type) {
            return find(view, QStringLiteral("eqType%1").arg(type))->property("checked").toBool();
        };
        // Dimmed (its fade done): set for later, or not used by the EQ's type.
        auto dimmed = [&](const char* name) { return find(view, QString::fromLatin1(name))->opacity() < 0.99; };
        QVERIFY(checked(5) && !checked(0));  // high-pass
        // The type buttons' faces are the EQ's (EqTypeIcon), its types in the S/C EQ Type's order, dimmed while
        // the EQ is off.
        const int kinds[] = {EqGraph::LowShelf, EqGraph::Bell,     EqGraph::HighShelf,
                             EqGraph::HighCut,  EqGraph::BandPass, EqGraph::LowCut};
        for (int type = 0; type < 6; ++type) {
            QQuickItem* face = qvariant_cast<QQuickItem*>(
                find(view, QStringLiteral("eqType%1").arg(type))->property("background"));
            QVERIFY(qobject_cast<EqTypeIcon*>(face));
            QCOMPARE(face->property("kind").toInt(), kinds[type]);
        }
        QTRY_COMPARE(find(view, QStringLiteral("eqType0"))->parentItem()->opacity(), 0.55);
        QVERIFY(knob(view, "sc_eq_gain")->bipolar());
        for (const char* name : {"sc_gain", "sc_mix", "sc_eq_freq", "sc_eq_q"})
            QVERIFY2(!knob(view, name)->bipolar(), name);
        QCOMPARE(textOf(view, "sidechainSource"), QStringLiteral("No Sidechain"));
        // Set for later, dimmed but settable: Gain and Dry/Wet without a sidechain, the EQ's knobs while it
        // is off (as its type buttons and its curve). Only what the type doesn't use is disabled (a high-pass:
        // the gain).
        QVERIFY(enabled("sc_gain") && enabled("sc_mix"));
        QVERIFY(enabled("sc_eq_freq") && enabled("sc_eq_q") && !enabled("sc_eq_gain"));
        QTRY_VERIFY(dimmed("sc_gain") && dimmed("sc_mix"));
        QTRY_VERIFY(dimmed("sc_eq_freq") && dimmed("sc_eq_q") && dimmed("sc_eq_gain"));
        for (const char* id : {"sc_eq_freq", "sc_gain"}) {
            const double before = value(id);
            const int steps = undo()->index();
            const QPoint at = centerOf(knob(view, id));
            QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
            dragTo(at - QPoint(0, 10));
            dragTo(at - QPoint(0, 20));
            QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 20));
            QVERIFY2(value(id) > before, id);
            QCOMPARE(undo()->index(), steps + 1);
            undo()->undo();
            QCOMPARE(value(id), before);
        }

        // The EQ on: Freq and Q (a high-pass has no gain); a bell: Gain too; a low shelf: no Q.
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(button(view, "sc_eq")));
        QCOMPARE(value("sc_eq"), 1.0);
        QVERIFY(enabled("sc_eq_freq") && enabled("sc_eq_q") && !enabled("sc_eq_gain"));
        QTRY_VERIFY(!dimmed("sc_eq_freq") && !dimmed("sc_eq_q") && dimmed("sc_eq_gain"));
        // Every cell's readout shows its widest values whole ("15.00 kHz", "-70.0 dB": each cell is wide enough).
        const std::pair<const char*, double> widest[] = {
            {"sc_eq_freq", 10000.0}, {"sc_eq_freq", 15000.0}, {"sc_eq_q", 0.1}, {"sc_eq_q", 12.0},
            {"sc_eq_gain", -15.0},   {"sc_gain", -70.0},      {"sc_gain", 24.0}, {"sc_mix", 100.0}};
        for (const auto& [id, at] : widest) {
            const bool set = value(id) != at;  // (Dry/Wet's 100 % is its default)
            if (set)
                editor()->setDeviceParam(shown.track, shown.device, QString::fromLatin1(id), at);
            QQuickItem* readout = find(view, QString::fromLatin1(id))->childItems().last();
            QVERIFY2(!readout->property("truncated").toBool(), qPrintable(readout->property("text").toString()));
            if (set)
                undo()->undo();
        }
        QCOMPARE(value("sc_eq_freq"), 80.0);
        int steps = undo()->index();
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(find(view, QStringLiteral("eqType1"))));
        QCOMPARE(value("sc_eq_type"), 1.0);
        QCOMPARE(undo()->index(), steps + 1);
        QVERIFY(checked(1) && !checked(5));
        QVERIFY(enabled("sc_eq_gain") && enabled("sc_eq_q"));
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(find(view, QStringLiteral("eqType0"))));
        QCOMPARE(value("sc_eq_type"), 0.0);
        QVERIFY(enabled("sc_eq_gain") && !enabled("sc_eq_q"));
        QTRY_VERIFY(!dimmed("sc_eq_gain") && dimmed("sc_eq_q"));
        undo()->undo();
        undo()->undo();
        QCOMPARE(value("sc_eq_type"), 5.0);
        undo()->undo();
        QCOMPARE(value("sc_eq"), 0.0);

        // Listen: a switch, undoable.
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(button(view, "sc_listen")));
        QCOMPARE(value("sc_listen"), 1.0);
        undo()->undo();
        QCOMPARE(value("sc_listen"), 0.0);

        // A sidechain from another track: the source button names it (and follows a rename), the
        // external knobs come on.
        const QString other = audioTrackWith(tone(110.0, kSampleRate), QStringLiteral("other"), 1.0);
        QVERIFY(!other.isEmpty());
        editor()->setDeviceSidechain(shown.track, shown.device, Sidechain{other, kPreFader});
        QVERIFY(shown.graph->keyed());
        const QString name = project()->findTrack(other)->name;
        QCOMPARE(textOf(view, "sidechainSource"), name);
        editor()->renameTrack(other, QStringLiteral("Kick"));
        QCOMPARE(textOf(view, "sidechainSource"), QStringLiteral("Kick"));
        QTRY_VERIFY(!dimmed("sc_gain") && !dimmed("sc_mix"));
        QSignalSpy menu(view, SIGNAL(sidechainMenuRequested(QVariant)));
        click(find(view, QStringLiteral("sidechainSource")));
        QCOMPARE(menu.count(), 1);  // (the frame opens the menu under the button it names)
        QCOMPARE(qvariant_cast<QQuickItem*>(menu.at(0).at(0)), find(view, QStringLiteral("sidechainSource")));
        undo()->undo();  // (the rename)
        undo()->undo();  // (the sidechain)
        QVERIFY(!shown.graph->keyed());
        QCOMPARE(textOf(view, "sidechainSource"), QStringLiteral("No Sidechain"));

        // Shown again (the editor made anew), it keeps its section unfolded; folded, the width comes back.
        QQuickItem* again = show(QStringLiteral("gate"), shown.track, shown.device);
        QVERIFY(again);
        QCOMPARE(again->implicitWidth(), folded + sectionWidth + 13);
        click(find(again, QStringLiteral("sidechainFold")));
        QTRY_COMPARE(again->implicitWidth(), folded);
    }

    void keyGraph() {
        const Shown shown = gate(tone(220.0, kSampleRate), 1.0);
        QVERIFY(shown.view && shown.graph);
        auto value = [&](const char* id) { return param(shown.track, shown.device, QString::fromLatin1(id)); };
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                          centerOf(find(shown.view, QStringLiteral("sidechainFold"))));
        QTRY_COMPARE(shown.view->implicitWidth(), shown.view->property("foldedWidth").toDouble()
                                                     + shown.view->property("sectionWidth").toDouble() + 13);
        QVERIFY(fitted());
        auto* graph = find<GateKeyGraph>(shown.view, QStringLiteral("keyGraph"));
        QVERIFY(graph);
        QVERIFY(graph->height() >= GateKeyGraph::kMinimumHeight);
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("sc_eq"), 1.0);
        QVERIFY(graph->active());

        // The curve is the engine's filter at the engine's rate, for the parameters as the model has them.
        auto checkCurve = [&] {
            const int type = int(value("sc_eq_type"));
            const double freq = value("sc_eq_freq"), q = value("sc_eq_q"), gain = value("sc_eq_gain");
            const std::vector<double>& frequencies = graph->frequencies();
            const std::vector<double>& response = graph->responseDb();
            QVERIFY(frequencies.size() > 200);
            for (size_t i = 0; i < frequencies.size(); i += 17)
                QCOMPARE(response[i],
                         gateKeyFilterDb(type, freq, q, gain, bridge()->sampleRate(), {frequencies[i]}).value(0));
        };
        QCOMPARE(value("sc_eq_type"), 5.0);
        checkCurve();

        // A bell: across for the frequency, up for the gain, in one step; the dot following.
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("sc_eq_type"), 1.0);
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("sc_eq_freq"), 300.0);
        checkCurve();
        int steps = undo()->index();
        QPoint at = scenePoint(graph, graph->dot());
        const QPointF dot = graph->dot();
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at + QPoint(20, -5));
        dragTo(at + QPoint(40, -10));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(40, -10));
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(undo()->text(undo()->index() - 1), QStringLiteral("Change Gate Key Filter"));
        const double freq = value("sc_eq_freq"), gain = value("sc_eq_gain");
        QVERIFY2(std::abs(freq / graph->freqAt(dot.x() + 40) - 1.0) < 0.01, qPrintable(QString::number(freq)));
        QVERIFY2(gain > 1.0, qPrintable(QString::number(gain)));
        QVERIFY(std::abs(graph->dot().x() - graph->xOf(freq)) < 1e-6);
        QVERIFY(std::abs(graph->dot().y() - graph->yOf(gain)) < 1e-6);
        // The dot under the mouse: 10 px up is that many dB on the graph's scale.
        QVERIFY2(std::abs(graph->dot().y() - (dot.y() - 10)) < 0.5, qPrintable(QString::number(graph->dot().y())));
        QVERIFY2(std::abs(gain - (graph->dbAt(dot.y() - 10) - graph->dbAt(dot.y()))) < 0.1,
                 qPrintable(QString::number(gain)));
        checkCurve();

        // The bell's Q: Ctrl-drag up doubles it every kQPixels (the gain stays); the wheel over the dot
        // sets it too, notches close together one step; off the dot the wheel goes on.
        const double q0 = value("sc_eq_q");
        steps = undo()->index();
        at = scenePoint(graph, graph->dot());
        QTest::mousePress(window_, Qt::LeftButton, Qt::ControlModifier, at);
        dragTo(at - QPoint(0, int(GateKeyGraph::kQPixels)), Qt::ControlModifier);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::ControlModifier, at - QPoint(0, int(GateKeyGraph::kQPixels)));
        QVERIFY2(std::abs(value("sc_eq_q") / (2 * q0) - 1.0) < 0.01, qPrintable(QString::number(value("sc_eq_q"))));
        QCOMPARE(value("sc_eq_gain"), gain);
        QCOMPARE(undo()->index(), steps + 1);
        const double q1 = value("sc_eq_q");
        wheel(window_, scenePoint(graph, graph->dot()), 120);
        wheel(window_, scenePoint(graph, graph->dot()), 120);
        QVERIFY2(std::abs(value("sc_eq_q") / (q1 * 1.15 * 1.15) - 1.0) < 1e-3,
                 qPrintable(QString::number(value("sc_eq_q"))));
        QCOMPARE(undo()->index(), steps + 2);
        QCOMPARE(undo()->text(undo()->index() - 1), QStringLiteral("Change Gate Key Filter Q"));
        const double q2 = value("sc_eq_q");
        wheel(window_, scenePoint(graph, graph->dot() + QPointF(60, 0)), 120);
        QCOMPARE(value("sc_eq_q"), q2);
        undo()->undo();
        undo()->undo();
        QVERIFY(std::abs(value("sc_eq_q") - q0) < 1e-9);

        // However far it is dragged, the frequency stays in its range.
        at = scenePoint(graph, graph->dot());
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at + QPoint(800, 0));
        QCOMPARE(value("sc_eq_freq"), 15000.0);
        dragTo(at - QPoint(1600, 0));
        QCOMPARE(value("sc_eq_freq"), 30.0);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(1600, 0));

        // A low-pass: up doubles the Q every kQPixels.
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("sc_eq_type"), 3.0);
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("sc_eq_freq"), 1000.0);
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("sc_eq_q"), 1.0);
        steps = undo()->index();
        at = scenePoint(graph, graph->dot());
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at - QPoint(0, 30));
        dragTo(at - QPoint(0, int(GateKeyGraph::kQPixels)));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, int(GateKeyGraph::kQPixels)));
        QVERIFY2(std::abs(value("sc_eq_q") / 2.0 - 1.0) < 0.01, qPrintable(QString::number(value("sc_eq_q"))));
        QCOMPARE(value("sc_eq_freq"), 1000.0);
        QCOMPARE(undo()->index(), steps + 1);
        checkCurve();

        // A double-click: Freq, Q and Gain back to their defaults, one step.
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, scenePoint(graph, graph->dot()));
        QCOMPARE(value("sc_eq_freq"), 80.0);
        QVERIFY(std::abs(value("sc_eq_q") - 0.71) < 1e-6);
        QCOMPARE(value("sc_eq_gain"), 0.0);
        QCOMPARE(undo()->index(), steps + 2);
    }

    void engineHasIt() {
        const Shown shown = gate(tone(220.0, kSampleRate), 1.0);
        QVERIFY(shown.view && shown.graph);
        GateGraph* graph = shown.graph;
        tick(3);
        const QPoint at = scenePoint(graph, QPointF(graph->plot().center().x(), graph->thresholdY()));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at + QPoint(0, 25));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(0, 25));
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(button(shown.view, "flip")));
        const auto id = bridge()->engineDeviceId(shown.track, shown.device);
        QVERIFY(id);
        for (const char* name : {"threshold", "flip"}) {
            const float model = float(param(shown.track, shown.device, QString::fromLatin1(name)));
            QTRY_COMPARE(engine()->processorParam(*id, engine()->processorParamIndex(*id, name)), model);
        }
        QVERIFY(param(shown.track, shown.device, QStringLiteral("threshold")) < -13.0);
    }

    void screenshots() {
        if (qEnvironmentVariable("SUBSTATION_UI_SCREENSHOTS").isEmpty())
            QSKIP("SUBSTATION_UI_SCREENSHOTS not set");
        const Shown shown = gate(bursts(4.0), 4.0);
        QVERIFY(shown.view && shown.graph);
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("threshold"), -30.0);
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("release"), 60.0);
        const double beatsPerSecond = project()->tempo() / 60.0;
        // 2.6 s at once, then as it plays: a tick's worth at a time (so it isn't idle when shot).
        auto play = [&](int ticks) {
            engine()->renderOffline(0.0, int(kSampleRate * 2.6));
            for (int i = 0; i < ticks; ++i) {
                engine()->renderOffline((2.6 + i / 60.0) * beatsPerSecond, kSampleRate / 60);
                tick(1);
            }
        };
        play(30);
        QVERIFY(!shown.graph->idle());
        QTest::qWait(50);
        save(grab(), QStringLiteral("gate.png"));

        // Hovering the threshold line: its value.
        GateGraph* graph = shown.graph;
        QTest::mouseMove(window_, scenePoint(graph, QPointF(graph->plot().center().x(), graph->thresholdY())));
        play(20);
        QTest::qWait(50);
        save(grab(), QStringLiteral("gate-hover.png"));
        QTest::mouseMove(window_, QPoint(1, 1));

        // The sidechain section, its EQ a band-pass at 200 Hz: the key drawn too.
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("sc_eq"), 1.0);
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("sc_eq_type"), 4.0);
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("sc_eq_freq"), 200.0);
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("sc_eq_q"), 2.0);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                          centerOf(find(shown.view, QStringLiteral("sidechainFold"))));
        QTRY_COMPARE(shown.view->implicitWidth(), shown.view->property("foldedWidth").toDouble()
                                                     + shown.view->property("sectionWidth").toDouble() + 13);
        QVERIFY(fitted());
        QTest::mouseMove(window_, QPoint(1, 1));
        play(30);
        QTest::qWait(50);
        save(grab(), QStringLiteral("gate-sidechain.png"));

        // Flipped and listening.
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("flip"), 1.0);
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("sc_listen"), 1.0);
        play(30);
        QTest::qWait(50);
        save(grab(), QStringLiteral("gate-flip-listen.png"));
    }
};

QTEST_MAIN(TestUiDeviceEditorsGate)
#include "test_ui_device_editors_gate.moc"
