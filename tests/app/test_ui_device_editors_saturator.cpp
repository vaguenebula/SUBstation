// The Saturator's editor (SaturatorEditor.qml, SaturatorCurve, SaturatorColorGraph),
// loaded as the device view loads it, with a real session over an engine:
// its controls bound to their parameters (undoable, the engine following), the
// curve and Color's EQ being the engine's own maths, the graphs' drags one undo
// step each (showing the automation of what they move most), and the displays
// reaching the graphs (rendering offline), holding over the gaps between
// blocks and letting them settle; it fits, every list and readout whole. With
// SUBSTATION_UI_SCREENSHOTS set to a folder, the editor is saved there as PNGs,
// with signal flowing, in four of its modes.

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QQuickItem>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTest>
#include <QUndoStack>
#include <QtQuickTest/quicktest.h>

#include <cmath>
#include <map>
#include <vector>

#include "EditorHarness.h"
#include "audio/SaturatorResponse.h"
#include "builtin/SaturatorDesign.h"
#include "controls/KnobItem.h"
#include "devices/SaturatorColorGraph.h"
#include "devices/SaturatorCurve.h"
#include "model/Automation.h"

using namespace sub::app;
using namespace sub::ui;
using sub::app::test::kSampleRate;

namespace {

const QStringList kKnobs = {QStringLiteral("drive"),    QStringLiteral("output"),   QStringLiteral("mix"),
                            QStringLiteral("base"),     QStringLiteral("freq"),     QStringLiteral("width"),
                            QStringLiteral("depth"),    QStringLiteral("threshold"), QStringLiteral("ws_drive"),
                            QStringLiteral("ws_lin"),   QStringLiteral("ws_curve"), QStringLiteral("ws_damp"),
                            QStringLiteral("ws_depth"), QStringLiteral("ws_period")};
const QStringList kColorKnobs = {QStringLiteral("base"), QStringLiteral("freq"), QStringLiteral("width"),
                                 QStringLiteral("depth")};
const QStringList kShaperKnobs = {QStringLiteral("ws_drive"), QStringLiteral("ws_lin"),   QStringLiteral("ws_curve"),
                                  QStringLiteral("ws_damp"),  QStringLiteral("ws_depth"), QStringLiteral("ws_period")};

// A tone whose level falls from `loud` to a tenth of it every `beat` seconds: something for the
// curve's dots and afterglow to follow.
std::vector<float> hits(double freq, int frames, double loud, double beat) {
    std::vector<float> samples(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const double t = double(i) / kSampleRate;
        const double envelope = loud * std::exp(-std::fmod(t, beat) / (beat / 2.3));
        samples[size_t(i)] = float(envelope * std::sin(2 * 3.14159265358979323846 * freq * t));
    }
    return samples;
}

}  // namespace

class TestUiSaturator : public QObject, public sub::app::test::EditorHarness {
    Q_OBJECT

    // What a test works on: a track playing a tone, a Saturator on it, its editor.
    struct Shown {
        QString track, device;
        QQuickItem* view = nullptr;
        SaturatorCurve* curve = nullptr;
        SaturatorColorGraph* color = nullptr;
    };

    Shown showSaturator(const std::vector<float>& signal) {
        Shown s;
        s.track = audioTrackWith(signal, QStringLiteral("tone"), double(signal.size()) / kSampleRate);
        if (s.track.isEmpty())
            return s;
        s.device = editor()->addDevice(s.track, QStringLiteral("saturator"));
        s.view = show(QStringLiteral("saturator"), s.track, s.device);
        if (s.view) {
            s.curve = find<SaturatorCurve>(s.view, QStringLiteral("saturatorCurve"));
            s.color = find<SaturatorColorGraph>(s.view, QStringLiteral("saturatorColor"));
        }
        return s;
    }
    Shown showSaturator() { return showSaturator(tone(1000.0, kSampleRate, 0.5)); }

    // An EditorKnob's dial (EditorKnob -> its ParamKnob -> the KnobItem).
    KnobItem* knobOf(QQuickItem* view, const QString& name) {
        QQuickItem* cell = find(view, name);
        auto* paramKnob = cell ? qvariant_cast<QQuickItem*>(cell->property("knob")) : nullptr;
        return paramKnob ? qvariant_cast<KnobItem*>(paramKnob->property("knob")) : nullptr;
    }

    // A ParamButton's or ParamChoice's clickable face.
    static QQuickItem* buttonOf(QQuickItem* control) {
        return control ? qvariant_cast<QQuickItem*>(control->property("button")) : nullptr;
    }

    // The parameter as the engine's processor has it.
    float engineParam(const Shown& s, const char* id) {
        const auto processor = bridge()->engineDeviceId(s.track, s.device);
        if (!processor)
            return -999.f;
        return engine_->processorParam(*processor, engine_->processorParamIndex(*processor, id));
    }

    // Renders `seconds` of the song from `from` (seconds) offline in pieces, the displays' clock
    // ticking after each (as the view refreshes while it plays), `wait` ms of real time between them.
    void play(double from, double seconds, int pieces = 1, int wait = 0) {
        const int frames = int(seconds * kSampleRate / pieces);
        for (int i = 0; i < pieces; ++i) {
            const double at = from + double(i) * frames / kSampleRate;
            engine_->renderOffline(at * project()->tempo() / 60.0, frames);
            refreshDisplays();
            if (wait > 0)
                QTest::qWait(wait);
        }
    }

private Q_SLOTS:
    void initTestCase() {
        if (!haveDisplay())
            QSKIP("needs a display: the offscreen platform renders Qt Quick in software, without this geometry");
        startHost();
    }

    void cleanupTestCase() { stopHost(); }

    void init() { clearHost(); }

    // --- It fits, and every control is there, bound -----------------------------------------

