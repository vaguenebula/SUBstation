// The Gate's editor (ui/qml/devices/editors/GateEditor.qml, GateGraph,
// GateKeyGraph) loaded as the device view loads it, over a real engine: it fits
// the view's height; every control is bound to its parameter (undoably, its
// value as it is now); the threshold and return lines and the key filter's dot
// drag in one undo step each; what the engine renders reaches the display, which
// scrolls and stops; the sidechain section folds and unfolds, follows the
// sidechain and its EQ; the key filter's curve is the engine's. With
// SUBSTATION_UI_SCREENSHOTS set, device-editors-gate*.png are saved there.

#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <vector>

#include "EditorHarness.h"
#include "audio/GateResponse.h"
#include "controls/KnobItem.h"
#include "devices/GateGraph.h"
#include "devices/GateKeyGraph.h"
#include "model/Device.h"

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

    // `item`'s rect in `in`'s coordinates.
    static QRectF rectIn(QQuickItem* item, QQuickItem* in) {
        return item->mapRectToItem(in, QRectF(0, 0, item->width(), item->height()));
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
        QCOMPARE(view->implicitWidth(), 566.0);

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
                                 "sidechainFold"}) {
            const QRectF rect = rectIn(find(view, QString::fromLatin1(name)), view);
            QVERIFY2(body.adjusted(8, 6, -8, -6).contains(rect), name);
            QVERIFY2(!rect.intersects(graph), name);
        }
        QTest::qWait(50);
        save(grab(), QStringLiteral("gate-idle.png"));
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
        tick(30);

        // Anywhere else in the plot: the threshold too, relative (no jump at the press).
        at = scenePoint(graph, QPointF(plot.center().x(), plot.top() + 12));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(value(), -40.0);
        dragTo(at + QPoint(0, 20));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(0, 20));
        QVERIFY2(std::abs(value() - (-40.0 - 20 * perPixel)) < 0.2, qPrintable(QString::number(value())));
        undo()->undo();
        tick(30);

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
        undo()->undo();
        tick(30);

        // A double-click on the threshold line puts it back (one step); on the return line, Return.
        at = scenePoint(graph, QPointF(plot.center().x() + 30, graph->thresholdY()));
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(value("threshold"), -12.0);
        QCOMPARE(undo()->index(), steps + 2);
        tick(30);
        at = scenePoint(graph, QPointF(plot.center().x() - 30, graph->returnY()));
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(value("return"), 3.0);
        QCOMPARE(undo()->index(), steps + 3);
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
        for (int i = 0; i < 400 && graph->animating(); ++i)
            tick(1, 16);
        QVERIFY(!graph->animating());
        const double settled = graph->scroll();
        tick(3);
        QCOMPARE(graph->scroll(), settled);
        QVERIFY(!graph->animating());

        // Closed: the floor's -40 dB under the input.
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("threshold"), 0.0);
        engine()->renderOffline(0.0, kSampleRate / 2);
        tick(3, 20);
        QVERIFY2(graph->passing() < 0.01, qPrintable(QString::number(graph->passing())));
        QVERIFY2(std::abs(graph->levelOut() - (20 * std::log10(0.5) - 40.0)) < 0.2,
                 qPrintable(QString::number(graph->levelOut())));
        QVERIFY(!graph->property("open").toBool());
    }

    void silenceDrawsNothing() {
        // Silence flowing in (as from a running audio device): the history scrolls on, but nothing it
        // draws changes, so the graph doesn't repaint.
        const Shown shown = gate(std::vector<float>(size_t(kSampleRate) * 6, 0.f), 6.0);
        QVERIFY(shown.view && shown.graph);
        GateGraph* graph = shown.graph;
        const double beatsPerSecond = project()->tempo() / 60.0;
        engine()->renderOffline(0.0, kSampleRate * 3);
        for (int i = 0; i < 100 && (i < 3 || graph->animating()); ++i)
            tick(1);
        QVERIFY(!graph->animating());
        for (int i = 0; i < 4; ++i) {
            const double before = graph->scroll();
            const qint64 newest = graph->newest();
            engine()->renderOffline((3.0 + 0.1 * i) * beatsPerSecond, kSampleRate / 10);
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

    void sidechainSection() {
        const Shown shown = gate(tone(220.0, kSampleRate), 1.0);
        QVERIFY(shown.view && shown.graph);
        QQuickItem* view = shown.view;
        auto value = [&](const char* id) { return param(shown.track, shown.device, QString::fromLatin1(id)); };
        QQuickItem* section = find(view, QStringLiteral("sidechainSection"));
        QVERIFY(section);
        QCOMPARE(section->width(), 0.0);
        QVERIFY(!section->isVisible());

        // Unfolded: wider, as tall as ever.
        click(find(view, QStringLiteral("sidechainFold")));
        QTRY_COMPARE_WITH_TIMEOUT(section->width(), 254.0, 300);
        QTRY_COMPARE(view->implicitWidth(), 833.0);
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
        QVERIFY(rectIn(shown.graph, view).left() >= inside.right() + 6);
        auto enabled = [&](const char* name) { return find(view, QString::fromLatin1(name))->isEnabled(); };
        auto checked = [&](int type) {
            return find(view, QStringLiteral("eqType%1").arg(type))->property("checked").toBool();
        };
        QVERIFY(!enabled("sc_gain") && !enabled("sc_mix"));  // (no sidechain)
        QVERIFY(!enabled("sc_eq_freq") && !enabled("sc_eq_q") && !enabled("sc_eq_gain"));  // (the EQ off)
        QVERIFY(checked(5) && !checked(0));  // high-pass
        QCOMPARE(textOf(view, "sidechainSource"), QStringLiteral("No Sidechain"));

        // The EQ on: Freq and Q (a high-pass has no gain); a bell: Gain too; a low shelf: no Q.
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(button(view, "sc_eq")));
        QCOMPARE(value("sc_eq"), 1.0);
        QVERIFY(enabled("sc_eq_freq") && enabled("sc_eq_q") && !enabled("sc_eq_gain"));
        int steps = undo()->index();
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(find(view, QStringLiteral("eqType1"))));
        QCOMPARE(value("sc_eq_type"), 1.0);
        QCOMPARE(undo()->index(), steps + 1);
        QVERIFY(checked(1) && !checked(5));
        QVERIFY(enabled("sc_eq_gain") && enabled("sc_eq_q"));
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(find(view, QStringLiteral("eqType0"))));
        QCOMPARE(value("sc_eq_type"), 0.0);
        QVERIFY(enabled("sc_eq_gain") && !enabled("sc_eq_q"));
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
        QVERIFY(enabled("sc_gain") && enabled("sc_mix"));
        QSignalSpy menu(view, SIGNAL(sidechainMenuRequested()));
        click(find(view, QStringLiteral("sidechainSource")));
        QCOMPARE(menu.count(), 1);
        undo()->undo();  // (the rename)
        undo()->undo();  // (the sidechain)
        QVERIFY(!shown.graph->keyed());
        QCOMPARE(textOf(view, "sidechainSource"), QStringLiteral("No Sidechain"));

        // Shown again (the editor made anew), it keeps its section unfolded; folded, the width comes back.
        QQuickItem* again = show(QStringLiteral("gate"), shown.track, shown.device);
        QVERIFY(again);
        QCOMPARE(again->implicitWidth(), 833.0);
        click(find(again, QStringLiteral("sidechainFold")));
        QTRY_COMPARE(again->implicitWidth(), 566.0);
    }

    void keyGraph() {
        const Shown shown = gate(tone(220.0, kSampleRate), 1.0);
        QVERIFY(shown.view && shown.graph);
        auto value = [&](const char* id) { return param(shown.track, shown.device, QString::fromLatin1(id)); };
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                          centerOf(find(shown.view, QStringLiteral("sidechainFold"))));
        QTRY_COMPARE(shown.view->implicitWidth(), 833.0);
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
        checkCurve();

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
        const Shown shown = gate(bursts(3.0), 3.0);
        QVERIFY(shown.view && shown.graph);
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("threshold"), -30.0);
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("release"), 60.0);
        engine()->renderOffline(0.0, int(kSampleRate * 2.6));
        tick(30);
        QTest::qWait(50);
        save(grab(), QStringLiteral("gate.png"));

        // Hovering the threshold line: its value.
        GateGraph* graph = shown.graph;
        QTest::mouseMove(window_, scenePoint(graph, QPointF(graph->plot().center().x(), graph->thresholdY())));
        tick(20);
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
        QTRY_COMPARE(shown.view->implicitWidth(), 833.0);
        QVERIFY(fitted());
        QTest::mouseMove(window_, QPoint(1, 1));
        engine()->renderOffline(0.0, int(kSampleRate * 2.6));
        tick(30);
        QTest::qWait(50);
        save(grab(), QStringLiteral("gate-sidechain.png"));

        // Flipped and listening.
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("flip"), 1.0);
        editor()->setDeviceParam(shown.track, shown.device, QStringLiteral("sc_listen"), 1.0);
        engine()->renderOffline(0.0, int(kSampleRate * 2.6));
        tick(30);
        QTest::qWait(50);
        save(grab(), QStringLiteral("gate-flip-listen.png"));
    }
};

QTEST_MAIN(TestUiDeviceEditorsGate)
#include "test_ui_device_editors_gate.moc"
