// The Chorus-Ensemble's editor (ui/qml/devices/editors/ChorusEditor.qml,
// ChorusGraph): loaded as the device view loads it, over a real engine; its
// mode tabs, strip and knobs, each bound to its parameter and undoable; its
// display's drags (one undo step each); what the engine's displays bring it
// (the voices where the engine's delays are, the glow, freezing and resting
// with nothing to show); and what the engine has after. With
// SUBSTATION_UI_SCREENSHOTS set to a folder, it is saved there as PNGs.

#include <QElapsedTimer>
#include <QFont>
#include <QFontMetricsF>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include <cmath>

#include "EditorHarness.h"
#include "audio/ChorusVoices.h"
#include "audio/EngineBridge.h"
#include "controls/KnobItem.h"
#include "controls/ValueBoxItem.h"
#include "devices/ChorusGraph.h"
#include "devices/DeviceParam.h"
#include "editor/ProjectEditor.h"
#include "model/Automation.h"
#include "model/ParamSpec.h"

using namespace sub::app;
using namespace sub::ui;
using sub::app::test::kSampleRate;

namespace {

// QML's warnings while a test runs (a binding to something missing, a type error): each test fails on any.
QStringList qmlWarnings;
QtMessageHandler previousHandler = nullptr;

void recordWarnings(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (type == QtWarningMsg && (message.contains(QStringLiteral(".qml")) || message.contains(QStringLiteral("QML"))))
        qmlWarnings << message;
    if (previousHandler)
        previousHandler(type, context, message);
}

}  // namespace

class TestUiDeviceEditorsChorus : public QObject, public sub::app::test::EditorHarness {
    Q_OBJECT

    QString track_, device_;

    // Every control: its object name and the parameter it is bound to.
    static constexpr struct {
        const char* name;
        const char* param;
    } kControls[] = {{"modeChorus", "mode"}, {"modeEnsemble", "mode"}, {"modeVibrato", "mode"}, {"taps1", "taps"},
                     {"taps2", "taps"}, {"time", "time"}, {"hp", "hp"}, {"hpFreq", "hp_freq"},
                     {"fbInvert", "fb_invert"}, {"rate", "rate"}, {"amount", "amount"}, {"feedback", "feedback"},
                     {"warmth", "warmth"}, {"width", "width"}, {"offset", "offset"}, {"shape", "shape"},
                     {"output", "output"}, {"mix", "mix"}};

    // A track playing a 220 Hz tone (at -6 dB) through a Chorus-Ensemble, its editor shown.
    QQuickItem* showChorus() {
        track_ = audioTrackWith(tone(220.0, kSampleRate), QStringLiteral("tone"), 1.0);
        if (track_.isEmpty())
            return nullptr;
        device_ = editor()->addDevice(track_, QStringLiteral("chorus"));
        return show(QStringLiteral("chorus"), track_, device_);
    }

    double value(const char* id) { return param(track_, device_, QString::fromLatin1(id)); }
    void set(const char* id, double v) { editor()->setDeviceParam(track_, device_, QString::fromLatin1(id), v); }

    // An EditorKnob's dial: the cell's ParamKnob's KnobItem.
    KnobItem* knob(QQuickItem* view, const char* id) {
        QQuickItem* cell = find(view, QString::fromLatin1(id));
        auto* paramKnob = cell ? qvariant_cast<QQuickItem*>(cell->property("knob")) : nullptr;
        return paramKnob ? qvariant_cast<KnobItem*>(paramKnob->property("knob")) : nullptr;
    }

    // A ParamButton's (or ParamChoice's) button.
    QQuickItem* button(QQuickItem* view, const char* name) {
        QQuickItem* control = find(view, QString::fromLatin1(name));
        return control ? qvariant_cast<QQuickItem*>(control->property("button")) : nullptr;
    }
    bool lit(QQuickItem* view, const char* name) {
        return find(view, QString::fromLatin1(name))->property("lit").toBool();
    }

    // The parameter a control is bound to.
    sub::ui::DeviceParam* boundParam(QQuickItem* view, const char* name) {
        QQuickItem* control = find(view, QString::fromLatin1(name));
        return control ? qvariant_cast<sub::ui::DeviceParam*>(control->property("param")) : nullptr;
    }

    void click(QQuickItem* view, const char* name) {
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(button(view, name)));
    }

    // Shown to the eye: visible, and not faded out.
    static bool shown(QQuickItem* item) {
        if (!item->isVisible())
            return false;
        for (QQuickItem* at = item; at; at = at->parentItem()) {
            if (at->opacity() < 0.99)
                return false;
        }
        return true;
    }

    // Every item under `root`, recursively.
    static void collect(QQuickItem* root, QList<QQuickItem*>& out) {
        for (QQuickItem* child : root->childItems()) {
            out << child;
            collect(child, out);
        }
    }

    // The editor's layout as the engine's: the indices the graph is drawing.
    ChorusLayout layoutNow() { return {int(value("mode")), int(value("taps")), int(value("time"))}; }

    // Ticks the displays' clock `count` times (each counts a clock period).
    void tick(int count) {
        for (int i = 0; i < count; ++i)
            refreshDisplays();
    }

    // As playing does, a piece at a time (each render starts afresh: the engine resets the device), the
    // displays read after each. `last` frames end it (where the LFO is then).
    void play(int pieces, int frames = 4096, int last = 0) {
        for (int i = 0; i < pieces; ++i) {
            engine()->renderOffline(0.0, frames);
            refreshDisplays();
        }
        if (last > 0) {
            engine()->renderOffline(0.0, last);
            refreshDisplays();
        }
    }

    // The engine's value of a parameter.
    float engineParam(const char* id) {
        const auto engineId = bridge()->engineDeviceId(track_, device_);
        return engineId ? engine_->processorParam(*engineId, engine_->processorParamIndex(*engineId, id)) : -999.f;
    }