    void fitsAndBinds() {
        const Shown s = showSaturator();
        QVERIFY(s.view && s.curve && s.color);
        QVERIFY2(s.view->implicitHeight() <= bodyHeight(),
                 qPrintable(QStringLiteral("%1 > %2").arg(s.view->implicitHeight()).arg(bodyHeight())));
        QVERIFY(bodyHeight() - s.view->implicitHeight() >= 6);
        QCOMPARE(s.view->height(), double(bodyHeight()));
        // The graphs grow into the body: the curve's column and Color's reach its bottom margin.
        QCOMPARE(s.curve->mapToItem(s.view, QPointF(0, 0)).y(), 6.0);
        QVERIFY(s.curve->height() > double(SaturatorCurve::kMinimumHeight));
        QVERIFY(s.color->height() >= double(SaturatorColorGraph::kMinimumHeight));

        // Every knob, at its parameter's value, with a tooltip.
        for (const QString& id : kKnobs) {
            KnobItem* knob = knobOf(s.view, id);
            QVERIFY2(knob, qPrintable(id));
            QCOMPARE(knob->value(), param(s.track, s.device, id));
            QVERIFY2(!find(s.view, id)->property("tooltip").toString().isEmpty(), qPrintable(id));
        }
        QCOMPARE(knobOf(s.view, QStringLiteral("drive"))->value(), 0.0);
        QCOMPARE(knobOf(s.view, QStringLiteral("threshold"))->value(), -18.0);
        QCOMPARE(knobOf(s.view, QStringLiteral("freq"))->value(), 1000.0);
        QCOMPARE(knobOf(s.view, QStringLiteral("mix"))->value(), 100.0);
        QCOMPARE(knobOf(s.view, QStringLiteral("ws_curve"))->value(), 50.0);
        QVERIFY(knobOf(s.view, QStringLiteral("freq"))->logScale());
        // Bipolar: the knobs whose range is symmetric about 0 (Drive, Amt Lo and Amt Hi, ±36 dB); no others.
        for (const QString& id : kKnobs) {
            const bool symmetric = id == u"drive" || id == u"base" || id == u"depth";
            QVERIFY2(knobOf(s.view, id)->bipolar() == symmetric, qPrintable(id));
        }
        // Color's knobs under Live 12.1's names (their ids those of before it).
        const std::map<QString, QString> captions = {{QStringLiteral("base"), QStringLiteral("Amt Lo")},
                                                     {QStringLiteral("freq"), QStringLiteral("Freq")},
                                                     {QStringLiteral("width"), QStringLiteral("Width")},
                                                     {QStringLiteral("depth"), QStringLiteral("Amt Hi")}};
        for (const auto& [id, caption] : captions)
            QCOMPARE(find(s.view, id)->property("title").toString(), caption);

        // The lists and switches.
        for (const char* id : {"type", "clip", "color", "dc", "hq"}) {
            QQuickItem* control = find(s.view, QString::fromLatin1(id));
            QVERIFY2(control && buttonOf(control), id);
            QVERIFY2(control->property("param").value<QObject*>(), id);
        }
        QCOMPARE(find(s.view, QStringLiteral("type"))->property("index").toInt(), 0);
        QCOMPARE(buttonOf(find(s.view, QStringLiteral("type")))->property("text").toString(),
                 QStringLiteral("Analog Clip"));
        QCOMPARE(buttonOf(find(s.view, QStringLiteral("clip")))->property("text").toString(),
                 QStringLiteral("No Clip"));
        QVERIFY(!find(s.view, QStringLiteral("color"))->property("lit").toBool());

        // Color is off and the curve isn't the Waveshaper: their knobs are dimmed (still editable).
        for (const QString& id : kColorKnobs + kShaperKnobs) {
            QQuickItem* cell = find(s.view, id);
            QTRY_COMPARE_WITH_TIMEOUT(cell->opacity(), 0.55, 500);
            QVERIFY(cell->isEnabled());
        }
        QVERIFY(find(s.view, QStringLiteral("waveshaperSection"))->isVisible());
        QVERIFY(!find(s.view, QStringLiteral("bassSection"))->isVisible());
        QCOMPARE(find(s.view, QStringLiteral("shaperTitle"))->property("text").toString(),
                 QStringLiteral("Waveshaper"));

        auto rectOf = [&](const QString& id) {
            QQuickItem* item = find(s.view, id);
            return item->mapRectToItem(s.view, QRectF(0, 0, item->width(), item->height()));
        };
        // The columns, 8 px apart (a 1 px line and 8 px more before Color and before the shaper's
        // controls), inside the body's margins; the editor as wide as they are.
        const std::vector<std::pair<QString, double>> order = {{QStringLiteral("front"), 8.0},
                                                               {QStringLiteral("curveColumn"), 8.0},
                                                               {QStringLiteral("levels"), 8.0},
                                                               {QStringLiteral("colorSection"), 17.0},
                                                               {QStringLiteral("shaperSection"), 17.0}};
        std::map<QString, QRectF> columns;
        double right = 0.0;
        for (const auto& [id, gap] : order) {
            const QRectF r = rectOf(id);
            QVERIFY2(r.left() == right + gap, qPrintable(id + QStringLiteral(" at ") + QString::number(r.left())));
            QCOMPARE(r.top(), 6.0);
            QCOMPARE(r.bottom(), s.view->height() - 6);
            columns[id] = r;
            right = r.right();
        }
        QCOMPARE(s.view->implicitWidth(), right + 8);
        QCOMPARE(columns.at(QStringLiteral("curveColumn")).width(), double(SaturatorCurve::kWidth));
        QCOMPARE(columns.at(QStringLiteral("colorSection")).width(), double(SaturatorColorGraph::kWidth));

        // The lists as wide as their longest names with the arrow, in the font they are drawn in (the
        // Type list, "Medium Curve", widening the front panel past its 84 px).
        for (const char* id : {"type", "clip"}) {
            QQuickItem* face = buttonOf(find(s.view, QString::fromLatin1(id)));
            const QFontMetricsF metrics(face->property("font").value<QFont>());
            const double room =
                face->width() - face->property("leftPadding").toDouble() - face->property("rightPadding").toDouble();
            for (const QString& name : find(s.view, QString::fromLatin1(id))->property("names").toStringList()) {
                const double needs = metrics.horizontalAdvance(name);
                QVERIFY2(needs <= room, qPrintable(QStringLiteral("%1: %2 px in %3").arg(name).arg(needs).arg(room)));
            }
        }

        // DC and HQ share the front panel's width, 4 px apart, every edge on a whole pixel (an odd width
        // halved would put the inner edges on half pixels, blurred): at the width the font gives and at one
        // more, so an odd and an even width are both tried whatever the font.
        QQuickItem* front = find(s.view, QStringLiteral("front"));
        const double frontWidth = front->width();
        for (const double width : {frontWidth, frontWidth + 1}) {
            front->setWidth(width);
            QVERIFY(QQuickTest::qWaitForPolish(s.view->window()));  // (the Row placing them)
            const QRectF panel = rectOf(QStringLiteral("front"));
            const QRectF dc = rectOf(QStringLiteral("dc")), hq = rectOf(QStringLiteral("hq"));
            QCOMPARE(dc.left(), panel.left());
            QCOMPARE(hq.right(), panel.right());
            QCOMPARE(hq.left() - dc.right(), 4.0);
            for (const double edge : {dc.left(), dc.right(), hq.left(), hq.right()})
                QVERIFY2(edge == std::round(edge),
                         qPrintable(QStringLiteral("%1 wide: an edge at %2").arg(width).arg(edge)));
        }
        front->setWidth(frontWidth);  // (the same width as its binding gives, the columns after it where they were)
        QVERIFY(QQuickTest::qWaitForPolish(s.view->window()));

        // No caption or readout is cut short, each knob at either end of its range.
        for (const QString& id : kKnobs) {
            const double before = param(s.track, s.device, id);
            for (const bool high : {false, true}) {
                KnobItem* knob = knobOf(s.view, id);
                editor()->setDeviceParam(s.track, s.device, id, high ? knob->to() : knob->from());
                QCoreApplication::processEvents();
                for (QQuickItem* text : find(s.view, id)->childItems()) {
                    if (text->inherits("QQuickText"))
                        QVERIFY2(!text->property("truncated").toBool(),
                                 qPrintable(id + QStringLiteral(": ") + text->property("text").toString()));
                }
            }
            editor()->setDeviceParam(s.track, s.device, id, before);
        }

        // Nothing overlaps: each control stays in its column, the shown shaper section with the
        // Waveshaper's knobs, then with the Bass Shaper's Threshold.
        const std::map<QString, QString> columnOf = {
            {QStringLiteral("drive"), QStringLiteral("front")},
            {QStringLiteral("type"), QStringLiteral("front")},
            {QStringLiteral("dc"), QStringLiteral("front")},
            {QStringLiteral("hq"), QStringLiteral("front")},
            {QStringLiteral("saturatorCurve"), QStringLiteral("curveColumn")},
            {QStringLiteral("clip"), QStringLiteral("curveColumn")},
            {QStringLiteral("output"), QStringLiteral("levels")},
            {QStringLiteral("mix"), QStringLiteral("levels")},
            {QStringLiteral("color"), QStringLiteral("colorSection")},
            {QStringLiteral("saturatorColor"), QStringLiteral("colorSection")},
            {QStringLiteral("base"), QStringLiteral("colorSection")},
            {QStringLiteral("freq"), QStringLiteral("colorSection")},
            {QStringLiteral("width"), QStringLiteral("colorSection")},
            {QStringLiteral("depth"), QStringLiteral("colorSection")},
            {QStringLiteral("shaperTitle"), QStringLiteral("shaperSection")},
            {QStringLiteral("threshold"), QStringLiteral("shaperSection")},
            {QStringLiteral("ws_drive"), QStringLiteral("shaperSection")},
            {QStringLiteral("ws_lin"), QStringLiteral("shaperSection")},
            {QStringLiteral("ws_curve"), QStringLiteral("shaperSection")},
            {QStringLiteral("ws_damp"), QStringLiteral("shaperSection")},
            {QStringLiteral("ws_depth"), QStringLiteral("shaperSection")},
            {QStringLiteral("ws_period"), QStringLiteral("shaperSection")},
        };
        auto inColumns = [&](int shown) {
            int checked = 0;
            for (const auto& [id, column] : columnOf) {
                QQuickItem* item = find(s.view, id);
                if (!item || !item->isVisible())
                    continue;
                ++checked;
                QVERIFY2(columns.at(column).contains(rectOf(id)), qPrintable(id + QStringLiteral(" in ") + column));
            }
            QCOMPARE(checked, shown);  // (all but the hidden section's)
        };
        inColumns(21);
        // Color's switch is above its graph, not over it (where it would hide a handle).
        QVERIFY(!rectOf(QStringLiteral("color")).intersects(rectOf(QStringLiteral("saturatorColor"))));
        QVERIFY(s.color->height() >= double(SaturatorColorGraph::kMinimumHeight));
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("type"), 2.0);
        QTRY_VERIFY_WITH_TIMEOUT(find(s.view, QStringLiteral("bassSection"))->isVisible() &&
                                     !find(s.view, QStringLiteral("waveshaperSection"))->isVisible(),
                                 500);
        inColumns(16);
        // (the hint under the Threshold too, its text within its width)
        QQuickItem* hint = find(s.view, QStringLiteral("bassHint"));
        QVERIFY(hint && hint->isVisible());
        QVERIFY(columns.at(QStringLiteral("shaperSection")).contains(rectOf(QStringLiteral("bassHint"))));
        QVERIFY2(hint->property("contentWidth").toDouble() <= hint->width(),
                 qPrintable(QString::number(hint->property("contentWidth").toDouble())));
    }

    // --- Every control sets its parameter undoably, and the engine follows ---------------------

    void controlsAreUndoable() {
        const Shown s = showSaturator();
        QVERIFY(s.view);
        auto value = [&](const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); };

        // A knob shows the parameter as it is, and its edits undo.
        KnobItem* drive = knobOf(s.view, QStringLiteral("drive"));
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("drive"), 12.0);
        QCOMPARE(drive->value(), 12.0);
        undo()->undo();
        QCOMPARE(drive->value(), 0.0);

        // Dragging it is one step.
        int steps = undo()->index();
        const QPoint at = centerOf(drive);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        for (int dy = 10; dy <= 30; dy += 10)
            dragTo(at - QPoint(0, dy));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 30));
        QVERIFY(value("drive") > 0.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(engineParam(s, "drive"), float(value("drive")));
        undo()->undo();
        QCOMPARE(value("drive"), 0.0);

        // Every other knob too (set as its knob sets it), one step each.
        for (const QString& id : kKnobs) {
            KnobItem* knob = knobOf(s.view, id);
            const double before = knob->value();
            const double to = knob->from() + 0.75 * (knob->to() - knob->from());
            steps = undo()->index();
            Q_EMIT knob->moved(to, QStringLiteral("gesture-") + id);
            QVERIFY2(std::abs(param(s.track, s.device, id) - to) < 1e-6, qPrintable(id));
            QCOMPARE(undo()->index(), steps + 1);
            QCOMPARE(engineParam(s, id.toLatin1().constData()), float(param(s.track, s.device, id)));
            undo()->undo();
            QCOMPARE(param(s.track, s.device, id), before);
            QCOMPARE(knob->value(), before);
        }

        // Color: the switch lights, its knobs light up; undone.
        QQuickItem* color = find(s.view, QStringLiteral("color"));
        steps = undo()->index();
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(buttonOf(color)));
        QCOMPARE(value("color"), 1.0);
        QCOMPARE(undo()->index(), steps + 1);
        QVERIFY(color->property("lit").toBool());
        for (const QString& id : kColorKnobs)
            QTRY_COMPARE_WITH_TIMEOUT(find(s.view, id)->opacity(), 1.0, 500);
        QCOMPARE(engineParam(s, "color"), 1.f);
        undo()->undo();
        QCOMPARE(value("color"), 0.0);
        QVERIFY(!color->property("lit").toBool());

        // The type: the Waveshaper lights its knobs; the Bass Shaper swaps in its Threshold.
        QQuickItem* type = find(s.view, QStringLiteral("type"));
        steps = undo()->index();
        QMetaObject::invokeMethod(type, "choose", Q_ARG(QVariant, 7));
        QCOMPARE(value("type"), 7.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(buttonOf(type)->property("text").toString(), QStringLiteral("Waveshaper"));
        for (const QString& id : kShaperKnobs)
            QTRY_COMPARE_WITH_TIMEOUT(find(s.view, id)->opacity(), 1.0, 500);
        QVERIFY(find(s.view, QStringLiteral("waveshaperSection"))->isVisible());
        QQuickItem* title = find(s.view, QStringLiteral("shaperTitle"));
        QCOMPARE(title->property("text").toString(), QStringLiteral("Waveshaper"));
        QMetaObject::invokeMethod(type, "choose", Q_ARG(QVariant, 2));
        QCOMPARE(value("type"), 2.0);
        QTRY_VERIFY_WITH_TIMEOUT(find(s.view, QStringLiteral("bassSection"))->isVisible() &&
                                     !find(s.view, QStringLiteral("waveshaperSection"))->isVisible(),
                                 500);
        QCOMPARE(title->property("text").toString(), QStringLiteral("Bass Shaper"));
        QCOMPARE(engineParam(s, "type"), 2.f);
        undo()->undo();
        undo()->undo();
        QCOMPARE(value("type"), 0.0);
        QTRY_VERIFY_WITH_TIMEOUT(find(s.view, QStringLiteral("waveshaperSection"))->isVisible() &&
                                     !find(s.view, QStringLiteral("bassSection"))->isVisible(),
                                 500);

        // Post Clip, from its list.
        QQuickItem* clip = find(s.view, QStringLiteral("clip"));
        steps = undo()->index();
        QMetaObject::invokeMethod(clip, "choose", Q_ARG(QVariant, 2));
        QCOMPARE(value("clip"), 2.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(buttonOf(clip)->property("text").toString(), QStringLiteral("Hard Clip"));
        QCOMPARE(engineParam(s, "clip"), 2.f);
        undo()->undo();
        QCOMPARE(value("clip"), 0.0);

        // DC.
        QQuickItem* dc = find(s.view, QStringLiteral("dc"));
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(buttonOf(dc)));
        QCOMPARE(value("dc"), 1.0);
        QVERIFY(dc->property("lit").toBool());
        QCOMPARE(engineParam(s, "dc"), 1.f);
        undo()->undo();
        QCOMPARE(value("dc"), 0.0);

        // Hi-Quality: the device's latency follows (36 samples of the 4x filters).
        QQuickItem* hq = find(s.view, QStringLiteral("hq"));
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(buttonOf(hq)));
        QCOMPARE(value("hq"), 1.0);
        QVERIFY(hq->property("lit").toBool());
        QTRY_COMPARE_WITH_TIMEOUT(bridge()->deviceLatency(s.track, s.device), 36, 1000);
        undo()->undo();
        QCOMPARE(value("hq"), 0.0);
        QTRY_COMPARE_WITH_TIMEOUT(bridge()->deviceLatency(s.track, s.device), 0, 1000);
    }

    // --- What is drawn is what plays -----------------------------------------------------------

    void curveIsTheSound() {
        const Shown s = showSaturator();
        QVERIFY(s.curve && s.color);
        SaturatorCurve* curve = s.curve;

        // The default curve is the engine's, point for point.
        const std::vector<double>& inputs = curve->inputs();
        QVERIFY(inputs.size() > 200);
        QCOMPARE(inputs.front(), -1.0);
        QCOMPARE(inputs.back(), 1.0);
        const QList<double> expected = saturatorCurve(SaturatorShape{}, QList<double>(inputs.begin(), inputs.end()));
        for (size_t i = 0; i < inputs.size(); i += 7)
            QCOMPARE(curve->targetCurve()[i], expected[qsizetype(i)]);
        QCOMPARE(curve->drawnCurve(), curve->targetCurve());
        QVERIFY(!curve->morphing());

        // A new shape morphs in (no lag behind the parameters themselves), and lands exactly on it.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("type"), 5.0);
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("drive"), 6.0);
        QVERIFY(curve->morphing());
        QVERIFY(curve->drawnCurve() != curve->targetCurve());
        QTRY_VERIFY_WITH_TIMEOUT(!curve->morphing(), 1000);
        QCOMPARE(curve->drawnCurve(), curve->targetCurve());
        SaturatorShape folded;
        folded.type = 5;
        folded.driveDb = 6.0;
        const QList<double> fold = saturatorCurve(folded, QList<double>(inputs.begin(), inputs.end()));
        for (size_t i = 0; i < inputs.size(); ++i)
            QCOMPARE(curve->targetCurve()[i], fold[qsizetype(i)]);
        // (Sinoid Fold at +6 dB folds back: full scale in comes out lower than half scale does)
        QVERIFY(std::abs(curve->targetCurve().back()) < curve->targetCurve()[inputs.size() * 3 / 4]);

        // Every type, both Post Clips and the shapers' own controls are the engine's too, each control
        // at a value of its own: compared with the engine's saturator::transfer itself (not the application
        // layer's wrapper the editor calls), so a setting read into the wrong place on the way would show.
        const std::vector<std::pair<const char*, double>> settings = {
            {"threshold", -30.0}, {"ws_drive", 70.0}, {"ws_lin", 30.0},   {"ws_curve", 80.0},
            {"ws_damp", 20.0},    {"ws_depth", 60.0}, {"ws_period", 40.0}};
        for (const auto& [id, v] : settings)
            editor()->setDeviceParam(s.track, s.device, QString::fromLatin1(id), v);
        for (const int clip : {1, 2}) {
            editor()->setDeviceParam(s.track, s.device, QStringLiteral("clip"), double(clip));
            for (int type = 0; type < sub::saturator::kTypes; ++type) {
                editor()->setDeviceParam(s.track, s.device, QStringLiteral("type"), double(type));
                const sub::saturator::Shape played =
                    sub::saturator::makeShape(type, -30.f, 70.f, 30.f, 80.f, 20.f, 60.f, 40.f);
                for (size_t i = 0; i < inputs.size(); ++i) {
                    const double sound = sub::saturator::transfer(played, sub::saturator::clipAt(clip),
                                                                  sub::expDbToGain(6.f), float(inputs[i]));
                    QVERIFY2(curve->targetCurve()[i] == sound,
                             qPrintable(QStringLiteral("type %1, clip %2, at %3").arg(type).arg(clip).arg(inputs[i])));
                }
            }
        }

        // Color's EQ is the engine's design at its rate (the engine's own colorResponseDb), once it has
        // eased there; each of its four controls away from its default.
        const double rate = bridge()->sampleRate();
        for (const auto& [id, v] :
             {std::pair{"color", 1.0}, {"base", 12.0}, {"freq", 2500.0}, {"width", 30.0}, {"depth", -6.0}})
            editor()->setDeviceParam(s.track, s.device, QString::fromLatin1(id), v);
        QTRY_VERIFY_WITH_TIMEOUT(s.color->settled(), 1000);
        const std::vector<double>& columns = s.color->columns();
        QVERIFY(columns.size() > 150);
        const sub::saturator::ColorDesign design = sub::saturator::colorDesign(12.0, 2500.0, 30.0, -6.0, rate);
        for (size_t i = 0; i < columns.size(); ++i)
            QCOMPARE(s.color->curveDb()[i], sub::saturator::colorResponseDb(design, columns[i], rate));
        // Its handles sit on it: Amt Lo's on the shelf (+12 dB), the peak's at its Amt Hi.
        QVERIFY(std::abs(s.color->peakHandle().x() - s.color->xOf(2500.0)) < 1e-9);
        QVERIFY(std::abs(s.color->peakHandle().y() - s.color->yOfDb(-6.0)) < 0.2);
        QVERIFY(std::abs(s.color->baseHandle().y() - s.color->yOfDb(12.0)) < 1.0);
    }

    // --- An editor opens on the device as it is, without easing in from the defaults ----------------

    void opensAsItIs() {
        const QString track = audioTrackWith(tone(1000.0, kSampleRate, 0.5), QStringLiteral("tone"), 1.0);
        QVERIFY(!track.isEmpty());
        const QString device = editor()->addDevice(track, QStringLiteral("saturator"));
        for (const auto& [id, v] : {std::pair{"type", 5.0}, {"drive", 12.0}, {"color", 1.0}, {"base", 12.0}})
            editor()->setDeviceParam(track, device, QString::fromLatin1(id), v);
        QQuickItem* view = show(QStringLiteral("saturator"), track, device);
        QVERIFY(view);
        auto* curve = find<SaturatorCurve>(view, QStringLiteral("saturatorCurve"));
        auto* color = find<SaturatorColorGraph>(view, QStringLiteral("saturatorColor"));
        QVERIFY(curve && color);
        // (show() has let 50 ms of ticks go by: a morph from the defaults would still be under way)
        QVERIFY(!curve->morphing());
        QCOMPARE(curve->drawnCurve(), curve->targetCurve());
        SaturatorShape folded;
        folded.type = 5;
        folded.driveDb = 12.0;
        const std::vector<double>& inputs = curve->inputs();
        const QList<double> fold = saturatorCurve(folded, QList<double>(inputs.begin(), inputs.end()));
        for (size_t i = 0; i < inputs.size(); ++i)
            QCOMPARE(curve->drawnCurve()[i], fold[qsizetype(i)]);
        const std::vector<double>& columns = color->columns();
        const QList<double> db = saturatorColorDb(12.0, 1000.0, 50.0, 0.0, bridge()->sampleRate(),
                                                  QList<double>(columns.begin(), columns.end()));
        for (size_t i = 0; i < columns.size(); ++i)
            QCOMPARE(color->curveDb()[i], db[qsizetype(i)]);
        QTRY_VERIFY_WITH_TIMEOUT(color->settled(), 1000);
    }

    // --- The graphs' drags, one undo step each ---------------------------------------------------

    void graphDragsAreOneStep() {
        const Shown s = showSaturator();
        QVERIFY(s.curve && s.color);
        auto value = [&](const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); };

        // The curve: up for Drive (kDrivePerPixel a pixel).
        int steps = undo()->index();
        const QPoint middle = scenePoint(s.curve, s.curve->plot().center());
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, middle);
        for (int dy = 10; dy <= 40; dy += 10)
            dragTo(middle - QPoint(0, dy));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, middle - QPoint(0, 40));
        QVERIFY2(std::abs(value("drive") - 10.0) < 0.01, qPrintable(QString::number(value("drive"))));
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(engineParam(s, "drive"), float(value("drive")));
        // (under the mouse the curve snaps to its shape, no lag)
        QVERIFY(!s.curve->morphing());

        // Each drag shows the automation of the parameter it moves most (that of the first one set).
        auto touchedKeys = [](const QSignalSpy& touched) {
            QStringList keys;
            for (const QList<QVariant>& args : touched)
                keys << args.at(1).toString();
            keys.removeDuplicates();
            return keys;
        };
        auto keyOf = [&](const char* id) {
            return QStringList{automation::deviceKey(s.device, QString::fromLatin1(id))};
        };

        // In the Bass Shaper, across for the Threshold too.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("type"), 2.0);
        steps = undo()->index();
        QSignalSpy touched(editor(), &ProjectEditor::parameterTouched);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, middle);
        for (int dx = 5; dx <= 20; dx += 5)
            dragTo(middle - QPoint(dx, 0));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, middle - QPoint(20, 0));
        QVERIFY2(std::abs(value("threshold") + 23.0) < 0.01, qPrintable(QString::number(value("threshold"))));
        QVERIFY(std::abs(value("drive") - 10.0) < 0.01);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(engineParam(s, "threshold"), float(value("threshold")));
        QCOMPARE(touchedKeys(touched), keyOf("threshold"));

        // In the Waveshaper, across for its Curve; Shift, pressed mid-drag, goes on finer from there.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("type"), 7.0);
        steps = undo()->index();
        touched.clear();
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, middle);
        dragTo(middle + QPoint(20, 0));
        QVERIFY(std::abs(value("ws_curve") - (50.0 + 20 * SaturatorCurve::kCurvePerPixel)) < 0.01);
        dragTo(middle + QPoint(40, 0), Qt::ShiftModifier);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::ShiftModifier, middle + QPoint(40, 0));
        const double curve = 50.0 + 20 * SaturatorCurve::kCurvePerPixel * (1.0 + SaturatorCurve::kFine);
        QVERIFY2(std::abs(value("ws_curve") - curve) < 0.01, qPrintable(QString::number(value("ws_curve"))));
        QVERIFY(std::abs(value("drive") - 10.0) < 0.01);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(touchedKeys(touched), keyOf("ws_curve"));

        // A double-click sets Drive back to 0 dB, one step.
        steps = undo()->index();
        // (long enough after the last press not to make a double-click of it)
        QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, middle);
        QCOMPARE(value("drive"), 0.0);
        QCOMPARE(undo()->index(), steps + 1);

        // Color: the peak's handle across for Freq, up for Amt Hi, switching Color on, all one step.
        SaturatorColorGraph* color = s.color;
        QTRY_VERIFY_WITH_TIMEOUT(color->settled(), 1000);
        QVERIFY(std::abs(color->peakHandle().x() - color->xOf(1000.0)) < 1e-9);
        QVERIFY(std::abs(color->peakHandle().y() - color->yOfDb(0.0)) < 1e-9);
        steps = undo()->index();
        const QPoint from = scenePoint(color, color->peakHandle());
        const QPoint to = scenePoint(color, QPointF(color->xOf(2000.0), color->peakHandle().y() - 20));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, from);
        for (int i = 1; i <= 4; ++i)
            dragTo(from + (to - from) * i / 4);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, to);
        const LogAxis axis = color->frequencyAxis();
        const double freq = axis.valueAt(axis.position(1000.0) + (to.x() - from.x()));
        QVERIFY2(std::abs(value("freq") - freq) < 0.06, qPrintable(QString::number(value("freq"))));
        QVERIFY(std::abs(value("freq") / 2000.0 - 1.0) < 0.03);
        const double depth = (from.y() - to.y()) * 2 * SaturatorColorGraph::kRangeDb / color->plot().height();
        QVERIFY2(std::abs(value("depth") - depth) < 0.01, qPrintable(QString::number(value("depth"))));
        QCOMPARE(value("color"), 1.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(engineParam(s, "freq"), float(value("freq")));
        QCOMPARE(engineParam(s, "depth"), float(value("depth")));
        QCOMPARE(engineParam(s, "color"), 1.f);
        undo()->undo();
        QCOMPARE(value("freq"), 1000.0);
        QCOMPARE(value("depth"), 0.0);
        QCOMPARE(value("color"), 0.0);
        undo()->redo();
        QTRY_VERIFY_WITH_TIMEOUT(color->settled(), 1000);

        // Amt Lo's handle, up; a double-click on a handle sets its gain back to 0 dB, one step.
        steps = undo()->index();
        const QPoint base = scenePoint(color, color->baseHandle());
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, base);
        dragTo(base - QPoint(0, 10));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, base - QPoint(0, 10));
        QVERIFY(value("base") > 5.0);
        QCOMPARE(undo()->index(), steps + 1);
        QTRY_VERIFY_WITH_TIMEOUT(color->settled(), 1000);
        steps = undo()->index();
        QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, scenePoint(color, color->peakHandle()));
        QCOMPARE(value("depth"), 0.0);
        QCOMPARE(undo()->index(), steps + 1);

        // A press away from both handles changes nothing.
        QTRY_VERIFY_WITH_TIMEOUT(color->settled(), 1000);
        steps = undo()->index();
        const QPoint away = scenePoint(color, QPointF(color->xOf(8000.0), color->plot().bottom() - 4));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, away);
        dragTo(away - QPoint(0, 20));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, away - QPoint(0, 20));
        QCOMPARE(undo()->index(), steps);

        // The peak dragged straight up shows Amt Hi's automation (what it moves), not Freq's. On past
        // the top: the handle stops at +36 dB while the mouse goes on, and is released away from it.
        touched.clear();
        const QPoint peak = scenePoint(color, color->peakHandle());
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, peak);
        for (int dy = 10; dy <= 50; dy += 10)
            dragTo(peak - QPoint(0, dy));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, peak - QPoint(0, 50));
        QCOMPARE(value("depth"), 36.0);
        QCOMPARE(touchedKeys(touched), keyOf("depth"));
        // Back over an empty spot the cursor is the plain one again; over a handle, the pointing hand.
        QTest::mouseMove(window_, scenePoint(color, QPointF(color->xOf(300.0), color->plot().bottom() - 6)));
        QCOMPARE(color->cursor().shape(), Qt::ArrowCursor);
        QTest::mouseMove(window_, scenePoint(color, color->baseHandle()));
        QCOMPARE(color->cursor().shape(), Qt::PointingHandCursor);
        QTest::mouseMove(window_, scenePoint(color, QPointF(color->xOf(300.0), color->plot().bottom() - 6)));
        QCOMPARE(color->cursor().shape(), Qt::ArrowCursor);

        // Amt Lo far up puts its handle near the graph's top, where nothing covers it: it drags, and
        // follows the mouse (it is on the shelf's slope, which moves a little less than Amt Lo itself).
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("base"), 30.0);
        QTRY_VERIFY_WITH_TIMEOUT(color->settled(), 1000);
        const QPointF high = color->baseHandle();
        QVERIFY2(high.y() < color->plot().top() + 0.25 * color->plot().height(), qPrintable(QString::number(high.y())));
        steps = undo()->index();
        const QPoint grabbed = scenePoint(color, high);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, grabbed);
        dragTo(grabbed + QPoint(0, 3));
        dragTo(grabbed + QPoint(0, 6));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, grabbed + QPoint(0, 6));
        QVERIFY2(value("base") < 30.0, qPrintable(QString::number(value("base"))));
        QCOMPARE(value("color"), 1.0);
        QCOMPARE(undo()->index(), steps + 1);
        QTRY_VERIFY_WITH_TIMEOUT(color->settled(), 1000);
        QVERIFY2(std::abs(color->baseHandle().y() - (high.y() + 6.0)) < 0.25,
                 qPrintable(QStringLiteral("%1 -> %2").arg(high.y()).arg(color->baseHandle().y())));
    }

    // --- The displays reach the graphs, which settle when the sound stops -------------------------

    void displaysReachTheGraphs() {
        const Shown s = showSaturator();
        QVERIFY(s.curve && s.color);
        SaturatorCurve* curve = s.curve;

        engine_->renderOffline(0.0, kSampleRate / 2);
        refreshDisplays();
        QVERIFY2(std::abs(curve->inputLevel() - 0.5) < 0.01, qPrintable(QString::number(curve->inputLevel())));
        QVERIFY2(std::abs(curve->outputLevel() - 0.5) < 0.01, qPrintable(QString::number(curve->outputLevel())));
        QVERIFY(curve->dotLevel() > 0.49);
        QVERIFY(!curve->settled());
        QVERIFY(curve->saturationTarget() < 0.01);  // (Analog Clip is straight at half scale)
        QVERIFY(s.color->live());
        QVERIFY(!s.color->settled());

        // A tick without values just after some is a gap between the audio's blocks (a buffer longer
        // than a tick), not silence: the levels, the dots, the bars and the spectrum hold.
        const double level = curve->inputLevel(), dot = curve->dotLevel(), bar = curve->outputBar();
        const std::vector<double> spectrum = s.color->inputSpectrum();
        QVERIFY(bar > 0.49);
        QVERIFY(!spectrum.empty());
        refreshDisplays();
        QCOMPARE(curve->inputLevel(), level);
        QCOMPARE(curve->dotLevel(), dot);
        QCOMPARE(curve->outputBar(), bar);
        QCOMPARE(s.color->inputSpectrum(), spectrum);

        // Without sound the dots fall back (the afterglow holding a moment where they were), then
        // everything settles: an idle editor stops repainting.
        QTRY_VERIFY_WITH_TIMEOUT(curve->glowLevel() > curve->dotLevel() + 0.1, 1000);
        QTRY_VERIFY_WITH_TIMEOUT(curve->dotLevel() < 0.05, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(curve->settled() && s.color->settled(), 6000);
        QCOMPARE(curve->dotLevel(), 0.0);
        QVERIFY(!s.color->live());

        // Driven 18 dB into Analog Clip, half scale is well past its corner: the dots glow hot.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("drive"), 18.0);
        engine_->renderOffline(0.0, kSampleRate / 2);
        refreshDisplays();
        QVERIFY2(curve->saturationTarget() > 0.7, qPrintable(QString::number(curve->saturationTarget())));
        QVERIFY(curve->saturation() > 0.0);
        QCOMPARE(curve->overFlash(), 0.0);
        // The output, clipped at full scale (the 4x filters aside, Analog Clip's 1).
        QVERIFY2(curve->outputLevel() > 0.99 && curve->outputLevel() < 1.01,
                 qPrintable(QString::number(curve->outputLevel())));
    }

    void overFullScale() {
        // A Utility 12 dB up before the Saturator: its input passes full scale, and the curve's sides flash,
        // fading once it is back under.
        const QString track = audioTrackWith(tone(1000.0, kSampleRate, 0.5), QStringLiteral("tone"), 1.0);
        QVERIFY(!track.isEmpty());
        const QString utility = editor()->addDevice(track, QStringLiteral("utility"));
        editor()->setDeviceParam(track, utility, QStringLiteral("gain"), 12.0);
        const QString device = editor()->addDevice(track, QStringLiteral("saturator"));
        QQuickItem* view = show(QStringLiteral("saturator"), track, device);
        QVERIFY(view);
        auto* curve = find<SaturatorCurve>(view, QStringLiteral("saturatorCurve"));
        QVERIFY(curve);
        engine_->renderOffline(0.0, kSampleRate / 4);
        refreshDisplays();
        QVERIFY2(curve->inputLevel() > 1.9, qPrintable(QString::number(curve->inputLevel())));
        QCOMPARE(curve->overFlash(), 1.0);
        QCOMPARE(curve->dotLevel(), curve->inputLevel());  // (drawn at full scale, the edge)
        QTRY_VERIFY_WITH_TIMEOUT(curve->overFlash() == 0.0, 2000);
        QTRY_VERIFY_WITH_TIMEOUT(curve->settled(), 6000);
    }

    // --- Pictures --------------------------------------------------------------------------------

    void screenshots() {
        // The defaults, a tone at half scale flowing.
        {
            const Shown s = showSaturator(hits(110.0, kSampleRate, 0.7, 0.25));
            QVERIFY(s.curve);
            // A hit, then its tail: the dots fall back from where it reached, which stays aglow.
            play(0.0, 0.05, 2, 20);
            play(0.17, 0.06, 4, 25);
            QVERIFY(s.curve->glowLevel() > s.curve->dotLevel() + 0.1);
            save(grab(), QStringLiteral("saturator.png"));
        }
        clearHost();
        // The Waveshaper with ripples, Color on and dipping, driven.
        {
            const Shown s = showSaturator(hits(220.0, kSampleRate, 0.8, 0.5));
            QVERIFY(s.curve);
            for (const auto& [id, v] : {std::pair{"type", 7.0}, {"ws_drive", 70.0}, {"ws_depth", 60.0},
                                        {"ws_period", 40.0}, {"color", 1.0}, {"depth", -12.0}, {"freq", 2500.0},
                                        {"base", 6.0}, {"drive", 3.0}, {"output", -3.0}})
                editor()->setDeviceParam(s.track, s.device, QString::fromLatin1(id), v);
            QTRY_VERIFY_WITH_TIMEOUT(!s.curve->morphing(), 1000);
            play(0.0, 0.3, 8, 20);
            // (the mouse over the peak's handle: it lights)
            QTest::mouseMove(window_, scenePoint(s.color, s.color->peakHandle()));
            QTest::qWait(30);
            save(grab(), QStringLiteral("saturator-waveshaper.png"));
        }
        clearHost();
        // The Bass Shaper driven into Soft Clip, Hi-Quality on.
        {
            const Shown s = showSaturator(hits(55.0, kSampleRate, 0.9, 0.5));
            QVERIFY(s.curve);
            for (const auto& [id, v] : {std::pair{"type", 2.0}, {"threshold", -30.0}, {"drive", 12.0},
                                        {"clip", 1.0}, {"hq", 1.0}})
                editor()->setDeviceParam(s.track, s.device, QString::fromLatin1(id), v);
            QTRY_VERIFY_WITH_TIMEOUT(!s.curve->morphing(), 1000);
            play(0.0, 0.15, 4, 20);
            QTest::qWait(120);  // (the dots fall back a little: the afterglow shows beyond them)
            save(grab(), QStringLiteral("saturator-bass.png"));
        }
        clearHost();
        // Medium Curve (the Type list's longest name) into Hard Clip, Color keeping the lows clean.
        {
            const Shown s = showSaturator(hits(110.0, kSampleRate, 0.8, 0.5));
            QVERIFY(s.curve);
            for (const auto& [id, v] : {std::pair{"type", 3.0}, {"drive", 12.0}, {"clip", 2.0}, {"color", 1.0},
                                        {"base", -12.0}, {"depth", 6.0}, {"freq", 3000.0}})
                editor()->setDeviceParam(s.track, s.device, QString::fromLatin1(id), v);
            QTRY_VERIFY_WITH_TIMEOUT(!s.curve->morphing(), 1000);
            play(0.0, 0.3, 8, 20);
            save(grab(), QStringLiteral("saturator-medium.png"));
        }
    }
};

QTEST_MAIN(TestUiSaturator)
#include "test_ui_device_editors_saturator.moc"
