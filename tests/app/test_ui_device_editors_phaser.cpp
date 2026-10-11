// The Phaser-Flanger's editor (ui/qml/devices/editors/PhaserEditor.qml,
// PhaserGraph): loaded as the device view loads it, over a real engine. It fits
// the view; every control is bound to its parameter and undoable, the swapped
// ones (Freq/Rate, Phase/Spin, the delay's Time) rebinding in place; the mode
// tabs, More (view state, not undone); a synced rate's wheel; the curve is the
// engine's design, its notch markers where the design puts them, a fine comb
// drawn as a band; the graph's drags and its double-click, one undo step each;
// what the engine's displays bring it (the sweep, the LFO's phase, the levels),
// going quiet without them and then not repainting; the meters showing the
// sound now, not a backlog's; the playback of the display values, smooth
// whatever the audio's block size, and the meters, the comet's tail and a
// random shape's trace keeping their pace; what the engine has after. With
// SUBSTATION_UI_SCREENSHOTS set to a folder, it is saved there as PNGs.

#include <QElapsedTimer>
#include <QFont>
#include <QFontMetricsF>
#include <QJSValue>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <memory>
#include <tuple>
#include <vector>

#include "../engine/harness/ThreadTime.h"  // (the cost tests' clock: the CPU time this thread has used)
#include "EditorHarness.h"
#include "audio/EngineBridge.h"
#include "audio/PhaserResponse.h"
#include "controls/KnobItem.h"
#include "devices/DeviceParam.h"
#include "devices/PhaserGraph.h"
#include "editor/ProjectEditor.h"
#include "model/ParamSpec.h"

using namespace sub::app;
using namespace sub::ui;
using sub::app::test::kSampleRate;
using subtest::threadSeconds;

namespace {

constexpr double kPi = 3.14159265358979323846;

// QML's warnings while a test runs (a binding to something missing, a type error): each test fails on any.
QStringList qmlWarnings;
QtMessageHandler previousHandler = nullptr;

void recordWarnings(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (type == QtWarningMsg && (message.contains(QStringLiteral(".qml")) || message.contains(QStringLiteral("QML"))))
        qmlWarnings << message;
    if (previousHandler)
        previousHandler(type, context, message);
}

// Every control shown by default (Phaser mode, More closed): its object name and parameter.
struct Control {
    const char* name;
    const char* param;
};
constexpr Control kKnobs[] = {{"notches", "notches"}, {"center", "center"},     {"spread", "spread"},
                              {"blend", "blend"},     {"rate", "lfo_freq"},     {"duty", "lfo_duty"},
                              {"phaseSpin", "phase"}, {"amount", "amount"},     {"feedback", "feedback"},
                              {"warmth", "warmth"},   {"output", "output"},     {"mix", "mix"}};
// The extra section's (More) and the delay modes' knobs.
constexpr Control kMoreKnobs[] = {{"lfo2Mix", "lfo2_mix"},     {"rate2", "lfo2_freq"},
                                  {"safeBass", "safe_bass"},   {"envAmount", "env_amount"},
                                  {"envAttack", "env_attack"}, {"envRelease", "env_release"}};
// Every switch and button: its object name and parameter.
constexpr Control kButtons[] = {{"sync", "lfo_sync"},   {"spinOn", "spin_on"}, {"fbInvert", "fb_invert"},
                                {"sync2", "lfo2_sync"}, {"envOn", "env_on"}};

}  // namespace

class TestUiDeviceEditorsPhaser : public QObject, public sub::app::test::EditorHarness {
    Q_OBJECT

    QString track_, device_;

    // A track playing a 440 Hz tone (at -6 dB) for `seconds` through a Phaser-Flanger, its editor shown.
    QQuickItem* showPhaser(int seconds = 1) {
        track_ = audioTrackWith(tone(440.0, seconds * kSampleRate), QStringLiteral("tone"), seconds);
        if (track_.isEmpty())
            return nullptr;
        device_ = editor()->addDevice(track_, QStringLiteral("phaser"));
        return show(QStringLiteral("phaser"), track_, device_);
    }

    double value(const char* id) { return param(track_, device_, QString::fromLatin1(id)); }
    void set(const char* id, double v) { editor()->setDeviceParam(track_, device_, QString::fromLatin1(id), v); }

    // An EditorKnob's (or the Env cell's) ParamKnob, its dial, and the parameter it is bound to.
    static QQuickItem* paramKnobOf(QQuickItem* cell) {
        return cell ? qvariant_cast<QQuickItem*>(cell->property("knob")) : nullptr;
    }
    static KnobItem* knobOf(QQuickItem* cell) {
        QQuickItem* paramKnob = paramKnobOf(cell);
        return paramKnob ? qvariant_cast<KnobItem*>(paramKnob->property("knob")) : nullptr;
    }
    static QString paramIdOf(QQuickItem* item) {
        QQuickItem* bound = paramKnobOf(item) ? paramKnobOf(item) : item;
        auto* p = bound ? qvariant_cast<sub::ui::DeviceParam*>(bound->property("param")) : nullptr;
        return p ? p->paramId() : QString();
    }
    // A ParamButton's (or ParamChoice's) RoleButton, to click.
    static QQuickItem* buttonOf(QQuickItem* item) {
        return item ? qvariant_cast<QQuickItem*>(item->property("button")) : nullptr;
    }
    // An EditorKnob's caption and readout texts.
    static QString captionOf(QQuickItem* cell) {
        return cell && !cell->childItems().isEmpty() ? cell->childItems().first()->property("text").toString()
                                                    : QString();
    }
    static QString readoutOf(QQuickItem* cell) {
        return cell && cell->childItems().size() >= 3 ? cell->childItems().at(2)->property("text").toString()
                                                      : QString();
    }

