// The Limiter's editor (ui/qml/devices/editors/LimiterEditor.qml, ui/src/devices/LimiterGraph): loaded
// as the device view loads it, over a real engine. It fits the view's height, and whatever the font
// every text shows whole (its columns as wide as their texts need, the graph's figures apart, its badges
// clear of the line's box); every control is bound to its parameter (undoably); during Gain and Output's
// crossfade only the one coming takes the mouse; it opens showing the device as it is (nothing animating
// in); dragging the line is one undo step, and the line's hover follows the line as it moves; what the
// engine publishes as it renders reaches the graph (levels, gain reduction, Soft Clip's share, the
// figures), which animates and then rests; the line and Soft Clip's band are the engine's own maths
// (LimiterResponse.h). With SUBSTATION_UI_SCREENSHOTS set to a folder, it is saved there limiting, in
// Soft Clip, with Maximize.

#include <QFontMetricsF>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <memory>
#include <tuple>
#include <vector>

#include "EditorHarness.h"
#include "audio/LimiterResponse.h"
#include "controls/KnobItem.h"
#include "controls/ValueBoxItem.h"
#include "devices/DeviceParam.h"
#include "devices/LimiterGraph.h"
#include "theme/Theme.h"

using namespace sub::app;
using namespace sub::ui;
using sub::app::test::kSampleRate;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Something with dynamics to limit: a kick every quarter of a second (a falling 55 Hz sine), a
// snare-ish noise burst between them, and a quiet chord under it all.
std::vector<float> beat(double seconds) {
    const int frames = int(seconds * kSampleRate);
    std::vector<float> out(std::size_t(frames), 0.0f);
    uint32_t noise = 12345;
    for (int i = 0; i < frames; ++i) {
        const double t = double(i) / kSampleRate;
        const double inBeat = std::fmod(t, 0.25);
        const double pitch = 55.0 + 90.0 * std::exp(-inBeat * 40.0);  // (a falling sweep)
        const double kick = 0.9 * std::exp(-inBeat * 18.0) * std::sin(2 * kPi * pitch * inBeat);
        const double half = std::fmod(t + 0.125, 0.25);
        noise = noise * 1664525u + 1013904223u;
        const double snare = (int(t / 0.125) % 4 == 3 ? 0.55 : 0.25) * std::exp(-half * 30.0) *
                             (double(noise >> 8) / double(1u << 24) * 2.0 - 1.0);
        const double chord = 0.08 * (std::sin(2 * kPi * 220.0 * t) + std::sin(2 * kPi * 277.2 * t) +
                                     std::sin(2 * kPi * 329.6 * t));
        out[std::size_t(i)] = float(kick + snare + chord);
    }
    return out;
}

}  // namespace

class TestUiDeviceEditorsLimiter : public QObject, public sub::app::test::EditorHarness {
    Q_OBJECT

    // A track playing `signal` (a 1 kHz tone at 0.5, -6.02 dBFS, by default) through a Limiter, its editor
    // shown: (track, device, editor, its graph).
    std::tuple<QString, QString, QQuickItem*, LimiterGraph*> limiter(const std::vector<float>& signal = {},
                                                                     double seconds = 1.0) {
        const QString track = audioTrackWith(signal.empty() ? tone(1000.0, int(seconds * kSampleRate)) : signal,
                                             QStringLiteral("tone"), seconds);
        if (track.isEmpty())
            return {};
        const QString device = editor()->addDevice(track, QStringLiteral("limiter"));
        QQuickItem* view = show(QStringLiteral("limiter"), track, device);
        return {track, device, view, view ? find<LimiterGraph>(view, QStringLiteral("limiterGraph")) : nullptr};
    }

    // An EditorKnob's dial.
    KnobItem* knob(QQuickItem* view, const char* name) {
        QQuickItem* cell = find(view, QString::fromLatin1(name));
        auto* paramKnob = cell ? qvariant_cast<QQuickItem*>(cell->property("knob")) : nullptr;
        return paramKnob ? qvariant_cast<KnobItem*>(paramKnob->property("knob")) : nullptr;
    }
    // A ParamBox's box.
    ValueBoxItem* box(QQuickItem* view, const char* name) {
        QQuickItem* item = find(view, QString::fromLatin1(name));
        return item ? qvariant_cast<ValueBoxItem*>(item->property("box")) : nullptr;
    }
    // The parameter a control is bound to.
    static sub::ui::DeviceParam* paramOf(QQuickItem* control) {
        return control ? qvariant_cast<sub::ui::DeviceParam*>(control->property("param")) : nullptr;
    }
    // A Text, as the captions and the readouts are (EditorCaption, EditorReadout), to measure texts with.
    std::unique_ptr<QQuickItem> textProbe_;
    // How wide `text` is in `font` as a caption or a readout lays it out: a Text's implicitWidth. (A Text lays
    // text out in the font's design metrics: where the font is hinted, QFontMetrics can be a pixel off it.)
    double textWidth(const QString& text, const QFont& font) {
        if (!textProbe_) {
            QQmlComponent component(qml_.get());
            component.setData(QByteArrayLiteral("import QtQuick\nText {}"), QUrl());
            textProbe_.reset(qobject_cast<QQuickItem*>(component.create()));
            if (!textProbe_) {
                qWarning() << component.errors();
                return 1e9;
            }
        }
        textProbe_->setProperty("font", font);
        textProbe_->setProperty("text", text);
        return textProbe_->implicitWidth();
    }
    // The widest text a parameter shows over its range (its format() at 20001 values across it, in even steps
    // of the value or, log-scaled, of its log: closer together than its texts step), a value showing it, and how
    // wide it is in `font`: as a box centres it (its advance) or, `asText`, as a Text lays it out.
    struct Widest {
        QString text;
        double width = 0.0;
        double value = 0.0;
    };
    Widest widestText(const sub::ui::DeviceParam* param, const QFont& font, bool asText) {
        const QFontMetricsF metrics(font);
        const double from = param->minimum(), to = param->maximum();
        constexpr int kSteps = 20000;
        Widest widest;
        QSet<QString> measured;
        for (int i = 0; i <= kSteps; ++i) {
            const double t = double(i) / kSteps;
            const double value = param->logScale() ? from * std::pow(to / from, t) : from + (to - from) * t;
            const QString text = param->format(value);
            if (measured.contains(text))
                continue;
            measured.insert(text);
            const double width = asText ? textWidth(text, font) : metrics.horizontalAdvance(text);
            if (width > widest.width)
                widest = {text, width, value};
        }
        return widest;
    }
    // Clicks a ParamButton.
    void click(QQuickItem* view, const char* name) {
        QQuickItem* control = find(view, QString::fromLatin1(name));
        QVERIFY(control);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                          centerOf(qvariant_cast<QQuickItem*>(control->property("button"))));
    }
    static bool lit(QQuickItem* view, const char* name) {
        QQuickItem* control = nullptr;
        for (QQuickItem* item : itemsNamed(view, QString::fromLatin1(name)))
            if (item->objectName() == QLatin1String(name))
                control = item;
        return control && control->property("lit").toBool();
    }
    // The engine's value of a parameter of the device.
    double engineParam(const QString& track, const QString& device, const char* id) {
        const auto pid = bridge()->engineDeviceId(track, device);
        if (!pid)
            return -999.0;
        return engine()->processorParam(*pid, engine()->processorParamIndex(*pid, id));
    }
    // Ticks the graph's animation until nothing moves (at most `ticks`).
    static void settle(LimiterGraph* graph, int ticks = 600) {
        for (int i = 0; i < ticks; ++i) {
            const int before = graph->updates();
            graph->advance(1.0 / 60);
            if (graph->updates() == before && i > 10)
                return;
        }
    }

