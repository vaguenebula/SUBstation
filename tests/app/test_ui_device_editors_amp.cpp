// The Amp's editor (AmpEditor.qml, AmpPanel, AmpDriveGraph, AmpToneGraph),
// loaded as the device view loads it, with a real session over an engine: it
// fits the body; its controls bound to their parameters (undoable, the engine
// following); the model buttons with their sliding underline; the tone curve and
// the transfer curve being the engine's own maths (the application layer's
// AmpResponse.h); the tone handles' drags one undo step each; and the displays
// reaching the face (rendering offline): the tubes, the dots, the lamp and the
// meter, holding through ticks that read nothing and cooling after, the sag's
// feedback, a backlog of display values counting for nothing; an editor opened
// on a model showing it at once. With SUBSTATION_UI_SCREENSHOTS set to a
// folder, the editor is saved there as PNGs: Lead with signal flowing, Bass in
// Dual turned up, a tone handle dragged, idle.

#include <QGuiApplication>
#include <QMouseEvent>
#include <QQuickItem>
#include <QSignalSpy>
#include <QStyleHints>
#include <QTest>
#include <QUndoStack>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <vector>

#include "EditorHarness.h"
#include "audio/AmpResponse.h"
#include "builtin/AmpDesign.h"
#include "controls/KnobItem.h"
#include "devices/AmpDriveGraph.h"
#include "devices/AmpPanel.h"
#include "devices/AmpToneGraph.h"
#include "devices/DeviceParam.h"
#include "model/Automation.h"

using namespace sub::app;
using namespace sub::ui;
using sub::app::test::kSampleRate;

namespace {

constexpr double kPi = 3.14159265358979323846;

const QStringList kDials = {QStringLiteral("gain"),   QStringLiteral("bass"),     QStringLiteral("middle"),
                            QStringLiteral("treble"), QStringLiteral("presence"), QStringLiteral("volume")};

// A pluck every `beat` seconds: a tone whose level falls from `loud` to a tenth: something for the
// tubes, the dots and their trail to follow.
std::vector<float> plucks(double freq, int frames, double loud, double beat) {
    std::vector<float> samples(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        const double t = double(i) / kSampleRate;
        const double envelope = loud * std::exp(-std::fmod(t, beat) / (beat / 2.3));
        samples[size_t(i)] = float(envelope * (std::sin(2 * kPi * freq * t) + 0.3 * std::sin(4 * kPi * freq * t)));
    }
    return samples;
}

double dbOf(double amplitude) { return 20.0 * std::log10(std::max(amplitude, 1e-9)); }

}  // namespace

class TestUiAmp : public QObject, public sub::app::test::EditorHarness {
    Q_OBJECT

    // What a test works on: a track playing a signal, an Amp on it, its editor and its parts.
    struct Shown {
        QString track, device;
        QQuickItem* view = nullptr;
        AmpPanel* panel = nullptr;
        AmpDriveGraph* drive = nullptr;
        AmpToneGraph* tone = nullptr;
    };

    Shown showAmp(const std::vector<float>& signal) {
        Shown s;
        s.track = audioTrackWith(signal, QStringLiteral("tone"), double(signal.size()) / kSampleRate);
        if (s.track.isEmpty())
            return s;
        s.device = editor()->addDevice(s.track, QStringLiteral("amp"));
        s.view = show(QStringLiteral("amp"), s.track, s.device);
        if (s.view) {
            s.panel = find<AmpPanel>(s.view, QStringLiteral("ampPanel"));
            s.drive = find<AmpDriveGraph>(s.view, QStringLiteral("ampDriveGraph"));
            s.tone = find<AmpToneGraph>(s.view, QStringLiteral("ampToneGraph"));
        }
        return s;
    }
    Shown showAmp() { return showAmp(tone(220.0, kSampleRate / 2, 0.25)); }

    // An EditorKnob's dial (EditorKnob -> its ParamKnob -> the KnobItem).
    KnobItem* knobOf(QQuickItem* view, const QString& name) {
        QQuickItem* cell = find(view, name);
        auto* paramKnob = cell ? qvariant_cast<QQuickItem*>(cell->property("knob")) : nullptr;
        return paramKnob ? qvariant_cast<KnobItem*>(paramKnob->property("knob")) : nullptr;
    }
    // An EditorKnob's readout: the text under its knob (its EditorReadout, wherever it sits among the cell's children).
    QString readoutOf(QQuickItem* view, const QString& name) {
        QQuickItem* cell = find(view, name);
        if (!cell)
            return QStringLiteral("?");
        for (QQuickItem* child : cell->childItems())
            if (QByteArray(child->metaObject()->className()).startsWith("EditorReadout"))
                return child->property("text").toString();
        return QStringLiteral("?");
    }
    // A menu's entries' texts ("" for a separator).
    static QStringList menuTexts(QObject* menu) {
        QStringList texts;
        for (int i = 0; i < menu->property("count").toInt(); ++i) {
            QQuickItem* item = nullptr;
            QMetaObject::invokeMethod(menu, "itemAt", Q_RETURN_ARG(QQuickItem*, item), Q_ARG(int, i));
            texts << (item ? item->property("text").toString() : QString());
        }
        return texts;
    }
    // A ParamButton's clickable face.
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
    std::vector<float> play(double from, double seconds, int pieces = 1, int wait = 0) {
        std::vector<float> all;
        const int frames = int(seconds * kSampleRate / pieces);
        for (int i = 0; i < pieces; ++i) {
            const double at = from + double(i) * frames / kSampleRate;
            const std::vector<float> out = engine_->renderOffline(at * project()->tempo() / 60.0, frames);
            all.insert(all.end(), out.begin(), out.end());
            refreshDisplays();
            if (wait > 0)
                QTest::qWait(wait);
        }
        return all;
    }

    // `count` display ticks `wait` ms apart (the easing goes by the time that passed).
    void ticks(int count, int wait = 16) {
        for (int i = 0; i < count; ++i) {
            QTest::qWait(wait);
            refreshDisplays();
        }
    }

