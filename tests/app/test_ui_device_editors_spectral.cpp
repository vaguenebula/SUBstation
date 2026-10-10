// The Spectral Compressor's editor (ui/qml/devices/editors/SpectralEditor.qml, ui/src/devices/SpectralGraph):
// loaded as the device view loads it, its knobs, value boxes and button bound to their parameters (undoably),
// the display's lines the engine's own (sub::app::spectralThresholdDb; drawn where they are when steep enough to
// leave the plot, their handles on them) and the level figures they cross fading, the Focus band's dim the engine's
// weights (sub::app::spectralFocusWeights) with the figures over it, its handles and edges dragged
// with the mouse (one undo step a drag, Shift finely from where it is pressed), the engine's displays reaching it
// as it renders offline (cuts, lifts, the held cut, the glow, Delta's tint), and the Sidechain badge (its menu
// under it). With
// SUBSTATION_UI_SCREENSHOTS set to a folder, it is saved there idle, at its widest values, with a steep threshold,
// with a signal flowing, lifting, keyed by a sidechain, and with Delta on.

#include <QByteArray>
#include <QFontMetricsF>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <array>
#include <cmath>
#include <random>
#include <tuple>
#include <vector>

#include "EditorHarness.h"
#include "audio/SpectralResponse.h"
#include "controls/KnobItem.h"
#include "devices/DeviceParam.h"
#include "devices/SpectralGraph.h"
#include "model/ParamSpec.h"
#include "theme/Theme.h"

using namespace sub::app;
using namespace sub::ui;
using sub::app::test::kSampleRate;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Pink-ish noise (Paul Kellet's filter on white noise) at `rmsDb` dBFS RMS, with sines of `tonesDb` dBFS peak at
// `tones` Hz on it: a dense mix with resonances poking out, as a spectral compressor is for. The white noise is
// mt19937's own output scaled by hand, the same on every platform (std::uniform_real_distribution's is not).
std::vector<float> pinkNoise(int frames, double rmsDb, const std::vector<double>& tones = {}, double tonesDb = -30.0,
                             unsigned seed = 7) {
    std::mt19937 random(seed);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    std::vector<double> pink(static_cast<size_t>(frames));
    double power = 0.0;
    for (int i = 0; i < frames; ++i) {
        const double w = double(random()) * (2.0 / 4294967295.0) - 1.0;
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        const double value = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362;
        b6 = w * 0.115926;
        pink[size_t(i)] = value;
        power += value * value;
    }
    const double scale = std::pow(10.0, rmsDb / 20.0) / std::sqrt(power / std::max(1, frames));
    const double amplitude = std::pow(10.0, tonesDb / 20.0);
    std::vector<float> out(static_cast<size_t>(frames));
    for (int i = 0; i < frames; ++i) {
        double value = pink[size_t(i)] * scale;
        for (const double hz : tones) value += amplitude * std::sin(2 * kPi * hz * i / kSampleRate);
        out[size_t(i)] = float(value);
    }
    return out;
}

// The display point nearest a frequency.
int pointNear(double hz) {
    return int(std::lround(std::log(hz / 20.0) / std::log(1000.0) * (SpectralGraph::kPoints - 1)));
}

// The median of `values` at the display points from `low` to `high` Hz.
double medianBetween(const std::array<float, SpectralGraph::kPoints>& values, double low, double high) {
    std::vector<double> chosen;
    for (int j = pointNear(low); j <= pointNear(high); ++j) chosen.push_back(values[size_t(j)]);
    std::sort(chosen.begin(), chosen.end());
    return chosen[chosen.size() / 2];
}

}  // namespace

class TestUiDeviceEditorsSpectral : public QObject, public sub::app::test::EditorHarness {
    Q_OBJECT

    QString track_, device_;
    QQuickItem* view_ = nullptr;
    SpectralGraph* graph_ = nullptr;

    int files_ = 0;

    // A track playing `signal` (as stereo) from the start, its audio loaded: its id ("" if it failed). Each
    // signal has a file of its own (the bridge keeps what it loaded by path).
    QString trackPlaying(const std::vector<float>& signal) {
        const QString name = QStringLiteral("signal%1").arg(++files_);
        const QString track = audioTrackWith(signal, name, double(signal.size()) / kSampleRate);
        const QString path = dir_.path(name + QStringLiteral(".wav"));
        if (!QTest::qWaitFor([&] { return !bridge()->isLoading(path); }, 5000))
            return {};
        return track;
    }

    // A track with the device (playing `signal`, if any), its editor shown.
    bool showDevice(const std::vector<float>& signal = {}) {
        track_ = signal.empty() ? editor()->addAudioTrack() : trackPlaying(signal);
        if (track_.isEmpty())
            return false;
        device_ = editor()->addDevice(track_, QStringLiteral("spectral"));
        view_ = show(QStringLiteral("spectral"), track_, device_);
        graph_ = view_ ? find<SpectralGraph>(view_, QStringLiteral("spectralGraph")) : nullptr;
        return view_ && graph_;
    }

    double value(const char* id) { return param(track_, device_, QString::fromLatin1(id)); }
    void set(const char* id, double v) { editor()->setDeviceParam(track_, device_, QString::fromLatin1(id), v); }

    // A Focus edge's value box (ParamBox), and the box inside it.
    QQuickItem* focusBox(const char* id) { return find(view_, QString::fromLatin1(id)); }
    QQuickItem* valueBox(const char* id) {
        QQuickItem* box = focusBox(id);
        return box ? qvariant_cast<QQuickItem*>(box->property("box")) : nullptr;
    }

    // How many columns of the display's plot have the key line's yellow in their bottom rows (where a line along
    // the floor would lie).
    int yellowOnTheFloor() {
        const QImage image = grab();
        const qreal scale = image.devicePixelRatio();
        const QRectF plot = graph_->mapRectToScene(graph_->plot());
        int columns = 0;
        for (int x = int(plot.left() * scale) + 2; x < int(plot.right() * scale) - 2; ++x) {
            bool yellow = false;
            for (int y = int((plot.bottom() - 3) * scale); y <= int(plot.bottom() * scale); ++y) {
                const QColor c = image.pixelColor(x, y);
                yellow = yellow || (c.red() > 150 && c.green() > 120 && c.blue() < 90 && c.green() > c.red() * 0.7);
            }
            columns += yellow ? 1 : 0;
        }
        return columns;
    }

    // How many pixels of the accent's orange the plot's upper `share` holds (the cuts hang there).
    int orangeInTheTop(double share) {
        const QImage image = grab();
        const qreal scale = image.devicePixelRatio();
        const QRectF plot = graph_->mapRectToScene(graph_->plot());
        int pixels = 0;
        for (int y = int(plot.top() * scale) + 1; y < int((plot.top() + share * plot.height()) * scale); ++y) {
            for (int x = int(plot.left() * scale) + 2; x < int(plot.right() * scale) - 2; ++x) {
                const QColor c = image.pixelColor(x, y);
                pixels += c.red() > 90 && c.red() > c.green() + 25 && c.green() > c.blue() + 15 ? 1 : 0;
            }
        }
        return pixels;
    }

    // An EditorKnob's dial.
    KnobItem* knob(const char* id) {
        QQuickItem* cell = find(view_, QString::fromLatin1(id));
        auto* paramKnob = cell ? qvariant_cast<QQuickItem*>(cell->property("knob")) : nullptr;
        return paramKnob ? qvariant_cast<KnobItem*>(paramKnob->property("knob")) : nullptr;
    }