private Q_SLOTS:
    void initTestCase() {
        if (!haveDisplay())
            QSKIP("needs a display: the offscreen platform renders Qt Quick in software, without this geometry");
        startHost();
    }

    void cleanupTestCase() {
        textProbe_.reset();  // (before its QML engine)
        stopHost();
    }

    void init() { clearHost(); }

    // --- Laid out and bound -------------------------------------------------------------------

    void fitsAndBinds() {
        auto [track, device, view, graph] = limiter();
        QVERIFY(view && graph);
        QVERIFY(view->implicitHeight() <= bodyHeight());  // fits the view
        // The graph grows into the body's height, 6 px from its top and bottom; the controls stay at the top.
        QCOMPARE(graph->height(), view->height() - 12);
        QCOMPARE(graph->width(), double(LimiterGraph::kWidth));

        KnobItem* gain = knob(view, "gain");
        KnobItem* release = knob(view, "release");
        QVERIFY(gain && release && knob(view, "output"));
        QCOMPARE(gain->value(), 0.0);
        QCOMPARE(release->value(), 300.0);
        QVERIFY(release->logScale());
        QVERIFY(gain->bipolar());  // (±24 dB)
        QVERIFY(!knob(view, "output")->bipolar() && !release->bipolar());
        QVERIFY(!find(view, QStringLiteral("output"))->isVisible());  // (Maximize is off)
        // Release is Auto's while it is on: dimmed, as Ableton greys it, and still settable.
        QVERIFY(find(view, QStringLiteral("release"))->isEnabled());
        QTRY_COMPARE(find(view, QStringLiteral("release"))->opacity(), 0.55);
        QVERIFY(lit(view, "autoRelease"));
        QVERIFY(lit(view, "routingLR"));
        QVERIFY(!lit(view, "routingMS"));
        QVERIFY(!lit(view, "maximize"));
        QCOMPARE(find(view, QStringLiteral("lookahead"))->property("index").toInt(), 1);  // 3 ms
        QCOMPARE(find(view, QStringLiteral("mode"))->property("index").toInt(), 0);
        ValueBoxItem* line = box(view, "lineBox");
        ValueBoxItem* link = box(view, "link");
        QVERIFY(line && link);
        QCOMPARE(line->text(), QStringLiteral("-0.3 dB"));
        QCOMPARE(link->text(), QStringLiteral("100 %"));
        // Whatever the font: the boxes are wide enough for their widest text with the automation dot (3.5 to
        // 8.5 px from the left) a pixel clear of it, centred (the line's box for the Ceiling's, and for the
        // Threshold's with Maximize); the knobs' cells for their names and widest values; the buttons and the
        // captions for their texts; the lists for their longest name with the arrow.
        auto boxFits = [&](ValueBoxItem* box, QQuickItem* control) {
            const Widest widest = widestText(paramOf(control), box->font(), false);
            QVERIFY2((box->width() - widest.width) / 2 >= 9.5,
                     qPrintable(QStringLiteral("\"%1\" (%2 px) in %3 px")
                                    .arg(widest.text)
                                    .arg(widest.width)
                                    .arg(box->width())));
        };
        boxFits(link, find(view, QStringLiteral("link")));
        boxFits(line, find(view, QStringLiteral("lineBox")));
        editor()->setDeviceParam(track, device, QStringLiteral("maximize"), 1.0);
        QTRY_COMPARE(paramOf(find(view, QStringLiteral("lineBox")))->paramId(), QStringLiteral("threshold"));
        boxFits(line, find(view, QStringLiteral("lineBox")));
        undo()->undo();
        const int knobSteps = undo()->index();
        for (const char* name : {"gain", "output", "release"}) {
            QQuickItem* cell = find(view, QString::fromLatin1(name));
            const QList<QQuickItem*> children = cell->childItems();
            QQuickItem* caption = children.first();
            QQuickItem* readout = children.last();
            const Widest widest = widestText(paramOf(cell), qvariant_cast<QFont>(readout->property("font")), true);
            const QString where = QStringLiteral("%1: \"%2\" (%3 px) in %4 px")
                                      .arg(QString::fromLatin1(name), widest.text)
                                      .arg(widest.width)
                                      .arg(readout->width());
            QVERIFY2(widest.width <= readout->width(), qPrintable(where));
            // (the readout showing it: whole)
            editor()->setDeviceParam(track, device, paramOf(cell)->paramId(), widest.value);
            QTRY_COMPARE(readout->property("text").toString(), widest.text);
            QVERIFY2(!readout->property("truncated").toBool() && readout->implicitWidth() <= readout->width(),
                     qPrintable(where));
            QVERIFY2(caption->implicitWidth() <= caption->width(), name);
        }
        while (undo()->index() > knobSteps)
            undo()->undo();
        for (const char* name : {"maximize", "autoRelease", "routingLR", "routingMS"}) {
            QQuickItem* button = find(view, QString::fromLatin1(name));
            QVERIFY2(button->implicitWidth() <= button->width(),
                     qPrintable(QStringLiteral("%1: %2 > %3").arg(name).arg(button->implicitWidth()).arg(
                         button->width())));
        }
        for (const char* name : {"lookaheadCaption", "modeCaption", "linkCaption"}) {
            QQuickItem* caption = find(view, QString::fromLatin1(name));
            QVERIFY2(caption->implicitWidth() <= caption->width(), name);
        }
        {
            // The line's caption, as wide as the wider of its names, so the box stays put when they swap.
            QQuickItem* caption = find(view, QStringLiteral("lineCaption"));
            const QFont font = qvariant_cast<QFont>(caption->property("font"));
            for (const QString& name : {QStringLiteral("Ceiling"), QStringLiteral("Threshold")})
                QVERIFY2(textWidth(name, font) <= caption->width(), qPrintable(name));
        }
        for (const char* name : {"lookahead", "mode"}) {
            QQuickItem* list = find(view, QString::fromLatin1(name));
            sub::ui::DeviceParam* p = paramOf(list);
            auto* button = qvariant_cast<QQuickItem*>(list->property("button"));
            QVERIFY(p && button);
            const int steps = undo()->index();
            for (int i = 0; i < p->labels().size(); ++i) {
                p->set(i);
                QTRY_COMPARE(list->property("index").toInt(), i);
                QVERIFY2(button->implicitWidth() <= list->width(), qPrintable(p->labels().at(i)));
            }
            while (undo()->index() > steps)
                undo()->undo();
        }
        QVERIFY(std::abs(graph->lineDb() + 0.3) < 1e-6);
        QVERIFY(!graph->maximize());

        // Every control is bound to its parameter and has a tooltip.
        const QList<std::pair<const char*, const char*>> bound = {
            {"gain", "gain"},         {"output", "output"},       {"maximize", "maximize"}, {"lineBox", "ceiling"},
            {"release", "release"},   {"autoRelease", "auto_release"}, {"lookahead", "lookahead"},
            {"mode", "mode"},         {"routingLR", "routing"},   {"routingMS", "routing"}, {"link", "link"}};
        for (const auto& [name, id] : bound) {
            QQuickItem* control = find(view, QString::fromLatin1(name));
            QVERIFY2(control, name);
            sub::ui::DeviceParam* p = paramOf(control);
            QVERIFY2(p && p->valid(), name);
            QCOMPARE(p->paramId(), QString::fromLatin1(id));
            QVERIFY2(!control->property("tooltip").toString().isEmpty(), name);
        }

        // Nothing overlaps: the columns side by side within the body, the line's box in the graph's header.
        // The body is as wide as the columns (as their texts need), the two knob columns alike.
        auto rectOf = [&](const char* name) {
            QQuickItem* item = find(view, QString::fromLatin1(name));
            return item ? item->mapRectToItem(view, QRectF(0, 0, item->width(), item->height())) : QRectF();
        };
        QCOMPARE(view->implicitWidth(), rectOf("link").right() + 8);
        QCOMPARE(rectOf("routingMS").right(), rectOf("link").right());
        QCOMPARE(rectOf("mode").right(), rectOf("link").right());
        QCOMPARE(rectOf("gain").width(), rectOf("release").width());
        QCOMPARE(rectOf("maximize").width(), rectOf("gain").width());
        const QRectF graphRect = rectOf("limiterGraph");
        QVERIFY(rectOf("gain").right() <= graphRect.left() && rectOf("maximize").right() <= graphRect.left());
        QVERIFY(rectOf("release").left() >= graphRect.right() && rectOf("autoRelease").left() >= graphRect.right());
        QVERIFY(rectOf("lookahead").left() >= rectOf("release").right());
        QVERIFY(rectOf("routingLR").right() <= rectOf("routingMS").left());
        for (const char* name :
             {"gain", "maximize", "release", "autoRelease", "lookahead", "mode", "routingMS", "link"}) {
            const QRectF r = rectOf(name);
            QVERIFY2(r.left() >= 8 && r.right() <= view->width() - 8 + 0.5 && r.top() >= 6 &&
                         r.bottom() <= view->height() - 6,
                     name);
        }
        const QRectF lineBox = rectOf("lineBox");
        QVERIFY(graphRect.contains(lineBox));
        QVERIFY(lineBox.bottom() <= graphRect.top() + graph->plot().top());  // in the header, over no plot
    }

    void controlsAreUndoable() {
        auto [track, device, view, graph] = limiter();
        QVERIFY(view && graph);
        auto value = [&](const char* id) { return param(track, device, QString::fromLatin1(id)); };

        // A knob follows its parameter, undoably.
        KnobItem* gain = knob(view, "gain");
        editor()->setDeviceParam(track, device, QStringLiteral("gain"), 6.0);
        QCOMPARE(gain->value(), 6.0);
        undo()->undo();
        QCOMPARE(gain->value(), 0.0);

        // Auto: off brings the Release knob up (it was dimmed); on dims it again. It can be set either way.
        click(view, "autoRelease");
        QCOMPARE(value("auto_release"), 0.0);
        QTRY_COMPARE(find(view, QStringLiteral("release"))->opacity(), 1.0);
        QVERIFY(!lit(view, "autoRelease"));
        undo()->undo();
        QCOMPARE(value("auto_release"), 1.0);
        QTRY_COMPARE(find(view, QStringLiteral("release"))->opacity(), 0.55);
        {
            const QPoint dial = centerOf(knob(view, "release"));
            QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, dial);
            dragTo(dial - QPoint(0, 30));
            QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, dial - QPoint(0, 30));
            QVERIFY2(value("release") > 300.0, qPrintable(QString::number(value("release"))));
            undo()->undo();
            QCOMPARE(value("release"), 300.0);
        }

        // Routing: a button per choice.
        click(view, "routingMS");
        QCOMPARE(value("routing"), 1.0);
        QVERIFY(lit(view, "routingMS") && !lit(view, "routingLR"));
        undo()->undo();
        QCOMPARE(value("routing"), 0.0);

        // Mode and Lookahead: lists. The lookahead is the device's latency: the engine's follows.
        QQuickItem* mode = find(view, QStringLiteral("mode"));
        QVERIFY(QMetaObject::invokeMethod(mode, "choose", Q_ARG(QVariant, 2)));  // as the list does
        QCOMPARE(value("mode"), 2.0);
        undo()->undo();
        QCOMPARE(value("mode"), 0.0);
        const auto pid = bridge()->engineDeviceId(track, device);
        QVERIFY(pid);
        QCOMPARE(engine()->processorInfo(*pid).latency, 144);
        QQuickItem* lookahead = find(view, QStringLiteral("lookahead"));
        QVERIFY(QMetaObject::invokeMethod(lookahead, "choose", Q_ARG(QVariant, 2)));
        QCOMPARE(value("lookahead"), 2.0);
        QTRY_COMPARE(engine()->processorInfo(*bridge()->engineDeviceId(track, device)).latency, 288);
        undo()->undo();
        QCOMPARE(value("lookahead"), 1.0);
        QTRY_COMPARE(engine()->processorInfo(*bridge()->engineDeviceId(track, device)).latency, 144);

        // Maximize: the Gain knob gives way to Output, the line becomes the Threshold (box and graph).
        click(view, "maximize");
        QCOMPARE(value("maximize"), 1.0);
        QTest::qWait(200);  // (the 120 ms crossfade)
        QVERIFY(!find(view, QStringLiteral("gain"))->isVisible());
        QVERIFY(find(view, QStringLiteral("output"))->isVisible());
        QCOMPARE(paramOf(find(view, QStringLiteral("lineBox")))->paramId(), QStringLiteral("threshold"));
        QCOMPARE(box(view, "lineBox")->text(), QStringLiteral("-0.3 dB"));  // (at the ceiling's: nothing changes)
        QVERIFY(graph->maximize());
        QVERIFY(std::abs(graph->lineDb() + 0.3) < 1e-6);
        undo()->undo();
        QCOMPARE(value("maximize"), 0.0);
        QTest::qWait(200);
        QVERIFY(find(view, QStringLiteral("gain"))->isVisible());
        QVERIFY(!find(view, QStringLiteral("output"))->isVisible());
        QCOMPARE(paramOf(find(view, QStringLiteral("lineBox")))->paramId(), QStringLiteral("ceiling"));
        QVERIFY(std::abs(graph->lineDb() + 0.3) < 1e-6);

        // Mid-crossfade, the knob fading in takes the press, not the one fading out over it: only the one
        // coming is enabled.
        click(view, "maximize");
        QTest::qWait(200);
        undo()->undo();
        QVERIFY(find(view, QStringLiteral("gain"))->isEnabled());
        QVERIFY(!find(view, QStringLiteral("output"))->isEnabled());
        for (int i = 0; i < 200 && !find(view, QStringLiteral("gain"))->isVisible(); ++i)
            QTest::qWait(2);  // (Gain shows from its first frame; Output, fading out, is still over it)
        const QPoint dial = centerOf(knob(view, "gain"));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, dial);
        dragTo(dial + QPoint(0, 30));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, dial + QPoint(0, 30));
        QVERIFY2(value("gain") < -0.5, qPrintable(QString::number(value("gain"))));
        QVERIFY(std::abs(value("output") + 0.3) < 1e-6);
        undo()->undo();
        QCOMPARE(value("gain"), 0.0);

        // Link's box: a drag is one undo step; double-click resets it.
        ValueBoxItem* link = box(view, "link");
        QVERIFY(link);
        editor()->setDeviceParam(track, device, QStringLiteral("link"), 50.0);
        int steps = undo()->index();
        const QPoint at = centerOf(link);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        for (int dy = 5; dy <= 20; dy += 5)
            dragTo(at - QPoint(0, dy));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 20));
        QVERIFY2(value("link") > 50.0 && value("link") < 100.0, qPrintable(QString::number(value("link"))));
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(undo()->count(), undo()->index());
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(value("link"), 100.0);

        // The engine has what the editor set.
        editor()->setDeviceParam(track, device, QStringLiteral("gain"), 4.5);
        editor()->setDeviceParam(track, device, QStringLiteral("ceiling"), -3.0);
        QTRY_COMPARE(engineParam(track, device, "gain"), 4.5);
        QCOMPARE(engineParam(track, device, "ceiling"), -3.0);
        QCOMPARE(engineParam(track, device, "link"), 100.0);
    }

    // --- The line ----------------------------------------------------------------------------

    void lineDragIsOneUndoStep() {
        auto [track, device, view, graph] = limiter();
        QVERIFY(view && graph);
        auto value = [&](const char* id) { return param(track, device, QString::fromLatin1(id)); };
        const double x = graph->plot().center().x();

        // Dragged down to -6 dB: the ceiling follows, one undo step; the line sits on it (no easing while held).
        int steps = undo()->index();
        const QPoint at = scenePoint(graph, QPointF(x, graph->lineY()));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        QVERIFY(graph->dragging());
        const QPoint to = scenePoint(graph, QPointF(x, graph->yOf(-6.0)));
        for (int i = 1; i <= 3; ++i)
            dragTo(at + (to - at) * i / 3);
        QVERIFY(std::abs(graph->lineY() - graph->yOf(value("ceiling"))) < 1e-6);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, to);
        QVERIFY(!graph->dragging());
        QVERIFY2(std::abs(value("ceiling") + 6.0) <= 0.3, qPrintable(QString::number(value("ceiling"))));
        QCOMPARE(std::round(value("ceiling") * 10) / 10, value("ceiling"));  // (to 0.1 dB)
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(undo()->count(), undo()->index());
        QVERIFY(std::abs(graph->lineY() - graph->yOf(value("ceiling"))) < 1e-4);
        QCOMPARE(box(view, "lineBox")->text(), QStringLiteral("%1 dB").arg(value("ceiling"), 0, 'f', 1));

        // With Shift, a quarter as far; pressing Shift halfway through doesn't make it jump.
        {
            const double before = value("ceiling");
            const QPoint from = scenePoint(graph, QPointF(x, graph->lineY()));
            QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, from);
            dragTo(from + QPoint(0, 10), Qt::ShiftModifier);
            dragTo(from + QPoint(0, 20), Qt::ShiftModifier);
            QTest::mouseRelease(window_, Qt::LeftButton, Qt::ShiftModifier, from + QPoint(0, 20));
            const double perPixel = (LimiterGraph::kTopDb - LimiterGraph::kFloorDb) / graph->plot().height();
            QVERIFY2(std::abs((before - value("ceiling")) - 20 * perPixel / 4) <= 0.1,
                     qPrintable(QString::number(value("ceiling"))));

            const double start = value("ceiling");
            const QPoint again = scenePoint(graph, QPointF(x, graph->lineY()));
            QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, again);
            dragTo(again - QPoint(0, 4));
            const double half = value("ceiling");
            QVERIFY(half > start);
            dragTo(again - QPoint(0, 5), Qt::ShiftModifier);
            QVERIFY2(std::abs(value("ceiling") - half) < 0.5, qPrintable(QString::number(value("ceiling"))));
            QTest::mouseRelease(window_, Qt::LeftButton, Qt::ShiftModifier, again - QPoint(0, 5));
        }

        // Double-click on the line: its default, one step. The same on the box.
        steps = undo()->index();
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, scenePoint(graph, QPointF(x, graph->lineY())));
        QVERIFY2(std::abs(value("ceiling") + 0.3) < 1e-6, qPrintable(QString::number(value("ceiling"))));
        QCOMPARE(undo()->index(), steps + 1);
        editor()->setDeviceParam(track, device, QStringLiteral("ceiling"), -9.0);
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(box(view, "lineBox")));
        QVERIFY(std::abs(value("ceiling") + 0.3) < 1e-6);

        // It stops at the parameter's range: dragged far up, the Ceiling's most (0 dB), and back down from
        // there at once.
        {
            const QPoint from = scenePoint(graph, QPointF(x, graph->lineY()));
            QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, from);
            dragTo(from - QPoint(0, 60));
            QCOMPARE(value("ceiling"), 0.0);
            dragTo(from - QPoint(0, 55));
            QVERIFY2(value("ceiling") < -0.5, qPrintable(QString::number(value("ceiling"))));
            QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, from - QPoint(0, 55));
            QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier,
                               scenePoint(graph, QPointF(x, graph->lineY())));
            QVERIFY(std::abs(value("ceiling") + 0.3) < 1e-6);
        }

        // A press away from the line changes nothing.
        steps = undo()->index();
        const QPoint away = scenePoint(graph, QPointF(x, graph->lineY() + 30));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, away);
        QVERIFY(!graph->dragging());
        dragTo(away + QPoint(0, 10));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, away + QPoint(0, 10));
        QCOMPARE(undo()->index(), steps);
        QVERIFY(std::abs(value("ceiling") + 0.3) < 1e-6);

        // With Maximize the line is the Threshold: the same drag sets it and leaves the Ceiling.
        editor()->setDeviceParam(track, device, QStringLiteral("threshold"), -3.0);
        editor()->setDeviceParam(track, device, QStringLiteral("maximize"), 1.0);
        settle(graph);  // (the line eases from the ceiling to the threshold)
        QVERIFY(std::abs(graph->lineY() - graph->yOf(-3.0)) < 1e-3);
        steps = undo()->index();
        const QPoint top = scenePoint(graph, QPointF(x, graph->lineY()));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, top);
        for (int i = 1; i <= 3; ++i)
            dragTo(top + QPoint(0, 4 * i));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, top + QPoint(0, 12));
        QVERIFY2(value("threshold") < -4.0, qPrintable(QString::number(value("threshold"))));
        QVERIFY(std::abs(value("ceiling") + 0.3) < 1e-6);
        QCOMPARE(undo()->index(), steps + 1);
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, scenePoint(graph, QPointF(x, graph->lineY())));
        QVERIFY(std::abs(value("threshold") + 0.3) < 1e-6);
    }

    void hoverFollowsTheLine() {
        auto [track, device, view, graph] = limiter();
        QVERIFY(view && graph);
        editor()->setDeviceParam(track, device, QStringLiteral("ceiling"), -6.0);
        settle(graph);
        const double x = graph->plot().center().x();
        // Over the line: it lights, and the cursor is for resizing.
        QTest::mouseMove(window_, scenePoint(graph, QPointF(x, graph->lineY() + 20)));
        QTest::mouseMove(window_, scenePoint(graph, QPointF(x, graph->lineY())));
        QTest::qWait(30);  // (the window delivers hover as it updates)
        settle(graph);
        QVERIFY(graph->hover() > 0.99);
        QCOMPARE(graph->cursor().shape(), Qt::SizeVerCursor);
        // The line moves away from the still mouse (a double-click's reset, undo, automation): the hover
        // and the cursor go with it.
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, scenePoint(graph, QPointF(x, graph->lineY())));
        QVERIFY(std::abs(param(track, device, QStringLiteral("ceiling")) + 0.3) < 1e-6);
        settle(graph);
        QVERIFY(graph->hover() < 0.01);
        QVERIFY(graph->cursor().shape() != Qt::SizeVerCursor);
        // And back under it.
        undo()->undo();
        settle(graph);
        QVERIFY(graph->hover() > 0.99);
        QCOMPARE(graph->cursor().shape(), Qt::SizeVerCursor);
    }

    void opensAsItIs() {
        // A Limiter set up before its editor opens (another track was shown, or the set was reopened): it
        // appears as it is, the line at its place and Soft Clip's band and badge in, nothing animating in.
        const QString track = audioTrackWith(tone(1000.0, int(kSampleRate)), QStringLiteral("tone"), 1.0);
        QVERIFY(!track.isEmpty());
        const QString device = editor()->addDevice(track, QStringLiteral("limiter"));
        editor()->setDeviceParam(track, device, QStringLiteral("ceiling"), -12.0);
        editor()->setDeviceParam(track, device, QStringLiteral("mode"), 1.0);
        QQuickItem* view = show(QStringLiteral("limiter"), track, device);
        auto* graph = view ? find<LimiterGraph>(view, QStringLiteral("limiterGraph")) : nullptr;
        QVERIFY(graph);
        QCOMPARE(graph->lineDb(), -12.0);
        QCOMPARE(graph->shownLineDb(), -12.0);
        QCOMPARE(graph->softBand(), 1.0);
        QCOMPARE(graph->badge(), 1.0);
        const int updates = graph->updates();
        settle(graph);
        QVERIFY(graph->updates() - updates <= 1);  // (nothing to move: at most the first paint)
    }

    // --- The displays ------------------------------------------------------------------------

    void displaysReachTheGraph() {
        auto [track, device, view, graph] = limiter();
        QVERIFY(view && graph);
        // +12 dB of Gain: the 0.5 tone comes in at +6 dBFS against a -0.3 dB ceiling.
        editor()->setDeviceParam(track, device, QStringLiteral("gain"), 12.0);
        engine()->renderOffline(0.0, kSampleRate / 2);
        refreshDisplays();
        const double over = 20 * std::log10(0.5) + 12.0;  // +6.02
        QVERIFY2(std::abs(graph->reduction() - (over + 0.3)) < 0.3, qPrintable(QString::number(graph->reduction())));
        QVERIFY2(std::abs(graph->levelIn() - over) < 0.3, qPrintable(QString::number(graph->levelIn())));
        QVERIFY2(std::abs(graph->levelOut() + 0.3) < 0.1, qPrintable(QString::number(graph->levelOut())));
        QCOMPARE(graph->clipping(), 0.0);
        QVERIFY(graph->valuesRead() > 0);
        // The figures: the reduction over the last half second; In (after Gain), GR and Out peaks.
        QCOMPARE(graph->figures(),
                 QStringList({QStringLiteral("GR −6.3 dB"), QStringLiteral("6.0"), QStringLiteral("-6.3"),
                              QStringLiteral("-0.3")}));

        // The history scrolls on; the meters read what the cursor passes, up at once. The line warms with
        // the gain reduction.
        const double cursor = graph->historyCursor();
        graph->advance(1.0 / 60);
        QVERIFY(graph->historyCursor() > cursor);
        QVERIFY2(std::abs(graph->meterIn(0).level - over) < 0.3, qPrintable(QString::number(graph->meterIn(0).level)));
        QVERIFY2(std::abs(graph->meterOut(0).peak + 0.3) < 0.1, qPrintable(QString::number(graph->meterOut(0).peak)));
        QVERIFY2(std::abs(graph->meterGr(0).level - (over + 0.3)) < 0.3 && graph->meterGr(1).level > 6.0,
                 qPrintable(QString::number(graph->meterGr(0).level)));
        for (int i = 0; i < 5; ++i)
            graph->advance(1.0 / 60);
        QVERIFY2(graph->glow() > 0.5, qPrintable(QString::number(graph->glow())));
        // It stops at the newest value read (it never runs past the data: the history holds still).
        for (int i = 0; i < 20 && graph->historyCursor() < double(graph->valuesRead()); ++i)
            graph->advance(1.0 / 60);
        QCOMPARE(graph->historyCursor(), double(graph->valuesRead()));
        // With nothing passing, the meters fall at 24 dB a second, their peaks held.
        const MeterBallistics out = graph->meterOut(0);
        graph->advance(1.0 / 60);
        QVERIFY(graph->meterOut(0).level >= out.level - 24.0 / 60 - 0.01);
        QVERIFY(graph->meterOut(0).level < out.level);
        QCOMPARE(graph->meterOut(0).peak, out.peak);
        for (int i = 0; i < 60; ++i)
            graph->advance(1.0 / 60);
        QCOMPARE(graph->historyCursor(), double(graph->valuesRead()));

        // Then everything settles, and it rests: no more repainting.
        settle(graph);
        int updates = graph->updates();
        for (int i = 0; i < 60; ++i)
            graph->advance(1.0 / 60);
        QCOMPARE(graph->updates(), updates);
        QCOMPARE(graph->meterIn(0).level, LimiterGraph::kFloorDb);
        QCOMPARE(graph->meterGr(0).level, 0.0);

        // Silence arriving (past the clip) scrolls the history empty, then rests too.
        engine()->renderOffline(4.0, 2 * kSampleRate);
        refreshDisplays();
        QCOMPARE(graph->levelIn(), -90.0);
        QCOMPARE(graph->reduction(), 0.0);
        settle(graph);
        engine()->renderOffline(4.0, kSampleRate / 4);
        refreshDisplays();
        updates = graph->updates();
        for (int i = 0; i < 30; ++i)
            graph->advance(1.0 / 60);
        QCOMPARE(graph->updates(), updates);

        // Soft Clip: +7 dB of Gain brings the tone to 1.16 of the ceiling, inside the knee (under its
        // top at 1.5): the knee does the work (1.159 -> 0.942), the limiter none.
        editor()->setDeviceParam(track, device, QStringLiteral("mode"), 1.0);
        editor()->setDeviceParam(track, device, QStringLiteral("gain"), 7.0);
        engine()->renderOffline(0.0, kSampleRate / 2);
        refreshDisplays();
        QVERIFY2(std::abs(graph->clipping() - 1.8) < 0.2, qPrintable(QString::number(graph->clipping())));
        QVERIFY2(graph->reduction() < 0.05, qPrintable(QString::number(graph->reduction())));
        graph->advance(1.0 / 60);
        QVERIFY2(std::abs(graph->meterClip().level - 1.8) < 0.2, qPrintable(QString::number(graph->meterClip().level)));
        // The line warms with it too (1.8 dB of the 6 that warm it fully), as it does with the limiter's own
        // reduction, of which there is none.
        for (int i = 0; i < 2; ++i)
            graph->advance(1.0 / 60);
        QVERIFY2(graph->glow() > 0.05 && graph->glow() < 0.35, qPrintable(QString::number(graph->glow())));
        // Both gain reduction figures include the knee's share, as the GR bars do: they agree.
        for (int i = 0; i < 58; ++i)
            graph->advance(1.0 / 60);
        const QStringList soft = graph->figures();
        QCOMPARE(soft.at(2), QStringLiteral("-1.8"));
        QCOMPARE(soft.at(0), QStringLiteral("GR −1.8 dB"));

        // With Maximize the history's output is drawn in the line's domain (Output - Threshold off), so
        // its top meets the line; the input is the raw input; the Out meter reads dBFS, as its axis.
        editor()->setDeviceParam(track, device, QStringLiteral("mode"), 0.0);
        editor()->setDeviceParam(track, device, QStringLiteral("maximize"), 1.0);
        editor()->setDeviceParam(track, device, QStringLiteral("threshold"), -12.0);
        editor()->setDeviceParam(track, device, QStringLiteral("output"), -1.0);
        settle(graph);  // (the meters back down)
        engine()->renderOffline(0.0, kSampleRate / 2);
        refreshDisplays();
        QVERIFY2(std::abs(graph->levelIn() - 20 * std::log10(0.5)) < 0.3,
                 qPrintable(QString::number(graph->levelIn())));
        QVERIFY2(std::abs(graph->levelOut() + 1.0) < 0.1, qPrintable(QString::number(graph->levelOut())));
        QVERIFY2(std::abs(graph->reduction() - 5.98) < 0.3, qPrintable(QString::number(graph->reduction())));
        graph->advance(1.0 / 60);
        QVERIFY2(std::abs(graph->meterOut(0).level + 1.0) < 0.1,
                 qPrintable(QString::number(graph->meterOut(0).level)));
        // The Out meter and figure are in dBFS as the output came, whatever the line does after.
        for (int i = 0; i < 60; ++i)
            graph->advance(1.0 / 60);
        const QString outFigure = graph->figures().at(3);
        QVERIFY2(outFigure.toDouble() > -2.0, qPrintable(outFigure));  // (dBFS, not the line's -12)
        editor()->setDeviceParam(track, device, QStringLiteral("threshold"), -6.0);
        graph->advance(1.0 / 60);
        QCOMPARE(graph->figures().at(3), outFigure);
        QVERIFY2(std::abs(graph->meterOut(0).peak + 1.0) < 0.1, qPrintable(QString::number(graph->meterOut(0).peak)));
    }

    void figuresKeepApart() {
        // Long figures: 24 dB of Gain into a -24 dB ceiling (In 18.0, GR -42.0, Out -24.0). Whatever the font,
        // the footer's figures are each as wide as their text, apart and inside the graph; with Maximize and
        // True Peak, the header's badges stay clear of the line's box.
        auto [track, device, view, graph] = limiter();
        QVERIFY(view && graph);
        editor()->setDeviceParam(track, device, QStringLiteral("gain"), 24.0);
        editor()->setDeviceParam(track, device, QStringLiteral("ceiling"), -24.0);
        engine()->renderOffline(0.0, kSampleRate / 2);
        refreshDisplays();
        for (int i = 0; i < 60; ++i)
            graph->advance(1.0 / 60);
        const QStringList texts = graph->figures();
        const QList<QRectF> rects = graph->figureRects();
        QCOMPARE(rects.size(), 4);
        QCOMPARE(texts.mid(1), QStringList({QStringLiteral("18.0"), QStringLiteral("-42.0"), QStringLiteral("-24.0")}));
        const QString where = texts.join(QStringLiteral(", "));
        for (int i = 0; i < 4; ++i) {
            const double width = QFontMetricsF(uiFont(i == 0 ? 8 : 7)).horizontalAdvance(texts.at(i));
            QVERIFY2(rects[i].width() >= width - 1e-9, qPrintable(where));
            QVERIFY2(rects[i].left() >= 0 && rects[i].right() <= graph->width() && rects[i].bottom() <= graph->height(),
                     qPrintable(where));
            if (i > 0)  // (left to right: the gain reduction, then the In, GR and Out peaks)
                QVERIFY2(rects[i].left() >= rects[i - 1].right() + (i > 1 ? LimiterGraph::kFigureGap : 0) - 1e-9,
                         qPrintable(where));
        }

        editor()->setDeviceParam(track, device, QStringLiteral("mode"), 2.0);
        editor()->setDeviceParam(track, device, QStringLiteral("maximize"), 1.0);
        for (int i = 0; i < 60; ++i)
            graph->advance(1.0 / 60);  // (the badges ease in)
        QVERIFY(graph->badge() > 0.99);
        QTest::qWait(200);  // (the knobs' crossfade)
        QQuickItem* lineBox = find(view, QStringLiteral("lineBox"));
        const QRectF box = lineBox->mapRectToItem(graph, QRectF(0, 0, lineBox->width(), lineBox->height()));
        for (const QRectF& badge : graph->badgeRects())
            QVERIFY2(box.right() + 4 <= badge.left(),
                     qPrintable(QStringLiteral("%1 > %2").arg(box.right()).arg(badge.left())));
        save(grab(), QStringLiteral("limiter-long-figures.png"));
    }

    void modeAnimates() {
        auto [track, device, view, graph] = limiter();
        QVERIFY(view && graph);
        settle(graph);
        QCOMPARE(graph->softBand(), 0.0);

        // Soft Clip's band eases in (and out again).
        editor()->setDeviceParam(track, device, QStringLiteral("mode"), 1.0);
        graph->advance(1.0 / 60);
        QVERIFY(graph->softBand() > 0.0 && graph->softBand() < 1.0);
        for (int i = 0; i < 30; ++i)
            graph->advance(1.0 / 60);
        QVERIFY(graph->softBand() > 0.99);
        editor()->setDeviceParam(track, device, QStringLiteral("mode"), 2.0);
        graph->advance(1.0 / 60);
        QVERIFY(graph->softBand() > 0.0 && graph->softBand() < 1.0);
        for (int i = 0; i < 30; ++i)
            graph->advance(1.0 / 60);
        QVERIFY(graph->softBand() < 0.01);
        QVERIFY(graph->badge() > 0.99);  // (True Peak's badge)

        // The line moved otherwise than by dragging (automation, undo, the box) eases to its place.
        settle(graph);
        editor()->setDeviceParam(track, device, QStringLiteral("ceiling"), -6.0);
        graph->advance(1.0 / 60);
        QVERIFY(graph->shownLineDb() < -0.31 && graph->shownLineDb() > -5.99);
        for (int i = 0; i < 30; ++i)
            graph->advance(1.0 / 60);
        QVERIFY(std::abs(graph->shownLineDb() + 6.0) < 0.01);
        // Then it rests.
        settle(graph);
        const int updates = graph->updates();
        graph->advance(1.0 / 60);
        QCOMPARE(graph->updates(), updates);
    }

    void sharedMaths() {
        auto [track, device, view, graph] = limiter();
        QVERIFY(view && graph);
        // Soft Clip's band: from where the knee starts to where it reaches the line, the engine's.
        QVERIFY(std::abs(graph->softKneeDb() + 6.02) < 0.01);
        QVERIFY(std::abs(graph->softTopDb() - 3.52) < 0.01);
        QCOMPARE(graph->softKneeDb(), limiterSoftKneeDb());
        QCOMPARE(graph->softTopDb(), limiterSoftTopDb());
        // The line: the ceiling, or with Maximize the threshold (the output then drawn Output - Threshold lower).
        const LimiterLine line = limiterLine(true, 0.0, -0.3, -12.0, -1.0);
        QVERIFY(std::abs(line.lineDb + 12.0) < 1e-6);
        QVERIFY(std::abs(line.outputShiftDb - 11.0) < 1e-5);
        const LimiterLine plain = limiterLine(false, 6.0, -0.3, -12.0, -1.0);
        QVERIFY(std::abs(plain.lineDb + 0.3) < 1e-6);
        QCOMPARE(plain.outputShiftDb, 0.0);
        // The axes: +12 dB at the plot's top, -36 at its bottom; the reduction's 24 dB at the bottom (6 dB
        // on the level's 0 dB line).
        const QRectF plot = graph->plot();
        QCOMPARE(graph->yOf(LimiterGraph::kTopDb), plot.top());
        QCOMPARE(graph->yOf(LimiterGraph::kFloorDb), plot.bottom());
        QCOMPARE(graph->grY(LimiterGraph::kGrRangeDb), plot.bottom());
        QVERIFY(std::abs(graph->grY(6.0) - graph->yOf(0.0)) < 1e-9);
    }

    // --- The look ----------------------------------------------------------------------------

    void screenshots() {
        auto [track, device, view, graph] = limiter(beat(2.0), 2.0);
        QVERIFY(view && graph);
        save(grab(), QStringLiteral("limiter-idle.png"));  // (as it comes: nothing played yet)
        // Limiting a beat: 6 dB of Gain.
        editor()->setDeviceParam(track, device, QStringLiteral("gain"), 6.0);
        // Played up to just after a kick, the display ticked on a little (grabbing renders it as it is).
        auto play = [&](double seconds) {
            engine()->renderOffline(0.0, int(seconds * kSampleRate));
            refreshDisplays();
            for (int i = 0; i < 2; ++i)
                graph->advance(1.0 / 60);
        };
        play(1.56);
        QVERIFY(graph->reduction() > 1.0);
        save(grab(), QStringLiteral("limiter.png"));

        // Soft Clip: the knee's band, its share of the reduction under it.
        editor()->setDeviceParam(track, device, QStringLiteral("mode"), 1.0);
        for (int i = 0; i < 30; ++i)
            graph->advance(1.0 / 60);
        play(1.56);
        save(grab(), QStringLiteral("limiter-soft-clip.png"));

        // Maximize: Output for Gain, the line the Threshold, MAX by the mode; True Peak, M/S.
        editor()->setDeviceParam(track, device, QStringLiteral("mode"), 2.0);
        editor()->setDeviceParam(track, device, QStringLiteral("routing"), 1.0);
        editor()->setDeviceParam(track, device, QStringLiteral("link"), 50.0);
        editor()->setDeviceParam(track, device, QStringLiteral("maximize"), 1.0);
        editor()->setDeviceParam(track, device, QStringLiteral("threshold"), -8.0);
        editor()->setDeviceParam(track, device, QStringLiteral("output"), -0.3);
        editor()->setDeviceParam(track, device, QStringLiteral("auto_release"), 0.0);
        for (int i = 0; i < 30; ++i)
            graph->advance(1.0 / 60);
        QTest::qWait(200);  // (the knobs' crossfade)
        play(1.56);
        save(grab(), QStringLiteral("limiter-maximize.png"));

        // The line held: thicker, its handle lit.
        const QPoint at = scenePoint(graph, QPointF(graph->plot().center().x(), graph->lineY()));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at + QPoint(0, 6));
        for (int i = 0; i < 10; ++i)
            graph->advance(1.0 / 60);
        save(grab(), QStringLiteral("limiter-drag.png"));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(0, 6));
    }
};

QTEST_MAIN(TestUiDeviceEditorsLimiter)
#include "test_ui_device_editors_limiter.moc"