    // A mouse event sent to the window as the system would, with its timestamp.
    void send(QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons buttons, ulong timestamp) {
        QMouseEvent event(type, QPointF(at), QPointF(at), QPointF(window_->mapToGlobal(at)), button, buttons,
                          Qt::NoModifier);
        event.setTimestamp(timestamp);
        QGuiApplication::sendEvent(window_, &event);
    }
    // A mouse event sent to an item itself, with its timestamp: whether it took it.
    bool sendTo(QQuickItem* item, QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons buttons,
                ulong timestamp) {
        QMouseEvent event(type, at, item->mapToScene(at), QPointF(window_->mapToGlobal(scenePoint(item, at))), button,
                          buttons, Qt::NoModifier);
        event.setTimestamp(timestamp);
        event.setAccepted(true);
        QCoreApplication::sendEvent(item, &event);
        return event.isAccepted();
    }
    // A wheel turned over an item's `at`, `notches` (120 each), with `modifiers`: whether it took it.
    bool wheelOver(QQuickItem* item, QPointF at, int notches, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QWheelEvent event(at, item->mapToGlobal(at), QPoint(), QPoint(0, 120 * notches), Qt::NoButton, modifiers,
                          Qt::NoScrollPhase, false);
        event.setAccepted(false);
        QCoreApplication::sendEvent(item, &event);
        return event.isAccepted();
    }

private Q_SLOTS:
    void initTestCase() {
        if (!haveDisplay())
            QSKIP("needs a display: the offscreen platform renders Qt Quick in software, without this geometry");
        startHost();
    }

    void cleanupTestCase() { stopHost(); }

    void init() { clearHost(); }

    // --- It loads, fits and lays out as Ableton's ----------------------------------------------

    void loads() {
        QVariant url;
        QMetaObject::invokeMethod(root_.get(), "editorFor", Q_RETURN_ARG(QVariant, url),
                                  Q_ARG(QVariant, QStringLiteral("amp")));
        QVERIFY(url.toString().endsWith(QStringLiteral("AmpEditor.qml")));
        const Shown s = showAmp();
        QVERIFY(s.view && s.panel && s.drive && s.tone);
        QVERIFY2(s.view->implicitHeight() <= bodyHeight(),
                 qPrintable(QStringLiteral("%1 > %2").arg(s.view->implicitHeight()).arg(bodyHeight())));
        QCOMPARE(s.view->implicitWidth(), 630.0);
        QCOMPARE(s.view->height(), double(bodyHeight()));
        QVERIFY(find(s.view, QStringLiteral("typeUnderline")));

        // Seven model buttons, named after the parameter's labels, as the engine's models.
        const std::vector<std::string>& models = sub::amp::modelLabels();
        QCOMPARE(models.size(), size_t(7));
        QList<QQuickItem*> controls;
        for (const std::string& model : models) {
            const QString name = QString::fromStdString(model);
            QQuickItem* button = find(s.view, QStringLiteral("type_") + name.toLower());
            QVERIFY(button);
            QCOMPARE(buttonOf(button)->property("text").toString(), name);
            controls << button;
        }
        for (const char* name : {"dual_mono", "dual_dual"}) {
            QQuickItem* button = find(s.view, QString::fromLatin1(name));
            QVERIFY(button);
            controls << button;
        }
        // Each button's label fits it (the monitor role has no padding).
        for (QQuickItem* button : controls) {
            auto* content = qvariant_cast<QQuickItem*>(buttonOf(button)->property("contentItem"));
            QVERIFY(content);
            QVERIFY2(content->implicitWidth() <= button->width(), qPrintable(button->objectName()));
        }
        for (const QString& id : kDials + QStringList{QStringLiteral("mix")}) {
            QVERIFY2(knobOf(s.view, id), qPrintable(id));
            controls << find(s.view, id);
        }
        controls << s.drive << s.tone;

        // The graphs grow into the body, 6 px above its bottom, at least 40 px tall.
        for (QQuickItem* graph : {static_cast<QQuickItem*>(s.drive), static_cast<QQuickItem*>(s.tone)}) {
            QVERIFY(graph->height() >= 40.0);
            QCOMPARE(graph->mapToItem(s.view, QPointF(0, graph->height())).y(), s.view->height() - 6.0);
        }
        // Nothing passes the right margin, and nothing overlaps.
        QList<QRectF> rects;
        for (QQuickItem* control : controls) {
            const QRectF rect = control->mapRectToItem(s.view, QRectF(0, 0, control->width(), control->height()));
            QVERIFY2(rect.right() <= 630.0 - 8.0, qPrintable(control->objectName()));
            QVERIFY2(rect.left() >= 8.0 && rect.top() >= 6.0, qPrintable(control->objectName()));
            for (int i = 0; i < rects.size(); ++i)
                QVERIFY2(!rects[i].intersects(rect),
                         qPrintable(control->objectName() + QStringLiteral(" overlaps ") + controls[i]->objectName()));
            rects << rect;
        }
        // The panel's wells sit where the layout leaves room: the meter at the right margin.
        QCOMPARE(s.panel->meterRect().right(), 630.0 - 8.0);
        QVERIFY(s.panel->tubeRect().bottom() < s.drive->mapToItem(s.view, QPointF(0, 0)).y());
        QCOMPARE(s.panel->jewelRect().bottom(), s.view->height() - 6.0);
        // The lamp's and the meter's tooltips cover them whole (the meter's its whole height).
        for (const auto& [name, rect] : {std::pair{QStringLiteral("lampTip"), s.panel->jewelRect()},
                                         std::pair{QStringLiteral("meterTip"), s.panel->meterRect()}}) {
            QQuickItem* tip = find(s.view, name);
            QVERIFY2(tip, qPrintable(name));
            QCOMPARE(tip->mapRectToItem(s.view, QRectF(0, 0, tip->width(), tip->height())), rect);
        }
    }

    // --- The knobs ---------------------------------------------------------------------------------