    // Where an item is in the editor.
    static QRectF rectOf(QQuickItem* view, QQuickItem* item) {
        return item->mapRectToItem(view, QRectF(0, 0, item->width(), item->height()));
    }
    // Shown: visible, and not faded out (dimmed is shown).
    static bool shown(QQuickItem* item) {
        if (!item->isVisible())
            return false;
        for (QQuickItem* at = item; at; at = at->parentItem()) {
            if (at->opacity() <= 0.0)
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

    // A Text in `font`, to measure texts as a caption or a readout lays them out (its width: the advance, and
    // whatever of the last glyph reaches past it).
    std::unique_ptr<QQuickItem> textProbe(const QFont& font) {
        QQmlComponent component(qml_.get());
        component.setData("import QtQuick\nText {}", QUrl());
        std::unique_ptr<QQuickItem> probe(qobject_cast<QQuickItem*>(component.create()));
        if (probe)
            probe->setProperty("font", font);
        return probe;
    }
    static double textWidth(QQuickItem* probe, const QString& text) {
        probe->setProperty("text", text);
        return probe->implicitWidth();
    }
    // The font's widest figure (figures may be proportional).
    static QString widestFigure(const QFont& font) {
        const QFontMetricsF metrics(font);
        QString widest = QStringLiteral("0");
        for (const QChar digit : QStringLiteral("123456789")) {
            if (metrics.horizontalAdvance(QString(digit)) > metrics.horizontalAdvance(widest))
                widest = QString(digit);
        }
        return widest;
    }
    // Rounded up to even pixels (as the editor's cells are, so that a knob centres in them on whole ones).
    static double even(double width) { return 2.0 * std::ceil(width / 2.0); }
    // The widest of some texts as measured: `value` of the texts themselves, `form` of their forms (each figure
    // the font's widest `figure`: as wide as any text of that form can be).
    struct Widest {
        double value = 0.0, form = 0.0;

        void add(double width) {
            value = std::max(value, width);
            form = std::max(form, width);
        }
        template <typename Measure>
        void add(const QStringList& texts, const QString& figure, Measure measure) {
            for (QString text : texts) {
                value = std::max(value, measure(text));
                for (QChar& c : text) {
                    if (c.isDigit())
                        c = figure.at(0);
                }
                form = std::max(form, measure(text));
            }
        }
        // `width` (even, as the editor's cells are) between what the values need and what their forms do, the
        // house's `least` at least.
        bool fits(double width, double least) const {
            return width == even(width) && width >= even(std::max(least, value))
                   && width <= even(std::max(least, form));
        }
    };

    // The values over a parameter's range, sampled finely enough to meet every text it reads as (evenly in log
    // for a log-scaled parameter).
    static std::vector<double> valuesOver(const sub::ui::DeviceParam* p) {
        std::vector<double> values;
        constexpr int kCount = 20000;
        for (int i = 0; i <= kCount; ++i) {
            const double t = double(i) / kCount;
            values.push_back(p->logScale() ? p->minimum() * std::pow(p->maximum() / p->minimum(), t)
                                           : p->minimum() + t * (p->maximum() - p->minimum()));
        }
        return values;
    }

    // A knob's caption whole, and every value its readout can show (its parameter's whole range, through the
    // knob's own formatter where it has one); both centred on the knob. `widest` takes them in (a list's names
    // as they are: the editor names them).
    void readoutFits(QQuickItem* cell, Widest& widest) {
        QVERIFY(cell && cell->childItems().size() >= 3);
        QQuickItem* caption = cell->childItems().at(0);
        QQuickItem* readout = cell->childItems().at(2);
        QVERIFY2(caption->implicitWidth() <= caption->width(), qPrintable(caption->property("text").toString()));
        auto* p = qvariant_cast<sub::ui::DeviceParam*>(paramKnobOf(cell)->property("param"));
        QVERIFY(p);
        const QJSValue formatter = cell->property("formatter").value<QJSValue>();
        const QFont font = readout->property("font").value<QFont>();
        const std::unique_ptr<QQuickItem> probe = textProbe(font);
        QVERIFY(probe);
        QStringList texts;
        for (const double v : valuesOver(p)) {
            const QString text = formatter.isCallable() ? formatter.call({QJSValue(v)}).toString() : p->format(v);
            if (texts.contains(text))
                continue;
            texts << text;
            QVERIFY2(textWidth(probe.get(), text) <= readout->width(),
                     qPrintable(cell->objectName() + QStringLiteral(": ") + text));
        }
        const auto measure = [&](const QString& text) { return textWidth(probe.get(), text); };
        if (p->isList()) {
            for (const QString& text : texts)
                widest.add(measure(text));
        } else {
            widest.add(texts, widestFigure(font), measure);
        }
        const double middle = rectOf(cell, paramKnobOf(cell)).center().x();
        QVERIFY2(std::abs(rectOf(cell, readout).center().x() - middle) < 0.5, qPrintable(cell->objectName()));
        if (cell->property("title").isValid()) {  // (an EditorKnob; Env's cell has its switch there)
            const std::unique_ptr<QQuickItem> captionProbe = textProbe(caption->property("font").value<QFont>());
            QVERIFY(captionProbe);
            widest.add(textWidth(captionProbe.get(), caption->property("text").toString()));
            QVERIFY2(std::abs(rectOf(cell, caption).center().x() - middle) < 0.5, qPrintable(cell->objectName()));
        }
    }

    // Everything shown fits: inside the body, no text cut short or spilling out of its box, no two of `controls`
    // over each other, the switches' texts inside their borders (a pixel each side), a list's clear of its arrow,
    // each ♪ clear of its caption's text and of the dial under it; and the editor as wide as its sections, the
    // last (`last`'s) 8 px from the right.
    void fits(QQuickItem* view, const QStringList& controls, const char* last) {
        QList<QQuickItem*> items;
        collect(view, items);
        const QRectF body(0, 0, view->width(), view->height());
        for (QQuickItem* item : items) {
            if (!shown(item) || item->width() <= 0 || item->height() <= 0)
                continue;
            const QRectF rect = rectOf(view, item);
            QVERIFY2(body.adjusted(-0.5, -0.5, 0.5, 0.5).contains(rect),
                     qPrintable(QStringLiteral("%1 (%2) at %3,%4 %5x%6")
                                    .arg(item->objectName(), QString::fromLatin1(item->metaObject()->className()))
                                    .arg(rect.x())
                                    .arg(rect.y())
                                    .arg(rect.width())
                                    .arg(rect.height())));
            if (item->inherits("QQuickText")) {
                const QString text = item->property("text").toString();
                QVERIFY2(!item->property("truncated").toBool(), qPrintable(text));
                QVERIFY2(item->property("contentWidth").toDouble() <= item->width() + 0.5, qPrintable(text));
            }
        }
        QList<QPair<QString, QRectF>> placed;
        for (const QString& name : controls) {
            QQuickItem* control = find(view, name);
            QVERIFY2(control, qPrintable(name));
            if (shown(control))
                placed.append({name, rectOf(view, control)});
        }
        for (qsizetype i = 0; i < placed.size(); ++i) {
            for (qsizetype j = i + 1; j < placed.size(); ++j) {
                const QRectF overlap = placed[i].second.intersected(placed[j].second);
                QVERIFY2(overlap.width() < 0.01 || overlap.height() < 0.01,
                         qPrintable(placed[i].first + QStringLiteral(" over ") + placed[j].first));
            }
        }
        for (const char* name : {"modePhaser", "modeFlanger", "modeDoubler", "spinOn", "fbInvert", "expandButton",
                                 "envOn", "sync", "sync2"}) {
            QQuickItem* control = find(view, QString::fromLatin1(name));
            QQuickItem* face = buttonOf(control) ? buttonOf(control) : control;
            if (shown(control))
                QVERIFY2(face->property("implicitContentWidth").toDouble() <= face->width() - 2.0, name);
        }
        QQuickItem* wave = buttonOf(find(view, QStringLiteral("wave")));
        QVERIFY(wave->implicitWidth() <= wave->width());
        for (const auto& [knobName, buttonName] : {std::pair{"rate", "sync"}, std::pair{"rate2", "sync2"}}) {
            QQuickItem* cell = find(view, QString::fromLatin1(knobName));
            QQuickItem* sync = find(view, QString::fromLatin1(buttonName));
            if (!shown(sync))
                continue;
            QQuickItem* caption = cell->childItems().at(0);
            const QRectF captionAt = rectOf(view, caption), syncAt = rectOf(view, sync);
            const double textRight = captionAt.center().x() + caption->implicitWidth() / 2;
            QVERIFY2(syncAt.left() >= textRight + 1.0 - 1e-6, buttonName);
            QVERIFY2(!syncAt.intersects(rectOf(view, knobOf(cell))), buttonName);
        }
        QCOMPARE(view->implicitWidth(), rectOf(view, find(view, QString::fromLatin1(last))).right() + 8.0);
    }

    // The sections in line and no wider than they need: the tabs 8 px in, the house's 58 px or the widest's own;
    // the mode's section 8 px after them (two cells, or the delay's Time and its notch: the Phaser's knobs
    // centred in it); then each 10 px after the last: the graph (PhaserGraph's own width), the LFO (two cells
    // `cell` wide, 4 px apart), the globals (three).
    void sectionsInLine(QQuickItem* view, double cell) {
        double tabWidth = 0.0;
        for (const char* tab : {"modePhaser", "modeFlanger", "modeDoubler"}) {
            const QRectF at = rectOf(view, find(view, QString::fromLatin1(tab)));
            QCOMPARE(at.left(), 8.0);
            tabWidth = std::max(tabWidth, find(view, QString::fromLatin1(tab))->implicitWidth());
        }
        const QRectF tab = rectOf(view, find(view, QStringLiteral("modePhaser")));
        QCOMPARE(tab.width(), std::max(58.0, std::ceil(tabWidth)));
        const QRectF mode = rectOf(view, find(view, QStringLiteral("delayKnobs")));  // (the section's width)
        const QRectF phaser = rectOf(view, find(view, QStringLiteral("phaserKnobs")));
        QCOMPARE(mode.left(), tab.right() + 8.0);
        QCOMPARE(phaser.width(), 2 * cell + 4.0);
        QCOMPARE(phaser.center().x(), mode.center().x());
        const QRectF graph = rectOf(view, find(view, QStringLiteral("phaserGraph")));
        QCOMPARE(graph.left(), mode.right() + 10.0);
        QCOMPARE(graph.width(), double(PhaserGraph::kWidth));
        const QRectF lfo = rectOf(view, find(view, QStringLiteral("rate")));
        QCOMPARE(lfo.left(), graph.right() + 10.0);
        QCOMPARE(rectOf(view, find(view, QStringLiteral("duty"))).left(), lfo.left());
        QCOMPARE(rectOf(view, find(view, QStringLiteral("amount"))).left(), lfo.left() + 2 * cell + 4.0 + 10.0);
    }

    // More (LFO 2, the envelope, Safe Bass) open or not: view state the editor keeps (DeviceViews).
    static void setExpanded(QQuickItem* view, bool open) {
        QVERIFY(QMetaObject::invokeMethod(view, "setExpanded", Q_ARG(QVariant, open)));
    }

    void click(QQuickItem* item) {
        QQuickItem* button = buttonOf(item) ? buttonOf(item) : item;
        QVERIFY(button);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(button));
    }

    // Drags a knob up by `pixels` (press, two moves, release): one gesture.
    void dragKnob(KnobItem* knob, int pixels) {
        const QPoint at = centerOf(knob);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at - QPoint(0, pixels / 2));
        dragTo(at - QPoint(0, pixels));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, pixels));
    }

    // Plays `frames` of the song through the engine from `beat`, then ticks the displays' clock.
    void play(double beat, int frames) {
        engine()->renderOffline(beat, frames);
        refreshDisplays();
    }

