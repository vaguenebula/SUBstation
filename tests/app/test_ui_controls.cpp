// The shared controls driven like a user would: knobs and value boxes dragged
// (one gesture key per drag), wheeled, double-clicked and typed into; the meter's
// fall and clip light; the oscilloscope's trigger and fade from a fake feed;
// toggle buttons that never take the focus. Run on a display (xvfb here).

#include <QCursor>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QWheelEvent>
#include <QtMath>

#include <cmath>
#include <memory>

#include "Ui.h"
#include "controls/KnobItem.h"
#include "controls/Meter.h"
#include "controls/OscilloscopeItem.h"
#include "controls/ValueBoxItem.h"

using sub::ui::KnobItem;
using sub::ui::Meter;
using sub::ui::OscilloscopeItem;
using sub::ui::ValueBoxItem;

namespace {

// The engine bridge's scope, faked: `written` moves when told to; the samples
// are a sine whose phase the test sets.
class FakeFeed : public QObject {
    Q_OBJECT
    Q_PROPERTY(quint64 scopeWritten READ scopeWritten)

public:
    quint64 written = 0;
    QList<float> samples;
    int reads = 0;

    quint64 scopeWritten() const { return written; }
    Q_INVOKABLE QList<float> scopeSamples(int frames) {
        ++reads;
        return samples.mid(qMax<qsizetype>(0, samples.size() - frames));
    }
};

// The controls' QML (at the end of the file: moc skips what follows a raw string).
extern const char* const kControls;

QPoint center(QQuickItem* item) {
    return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
}

// A move with modifiers (QTest's mouseMove has none), the left button held.
void moveWith(QQuickWindow* window, QPoint pos, Qt::KeyboardModifiers modifiers) {
    QMouseEvent event(QEvent::MouseMove, QPointF(pos), QPointF(window->mapToGlobal(pos)), Qt::NoButton,
                      Qt::LeftButton, modifiers);
    QGuiApplication::sendEvent(window, &event);
}

// Types ASCII text into whatever has the focus (QTest's keyClicks is for widgets).
void typeText(QQuickWindow* window, const char* text) {
    for (const char* c = text; *c; ++c)
        QTest::keyClick(window, *c);
}

void wheel(QQuickWindow* window, QPoint pos, int angle, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QWheelEvent event(QPointF(pos), QPointF(window->mapToGlobal(pos)), QPoint(), QPoint(0, angle), Qt::NoButton,
                      modifiers, Qt::NoScrollPhase, false);
    QGuiApplication::sendEvent(window, &event);
}

}  // namespace

class TestUiControls : public QObject {
    Q_OBJECT

    std::unique_ptr<QQmlEngine> engine_;
    std::unique_ptr<QObject> root_;
    QQuickWindow* window_ = nullptr;

    template <typename T>
    T* item(const char* name) {
        auto* found = window_->findChild<T*>(QString::fromLatin1(name));
        if (!found)
            qWarning() << "no" << name;
        return found;
    }

private slots:
    void initTestCase() {
        if (QGuiApplication::platformName() == QLatin1String("offscreen") ||
            QGuiApplication::platformName() == QLatin1String("minimal"))
            QSKIP("needs a display: the offscreen platform renders Qt Quick in software, without this geometry");
        sub::ui::setUpApplication();
        engine_ = std::make_unique<QQmlEngine>();
        sub::ui::setUpEngine(*engine_);
        QQmlComponent component(engine_.get());
        component.setData(kControls, QUrl(QStringLiteral("qrc:/test/Controls.qml")));
        root_.reset(component.create());
        if (!root_)
            qWarning() << component.errors();
        QVERIFY(root_);
        window_ = qobject_cast<QQuickWindow*>(root_.get());
        QVERIFY(window_);
        // Away from the screen's edges: a drag there makes the cursor jump.
        window_->setPosition(300, 300);
        QVERIFY(QTest::qWaitForWindowExposed(window_));
    }

    void cleanupTestCase() { root_.reset(); }

    void init() {
        QTest::mouseMove(window_, QPoint(500, 20));  // nothing hovered
    }

    // --- Knob ----------------------------------------------------------------------