    void knobsAreBound() {
        const Shown s = showAmp();
        QVERIFY(s.view);
        // The dials read as an amp's: "5.0", the dial unit's (no formatter in the editor).
        for (const QString& id : kDials) {
            KnobItem* knob = knobOf(s.view, id);
            QVERIFY2(knob, qPrintable(id));
            QCOMPARE(knob->value(), 5.0);
            QCOMPARE(knob->from(), 0.0);
            QCOMPARE(knob->to(), 10.0);
            QCOMPARE(readoutOf(s.view, id), QStringLiteral("5.0"));
            auto* p = qvariant_cast<sub::ui::DeviceParam*>(find(s.view, id)->property("param"));
            QVERIFY(p);
            QCOMPARE(p->text(), QStringLiteral("5.0"));
            QCOMPARE(p->format(7.26), QStringLiteral("7.3"));
            QCOMPARE(p->format(10.0), QStringLiteral("10.0"));
            QVERIFY(!find(s.view, id)->property("tooltip").toString().isEmpty());
        }
        QCOMPARE(knobOf(s.view, QStringLiteral("mix"))->value(), 100.0);
        QCOMPARE(readoutOf(s.view, QStringLiteral("mix")), QStringLiteral("100 %"));

        // An edit moves the knob and its readout; undo puts them back.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("gain"), 7.5);
        QCOMPARE(knobOf(s.view, QStringLiteral("gain"))->value(), 7.5);
        QCOMPARE(readoutOf(s.view, QStringLiteral("gain")), QStringLiteral("7.5"));
        undo()->undo();
        QCOMPARE(knobOf(s.view, QStringLiteral("gain"))->value(), 5.0);
        QCOMPARE(readoutOf(s.view, QStringLiteral("gain")), QStringLiteral("5.0"));

        // Dragging Gain up raises it, one undo step for the drag.
        const int steps = undo()->index();
        KnobItem* gain = knobOf(s.view, QStringLiteral("gain"));
        const QPoint at = centerOf(gain);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        for (int dy = 10; dy <= 40; dy += 10)
            dragTo(window_, at - QPoint(0, dy));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 40));
        QVERIFY2(param(s.track, s.device, QStringLiteral("gain")) > 5.3,
                 qPrintable(QString::number(param(s.track, s.device, QStringLiteral("gain")))));
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();
        QCOMPARE(param(s.track, s.device, QStringLiteral("gain")), 5.0);

        // Dry/Wet too.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("mix"), 40.0);
        QCOMPARE(readoutOf(s.view, QStringLiteral("mix")), QStringLiteral("40 %"));
        undo()->undo();
        QCOMPARE(knobOf(s.view, QStringLiteral("mix"))->value(), 100.0);
    }

    // --- The model buttons, the underline, Output -----------------------------------------------------

    void modelButtons() {
        const Shown s = showAmp();
        QVERIFY(s.view && s.panel);
        QQuickItem* clean = find(s.view, QStringLiteral("type_clean"));
        QQuickItem* lead = find(s.view, QStringLiteral("type_lead"));
        auto* underline = find<QQuickItem>(s.view, QStringLiteral("typeUnderline"));
        QVERIFY(clean && lead && underline);
        QVERIFY(clean->property("lit").toBool());
        QCOMPARE(s.panel->model(), 0);
        QCOMPARE(s.panel->modelColor(), ampModelColor(0));
        auto underX = [&](QQuickItem* button) { return button->mapToItem(s.view, QPointF(0, 0)).x(); };
        QVERIFY(std::abs(underline->x() - underX(clean)) < 0.5);
        QVERIFY(!buttonOf(lead)->property("tooltip").toString().isEmpty());

        // A click chooses Lead: lit, one undo step; the colour turns and the underline slides to it.
        const int steps = undo()->index();
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(buttonOf(lead)));
        QCOMPARE(param(s.track, s.device, QStringLiteral("type")), 4.0);
        QVERIFY(lead->property("lit").toBool());
        QVERIFY(!clean->property("lit").toBool());
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(s.panel->model(), 4);
        refreshDisplays();
        ticks(1);
        QVERIFY(s.panel->modelColor() != ampModelColor(4));  // (turning)
        ticks(60);
        QCOMPARE(s.panel->modelColor(), ampModelColor(4));
        QCOMPARE(underline->property("color").value<QColor>(), ampModelColor(4));
        QVERIFY2(std::abs(underline->x() - underX(lead)) < 0.5, qPrintable(QString::number(underline->x())));

        // Undo: Clean again, the underline back under it.
        undo()->undo();
        QCOMPARE(param(s.track, s.device, QStringLiteral("type")), 0.0);
        QVERIFY(clean->property("lit").toBool());
        ticks(30);
        QCOMPARE(s.panel->modelColor(), ampModelColor(0));
        QVERIFY(std::abs(underline->x() - underX(clean)) < 0.5);

        // Output: Mono, or Dual; undoable.
        QQuickItem* mono = find(s.view, QStringLiteral("dual_mono"));
        QQuickItem* dual = find(s.view, QStringLiteral("dual_dual"));
        QVERIFY(mono && dual);
        QVERIFY(mono->property("lit").toBool() && !dual->property("lit").toBool());
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(buttonOf(dual)));
        QCOMPARE(param(s.track, s.device, QStringLiteral("dual")), 1.0);
        QVERIFY(dual->property("lit").toBool() && !mono->property("lit").toBool());
        undo()->undo();
        QCOMPARE(param(s.track, s.device, QStringLiteral("dual")), 0.0);
        QVERIFY(mono->property("lit").toBool());
    }

    // --- The tone curve is the sound ---------------------------------------------------------------

    void toneGraphIsTheSound() {
        const Shown s = showAmp();
        QVERIFY(s.tone);
        const double rate = bridge()->sampleRate();
        const std::vector<double>& freqs = s.tone->frequencies();
        QVERIFY(freqs.size() > 300);
        QVERIFY(std::is_sorted(freqs.begin(), freqs.end()));
        const QList<double> all(freqs.begin(), freqs.end());
        const QList<double> expected = ampToneResponseDb(0, 5, 5, 5, 5, rate, all);
        for (size_t i = 0; i < freqs.size(); i += 17)
            QCOMPARE(s.tone->targetDb()[i], expected[qsizetype(i)]);
        QCOMPARE(s.tone->shownDb(), s.tone->targetDb());
        // The engine's own maths: the make-up puts the stack's peak at noon at 0 dB.
        QVERIFY(std::abs(*std::max_element(expected.begin(), expected.end())) < 0.3);
        QCOMPARE(ampToneResponseDb(0, 5, 5, 5, 5, rate, {700.0})[0],
                 sub::amp::toneResponseDb(sub::amp::voicing(0), 5, 5, 5, 5, rate, 700.0));

        // The handles sit on the curve at their frequencies.
        for (int i = 0; i < AmpToneGraph::kHandles; ++i) {
            const double hz = AmpToneGraph::kHandleList[size_t(i)].frequency;
            const QPointF at = s.tone->handlePos(i);
            QCOMPARE(at.x(), s.tone->xOf(hz));
            QVERIFY(std::abs(at.y() - s.tone->yOf(ampToneResponseDb(0, 5, 5, 5, 5, rate, {hz})[0])) < 1e-9);
        }
        // Handles at least 2 hits apart: a press never has two.
        for (int i = 1; i < AmpToneGraph::kHandles; ++i)
            QVERIFY(s.tone->handlePos(i).x() - s.tone->handlePos(i - 1).x() > 2 * AmpToneGraph::kHitPixels);

        // A dial moves it at once, by what the engine's maths says.
        const double before = ampToneResponseDb(0, 5, 5, 5, 5, rate, {3000.0})[0];
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("treble"), 10.0);
        const double after = ampToneResponseDb(0, 5, 5, 10, 5, rate, {3000.0})[0];
        QVERIFY(after - before > 2.0);
        const auto at3k = size_t(std::lower_bound(freqs.begin(), freqs.end(), 3000.0) - freqs.begin());
        QVERIFY(std::abs(s.tone->shownDb()[at3k] - after) < 1e-9);
        undo()->undo();

        // A new model's curve morphs in.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("type"), 6.0);
        const std::vector<double> target = s.tone->targetDb();
        QCOMPARE(target[at3k], ampToneResponseDb(6, 5, 5, 5, 5, rate, {3000.0})[0]);
        QVERIFY(s.tone->morphing());
        auto apart = [&] {
            double most = 0.0;
            for (size_t i = 0; i < target.size(); ++i)
                most = std::max(most, std::abs(s.tone->shownDb()[i] - target[i]));
            return most;
        };
        const double from = apart();  // (it starts from the curve drawn)
        QVERIFY2(from > 1.0, qPrintable(QString::number(from)));
        ticks(1);
        QVERIFY(apart() < from);
        ticks(30);
        QVERIFY(!s.tone->morphing());
        for (size_t i = 0; i < target.size(); ++i) QVERIFY(std::abs(s.tone->shownDb()[i] - target[i]) < 1e-6);
    }

    // --- The tone handles: drags, fine drags, double-clicks, hover --------------------------------------

    void toneHandlesDrag() {
        const Shown s = showAmp();
        QVERIFY(s.tone);
        auto value = [&](const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); };
        constexpr int kBass = 0, kMiddle = 1, kTreble = 2;

        // Treble's handle up 24 px: 3 steps, one undo step.
        int steps = undo()->index();
        QPoint at = scenePoint(s.tone, s.tone->handlePos(kTreble));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(s.tone->dragging(), kTreble);
        for (int dy = 8; dy <= 24; dy += 8)
            dragTo(window_, at - QPoint(0, dy));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 24));
        QVERIFY2(std::abs(value("treble") - 8.0) < 0.05, qPrintable(QString::number(value("treble"))));
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(s.tone->dragging(), -1);
        undo()->undo();
        QCOMPARE(value("treble"), 5.0);

        // With Shift, finely: 40 px is one step.
        steps = undo()->index();
        at = scenePoint(s.tone, s.tone->handlePos(kTreble));
        QTest::mousePress(window_, Qt::LeftButton, Qt::ShiftModifier, at);
        for (int dy = 10; dy <= 40; dy += 10)
            dragTo(window_, at - QPoint(0, dy), Qt::ShiftModifier);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::ShiftModifier, at - QPoint(0, 40));
        QVERIFY2(std::abs(value("treble") - 6.0) < 0.05, qPrintable(QString::number(value("treble"))));
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();

        // A press away from every handle takes nothing (it goes on to the frame): nothing changes.
        steps = undo()->index();
        const QPointF away(s.tone->xOf(100.0) + 30.0, s.tone->height() / 2);
        QCOMPARE(s.tone->handleAt(away), -1);
        {
            QMouseEvent press(QEvent::MouseButtonPress, away, s.tone->mapToScene(away),
                              QPointF(window_->mapToGlobal(scenePoint(s.tone, away))), Qt::LeftButton, Qt::LeftButton,
                              Qt::NoModifier);
            QCoreApplication::sendEvent(s.tone, &press);
            QVERIFY(!press.isAccepted());
        }
        at = scenePoint(s.tone, away);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(window_, at - QPoint(0, 30));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 30));
        QCOMPARE(undo()->index(), steps);
        for (const char* id : {"bass", "middle", "treble", "presence"}) QCOMPARE(value(id), 5.0);
        // A right-click on a handle gives its parameter's menu (the knob's), and changes nothing.
        QSignalSpy menus(s.tone, &AmpToneGraph::handleMenuRequested);
        QTest::mouseClick(window_, Qt::RightButton, Qt::NoModifier, scenePoint(s.tone, s.tone->handlePos(kBass)));
        QCOMPARE(menus.count(), 1);
        QCOMPARE(menus.at(0).at(0).toString(), QStringLiteral("bass"));
        auto* menu = qvariant_cast<QObject*>(s.view->property("handleMenu"));
        QVERIFY(menu);
        auto* menuParam = qvariant_cast<sub::ui::DeviceParam*>(menu->property("param"));
        QVERIFY(menuParam && menuParam->property("paramId").toString() == QStringLiteral("bass"));
        QTRY_VERIFY(menu->property("visible").toBool());
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("visible").toBool());
        QCOMPARE(undo()->index(), steps);
        QCOMPARE(value("bass"), 5.0);
        // One away from the handles is the frame's (not taken).
        QVERIFY(!sendTo(s.tone, QEvent::MouseButtonPress, away, Qt::RightButton, Qt::RightButton, 1000));
        sendTo(s.tone, QEvent::MouseButtonRelease, away, Qt::RightButton, Qt::NoButton, 1010);
        QCOMPARE(menus.count(), 1);

        // A double-click on a handle puts its dial back to 5, in one undo step.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("middle"), 2.0);
        steps = undo()->index();
        QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 50);
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, scenePoint(s.tone, s.tone->handlePos(kMiddle)));
        QCOMPARE(value("middle"), 5.0);
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();
        QCOMPARE(value("middle"), 2.0);
        // ... even when the second press moves a little before it is let go: it starts no drag.
        at = scenePoint(s.tone, s.tone->handlePos(kMiddle));
        const ulong t = ulong(QTest::lastMouseTimestamp) + 5000;
        send(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton, t);
        send(QEvent::MouseButtonRelease, at, Qt::LeftButton, Qt::NoButton, t + 40);
        send(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton, t + 80);
        send(QEvent::MouseButtonDblClick, at, Qt::LeftButton, Qt::LeftButton, t + 80);
        send(QEvent::MouseMove, at - QPoint(0, 5), Qt::NoButton, Qt::LeftButton, t + 100);
        send(QEvent::MouseButtonRelease, at - QPoint(0, 5), Qt::LeftButton, Qt::NoButton, t + 120);
        QTest::lastMouseTimestamp = int(t + 2000);
        QCOMPARE(value("middle"), 5.0);
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();
        QCOMPARE(value("middle"), 2.0);

        // Hovering a handle grows it and gives the up-and-down cursor; leaving puts both back.
        QTest::mouseMove(window_, scenePoint(s.tone, s.tone->handlePos(kBass)));
        QTRY_COMPARE(s.tone->hovered(), kBass);
        QCOMPARE(s.tone->cursor().shape(), Qt::SizeVerCursor);
        ticks(30);
        QVERIFY(std::abs(s.tone->handleRadius(kBass) - AmpToneGraph::kHoverRadius) < 0.01);
        QCOMPARE(s.tone->handleRadius(kMiddle), AmpToneGraph::kRadius);
        QTest::mouseMove(window_, scenePoint(s.tone, QPointF(s.tone->xOf(100.0) + 30.0, s.tone->height() / 2)));
        QTRY_COMPARE(s.tone->hovered(), -1);
        QVERIFY(s.tone->cursor().shape() != Qt::SizeVerCursor);
        ticks(30);
        QVERIFY(std::abs(s.tone->handleRadius(kBass) - AmpToneGraph::kRadius) < 0.01);
    }

    // The handles are their parameters' controls as the knobs are: Shift mid-drag changing the
    // rate from there on, the wheel, the automation dot and the menu; a double-click away from
    // them is the frame's.
    void handlesAreControls() {
        const Shown s = showAmp();
        QVERIFY(s.view && s.tone);
        auto value = [&](const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); };
        constexpr int kBass = 0, kMiddle = 1, kTreble = 2;

        // Treble up 32 px (to 9.0), then Shift: a pixel more is a fifth of a step's eighth, no jump;
        // Shift let go: a pixel is an eighth of a step again, from there.
        int steps = undo()->index();
        const QPoint t = scenePoint(s.tone, s.tone->handlePos(kTreble));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, t);
        for (int dy = 8; dy <= 32; dy += 8)
            dragTo(window_, t - QPoint(0, dy));
        QVERIFY2(std::abs(value("treble") - 9.0) < 0.006, qPrintable(QString::number(value("treble"))));
        dragTo(window_, t - QPoint(0, 33), Qt::ShiftModifier);
        QVERIFY2(std::abs(value("treble") - 9.025) < 0.006, qPrintable(QString::number(value("treble"))));
        dragTo(window_, t - QPoint(0, 34));
        QVERIFY2(std::abs(value("treble") - 9.15) < 0.006, qPrintable(QString::number(value("treble"))));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, t - QPoint(0, 34));
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();
        QCOMPARE(value("treble"), 5.0);

        // The wheel over a handle: 0.2 a notch (the knob's), a fifth of that with Shift; notches close
        // together are one undo step. Away from the handles it goes on (to the view).
        steps = undo()->index();
        const QPointF b = s.tone->handlePos(kBass);
        QVERIFY(wheelOver(s.tone, b, 1));
        QVERIFY(wheelOver(s.tone, b, 1));
        QVERIFY2(std::abs(value("bass") - 5.4) < 1e-9, qPrintable(QString::number(value("bass"))));
        QVERIFY(wheelOver(s.tone, b, -1, Qt::ShiftModifier));
        QVERIFY2(std::abs(value("bass") - 5.36) < 1e-9, qPrintable(QString::number(value("bass"))));
        QCOMPARE(undo()->index(), steps + 1);
        const QPointF away(s.tone->xOf(100.0) + 30.0, s.tone->height() / 2);
        QVERIFY(!wheelOver(s.tone, away, 1));
        QVERIFY(std::abs(value("bass") - 5.36) < 1e-9);
        undo()->undo();
        QCOMPARE(value("bass"), 5.0);

        // Its parameter's automation shows on the handle as on the knob: the red dot, grey once
        // overridden; its menu is the knob's.
        const QString key = automation::deviceKey(s.device, QStringLiteral("bass"));
        QCOMPARE(s.tone->handleAutomation(kBass), QString());
        editor()->setEnvelope(s.track, key, {{0.0, 0.5, 0.0}, {16.0, 0.5, 0.0}});
        QCOMPARE(s.tone->handleAutomation(kBass), QStringLiteral("on"));
        QCOMPARE(knobOf(s.view, QStringLiteral("bass"))->automation(), QStringLiteral("on"));
        QCOMPARE(s.tone->handleAutomation(kMiddle), QString());
        QTest::qWait(30);
        save(grab(), QStringLiteral("amp-handle-automated.png"));
        bridge()->overrideAutomation(s.track, key);
        QCOMPARE(s.tone->handleAutomation(kBass), QStringLiteral("off"));
        auto* menu = qvariant_cast<QObject*>(s.view->property("handleMenu"));
        QVERIFY(menu);
        QTest::mouseClick(window_, Qt::RightButton, Qt::NoModifier, scenePoint(s.tone, s.tone->handlePos(kBass)));
        QTRY_VERIFY(menu->property("visible").toBool());
        QCOMPARE(menuTexts(menu), (QStringList{"Show Automation", "Delete Automation", "Re-Enable Automation"}));
        QMetaObject::invokeMethod(menu, "close");
        QTRY_VERIFY(!menu->property("visible").toBool());
        bridge()->reEnableAutomation(s.track);
        QCOMPARE(s.tone->handleAutomation(kBass), QStringLiteral("on"));
        editor()->clearEnvelope(s.track, key);
        QCOMPARE(s.tone->handleAutomation(kBass), QString());

        // A double-click away from the handles: neither press is the tone graph's, so the frame
        // under it has both and its double-click (Ctrl folds the device). On a handle it takes the
        // second press, its double-click following (Middle back to 5).
        ulong at = ulong(QTest::lastMouseTimestamp) + 5000;
        QVERIFY(!sendTo(s.tone, QEvent::MouseButtonPress, away, Qt::LeftButton, Qt::LeftButton, at));
        sendTo(s.tone, QEvent::MouseButtonRelease, away, Qt::LeftButton, Qt::NoButton, at + 40);
        QVERIFY(!sendTo(s.tone, QEvent::MouseButtonPress, away, Qt::LeftButton, Qt::LeftButton, at + 80));
        QVERIFY(!sendTo(s.tone, QEvent::MouseButtonDblClick, away, Qt::LeftButton, Qt::LeftButton, at + 80));
        sendTo(s.tone, QEvent::MouseButtonRelease, away, Qt::LeftButton, Qt::NoButton, at + 120);
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("middle"), 2.0);
        const QPointF m = s.tone->handlePos(kMiddle);
        at += 5000;
        QVERIFY(sendTo(s.tone, QEvent::MouseButtonPress, m, Qt::LeftButton, Qt::LeftButton, at));
        sendTo(s.tone, QEvent::MouseButtonRelease, m, Qt::LeftButton, Qt::NoButton, at + 40);
        QVERIFY(sendTo(s.tone, QEvent::MouseButtonPress, m, Qt::LeftButton, Qt::LeftButton, at + 80));
        QVERIFY(sendTo(s.tone, QEvent::MouseButtonDblClick, m, Qt::LeftButton, Qt::LeftButton, at + 80));
        sendTo(s.tone, QEvent::MouseButtonRelease, m, Qt::LeftButton, Qt::NoButton, at + 120);
        QTest::lastMouseTimestamp = int(at + 2000);
        QCOMPARE(value("middle"), 5.0);
    }

    // --- The transfer curve is the sound ------------------------------------------------------------

    void driveGraphIsTheTransfer() {
        const Shown s = showAmp();
        QVERIFY(s.drive);
        const double rate = bridge()->sampleRate();
        const std::vector<QPointF>& curve = s.drive->curve();
        QVERIFY(curve.size() >= 100);
        QCOMPARE(curve.front().x(), -1.0);
        QCOMPARE(curve.back().x(), 1.0);
        for (size_t i = 0; i < curve.size(); i += 10)
            QCOMPARE(curve[i].y(), ampTransfer(0, 5, 5, 5, 5, 5, 5, 0.0, rate, {curve[i].x()})[0]);
        QCOMPARE(ampTransfer(3, 7, 4, 6, 5, 5, 8, 1.0, rate, {0.3})[0],
                 sub::amp::transfer(sub::amp::voicing(3), 7, 4, 6, 5, 5, 8, 1.0, rate, 0.3));
        QCOMPARE(s.drive->outputAt(0.5), ampTransfer(0, 5, 5, 5, 5, 5, 5, 0.0, rate, {0.5})[0]);
        // Up is scaled to the curve's reach: its largest output a little under the top.
        const double reach = std::max(std::abs(curve.front().y()), std::abs(curve.back().y()));
        QVERIFY(std::abs(s.drive->outputRange() * AmpDriveGraph::kReach - reach) < 1e-9);
        QVERIFY(s.drive->yOf(reach) > s.drive->plot().top());

        // More Gain: steeper through 0, and it clips sooner.
        const size_t middle = curve.size() / 2;
        auto slope = [&] {
            const std::vector<QPointF>& c = s.drive->curve();
            return (c[middle + 1].y() - c[middle - 1].y()) / (c[middle + 1].x() - c[middle - 1].x());
        };
        const double gentle = slope();
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("gain"), 10.0);
        QVERIFY2(slope() > 2.0 * gentle, qPrintable(QStringLiteral("%1 %2").arg(slope()).arg(gentle)));
        QCOMPARE(s.drive->curve()[middle + 7].y(), ampTransfer(0, 10, 5, 5, 5, 5, 5, 0.0, rate,
                                                               {s.drive->curve()[middle + 7].x()})[0]);
        undo()->undo();
        QCOMPARE(slope(), gentle);
    }

    // --- The displays reach the face ------------------------------------------------------------------

    void displaysReachTheFace() {
        const Shown s = showAmp(tone(220.0, kSampleRate, 0.25));
        QVERIFY(s.panel && s.drive);
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("type"), 4.0);  // Lead
        refreshDisplays();  // (whatever came before)
        const std::vector<float> out = engine_->renderOffline(0.0, kSampleRate / 4);
        refreshDisplays();

        // The input's peak: -12 dBFS, the dot there on the curve.
        QVERIFY2(std::abs(s.drive->inputLevel() - dbOf(0.25)) < 0.5,
                 qPrintable(QString::number(s.drive->inputLevel())));
        QVERIFY2(std::abs(s.drive->dot().x() - s.drive->xOf(0.25)) < 1.0,
                 qPrintable(QString::number(s.drive->dot().x())));
        // Lead drives V2 and V3 far harder than V1: they glow brighter.
        QVERIFY2(s.panel->glowTarget(1) > s.panel->glowTarget(0) && s.panel->glowTarget(2) > s.panel->glowTarget(0),
                 qPrintable(QStringLiteral("%1 %2 %3").arg(s.panel->glowTarget(0)).arg(s.panel->glowTarget(1))
                                .arg(s.panel->glowTarget(2))));
        QVERIFY(s.panel->glowTarget(2) > 0.9);
        // The meter: the output's peak now (a read counts its last 50 ms: what came before is history).
        double peak = 0.0;
        for (size_t i = out.size() - size_t(2 * kSampleRate / 20); i < out.size(); ++i)
            peak = std::max(peak, double(std::abs(out[i])));
        QVERIFY2(std::abs(s.panel->outputLevel() - dbOf(peak)) < 0.5,
                 qPrintable(QStringLiteral("%1 %2").arg(s.panel->outputLevel()).arg(dbOf(peak))));
        // Playing on, the lamp rises with the output and V3 glows nearly full.
        play(0.25, 0.25, 10, 16);
        QVERIFY2(s.panel->lamp() > 0.6, qPrintable(QString::number(s.panel->lamp())));
        QVERIFY2(s.panel->glow(2) > 0.85, qPrintable(QString::number(s.panel->glow(2))));

        // A tick that reads nothing holds what there was (the engine hands values over at its own pace).
        const std::vector<double> held = {s.panel->glowTarget(0), s.panel->glowTarget(1), s.panel->glowTarget(2),
                                          s.panel->glowTarget(3)};
        const double input = s.drive->inputLevel();
        refreshDisplays();
        for (int i = 0; i < AmpPanel::kTubes; ++i) QCOMPARE(s.panel->glowTarget(i), held[size_t(i)]);
        QVERIFY(s.drive->inputLevel() >= input - 1.0);
        // With nothing at all for a while, everything cools to the idle glow.
        ticks(25);
        for (int i = 0; i < AmpPanel::kTubes; ++i) QCOMPARE(s.panel->glowTarget(i), AmpPanel::kIdleGlow);

        // Silence: the tubes cool, the lamp dims back to idle.
        engine_->renderOffline(10.0 * project()->tempo() / 60.0, kSampleRate / 8);  // (past the clip)
        ticks(60);
        for (int i = 0; i < AmpPanel::kTubes; ++i)
            QVERIFY2(s.panel->glow(i) <= 0.2, qPrintable(QString::number(s.panel->glow(i))));
        QVERIFY(s.panel->lamp() < 0.5);
        QVERIFY(s.drive->inputLevel() < -25.0);  // (the dots fall back 18 dB a second)
    }

    // As the supply sags, the drive curve's shoulder is made for the sag (the power stage driven
    // less), the lamp dims and the power tube shows it.
    void sagShows() {
        const Shown s = showAmp(tone(220.0, kSampleRate, 0.25));
        QVERIFY(s.panel && s.drive);
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("type"), 6.0);  // Bass
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("volume"), 10.0);
        refreshDisplays();
        play(0.0, 0.5, 24, 16);
        QVERIFY2(s.panel->sagDb() > 1.0, qPrintable(QString::number(s.panel->sagDb())));
        // The curve as drawn is made for the sag as eased (within its step of 0.05 dB).
        QVERIFY2(std::abs(s.drive->sagDb() - s.panel->sagDb()) < 0.1,
                 qPrintable(QStringLiteral("%1 %2").arg(s.drive->sagDb()).arg(s.panel->sagDb())));
        const double rate = bridge()->sampleRate();
        const std::vector<QPointF>& curve = s.drive->curve();
        QVERIFY(curve.size() >= 100);
        for (size_t i = 0; i < curve.size(); i += 10)
            QCOMPARE(curve[i].y(), ampTransfer(6, 5, 5, 5, 5, 5, 10, s.drive->sagDb(), rate, {curve[i].x()})[0]);
        // Its shoulder lower than with no sag; the plot's scale is the one with none.
        QList<double> xs;
        for (const QPointF& point : curve)
            xs.append(point.x());
        const QList<double> rested = ampTransfer(6, 5, 5, 5, 5, 5, 10, 0.0, rate, xs);
        double lower = 0.0;
        for (size_t i = 0; i < curve.size(); ++i)
            lower = std::max(lower, std::abs(rested[qsizetype(i)]) - std::abs(curve[i].y()));
        QVERIFY2(lower > 0.03 * s.drive->outputRange(),
                 qPrintable(QStringLiteral("%1 of %2").arg(lower).arg(s.drive->outputRange())));
        const double reach = std::max(rested.back(), -rested.front());
        QVERIFY(std::abs(s.drive->outputRange() * AmpDriveGraph::kReach - reach) < 1e-9);
        // The lamp dimmer than the same output would light it with the supply holding up.
        const double holding = AmpPanel::lampFor(s.panel->outputLevel(), 0.0);
        QVERIFY2(s.panel->lamp() < 0.95 * holding,
                 qPrintable(QStringLiteral("%1 %2").arg(s.panel->lamp()).arg(holding)));
    }

    // An editor opened on an amp that isn't Clean shows its model at once: no turn of colour from
    // Clean's, no curve sweeping up from one with every dial at 0.
    void opensOnItsModel() {
        const QString track = audioTrackWith(tone(220.0, kSampleRate / 2, 0.25), QStringLiteral("tone"), 0.5);
        QVERIFY(!track.isEmpty());
        const QString device = editor()->addDevice(track, QStringLiteral("amp"));
        editor()->setDeviceParam(track, device, QStringLiteral("type"), 4.0);  // Lead
        editor()->setDeviceParam(track, device, QStringLiteral("bass"), 7.0);
        QQuickItem* view = show(QStringLiteral("amp"), track, device);
        QVERIFY(view);
        auto* panel = find<AmpPanel>(view, QStringLiteral("ampPanel"));
        auto* toneGraph = find<AmpToneGraph>(view, QStringLiteral("ampToneGraph"));
        auto* drive = find<AmpDriveGraph>(view, QStringLiteral("ampDriveGraph"));
        QVERIFY(panel && toneGraph && drive);
        QCOMPARE(panel->model(), 4);
        QCOMPARE(panel->modelColor(), ampModelColor(4));
        QVERIFY(!toneGraph->morphing());
        QCOMPARE(toneGraph->shownDb(), toneGraph->targetDb());
        const double rate = bridge()->sampleRate();
        const std::vector<double>& freqs = toneGraph->frequencies();
        const auto at100 = size_t(std::lower_bound(freqs.begin(), freqs.end(), 100.0) - freqs.begin());
        QCOMPARE(toneGraph->shownDb()[at100], ampToneResponseDb(4, 7, 5, 5, 5, rate, {100.0})[0]);
        QCOMPARE(drive->curve().back().y(), ampTransfer(4, 5, 7, 5, 5, 5, 5, 0.0, rate, {1.0})[0]);
        ticks(3);
        QCOMPARE(panel->modelColor(), ampModelColor(4));  // (nothing turning)
        QVERIFY(!toneGraph->morphing());
        auto* underline = find<QQuickItem>(view, QStringLiteral("typeUnderline"));
        QVERIFY(underline);
        QCOMPARE(underline->property("color").value<QColor>(), ampModelColor(4));
    }

    // What came while the editor wasn't showing is history: its first look at the displays shows
    // the level now, not the loudest of the backlog (seconds of it), held by the meters' ballistics.
    void backlogIsHistory() {
        // A Lead played loud for a second, then quiet (-50.5 dBFS): rendered before the editor shows.
        std::vector<float> signal = tone(220.0, kSampleRate, 0.9);
        const std::vector<float> quiet = tone(220.0, kSampleRate * 3 / 2, 0.003);
        signal.insert(signal.end(), quiet.begin(), quiet.end());
        const QString track = audioTrackWith(signal, QStringLiteral("tone"), double(signal.size()) / kSampleRate);
        QVERIFY(!track.isEmpty());
        const QString device = editor()->addDevice(track, QStringLiteral("amp"));
        editor()->setDeviceParam(track, device, QStringLiteral("type"), 4.0);
        // (Once the engine plays the clip through the amp: its source and the device load as they will.)
        const auto playsLead = [&] {
            double most = 0.0;
            for (const float v : engine_->renderOffline(0.0, kSampleRate / 20))
                most = std::max(most, double(std::abs(v)));
            return bridge()->engineDeviceId(track, device).has_value() && most > 0.1 && most < 0.5;  // (Lead's level)
        };
        QTRY_VERIFY(playsLead());
        const std::vector<float> out = engine_->renderOffline(0.0, int(signal.size()));
        QQuickItem* view = show(QStringLiteral("amp"), track, device);
        QVERIFY(view);
        auto* panel = find<AmpPanel>(view, QStringLiteral("ampPanel"));
        auto* drive = find<AmpDriveGraph>(view, QStringLiteral("ampDriveGraph"));
        QVERIFY(panel && drive);
        refreshDisplays();
        QVERIFY2(std::abs(drive->inputLevel() - dbOf(0.003)) < 1.0, qPrintable(QString::number(drive->inputLevel())));
        double peak = 0.0;  // the output's over the render's last 50 ms
        for (size_t i = out.size() - size_t(2 * kSampleRate / 20); i < out.size(); ++i)
            peak = std::max(peak, double(std::abs(out[i])));
        QVERIFY2(std::abs(panel->outputLevel() - dbOf(peak)) < 1.0,
                 qPrintable(QStringLiteral("%1 %2").arg(panel->outputLevel()).arg(dbOf(peak))));
        for (int i = 0; i < 3; ++i)  // (the loud second drove every preamp tube to full)
            QVERIFY2(panel->glowTarget(i) < 0.5, qPrintable(QString::number(panel->glowTarget(i))));
    }

    // A face that has settled draws nothing more until something changes.
    void idleDoesNotRepaint() {
        const Shown s = showAmp();
        QVERIFY(s.panel && s.drive && s.tone);
        ticks(90);  // (everything settles)
        const int panel = s.panel->lastStats().frames, drive = s.drive->lastStats().frames,
                  tone = s.tone->lastStats().frames;
        ticks(20);
        QCOMPARE(s.panel->lastStats().frames, panel);
        QCOMPARE(s.drive->lastStats().frames, drive);
        QCOMPARE(s.tone->lastStats().frames, tone);
    }

    // --- The engine has what the editor set --------------------------------------------------------

    void engineHasTheEdits() {
        const Shown s = showAmp();
        QVERIFY(s.view && s.tone);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                          centerOf(buttonOf(find(s.view, QStringLiteral("type_rock")))));
        const QPoint at = centerOf(knobOf(s.view, QStringLiteral("gain")));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(window_, at - QPoint(0, 30));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 30));
        const QPoint t = scenePoint(s.tone, s.tone->handlePos(2));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, t);
        dragTo(window_, t + QPoint(0, 16));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, t + QPoint(0, 16));
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                          centerOf(buttonOf(find(s.view, QStringLiteral("dual_dual")))));

        QCOMPARE(param(s.track, s.device, QStringLiteral("type")), 3.0);
        QVERIFY(param(s.track, s.device, QStringLiteral("gain")) > 5.0);
        QVERIFY(std::abs(param(s.track, s.device, QStringLiteral("treble")) - 3.0) < 0.05);
        QCOMPARE(param(s.track, s.device, QStringLiteral("dual")), 1.0);
        for (const char* id : {"type", "gain", "treble", "dual"})
            QCOMPARE(engineParam(s, id), float(param(s.track, s.device, QString::fromLatin1(id))));
    }

    // --- Screenshots -----------------------------------------------------------------------------

    void screenshots() {
        const Shown s = showAmp(plucks(196.0, kSampleRate * 4, 0.5, 0.5));
        QVERIFY(s.view && s.panel && s.drive);
        save(grab(), QStringLiteral("amp-idle.png"));

        // Lead, Gain 7, plucked: the tubes glow, the dots ride the curve, the trail behind them.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("type"), 4.0);
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("gain"), 7.0);
        play(0.0, 0.36, 22, 16);
        QVERIFY(s.panel->glow(2) > 0.5);
        QTest::qWait(30);
        save(grab(), QStringLiteral("amp.png"));

        // Bass in Dual, turned up: the power tube driven and sagging blue, the lamp dimmed.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("type"), 6.0);
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("dual"), 1.0);
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("volume"), 9.0);
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("bass"), 7.5);
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("treble"), 3.5);
        play(1.0, 0.3, 18, 16);
        QVERIFY(s.panel->sagDb() > 1.0);
        QTest::qWait(30);
        save(grab(), QStringLiteral("amp-bass-dual.png"));

        // Blues, Middle's handle being dragged: its readout over the curve.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("type"), 2.0);
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("dual"), 0.0);
        ticks(20);
        const QPoint m = scenePoint(s.tone, s.tone->handlePos(1));
        QTest::mouseMove(window_, m);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, m);
        dragTo(window_, m + QPoint(0, 10));
        dragTo(window_, m + QPoint(0, 20));
        play(2.0, 0.2, 12, 16);
        QTest::qWait(30);
        save(grab(), QStringLiteral("amp-blues-drag.png"));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, m + QPoint(0, 20));
        QVERIFY(std::abs(param(s.track, s.device, QStringLiteral("middle")) - 2.5) < 0.05);
    }
};

QTEST_MAIN(TestUiAmp)
#include "test_ui_device_editors_amp.moc"