private Q_SLOTS:
    void initTestCase() {
        if (!haveDisplay())
            QSKIP("Qt Quick's scene-graph items need a display (run under xvfb with QT_QPA_PLATFORM=xcb)");
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

    // It fits the view, and every control is there, bound to its parameter.
    void fitsAndBinds() {
        QQuickItem* view = showPhaser();
        QVERIFY(view);
        QVERIFY2(view->implicitHeight() <= bodyHeight(),
                 qPrintable(QStringLiteral("%1 > %2").arg(view->implicitHeight()).arg(bodyHeight())));
        for (const char* name : {"modePhaser", "modeFlanger", "modeDoubler", "phaserKnobs", "notches", "center",
                                 "spread", "blend", "delayKnobs", "time", "notchReadout", "phaserGraph", "rate", "sync",
                                 "wave", "spinOn", "duty", "phaseSpin", "amount", "feedback", "fbInvert", "mix",
                                 "warmth", "output", "expandButton", "extraSection", "lfo2Mix", "rate2", "sync2",
                                 "safeBass", "envAmount", "envOn", "envAttack", "envRelease"})
            QVERIFY2(find(view, QString::fromLatin1(name)), name);
        QVERIFY(!find(view, QStringLiteral("extraSection"))->isVisible());
        QVERIFY(!find(view, QStringLiteral("delayKnobs"))->isVisible());

        for (const Control& control : kKnobs) {
            QQuickItem* cell = find(view, QString::fromLatin1(control.name));
            QVERIFY2(knobOf(cell), control.name);
            QCOMPARE(paramIdOf(cell), QString::fromLatin1(control.param));
            QCOMPARE(knobOf(cell)->value(), value(control.param));
        }
        for (const Control& control : kMoreKnobs)
            QCOMPARE(paramIdOf(find(view, QString::fromLatin1(control.name))), QString::fromLatin1(control.param));
        for (const Control& control : kButtons)
            QCOMPARE(paramIdOf(find(view, QString::fromLatin1(control.name))), QString::fromLatin1(control.param));
        QCOMPARE(paramIdOf(find(view, QStringLiteral("wave"))), QStringLiteral("lfo_wave"));
        QCOMPARE(paramIdOf(find(view, QStringLiteral("time"))), QStringLiteral("flange_time"));
        for (const char* tab : {"modePhaser", "modeFlanger", "modeDoubler"})
            QCOMPARE(paramIdOf(find(view, QString::fromLatin1(tab))), QStringLiteral("mode"));

        // As the parameters are: whole notches; Center in log; Phase in degrees; Blend unsigned.
        KnobItem* notches = knobOf(find(view, QStringLiteral("notches")));
        QCOMPARE(notches->value(), 4.0);
        QCOMPARE(notches->step(), 1.0);
        KnobItem* center = knobOf(find(view, QStringLiteral("center")));
        QCOMPARE(center->value(), 1000.0);
        QVERIFY(center->logScale());
        QCOMPARE(knobOf(find(view, QStringLiteral("mix")))->value(), 50.0);
        QCOMPARE(readoutOf(find(view, QStringLiteral("phaseSpin"))), QStringLiteral("180°"));
        QCOMPARE(formatValue(180.0, QStringLiteral("°")), QStringLiteral("180°"));
        QCOMPARE(readoutOf(find(view, QStringLiteral("blend"))), QStringLiteral("0.00"));
        QCOMPARE(readoutOf(find(view, QStringLiteral("center"))), QStringLiteral("1.00 kHz"));
        QCOMPARE(captionOf(find(view, QStringLiteral("rate"))), QStringLiteral("Freq"));
        // The house's 34 px knobs; bipolar where the range is about 0 (Duty, Env), Output (-36..+6 dB) not.
        for (const Control& control : kKnobs)
            QCOMPARE(knobOf(find(view, QString::fromLatin1(control.name)))->width(), 34.0);
        QVERIFY(knobOf(find(view, QStringLiteral("duty")))->bipolar());
        QVERIFY(knobOf(find(view, QStringLiteral("envAmount")))->bipolar());
        QVERIFY(!knobOf(find(view, QStringLiteral("output")))->bipolar());
        QVERIFY(find(view, QStringLiteral("modePhaser"))->property("lit").toBool());
        QVERIFY(!find(view, QStringLiteral("modeFlanger"))->property("lit").toBool());
        // The tabs on whole pixels (their borders crisp), filling the height between the margins.
        double tabsEnd = 0.0;
        for (const char* tab : {"modePhaser", "modeFlanger", "modeDoubler"}) {
            QQuickItem* item = find(view, QString::fromLatin1(tab));
            const QRectF at = item->mapRectToItem(view, QRectF(0, 0, item->width(), item->height()));
            QVERIFY2(at.top() == std::round(at.top()) && at.height() == std::round(at.height()), tab);
            tabsEnd = at.bottom();
        }
        QCOMPARE(tabsEnd, view->height() - 6);

        // The knob rows sit inside the body's margins (6 px), the graph fills its height.
        auto* graph = find<PhaserGraph>(view, QStringLiteral("phaserGraph"));
        QVERIFY(graph);
        QCOMPARE(graph->height(), view->height() - 12);
        for (const Control& control : kKnobs) {
            QQuickItem* cell = find(view, QString::fromLatin1(control.name));
            const QRectF at = cell->mapRectToItem(view, QRectF(0, 0, cell->width(), cell->height()));
            QVERIFY2(at.top() >= 6 && at.bottom() <= view->height() - 6, control.name);
        }

        // Whatever the font, everything fits and shows its whole text, and the editor is as wide as its
        // sections: as it opens; with the swapped controls' others (the synced rates, Spin) and More open; in
        // the delay modes. Every caption and every value a readout can show whole.
        const QStringList controls = {
            "modePhaser", "modeFlanger", "modeDoubler", "notches", "center", "spread", "blend", "time",
            "notchReadout", "phaserGraph", "rate", "wave", "spinOn", "duty", "phaseSpin", "amount", "feedback",
            "fbInvert", "expandButton", "warmth", "output", "mix", "lfo2Mix", "rate2", "safeBass", "envAmount",
            "envAttack", "envRelease"};
        fits(view, controls, "mix");
        QVERIFY(!QTest::currentTestFailed());
        Widest widest;  // (the cells' captions and readouts)
        for (const char* name : {"notches", "center", "spread", "blend", "rate", "duty", "phaseSpin", "amount",
                                 "feedback", "warmth", "output", "mix", "lfo2Mix", "rate2", "safeBass", "envAmount",
                                 "envAttack", "envRelease"}) {
            readoutFits(find(view, QString::fromLatin1(name)), widest);
            QVERIFY2(!QTest::currentTestFailed(), name);
        }
        // (a synced rate's caption, centred with its ♪ at the cell's right a pixel clear)
        auto syncedCaptions = [&] {
            for (const auto& [knobName, buttonName] : {std::pair{"rate", "sync"}, std::pair{"rate2", "sync2"}}) {
                QQuickItem* caption = find(view, QString::fromLatin1(knobName))->childItems().at(0);
                const double sync = find(view, QString::fromLatin1(buttonName))->width();
                widest.add(caption->implicitWidth() + 2 * (sync + 1.0));
            }
        };
        syncedCaptions();
        set("lfo_sync", 1);
        set("lfo2_sync", 1);
        set("spin_on", 1);
        QCOMPARE(captionOf(find(view, QStringLiteral("rate"))), QStringLiteral("Rate"));
        QCOMPARE(captionOf(find(view, QStringLiteral("phaseSpin"))), QStringLiteral("Spin"));
        setExpanded(view, true);
        QTRY_COMPARE(find(view, QStringLiteral("extraSection"))->opacity(), 1.0);
        QVERIFY(fitted());
        fits(view, controls, "envRelease");
        QVERIFY(!QTest::currentTestFailed());
        for (const char* name : {"rate", "phaseSpin", "rate2"}) {
            readoutFits(find(view, QString::fromLatin1(name)), widest);
            QVERIFY2(!QTest::currentTestFailed(), name);
        }
        syncedCaptions();
        // (the buttons a cell holds: their text with 2 px either side, the border and a pixel clear)
        for (const char* name : {"spinOn", "fbInvert", "expandButton", "envOn"}) {
            QQuickItem* control = find(view, QString::fromLatin1(name));
            QQuickItem* face = buttonOf(control) ? buttonOf(control) : control;
            widest.add(face->property("implicitContentWidth").toDouble() + 4.0);
        }
        // And no wider than they need, whatever the font (732 px in the default, 906 with More): a knob's cell the
        // house's 52 px or the widest of all that, on even pixels so that the knob centres under them exactly. As
        // the editor measures a readout's values by their forms, each figure the font's widest, it may be wider
        // than the widest value itself where figures are proportional, but never wider than the widest form (where
        // they are all as wide, as in most fonts, the two are the same). The sections in line, 10 px apart.
        const double cell = find(view, QStringLiteral("notches"))->width();
        QVERIFY2(widest.fits(cell, 52.0), qPrintable(QString::number(cell)));
        for (const Control& control : kKnobs)
            QVERIFY2(find(view, QString::fromLatin1(control.name))->width() == cell, control.name);
        for (const Control& control : kMoreKnobs)
            QVERIFY2(find(view, QString::fromLatin1(control.name))->width() == cell, control.name);
        sectionsInLine(view, cell);
        QVERIFY(!QTest::currentTestFailed());
        QCOMPARE(rectOf(view, find(view, QStringLiteral("lfo2Mix"))).left(),
                 rectOf(view, find(view, QStringLiteral("amount"))).left() + 3 * cell + 2 * 4.0 + 10.0);
        setExpanded(view, false);
        QVERIFY(fitted());
        // The delay modes: the Time knob over both delays' ranges, and the Flanger's first notch under it over
        // its range (as the graph names it: 500 / Time Hz).
        QQuickItem* time = find(view, QStringLiteral("time"));
        QQuickItem* notch = find(view, QStringLiteral("notchReadout"));
        set("mode", 1);
        QTRY_VERIFY(!find(view, QStringLiteral("phaserKnobs"))->isVisible());
        QTRY_COMPARE(find(view, QStringLiteral("delayKnobs"))->opacity(), 1.0);
        fits(view, controls, "mix");
        QVERIFY(!QTest::currentTestFailed());
        Widest widestTime;
        readoutFits(time, widestTime);
        QVERIFY(!QTest::currentTestFailed());
        const QFont notchFont = notch->property("font").value<QFont>();
        const std::unique_ptr<QQuickItem> probe = textProbe(notchFont);
        QVERIFY(probe);
        QStringList notchTexts;
        for (const double ms : valuesOver(qvariant_cast<sub::ui::DeviceParam*>(paramKnobOf(time)->property("param")))) {
            const QString text = QStringLiteral("Notch %1").arg(formatValue(500.0 / ms, QStringLiteral("Hz")));
            QVERIFY2(textWidth(probe.get(), text) <= notch->width(), qPrintable(text));
            if (!notchTexts.contains(text))
                notchTexts << text;
        }
        set("mode", 2);
        QCOMPARE(paramIdOf(time), QStringLiteral("doubler_time"));
        readoutFits(time, widestTime);
        QVERIFY(!QTest::currentTestFailed());
        // The Time knob's cell: an EditorKnob's width at its size, or its widest caption or readout; the mode's
        // section two cells, or the Time knob's or its notch's, all on even pixels (the Time knob centred in it).
        QVERIFY2(widestTime.fits(time->width(), knobOf(time)->width() + 16.0),
                 qPrintable(QString::number(time->width())));
        QQuickItem* section = find(view, QStringLiteral("delayKnobs"));
        Widest widestSection;
        widestSection.add(notchTexts, widestFigure(notchFont),
                          [&](const QString& text) { return textWidth(probe.get(), text); });
        QVERIFY2(widestSection.fits(section->width(), std::max(2 * cell + 4.0, time->width())),
                 qPrintable(QString::number(section->width())));
        QCOMPARE(rectOf(view, time).center().x(), rectOf(view, section).center().x());
    }

    // Every knob and switch sets its parameter, one undo step each.
    void editsAreUndoable() {
        QQuickItem* view = showPhaser();
        QVERIFY(view);
        set("notches", 12);
        QCOMPARE(knobOf(find(view, QStringLiteral("notches")))->value(), 12.0);
        undo()->undo();
        QCOMPARE(knobOf(find(view, QStringLiteral("notches")))->value(), 4.0);

        auto dragEach = [&](const Control* controls, size_t count) {
            for (size_t i = 0; i < count; ++i) {
                const Control& control = controls[i];
                KnobItem* knob = knobOf(find(view, QString::fromLatin1(control.name)));
                QVERIFY2(knob && knob->isVisible(), control.name);
                const double before = value(control.param);
                const int steps = undo()->index();
                dragKnob(knob, 40);
                QVERIFY2(value(control.param) != before, control.name);
                QCOMPARE(undo()->index(), steps + 1);
                QCOMPARE(undo()->count(), undo()->index());
                QCOMPARE(knob->value(), value(control.param));
                undo()->undo();
                QCOMPARE(value(control.param), before);
            }
        };
        dragEach(kKnobs, std::size(kKnobs));
        QVERIFY(!QTest::currentTestFailed());
        // The extra section's, shown.
        setExpanded(view, true);
        QTRY_VERIFY(find(view, QStringLiteral("extraSection"))->isVisible());
        QTest::qWait(50);
        dragEach(kMoreKnobs, std::size(kMoreKnobs));
        QVERIFY(!QTest::currentTestFailed());

        // Switches: one step each, undone.
        for (const Control& control : kButtons) {
            const int steps = undo()->index();
            click(find(view, QString::fromLatin1(control.name)));
            QCOMPARE(value(control.param), 1.0);
            QCOMPARE(undo()->index(), steps + 1);
            undo()->undo();
            QCOMPARE(value(control.param), 0.0);
        }
        // The waveform from its list.
        QQuickItem* wave = find(view, QStringLiteral("wave"));
        QVERIFY(QMetaObject::invokeMethod(wave, "choose", Q_ARG(QVariant, 9)));
        QCOMPARE(value("lfo_wave"), 9.0);
        undo()->undo();
        QCOMPARE(value("lfo_wave"), 1.0);

        // The Time knob, in the delay modes.
        set("mode", 1);
        QTRY_VERIFY(find(view, QStringLiteral("delayKnobs"))->isVisible());
        const Control time[] = {{"time", "flange_time"}};
        dragEach(time, 1);
        setExpanded(view, false);
    }

    // The mode tabs: lit while chosen; the mode's controls crossfade; the Time rebinds.
    void modeTabs() {
        QQuickItem* view = showPhaser();
        QVERIFY(view);
        auto* graph = find<PhaserGraph>(view, QStringLiteral("phaserGraph"));
        QQuickItem* phaserKnobs = find(view, QStringLiteral("phaserKnobs"));
        QQuickItem* delayKnobs = find(view, QStringLiteral("delayKnobs"));
        QQuickItem* notchReadout = find(view, QStringLiteral("notchReadout"));
        QQuickItem* time = find(view, QStringLiteral("time"));
        const int steps = undo()->index();

        click(find(view, QStringLiteral("modeFlanger")));
        QCOMPARE(value("mode"), 1.0);
        QVERIFY(find(view, QStringLiteral("modeFlanger"))->property("lit").toBool());
        QVERIFY(!find(view, QStringLiteral("modePhaser"))->property("lit").toBool());
        QTRY_VERIFY(!phaserKnobs->isVisible());
        QVERIFY(delayKnobs->isVisible() && delayKnobs->opacity() == 1.0);
        QCOMPARE(paramIdOf(time), QStringLiteral("flange_time"));
        QCOMPARE(readoutOf(time), QStringLiteral("2.5 ms"));
        QVERIFY(notchReadout->isVisible());
        QCOMPARE(notchReadout->property("text").toString(), QStringLiteral("Notch 200 Hz"));  // 1 / (2 · 2.5 ms)
        QCOMPARE(graph->mode(), 1);
        const QPointF timeAt = time->mapToItem(view, QPointF(0, 0));

        click(find(view, QStringLiteral("modeDoubler")));
        QCOMPARE(value("mode"), 2.0);
        QCOMPARE(paramIdOf(time), QStringLiteral("doubler_time"));
        QCOMPARE(readoutOf(time), QStringLiteral("30 ms"));
        QVERIFY(!notchReadout->isVisible());
        QCOMPARE(time->mapToItem(view, QPointF(0, 0)), timeAt);  // (the knob stays put without the notch under it)
        QCOMPARE(graph->mode(), 2);
        QCOMPARE(undo()->index(), steps + 2);

        undo()->undo();
        undo()->undo();
        QCOMPARE(value("mode"), 0.0);
        QTRY_VERIFY(!delayKnobs->isVisible());
        QVERIFY(phaserKnobs->isVisible());
        QCOMPARE(graph->mode(), 0);
    }

    // Freq/Rate and Phase/Spin swap with their switches; Ø inverts the feedback.
    void swappedControls() {
        QQuickItem* view = showPhaser();
        QVERIFY(view);
        QQuickItem* rate = find(view, QStringLiteral("rate"));
        QQuickItem* phaseSpin = find(view, QStringLiteral("phaseSpin"));

        int steps = undo()->index();
        click(find(view, QStringLiteral("sync")));
        QCOMPARE(value("lfo_sync"), 1.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(paramIdOf(rate), QStringLiteral("lfo_rate"));
        QCOMPARE(knobOf(rate)->step(), 1.0);
        QCOMPARE(knobOf(rate)->to(), 21.0);  // (Live's 22 divisions)
        QCOMPARE(readoutOf(rate), QStringLiteral("1 Bar"));
        QCOMPARE(captionOf(rate), QStringLiteral("Rate"));
        QVERIFY(find(view, QStringLiteral("sync"))->property("lit").toBool());
        // Synced, the wheel steps through the note values, one a notch (an undo step each); free, the
        // knob's own wheel moves it smoothly.
        steps = undo()->index();
        wheel(centerOf(knobOf(rate)), 120);
        QCOMPARE(value("lfo_rate"), 16.0);
        QCOMPARE(readoutOf(rate), QStringLiteral("1.5 Bars"));
        wheel(centerOf(knobOf(rate)), -240);
        QCOMPARE(value("lfo_rate"), 14.0);
        QCOMPARE(readoutOf(rate), QStringLiteral("3/4"));
        for (int eighth = 0; eighth < 8; ++eighth) wheel(centerOf(knobOf(rate)), 15);  // (a fine wheel's)
        QCOMPARE(value("lfo_rate"), 15.0);
        QCOMPARE(undo()->index(), steps + 3);
        undo()->undo();
        undo()->undo();
        undo()->undo();
        QCOMPARE(value("lfo_rate"), 15.0);

        steps = undo()->index();
        click(find(view, QStringLiteral("spinOn")));
        QCOMPARE(value("spin_on"), 1.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(paramIdOf(phaseSpin), QStringLiteral("spin"));
        QCOMPARE(captionOf(phaseSpin), QStringLiteral("Spin"));
        QCOMPARE(readoutOf(phaseSpin), QStringLiteral("10 %"));

        steps = undo()->index();
        click(find(view, QStringLiteral("fbInvert")));
        QCOMPARE(value("fb_invert"), 1.0);
        QCOMPARE(undo()->index(), steps + 1);

        undo()->undo();
        undo()->undo();
        undo()->undo();
        QCOMPARE(paramIdOf(rate), QStringLiteral("lfo_freq"));
        QCOMPARE(knobOf(rate)->step(), 0.0);
        QCOMPARE(readoutOf(rate), QStringLiteral("0.50 Hz"));
        QCOMPARE(knobOf(rate)->to(), 40.0);  // (Live's fastest)
        steps = undo()->index();
        wheel(centerOf(knobOf(rate)), 120);
        QVERIFY(value("lfo_freq") > 0.5);
        QCOMPARE(value("lfo_rate"), 15.0);
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();
        QCOMPARE(paramIdOf(phaseSpin), QStringLiteral("phase"));
        QCOMPARE(value("fb_invert"), 0.0);
    }

    // More shows LFO 2, the envelope and Safe Bass: view state, not an edit, kept per device.
    void expandIsViewState() {
        QQuickItem* view = showPhaser();
        QVERIFY(view);
        QQuickItem* expand = find(view, QStringLiteral("expandButton"));
        QQuickItem* extra = find(view, QStringLiteral("extraSection"));
        const int count = undo()->count();
        const bool clean = undo()->isClean();
        QVERIFY(!expand->property("checked").toBool());
        const double collapsed = view->implicitWidth();

        // Open: as wide again as the section (a gap and three cells).
        click(expand);
        QCOMPARE(view->implicitWidth(), collapsed + extra->width());
        QTRY_VERIFY(extra->isVisible());
        QTRY_COMPARE(extra->opacity(), 1.0);
        QVERIFY(expand->property("checked").toBool());
        QCOMPARE(undo()->count(), count);
        QCOMPARE(undo()->isClean(), clean);  // (the project isn't changed)
        QVERIFY(fitted());
        QTest::qWait(50);
        save(grab(), QStringLiteral("phaser-expanded-off.png"));

        // Shown again: still expanded.
        QMetaObject::invokeMethod(root_.get(), "clear");
        view = show(QStringLiteral("phaser"), track_, device_);
        QVERIFY(view);
        QCOMPARE(view->implicitWidth(), collapsed + find(view, QStringLiteral("extraSection"))->width());
        expand = find(view, QStringLiteral("expandButton"));
        QVERIFY(expand->property("checked").toBool());
        QVERIFY(find(view, QStringLiteral("extraSection"))->isVisible());

        // Env Follow: its knobs light up; the switch itself never dims.
        QQuickItem* envOn = find(view, QStringLiteral("envOn"));
        QQuickItem* envAmount = find(view, QStringLiteral("envAmount"));
        QQuickItem* attack = find(view, QStringLiteral("envAttack"));
        QVERIFY(paramKnobOf(envAmount));
        const QQuickItem* envKnob = paramKnobOf(envAmount)->parentItem();
        QCOMPARE(envKnob->opacity(), 0.55);
        QCOMPARE(attack->opacity(), 0.55);
        click(envOn);
        QCOMPARE(value("env_on"), 1.0);
        QTRY_COMPARE(envKnob->opacity(), 1.0);
        QTRY_COMPARE(attack->opacity(), 1.0);
        QCOMPARE(envOn->opacity(), 1.0);
        // LFO 2's rate is dimmed while it has no share.
        QQuickItem* rate2 = find(view, QStringLiteral("rate2"))->parentItem();
        QCOMPARE(rate2->opacity(), 0.55);
        set("lfo2_mix", 50);
        QTRY_COMPARE(rate2->opacity(), 1.0);
        QTest::qWait(50);
        save(grab(), QStringLiteral("phaser-expanded.png"));

        click(find(view, QStringLiteral("expandButton")));
        QCOMPARE(view->implicitWidth(), collapsed);
        QVERIFY(!find(view, QStringLiteral("expandButton"))->property("checked").toBool());
        QVERIFY(!find(view, QStringLiteral("extraSection"))->isVisible());
    }

    // Not playing, the curve is the engine's design at the parameters; the notches are marked where it puts them.
    void curveIsTheDesign() {
        QQuickItem* view = showPhaser();
        QVERIFY(view);
        auto* graph = find<PhaserGraph>(view, QStringLiteral("phaserGraph"));
        QVERIFY(graph);
        QVERIFY(!graph->live());
        set("feedback", 0);  // (where the notches are, and how deep, without it)
        const double rate = bridge()->sampleRate();
        const double q = phaserQ(50.0);
        PhaserCurve design;
        design.mode = 0;
        design.notches = 4;
        design.centerHz = 1000.0;
        design.q = q;
        design.feedback = 0.0;
        design.mix = 0.5;
        // (the drawn sweep and Q are the parameters', kept in log2: to within a rounding)
        QVERIFY(std::abs(graph->sweepLeft() / 1000.0 - 1.0) < 1e-12 && std::abs(graph->qLeft() / q - 1.0) < 1e-12);
        design.centerHz = graph->sweepLeft();
        design.q = graph->qLeft();
        const PhaserCurvePoints& curve = graph->curveLeft();
        QVERIFY(curve.lineHz.size() > 200);
        QCOMPARE(curve.top.size(), qsizetype(graph->plot().width()));
        for (qsizetype i = 0; i < curve.lineHz.size(); i += 13)
            QCOMPARE(curve.lineDb[i], phaserResponseDb(design, rate, {curve.lineHz[i]})[0]);
        for (qsizetype i = 1; i < curve.lineHz.size(); ++i) QVERIFY(curve.lineHz[i] > curve.lineHz[i - 1]);
        const QList<double> notches = phaserNotchFrequencies(4, 1000.0, q, rate);
        QCOMPARE(graph->notchMarkers().size(), size_t(4));
        for (int k = 0; k < 4; ++k) {
            QVERIFY(std::abs(graph->notchMarkers()[size_t(k)] - notches[k]) < 1e-9);
            // each drawn at its depth
            const auto at = std::find_if(curve.lineHz.begin(), curve.lineHz.end(),
                                         [&](double f) { return std::abs(f - notches[k]) < 1e-9; });
            QVERIFY(at != curve.lineHz.end());
            QVERIFY(curve.lineDb[at - curve.lineHz.begin()] < -60.0);
        }
        QVERIFY(graph->curveRight().lineHz.isEmpty());  // the right is the left's

        set("notches", 8);
        QCOMPARE(graph->notchMarkers().size(), size_t(8));
        set("center", 300.0);
        QVERIFY(std::abs(graph->sweepLeft() - 300.0) < 1e-9);  // at once: it is what the user drags

        // Flanger 1 ms: the comb's notches at 500, 1500 ... Hz, marked while apart enough to tell.
        set("mode", 1);
        set("flange_time", 1.0);
        QVERIFY(std::abs(graph->sweepLeft() - 1.0) < 1e-9);
        const std::vector<double> combs = graph->notchMarkers();
        QCOMPARE(combs.size(), size_t(5));
        for (size_t k = 0; k < combs.size(); ++k) QVERIFY(std::abs(combs[k] - (500.0 + 1000.0 * double(k))) < 1e-9);
        for (const bool dense : graph->curveLeft().dense) QVERIFY(!dense);

        // Doubler 30 ms: the comb is finer than a column from about 1.1 kHz up: drawn as a band.
        set("mode", 2);
        const PhaserCurvePoints& doubler = graph->curveLeft();
        const LogAxis axis = graph->frequencyAxis();
        int denseBelow1k = 0, sparseAbove2k = 0, dense = 0;
        for (qsizetype c = 0; c < doubler.dense.size(); ++c) {
            const double f = axis.valueAt(axis.from + double(c) + 0.5);
            if (doubler.dense[c]) {
                ++dense;
                QVERIFY(doubler.top[c] - doubler.bottom[c] > 20.0);  // peaks and notches both
            }
            if (f < 1000.0 && doubler.dense[c])
                ++denseBelow1k;
            if (f > 2000.0 && !doubler.dense[c])
                ++sparseAbove2k;
        }
        QVERIFY(dense > 50);
        QCOMPARE(denseBelow1k, 0);
        QCOMPARE(sparseAbove2k, 0);
        QTRY_VERIFY(!find(view, QStringLiteral("phaserKnobs"))->isVisible());  // (the mode's controls crossfaded)
        QTest::qWait(50);
        save(grab(), QStringLiteral("phaser-doubler-idle.png"));
    }

    // Dragging the graph: across and up, one undo step per drag; a double-click resets.
    void graphDragIsOneStep() {
        QQuickItem* view = showPhaser();
        QVERIFY(view);
        auto* graph = find<PhaserGraph>(view, QStringLiteral("phaserGraph"));
        QVERIFY(graph);
        const QRectF plot = graph->plot();
        auto drag = [&](QPoint at, int up) {
            QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
            for (int dy = up / 3; dy <= up; dy += up / 3) dragTo(at - QPoint(0, dy));
            QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, up));
        };

        // Phaser: across for the Center, up for the Spread.
        int steps = undo()->index();
        drag(scenePoint(graph, QPointF(graph->xOf(250.0), plot.center().y())), 60);
        QVERIFY2(std::abs(value("center") / 250.0 - 1.0) < 0.02, qPrintable(QString::number(value("center"))));
        QVERIFY2(std::abs(value("spread") - 90.0) < 0.5, qPrintable(QString::number(value("spread"))));
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(undo()->count(), undo()->index());
        QVERIFY(std::abs(graph->sweepLeft() - value("center")) < 1e-6);

        // A double-click: back to the defaults, one step with its first click's jump to where it was
        // clicked, so one undo goes back to what was there before.
        const QPoint somewhere = scenePoint(graph, QPointF(graph->xOf(3000.0), plot.center().y()));
        QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
        steps = undo()->index();
        const double centerBefore = value("center"), spreadBefore = value("spread");
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, somewhere);
        QCOMPARE(value("center"), 1000.0);
        QCOMPARE(value("spread"), 50.0);
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();
        QCOMPARE(value("center"), centerBefore);
        QCOMPARE(value("spread"), spreadBefore);
        undo()->redo();
        QCOMPARE(value("center"), 1000.0);

        // The drag's cursor over the plot only: the strip and the meters take no drag.
        QTest::mouseMove(window_, scenePoint(graph, plot.center()));
        QTRY_COMPARE(graph->cursor().shape(), Qt::SizeAllCursor);
        QTest::mouseMove(window_, scenePoint(graph, graph->strip().center()));
        QTRY_VERIFY(graph->cursor().shape() != Qt::SizeAllCursor);
        QTest::mouseMove(window_, scenePoint(graph, graph->meters().center()));
        QTRY_VERIFY(graph->cursor().shape() != Qt::SizeAllCursor);

        // A press in the LFO strip changes nothing (it goes to the frame).
        steps = undo()->index();
        const QPoint inStrip = scenePoint(graph, graph->strip().center());
        drag(inStrip, 30);
        QCOMPARE(undo()->index(), steps);
        QCOMPARE(value("center"), 1000.0);

        // Flanger: the comb's first notch under the mouse, up for the Feedback.
        set("mode", 1);
        steps = undo()->index();
        const double press = 100.0;  // Hz: a 5 ms comb's first notch
        QVERIFY(std::abs(graph->xOfTime(5.0) - graph->xOf(press)) < 1e-9);
        drag(scenePoint(graph, QPointF(graph->xOfTime(5.0), plot.center().y())), 30);
        QVERIFY2(std::abs(value("flange_time") / 5.0 - 1.0) < 0.03, qPrintable(QString::number(value("flange_time"))));
        QVERIFY2(std::abs(value("feedback") - 70.0) < 0.5, qPrintable(QString::number(value("feedback"))));
        QCOMPARE(undo()->index(), steps + 1);
        QVERIFY(!graph->notchMarkers().empty());
        QVERIFY2(std::abs(graph->notchMarkers().front() / press - 1.0) < 0.03,
                 qPrintable(QString::number(graph->notchMarkers().front())));

        // Doubler: log across 20..150 ms, the longest at the left.
        set("mode", 2);
        steps = undo()->index();
        drag(scenePoint(graph, QPointF(graph->xOfTime(60.0), plot.center().y())), 15);
        QVERIFY2(std::abs(value("doubler_time") / 60.0 - 1.0) < 0.03,
                 qPrintable(QString::number(value("doubler_time"))));
        QCOMPARE(undo()->index(), steps + 1);
        QVERIFY(graph->xOfTime(150.0) < graph->xOfTime(20.0));
        const QPoint doublerAt = scenePoint(graph, QPointF(graph->xOfTime(40.0), plot.center().y()));
        QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
        steps = undo()->index();
        const double timeBefore = value("doubler_time"), feedbackBefore = value("feedback");
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, doublerAt);
        QCOMPARE(value("doubler_time"), 30.0);
        QCOMPARE(value("feedback"), 50.0);
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();
        QCOMPARE(value("doubler_time"), timeBefore);
        QCOMPARE(value("feedback"), feedbackBefore);
    }

    // Playing, the graph draws what the engine publishes: the sweep, the LFO's phase and value, the levels.
    void displaysReachTheGraph() {
        QQuickItem* view = showPhaser();
        QVERIFY(view);
        auto* graph = find<PhaserGraph>(view, QStringLiteral("phaserGraph"));
        QVERIFY(graph);
        set("amount", 100);
        set("lfo_wave", 0);  // Sine
        set("lfo_freq", 2.0);
        refreshDisplays();
        QVERIFY(!graph->live());

        // The engine has what the editor set.
        const auto id = bridge()->engineDeviceId(track_, device_);
        QVERIFY(id);
        for (const char* p : {"amount", "lfo_wave", "lfo_freq", "notches", "center", "mix"})
            QCOMPARE(engine()->processorParam(*id, engine()->processorParamIndex(*id, p)), float(value(p)));

        play(0.0, kSampleRate / 2);
        QVERIFY(graph->live());
        // (the 93rd value: 23 808 frames in, at 2 Hz)
        QVERIFY2(std::abs(graph->lfoPhase() - 0.992) < 0.01, qPrintable(QString::number(graph->lfoPhase())));
        QVERIFY2(std::abs(graph->lfoValue() - std::sin(2 * kPi * graph->lfoPhase())) < 1e-3,
                 qPrintable(QString::number(graph->lfoValue())));
        QVERIFY(graph->sweepLeft() >= 125.0 && graph->sweepLeft() <= 8000.0);
        QVERIFY2(std::abs(graph->levelIn() - 20 * std::log10(0.5)) < 0.5,
                 qPrintable(QString::number(graph->levelIn())));
        QVERIFY(graph->levelOut() > -60.0);
        // A few ticks on (no more values: the playhead waits at the newest), the drawn sweep is the engine's.
        // (Ticked by hand, each counts as a display tick, 16 ms: a busy machine can't make them stale.)
        for (int i = 0; i < 8; ++i) refreshDisplays();
        QVERIFY(graph->live());
        const double rate = bridge()->sampleRate();
        const double expected = phaserCenterHz(1000.0, 0.0, graph->modulation(), rate);
        QVERIFY2(std::abs(std::log2(graph->sweepLeft() / expected)) < 0.02,
                 qPrintable(QStringLiteral("%1 %2").arg(graph->sweepLeft()).arg(expected)));
        QVERIFY(std::abs(graph->modulation() - graph->lfoValue()) < 0.1);  // Amount 100 %: the LFO itself
        PhaserCurve design = graph->curveSettings();
        QCOMPARE(design.centerHz, graph->sweepLeft());
        QCOMPARE(design.q, graph->qLeft());
        const PhaserCurvePoints& curve = graph->curveLeft();
        for (qsizetype i = 0; i < curve.lineHz.size(); i += 11)
            QVERIFY(std::abs(curve.lineDb[i] - phaserResponseDb(design, rate, {curve.lineHz[i]})[0]) < 1e-9);
        // Phase 180: the right channel's sweep mirrors the left's, its curve drawn too.
        QVERIFY2(std::abs(std::log2(graph->sweepLeft() * graph->sweepRight() / 1e6)) < 0.05,
                 qPrintable(QString::number(graph->sweepRight())));
        QVERIFY(!graph->curveRight().lineHz.isEmpty());
        QVERIFY(graph->dotOpacity() > 0.35);
    }

    // Without display values the graph goes quiet: the curve back where the parameters put it, then no repaints.
    void goesQuiet() {
        QQuickItem* view = showPhaser();
        QVERIFY(view);
        auto* graph = find<PhaserGraph>(view, QStringLiteral("phaserGraph"));
        set("amount", 100);
        set("lfo_wave", 0);
        set("lfo_freq", 2.0);
        play(0.0, kSampleRate / 2);
        QVERIFY(graph->live());
        QTest::qWait(400);
        for (int i = 0; i < 60; ++i) refreshDisplays();
        QVERIFY(!graph->live());
        QVERIFY2(std::abs(graph->sweepLeft() / 1000.0 - 1.0) < 0.01, qPrintable(QString::number(graph->sweepLeft())));
        QVERIFY(graph->dotOpacity() < 0.5);
        // Once the meters have fallen, nothing moves: no more repaints.
        for (int i = 0; i < 600 && graph->moving(); ++i) refreshDisplays();
        QVERIFY(!graph->moving());
        QCOMPARE(graph->levelIn(), -60.0);
        refreshDisplays();
        QVERIFY(!graph->moving());
        // An edit is drawn at once.
        set("center", 2000.0);
        QVERIFY(std::abs(graph->sweepLeft() - 2000.0) < 1e-9);
        refreshDisplays();
        QVERIFY(!graph->moving());
    }

    // The meters show the sound now: shown again after a loud part, the device silent since, they don't light
    // up from the display's history (a read then brings seconds of it).
    void metersShowNow() {
        QQuickItem* view = showPhaser(4);
        QVERIFY(view);
        auto* graph = find<PhaserGraph>(view, QStringLiteral("phaserGraph"));
        QVERIFY(graph);
        for (int i = 0; i < 30; ++i) {
            play(10.0, 768);  // (silence: after the tone's clip)
            QVERIFY2(graph->levelIn() < -40.0, qPrintable(QString::number(graph->levelIn())));
        }
        QVERIFY(graph->live());

        // Hidden through a loud part, silent again, then shown.
        view->setVisible(false);
        engine()->renderOffline(0.0, 2 * kSampleRate);
        engine()->renderOffline(10.0, 3 * kSampleRate);
        view->setVisible(true);
        play(10.0, 768);
        QVERIFY2(graph->levelIn() < -40.0, qPrintable(QString::number(graph->levelIn())));
        QVERIFY2(graph->levelOut() < -40.0, qPrintable(QString::number(graph->levelOut())));

        // Sound now: the meters show it.
        play(0.0, 4096);
        QVERIFY2(std::abs(graph->levelIn() - 20 * std::log10(0.5)) < 0.5,
                 qPrintable(QString::number(graph->levelIn())));

        // A large block's values all count, not only the newest the tick's time covers: one of 2048 frames, the
        // tone's last 1024 then silence, shows the tone (its four silent values alone would cover a tick).
        for (int i = 0; i < 120; ++i) play(10.0, 768);  // (the meter falling 24 dB a second)
        QVERIFY2(graph->levelIn() < -40.0, qPrintable(QString::number(graph->levelIn())));
        refreshDisplays();  // (so the tick below counts a display tick's time, too short for the older values)
        play(double(4 * kSampleRate - 1024) / (kSampleRate / 2), 2048);
        QVERIFY2(std::abs(graph->levelIn() - 20 * std::log10(0.5)) < 1.0,
                 qPrintable(QString::number(graph->levelIn())));
    }

    // The display values played back smoothly, whatever batches they come in.
    void displayPlaybackIsSmooth() {
        using Playback = DisplayPlayback<1>;
        Playback playback;
        const double fps = 48000.0 / 256.0;
        const double tick = 1.0 / 60.0, block = 1024.0 / 48000.0;
        auto frameOf = [](double v) { return Playback::Frame{float(v)}; };
        int appended = 0;
        double nextBlock = 0.0, last = -1.0;
        for (int t = 0; t < 120; ++t) {  // 2 s
            const double now = t * tick;
            int batch = 0;
            for (; nextBlock <= now; nextBlock += block) {
                for (int k = 0; k < 4; ++k) playback.append(frameOf(appended++));
                batch += 4;
            }
            playback.endBatch(batch);
            playback.advance(tick, fps);
            const double v = playback.value(0);
            if (now > 0.5) {
                QVERIFY2(std::abs((v - last) - fps * tick) < 1.0,
                         qPrintable(QStringLiteral("%1 at %2").arg(v - last).arg(now)));
                QVERIFY2(playback.lag() >= 1.0 && playback.lag() <= 9.0, qPrintable(QString::number(playback.lag())));
            }
            last = v;
        }
        QCOMPARE(playback.target(), 4);

        // A phase wrapping between two frames: through 1, not back through 0.5.
        Playback wrap;
        const Playback::Frame a = frameOf(0.98), b = frameOf(0.02);
        wrap.append(a);
        wrap.append(b);
        wrap.endBatch(2);
        // (Half a frame's time: it starts again the target, 2, behind the newest (held at 0), and moves
        // on at 0.925 of its pace, nearer the newest than the target: 0.4625 of a frame.)
        wrap.advance(0.5 / fps, fps);
        QCOMPARE(wrap.head(), 0.4625);
        const double expected = 0.98 + 0.04 * 0.4625;
        QVERIFY2(std::abs(wrap.phase(0) - expected) < 1e-6, qPrintable(QString::number(wrap.phase(0))));

        // Appends stopping: it holds at the newest.
        for (int i = 0; i < 60; ++i) {
            playback.endBatch(0);
            playback.advance(tick, fps);
        }
        QCOMPARE(playback.head(), double(playback.newest()));
        QCOMPARE(playback.value(0), double(appended - 1));
        // A batch after a gap of a second: from the newest less the target lag.
        for (int i = 0; i < 30; ++i) playback.append(frameOf(appended++));
        playback.endBatch(30);
        playback.advance(0.0, fps);
        QCOMPARE(playback.target(), Playback::kMaxTarget);
        QCOMPARE(playback.lag(), double(Playback::kMaxTarget));
        // Far behind (a block bigger than it keeps): it jumps up.
        for (int i = 0; i < 100; ++i) playback.append(frameOf(appended++));
        playback.endBatch(100);
        playback.advance(tick, fps);
        QVERIFY(playback.lag() <= Playback::kMaxTarget);
        QVERIFY(playback.value(0) >= appended - 1 - Playback::kMaxTarget);
    }

    // What a curve costs (worked out again each tick while the sweep moves, twice in stereo): well under a
    // millisecond, also with 42 notches or a fine comb. In the thread's CPU time, in an optimized build.
    void curveCost() {
#ifndef NDEBUG
        QSKIP("timing needs an optimized build");
#else
        PhaserCurve curve;
        curve.feedback = phaserFeedbackGain(60.0, false);
        const int columns = int(PhaserGraph::kWidth - 2 - 3 * PhaserGraph::kMeterWidth);
        for (const auto& [mode, notches, label] :
             {std::tuple{0, 6, "Phaser, 6 notches"}, std::tuple{0, 42, "Phaser, 42 notches"},
              std::tuple{1, 4, "Flanger, 2.5 ms"}, std::tuple{2, 4, "Doubler, 30 ms"}}) {
            curve.mode = mode;
            curve.notches = notches;
            curve.delayMs = mode == 2 ? 30.0 : 2.5;
            constexpr int kRuns = 50;
            const double start = threadSeconds();
            for (int i = 0; i < kRuns; ++i) {
                curve.centerHz = 400.0 + 10.0 * i;  // (a moving sweep)
                const PhaserCurvePoints points =
                    phaserCurvePoints(curve, PhaserGraph::kLow, PhaserGraph::kHigh, columns, kSampleRate);
                QVERIFY(!points.lineHz.isEmpty());
            }
            const double us = (threadSeconds() - start) / kRuns * 1e6;
            QVERIFY2(us < 2000.0, qPrintable(QStringLiteral("a curve (%1): %2 us").arg(QLatin1String(label)).arg(us)));
        }
#endif
    }

    // The graph's animation keeps its pace whatever the audio's block size and the LFO's rate: with
    // 2048-frame blocks (a tick in two bringing nothing) the meters still fall 24 dB a second; a fast
    // LFO's comet tail follows the way it went; a random shape's trace starts afresh when chosen.
    void animationKeepsPace() {
        QQuickItem* view = showPhaser(1);
        QVERIFY(view);
        auto* graph = find<PhaserGraph>(view, QStringLiteral("phaserGraph"));
        QVERIFY(graph);
        set("lfo_sync", 1);
        set("lfo_rate", 4);  // "1/16": 8 Hz at 120 BPM
        set("lfo_wave", 0);
        set("amount", 100);
        // Played as the audio thread would: a block whenever the wall clock makes one due, the displays
        // ticking every 16 ms, from `beat` on.
        double beat = 0.0;
        auto playLive = [&](double seconds, int block) {
            QElapsedTimer clock;
            clock.start();
            const double blockSeconds = double(block) / kSampleRate;
            qint64 blocks = 0;
            while (clock.elapsed() < qint64(seconds * 1000.0)) {
                for (; double(blocks) * blockSeconds <= double(clock.elapsed()) / 1000.0; ++blocks) {
                    engine()->renderOffline(beat, block);
                    beat += blockSeconds * 2.0;  // (120 BPM)
                }
                refreshDisplays();
                QTest::qWait(16);
            }
        };
        playLive(0.5, 2048);
        QVERIFY(graph->live());
        QVERIFY2(std::abs(graph->levelIn() - 20 * std::log10(0.5)) < 1.0,
                 qPrintable(QString::number(graph->levelIn())));
        // 8 Hz: more than half a cycle over the trail's ten ticks (the way the dot went: the phase apart from the
        // oldest's would read that as less than it is, or as nothing).
        QVERIFY2(graph->trailSpan() > 0.5, qPrintable(QString::number(graph->trailSpan())));

        // The tone over (it ends at beat 2): the input falls at its pace, though some ticks bring nothing.
        beat = 2.5;
        playLive(1.0, 2048);
        QVERIFY(graph->live());
        QVERIFY2(graph->levelIn() < -6.0 - 18.0, qPrintable(QString::number(graph->levelIn())));

        // Random S&H: its trace fills from the values that come; chosen again after another shape, it starts afresh.
        set("lfo_wave", 9);
        QCOMPARE(graph->traceLength(), 0);
        playLive(0.3, 1024);
        const int traced = graph->traceLength();
        QVERIFY2(traced > 0 && traced < 100, qPrintable(QString::number(traced)));  // (0.3 s: 56 values)
        set("lfo_wave", 0);
        playLive(0.1, 1024);
        set("lfo_wave", 9);
        QCOMPARE(graph->traceLength(), 0);
    }

    // What it looks like: playing (the curve and the LFO moving), the Flanger, the Doubler's band.
    void screenshots() {
        QQuickItem* view = showPhaser(8);
        QVERIFY(view);
        auto* graph = find<PhaserGraph>(view, QStringLiteral("phaserGraph"));
        QTest::qWait(50);
        save(grab(), QStringLiteral("phaser-idle.png"));

        // Synced (so the LFO carries on from one render to the next), played a block at a time.
        set("lfo_sync", 1);
        set("lfo_rate", 13);  // "1/2": a cycle a second at 120 BPM
        set("amount", 70);
        set("feedback", 60);
        set("notches", 6);
        set("spread", 60);
        const double beatsPerBlock = 1024.0 / kSampleRate * 2.0;
        auto playFor = [&](int blocks, double& beat) {
            for (int i = 0; i < blocks; ++i, beat += beatsPerBlock) {
                engine()->renderOffline(beat, 1024);
                refreshDisplays();
                QTest::qWait(16);
            }
        };
        double beat = 0.3;
        playFor(30, beat);
        QVERIFY(graph->live());
        save(grab(), QStringLiteral("phaser.png"));

        set("mode", 1);
        set("flange_time", 1.2);
        set("feedback", 70);
        playFor(30, beat);
        save(grab(), QStringLiteral("phaser-flanger.png"));

        set("mode", 2);
        set("lfo_wave", 9);  // Random S&H: a trace
        set("lfo_rate", 9);  // 1/4
        playFor(40, beat);
        save(grab(), QStringLiteral("phaser-doubler.png"));

        // Expanded, the envelope following and LFO 2 mixed in: the envelope's bar, LFO 2's share.
        set("mode", 0);
        set("lfo_wave", 2);  // Triangle Analog
        set("env_on", 1);
        set("env_amount", 60);
        set("lfo2_mix", 40);
        set("feedback", 30);
        setExpanded(view, true);
        QVERIFY(fitted());
        playFor(30, beat);
        QVERIFY(graph->envelope() > 0.5);  // (a 0.5 tone: -6 dBFS, 0.875 of the follower's range)
        save(grab(), QStringLiteral("phaser-expanded-playing.png"));
        setExpanded(view, false);
    }
};

QTEST_MAIN(TestUiDeviceEditorsPhaser)
#include "test_ui_device_editors_phaser.moc"