    // The display's clock ticked `count` times, a display tick each (the easing settles in about 120).
    void tick(int count = 120) {
        for (int i = 0; i < count; ++i) refreshDisplays();
    }

    QPoint at(int handle) { return scenePoint(graph_, graph_->handle(handle)); }

    // A line drawn (SpectralGraph::thresholdLine(), belowLine()) at x.
    static double yOn(const QLineF& line, double x) { return line.y1() + line.dy() * (x - line.x1()) / line.dx(); }

    // Where a level maps on the display's axis, not held to the plot.
    double trueY(double db) {
        const QRectF plot = graph_->plot();
        return plot.bottom() -
               (db - SpectralGraph::kFloorDb) / (SpectralGraph::kCeilingDb - SpectralGraph::kFloorDb) * plot.height();
    }

    // A handle is on `line` as drawn, inside the plot.
    bool onTheDrawnLine(int handle, const QLineF& line) {
        const QPointF point = graph_->handle(handle);
        const bool on = std::abs(point.y() - yOn(line, point.x())) < 0.5 && graph_->plot().contains(point);
        if (!on)
            qWarning("handle %d at (%.1f, %.1f), the line there at %.1f", handle, point.x(), point.y(),
                     yOn(line, point.x()));
        return on;
    }

    // The middle row of the threshold's orange core in the display's column at `hz` (scene px; -1: none).
    double orangeRowAt(double hz) {
        const QImage image = grab();
        const qreal scale = image.devicePixelRatio();
        const QRectF plot = graph_->mapRectToScene(graph_->plot());
        const int x = int(std::lround(graph_->mapToScene(QPointF(graph_->xOf(hz), 0)).x() * scale));
        double rows = 0.0;
        int count = 0;
        for (int y = int(plot.top() * scale); y <= int(plot.bottom() * scale); ++y) {
            const QColor c = image.pixelColor(x, y);
            if (c.red() > 180 && c.green() > 100 && c.blue() < 90) {
                rows += y;
                ++count;
            }
        }
        return count ? (rows / count + 0.5) / scale : -1.0;
    }

    // A drag from `from` by `by` in `steps` moves, with `modifiers`.
    void drag(QPoint from, QPoint by, Qt::KeyboardModifiers modifiers = Qt::NoModifier, int steps = 3) {
        QTest::mousePress(window_, Qt::LeftButton, modifiers, from);
        for (int i = 1; i <= steps; ++i) dragTo(from + by * i / steps, modifiers);
        QTest::mouseRelease(window_, Qt::LeftButton, modifiers, from + by);
    }

    // The engine's value of the device's parameter.
    float engineValue(const char* id) {
        const auto processor = bridge()->engineDeviceId(track_, device_);
        if (!processor)
            return -9999.f;
        return engine()->processorParam(*processor, engine()->processorParamIndex(*processor, id));
    }

private Q_SLOTS:
    void initTestCase() {
        if (!haveDisplay())
            QSKIP("needs a display: the offscreen platform renders Qt Quick in software, without this geometry");
        startHost();
    }

    void cleanupTestCase() { stopHost(); }

    void init() { clearHost(); }

    // --- Layout ------------------------------------------------------------------------------------

    void layout() {
        QVERIFY(showDevice());
        QVERIFY2(view_->implicitHeight() <= bodyHeight(),
                 qPrintable(QStringLiteral("%1 > %2").arg(view_->implicitHeight()).arg(bodyHeight())));

        // Thirteen knobs, each reading its parameter's default, bound to it.
        const std::vector<std::pair<const char*, double>> defaults{
            {"threshold", -18.0}, {"ratio", 2.0}, {"below", -48.0}, {"upward", 1.0}, {"tilt", 0.0},
            {"knee", 6.0},        {"range", 24.0}, {"smooth", 40.0}, {"attack", 20.0}, {"release", 150.0},
            {"link", 100.0},      {"mix", 100.0},  {"output", 0.0}};
        const QRectF graphRect = graph_->mapRectToScene(QRectF(0, 0, graph_->width(), graph_->height()));
        for (const auto& [id, expected] : defaults) {
            KnobItem* dial = knob(id);
            QVERIFY2(dial, id);
            QCOMPARE(dial->value(), expected);
            QQuickItem* cell = find(view_, QString::fromLatin1(id));
            auto* p = qvariant_cast<sub::ui::DeviceParam*>(cell->property("param"));
            QVERIFY2(p && p->valid() && p->paramId() == QString::fromLatin1(id), id);
            const QRectF rect = cell->mapRectToScene(QRectF(0, 0, cell->width(), cell->height()));
            QVERIFY2(!rect.intersects(graphRect), id);  // nothing over the display
            QVERIFY2(rect.top() >= 1 + 6 - 0.5 && rect.bottom() <= 1 + view_->height() - 6 + 0.5, id);  // the margins
            QVERIFY2(rect.left() >= 1 + 8 - 0.5 && rect.right() <= 1 + view_->width() - 8 + 0.5, id);
        }
        for (const char* id : {"ratio", "upward", "attack", "release"}) QVERIFY2(knob(id)->logScale(), id);
        for (const char* id : {"threshold", "below", "knee", "range", "smooth", "link", "mix"})
            QVERIFY2(!knob(id)->logScale() && !knob(id)->bipolar(), id);
        QVERIFY(knob("tilt")->bipolar() && knob("output")->bipolar());

        // The Focus edges' value boxes: bound to their parameters, at the right beside the display, level with the
        // knobs of their rows.
        for (const auto& [id, rowKnob, expected] : {std::tuple{"focus_lo", "attack", 20.0},
                                                   std::tuple{"focus_hi", "mix", 20000.0}}) {
            QQuickItem* box = focusBox(id);
            QVERIFY2(box, id);
            auto* p = qvariant_cast<sub::ui::DeviceParam*>(box->property("param"));
            QVERIFY2(p && p->valid() && p->paramId() == QString::fromLatin1(id), id);
            QCOMPARE(valueBox(id)->property("value").toDouble(), expected);
            auto* dial = qvariant_cast<QQuickItem*>(find(view_, QString::fromLatin1(rowKnob))->property("knob"));
            QVERIFY2(std::abs(centerOf(box).y() - centerOf(dial).y()) <= 2, id);
            const QRectF rect = box->mapRectToScene(QRectF(0, 0, box->width(), box->height()));
            QVERIFY2(!rect.intersects(graphRect) && rect.left() > graphRect.right() && rect.left() < centerOf(dial).x(),
                     id);
            QVERIFY2(rect.right() <= 1 + view_->width() - 8 + 0.5, id);
            // Wide enough for the widest value ("20.00 kHz", centred) clear of the automation dot (x 3.5..8.5).
            const double widest =
                QFontMetricsF(uiFont(8)).horizontalAdvance(formatValue(20000.0, QStringLiteral("Hz")));
            QVERIFY2((box->width() - widest) / 2 >= 8.5 + 2.0,
                     qPrintable(QStringLiteral("%1: %2 px for %3").arg(id).arg(box->width()).arg(widest)));
            QVERIFY2(box->width() <= widest + 2 * 11 + 1, id);  // (and no wider)
        }

        // Delta: a switch, off, level with the knobs beside it.
        QQuickItem* delta = find(view_, QStringLiteral("delta"));
        QVERIFY(delta);
        QVERIFY(!delta->property("lit").toBool());
        auto* mixDial = qvariant_cast<QQuickItem*>(find(view_, QStringLiteral("mix"))->property("knob"));
        QVERIFY(std::abs(centerOf(delta).y() - centerOf(mixDial).y()) <= 4);

        // The display between the knobs: 376 px, its plot roomy enough, the badge clear of the header's text.
        QCOMPARE(graph_->width(), double(SpectralGraph::kWidth));
        const QRectF plot = graph_->plot();
        QVERIFY2(plot.width() >= 300 && plot.height() >= 90,
                 qPrintable(QStringLiteral("%1 x %2").arg(plot.width()).arg(plot.height())));
        QVERIFY(graph_->x() >= 0 && graph_->x() + graph_->width() <= view_->width());
        QVERIFY(graph_->y() >= 6 && graph_->y() + graph_->height() <= view_->height() - 6 + 0.5);
        // Below: dimmed while Upward is 1:1 (it does nothing then), still settable.
        QQuickItem* below = find(view_, QStringLiteral("below"));
        QCOMPARE(below->opacity(), 0.55);
        QVERIFY(below->isEnabled());
        set("upward", 2.0);
        QTRY_COMPARE(below->opacity(), 1.0);
        undo()->undo();
        QTRY_COMPARE(below->opacity(), 0.55);

        QQuickItem* badge = find(view_, QStringLiteral("sidechainBadge"));
        QVERIFY(badge);
        const QRectF badgeRect = badge->mapRectToScene(QRectF(0, 0, badge->width(), badge->height()));
        QVERIFY(badgeRect.right() < graphRect.left() + SpectralGraph::kHeaderLeft);
        QVERIFY(badgeRect.bottom() <= graph_->mapToScene(plot.topLeft()).y() + 0.5);  // over the header, not the plot
        QVERIFY(!badge->property("lit").toBool());

        // Every name and value reads whole, the widest too ("Stereo Link", "-0.5 dB/oct").
        QCOMPARE(formatValue(-1.5, QStringLiteral("dB/oct")), QStringLiteral("-1.5 dB/oct"));
        QCOMPARE(formatValue(0.0, QStringLiteral("dB/oct")), QStringLiteral("0.0 dB/oct"));
        const int before = undo()->index();
        for (const auto& [id, v] : std::vector<std::pair<const char*, double>>{
                 {"threshold", -72.0}, {"below", -72.0}, {"tilt", -0.5}, {"ratio", 20.0}, {"upward", 10.0},
                 {"release", 5000.0}, {"attack", 1000.0}, {"output", -24.0}, {"smooth", 100.0}, {"range", 48.0}})
            set(id, v);
        for (const auto& entry : defaults) {
            const char* id = entry.first;
            QQuickItem* cell = find(view_, QString::fromLatin1(id));
            int texts = 0;
            for (QQuickItem* child : cell->childItems()) {
                if (child->property("truncated").isValid()) {
                    ++texts;
                    QVERIFY2(!child->property("truncated").toBool(), qPrintable(child->property("text").toString()));
                }
            }
            QCOMPARE(texts, 2);  // its name and its value
        }
        tick();  // (the lines glided there)
        QTRY_COMPARE(below->opacity(), 1.0);  // (Upward 10:1: Below no longer dimmed)
        QTest::qWait(50);
        save(grab(), QStringLiteral("spectral-widest.png"));
        while (undo()->index() > before) undo()->undo();
        tick();
        QTRY_COMPARE(below->opacity(), 0.55);
        QTest::qWait(50);
        save(grab(), QStringLiteral("spectral-idle.png"));
    }