    void knobDragIsOneGesture() {
        auto* knob = item<KnobItem>("knob");
        QVERIFY(knob);
        knob->setValue(0.5);
        QSignalSpy moved(knob, &KnobItem::moved);
        QSignalSpy touched(knob, &KnobItem::touched);
        const QPoint at = center(knob);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        QCOMPARE(touched.count(), 1);
        QVERIFY(knob->dragging());
        QVERIFY(!QGuiApplication::overrideCursor());  // a click leaves the cursor be
        for (int dy = 20; dy <= 60; dy += 20)
            QTest::mouseMove(window_, at - QPoint(0, dy));
        // The cursor hides while dragging.
        QVERIFY(QGuiApplication::overrideCursor());
        QCOMPARE(QGuiApplication::overrideCursor()->shape(), Qt::BlankCursor);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 60));
        QVERIFY(!QGuiApplication::overrideCursor());
        QVERIFY(!knob->dragging());
        // 60 px up of 600 for the whole range.
        QCOMPARE(moved.count(), 3);
        QVERIFY(qFuzzyCompare(knob->value(), 0.6));
        const QString key = moved.first().at(1).toString();
        QVERIFY(!key.isEmpty());
        for (const QList<QVariant>& args : moved)
            QCOMPARE(args.at(1).toString(), key);
        QVERIFY(knob->relative());

        // Another drag: another key. Down this time, with Shift (6000 px for the range).
        moved.clear();
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        moveWith(window_, at + QPoint(0, 30), Qt::ShiftModifier);
        moveWith(window_, at + QPoint(0, 60), Qt::NoModifier);  // Shift let go: 600 px again from here
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(0, 60));
        QCOMPARE(moved.count(), 2);
        QVERIFY(moved.at(0).at(1).toString() != key);
        QCOMPARE(moved.at(0).at(1).toString(), moved.at(1).at(1).toString());
        QVERIFY(qAbs(knob->value() - (0.6 - 30.0 / 6000 - 30.0 / 600)) < 1e-9);
    }

    void knobValueFromOutsideIsQuiet() {
        auto* knob = item<KnobItem>("knob");
        QSignalSpy moved(knob, &KnobItem::moved);
        QSignalSpy changed(knob, &KnobItem::valueChanged);
        knob->setValue(0.25);
        QCOMPARE(changed.count(), 1);
        QCOMPARE(moved.count(), 0);
        knob->setValue(7.0);  // kept in range
        QCOMPARE(knob->value(), 1.0);
        QCOMPARE(knob->text(), QStringLiteral("1.00"));
    }

    void knobWheel() {
        auto* knob = item<KnobItem>("knob");
        knob->setValue(0.5);
        QSignalSpy moved(knob, &KnobItem::moved);
        wheel(window_, center(knob), 120);
        wheel(window_, center(knob), 120);
        QCOMPARE(moved.count(), 2);
        QVERIFY(qAbs(knob->value() - 0.54) < 1e-9);  // 1/50 of the range a notch
        QVERIFY(moved.at(0).at(1).toString() != moved.at(1).at(1).toString());  // a notch is a gesture
        wheel(window_, center(knob), -240);
        QVERIFY(qAbs(knob->value() - 0.50) < 1e-9);

        auto* still = item<KnobItem>("noWheelKnob");
        QSignalSpy stillMoved(still, &KnobItem::moved);
        wheel(window_, center(still), 120);
        QCOMPARE(stillMoved.count(), 0);
        QCOMPARE(still->value(), 0.5);
    }

    void knobDoubleClickResets() {
        auto* knob = item<KnobItem>("defaultKnob");
        knob->setValue(0.7);
        QSignalSpy moved(knob, &KnobItem::moved);
        QSignalSpy touched(knob, &KnobItem::touched);
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, center(knob));
        QCOMPARE(knob->value(), 0.0);
        QCOMPARE(moved.count(), 1);
        QVERIFY(!knob->relative());
        QVERIFY(touched.count() >= 1);

        // Without a default, back to where it was made.
        auto* plain = item<KnobItem>("knob");
        QCOMPARE(plain->defaultValue(), 0.5);
    }

    void knobLogScaleAndStep() {
        auto* log = item<KnobItem>("logKnob");
        // Evenly in log(value): the middle of 20..20000 is sqrt(20 * 20000).
        QVERIFY(qAbs(log->fraction(std::sqrt(20.0 * 20000.0)) - 0.5) < 1e-9);
        QVERIFY(qAbs(log->fromFraction(0.5) - std::sqrt(20.0 * 20000.0)) < 1e-6);
        QSignalSpy moved(log, &KnobItem::moved);
        log->setValue(20);
        const QPoint at = center(log);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        QTest::mouseMove(window_, at - QPoint(0, 300));  // half the range
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 300));
        QVERIFY(qAbs(log->value() - std::sqrt(20.0 * 20000.0)) < 1e-6);

        auto* stepped = item<KnobItem>("stepKnob");
        QSignalSpy steps(stepped, &KnobItem::moved);
        const QPoint s = center(stepped);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, s);
        for (int dy = 10; dy <= 100; dy += 10)
            QTest::mouseMove(window_, s - QPoint(0, dy));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, s - QPoint(0, 100));
        // 100 px of 600 over 0..10: 1.67 -> 2 steps; only whole steps, each emitted once.
        QCOMPARE(stepped->value(), 7.0);
        for (const QList<QVariant>& args : steps)
            QCOMPARE(args.at(0).toDouble(), std::round(args.at(0).toDouble()));
        QCOMPARE(steps.count(), 2);
    }

    void knobTyping() {
        auto* knob = item<KnobItem>("typedKnob");
        QVERIFY(knob->typeable());
        QVERIFY(!item<KnobItem>("knob")->typeable());
        QSignalSpy requested(knob, &KnobItem::editRequested);
        QSignalSpy moved(knob, &KnobItem::moved);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(knob));
        QVERIFY(knob->hasActiveFocus());  // clicked: typing goes to it
        QTest::keyClick(window_, Qt::Key_5);
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.first().at(0).toString(), QStringLiteral("5"));
        QTest::keyClick(window_, Qt::Key_0);
        QTest::keyClick(window_, Qt::Key_Return);
        QTRY_COMPARE(knob->value(), 50.0);
        QCOMPARE(moved.count(), 1);
        QVERIFY(!knob->relative());
        // Text that doesn't parse changes nothing.
        QVERIFY(!knob->applyTyped(QStringLiteral("loud")));
        QCOMPARE(knob->value(), 50.0);
        // A knob without a parser takes no focus from a click, and ignores digits.
        auto* plain = item<KnobItem>("knob");
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(plain));
        QVERIFY(!plain->hasActiveFocus());
        plain->forceActiveFocus();
        QSignalSpy plainRequested(plain, &KnobItem::editRequested);
        QTest::keyClick(window_, Qt::Key_5);
        QCOMPARE(plainRequested.count(), 0);
    }

    void knobPaintsItsAutomationDot() {
        auto* knob = item<KnobItem>("knob");
        knob->setAutomation(QStringLiteral("on"));
        QTRY_VERIFY(knob->lastStats().frames > 0);
        QTest::qWait(50);
        QImage image = window_->grabWindow();
        const qreal dpr = window_->effectiveDevicePixelRatio();
        const QPointF dot = knob->mapToScene(QPointF(knob->width() - 3.5, 3.5));
        const QRgb pixel = image.pixel(int(dot.x() * dpr), int(dot.y() * dpr));
        QVERIFY2(qRed(pixel) > 200 && qGreen(pixel) < 120, qPrintable(QString::number(pixel, 16)));
        knob->setAutomation(QStringLiteral("off"));
        QTest::qWait(50);
        image = window_->grabWindow();
        const QRgb grey = image.pixel(int(dot.x() * dpr), int(dot.y() * dpr));
        QVERIFY(qAbs(qRed(grey) - qGreen(grey)) < 20 && qRed(grey) > 90);
        knob->setAutomation(QString());
    }

    // --- ValueBox --------------------------------------------------------------------

    void valueBoxParsing() {
        auto parse = [](const char* text) { return ValueBoxItem::parseFloat(QString::fromUtf8(text)); };
        QCOMPARE(parse("12.5"), std::optional<double>(12.5));
        QCOMPARE(parse("  -6 dB "), std::optional<double>(-6.0));
        QCOMPARE(parse("-12.5dB"), std::optional<double>(-12.5));
        QCOMPARE(parse("120bpm"), std::optional<double>(120.0));
        QCOMPARE(parse("120 BPM"), std::optional<double>(120.0));
        QCOMPARE(parse("50 %"), std::optional<double>(50.0));
        QCOMPARE(parse("-inf"), std::optional<double>(-70.0));
        QCOMPARE(parse("-INF dB"), std::optional<double>(-70.0));
        QCOMPARE(parse("inf"), std::optional<double>(-70.0));
        QCOMPARE(parse("1e2"), std::optional<double>(100.0));
        QCOMPARE(parse("+3"), std::optional<double>(3.0));
        QVERIFY(!parse("loud"));
        QVERIFY(!parse(""));
        QVERIFY(!parse("dB"));
        QCOMPARE(ValueBoxItem::parseNumber(QStringLiteral("2 dB")).toDouble(), 2.0);
        QVERIFY(ValueBoxItem::parseNumber(QStringLiteral("x")).isNull());
    }

    void valueBoxDragsByStep() {
        auto* tempo = item<ValueBoxItem>("tempo");
        tempo->setValue(120);
        QSignalSpy moved(tempo, &ValueBoxItem::moved);
        const QPoint at = center(tempo);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        QVERIFY(tempo->dragging());
        for (int dy = 10; dy <= 40; dy += 10)
            QTest::mouseMove(window_, at - QPoint(0, dy));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 40));
        // A quarter of a step a pixel: 40 px x 0.25 x 0.25.
        QCOMPARE(tempo->value(), 122.5);
        QCOMPARE(tempo->text(), QStringLiteral("122.50"));
        QCOMPARE(moved.count(), 4);
        for (const QList<QVariant>& args : moved)
            QCOMPARE(args.at(1).toString(), moved.first().at(1).toString());
        // Fine with Shift: a tenth of that, adding up across small moves (not lost to rounding).
        moved.clear();
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        for (int dy = 2; dy <= 40; dy += 2)
            moveWith(window_, at - QPoint(0, dy), Qt::ShiftModifier);
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 40));
        QCOMPARE(tempo->value(), 122.75);
        QVERIFY(moved.count() >= 1);
    }

    void valueBoxWheelRunIsOneGesture() {
        auto* tempo = item<ValueBoxItem>("tempo");
        tempo->setValue(120);
        QSignalSpy moved(tempo, &ValueBoxItem::moved);
        wheel(window_, center(tempo), 120);
        wheel(window_, center(tempo), 120);
        QCOMPARE(tempo->value(), 125.0);  // ten steps a notch
        QCOMPARE(moved.count(), 2);
        QCOMPARE(moved.at(0).at(1).toString(), moved.at(1).at(1).toString());
        QTest::qWait(700);  // more than 0.6 s: a new gesture
        wheel(window_, center(tempo), -120);
        QCOMPARE(tempo->value(), 122.5);
        QVERIFY(moved.at(2).at(1).toString() != moved.at(0).at(1).toString());
    }

    void valueBoxChoices() {
        auto* box = item<ValueBoxItem>("denominator");
        QCOMPARE(box->value(), 4.0);
        box->setValue(5);  // the nearest choice
        QCOMPARE(box->value(), 4.0);
        box->setValue(7);
        QCOMPARE(box->value(), 8.0);
        box->setValue(4);
        const QPoint at = center(box);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        QTest::mouseMove(window_, at - QPoint(0, 25));  // a choice per 10 px from the press
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 25));
        QCOMPARE(box->value(), 16.0);
        wheel(window_, at, -120);
        QCOMPARE(box->value(), 8.0);
        wheel(window_, at, 120);
        wheel(window_, at, 120);
        wheel(window_, at, 120);
        QCOMPARE(box->value(), 32.0);  // the last
    }

    void valueBoxDoubleClick() {
        // Without a default: the text field, its text all selected.
        auto* tempo = item<ValueBoxItem>("tempo");
        tempo->setValue(120);
        QSignalSpy requested(tempo, &ValueBoxItem::editRequested);
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, center(tempo));
        QCOMPARE(requested.count(), 1);
        QCOMPARE(requested.first().at(0).toString(), QStringLiteral("120.00"));
        QCOMPARE(requested.first().at(1).toBool(), true);
        typeText(window_, "140 bpm");  // replaces the selection
        QTest::keyClick(window_, Qt::Key_Return);
        QTRY_COMPARE(tempo->value(), 140.0);
        QVERIFY(!tempo->relative());

        // With one: resets to it; typing a digit opens the field.
        auto* volume = item<ValueBoxItem>("volume");
        QVERIFY(volume->typeable());
        volume->setValue(-12);
        QSignalSpy moved(volume, &ValueBoxItem::moved);
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, center(volume));
        QCOMPARE(volume->value(), 0.0);
        QCOMPARE(moved.count(), 1);
        QSignalSpy typed(volume, &ValueBoxItem::editRequested);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(volume));
        QTest::keyClick(window_, Qt::Key_Minus);
        QCOMPARE(typed.count(), 1);
        QCOMPARE(typed.first().at(1).toBool(), false);
        typeText(window_, "inf");
        QTest::keyClick(window_, Qt::Key_Return);
        QTRY_COMPARE(volume->value(), -70.0);
    }

    void valueBoxRounds() {
        auto* volume = item<ValueBoxItem>("volume");
        volume->setValue(-3.04);
        QCOMPARE(volume->value(), -3.0);  // one decimal
        volume->setValue(100);
        QCOMPARE(volume->value(), 6.0);
        QCOMPARE(volume->text(), QStringLiteral("6.0"));
    }

    // --- Meter -----------------------------------------------------------------------

    void meterFallsAndClips() {
        auto* meter = item<Meter>("meter");
        meter->reset();
        QCOMPARE(Meter::fraction(0.0), 0.0);
        QVERIFY(qAbs(Meter::fraction(1.0) - 60.0 / 66.0) < 1e-12);  // 0 dB
        QCOMPARE(Meter::fraction(2.0), 1.0);                          // +6 dB: the top
        QCOMPARE(Meter::fraction(0.0005), 0.0);                       // below -60 dB
        meter->setLevels(1.0, 0.5);
        QVERIFY(qAbs(meter->leftLevel() - 60.0 / 66.0) < 1e-12);
        QVERIFY(qAbs(meter->rightLevel() - Meter::fraction(0.5)) < 1e-12);
        QVERIFY(meter->clipped());  // full scale
        // Silence: down 3.5 % of the scale an update.
        meter->setLevels(0.0, 0.0);
        QVERIFY(qAbs(meter->leftLevel() - (60.0 / 66.0 - 0.035)) < 1e-12);
        meter->setLevels(0.0, 0.0);
        QVERIFY(qAbs(meter->leftLevel() - (60.0 / 66.0 - 0.07)) < 1e-12);
        // A louder peak shows at once.
        meter->setLevels(1.0, 0.0);
        QVERIFY(qAbs(meter->leftLevel() - 60.0 / 66.0) < 1e-12);
        for (int i = 0; i < 30; ++i)
            meter->setLevels(0.0, 0.0);
        QCOMPARE(meter->leftLevel(), 0.0);
        QVERIFY(meter->clipped());  // it stays until clicked
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(meter));
        QVERIFY(!meter->clipped());
    }

    // --- Oscilloscope --------------------------------------------------------------

    void scopeTrigger() {
        // The last rising zero crossing that leaves a full window after it.
        std::vector<float> samples(3000);
        for (size_t i = 0; i < samples.size(); ++i)
            samples[i] = float(std::sin(2 * M_PI * (double(i) - 7.5) / 100.0));  // rises through 0 at 7.5, 107.5, ...
        const int start = OscilloscopeItem::trigger(samples.data(), 3000, 1024);
        QCOMPARE(start, 1908);  // the first sample after the last crossing before 3000 - 1024
        QVERIFY(samples[size_t(start) - 1] < 0 && samples[size_t(start)] >= 0);
        QVERIFY(3000 - start >= 1024);
        // No crossing: the newest window.
        std::vector<float> flat(3000, 0.5f);
        QCOMPARE(OscilloscopeItem::trigger(flat.data(), 3000, 1024), 3000 - 1024);
        // Too few samples: from the start.
        QCOMPARE(OscilloscopeItem::trigger(samples.data(), 500, 1024), 0);
    }

    void scopeTrace() {
        // One column per pixel: its highest, then its lowest where they are a pixel apart.
        const float samples[] = {0.5f, -0.5f, 0.0f, 0.0f, 1.5f, -1.5f, 0.25f, 0.25f};
        const auto points = OscilloscopeItem::trace(samples, 8, 1.0, 4, 13.0, 10.0);
        QCOMPARE(int(points.size()), 6);
        QCOMPARE(points[0], QPointF(1.5, 8.0));   // 13 - 0.5 x 10
        QCOMPARE(points[1], QPointF(1.5, 18.0));
        QCOMPARE(points[2], QPointF(2.5, 13.0));  // flat: the top only
        QCOMPARE(points[3], QPointF(3.5, 3.0));   // clipped to +-1
        QCOMPARE(points[4], QPointF(3.5, 23.0));
        QCOMPARE(points[5], QPointF(4.5, 10.5));
        // More columns than samples: a column without its own sample shows its first.
        const auto wide = OscilloscopeItem::trace(samples, 2, 0.0, 4, 0.0, 1.0);
        QCOMPARE(int(wide.size()), 4);
    }

    void scopeReadsAFeed() {
        auto* scope = item<OscilloscopeItem>("scope");
        FakeFeed feed;
        // A sine rising through zero between 7 + 100 k and 8 + 100 k.
        for (int i = 0; i < 2048; ++i)
            feed.samples << float(0.8 * std::sin(2 * M_PI * (i - 7.5) / 100.0));
        feed.written = 2048;
        scope->setFeed(&feed);
        scope->poll();
        QCOMPARE(int(scope->samples().size()), 1024);
        QCOMPARE(scope->samples().front(), feed.samples[1008]);  // the last crossing leaving 1024 after it
        QVERIFY(scope->tracePoints().size() >= 148);
        const int reads = feed.reads;
        // Nothing new: no read, and it shrinks by 0.8 an update.
        const float before = scope->samples()[25];
        scope->poll();
        QCOMPARE(feed.reads, reads);
        QVERIFY(qAbs(scope->samples()[25] - before * 0.8f) < 1e-6f);
        for (int i = 0; i < 60 && !scope->samples().empty(); ++i)
            scope->poll();
        QVERIFY(scope->samples().empty());  // flat, then idle
        // Hidden: it skips its work.
        feed.written += 512;
        scope->setVisible(false);
        scope->poll();
        QCOMPARE(feed.reads, reads);
        scope->setVisible(true);
        scope->poll();
        QCOMPARE(feed.reads, reads + 1);
        scope->setFeed(nullptr);
    }

    // --- ToggleButton ------------------------------------------------------------------

    void toggleButtonNeverTakesFocus() {
        auto* field = item<QQuickItem>("field");
        auto* toggle = item<QQuickItem>("toggle");
        QVERIFY(field && toggle);
        field->forceActiveFocus();
        QSignalSpy toggled(toggle, SIGNAL(toggled()));
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, center(toggle));
        QCOMPARE(toggle->property("checked").toBool(), true);
        QCOMPARE(toggled.count(), 1);
        QVERIFY(field->hasActiveFocus());  // the click left the focus where it was
        QVERIFY(!toggle->hasActiveFocus());
        // Shown from the model: no toggled().
        QMetaObject::invokeMethod(toggle, "setCheckedSilently", Q_ARG(QVariant, false));
        QCOMPARE(toggle->property("checked").toBool(), false);
        QCOMPARE(toggled.count(), 1);
    }
};