private Q_SLOTS:
    void initTestCase() {
        if (!haveDisplay())
            QSKIP("needs a display: the offscreen platform renders Qt Quick in software, without this geometry");
        previousHandler = qInstallMessageHandler(recordWarnings);
        startHost();
    }

    void cleanupTestCase() {
        stopHost();
        qInstallMessageHandler(previousHandler);
    }

    void init() {
        clearHost();
        qmlWarnings.clear();
    }

    void cleanup() { QVERIFY2(qmlWarnings.isEmpty(), qPrintable(qmlWarnings.join(QLatin1Char('\n')))); }

    void registry() {
        QVariant url;
        QMetaObject::invokeMethod(root_.get(), "editorFor", Q_RETURN_ARG(QVariant, url),
                                  Q_ARG(QVariant, QStringLiteral("chorus")));
        QVERIFY(url.toString().endsWith(QStringLiteral("ChorusEditor.qml")));
        QCOMPARE(formatValue(90.0, QStringLiteral("°")), QStringLiteral("90°"));
    }

    void fitsAndBinds() {
        QQuickItem* view = showChorus();
        QVERIFY(view);
        // It fits the device view's body, as wide as its parts.
        QVERIFY2(view->implicitHeight() <= bodyHeight(),
                 qPrintable(QStringLiteral("%1 > %2").arg(view->implicitHeight()).arg(bodyHeight())));
        QCOMPARE(view->implicitWidth(), 534.0);

        // Every control is there, bound to its parameter, with a tooltip.
        for (const auto& [name, id] : kControls) {
            QQuickItem* control = find(view, QString::fromLatin1(name));
            QVERIFY2(control, name);
            sub::ui::DeviceParam* p = boundParam(view, name);
            QVERIFY2(p && p->valid() && p->paramId() == QString::fromLatin1(id), name);
            const QVariant tooltip = control->property("tooltip");
            QVariant buttonTooltip;
            if (QQuickItem* inner = button(view, name))
                buttonTooltip = inner->property("tooltip");
            QVERIFY2(!tooltip.toString().isEmpty() || !buttonTooltip.toString().isEmpty(), name);
        }
        auto* graph = find<ChorusGraph>(view, QStringLiteral("chorusGraph"));
        QVERIFY(graph);
        QVERIFY(graph->height() >= ChorusGraph::kMinimumHeight);

        // The margins: the tabs 6 px below the top, the strip and the knobs' bottom row 6 px above the bottom,
        // the display 8 px in from the left, the last column 8 px from the right.
        auto rectOf = [&](QQuickItem* item) {
            return item->mapRectToItem(view, QRectF(0, 0, item->width(), item->height()));
        };
        QCOMPARE(rectOf(find(view, QStringLiteral("modeChorus"))).topLeft(), QPointF(8, 6));
        QCOMPARE(rectOf(find(view, QStringLiteral("rate"))).top(), 6.0);
        QVERIFY(std::abs(rectOf(find(view, QStringLiteral("mix"))).bottom() - (view->height() - 6)) < 0.5);
        QVERIFY(std::abs(rectOf(find(view, QStringLiteral("hpFreq"))).bottom() - (view->height() - 6)) < 0.5);
        QVERIFY(std::abs(rectOf(find(view, QStringLiteral("modeVibrato"))).right() - rectOf(graph).right()) < 0.5);
        QCOMPARE(rectOf(graph).left(), 8.0);
        QCOMPARE(rectOf(graph).width(), 234.0);

        // Nothing overflows the body, no text is cut short, and no two controls shown overlap.
        QList<QQuickItem*> items;
        collect(view, items);
        const QRectF body(0, 0, view->width(), view->height());
        for (QQuickItem* item : items) {
            if (!shown(item) || item->width() <= 0 || item->height() <= 0)
                continue;
            const QRectF rect = rectOf(item);
            QVERIFY2(body.adjusted(-0.5, -0.5, 0.5, 0.5).contains(rect),
                     qPrintable(QStringLiteral("%1 (%2) at %3,%4 %5x%6")
                                    .arg(item->objectName(), QString::fromLatin1(item->metaObject()->className()))
                                    .arg(rect.x())
                                    .arg(rect.y())
                                    .arg(rect.width())
                                    .arg(rect.height())));
            if (item->inherits("QQuickText")) {  // (cut short, or spilling out of its box)
                const QString text = item->property("text").toString();
                QVERIFY2(!item->property("truncated").toBool(), qPrintable(text));
                QVERIFY2(item->property("contentWidth").toDouble() <= item->width() + 0.5, qPrintable(text));
            }
        }
        QList<QPair<QString, QRectF>> placed;
        for (const auto& [name, id] : kControls) {
            QQuickItem* control = find(view, QString::fromLatin1(name));
            if (shown(control))
                placed.append({QString::fromLatin1(name), rectOf(control)});
        }
        placed.append({QStringLiteral("chorusGraph"), rectOf(graph)});
        placed.append({QStringLiteral("tapsCaption"), rectOf(find(view, QStringLiteral("tapsCaption")))});
        for (qsizetype i = 0; i < placed.size(); ++i) {
            for (qsizetype j = i + 1; j < placed.size(); ++j) {
                const QRectF overlap = placed[i].second.intersected(placed[j].second);
                QVERIFY2(overlap.width() < 0.01 || overlap.height() < 0.01,
                         qPrintable(placed[i].first + QStringLiteral(" over ") + placed[j].first));
            }
        }

        // The knobs read their parameters' defaults; the Rate and the high-pass turn in log.
        const struct {
            const char* id;
            double value;
        } defaults[] = {{"rate", 0.8},   {"amount", 50.0}, {"feedback", 0.0}, {"warmth", 0.0},
                        {"width", 100.0}, {"output", 0.0}, {"mix", 50.0}};
        for (const auto& [id, expected] : defaults) {
            KnobItem* dial = knob(view, id);
            QVERIFY2(dial, id);
            QVERIFY2(std::abs(dial->value() - expected) < 1e-6, id);
        }
        QVERIFY(knob(view, "rate")->logScale() && !knob(view, "amount")->logScale());
        auto* hpFreq = qvariant_cast<ValueBoxItem*>(find(view, QStringLiteral("hpFreq"))->property("box"));
        QVERIFY(hpFreq && hpFreq->logScale());
        QCOMPARE(hpFreq->text(), QStringLiteral("100 Hz"));
        // Wide enough for its widest values and the automation dot (at 6 px, 2.5 px round: the text centred
        // clear of it), and Time for its longest choice and its arrow.
        const QFontMetricsF boxFont(hpFreq->property("font").value<QFont>());
        for (const double hz : {20.0, 999.0, 1000.0, 1500.0, 2000.0}) {
            const QString text = formatValue(hz, QStringLiteral("Hz"));
            QVERIFY2(boxFont.horizontalAdvance(text) + 16 <= hpFreq->width() + 0.5, qPrintable(text));
        }
        for (int choice = 0; choice < 6; ++choice) {
            set("time", choice);
            QQuickItem* face = button(view, "time");
            QVERIFY2(face->implicitWidth() <= face->width() + 0.5, qPrintable(face->property("text").toString()));
        }
        set("time", 0.0);

        // Chorus mode: Taps, Time and Width shown, not Offset or Shape; Ø enabled; the tab lit.
        QVERIFY(lit(view, "modeChorus") && !lit(view, "modeEnsemble") && !lit(view, "modeVibrato"));
        QVERIFY(lit(view, "taps2") && !lit(view, "taps1"));
        QVERIFY(shown(find(view, QStringLiteral("width"))) && shown(find(view, QStringLiteral("taps1"))) &&
                shown(find(view, QStringLiteral("time"))));
        QVERIFY(!find(view, QStringLiteral("offset"))->isVisible());
        QVERIFY(!find(view, QStringLiteral("shape"))->isVisible());
        QVERIFY(!find(view, QStringLiteral("voicesText"))->isVisible());
        QVERIFY(find(view, QStringLiteral("fbInvert"))->isEnabled());
        QCOMPARE(button(view, "time")->property("text").toString(), QStringLiteral("Auto"));

        // The graph: two taps a side, the axis Auto's range (1.5 to 11.5 ms), the readout the detune.
        QCOMPARE(graph->voiceCount(), 4);
        QCOMPARE(graph->axisLowMs(), 1.5);
        QCOMPARE(graph->axisHighMs(), 11.5);
        QCOMPARE(graph->centreMs(), 4.0);
        QCOMPARE(graph->readout(), QStringLiteral("±22 ct"));
        QVERIFY(std::abs(graph->peakDetuneCents() / chorusPeakDetuneCents({0, 1, 0}, 0.8, 50.0, 0.0) - 1.0) < 1e-6);
        const QRectF plot = graph->plot();
        QCOMPARE(graph->xOfCycles(0.0), plot.right() - ChorusGraph::kNowInset);
        QCOMPARE(graph->xOfCycles(graph->windowCycles()), plot.left());
        QVERIFY(std::abs(graph->yOf(11.5) - (plot.top() + plot.height() * 0.12 / 1.24)) < 1e-9);
        QVERIFY(std::abs(graph->yOf(1.5) - (plot.bottom() - plot.height() * 0.12 / 1.24)) < 1e-9);
    }

    void knobsUndoable() {
        QQuickItem* view = showChorus();
        QVERIFY(view);
        auto* graph = find<ChorusGraph>(view, QStringLiteral("chorusGraph"));
        QVERIFY(graph);

        // An edit: the knob follows it, the graph eases to it (the swing; the axis is the layout's), undo too.
        set("amount", 80.0);
        QCOMPARE(knob(view, "amount")->value(), 80.0);
        tick(20);
        const ChorusLayout layout = layoutNow();
        QVERIFY2(std::abs(graph->swingMs() / chorusSwingMs(layout, 80.0) - 1.0) < 0.01,
                 qPrintable(QString::number(graph->swingMs())));
        QCOMPARE(graph->axisLowMs(), 1.5);
        QCOMPARE(graph->axisHighMs(), 11.5);
        undo()->undo();
        QCOMPARE(knob(view, "amount")->value(), 50.0);
        tick(20);
        QVERIFY(std::abs(graph->swingMs() / chorusSwingMs(layout, 50.0) - 1.0) < 0.01);

        // Every knob: an edit and its undo shown, and a drag up one undo step (Offset and Shape in Vibrato).
        const struct {
            const char* id;
            double other;
        } knobs[] = {{"rate", 3.0},  {"amount", 70.0}, {"feedback", 40.0}, {"warmth", 30.0}, {"width", 150.0},
                     {"output", -6.0}, {"mix", 80.0}, {"offset", 90.0},   {"shape", 50.0}};
        for (const auto& [id, other] : knobs) {
            if (QByteArray(id) == "offset") {
                click(view, "modeVibrato");
                QTRY_VERIFY(shown(find(view, QStringLiteral("offset"))));
            }
            KnobItem* dial = knob(view, id);
            QVERIFY2(dial, id);
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
            QVERIFY2(value(id) > before, id);
            QCOMPARE(undo()->index(), steps + 1);
            QCOMPARE(float(engineParam(id)), float(value(id)));  // the engine has it
            undo()->undo();
            QCOMPARE(value(id), before);
        }
    }

    void modes() {
        QQuickItem* view = showChorus();
        QVERIFY(view);
        auto* graph = find<ChorusGraph>(view, QStringLiteral("chorusGraph"));
        QVERIFY(graph);
        const int steps = undo()->index();

        // Ensemble: three voices a side, the strip naming them instead of Taps and Time.
        click(view, "modeEnsemble");
        QCOMPARE(value("mode"), 1.0);
        QVERIFY(lit(view, "modeEnsemble") && !lit(view, "modeChorus"));
        QCOMPARE(graph->voiceCount(), 6);
        QVERIFY(graph->layoutFade() < 1.0);  // the new voices fade in, the old out
        QTRY_VERIFY(!find(view, QStringLiteral("taps1"))->isVisible());
        QTRY_VERIFY(shown(find(view, QStringLiteral("voicesText"))));
        QCOMPARE(find(view, QStringLiteral("voicesText"))->property("text").toString(),
                 QStringLiteral("3 voices a side"));
        QCOMPARE(undo()->index(), steps + 1);
        tick(40);
        QCOMPARE(graph->layoutFade(), 1.0);
        QCOMPARE(graph->axisLowMs(), 2.5);
        QCOMPARE(graph->axisHighMs(), 12.5);

        // Vibrato: one voice a side; Offset and Shape instead of Width; Feedback and Ø dimmed (no feedback in
        // Vibrato) but still settable. The sets cross-fade one after the other, never both shown at once (their
        // texts would overlap).
        QQuickItem* widthSet = find(view, QStringLiteral("width"))->parentItem();
        QQuickItem* vibratoSet = find(view, QStringLiteral("offset"))->parentItem();
        QQuickItem* tapsSet = find(view, QStringLiteral("taps1"))->parentItem();
        QQuickItem* voicesText = find(view, QStringLiteral("voicesText"));
        click(view, "modeVibrato");
        QElapsedTimer fading;
        fading.start();
        bool sawBoth = false, sawFading = false;
        while (fading.elapsed() < 300) {
            sawBoth |= widthSet->opacity() > 0.0 && vibratoSet->opacity() > 0.0;
            for (QQuickItem* set : {widthSet, vibratoSet})
                sawFading |= set->opacity() > 0.0 && set->opacity() < 1.0;
            QTest::qWait(5);
        }
        QVERIFY(!sawBoth);
        QVERIFY(sawFading);  // (it did fade)
        QCOMPARE(value("mode"), 2.0);
        QCOMPARE(graph->voiceCount(), 2);
        QTRY_VERIFY(shown(find(view, QStringLiteral("offset"))) && shown(find(view, QStringLiteral("shape"))));
        QTRY_VERIFY(!find(view, QStringLiteral("width"))->isVisible());
        QCOMPARE(find(view, QStringLiteral("voicesText"))->property("text").toString(),
                 QStringLiteral("1 voice a side"));
        QTRY_COMPARE(find(view, QStringLiteral("fbInvert"))->opacity(), 0.55);
        QTRY_COMPARE(find(view, QStringLiteral("feedback"))->opacity(), 0.55);
        QVERIFY(find(view, QStringLiteral("fbInvert"))->isEnabled());
        QVERIFY(find(view, QStringLiteral("feedback"))->isEnabled());
        QCOMPARE(undo()->index(), steps + 2);
        click(view, "fbInvert");
        QCOMPARE(value("fb_invert"), 1.0);
        QCOMPARE(undo()->index(), steps + 3);
        undo()->undo();
        QCOMPARE(value("fb_invert"), 0.0);
        tick(40);
        QCOMPARE(graph->axisLowMs(), 0.5);
        QCOMPARE(graph->axisHighMs(), 11.5);

        // Back to Chorus: the text keeps naming Vibrato's voices while it fades out, and Taps and Time fade in
        // only once it has.
        undo()->undo();
        QCOMPARE(value("mode"), 1.0);
        undo()->undo();
        QCOMPARE(value("mode"), 0.0);
        fading.restart();
        sawBoth = false;
        while (fading.elapsed() < 300) {
            sawBoth |= tapsSet->opacity() > 0.0 && voicesText->opacity() > 0.0;
            QTest::qWait(5);
        }
        QVERIFY(!sawBoth);
        QCOMPARE(graph->voiceCount(), 4);
        QVERIFY(lit(view, "modeChorus"));
        QTRY_VERIFY(shown(find(view, QStringLiteral("taps1"))) && shown(find(view, QStringLiteral("width"))));
        QTRY_COMPARE(find(view, QStringLiteral("fbInvert"))->opacity(), 1.0);
        QTRY_COMPARE(find(view, QStringLiteral("feedback"))->opacity(), 1.0);
        QCOMPARE(float(engineParam("mode")), 0.f);

        // One opened in Vibrato and switched straight to Chorus: the text names Vibrato's voice while it fades
        // out. The counts are the engine's (chorusVoices).
        device_ = editor()->addDevice(track_, QStringLiteral("chorus"));
        set("mode", 2.0);
        view = show(QStringLiteral("chorus"), track_, device_);
        QVERIFY(view);
        voicesText = find(view, QStringLiteral("voicesText"));
        QTRY_VERIFY(shown(voicesText));
        QCOMPARE(voicesText->property("text").toString(), QStringLiteral("1 voice a side"));
        QCOMPARE(chorusVoices({2, 1, 0}), 1);
        set("mode", 0.0);
        fading.restart();
        bool sawText = false;
        while (fading.elapsed() < 300) {
            if (voicesText->isVisible() && voicesText->opacity() > 0.0) {
                sawText = true;
                QCOMPARE(voicesText->property("text").toString(), QStringLiteral("1 voice a side"));
            }
            QTest::qWait(5);
        }
        QVERIFY(sawText);  // (it did fade)
    }

    void tapsTimeHpInvert() {
        QQuickItem* view = showChorus();
        QVERIFY(view);
        auto* graph = find<ChorusGraph>(view, QStringLiteral("chorusGraph"));
        QVERIFY(graph);
        const int steps = undo()->count();

        click(view, "taps1");
        QCOMPARE(value("taps"), 0.0);
        QVERIFY(lit(view, "taps1") && !lit(view, "taps2"));
        QCOMPARE(graph->voiceCount(), 2);
        QCOMPARE(undo()->count(), steps + 1);

        // Time, from its list: a fixed delay, the axis round it.
        QQuickItem* time = find(view, QStringLiteral("time"));
        QMetaObject::invokeMethod(time, "choose", Q_ARG(QVariant, 5));
        QCOMPARE(value("time"), 5.0);
        QCOMPARE(button(view, "time")->property("text").toString(), QStringLiteral("50 ms"));
        QCOMPARE(graph->centreMs(), 50.0);
        QCOMPARE(undo()->count(), steps + 2);
        tick(40);
        QCOMPARE(graph->axisLowMs(), 46.0);
        QCOMPARE(graph->axisHighMs(), 54.0);
        // Its menu lists the times.
        click(view, "time");
        auto* menu = qvariant_cast<QObject*>(time->property("menu"));
        QVERIFY(menu);
        QTRY_VERIFY(menu->property("opened").toBool());
        QCOMPARE(menu->property("count").toInt(), 6);
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("visible").toBool());

        click(view, "hp");
        QCOMPARE(value("hp"), 1.0);
        QVERIFY(lit(view, "hp"));
        QTRY_COMPARE(find(view, QStringLiteral("hpFreq"))->opacity(), 1.0);
        QCOMPARE(undo()->count(), steps + 3);

        click(view, "fbInvert");
        QCOMPARE(value("fb_invert"), 1.0);
        QVERIFY(lit(view, "fbInvert"));
        QCOMPARE(undo()->count(), steps + 4);

        // The high-pass's frequency: typed, and dragged in log.
        auto* hpFreq = qvariant_cast<ValueBoxItem*>(find(view, QStringLiteral("hpFreq"))->property("box"));
        QVERIFY(hpFreq->applyTyped(QStringLiteral("1k")));
        QCOMPARE(value("hp_freq"), 1000.0);
        QCOMPARE(hpFreq->text(), QStringLiteral("1.00 kHz"));
        QCOMPARE(undo()->count(), steps + 5);

        QCOMPARE(float(engineParam("taps")), 0.f);
        QCOMPARE(float(engineParam("time")), 5.f);
        QCOMPARE(float(engineParam("hp")), 1.f);
        QCOMPARE(float(engineParam("fb_invert")), 1.f);
        QCOMPARE(float(engineParam("hp_freq")), 1000.f);

        while (undo()->index() > steps)
            undo()->undo();
        QCOMPARE(value("taps"), 1.0);
        QCOMPARE(value("time"), 0.0);
        QCOMPARE(value("hp"), 0.0);
        QCOMPARE(value("fb_invert"), 0.0);
        QCOMPARE(value("hp_freq"), 100.0);
        QCOMPARE(graph->voiceCount(), 4);
        QCOMPARE(graph->centreMs(), 4.0);
        QTRY_COMPARE(find(view, QStringLiteral("hpFreq"))->opacity(), 0.55);
    }

    void graphDrag() {
        QQuickItem* view = showChorus();
        QVERIFY(view);
        auto* graph = find<ChorusGraph>(view, QStringLiteral("chorusGraph"));
        QVERIFY(graph);
        const int steps = undo()->index();

        // Up for the Rate: twice it for kRatePixels; the Amount stays.
        const QPoint at = centerOf(graph);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(undo()->index(), steps);  // (a press alone changes nothing)
        dragTo(at - QPoint(0, 20));
        dragTo(at - QPoint(0, 40));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 40));
        QVERIFY2(std::abs(value("rate") / 1.6 - 1.0) < 0.02, qPrintable(QString::number(value("rate"))));
        QCOMPARE(value("amount"), 50.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(undo()->count(), undo()->index());
        QCOMPARE(knob(view, "rate")->value(), value("rate"));

        // Across for the Amount: all of it for kAmountPixels.
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        for (int dx = 15; dx <= 45; dx += 15)
            dragTo(at + QPoint(dx, 0));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(45, 0));
        QVERIFY2(std::abs(value("amount") - 80.0) < 0.5, qPrintable(QString::number(value("amount"))));
        QVERIFY(std::abs(value("rate") / 1.6 - 1.0) < 0.02);
        QCOMPARE(undo()->index(), steps + 2);
        QCOMPARE(undo()->count(), undo()->index());

        // Shift: a quarter as far (80 px: half an octave).
        QTest::mousePress(window_, Qt::LeftButton, Qt::ShiftModifier, at);
        dragTo(at - QPoint(0, 80), Qt::ShiftModifier);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::ShiftModifier, at - QPoint(0, 80));
        QVERIFY2(std::abs(value("rate") / (1.6 * std::sqrt(2.0)) - 1.0) < 0.02,
                 qPrintable(QString::number(value("rate"))));
        QCOMPARE(undo()->index(), steps + 3);

        // Shift pressed mid-drag: finer from there on, without a jump.
        const double before = value("rate");
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at - QPoint(0, 20));
        QVERIFY(std::abs(value("rate") / (before * std::sqrt(2.0)) - 1.0) < 0.01);
        dragTo(at - QPoint(0, 60), Qt::ShiftModifier);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::ShiftModifier, at - QPoint(0, 60));
        QVERIFY2(std::abs(value("rate") / (before * std::pow(2.0, 0.75)) - 1.0) < 0.01,
                 qPrintable(QString::number(value("rate"))));
        QCOMPARE(undo()->index(), steps + 4);

        // Held to the Rate's range.
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at - QPoint(0, 400));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 400));
        QCOMPARE(value("rate"), 15.0);

        // The engine has what the drags set.
        QCOMPARE(engineParam("rate"), 15.f);
        QCOMPARE(engineParam("amount"), float(value("amount")));

        // Other buttons go on to the frame (its menu).
        const int index = undo()->index();
        QTest::mousePress(window_, Qt::RightButton, Qt::NoModifier, at);
        QTest::mouseRelease(window_, Qt::RightButton, Qt::NoModifier, at);
        QCOMPARE(undo()->index(), index);
        while (undo()->index() > steps)
            undo()->undo();
        QCOMPARE(float(value("rate")), 0.8f);
        QCOMPARE(value("amount"), 50.0);
    }

    void graphDragSetsOne() {
        QQuickItem* view = showChorus();
        QVERIFY(view);
        auto* graph = find<ChorusGraph>(view, QStringLiteral("chorusGraph"));
        QVERIFY(graph);
        const int steps = undo()->index();
        const QPoint at = centerOf(graph);

        // Up with a hand's drift sideways: the Rate, and only it (the first few pixels pick which).
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at + QPoint(1, -2));
        QCOMPARE(undo()->index(), steps);  // (not yet: too short to tell)
        dragTo(at + QPoint(2, -20));
        dragTo(at + QPoint(6, -40));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(6, -40));
        QVERIFY2(std::abs(value("rate") / 1.6 - 1.0) < 0.02, qPrintable(QString::number(value("rate"))));
        QCOMPARE(value("amount"), 50.0);
        QCOMPARE(undo()->index(), steps + 1);

        // Across with drift up and down: the Amount, and only it.
        const double rate = value("rate");
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at + QPoint(20, -2));
        dragTo(at + QPoint(45, 8));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(45, 8));
        QVERIFY2(std::abs(value("amount") - 80.0) < 0.5, qPrintable(QString::number(value("amount"))));
        QCOMPARE(value("rate"), rate);
        QCOMPARE(undo()->index(), steps + 2);

        // The Amount automated: a drag for the Rate leaves its envelope playing (not overridden).
        const QString amountKey = sub::app::automation::deviceKey(device_, QStringLiteral("amount"));
        editor()->setEnvelope(track_, amountKey, {{0.0, 0.2, 0.0}, {16.0, 0.9, 0.0}});
        QVERIFY(bridge()->isAutomated(track_, amountKey));
        QVERIFY(!bridge()->isOverridden(track_, amountKey));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at - QPoint(0, 20));
        dragTo(at - QPoint(0, 40));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 40));
        QVERIFY(value("rate") > rate);
        QVERIFY(!bridge()->isOverridden(track_, amountKey));
    }

    void displaysReachGraph() {
        QQuickItem* view = showChorus();
        QVERIFY(view);
        auto* graph = find<ChorusGraph>(view, QStringLiteral("chorusGraph"));
        QVERIFY(graph);
        set("taps", 0.0);  // one tap: the wet is the tone at its level
        set("rate", 2.0);
        QCOMPARE(engineParam("rate"), 2.f);
        tick(20);
        QVERIFY(graph->frozen());  // nothing rendered yet
        QVERIFY(graph->resting());

        // The phase is the engine's newest (the first values snap): 187 values of 128 samples, from a reset.
        engine()->renderOffline(0.0, 24000);
        QElapsedTimer ticking;  // (from this tick: the time the ticks below count lies within what it reads)
        ticking.start();
        refreshDisplays();
        const double expected = std::fmod(2.0 * 23936.0 / 48000.0, 1.0);
        QVERIFY2(std::abs(graph->phase() - expected) < 1e-6, qPrintable(QString::number(graph->phase())));
        QVERIFY(!graph->frozen());
        const ChorusLayout layout = layoutNow();
        QCOMPARE(graph->voiceCount(), 2);
        for (int channel = 0; channel < 2; ++channel) {
            const double ms =
                chorusDelayMs(layout, 50.0, 0.0, graph->phase() + chorusVoicePhase(layout, channel, 0, 0.0));
            QVERIFY(std::abs(graph->voiceDelayMs(channel, 0) - ms) < 1e-9);
            QCOMPARE(graph->voiceDot(channel, 0), QPointF(graph->xOfCycles(0.0), graph->yOf(ms)));
        }
        // The two sides opposite: the right's dot mirrors the left's about the centre.
        QVERIFY(std::abs(graph->voiceDelayMs(0, 0) + graph->voiceDelayMs(1, 0) - 2 * graph->centreMs()) < 1e-9);

        // The glow: the tone at -6 dB lights it; with nothing more rendered it freezes and goes out.
        QSignalSpy animated(graph, &ChorusGraph::animated);
        const double phase = graph->phase();
        tick(10);
        const double seconds = double(ticking.nsecsElapsed()) / 1e9;
        QVERIFY2(graph->glow() > 0.9, qPrintable(QString::number(graph->glow())));
        QVERIFY(!graph->frozen() && !graph->resting());
        QCOMPARE(animated.count(), 10);  // moving: every tick repaints
        // The estimate runs on at the rate: 2 Hz over the ticks, each counting at least 16 ms and at most the
        // real time since the tick before more (a loaded machine).
        const double advance = std::fmod(graph->phase() - phase + 1.0, 1.0);
        QVERIFY2(advance >= 10 * 0.016 * 2.0 - 1e-6 && advance <= 2.0 * (10 * 0.016 + seconds) + 1e-6,
                 qPrintable(QString::number(advance)));
        const double glow = graph->glow();
        tick(30);
        QVERIFY(graph->frozen());
        QVERIFY(graph->glow() < glow);
        const double frozenPhase = graph->phase();
        tick(40);
        QCOMPARE(graph->phase(), frozenPhase);  // stopped
        QCOMPARE(graph->glow(), 0.0);
        // Settled and frozen: nothing more to draw.
        animated.clear();
        tick(10);
        QCOMPARE(animated.count(), 0);

        // Silence passing: the traces rest (no repaints) though the device runs.
        auto silence = [&](int pieces) {
            for (int i = 0; i < pieces; ++i) {
                engine()->renderOffline(8.0, 4096);  // (after the tone's clip)
                refreshDisplays();
                tick(4);
            }
        };
        silence(1);
        QVERIFY(!graph->frozen());
        QVERIFY(graph->resting());
        // (values after a gap: the phase drawn is the engine's newest, resting or not; 32 values from a reset)
        QVERIFY2(std::abs(graph->phase() - std::fmod(2.0 * 4096.0 / 48000.0, 1.0)) < 1e-6,
                 qPrintable(QString::number(graph->phase())));
        silence(20);  // (the dimming eases away)
        animated.clear();
        silence(3);
        QVERIFY(!graph->frozen() && graph->resting());
        QCOMPARE(animated.count(), 0);
        QCOMPARE(graph->glow(), 0.0);
        // Resting, the traces hold where they are though the engine's phase runs on (as live: each piece a
        // tick longer than the last), and a parameter changing eases them in place instead of scrolling them
        // on by all the phase gone meanwhile.
        int length = 4096;
        auto live = [&](int pieces) {
            for (int i = 0; i < pieces; ++i) {
                engine()->renderOffline(8.0, length);
                length += 768;
                refreshDisplays();
            }
        };
        live(3);
        const double rested = graph->phase();
        live(30);
        QVERIFY(!graph->frozen() && graph->resting());
        QCOMPARE(animated.count(), 0);
        QCOMPARE(graph->phase(), rested);
        set("amount", 90.0);
        live(1);
        QVERIFY(animated.count() > 0);
        QCOMPARE(graph->phase(), rested);

        // The device off: frozen however much renders.
        engine()->renderOffline(0.0, 8192);
        refreshDisplays();
        QVERIFY(!graph->frozen());
        editor()->setDeviceEnabled(track_, device_, false);
        engine()->renderOffline(0.0, 8192);
        refreshDisplays();
        QVERIFY(graph->frozen());
        editor()->setDeviceEnabled(track_, device_, true);
    }

    // The level the glow shows is the sound's now: an editor opened (or shown again) after a loud part, the
    // device silent since, doesn't light up from the display's history.
    void glowShowsNow() {
        const QString name = QStringLiteral("long tone");
        track_ = audioTrackWith(tone(220.0, 4 * kSampleRate), name, 4.0);
        QVERIFY(!track_.isEmpty());
        device_ = editor()->addDevice(track_, QStringLiteral("chorus"));
        const QString path = dir_.path(name + QStringLiteral(".wav"));
        QVERIFY(QTest::qWaitFor([&] { return bridge()->source(path) != nullptr; }));  // (the clip's audio loaded)
        engine()->renderOffline(0.0, 2 * 48000);  // loud
        engine()->renderOffline(8.0, 3 * 48000);  // then silent (after the clip)
        QQuickItem* view = show(QStringLiteral("chorus"), track_, device_);
        QVERIFY(view);
        auto* graph = find<ChorusGraph>(view, QStringLiteral("chorusGraph"));
        QVERIFY(graph);
        for (int i = 0; i < 30; ++i) {
            engine()->renderOffline(8.0, 768);
            refreshDisplays();
            QVERIFY2(graph->glow() < 0.05, qPrintable(QString::number(graph->glow())));
        }
        QVERIFY(graph->resting() && !graph->frozen());

        // Hidden through a loud part, silent again, then shown.
        view->setVisible(false);
        engine()->renderOffline(0.0, 2 * 48000);
        engine()->renderOffline(8.0, 3 * 48000);
        view->setVisible(true);
        engine()->renderOffline(8.0, 768);
        refreshDisplays();
        QVERIFY2(graph->glow() < 0.05, qPrintable(QString::number(graph->glow())));

        // Sound now: it lights up.
        engine()->renderOffline(0.0, 4096);
        refreshDisplays();
        tick(5);
        QVERIFY2(graph->glow() > 0.5, qPrintable(QString::number(graph->glow())));
    }

    // A layout change during a fade: each layout's voices go on from what they show (nothing pops), and one
    // fading out comes back from where it is.
    void layoutsFadeWithoutPops() {
        QQuickItem* view = showChorus();
        QVERIFY(view);
        auto* graph = find<ChorusGraph>(view, QStringLiteral("chorusGraph"));
        QVERIFY(graph);
        const ChorusLayout chorus{0, 1, 0}, ensemble{1, 1, 0}, vibrato{2, 1, 0};
        refreshDisplays();
        set("mode", 1.0);
        tick(2);
        const double in = graph->layoutAlpha(ensemble), out = graph->layoutAlpha(chorus);
        QVERIFY2(in > 0.2 && in < 0.95 && out > 0.05 && out < 0.8,
                 qPrintable(QStringLiteral("%1 %2").arg(in).arg(out)));
        set("mode", 2.0);  // half way: Vibrato fades in, the other two fade out from where they are
        QCOMPARE(graph->layoutAlpha(ensemble), in);
        QCOMPARE(graph->layoutAlpha(chorus), out);
        QCOMPARE(graph->layoutAlpha(vibrato), 0.0);
        QCOMPARE(graph->layoutFade(), 0.0);
        tick(1);
        QVERIFY(graph->layoutAlpha(ensemble) < in && graph->layoutAlpha(chorus) < out);
        QVERIFY(graph->layoutAlpha(vibrato) > 0.0);
        const double ensembleNow = graph->layoutAlpha(ensemble);
        set("mode", 1.0);  // back to Ensemble while it fades out: from there
        QCOMPARE(graph->layoutAlpha(ensemble), ensembleNow);
        QCOMPARE(graph->layoutFade(), ensembleNow);
        tick(40);
        QCOMPARE(graph->layoutAlpha(ensemble), 1.0);
        QCOMPARE(graph->layoutAlpha(chorus), 0.0);
        QCOMPARE(graph->layoutAlpha(vibrato), 0.0);
        QCOMPARE(graph->voiceCount(), 6);
    }

    void screenshots() {
        QQuickItem* view = showChorus();
        QVERIFY(view);
        auto* graph = find<ChorusGraph>(view, QStringLiteral("chorusGraph"));
        QVERIFY(graph);

        // Nothing playing: still and dim.
        tick(10);
        QTest::qWait(50);
        save(grab(), QStringLiteral("chorus-idle.png"));

        // The defaults with the tone going through.
        play(8, 2048, 30000);
        QVERIFY(graph->glow() > 0.5);
        QTest::qWait(50);
        save(grab(), QStringLiteral("chorus.png"));

        // Ensemble, deep, with feedback and warmth.
        set("mode", 1.0);
        set("amount", 100.0);
        set("feedback", 40.0);
        set("warmth", 40.0);
        tick(20);
        play(4, 4096, 21000);
        QTest::qWait(150);
        play(1, 4096, 21000);
        QTest::qWait(30);
        save(grab(), QStringLiteral("chorus-ensemble.png"));

        // Vibrato, fast, its sides a quarter apart and half way to a triangle, fully wet.
        set("mode", 2.0);
        set("rate", 5.0);
        set("shape", 60.0);
        set("offset", 90.0);
        set("mix", 100.0);
        tick(20);
        play(4, 4096, 9000);
        QTest::qWait(150);
        play(1, 4096, 9000);
        QTest::qWait(30);
        save(grab(), QStringLiteral("chorus-vibrato.png"));

        // Chorus at 20 ms, one tap, the high-pass on; dragged (the header shows the Rate and the Amount).
        set("mode", 0.0);
        set("time", 3.0);
        set("taps", 0.0);
        set("hp", 1.0);
        set("rate", 1.2);
        set("mix", 50.0);
        tick(20);
        play(4, 4096, 26000);
        const QPoint at = centerOf(graph);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at + QPoint(0, -1));
        QTest::qWait(150);
        play(1, 4096, 26000);
        QTest::qWait(30);
        save(grab(), QStringLiteral("chorus-time.png"));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(0, -1));

        // Half way from those voices to Ensemble's: the old fading out as the new fade in, the axis easing.
        refreshDisplays();  // (the clock starts afresh: the ticks below count a clock period each)
        set("mode", 1.0);
        tick(2);
        QVERIFY2(graph->layoutFade() > 0.3 && graph->layoutFade() < 0.95,
                 qPrintable(QString::number(graph->layoutFade())));
        save(grab(), QStringLiteral("chorus-fading.png"));

        // Half way to Vibrato: the strip and the mode's column between their sets, one fading out before
        // the other fades in.
        tick(40);
        set("mode", 2.0);
        QTest::qWait(50);
        save(grab(), QStringLiteral("chorus-switching.png"));
    }
};

QTEST_MAIN(TestUiDeviceEditorsChorus)
#include "test_ui_device_editors_chorus.moc"
