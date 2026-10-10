// The Spectral Compressor's editor (ui/qml/devices/editors/SpectralEditor.qml, ui/src/devices/SpectralGraph):
// loaded as the device view loads it, its knobs and button bound to their parameters (undoably), the display's
// lines the engine's own (sub::app::spectralThresholdDb), its handles and edges dragged with the mouse (one undo
// step a drag), the engine's displays reaching it as it renders offline, and the Sidechain badge. With
// SUBSTATION_UI_SCREENSHOTS set to a folder, it is saved there idle, with a signal flowing, keyed by a
// sidechain, and with Delta on.

#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "EditorHarness.h"
#include "audio/SpectralResponse.h"
#include "controls/KnobItem.h"
#include "devices/DeviceParam.h"
#include "devices/SpectralGraph.h"
#include "model/ParamSpec.h"

using namespace sub::app;
using namespace sub::ui;
using sub::app::test::kSampleRate;

namespace {

constexpr double kPi = 3.14159265358979323846;

// Pink-ish noise (Paul Kellet's filter on white noise) at `rmsDb` dBFS RMS, with sines of `tonesDb` dBFS peak at
// `tones` Hz on it: a dense mix with resonances poking out, as a spectral compressor is for.
std::vector<float> pinkNoise(int frames, double rmsDb, const std::vector<double>& tones = {}, double tonesDb = -30.0,
                             unsigned seed = 7) {
    std::mt19937 random(seed);
    std::uniform_real_distribution<double> white(-1.0, 1.0);
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
    std::vector<double> pink(static_cast<size_t>(frames));
    double power = 0.0;
    for (int i = 0; i < frames; ++i) {
        const double w = white(random);
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

    // An EditorKnob's dial.
    KnobItem* knob(const char* id) {
        QQuickItem* cell = find(view_, QString::fromLatin1(id));
        auto* paramKnob = cell ? qvariant_cast<QQuickItem*>(cell->property("knob")) : nullptr;
        return paramKnob ? qvariant_cast<KnobItem*>(paramKnob->property("knob")) : nullptr;
    }

    // The display's clock ticked `count` times (the easing settles in about 120).
    void tick(int count = 120) {
        for (int i = 0; i < count; ++i) refreshDisplays();
    }

    QPoint at(int handle) { return scenePoint(graph_, graph_->handle(handle)); }

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
        QCOMPARE(view_->implicitWidth(), 860.0);

        // Thirteen knobs, each reading its parameter's default, bound to it.
        const std::vector<std::pair<const char*, double>> defaults{
            {"threshold", -24.0}, {"ratio", 3.0}, {"below", -48.0}, {"upward", 1.0}, {"tilt", 0.0},
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

        // Delta: a switch, off, level with the knobs beside it.
        QQuickItem* delta = find(view_, QStringLiteral("delta"));
        QVERIFY(delta);
        QVERIFY(!delta->property("lit").toBool());
        auto* mixDial = qvariant_cast<QQuickItem*>(find(view_, QStringLiteral("mix"))->property("knob"));
        QVERIFY(std::abs(centerOf(delta).y() - centerOf(mixDial).y()) <= 4);

        // The display between the knobs: 348 px, its plot roomy enough, the badge clear of the header's text.
        QCOMPARE(graph_->width(), double(SpectralGraph::kWidth));
        const QRectF plot = graph_->plot();
        QVERIFY2(plot.width() >= 300 && plot.height() >= 90,
                 qPrintable(QStringLiteral("%1 x %2").arg(plot.width()).arg(plot.height())));
        QVERIFY(graph_->x() >= 0 && graph_->x() + graph_->width() <= view_->width());
        QVERIFY(graph_->y() >= 6 && graph_->y() + graph_->height() <= view_->height() - 6 + 0.5);
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
        QTest::qWait(50);
        save(grab(), QStringLiteral("spectral-widest.png"));
        while (undo()->index() > before) undo()->undo();
        tick();
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
        QVERIFY2(value("threshold") > -24.0, qPrintable(QString::number(value("threshold"))));
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();
        QCOMPARE(value("threshold"), -24.0);

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
        QCOMPARE(graph_->thresholdAt(1000.0), -24.0);
        QCOMPARE(graph_->thresholdAt(20.0), -24.0);  // flat: it follows pink noise
        set("upward", 2.0);
        set("below", -40.0);
        check();
        set("tilt", 3.0);
        check();
        QVERIFY(std::abs(graph_->thresholdAt(10000.0) - (-24.0 + 3.0 * std::log2(10.0))) < 1e-6);
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
        QCOMPARE(graph_->readout(), QStringLiteral("Threshold ") + formatValue(-24.0, QStringLiteral("dB")));

        // The pivot dragged up 30 px: up as far, one undo step, the tilt left alone.
        int steps = undo()->index();
        drag(at(SpectralGraph::ThresholdHandle), QPoint(0, -30));
        QVERIFY2(std::abs(value("threshold") - (-24.0 + 30 * dbPerPixel)) <= 0.2,
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

        // A double-click on the pivot: back to -24, one undo step.
        tick();
        steps = undo()->index();
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, at(SpectralGraph::ThresholdHandle));
        QCOMPARE(value("threshold"), -24.0);
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
        QCOMPARE(value("threshold"), -24.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(engineValue("tilt"), float(value("tilt")));

        // The 100 Hz end up: the tilt falls.
        const double before = value("tilt");
        drag(at(SpectralGraph::TiltLow), QPoint(0, -20));
        QVERIFY2(std::abs(value("tilt") - (before - 20 * dbPerPixel / octaves)) <= 0.02,
                 qPrintable(QString::number(value("tilt"))));

        // The handles ride the line, held within the plot.
        set("tilt", 6.0);
        set("threshold", 12.0);
        tick();
        const QRectF plot = graph_->plot();
        for (const auto& [handle, hz] : {std::pair{int(SpectralGraph::TiltLow), 100.0},
                                         std::pair{int(SpectralGraph::ThresholdHandle), 1000.0},
                                         std::pair{int(SpectralGraph::TiltHigh), 10000.0}}) {
            const QPointF point = graph_->handle(handle);
            QVERIFY(std::abs(point.y() - graph_->yOfLevel(graph_->thresholdAt(hz))) < 0.5);
            QVERIFY(plot.top() <= point.y() && point.y() <= plot.bottom());
        }
        QCOMPARE(graph_->handle(SpectralGraph::TiltHigh).y(), plot.top());  // (+12 + 20 dB: off the top)

        // A double-click: level with pink again.
        set("threshold", -24.0);
        tick();
        steps = undo()->index();
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, at(SpectralGraph::TiltHigh));
        QCOMPARE(value("tilt"), 0.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(undo()->undoText(), QStringLiteral("Reset Tilt"));
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
        QCOMPARE(value("threshold"), -24.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(engineValue("below"), float(value("below")));
        QCOMPARE(graph_->readout(), QStringLiteral("Below ") + formatValue(value("below"), QStringLiteral("dB")));

        // Over the threshold it is drawn on it, and a drag starts from there.
        set("below", 0.0);
        tick();
        drag(at(SpectralGraph::BelowHandle), QPoint(0, 10));
        QVERIFY2(std::abs(value("below") - (-24.0 - 10 * dbPerPixel)) <= 0.2,
                 qPrintable(QString::number(value("below"))));
        QCOMPARE(value("threshold"), -24.0);

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
        QCOMPARE(value("threshold"), -24.0);
        QCOMPARE(value("tilt"), 0.0);
        QCOMPARE(value("below"), -48.0);
        QCOMPARE(value("focus_lo"), 20.0);
        QCOMPARE(value("focus_hi"), 20000.0);
    }

    // --- The displays ------------------------------------------------------------------------------

    void displaysReachTheGraph() {
        QVERIFY(showDevice(pinkNoise(kSampleRate, -12.0, {350.0, 1200.0, 3500.0}, -24.0)));
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
        // The tones stand out of the noise, and are cut deepest there.
        const auto nearest = [&](double hz) {
            return int(std::lround(std::log(hz / 20.0) / std::log(1000.0) * (SpectralGraph::kPoints - 1)));
        };
        QVERIFY(input[size_t(nearest(1200.0))] > input[size_t(nearest(1700.0))] + 6.0f);
        QVERIFY(gain[size_t(nearest(1200.0))] < gain[size_t(nearest(1700.0))] - 1.0f);

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

        // Once everything has settled, nothing is drawn again however often the display ticks.
        for (int i = 0; i < 600; ++i) refreshDisplays();  // (5 s: the meters' peaks have fallen)
        QTest::qWait(50);
        const int settled = graph_->lastStats().frames;
        for (int i = 0; i < 30; ++i) refreshDisplays();
        QTest::qWait(50);
        QCOMPARE(graph_->lastStats().frames, settled);
    }

    void deltaTintsTheOutput() {
        QVERIFY(showDevice(pinkNoise(kSampleRate, -12.0, {350.0, 1200.0, 3500.0}, -24.0)));
        set("threshold", -30.0);
        set("delta", 1.0);
        QCOMPARE(engineValue("delta"), 1.f);
        engine()->renderOffline(0.0, kSampleRate / 2);
        tick(30);
        // With Delta on, the output display is what is taken away: under the input everywhere.
        const auto& input = graph_->latestInput();
        const std::vector<double>& output = graph_->shownOutput();
        for (int j = 0; j < SpectralGraph::kPoints; ++j)
            QVERIFY(graph_->latestOutput()[size_t(j)] <= input[size_t(j)] + 0.01f);
        QVERIFY(*std::max_element(output.begin(), output.end()) > -40.0);
        QTest::qWait(50);
        save(grab(), QStringLiteral("spectral-delta.png"));
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
        QVERIFY(gain[size_t(SpectralGraph::kPoints * 3 / 4)] > -0.5f);                 // not elsewhere (2.6 kHz)
        QTest::qWait(50);
        save(grab(), QStringLiteral("spectral-keyed.png"));

        // A click on it asks the frame for the sidechain menu, and changes nothing.
        QSignalSpy asked(view_, SIGNAL(sidechainMenuRequested()));
        const int steps = undo()->index();
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(badge));
        QCOMPARE(asked.count(), 1);
        QCOMPARE(undo()->index(), steps);

        // Undone: unlit.
        while (graph_->keyed() && undo()->canUndo()) undo()->undo();
        QVERIFY(!graph_->keyed());
        QVERIFY(!badge->property("lit").toBool());
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
