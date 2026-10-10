// The Erosion's editor (ui/qml/devices/editors/ErosionEditor.qml, ErosionGraph,
// ErosionScope): loaded as the device view loads it, over a real engine; its
// knobs, its X-Y display's drags (Shift, Alt) and wheel (in the device chain
// too), what the engine's displays bring it (and not a backlog's worth after the
// sound stopped), and what the engine plays after. With
// SUBSTATION_UI_SCREENSHOTS set to a folder, it is saved there as PNGs.

#include <QCursor>
#include <QElapsedTimer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <vector>

#include "EditorHarness.h"
#include "audio/EngineBridge.h"
#include "audio/ErosionResponse.h"
#include "controls/KnobItem.h"
#include "devices/DeviceChainArea.h"
#include "devices/DeviceParam.h"
#include "devices/DisplayClock.h"
#include "devices/EditorPaint.h"
#include "devices/ErosionGraph.h"
#include "devices/ErosionScope.h"
#include "editor/ProjectEditor.h"

using namespace sub::app;
using namespace sub::ui;
using sub::app::test::kSampleRate;

class TestUiDeviceEditorsErosion : public QObject, public sub::app::test::EditorHarness {
    Q_OBJECT

    QString track_, device_;

    // A track playing `mono` (a 3 kHz tone by default) through an Erosion, its editor shown.
    QQuickItem* showErosion(const std::vector<float>& mono = tone(3000.0, kSampleRate)) {
        track_ = audioTrackWith(mono, QStringLiteral("tone"), 1.0);
        if (track_.isEmpty())
            return nullptr;
        device_ = editor()->addDevice(track_, QStringLiteral("erosion"));
        return show(QStringLiteral("erosion"), track_, device_);
    }

    double value(const char* id) { return param(track_, device_, QString::fromLatin1(id)); }
    void set(const char* id, double v) { editor()->setDeviceParam(track_, device_, QString::fromLatin1(id), v); }

    // An EditorKnob's dial: the cell's ParamKnob's KnobItem.
    KnobItem* knob(QQuickItem* view, const char* id) {
        QQuickItem* cell = find(view, QString::fromLatin1(id));
        auto* paramKnob = cell ? qvariant_cast<QQuickItem*>(cell->property("knob")) : nullptr;
        return paramKnob ? qvariant_cast<KnobItem*>(paramKnob->property("knob")) : nullptr;
    }

    // The parameter an EditorKnob is bound to.
    sub::ui::DeviceParam* boundParam(QQuickItem* view, const char* id) {
        QQuickItem* cell = find(view, QString::fromLatin1(id));
        return cell ? qvariant_cast<sub::ui::DeviceParam*>(cell->property("param")) : nullptr;
    }

    // The texts shown in an EditorKnob's cell (its caption and its readout).
    QStringList texts(QQuickItem* view, const char* id) {
        QStringList found;
        if (QQuickItem* cell = find(view, QString::fromLatin1(id))) {
            for (QQuickItem* child : cell->childItems()) {
                if (child->inherits("QQuickText"))
                    found << child->property("text").toString();
            }
        }
        return found;
    }

    // Every item under `root`, recursively.
    static void collect(QQuickItem* root, QList<QQuickItem*>& out) {
        for (QQuickItem* child : root->childItems()) {
            out << child;
            collect(child, out);
        }
    }

    static bool near(double a, double b, double relative) { return std::abs(a / b - 1.0) <= relative; }

    // A knob's drag puts the pointer back where it was pressed when it is released (DragCursor), and the
    // window system delivers that as a move of its own, later: let it land before the next press, or it
    // lands in the next drag (as a jump from where the last knob was).
    void pointerBackAt(QPoint pressed) {
        const QPoint global = window_->mapToGlobal(pressed);
        (void)QTest::qWaitFor([&] { return QCursor::pos() == global; }, 500);  // (if it can be warped at all)
        QTest::qWait(50);
    }

    // As playing does: rendered a piece at a time, the displays refreshed after each (each render starts
    // afresh, so pieces as long as the analyzer's window keep the seams out of sight).
    void play(int pieces, int frames = 4096) {
        const double beatsPerFrame = project()->tempo() / 60.0 / kSampleRate;
        for (int i = 0; i < pieces; ++i) {
            engine()->renderOffline(i * frames * beatsPerFrame, frames);
            refreshDisplays();
        }
    }