    // --- Knobs and the button ------------------------------------------------------------------------

    void knobsAreUndoable() {
        QVERIFY(showDevice());
        const std::vector<std::pair<const char*, double>> changes{
            {"threshold", -40.0}, {"ratio", 8.0}, {"below", -60.0}, {"upward", 2.0}, {"tilt", -1.5},
            {"knee", 12.0},       {"range", 12.0}, {"smooth", 80.0}, {"attack", 5.0}, {"release", 400.0},
            {"link", 50.0},       {"mix", 70.0},  {"output", -3.0}};
        for (const auto& [id, v] : changes) {
            KnobItem* dial = knob(id);
            QVERIFY2(dial, id);
            const double before = dial->value();
            set(id, v);
            QCOMPARE(dial->value(), v);
            undo()->undo();
            QCOMPARE(dial->value(), before);
        }

        // A drag on a knob: the parameter, one undo step; undo.
        const int steps = undo()->index();
        auto* thresholdKnob = qvariant_cast<QQuickItem*>(find(view_, QStringLiteral("threshold"))->property("knob"));
        drag(centerOf(thresholdKnob), QPoint(0, -40), Qt::NoModifier, 4);
        QVERIFY2(value("threshold") > -18.0, qPrintable(QString::number(value("threshold"))));
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();
        QCOMPARE(value("threshold"), -18.0);

        // The Focus boxes: each shows its parameter; a drag up on one raises it (evenly in log frequency), one
        // undo step; undo.
        set("focus_hi", 8000.0);
        QCOMPARE(valueBox("focus_hi")->property("value").toDouble(), 8000.0);
        undo()->undo();
        QCOMPARE(valueBox("focus_hi")->property("value").toDouble(), 20000.0);
        drag(centerOf(valueBox("focus_lo")), QPoint(0, -30), Qt::NoModifier, 3);
        QVERIFY2(value("focus_lo") > 25.0, qPrintable(QString::number(value("focus_lo"))));
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(engineValue("focus_lo"), float(value("focus_lo")));
        undo()->undo();
        QCOMPARE(value("focus_lo"), 20.0);

        // Delta: a click switches it on (lit), one undo step; undo.
        QQuickItem* delta = find(view_, QStringLiteral("delta"));
        auto* button = qvariant_cast<QQuickItem*>(delta->property("button"));
        QVERIFY(button);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(button));
        QCOMPARE(value("delta"), 1.0);
        QVERIFY(delta->property("lit").toBool());
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(engineValue("delta"), 1.f);
        undo()->undo();
        QCOMPARE(value("delta"), 0.0);
        QVERIFY(!delta->property("lit").toBool());
    }

    // --- The display's lines -------------------------------------------------------------------------

    void linesAreTheEngines() {
        QVERIFY(showDevice());
        const QList<double> frequencies{20.0, 100.0, 1000.0, 10000.0, 20000.0};
        auto check = [&] {
            tick();
            const QList<double> threshold = spectralThresholdDb(value("threshold"), value("tilt"), frequencies);
            const QList<double> below =
                spectralBelowDb(value("threshold"), value("below"), value("tilt"), frequencies);
            for (int i = 0; i < frequencies.size(); ++i) {
                QVERIFY2(std::abs(graph_->thresholdAt(frequencies[i]) - threshold[i]) < 1e-6,
                         qPrintable(QString::number(graph_->thresholdAt(frequencies[i]))));
                QVERIFY2(std::abs(graph_->belowAt(frequencies[i]) - below[i]) < 1e-6,
                         qPrintable(QString::number(graph_->belowAt(frequencies[i]))));
            }
        };
        check();
        QCOMPARE(graph_->thresholdAt(1000.0), -18.0);
        QCOMPARE(graph_->thresholdAt(20.0), -18.0);  // flat: it follows pink noise
        set("upward", 2.0);
        set("below", -40.0);
        check();
        set("tilt", 3.0);
        check();
        QVERIFY(std::abs(graph_->thresholdAt(10000.0) - (-18.0 + 3.0 * std::log2(10.0))) < 1e-6);
        set("below", 0.0);  // over the threshold: drawn on it (the engine's min)
        check();
        for (const double hz : frequencies) QVERIFY(std::abs(graph_->belowAt(hz) - graph_->thresholdAt(hz)) < 1e-9);

        // An undo glides the line back (no jump), then it is the engine's again.
        undo()->undo();
        tick(10);
        QVERIFY(graph_->belowAt(1000.0) < graph_->thresholdAt(1000.0));
        QVERIFY(graph_->belowAt(1000.0) > -40.0);  // on its way
        check();

        // The Focus edges as drawn: the parameters'.
        set("focus_lo", 120.0);
        set("focus_hi", 8000.0);
        tick();
        QVERIFY(std::abs(graph_->focusLowShown() - 120.0) < 1e-3);
        QVERIFY(std::abs(graph_->focusHighShown() - 8000.0) < 1e-2);
        const QList<double> weights = spectralFocusWeights(120.0, 8000.0, {60.0, 120.0, 1000.0, 8000.0, 16000.0});
        QCOMPARE(weights[0], 0.0);
        QCOMPARE(weights[1], 1.0);
        QCOMPARE(weights[4], 0.0);
    }

    // --- Dragging ----------------------------------------------------------------------------------

    void dragTheThreshold() {
        QVERIFY(showDevice());
        tick();
        const double dbPerPixel = graph_->dbPerPixel();

        // Hovering the pivot: its readout.
        QTest::mouseMove(window_, at(SpectralGraph::ThresholdHandle));
        QTRY_COMPARE(graph_->hoveredHandle(), int(SpectralGraph::ThresholdHandle));
        QCOMPARE(graph_->readout(), QStringLiteral("Threshold ") + formatValue(-18.0, QStringLiteral("dB")));

        // The pivot dragged up 30 px: up as far, one undo step, the tilt left alone.
        int steps = undo()->index();
        drag(at(SpectralGraph::ThresholdHandle), QPoint(0, -30));
        QVERIFY2(std::abs(value("threshold") - (-18.0 + 30 * dbPerPixel)) <= 0.2,
                 qPrintable(QString::number(value("threshold"))));
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(value("tilt"), 0.0);
        QCOMPARE(engineValue("threshold"), float(value("threshold")));

        // Anywhere on the line (at 300 Hz), down 20 px.
        double before = value("threshold");
        steps = undo()->index();
        const QPoint line =
            scenePoint(graph_, QPointF(graph_->xOf(300.0), graph_->yOfLevel(graph_->thresholdAt(300.0))));
        drag(line, QPoint(0, 20));
        QVERIFY2(std::abs(value("threshold") - (before - 20 * dbPerPixel)) <= 0.2,
                 qPrintable(QString::number(value("threshold"))));
        QCOMPARE(undo()->index(), steps + 1);

        // Shift: a tenth as far.
        before = value("threshold");
        drag(at(SpectralGraph::ThresholdHandle), QPoint(0, -30), Qt::ShiftModifier);
        QVERIFY2(std::abs(value("threshold") - (before + 3 * dbPerPixel)) <= 0.11,
                 qPrintable(QString::number(value("threshold"))));

        // Shift pressed partway through a drag slows what follows, from where the mouse is: the value doesn't jump
        // back towards where the drag began (nor forward when Shift is let go).
        set("threshold", -40.0);
        tick();
        before = value("threshold");
        QPoint mouse = at(SpectralGraph::ThresholdHandle);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, mouse);
        for (int i = 0; i < 4; ++i) dragTo(mouse -= QPoint(0, 10));
        const double fast = value("threshold");
        QVERIFY2(std::abs(fast - (before + 40 * dbPerPixel)) <= 0.2, qPrintable(QString::number(fast)));
        dragTo(mouse -= QPoint(0, 1), Qt::ShiftModifier);
        QVERIFY2(std::abs(value("threshold") - (fast + 0.1 * dbPerPixel)) <= 0.11,
                 qPrintable(QString::number(value("threshold"))));
        for (int i = 0; i < 10; ++i) dragTo(mouse -= QPoint(0, 1), Qt::ShiftModifier);
        const double fine = value("threshold");
        QVERIFY2(std::abs(fine - (fast + 1.1 * dbPerPixel)) <= 0.11, qPrintable(QString::number(fine)));
        dragTo(mouse -= QPoint(0, 1));
        QVERIFY2(std::abs(value("threshold") - (fine + dbPerPixel)) <= 0.11,
                 qPrintable(QString::number(value("threshold"))));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, mouse);

        // A double-click on the pivot: back to -18, one undo step.
        tick();
        steps = undo()->index();
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, at(SpectralGraph::ThresholdHandle));
        QCOMPARE(value("threshold"), -18.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(undo()->undoText(), QStringLiteral("Reset Threshold"));
    }

    void tiltByTheHandles() {
        QVERIFY(showDevice());
        tick();
        const double dbPerPixel = graph_->dbPerPixel();
        const double octaves = std::log2(10000.0 / 1000.0);

        // The 10 kHz end up 20 px: that end rises, turning the line about 1 kHz.
        int steps = undo()->index();
        drag(at(SpectralGraph::TiltHigh), QPoint(0, -20));
        QVERIFY2(std::abs(value("tilt") - 20 * dbPerPixel / octaves) <= 0.02,
                 qPrintable(QString::number(value("tilt"))));
        QCOMPARE(value("threshold"), -18.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(engineValue("tilt"), float(value("tilt")));

        // The 100 Hz end up: the tilt falls.
        const double before = value("tilt");
        drag(at(SpectralGraph::TiltLow), QPoint(0, -20));
        QVERIFY2(std::abs(value("tilt") - (before - 20 * dbPerPixel / octaves)) <= 0.02,
                 qPrintable(QString::number(value("tilt"))));

        // The handles ride the line as drawn, within the plot (more in linesLeavingThePlot).
        set("tilt", 6.0);
        set("threshold", 12.0);
        tick();
        const QRectF plot = graph_->plot();
        for (const int handle :
             {int(SpectralGraph::TiltLow), int(SpectralGraph::ThresholdHandle), int(SpectralGraph::TiltHigh)})
            QVERIFY(onTheDrawnLine(handle, graph_->thresholdLine()));
        QCOMPARE(graph_->handle(SpectralGraph::TiltHigh).y(), plot.top());  // (+12 + 20 dB: off the top)

        // A double-click: level with pink again.
        set("threshold", -18.0);
        tick();
        steps = undo()->index();
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, at(SpectralGraph::TiltHigh));
        QCOMPARE(value("tilt"), 0.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(undo()->undoText(), QStringLiteral("Reset Tilt"));
    }

    // A line steep enough to leave the plot is drawn where it is up to the edge it leaves by (the plot clips it; its
    // ends aren't held to the axis, which bent it), each handle stays on it inside the plot (one whose own frequency
    // is off the axis where the line leaves), and the mouse finds the line only where it is drawn.
    void linesLeavingThePlot() {
        QVERIFY(showDevice());
        const QRectF plot = graph_->plot();
        const QList<double> frequencies{20.0, 100.0, 300.0, 1000.0, 3000.0, 10000.0, 20000.0};
        struct Steep {
            double threshold, tilt;
            int handle;     // the handle off the axis at its own frequency
            double leaves;  // where the line leaves the plot on its way there (Hz)
            bool top;       // by the top (else the floor)
        };
        // E.g. -60 dB, +6 dB/oct: 100 Hz is at -79.9 dB, under the -78 dB floor, which the line meets at 125 Hz.
        for (const Steep& steep : {Steep{-60.0, 6.0, SpectralGraph::TiltLow, 125.0, false},
                                   Steep{0.0, 6.0, SpectralGraph::TiltHigh, 8000.0, true},
                                   Steep{-72.0, -6.0, SpectralGraph::TiltHigh, 2000.0, false},
                                   Steep{12.0, -6.0, SpectralGraph::TiltLow, 500.0, true}}) {
            set("threshold", steep.threshold);
            set("tilt", steep.tilt);
            tick();
            const QLineF line = graph_->thresholdLine();
            QCOMPARE(line.x1(), plot.left());
            QCOMPARE(line.x2(), plot.right());
            for (const double hz : frequencies) {
                const double level = steep.threshold + steep.tilt * std::log2(hz / 1000.0);
                QVERIFY2(std::abs(yOn(line, graph_->xOf(hz)) - trueY(level)) < 1e-6,
                         qPrintable(QStringLiteral("%1 dB, %2 dB/oct at %3 Hz").arg(steep.threshold)
                                        .arg(steep.tilt).arg(hz)));
            }
            const QPointF pivot = graph_->handle(SpectralGraph::ThresholdHandle);
            QVERIFY(std::abs(pivot.x() - graph_->xOf(1000.0)) < 1e-6);
            QVERIFY(std::abs(pivot.y() - trueY(steep.threshold)) < 1e-6);
            for (const int handle :
                 {int(SpectralGraph::ThresholdHandle), int(SpectralGraph::TiltLow), int(SpectralGraph::TiltHigh)})
                QVERIFY(onTheDrawnLine(handle, line));
            const QPointF off = graph_->handle(steep.handle);
            QCOMPARE(off.y(), steep.top ? plot.top() : plot.bottom());
            QVERIFY2(std::abs(off.x() - graph_->xOf(steep.leaves)) < 0.5, qPrintable(QString::number(off.x())));
        }

        // The Below line likewise (-72 dB, +6 dB/oct: 300 Hz is at -82.4 dB; the line meets the floor at 500 Hz).
        set("threshold", -30.0);
        set("tilt", 6.0);
        set("upward", 2.0);
        set("below", -72.0);
        tick();
        const QLineF below = graph_->belowLine();
        QVERIFY(std::abs(below.y1() - trueY(-72.0 + 6.0 * std::log2(20.0 / 1000.0))) < 1e-6);
        QVERIFY(onTheDrawnLine(SpectralGraph::BelowHandle, below));
        QCOMPARE(graph_->handle(SpectralGraph::BelowHandle).y(), plot.bottom());
        QVERIFY(std::abs(graph_->handle(SpectralGraph::BelowHandle).x() - graph_->xOf(500.0)) < 0.5);
        for (const int handle :
             {int(SpectralGraph::ThresholdHandle), int(SpectralGraph::TiltLow), int(SpectralGraph::TiltHigh)})
            QVERIFY(onTheDrawnLine(handle, graph_->thresholdLine()));

        // Painted where it is: at -60 dB, +6 dB/oct the orange line crosses 300 Hz at -70.4 dB (bent onto the
        // floor at 20 Hz it crossed at -60.8, 11 px higher).
        set("upward", 1.0);
        set("threshold", -60.0);
        tick();
        QTest::qWait(50);
        const double expected = graph_->mapToScene(QPointF(0, trueY(-60.0 + 6.0 * std::log2(0.3)))).y();
        const double drawn = orangeRowAt(300.0);
        QVERIFY2(std::abs(drawn - expected) < 1.5,
                 qPrintable(QStringLiteral("%1 against %2").arg(drawn).arg(expected)));
        save(grab(), QStringLiteral("spectral-steep.png"));

        // The mouse: nothing over the floor where the line has gone under it (at 40 Hz it is at -87.9 dB); the line
        // where it is drawn.
        QTest::mouseMove(window_, scenePoint(graph_, QPointF(graph_->xOf(40.0), plot.bottom() - 2)));
        QTest::qWait(20);
        QCOMPARE(graph_->hoveredHandle(), int(SpectralGraph::None));
        const QPointF onLine(graph_->xOf(300.0), yOn(graph_->thresholdLine(), graph_->xOf(300.0)));
        QTest::mouseMove(window_, scenePoint(graph_, onLine + QPointF(0, 3)));
        QTRY_COMPARE(graph_->hoveredHandle(), int(SpectralGraph::ThresholdHandle));
        QTest::mouseMove(window_, scenePoint(graph_, QPointF(plot.center().x(), plot.top() + 4)));
        QTRY_COMPARE(graph_->hoveredHandle(), int(SpectralGraph::None));

        // A handle held at the edge still drags as it does on its own frequency: 100 Hz's up 20 px.
        const double dbPerPixel = graph_->dbPerPixel();
        const int steps = undo()->index();
        drag(at(SpectralGraph::TiltLow), QPoint(0, -20));
        QVERIFY2(std::abs(value("tilt") - (6.0 - 20 * dbPerPixel / std::log2(10.0))) <= 0.02,
                 qPrintable(QString::number(value("tilt"))));
        QCOMPARE(value("threshold"), -60.0);
        QCOMPARE(undo()->index(), steps + 1);
    }

    // The level figures at the plot's left: one a threshold runs through fades out (at the defaults the threshold,
    // -18 dB, runs through "−24", which sits just over its line), smoothly as the line moves.
    void figuresStepAside() {
        QVERIFY(showDevice());
        tick();
        QVERIFY2(graph_->figureShown(-24.0) < 0.01, qPrintable(QString::number(graph_->figureShown(-24.0))));
        for (const double db : {0.0, -48.0, -72.0}) QCOMPARE(graph_->figureShown(db), 1.0);
        set("threshold", -30.0);
        tick();
        for (const double db : SpectralGraph::kLevelFigures) QCOMPARE(graph_->figureShown(db), 1.0);
        set("threshold", -24.0);  // on the figure's own line, under it: still read
        tick();
        QVERIFY(graph_->figureShown(-24.0) > 0.5);

        // Fading as the line passes, without a jump.
        double last = graph_->figureShown(-24.0);
        for (double db = -24.0; db <= -12.0; db += 0.1) {
            set("threshold", db);
            tick(1);
            const double shown = graph_->figureShown(-24.0);
            QVERIFY2(std::abs(shown - last) < 0.1, qPrintable(QStringLiteral("%1 at %2 dB").arg(shown).arg(db)));
            last = shown;
        }

        // The Below line too, while it is shown.
        set("threshold", -18.0);
        set("below", -42.0);
        tick();
        QCOMPARE(graph_->figureShown(-48.0), 1.0);  // (Upward 1:1: no Below line)
        set("upward", 2.0);
        tick();
        QVERIFY(graph_->figureShown(-48.0) < 0.01);
    }

    // The Focus band's dim is the engine's weights for the edges drawn: the 0 dB grid line reads full where a
    // frequency gets all of its gain, darkest where it gets none, and between where it gets half. The level figures,
    // at the plot's left, are drawn over the dim: as bright with the band raised as without.
    void focusDimFollowsTheWeights() {
        QVERIFY(showDevice());
        tick();
        QTest::qWait(50);
        // The brightest pixel of a box of the display (graph coordinates).
        const auto brightest = [&](const QImage& image, const QRectF& box) {
            const qreal scale = image.devicePixelRatio();
            const QRect pixels = QRectF(graph_->mapToScene(box.topLeft()) * scale, box.size() * scale).toAlignedRect();
            int most = 0;
            for (int y = pixels.top(); y <= pixels.bottom(); ++y)
                for (int x = pixels.left(); x <= pixels.right(); ++x) most = std::max(most, qGray(image.pixel(x, y)));
            return most;
        };
        const auto figure = [&](double db) {
            return QRectF(graph_->plot().left() + 9, graph_->yOfLevel(db) - 11, 20, 10);
        };
        QImage image = grab();
        const int open72 = brightest(image, figure(-72.0)), open48 = brightest(image, figure(-48.0));

        set("focus_lo", 1200.0);
        tick();
        const double half = 1200.0 / std::exp2(1.0 / 6.0);  // half its gain: half a fade (a third of an octave) out
        const QList<double> weights = spectralFocusWeights(1200.0, 20000.0, {650.0, half, 1500.0});
        QCOMPARE(weights[0], 0.0);
        QVERIFY(std::abs(weights[1] - 0.5) < 1e-9);
        QCOMPARE(weights[2], 1.0);
        QTest::qWait(50);
        image = grab();
        // (Each frequency well clear of the grid's upright lines, a column of the 0 dB line's rows.)
        const auto line = [&](double hz) {
            return brightest(image, QRectF(graph_->xOf(hz), graph_->yOfLevel(0.0) - 2, 0.5, 4));
        };
        const int none = line(650.0), between = line(half), full = line(1500.0);
        QVERIFY2(none + 2 < between && between + 2 < full,
                 qPrintable(QStringLiteral("%1 %2 %3").arg(none).arg(between).arg(full)));
        QVERIFY2(brightest(image, figure(-72.0)) >= open72 - 4 && brightest(image, figure(-48.0)) >= open48 - 4,
                 qPrintable(QStringLiteral("%1 (%2), %3 (%4)").arg(brightest(image, figure(-72.0))).arg(open72)
                                .arg(brightest(image, figure(-48.0))).arg(open48)));
        QVERIFY(open72 > 110 && open48 > 110);
    }

    void dragBelow() {
        QVERIFY(showDevice());
        tick();
        const double dbPerPixel = graph_->dbPerPixel();

        // Upward at 1:1: no Below line to hover or drag.
        const QPoint hidden = at(SpectralGraph::BelowHandle);
        QTest::mouseMove(window_, hidden);
        QTest::qWait(20);
        QVERIFY(graph_->hoveredHandle() != SpectralGraph::BelowHandle);
        int steps = undo()->index();
        drag(hidden, QPoint(0, 20));
        QCOMPARE(value("below"), -48.0);
        QCOMPARE(undo()->index(), steps);

        // Upward on: its handle dragged down 20 px.
        set("upward", 2.0);
        tick();
        steps = undo()->index();
        drag(at(SpectralGraph::BelowHandle), QPoint(0, 20));
        QVERIFY2(std::abs(value("below") - (-48.0 - 20 * dbPerPixel)) <= 0.2,
                 qPrintable(QString::number(value("below"))));
        QCOMPARE(value("threshold"), -18.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(engineValue("below"), float(value("below")));
        QCOMPARE(graph_->readout(), QStringLiteral("Below ") + formatValue(value("below"), QStringLiteral("dB")));

        // Over the threshold it is drawn on it, and a drag starts from there.
        set("below", 0.0);
        tick();
        drag(at(SpectralGraph::BelowHandle), QPoint(0, 10));
        QVERIFY2(std::abs(value("below") - (-18.0 - 10 * dbPerPixel)) <= 0.2,
                 qPrintable(QString::number(value("below"))));
        QCOMPARE(value("threshold"), -18.0);

        // A double-click: back to -48.
        tick();
        steps = undo()->index();
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, at(SpectralGraph::BelowHandle));
        QCOMPARE(value("below"), -48.0);
        QCOMPARE(undo()->index(), steps + 1);
    }

    void dragTheFocusEdges() {
        QVERIFY(showDevice());
        tick();
        const double width = graph_->plot().width();

        // The low edge (open, at 20 Hz) dragged a fifth of the way across: 20 Hz × 1000^0.2.
        int steps = undo()->index();
        drag(at(SpectralGraph::FocusLowEdge), QPoint(int(std::lround(0.2 * width)), 0));
        QVERIFY2(std::abs(value("focus_lo") - 80.0) <= 3.0, qPrintable(QString::number(value("focus_lo"))));
        QCOMPARE(value("focus_hi"), 20000.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(graph_->readout(),
                 QStringLiteral("Focus Low ") + formatValue(value("focus_lo"), QStringLiteral("Hz")));
        QCOMPARE(engineValue("focus_lo"), float(value("focus_lo")));

        // Shift partway through an edge's drag: what follows is finer, nothing jumps back.
        tick();
        const double low = value("focus_lo");
        QPoint grip = at(SpectralGraph::FocusLowEdge);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, grip);
        for (int i = 0; i < 4; ++i) dragTo(grip += QPoint(10, 0));
        const double wide = value("focus_lo");
        QVERIFY2(std::abs(wide - low * std::pow(1000.0, 40.0 / width)) <= 2.0, qPrintable(QString::number(wide)));
        dragTo(grip += QPoint(1, 0), Qt::ShiftModifier);
        QVERIFY2(std::abs(value("focus_lo") - wide * std::pow(1000.0, 0.1 / width)) <= 1.0,
                 qPrintable(QString::number(value("focus_lo"))));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, grip);
        undo()->undo();

        // The high edge dragged left past the low one: held a third of an octave above it.
        tick();
        steps = undo()->index();
        const QPoint high = at(SpectralGraph::FocusHighEdge);
        drag(high, QPoint(int(graph_->xOf(40.0)) - graph_->mapFromScene(high).toPoint().x(), 0), Qt::NoModifier, 6);
        QVERIFY2(std::abs(value("focus_hi") - value("focus_lo") * std::exp2(1.0 / 3.0)) <= 1.0,
                 qPrintable(QString::number(value("focus_hi"))));
        QCOMPARE(undo()->index(), steps + 1);

        // Double-clicks: all the way out again.
        tick();
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, at(SpectralGraph::FocusLowEdge));
        QCOMPARE(value("focus_lo"), 20.0);
        QCOMPARE(undo()->undoText(), QStringLiteral("Reset Focus Low"));
        tick();
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, at(SpectralGraph::FocusHighEdge));
        QCOMPARE(value("focus_hi"), 20000.0);
        QCOMPARE(undo()->undoText(), QStringLiteral("Reset Focus High"));
    }

    void elsewhereDoesNothing() {
        QVERIFY(showDevice());
        tick();
        const int steps = undo()->index(), count = undo()->count();
        const QRectF plot = graph_->plot();
        const QPoint bottom = scenePoint(graph_, QPointF(graph_->xOf(1000.0), plot.bottom() - 3));
        QTest::mouseMove(window_, bottom);
        QTest::qWait(20);
        QCOMPARE(graph_->hoveredHandle(), int(SpectralGraph::None));
        drag(bottom, QPoint(0, -40));
        QCOMPARE(undo()->index(), steps);
        QCOMPARE(undo()->count(), count);
        QCOMPARE(value("threshold"), -18.0);
        QCOMPARE(value("tilt"), 0.0);
        QCOMPARE(value("below"), -48.0);
        QCOMPARE(value("focus_lo"), 20.0);
        QCOMPARE(value("focus_hi"), 20000.0);
    }

    // --- The displays ------------------------------------------------------------------------------

    void displaysReachTheGraph() {
        QVERIFY(showDevice(pinkNoise(kSampleRate, -12.0, {350.0, 1200.0, 3500.0}, -18.0)));
        set("threshold", -40.0);
        tick();
        QCOMPARE(graph_->framesSeen(), qint64(0));

        // Half a second rendered: the frames come, the cuts where the sound is, the meters.
        engine()->renderOffline(0.0, kSampleRate / 2);
        for (int i = 0; i < 10; ++i) refreshDisplays();
        QVERIFY(graph_->framesSeen() > 20);
        const auto& gain = graph_->latestGain();
        const auto& input = graph_->latestInput();
        const auto& key = graph_->latestKey();
        const QList<double> frequencies = spectralDisplayFrequencies();
        const QList<double> threshold = spectralThresholdDb(-40.0, 0.0, frequencies);
        int cut = 0, loud = 0, over = 0;
        for (int j = 0; j < SpectralGraph::kPoints; ++j) {
            cut += gain[size_t(j)] < -3.0f && frequencies[j] > 50.0 ? 1 : 0;
            loud += input[size_t(j)] > -60.0f ? 1 : 0;
            over += key[size_t(j)] > threshold[j] ? 1 : 0;
            QVERIFY(gain[size_t(j)] >= -24.0001f && gain[size_t(j)] <= 0.0001f);  // within Range, no lift
        }
        QVERIFY2(cut > 100, qPrintable(QString::number(cut)));
        QVERIFY2(loud > 100 && over > 100, qPrintable(QStringLiteral("%1 %2").arg(loud).arg(over)));
        QVERIFY2(graph_->maxCutDb() > 3.0, qPrintable(QString::number(graph_->maxCutDb())));
        QCOMPARE(graph_->maxLiftDb(), 0.0);
        QVERIFY2(graph_->levelIn() > -30.0, qPrintable(QString::number(graph_->levelIn())));
        QVERIFY2(graph_->levelOut() < graph_->levelIn() - 3.0, qPrintable(QString::number(graph_->levelOut())));
        // The tones stand out of the noise around them (the median of the points from 1.5 to 2.5 kHz: one frame of
        // noise, so not any one point), and are cut deepest there. (Over 300 noise draws the smallest margins
        // were 10.1 and 4.4 dB.)
        const double noiseIn = medianBetween(input, 1500.0, 2500.0), noiseGain = medianBetween(gain, 1500.0, 2500.0);
        QVERIFY2(input[size_t(pointNear(1200.0))] > noiseIn + 6.0,
                 qPrintable(QStringLiteral("%1 %2").arg(input[size_t(pointNear(1200.0))]).arg(noiseIn)));
        QVERIFY2(gain[size_t(pointNear(1200.0))] < noiseGain - 1.0,
                 qPrintable(QStringLiteral("%1 %2").arg(gain[size_t(pointNear(1200.0))]).arg(noiseGain)));

        // The editor's view of it: drawn and moving.
        for (int i = 0; i < 20; ++i) refreshDisplays();
        QTest::qWait(50);
        const int painted = graph_->lastStats().frames;
        QVERIFY(painted > 0);

        // No audio: the spectra sink to the floor without a bounce, the cuts let go (never past 0), the meters fall.
        std::vector<double> input0 = graph_->shownInput(), gain0 = graph_->shownGain();
        std::vector<double> lastInput = input0;
        std::vector<double> lastGain = gain0;
        std::vector<bool> returning(gain0.size(), false);
        const double levelIn = graph_->levelIn();
        for (int i = 0; i < 60; ++i) {
            refreshDisplays();
            const std::vector<double>& shown = graph_->shownInput();
            const std::vector<double>& gains = graph_->shownGain();
            for (size_t j = 0; j < shown.size(); ++j) {
                QVERIFY(shown[j] <= lastInput[j] + 1e-9);
                QVERIFY(gains[j] <= 1e-9);  // a cut eases back to 0, never past it
                if (returning[j])
                    QVERIFY(gains[j] >= lastGain[j] - 1e-9);  // and once on its way back, never deeper again
                returning[j] = returning[j] || gains[j] > lastGain[j];
            }
            lastInput = shown;
            lastGain = gains;
        }
        double fell = 0.0;
        for (size_t j = 0; j < lastInput.size(); ++j) fell = std::max(fell, input0[j] - lastInput[j]);
        QVERIFY2(fell > 20.0, qPrintable(QString::number(fell)));
        QVERIFY(*std::min_element(lastGain.begin(), lastGain.end()) > -0.5);
        QVERIFY(graph_->levelIn() < levelIn);

        // The recent deepest cut's line outlives the cut: once the curtain has let go, it still holds (0.8 s
        // from the last cut), then falls (18 dB/s) and goes.
        const auto curtainGone = [&] {
            const std::vector<double>& gains = graph_->shownGain();
            return graph_->maxCutDb() < 0.05 && *std::min_element(gains.begin(), gains.end()) > -0.05;
        };
        for (int i = 0; i < 240 && !curtainGone(); ++i) refreshDisplays();
        QVERIFY(curtainGone());
        QVERIFY(graph_->cutHeld());
        QTest::qWait(50);
        QVERIFY(orangeInTheTop(0.4) > 20);  // drawn (the threshold, at -40 dB, is lower down)
        for (int i = 0; i < 600 && graph_->cutHeld(); ++i) refreshDisplays();
        QVERIFY(!graph_->cutHeld());
        QTest::qWait(50);
        QCOMPARE(orangeInTheTop(0.4), 0);

        // Once everything has settled, nothing is drawn again however often the display ticks.
        for (int i = 0; i < 600; ++i) refreshDisplays();  // (10 s: the meters' peaks have fallen)
        QTest::qWait(50);
        const int settled = graph_->lastStats().frames;
        for (int i = 0; i < 30; ++i) refreshDisplays();
        QTest::qWait(50);
        QCOMPARE(graph_->lastStats().frames, settled);
    }

    void deltaTintsTheOutput() {
        QVERIFY(showDevice(pinkNoise(kSampleRate, -12.0, {350.0, 1200.0, 3500.0}, -24.0)));
        set("threshold", -30.0);
        tick();
        QCOMPARE(graph_->deltaShare(), 0.0);
        set("delta", 1.0);
        QCOMPARE(engineValue("delta"), 1.f);
        tick(3);
        QVERIFY(graph_->deltaShare() > 0.0 && graph_->deltaShare() < 1.0);  // (eased: it tints over 80 ms)
        engine()->renderOffline(0.0, kSampleRate / 2);
        tick(15);  // (0.24 s: before the displays, given no more frames, sink back after 0.3 s)
        QVERIFY(graph_->deltaShare() > 0.95);
        // With Delta on, the output display is what is taken away: under the input everywhere.
        const auto& input = graph_->latestInput();
        const std::vector<double>& output = graph_->shownOutput();
        for (int j = 0; j < SpectralGraph::kPoints; ++j)
            QVERIFY(graph_->latestOutput()[size_t(j)] <= input[size_t(j)] + 0.01f);
        QVERIFY(*std::max_element(output.begin(), output.end()) > -40.0);
        QTest::qWait(50);
        save(grab(), QStringLiteral("spectral-delta.png"));
        tick();
        QVERIFY(graph_->deltaShare() > 0.999);
        set("delta", 0.0);
        tick();
        QVERIFY(graph_->deltaShare() < 0.001);
    }

    // --- The glow over the threshold --------------------------------------------------------------

    void glowOnlyWhileCutting() {
        QVERIFY(showDevice());
        tick();
        QCOMPARE(graph_->hotShare(), 1.0);
        // Nothing over the threshold is turned down: Ratio 1:1 (Upward on, so the line stays lit), Range 0 or
        // Dry/Wet 0. The glow fades out, and back in.
        for (const auto& [id, off] : {std::pair{"ratio", 1.0}, std::pair{"range", 0.0}, std::pair{"mix", 0.0}}) {
            if (QByteArray(id) == "ratio")
                set("upward", 2.0);
            set(id, off);
            tick(3);
            QVERIFY2(graph_->hotShare() > 0.0 && graph_->hotShare() < 1.0, id);  // (eased)
            tick();
            QVERIFY2(graph_->hotShare() < 0.001, id);
            undo()->undo();
            tick();
            QVERIFY2(graph_->hotShare() > 0.999, id);
        }
    }

    // --- The sidechain -------------------------------------------------------------------------------

    void badgeFollowsTheSidechain() {
        QVERIFY(showDevice(pinkNoise(kSampleRate, -18.0)));
        QQuickItem* badge = find(view_, QStringLiteral("sidechainBadge"));
        QVERIFY(badge);
        QVERIFY(!graph_->keyed());
        QVERIFY(!badge->property("lit").toBool());

        // A bass on another track keys it: lit; its level is what is compared (the dashed line).
        std::vector<float> bass = tone(80.0, kSampleRate, 0.5);
        const QString keyTrack = trackPlaying(bass);
        QVERIFY(!keyTrack.isEmpty());
        QSignalSpy keyed(graph_, &SpectralGraph::keyedChanged);
        QVERIFY(editor()->trySetDeviceSidechain(track_, device_, keyTrack, kPreFader));
        QCOMPARE(keyed.count(), 1);
        QVERIFY(graph_->keyed());
        QVERIFY(badge->property("lit").toBool());
        // Keyed, but nothing playing yet: no dashed line lies along the plot's floor.
        tick();
        QTest::qWait(50);
        QCOMPARE(yellowOnTheFloor(), 0);
        set("threshold", -30.0);
        tick();
        QTest::qWait(200);  // (the badge lit)
        engine()->renderOffline(0.0, kSampleRate / 2);
        tick(30);
        const auto& key = graph_->latestKey();
        const auto& gain = graph_->latestGain();
        int top = 0;
        for (int j = 1; j < SpectralGraph::kPoints; ++j)
            if (key[size_t(j)] > key[size_t(top)])
                top = j;
        const double topHz = spectralDisplayFrequencies()[top];
        QVERIFY2(topHz > 60.0 && topHz < 110.0, qPrintable(QString::number(topHz)));  // the bass, not the noise
        QVERIFY(gain[size_t(top)] < -6.0f);                                            // ducked under it
        QVERIFY(gain[size_t(SpectralGraph::kPoints * 3 / 4)] > -0.5f);                 // not elsewhere (3.7 kHz)
        QTest::qWait(50);
        save(grab(), QStringLiteral("spectral-keyed.png"));

        // A click on it asks the frame for the sidechain menu, under the badge, and changes nothing.
        QSignalSpy asked(view_, SIGNAL(sidechainMenuRequested(QVariant)));
        const int steps = undo()->index();
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(badge));
        QCOMPARE(asked.count(), 1);
        QCOMPARE(qvariant_cast<QQuickItem*>(asked.at(0).at(0)), badge);
        QCOMPARE(undo()->index(), steps);

        // Undone: unlit.
        while (graph_->keyed() && undo()->canUndo()) undo()->undo();
        QVERIFY(!graph_->keyed());
        QVERIFY(!badge->property("lit").toBool());
    }

    // --- Lifts -------------------------------------------------------------------------------------------

    void liftsRiseFromTheBottom() {
        // Quiet pink noise under Below, with one loud tone over the threshold: the quiet frequencies are brought up
        // (green, from the bottom; the header's figure), the tone turned down.
        QVERIFY(showDevice(pinkNoise(kSampleRate, -50.0, {1000.0}, -12.0)));
        set("upward", 2.0);
        set("below", -30.0);
        set("threshold", -18.0);
        tick();
        QTRY_COMPARE(find(view_, QStringLiteral("below"))->opacity(), 1.0);  // (for the screenshot)
        engine()->renderOffline(0.0, kSampleRate / 2);
        tick(15);  // (0.24 s: before the displays, given no more frames, sink back after 0.3 s)
        const auto& gain = graph_->latestGain();
        int lifted = 0;
        for (int j = pointNear(100.0); j <= pointNear(10000.0); ++j) lifted += gain[size_t(j)] > 3.0f ? 1 : 0;
        QVERIFY2(lifted > 60, qPrintable(QString::number(lifted)));
        QVERIFY(gain[size_t(pointNear(1000.0))] < -3.0f);  // the tone: cut
        QVERIFY2(graph_->maxLiftDb() > 3.0, qPrintable(QString::number(graph_->maxLiftDb())));
        const std::vector<double>& shown = graph_->shownGain();
        QVERIFY(*std::max_element(shown.begin(), shown.end()) > 3.0);
        QVERIFY(graph_->maxCutDb() > 3.0);
        QTest::qWait(50);
        save(grab(), QStringLiteral("spectral-lift.png"));
    }

    // --- How it looks ----------------------------------------------------------------------------------

    void screenshot() {
        QVERIFY(showDevice(pinkNoise(2 * kSampleRate, -14.0, {350.0, 1200.0, 3500.0}, -26.0)));
        set("threshold", -26.0);
        set("tilt", -0.5);
        set("upward", 2.0);
        set("below", -40.0);
        set("focus_lo", 60.0);
        set("focus_hi", 12000.0);
        set("smooth", 25.0);
        QCOMPARE(engineValue("focus_hi"), 12000.f);
        tick();  // (the lines settled)
        QTRY_COMPARE(find(view_, QStringLiteral("below"))->opacity(), 1.0);  // (Upward on: Below no longer dimmed)
        engine()->renderOffline(0.0, kSampleRate);
        tick(12);
        // The value under the mouse reads in the header.
        QTest::mouseMove(window_, at(SpectralGraph::BelowHandle));
        QTRY_COMPARE(graph_->hoveredHandle(), int(SpectralGraph::BelowHandle));
        tick(12);
        QVERIFY(graph_->maxCutDb() > 3.0);
        QCOMPARE(graph_->readout(), QStringLiteral("Below -40.0 dB"));
        QTest::qWait(50);
        save(grab(), QStringLiteral("spectral.png"));

        // While the threshold is dragged: its pivot grown and haloed, its value in the header.
        const QPoint pivot = at(SpectralGraph::ThresholdHandle);
        QTest::mouseMove(window_, pivot);
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, pivot);
        dragTo(pivot - QPoint(0, 8));
        engine()->renderOffline(0.0, kSampleRate);
        tick(12);
        QCOMPARE(graph_->hoveredHandle(), int(SpectralGraph::ThresholdHandle));
        QVERIFY(graph_->readout().startsWith(QStringLiteral("Threshold ")));
        QTest::qWait(50);
        save(grab(), QStringLiteral("spectral-drag.png"));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, pivot - QPoint(0, 8));
    }
};

QTEST_MAIN(TestUiDeviceEditorsSpectral)
#include "test_ui_device_editors_spectral.moc"
