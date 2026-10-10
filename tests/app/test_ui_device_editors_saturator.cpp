// The Saturator's editor (SaturatorEditor.qml, SaturatorCurve, SaturatorColorGraph),
// loaded as the device view loads it, with a real session over an engine:
// its controls bound to their parameters (undoable, the engine following), the
// curve and Color's EQ being the engine's own maths, the graphs' drags one undo
// step each, and the displays reaching the graphs (rendering offline) and
// letting them settle. With SUBSTATION_UI_SCREENSHOTS set to a folder, the
// editor is saved there as PNGs, with signal flowing, in three of its modes.

#include <QGuiApplication>
#include <QQuickItem>
#include <QStyleHints>
#include <QTest>
#include <QUndoStack>

#include <cmath>
#include <vector>

#include "EditorHarness.h"
#include "audio/SaturatorResponse.h"
#include "controls/KnobItem.h"
#include "devices/SaturatorColorGraph.h"
#include "devices/SaturatorCurve.h"

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
        QCOMPARE(s.view->implicitWidth(), 756.0);
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
        QVERIFY(knobOf(s.view, QStringLiteral("base"))->bipolar());
        QVERIFY(knobOf(s.view, QStringLiteral("depth"))->bipolar());
        QVERIFY(!knobOf(s.view, QStringLiteral("drive"))->bipolar());

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

        // Nothing overlaps: the columns' items stay in their columns, inside the body.
        const QRectF body(0, 0, s.view->width(), s.view->height());
        for (const QString& id : kKnobs + QStringList{QStringLiteral("type"), QStringLiteral("clip"),
                                                       QStringLiteral("color"), QStringLiteral("dc"),
                                                       QStringLiteral("hq")}) {
            QQuickItem* item = find(s.view, id);
            if (!item->isVisible())
                continue;
            const QRectF at = item->mapRectToItem(s.view, QRectF(0, 0, item->width(), item->height()));
            QVERIFY2(body.adjusted(8, 6, -8, -6).contains(at), qPrintable(id));
        }
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

        // Every type, Post Clip and the shapers' own controls are the engine's too.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("type"), 2.0);
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("threshold"), -30.0);
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("clip"), 1.0);
        SaturatorShape bass;
        bass.type = 2;
        bass.driveDb = 6.0;
        bass.thresholdDb = -30.0;
        bass.clip = 1;
        QCOMPARE(curve->targetCurve()[inputs.size() - 3], saturatorCurveAt(bass, inputs[inputs.size() - 3]));
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("type"), 7.0);
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("ws_depth"), 60.0);
        SaturatorShape shaper = bass;
        shaper.type = 7;
        shaper.wsDepth = 60.0;
        for (size_t i = 0; i < inputs.size(); i += 11)
            QCOMPARE(curve->targetCurve()[i], saturatorCurveAt(shaper, inputs[i]));

        // Color's EQ is the engine's design at its rate, once it has eased there.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("color"), 1.0);
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("base"), 12.0);
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("depth"), -6.0);
        QTRY_VERIFY_WITH_TIMEOUT(s.color->settled(), 1000);
        const std::vector<double>& columns = s.color->columns();
        QVERIFY(columns.size() > 150);
        const QList<double> db = saturatorColorDb(12.0, 1000.0, 50.0, -6.0, bridge()->sampleRate(),
                                                  QList<double>(columns.begin(), columns.end()));
        for (size_t i = 0; i < columns.size(); ++i)
            QCOMPARE(s.color->curveDb()[i], db[qsizetype(i)]);
        // Its handles sit on it: Base on the shelf (+12 dB), the peak at its Depth.
        QVERIFY(std::abs(s.color->peakHandle().x() - s.color->xOf(1000.0)) < 1e-9);
        QVERIFY(std::abs(s.color->peakHandle().y() - s.color->yOfDb(-6.0)) < 0.2);
        QVERIFY(std::abs(s.color->baseHandle().y() - s.color->yOfDb(12.0)) < 1.0);
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

        // In the Bass Shaper, across for the Threshold too.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("type"), 2.0);
        steps = undo()->index();
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, middle);
        for (int dx = 5; dx <= 20; dx += 5)
            dragTo(middle - QPoint(dx, 0));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, middle - QPoint(20, 0));
        QVERIFY2(std::abs(value("threshold") + 23.0) < 0.01, qPrintable(QString::number(value("threshold"))));
        QVERIFY(std::abs(value("drive") - 10.0) < 0.01);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(engineParam(s, "threshold"), float(value("threshold")));

        // In the Waveshaper, across for its Curve; Shift, pressed mid-drag, goes on finer from there.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("type"), 7.0);
        steps = undo()->index();
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, middle);
        dragTo(middle + QPoint(20, 0));
        QVERIFY(std::abs(value("ws_curve") - (50.0 + 20 * SaturatorCurve::kCurvePerPixel)) < 0.01);
        dragTo(middle + QPoint(40, 0), Qt::ShiftModifier);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::ShiftModifier, middle + QPoint(40, 0));
        const double curve = 50.0 + 20 * SaturatorCurve::kCurvePerPixel * (1.0 + SaturatorCurve::kFine);
        QVERIFY2(std::abs(value("ws_curve") - curve) < 0.01, qPrintable(QString::number(value("ws_curve"))));
        QVERIFY(std::abs(value("drive") - 10.0) < 0.01);
        QCOMPARE(undo()->index(), steps + 1);

        // A double-click sets Drive back to 0 dB, one step.
        steps = undo()->index();
        // (long enough after the last press not to make a double-click of it)
        QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, middle);
        QCOMPARE(value("drive"), 0.0);
        QCOMPARE(undo()->index(), steps + 1);

        // Color: the peak's handle across for Freq, up for Depth, switching Color on, all one step.
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

        // Base's handle, up; a double-click on a handle sets its gain back to 0 dB, one step.
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
        QVERIFY(curve->glowLevel() >= curve->dotLevel());
        QVERIFY(!curve->settled());
        QVERIFY(curve->saturationTarget() < 0.01);  // (Analog Clip is straight at half scale)
        QVERIFY(s.color->live());
        QVERIFY(!s.color->settled());

        // Without sound the dots fall back, then everything settles: an idle editor stops repainting.
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
        QVERIFY(curve->glowLevel() >= curve->dotLevel());
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
    }
};

QTEST_MAIN(TestUiSaturator)
#include "test_ui_device_editors_saturator.moc"