    // A buzz: the first 24 harmonics of 110 Hz, falling as 1/n (a soft sawtooth), peaking near 0.5.
    static std::vector<float> buzz(int frames) {
        std::vector<float> samples(static_cast<size_t>(frames));
        for (int i = 0; i < frames; ++i) {
            double sum = 0.0;
            for (int n = 1; n <= 24; ++n)
                sum += std::sin(2 * 3.14159265358979323846 * 110.0 * n * i / kSampleRate) / n;
            samples[size_t(i)] = float(0.28 * sum);
        }
        return samples;
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
                                  Q_ARG(QVariant, QStringLiteral("erosion")));
        QVERIFY(url.toString().endsWith(QStringLiteral("ErosionEditor.qml")));
    }

    void fitsAndShowsItsParameters() {
        QQuickItem* view = showErosion();
        QVERIFY(view);
        // It fits the device view's body, as wide as its parts.
        QVERIFY2(view->implicitHeight() <= bodyHeight(),
                 qPrintable(QStringLiteral("%1 > %2").arg(view->implicitHeight()).arg(bodyHeight())));
        const int cell = view->property("cellWidth").toInt();
        QCOMPARE(view->implicitWidth(), double(8 + 3 * cell + 300 + 2 * 8 + 4 + 8 - 2));
        QCOMPARE(view->implicitWidth(), 532.0);
        // Nothing overflows the fixed body (the scope's cell included), nothing is clipped.
        QList<QQuickItem*> items;
        collect(view, items);
        const QRectF body(0, 0, view->width(), view->height());
        for (QQuickItem* item : items) {
            if (!item->isVisible() || item->width() <= 0 || item->height() <= 0)
                continue;
            const QRectF rect = item->mapRectToItem(view, QRectF(0, 0, item->width(), item->height()));
            QVERIFY2(body.adjusted(-0.5, -0.5, 0.5, 0.5).contains(rect),
                     qPrintable(QStringLiteral("%1 (%2) at %3,%4 %5x%6")
                                    .arg(item->objectName(), QString::fromLatin1(item->metaObject()->className()))
                                    .arg(rect.x())
                                    .arg(rect.y())
                                    .arg(rect.width())
                                    .arg(rect.height())));
            // No caption or readout is cut short ("Noise Blend" and "Frequency" included).
            if (item->inherits("QQuickText"))
                QVERIFY2(!item->property("truncated").toBool(),
                         qPrintable(QStringLiteral("%1: %2 px in %3")
                                        .arg(item->property("text").toString())
                                        .arg(item->property("implicitWidth").toDouble())
                                        .arg(item->width())));
        }

        // The knobs, reading their parameters; Frequency and Width turn in log.
        KnobItem* freq = knob(view, "freq");
        KnobItem* width = knob(view, "width");
        KnobItem* amount = knob(view, "amount");
        KnobItem* blend = knob(view, "blend");
        KnobItem* stereo = knob(view, "stereo");
        QVERIFY(freq && width && amount && blend && stereo);
        QCOMPARE(freq->value(), 1000.0);
        QCOMPARE(width->value(), 2.5);
        QCOMPARE(amount->value(), 25.0);
        QCOMPARE(blend->value(), 100.0);
        QCOMPARE(stereo->value(), 0.0);
        QVERIFY(freq->logScale() && width->logScale() && !amount->logScale());
        QVERIFY(texts(view, "freq").contains(QStringLiteral("1.00 kHz")));
        QVERIFY(texts(view, "width").contains(QStringLiteral("2.50 oct")));
        QVERIFY(texts(view, "amount").contains(QStringLiteral("25 %")));
        QVERIFY(texts(view, "blend").contains(QStringLiteral("Noise Blend")));
        QVERIFY(texts(view, "stereo").contains(QStringLiteral("Stereo")));

        auto* graph = find<ErosionGraph>(view, QStringLiteral("erosionGraph"));
        auto* scope = find<ErosionScope>(view, QStringLiteral("erosionScope"));
        QVERIFY(graph && scope);
        QVERIFY(graph->height() >= ErosionGraph::kMinimumHeight);
        QCOMPARE(graph->mapRectToItem(view, QRectF(0, 0, graph->width(), graph->height())).top(), 6.0);
        const double graphBottom = graph->mapRectToItem(view, QRectF(0, 0, 1, graph->height())).bottom();
        QVERIFY(std::abs(view->height() - graphBottom - 6.0) < 0.5);
        QVERIFY(scope->width() >= 40 && scope->width() == scope->height());
        // The glyphs: the noise lit, the sine faint; Width as bright as the others.
        QVERIFY(std::abs(find(view, QStringLiteral("sineGlyph"))->opacity() - 0.3) < 0.01);
        QVERIFY(std::abs(find(view, QStringLiteral("noiseGlyph"))->opacity() - 1.0) < 0.01);
        QCOMPARE(find(view, QStringLiteral("width"))->opacity(), 1.0);
    }

    void opensAsTheDeviceIs() {
        // The graph's Noise Blend weights, which the editor's glyphs and Width's dimming bind to, are only ever
        // the device's: given its session, track and device one at a time (as QML sets them), on a device left
        // at Blend 100 (the noise alone), it never reports what value() reads before the device is there (0s:
        // Blend 0's weights, the sine alone).
        track_ = audioTrackWith(tone(3000.0, kSampleRate), QStringLiteral("tone"), 1.0);
        QVERIFY(!track_.isEmpty());
        device_ = editor()->addDevice(track_, QStringLiteral("erosion"));
        QCOMPARE(value("blend"), 100.0);
        {
            ErosionGraph graph;
            QList<QPair<double, double>> reported;
            connect(&graph, &ErosionGraph::weightsChanged, this,
                    [&] { reported.append({graph.sineWeight(), graph.noiseWeight()}); });
            graph.setSession(session_.get());
            graph.setTrackId(track_);
            graph.setDeviceId(device_);
            QVERIFY(graph.alive());
            for (const auto& [sine, noise] : std::as_const(reported)) {
                QVERIFY2(sine == 0.0 && noise == 1.0,
                         qPrintable(QStringLiteral("reported %1, %2").arg(sine).arg(noise)));
            }
            QCOMPARE(graph.sineWeight(), 0.0);
            QCOMPARE(graph.noiseWeight(), 1.0);
        }

        // An editor opened on an Erosion playing its sine alone shows Width dimmed and the sine's glyph lit at
        // once.
        set("blend", 0.0);
        QQuickItem* view = show(QStringLiteral("erosion"), track_, device_);
        QVERIFY(view);
        QVERIFY2(std::abs(find(view, QStringLiteral("width"))->opacity() - 0.55) < 1e-6,
                 qPrintable(QString::number(find(view, QStringLiteral("width"))->opacity())));
        QVERIFY(std::abs(find(view, QStringLiteral("sineGlyph"))->opacity() - 1.0) < 1e-6);
        QVERIFY(std::abs(find(view, QStringLiteral("noiseGlyph"))->opacity() - 0.3) < 1e-6);
    }

    void controlsAreUndoable() {
        QQuickItem* view = showErosion();
        QVERIFY(view);
        auto* graph = find<ErosionGraph>(view, QStringLiteral("erosionGraph"));
        QVERIFY(graph);

        // Every knob is bound to its parameter: it follows an edit and its undo, and dragging it is
        // one undo step.
        const struct {
            const char* id;
            double other;
        } knobs[] = {{"freq", 4000.0}, {"width", 0.5}, {"amount", 60.0}, {"blend", 0.0}, {"stereo", 50.0}};
        for (const auto& [id, other] : knobs) {
            sub::ui::DeviceParam* p = boundParam(view, id);
            QVERIFY2(p && p->valid() && p->paramId() == QString::fromLatin1(id), id);
            KnobItem* dial = knob(view, id);
            QVERIFY(dial);
            const double before = dial->value();
            set(id, other);
            QCOMPARE(dial->value(), other);
            undo()->undo();
            QCOMPARE(dial->value(), before);

            const int steps = undo()->index();
            const QPoint at = centerOf(dial);
            QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
            for (int dy = 10; dy <= 30; dy += 10)
                dragTo(window_, at - QPoint(0, dy));
            QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 30));
            pointerBackAt(at);
            if (before < dial->to())
                QVERIFY2(value(id) > before, id);
            else
                QVERIFY2(value(id) == before, id);
            QCOMPARE(undo()->index(), steps + (value(id) != before ? 1 : 0));
            if (value(id) != before)
                undo()->undo();
            QCOMPARE(value(id), before);
        }

        // The graph follows the Amount.
        set("amount", 60.0);
        QCOMPARE(graph->dot().y(), graph->yOfAmount(60.0));
        undo()->undo();
        QCOMPARE(graph->dot().y(), graph->yOfAmount(25.0));

        // The glyphs as bright as the engine's equal-power weights say (through the graph: never copied).
        QQuickItem* sine = find(view, QStringLiteral("sineGlyph"));
        QQuickItem* noise = find(view, QStringLiteral("noiseGlyph"));
        QQuickItem* width = find(view, QStringLiteral("width"));
        set("blend", 30.0);
        const auto [sineWeight, noiseWeight] = erosionBlendWeights(30.0);
        QCOMPARE(view->property("sineWeight").toDouble(), sineWeight);
        QCOMPARE(view->property("noiseWeight").toDouble(), noiseWeight);
        QVERIFY(std::abs(sine->opacity() - (0.3 + 0.7 * sineWeight)) < 1e-6);
        QVERIFY(std::abs(noise->opacity() - (0.3 + 0.7 * noiseWeight)) < 1e-6);
        undo()->undo();

        // Noise Blend at 0: the sine's glyph lit, the noise's faint, Width dimmed (it does nothing to the sine).
        set("blend", 0.0);
        QVERIFY(std::abs(sine->opacity() - 1.0) < 0.01);
        QVERIFY(std::abs(noise->opacity() - 0.3) < 0.01);
        QTRY_VERIFY(std::abs(width->opacity() - 0.55) < 0.01);  // (EditorKnob's disabled look and 120 ms Behavior)
        QVERIFY(width->isEnabled());  // (still settable, as Live's)
        QCOMPARE(graph->noiseWeight(), 0.0);
        QCOMPARE(graph->sineWeight(), 1.0);
        QCOMPARE(view->property("sineWeight").toDouble(), 1.0);  // (the engine's weights, through the graph)
        QCOMPARE(view->property("noiseWeight").toDouble(), 0.0);
        QTest::qWait(50);
        save(grab(), QStringLiteral("erosion-sine.png"));
        undo()->undo();
        QTRY_COMPARE(width->opacity(), 1.0);
        QCOMPARE(graph->noiseWeight(), 1.0);
        QCOMPARE(graph->sineWeight(), 0.0);
    }

    void graphIsTheEnginesFilter() {
        QQuickItem* view = showErosion();
        QVERIFY(view);
        auto* graph = find<ErosionGraph>(view, QStringLiteral("erosionGraph"));
        QVERIFY(graph);
        const double rate = bridge()->sampleRate();
        const std::vector<double>& frequencies = graph->frequencies();
        const std::vector<double>& magnitudes = graph->magnitudes();
        QVERIFY(frequencies.size() >= size_t(graph->plot().width()));
        QCOMPARE(magnitudes.size(), frequencies.size());
        QVERIFY(std::is_sorted(frequencies.begin(), frequencies.end()));
        for (size_t i = 0; i < frequencies.size(); i += 17)
            QCOMPARE(magnitudes[i], erosionBandMagnitude(1000.0, 2.5, rate, {frequencies[i]})[0]);
        // It peaks at exactly 1 where the modulator is tuned (that frequency is one of the outline's).
        const size_t top = size_t(std::max_element(magnitudes.begin(), magnitudes.end()) - magnitudes.begin());
        QCOMPARE(magnitudes[top], 1.0);
        QCOMPARE(frequencies[top], graph->tunedFrequency());
        QCOMPARE(graph->tunedFrequency(), 1000.0);
        // The columns: all of the band in the one holding 1000 Hz, never less than the outline at their middles.
        const std::vector<double>& columns = graph->columnFrequencies();
        const std::vector<double>& columnMagnitudes = graph->columnMagnitudes();
        QVERIFY(!columns.empty() && columns.size() == columnMagnitudes.size());
        const std::vector<double> atColumns = [&] {
            const QList<double> list =
                erosionBandMagnitude(1000.0, 2.5, rate, QList<double>(columns.begin(), columns.end()));
            return std::vector<double>(list.begin(), list.end());
        }();
        bool holds = false;
        for (size_t c = 0; c < columns.size(); ++c) {
            QVERIFY(columnMagnitudes[c] >= atColumns[c]);
            const double left = graph->freqAt(graph->plot().left() + double(c) * ErosionGraph::kColumn);
            const double right = graph->freqAt(graph->plot().left() + double(c + 1) * ErosionGraph::kColumn);
            if (left <= 1000.0 && 1000.0 < right) {
                QCOMPARE(columnMagnitudes[c], 1.0);
                holds = true;
            }
        }
        QVERIFY(holds);
        QVERIFY(std::abs(graph->dot().x() - graph->xOf(1000.0)) < 1e-9);
        QCOMPARE(graph->dot().y(), graph->yOfAmount(25.0));
        // The band's edges, where the engine's filter is 3 dB down, and the excursion as it plays.
        const QPair<double, double> edges = erosionBandEdges(1000.0, 2.5, rate);
        QVERIFY(edges.first < 1000.0 && 1000.0 < edges.second);
        QVERIFY(std::abs(erosionBandMagnitude(1000.0, 2.5, rate, {edges.first})[0] - std::sqrt(0.5)) < 1e-3);
        QVERIFY(std::abs(graph->excursionMs() - erosionExcursionMs(25.0, rate)) < 1e-12);
        QCOMPARE(erosionExcursionText(erosionExcursionMs(25.0, 48000.0)), QStringLiteral("±87 µs"));
        QCOMPARE(erosionExcursionText(erosionExcursionMs(100.0, 48000.0)), QStringLiteral("±1.38 ms"));
        QCOMPARE(erosionExcursionText(0.0), QStringLiteral("±0 µs"));
        QCOMPARE(erosionExcursionText(0.0009), QStringLiteral("±0.9 µs"));

        // The dot's travel keeps its ring and its halo clear of the strip's texts at Amount 100, and its ring
        // inside the well at 0; across it, Amount and height map both ways.
        const QRectF plot = graph->plot();
        QVERIFY(graph->yOfAmount(100.0) - ErosionGraph::kDotRadius - ErosionGraph::kHaloGrowth > plot.top());
        QVERIFY(graph->yOfAmount(0.0) + ErosionGraph::kDotRadius + 1.0 <= plot.bottom());
        for (const double amount : {0.0, 12.5, 60.0, 100.0})
            QVERIFY(std::abs(graph->amountAt(graph->yOfAmount(amount)) - amount) < 1e-9);
        QCOMPARE(graph->amountAt(plot.top()), 100.0);
        QCOMPARE(graph->amountAt(plot.bottom()), 0.0);

        // Narrower, fewer frequencies are well inside the band; narrower than a pixel, it still peaks at the dot.
        const auto above = [&] {
            const std::vector<double>& magnitudes = graph->magnitudes();
            return std::count_if(magnitudes.begin(), magnitudes.end(), [](double m) { return m > 0.5; });
        };
        const auto wide = above();
        set("width", 0.5);
        QVERIFY(above() < wide);
        set("width", 0.1);
        QCOMPARE(*std::max_element(graph->magnitudes().begin(), graph->magnitudes().end()), 1.0);
        set("freq", 200.0);
        QVERIFY(std::abs(graph->dot().x() - graph->xOf(200.0)) < 1e-9);
        QCOMPARE(*std::max_element(graph->columnMagnitudes().begin(), graph->columnMagnitudes().end()), 1.0);
    }

    void graphDragIsOneUndoStep() {
        QQuickItem* view = showErosion();
        QVERIFY(view);
        auto* graph = find<ErosionGraph>(view, QStringLiteral("erosionGraph"));
        QVERIFY(graph);
        QSignalSpy dragging(graph, &ErosionGraph::draggingChanged);

        // The dot jumps to the press, then follows: across the Frequency, up the Amount; one undo step.
        int steps = undo()->index();
        const QPoint at = scenePoint(graph, QPointF(graph->xOf(250.0), graph->yOfAmount(40.0)));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        QVERIFY(graph->dragging());
        QVERIFY2(near(value("freq"), 250.0, 0.02), qPrintable(QString::number(value("freq"))));
        QVERIFY(std::abs(value("amount") - 40.0) < 1.0);
        const QPoint to = scenePoint(graph, QPointF(graph->xOf(500.0), graph->yOfAmount(60.0)));
        for (int i = 1; i <= 3; ++i)
            dragTo(window_, at + (to - at) * i / 3);
        QVERIFY(graph->dragging());
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, to);
        QVERIFY(!graph->dragging());
        QCOMPARE(dragging.count(), 2);
        QVERIFY2(near(value("freq"), 500.0, 0.02), qPrintable(QString::number(value("freq"))));
        QVERIFY2(std::abs(value("amount") - 60.0) <= 1.0, qPrintable(QString::number(value("amount"))));
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(undo()->count(), undo()->index());
        QCOMPARE(undo()->text(undo()->index() - 1), QStringLiteral("Change Erosion Frequency and Amount"));
        QVERIFY(std::abs(graph->dot().x() - graph->xOf(value("freq"))) < 1e-6);
        undo()->undo();
        QCOMPARE(value("freq"), 1000.0);
        QCOMPARE(value("amount"), 25.0);

        // Shift: from where the dot is (the press changes nothing), finely: the mouse's 100 px move it 15.
        steps = undo()->index();
        const QPoint shiftAt = scenePoint(graph, graph->dot() - QPointF(40, 0));
        QTest::mousePress(window_, Qt::LeftButton, Qt::ShiftModifier, shiftAt);
        QCOMPARE(value("freq"), 1000.0);
        QCOMPARE(value("amount"), 25.0);
        QCOMPARE(undo()->index(), steps);
        for (int dx = 25; dx <= 100; dx += 25)
            dragTo(window_, shiftAt + QPoint(dx, 0), Qt::ShiftModifier);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::ShiftModifier, shiftAt + QPoint(100, 0));
        const double expected = 1000.0 * std::pow(1000.0, 15.0 / graph->plot().width());
        QVERIFY2(near(value("freq"), expected, 0.005), qPrintable(QString::number(value("freq"))));
        QCOMPARE(value("amount"), 25.0);  // (it didn't move up or down: exactly as it was)
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();

        // Dragged out of the field and back, the dot comes back under the mouse.
        const QPoint from = scenePoint(graph, QPointF(graph->xOf(2000.0), graph->yOfAmount(50.0)));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, from);
        dragTo(window_, from + QPoint(0, 300));
        QCOMPARE(value("amount"), 0.0);
        dragTo(window_, from);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, from);
        QVERIFY2(std::abs(value("amount") - 50.0) < 1.0, qPrintable(QString::number(value("amount"))));
        undo()->undo();

        // A wheel notch while the dot is dragged does nothing (the drag has the mouse): still one undo step.
        steps = undo()->index();
        const QPoint start = scenePoint(graph, QPointF(graph->xOf(3000.0), graph->yOfAmount(30.0)));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, start);
        dragTo(window_, start + QPoint(10, -10));
        wheel(start + QPoint(10, -10), 120);
        QCOMPARE(value("width"), 2.5);
        dragTo(window_, start + QPoint(20, -20));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, start + QPoint(20, -20));
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();
        QCOMPARE(value("freq"), 1000.0);
        QCOMPARE(value("amount"), 25.0);

        // Other buttons go on to the frame (its menu).
        steps = undo()->index();
        QTest::mouseClick(window_, Qt::RightButton, Qt::NoModifier, centerOf(graph));
        QCOMPARE(undo()->index(), steps);
        QVERIFY(!graph->dragging());
    }

    void altDragAndWheelSetWidth() {
        QQuickItem* view = showErosion();
        QVERIFY(view);
        auto* graph = find<ErosionGraph>(view, QStringLiteral("erosionGraph"));
        QVERIFY(graph);

        // Alt: up and down for the Width, from where the dot is (the press changes nothing); across is
        // untouched while the mouse doesn't move across.
        int steps = undo()->index();
        const QPoint at = scenePoint(graph, graph->dot() + QPointF(30, 0));
        QTest::mousePress(window_, Qt::LeftButton, Qt::AltModifier, at);
        QCOMPARE(value("width"), 2.5);
        QCOMPARE(undo()->index(), steps);
        for (int dy = 10; dy <= 40; dy += 10)
            dragTo(window_, at - QPoint(0, dy), Qt::AltModifier);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::AltModifier, at - QPoint(0, 40));
        QVERIFY2(near(value("width"), 5.0, 0.005), qPrintable(QString::number(value("width"))));
        QCOMPARE(value("freq"), 1000.0);
        QCOMPARE(value("amount"), 25.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(undo()->text(undo()->index() - 1), QStringLiteral("Change Erosion Width"));
        undo()->undo();
        QCOMPARE(value("width"), 2.5);

        // The wheel: a notch multiplies the Width by 2^(1/4) (about 19 %), notches in a quick burst one undo step.
        steps = undo()->index();
        const QPoint middle = centerOf(graph);
        wheel(middle, 120);
        QTest::qWait(100);
        wheel(middle, 120);
        QVERIFY2(near(value("width"), 2.5 * std::exp2(0.5), 0.005), qPrintable(QString::number(value("width"))));
        QCOMPARE(undo()->index(), steps + 1);
        QTest::qWait(500);
        wheel(middle, 120);
        QVERIFY2(near(value("width"), 2.5 * std::exp2(0.75), 0.005), qPrintable(QString::number(value("width"))));
        QCOMPARE(undo()->index(), steps + 2);
        const double before = value("width");
        wheel(middle, 120, Qt::ControlModifier);  // finely: 2^(1/16)
        QVERIFY2(near(value("width"), before * std::exp2(1.0 / 16.0), 0.005),
                 qPrintable(QString::number(value("width"))));
        wheel(middle, -120 * 40);  // (held at the bottom of its range)
        QCOMPARE(value("width"), kErosionMinWidth);
        while (undo()->index() > steps)
            undo()->undo();
        QCOMPARE(value("width"), 2.5);

        // A notch during an Alt drag: the drag goes on from the new width, all of it one undo step.
        steps = undo()->index();
        const QPoint dotAt = scenePoint(graph, graph->dot());
        QTest::mousePress(window_, Qt::LeftButton, Qt::AltModifier, dotAt);
        dragTo(window_, dotAt - QPoint(0, 40), Qt::AltModifier);
        QVERIFY(near(value("width"), 5.0, 0.005));
        wheel(dotAt - QPoint(0, 40), 120, Qt::AltModifier);
        QVERIFY2(near(value("width"), 5.0 * std::exp2(0.25), 0.005), qPrintable(QString::number(value("width"))));
        dragTo(window_, dotAt - QPoint(0, 40), Qt::AltModifier);  // no jump back
        QVERIFY(near(value("width"), 5.0 * std::exp2(0.25), 0.005));
        dragTo(window_, dotAt - QPoint(0, 80), Qt::AltModifier);
        QVERIFY2(near(value("width"), 10.0, 0.005), qPrintable(QString::number(value("width"))));  // (held at 10)
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::AltModifier, dotAt - QPoint(0, 80));
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();
        QCOMPARE(value("width"), 2.5);
    }

    void wheelInTheDeviceChain() {
        // In the device view the editor sits in a DeviceChainArea, which takes Shift+wheel anywhere over the
        // chain to scroll it before the graph sees it: the fine wheel is Ctrl's, which the chain lets through,
        // as it does the plain wheel.
        QQuickItem* view = showErosion();
        QVERIFY(view);
        auto* graph = find<ErosionGraph>(view, QStringLiteral("erosionGraph"));
        QVERIFY(graph);
        QQuickItem* body = view->parentItem();  // (the host's loader, at the window's top left)
        QQuickItem* top = body->parentItem();
        auto* chain = new DeviceChainArea;  // (parented once made: it watches its window's events from then)
        chain->setParentItem(top);
        chain->setSize(top->size());
        auto* content = new QQuickItem(chain);  // a chain wider than the view, to scroll
        content->setSize(QSizeF(4 * top->width(), top->height()));
        chain->setContent(content);
        body->setParentItem(chain);
        const auto restore = qScopeGuard([&] {
            body->setParentItem(top);
            delete chain;
        });
        const QPoint middle = centerOf(graph);
        wheel(middle, 120);
        QVERIFY2(near(value("width"), 2.5 * std::exp2(0.25), 0.005), qPrintable(QString::number(value("width"))));
        const double before = value("width");
        QTest::qWait(500);  // (a new burst)
        wheel(middle, 120, Qt::ControlModifier);
        QVERIFY2(near(value("width"), before * std::exp2(1.0 / 16.0), 0.005),
                 qPrintable(QString::number(value("width"))));
        const double fine = value("width");
        wheel(middle, -120, Qt::ShiftModifier);
        QCOMPARE(value("width"), fine);  // the chain's: it scrolled
        QCOMPARE(chain->contentX(), double(DeviceChainArea::kWheelScroll));
    }

    void quietAfterTheSoundStopped() {
        // An editor shown after the sound stopped, or shown again, reads the backlog of what was eroded (the
        // `erosion` display keeps 44 s of it): only its newest values are now, so in silence it stays still.
        QQuickItem* view = showErosion(buzz(kSampleRate));  // (a second of it, then silence)
        QVERIFY(view);
        auto* graph = find<ErosionGraph>(view, QStringLiteral("erosionGraph"));
        auto* scope = find<ErosionScope>(view, QStringLiteral("erosionScope"));
        QVERIFY(graph && scope);
        set("amount", 60.0);
        engine()->renderOffline(0.0, 3 * kSampleRate);  // eroded, then two seconds of silence: not read yet
        refreshDisplays();
        QCOMPARE(graph->erosionDb(), kErosionFloorDb);
        QCOMPARE(graph->activity(), 0.0);
        QCOMPARE(scope->activity(), 0.0);

        // Hidden while it played and stopped, then shown again: the same.
        view->setVisible(false);
        engine()->renderOffline(0.0, 3 * kSampleRate);
        refreshDisplays();  // (not read: hidden)
        view->setVisible(true);
        refreshDisplays();
        QCOMPARE(graph->erosionDb(), kErosionFloorDb);
        QCOMPARE(graph->activity(), 0.0);
        QCOMPARE(scope->activity(), 0.0);

        // While it plays, it shows.
        engine()->renderOffline(0.0, kSampleRate / 2);
        refreshDisplays();
        QVERIFY2(graph->erosionDb() > -40.0, qPrintable(QString::number(graph->erosionDb())));
        QVERIFY(graph->activity() > 0.1 && scope->activity() > 0.1);
    }

    void newestValuesAtEachDisplaysRate() {
        // What the graph and the scope read the `erosion` display with (DeviceCanvas::readRecent): of what came
        // since the last read, only the newest values covering the time asked, at the display's own rate (the
        // meter's, a value per many samples; the input's, a value per sample); at least one; none if none came.
        QVERIFY(showErosion());
        set("amount", 50.0);
        struct Probe : DeviceCanvas {
            void paint(SgPainter&) override {}
        } all, recent;
        for (Probe* probe : {&all, &recent}) {
            probe->setSession(session_.get());
            probe->setTrackId(track_);
            probe->setDeviceId(device_);
        }
        int meterRate = 0;
        for (const ProcessorDisplay& display : bridge()->processorDisplays(track_, device_)) {
            if (display.id == QLatin1String("erosion"))
                meterRate = display.samplesPerValue;
        }
        QVERIFY(meterRate > 1);
        engine()->renderOffline(0.0, kSampleRate / 2);
        const std::vector<float> meter = all.readDisplay(QStringLiteral("erosion"));
        const std::vector<float> input = all.readDisplay(QStringLiteral("input"));
        const std::vector<float> newestMeter =
            recent.readRecent(QStringLiteral("erosion"), ErosionGraph::kRecentSeconds);
        const std::vector<float> newestInput = recent.readRecent(QStringLiteral("input"), 0.01);
        const auto meterCount = size_t(std::ceil(ErosionGraph::kRecentSeconds * kSampleRate / meterRate));
        QVERIFY(meter.size() > meterCount && input.size() > size_t(kSampleRate / 100));
        QVERIFY(newestMeter == std::vector<float>(meter.end() - std::ptrdiff_t(meterCount), meter.end()));
        QVERIFY(newestInput == std::vector<float>(input.end() - kSampleRate / 100, input.end()));
        engine()->renderOffline(0.0, kSampleRate / 10);
        QCOMPARE(recent.readRecent(QStringLiteral("erosion"), 0.0).size(), size_t(1));
        QVERIFY(recent.readRecent(QStringLiteral("erosion"), 1.0).empty());
    }

    void displaysReachTheEditor() {
        QQuickItem* view = showErosion();
        QVERIFY(view);
        auto* graph = find<ErosionGraph>(view, QStringLiteral("erosionGraph"));
        auto* scope = find<ErosionScope>(view, QStringLiteral("erosionScope"));
        QVERIFY(graph && scope);
        QVERIFY(!graph->spectrumLive());
        QCOMPARE(graph->activity(), 0.0);

        // The tone eroded: its spectrum, how much is eroded, the modulation, all reach the editor.
        set("amount", 50.0);
        engine()->renderOffline(0.0, kSampleRate / 2);
        QElapsedTimer sinceRisen;  // (from before the refresh that raises it: the fall counts from its tick)
        sinceRisen.start();
        refreshDisplays();
        QVERIFY(graph->spectrumLive());
        QVERIFY2(graph->erosionDb() > -40.0, qPrintable(QString::number(graph->erosionDb())));
        QVERIFY(graph->activity() > 0.1);
        QVERIFY(graph->animating());
        QCOMPARE(scope->pointCount(), ErosionScope::kRing);
        QVERIFY2(scope->spread() < 1e-6, qPrintable(QString::number(scope->spread())));  // mono: both sides alike
        QVERIFY(scope->activity() > 0.1);
        // Nothing more coming, it falls back slowly, by the time each refresh says (DeviceCanvas::tickSeconds:
        // the time since the refresh before, but at least a tick): never less than a tick's worth a refresh, and
        // never more than the time they took plus a tick each (a one-pole's fall over several steps is its fall
        // over their sum), however long a busy machine makes them.
        const double risen = graph->activity();
        for (int i = 0; i < 10; ++i)
            refreshDisplays();
        const double tick = kDisplayRefreshMs / 1000.0;
        const double took = double(sinceRisen.nsecsElapsed()) / 1e9;
        const double fallen = risen * std::pow(1.0 - easeFraction(tick, ErosionGraph::kActivityFallSeconds), 10);
        const double least = risen * (1.0 - easeFraction(took + 10 * tick, ErosionGraph::kActivityFallSeconds));
        QVERIFY2(graph->activity() <= fallen + 1e-9 && graph->activity() >= least,
                 qPrintable(QStringLiteral("%1 from %2 in %3 s").arg(graph->activity()).arg(risen).arg(took)));
        QVERIFY(graph->animating());

        // Stereo opens the modulation out: a round cloud at 100 %, about half as wide at 50 %.
        set("stereo", 100.0);
        engine()->renderOffline(0.0, kSampleRate / 2);
        refreshDisplays();
        QVERIFY2(scope->spread() >= 0.7 && scope->spread() <= 1.4, qPrintable(QString::number(scope->spread())));
        set("stereo", 50.0);
        engine()->renderOffline(0.0, kSampleRate / 2);
        refreshDisplays();
        QVERIFY2(scope->spread() >= 0.35 && scope->spread() <= 0.7, qPrintable(QString::number(scope->spread())));
        // The sine at full Stereo: its sides a quarter cycle apart, a circle.
        set("blend", 0.0);
        set("stereo", 100.0);
        engine()->renderOffline(0.0, kSampleRate / 2);
        refreshDisplays();
        QVERIFY2(std::abs(scope->spread() - 1.0) < 0.05, qPrintable(QString::number(scope->spread())));
        set("blend", 100.0);

        // At Amount 0 nothing is eroded; with nothing more coming the graph comes to rest and stops repainting.
        set("amount", 0.0);
        engine()->renderOffline(0.0, kSampleRate / 2);
        refreshDisplays();
        QCOMPARE(graph->erosionDb(), kErosionFloorDb);
        for (int i = 0; i < 300; ++i)
            refreshDisplays();
        QCOMPARE(graph->activity(), 0.0);
        QCOMPARE(scope->activity(), 0.0);
        QVERIFY(!graph->spectrumLive());
        QVERIFY(!graph->animating());
    }

    void engineHasWhatTheEditorSet() {
        QQuickItem* view = showErosion();
        QVERIFY(view);
        auto* graph = find<ErosionGraph>(view, QStringLiteral("erosionGraph"));
        QVERIFY(graph);
        const QPoint at = scenePoint(graph, QPointF(graph->xOf(3300.0), graph->yOfAmount(70.0)));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(window_, at + QPoint(12, -6));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(12, -6));
        wheel(centerOf(graph), -240);
        QVERIFY(value("freq") > 3300.0 && value("amount") > 70.0 && value("width") < 2.5);
        const auto id = bridge()->engineDeviceId(track_, device_);
        QVERIFY(id);
        for (const char* p : {"freq", "amount", "width"})
            QCOMPARE(engine()->processorParam(*id, engine()->processorParamIndex(*id, p)), float(value(p)));
    }

    void screenshot() {
        QQuickItem* view = showErosion(buzz(kSampleRate));
        QVERIFY(view);
        auto* graph = find<ErosionGraph>(view, QStringLiteral("erosionGraph"));
        QVERIFY(graph);
        QTest::qWait(50);
        save(grab(), QStringLiteral("erosion-idle.png"));  // (nothing playing: the band, the dot)

        // Band, spike and both shimmer layers in view, the buzz eroded.
        set("blend", 70.0);
        set("stereo", 60.0);
        set("amount", 45.0);
        play(8);
        QVERIFY(graph->spectrumLive() && graph->activity() > 0.5);
        QTest::qWait(50);
        save(grab(), QStringLiteral("erosion.png"));

        // A sine at full stereo: the spike, and a circle in the scope.
        set("blend", 0.0);
        set("stereo", 100.0);
        set("freq", 400.0);
        set("amount", 70.0);
        play(8);
        QTest::qWait(50);
        save(grab(), QStringLiteral("erosion-sine-stereo.png"));
    }
};

QTEST_MAIN(TestUiDeviceEditorsErosion)
#include "test_ui_device_editors_erosion.moc"