QTEST_MAIN(TestUiControls)
#include "test_ui_controls.moc"

namespace {
const char* const kControls = R"QML(
import QtQuick
import QtQuick.Controls
import SUBstation

Window {
    width: 520
    height: 300
    visible: true

    Knob { objectName: "knob"; x: 20; y: 100; width: 40; height: 40; value: 0.5 }
    Knob { objectName: "defaultKnob"; x: 70; y: 100; width: 40; height: 40; from: -1; to: 1; value: 0.25; defaultValue: 0 }
    Knob { objectName: "logKnob"; x: 120; y: 100; width: 40; height: 40; from: 20; to: 20000; value: 1000; logScale: true }
    Knob { objectName: "stepKnob"; x: 170; y: 100; width: 40; height: 40; from: 0; to: 10; step: 1; value: 5 }
    Knob { objectName: "typedKnob"; x: 220; y: 100; width: 40; height: 40; from: 0; to: 100; value: 10
           parser: text => { const v = parseFloat(text); return isNaN(v) ? null : v } }
    Knob { objectName: "noWheelKnob"; x: 270; y: 100; width: 40; height: 40; wheel: false; value: 0.5 }
    ValueBox { objectName: "tempo"; x: 20; y: 180; width: 70; height: 24; from: 20; to: 999; value: 120; step: 0.25; decimals: 2 }
    ValueBox { objectName: "denominator"; x: 100; y: 180; width: 40; height: 24; from: 1; to: 32; value: 4; decimals: 0
               choices: [1, 2, 4, 8, 16, 32] }
    ValueBox { objectName: "volume"; x: 150; y: 180; width: 70; height: 24; from: -70; to: 6; value: -6; step: 0.25
               decimals: 1; defaultValue: 0 }
    Meter { objectName: "meter"; x: 240; y: 170; width: 10; height: 60 }
    Oscilloscope { objectName: "scope"; x: 270; y: 180 }
    ToggleButton { objectName: "toggle"; x: 20; y: 240; text: "S"; role: "solo" }
    TextField { objectName: "field"; x: 100; y: 240; width: 100 }
}
)QML";
}  // namespace
