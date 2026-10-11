// Multiband Dynamics' editor (ui/qml/devices/editors/MultibandEditor.qml,
// ui/src/devices/MultibandGraph): loaded as the device view loads it, over a
// real engine; whatever the font, every text whole (its columns as wide as
// their texts need, the automation dot clear of the boxes' texts); its controls
// bound to their parameters (undoably, and the engine has what they set), the
// graph's drags, double-clicks and wheel (one undo step a gesture), the
// displays reaching the graph as the engine renders, and its animation easing
// and settling. Also the `ratio` unit and the ratios and times typed, which
// need no window (they run on any platform). With SUBSTATION_UI_SCREENSHOTS set
// to a folder, the editor is saved there.

#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <vector>

#include "EditorHarness.h"
#include "audio/EngineBridge.h"
#include "audio/MultibandResponse.h"
#include "controls/KnobItem.h"
#include "controls/ValueBoxItem.h"
#include "devices/DeviceParam.h"
#include "devices/DisplayClock.h"
#include "devices/MultibandGraph.h"
#include "editor/ProjectEditor.h"
#include "input/GestureKey.h"
#include "model/ParamSpec.h"

using namespace sub::app;
using namespace sub::ui;
using sub::app::test::kSampleRate;

namespace {

constexpr int kLow = MultibandGraph::Low, kMid = MultibandGraph::Mid, kHigh = MultibandGraph::High;
constexpr int kBelow = MultibandGraph::Below, kAbove = MultibandGraph::Above;
// Ticks by hand (each counts as one of the clock's, at least) within which the meters hold what they last read.
constexpr int kHoldTicks = int(MultibandGraph::kHoldSeconds * 1000.0 / sub::ui::kDisplayRefreshMs);

}  // namespace

class TestUiDeviceEditorsMultiband : public QObject, public sub::app::test::EditorHarness {
    Q_OBJECT

    // A file name of its own for a track's audio: the engine keeps sources by their path, and a file written
    // again under one name while an earlier test's load of it is still reading can leave that load's audio.
    QString fileName(const char* name) { return QString::fromLatin1(name) + QString::number(++files_); }
    int files_ = 0;

    // A track playing `mono` with a Multiband Dynamics on it, `settings` set first, its editor shown.
    struct Shown {
        QString track, device;
        QQuickItem* view = nullptr;
        MultibandGraph* graph = nullptr;
    };
    Shown showDevice(const sub::app::OrderedMap<QString, double>& settings = {},
                     const std::vector<float>& mono = tone(1000.0, kSampleRate)) {
        Shown shown;
        shown.track = audioTrackWith(mono, fileName("tone"), 1.0);
        if (shown.track.isEmpty())
            return shown;
        shown.device = editor()->addDevice(shown.track, QStringLiteral("multiband"));
        if (!settings.isEmpty())
            editor()->setDeviceParams(shown.track, shown.device, settings);
        undo()->clear();
        shown.view = show(QStringLiteral("multiband"), shown.track, shown.device);
        if (shown.view)
            shown.graph = find<MultibandGraph>(shown.view, QStringLiteral("multibandGraph"));
        return shown;
    }

    ValueBoxItem* box(QQuickItem* view, const char* name) {
        QQuickItem* item = find(view, QString::fromLatin1(name));
        return item ? qvariant_cast<ValueBoxItem*>(item->property("box")) : nullptr;
    }
    QQuickItem* button(QQuickItem* view, const char* name) {
        QQuickItem* item = find(view, QString::fromLatin1(name));
        return item ? qvariant_cast<QQuickItem*>(item->property("button")) : nullptr;
    }
    void click(QQuickItem* item) { QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, centerOf(item)); }
    void ticks(int count) {
        for (int i = 0; i < count; ++i)
            refreshDisplays();
    }
    // A drag on the graph from `from` by `by`, in `steps` moves.
    void drag(MultibandGraph* graph, QPointF from, QPoint by, int steps = 3,
              Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        const QPoint at = scenePoint(graph, from);
        QTest::mousePress(window_, Qt::LeftButton, modifiers, at);
        for (int i = 1; i <= steps; ++i)
            dragTo(at + by * i / steps, modifiers);
        QTest::mouseRelease(window_, Qt::LeftButton, modifiers, at + by);
    }
    // One undo step more than `before`, nothing to redo.
    bool oneStepAfter(int before) { return undo()->index() == before + 1 && undo()->count() == undo()->index(); }
    // Ticks with no new audio (the rest of the hold, then the meters letting go) until the band's lane is still,
    // checking every tick that its bars and its figure agree: the change drawn runs from the in meter to the out
    // meter, and the figure is their difference, or the last reading where the change goes on past a meter at the
    // floor (as while playing). The first disagreement, or "" (and `ticked`, how many ticks it took).
    QString lettingGo(MultibandGraph* graph, int band, double reading, int* ticked = nullptr) {
        constexpr double kFloor = MultibandGraph::kFloorDb, kTolerance = 0.2;
        for (int i = 1; i <= 600; ++i) {
            refreshDisplays();
            const double in = graph->inLevel(band), out = graph->outLevel(band), gain = graph->gainShown(band);
            const auto [from, to] = graph->changeSpan(band);
            const QString at = QStringLiteral("tick %1: in %2, out %3, shown %4, drawn from %5 to %6")
                                   .arg(i)
                                   .arg(in)
                                   .arg(out)
                                   .arg(gain)
                                   .arg(from)
                                   .arg(to);
            if (to != out || (in > kFloor && std::abs(from - in) > kTolerance))
                return at;
            if (in > kFloor && out > kFloor && std::abs(gain - (out - in)) > kTolerance)
                return at;
            if ((in > kFloor) != (out > kFloor) && std::abs(gain - reading) > kTolerance)  // (one at the floor)
                return at;
            if (in <= kFloor && out <= kFloor && !graph->animating()) {
                if (ticked)
                    *ticked = i;
                return gain == 0.0 ? QString() : at;
            }
        }
        return QStringLiteral("still moving");
    }
    static sub::ui::DeviceParam* paramOf(QQuickItem* control) {
        return control ? qvariant_cast<sub::ui::DeviceParam*>(control->property("param")) : nullptr;
    }

    // The application layer's texts: plain checks, no window.
    static bool headless(const char* function) {
        return std::strcmp(function, "ratioTexts") == 0 || std::strcmp(function, "timeTexts") == 0;
    }

private Q_SLOTS:
    void initTestCase() {
        if (haveDisplay())
            startHost();
    }

    void cleanupTestCase() { stopHost(); }

    void init() {
        if (headless(QTest::currentTestFunction()))
            return;
        if (!haveDisplay())
            QSKIP("needs a display: the offscreen platform renders Qt Quick in software, without this geometry");
        clearHost();
    }

    void fitsAndShowsEveryControl() {
        const Shown s = showDevice();
        QVERIFY(s.view && s.graph);
        QVERIFY2(s.view->implicitHeight() <= bodyHeight(),
                 qPrintable(QStringLiteral("%1 > %2").arg(s.view->implicitHeight()).arg(bodyHeight())));

        // The device's own controls: within the body, their rows one under the other.
        QQuickItem* globals = find(s.view, QStringLiteral("globals"));
        QVERIFY(globals);
        QVERIFY(globals->y() + globals->height() <= s.view->height() - 6);
        double bottom = 0.0;
        for (QQuickItem* row : globals->childItems()) {
            if (!row->isVisible())
                continue;
            QVERIFY(row->y() >= bottom);
            bottom = row->y() + row->height();
        }

        for (const char* name : {"highActive", "midActive", "lowActive", "highOn", "lowOn", "highSolo", "midSolo",
                                 "lowSolo", "xoverHigh", "xoverLow", "highIn", "midIn", "lowIn", "highOut", "midOut",
                                 "lowOut", "highAbove", "midAbove", "lowAbove", "highAboveRatio", "midAboveRatio",
                                 "lowAboveRatio", "highBelow", "midBelow", "lowBelow", "highBelowRatio",
                                 "midBelowRatio", "lowBelowRatio", "highAttack", "midAttack", "lowAttack",
                                 "highRelease", "midRelease", "lowRelease", "pages", "pageTime", "pageBelow",
                                 "pageAbove", "pageCaption", "pageCaption2", "amount", "time", "output", "softKnee",
                                 "modePeak", "modeRms", "scGain", "scMix", "sidechainButton", "scListen",
                                 "multibandGraph"})
            QVERIFY2(find(s.view, QString::fromLatin1(name)), name);
        // The bands' buttons are their activators, the switches beside the crossovers their splits.
        for (const auto& [name, id] : {std::pair{"highActive", "high_active"}, std::pair{"midActive", "mid_active"},
                                       std::pair{"lowActive", "low_active"}, std::pair{"highOn", "high_on"},
                                       std::pair{"lowOn", "low_on"}}) {
            QObject* bound = qvariant_cast<QObject*>(find(s.view, QString::fromLatin1(name))->property("param"));
            QVERIFY2(bound && bound->property("paramId").toString() == QString::fromLatin1(id), name);
        }
        QCOMPARE(find(s.view, QStringLiteral("midActive"))->property("text").toString(), QStringLiteral("Mid"));
        QVERIFY(find(s.view, QStringLiteral("highActive"))->property("lit").toBool());

        // The boxes read their parameters' defaults, in their units.
        QCOMPARE(box(s.view, "midAbove")->value(), -20.0);
        QCOMPARE(box(s.view, "midAbove")->text(), QStringLiteral("-20.0 dB"));
        QCOMPARE(box(s.view, "midBelow")->value(), -40.0);
        QCOMPARE(box(s.view, "midAboveRatio")->value(), 1.0);
        QCOMPARE(box(s.view, "midAboveRatio")->text(), QStringLiteral("1:1.00"));
        QCOMPARE(box(s.view, "xoverLow")->value(), 120.0);
        QCOMPARE(box(s.view, "xoverHigh")->value(), 2500.0);
        QCOMPARE(box(s.view, "xoverHigh")->text(), QStringLiteral("2.50 kHz"));
        QCOMPARE(box(s.view, "midAttack")->text(), QStringLiteral("10 ms"));
        // A double-click resets each to its parameter's default, the engine's.
        QHash<QString, double> defaults;
        for (const ProcessorParam& param : bridge()->deviceParams(s.track, s.device))
            defaults.insert(param.id, param.defaultValue);
        std::vector<std::pair<QString, QString>> boxes = {{QStringLiteral("xoverHigh"), QStringLiteral("xover_high")},
                                                          {QStringLiteral("xoverLow"), QStringLiteral("xover_low")}};
        for (const char* band : {"high", "mid", "low"}) {
            for (const auto& [field, id] : {std::pair{"In", "in"}, std::pair{"Out", "out"}, std::pair{"Above", "above"},
                                            std::pair{"AboveRatio", "above_ratio"}, std::pair{"Below", "below"},
                                            std::pair{"BelowRatio", "below_ratio"}, std::pair{"Attack", "attack"},
                                            std::pair{"Release", "release"}})
                boxes.emplace_back(QString::fromLatin1(band) + QString::fromLatin1(field),
                                   QString::fromLatin1(band) + QLatin1Char('_') + QString::fromLatin1(id));
        }
        for (const auto& [name, id] : boxes) {
            const QVariant reset = box(s.view, qPrintable(name))->defaultValue();
            QVERIFY2(defaults.contains(id) && reset.isValid() && reset.toDouble() == defaults.value(id),
                     qPrintable(QStringLiteral("%1: %2").arg(name, reset.toString())));
        }
        auto* amount =qvariant_cast<KnobItem*>(
            qvariant_cast<QQuickItem*>(find(s.view, QStringLiteral("amount"))->property("knob"))->property("knob"));
        QVERIFY(amount);
        QCOMPARE(amount->value(), 100.0);

        // The columns side by side, apart, each as wide as its contents need in the font the UI has (checked
        // below), and the editor as wide as they are.
        auto left = [&](const char* name) {
            return find(s.view, QString::fromLatin1(name))->mapToItem(s.view, QPointF(0, 0)).x();
        };
        auto right = [&](const char* name) {
            QQuickItem* item = find(s.view, QString::fromLatin1(name));
            return item->mapToItem(s.view, QPointF(item->width(), 0)).x();
        };
        const char* const columns[] = {"highOn", "xoverHigh",     "midIn",  "multibandGraph", "midAbove",
                                       "midAboveRatio", "midOut", "globals"};
        for (std::size_t i = 1; i < std::size(columns); ++i)
            QVERIFY2(right(columns[i - 1]) <= left(columns[i]), columns[i]);
        QVERIFY(right("highActive") <= left("highSolo") && right("highSolo") <= left("midIn"));
        QVERIFY(left("highActive") == left("highOn") && right("highOn") <= left("xoverHigh"));
        // (the band column's edges line up: the buttons fill it above, the crossover box below)
        QCOMPARE(right("highSolo"), right("xoverHigh"));
        QCOMPARE(right("lowSolo"), right("xoverLow"));
        QVERIFY(left("highOn") >= 8 && right("globals") <= s.view->width() - 8);
        QCOMPARE(s.view->implicitWidth(), right("globals") + 8);
        // The device's own controls: the button rows fill their column, so their edges line up; the knobs
        // within it.
        QCOMPARE(left("softKnee"), left("globals"));
        QCOMPARE(right("modeRms"), right("globals"));
        QCOMPARE(left("scGain"), left("globals"));
        QCOMPARE(right("scListen"), right("globals"));
        QVERIFY(left("amount") >= left("globals") && right("output") <= right("globals"));

        // Every box shows every text its parameter takes whole, whatever the font, a pixel clear of the
        // automation dot (drawn from 3.5 to 8.5 px from its left), so the dot never touches a minus sign.
        auto items = [&](const QStringList& names) {
            QList<QQuickItem*> found;
            for (const QString& name : names)
                found << find(s.view, name);
            return found;
        };
        auto bandBoxes = [&](std::initializer_list<const char*> fields) {
            QStringList names;
            for (const char* band : {"high", "mid", "low"}) {
                for (const char* field : fields)
                    names << QString::fromLatin1(band) + QString::fromLatin1(field);
            }
            return names;
        };
        const QStringList levelBoxes = bandBoxes({"In", "Out"});
        const QStringList fieldBoxes = bandBoxes({"Above", "Below", "Attack"});
        const QStringList field2Boxes = bandBoxes({"AboveRatio", "BelowRatio", "Release"});
        const QStringList crossovers = {QStringLiteral("xoverHigh"), QStringLiteral("xoverLow")};
        for (const QString& name : levelBoxes + fieldBoxes + field2Boxes + crossovers) {
            QQuickItem* item = find(s.view, name);
            QVERIFY2(item && box(s.view, qPrintable(name)) && paramOf(item), qPrintable(name));
            QVERIFY2(item->width() >= item->implicitWidth(), qPrintable(name));
            const QString problem = boxTextsProblem(item);
            QVERIFY2(problem.isEmpty(), qPrintable(problem));
        }
        // Each column as wide as its boxes' texts need and its caption (on every page), and no wider: the band
        // column as its crossover box, or its buttons (a band's name, and its solo, 2 px apart) if they need more.
        auto widthOf = [&](const QString& name) { return find(s.view, name)->width(); };
        const QVariant captionFont = find(s.view, QStringLiteral("inCaption"))->property("font");
        auto captionsNeed = [&](const QStringList& texts) { return std::ceil(widestOf(texts, captionFont)); };
        const double levelsNeed = std::max(boxesNeed(items(levelBoxes)),
                                           captionsNeed({QStringLiteral("Input"), QStringLiteral("Output")}));
        const double fieldsNeed = std::max(boxesNeed(items(fieldBoxes)),
                                           captionsNeed({QStringLiteral("Above"), QStringLiteral("Below"),
                                                         QStringLiteral("Attack")}));
        const double fields2Need = std::max(boxesNeed(items(field2Boxes)),
                                            captionsNeed({QStringLiteral("Ratio"), QStringLiteral("Release")}));
        for (const QString& name : levelBoxes)
            QCOMPARE(widthOf(name), levelsNeed);
        for (const QString& name : fieldBoxes)
            QCOMPARE(widthOf(name), fieldsNeed);
        for (const QString& name : field2Boxes)
            QCOMPARE(widthOf(name), fields2Need);
        double buttonsNeed = 0.0;
        for (const char* name : {"highActive", "midActive", "lowActive"})
            buttonsNeed = std::max(buttonsNeed, find(s.view, QString::fromLatin1(name))->implicitWidth());
        QCOMPARE(right("highSolo") - left("highActive"),
                 std::max(16 + boxesNeed(items(crossovers)), std::ceil(buttonsNeed) + 2 + 18));
        // Every knob's caption and every text its readout takes fit its cell (and the cell is sized by those
        // texts, all of them), as a Text lays them out; each cell as wide as they need (the device's three at
        // least EditorKnob's 52 px), and no wider.
        for (const char* name : {"amount", "time", "output", "scGain", "scMix"}) {
            const QString problem = knobTextsProblem(find(s.view, QString::fromLatin1(name)));
            QVERIFY2(problem.isEmpty(), qPrintable(problem));
        }
        const QStringList knobs = {QStringLiteral("amount"), QStringLiteral("time"), QStringLiteral("output")};
        const QStringList scKnobs = {QStringLiteral("scGain"), QStringLiteral("scMix")};
        for (const QString& name : knobs)
            QCOMPARE(widthOf(name), std::max(52.0, std::ceil(knobsNeed(items(knobs)))));
        for (const QString& name : scKnobs)
            QCOMPARE(widthOf(name), std::ceil(knobsNeed(items(scKnobs))));
        // Every button's text fits within its padding.
        for (const char* name : {"highActive", "midActive", "lowActive", "highSolo", "midSolo", "lowSolo", "highOn",
                                 "lowOn", "pageTime", "pageBelow", "pageAbove", "softKnee", "modePeak", "modeRms",
                                 "sidechainButton", "scListen"}) {
            QQuickItem* item = find(s.view, QString::fromLatin1(name));
            QVERIFY2(item->implicitWidth() <= item->width(),
                     qPrintable(QStringLiteral("%1: %2 > %3").arg(name).arg(item->implicitWidth()).arg(item->width())));
        }
        // The T/B/A buttons in the header over the band column, clear of the High row's buttons and of Input's
        // caption; every field column has its caption.
        auto under = [&](const char* name) {
            QQuickItem* item = find(s.view, QString::fromLatin1(name));
            return item->mapToItem(s.view, QPointF(0, item->height())).y();
        };
        auto top = [&](const char* name) {
            return find(s.view, QString::fromLatin1(name))->mapToItem(s.view, QPointF()).y();
        };
        QVERIFY(left("pages") >= 8 && right("pages") <= s.view->property("inX").toDouble());
        QVERIFY(under("pages") <= top("highActive"));
        QCOMPARE(left("pageCaption"), left("midAbove"));
        QCOMPARE(left("pageCaption2"), left("midAboveRatio"));
        QCOMPARE(left("midAttack"), left("midAbove"));
        QCOMPARE(right("midRelease"), right("midAboveRatio"));
        // The captions whole over their columns (and on every page, below).
        auto captionFits = [&](const char* name) {
            QQuickItem* caption = find(s.view, QString::fromLatin1(name));
            return caption->implicitWidth() <= caption->width();
        };
        for (const char* name : {"inCaption", "pageCaption", "pageCaption2", "outCaption"})
            QVERIFY2(captionFits(name), name);

        // The rows line up with the graph's lanes.
        for (const auto& [name, band] :
             {std::pair{"highIn", kHigh}, std::pair{"midIn", kMid}, std::pair{"lowIn", kLow}}) {
            QQuickItem* in = find(s.view, QString::fromLatin1(name));
            const double y = in->mapToScene(QPointF(0, in->height() / 2)).y();
            const QRectF lane = s.graph->lane(band);
            const double top = s.graph->mapToScene(lane.topLeft()).y(),
                         under = s.graph->mapToScene(lane.bottomLeft()).y();
            QVERIFY2(top < y && y < under, name);
        }
        QCOMPARE(s.graph->rowHeight(), (s.view->height() - 12 - 16) / 3);

        // The fields: Above's to start with, then Time's, then Below's, each column captioned.
        auto visible = [&](const char* name) { return find(s.view, QString::fromLatin1(name))->isVisible(); };
        auto caption = [&](const char* name) { return find(s.view, QString::fromLatin1(name))->property("text"); };
        QVERIFY(visible("midAbove") && visible("midAboveRatio"));
        QVERIFY(!visible("midBelow") && !visible("midAttack"));
        QVERIFY(find(s.view, QStringLiteral("pageAbove"))->property("checked").toBool());
        QCOMPARE(caption("pageCaption").toString(), QStringLiteral("Above"));
        QCOMPARE(caption("pageCaption2").toString(), QStringLiteral("Ratio"));
        click(find(s.view, QStringLiteral("pageTime")));
        QVERIFY(visible("midAttack") && visible("midRelease") && visible("highAttack"));
        QVERIFY(!visible("midAbove") && !visible("midBelow"));
        QVERIFY(find(s.view, QStringLiteral("pageTime"))->property("checked").toBool());
        QVERIFY(!find(s.view, QStringLiteral("pageAbove"))->property("checked").toBool());
        QCOMPARE(caption("pageCaption").toString(), QStringLiteral("Attack"));
        QCOMPARE(caption("pageCaption2").toString(), QStringLiteral("Release"));
        QVERIFY(captionFits("pageCaption") && captionFits("pageCaption2"));
        click(find(s.view, QStringLiteral("pageBelow")));
        QVERIFY(visible("midBelow") && visible("lowBelowRatio"));
        QVERIFY(!visible("midAttack") && !visible("midAbove"));
        QCOMPARE(caption("pageCaption").toString(), QStringLiteral("Below"));
        QCOMPARE(caption("pageCaption2").toString(), QStringLiteral("Ratio"));
        QVERIFY(captionFits("pageCaption") && captionFits("pageCaption2"));
        QCOMPARE(undo()->count(), 0);  // (a view, not an edit)
        // The page is the device's, not the editor's: an editor made again (as the chain's frames are, when
        // its devices change) shows it still.
        QQuickItem* again = show(QStringLiteral("multiband"), s.track, s.device);
        QVERIFY(again);
        QVERIFY(find(again, QStringLiteral("midBelow"))->isVisible());
        QVERIFY(!find(again, QStringLiteral("midAbove"))->isVisible());
        QVERIFY(find(again, QStringLiteral("pageBelow"))->property("checked").toBool());

        // At the least height it asks for, the rows and the device's controls still fit, apart.
        QQuickItem* compact = show(QStringLiteral("multiband"), s.track, s.device, int(again->implicitHeight()));
        QVERIFY(compact);
        QQuickItem* low = find(compact, QStringLiteral("xoverLow"));
        QVERIFY(low->mapToItem(compact, QPointF(0, low->height())).y() <= compact->height() - 6 + 0.5);
        globals = find(compact, QStringLiteral("globals"));
        QVERIFY(globals->y() + globals->height() <= compact->height() - 6);
        auto* graph = find<MultibandGraph>(compact, QStringLiteral("multibandGraph"));
        QVERIFY(graph->rowHeight() >= MultibandGraph::kMinRowHeight);
        QTest::qWait(30);
        save(grab(), QStringLiteral("multiband-compact.png"));
    }

    void controlsAreUndoable() {
        const Shown s = showDevice();
        QVERIFY(s.view && s.graph);
        auto value = [&](const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); };

        // A box dragged: one undo step.
        {
            const int before = undo()->index();
            const QPoint at = centerOf(box(s.view, "midIn"));
            QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
            for (int dy = 20; dy <= 40; dy += 20)
                dragTo(at - QPoint(0, dy));
            QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 40));
            QVERIFY2(std::abs(value("mid_in") - 1.0) < 0.051, qPrintable(QString::number(value("mid_in"))));
            QVERIFY(oneStepAfter(before));
            undo()->undo();
            QCOMPARE(value("mid_in"), 0.0);
        }

        // High's split switched off (the switch beside its crossover): its lane and its controls dim (still
        // editable), its solo can't be used.
        {
            const int before = undo()->index();
            click(button(s.view, "highOn"));
            QCOMPARE(value("high_on"), 0.0);
            QVERIFY(oneStepAfter(before));
            QVERIFY(!find(s.view, QStringLiteral("highOn"))->property("lit").toBool());
            QVERIFY(!s.graph->bandOn(kHigh));
            QVERIFY(!find(s.view, QStringLiteral("highSolo"))->isEnabled());
            QVERIFY(find(s.view, QStringLiteral("midSolo"))->isEnabled());
            QTRY_COMPARE(find(s.view, QStringLiteral("highAbove"))->opacity(), 0.55);
            QTRY_COMPARE(find(s.view, QStringLiteral("xoverHigh"))->opacity(), 0.55);
            QTRY_COMPARE(find(s.view, QStringLiteral("highActive"))->opacity(), 0.55);
            QVERIFY(find(s.view, QStringLiteral("highAbove"))->isEnabled());
            QCOMPARE(find(s.view, QStringLiteral("midAbove"))->opacity(), 1.0);
            undo()->undo();
            QCOMPARE(value("high_on"), 1.0);
            QVERIFY(s.graph->bandOn(kHigh));
            QTRY_COMPARE(find(s.view, QStringLiteral("highAbove"))->opacity(), 1.0);
        }

        // The Mid band bypassed (its activator, the band's button): its controls dim (still editable), its
        // solo still works, its crossovers stay as they are; one step.
        {
            const int before = undo()->index();
            click(button(s.view, "midActive"));
            QCOMPARE(value("mid_active"), 0.0);
            QVERIFY(oneStepAfter(before));
            QVERIFY(!find(s.view, QStringLiteral("midActive"))->property("lit").toBool());
            QVERIFY(!s.graph->bandActive(kMid) && s.graph->bandOn(kMid));
            QVERIFY(find(s.view, QStringLiteral("midSolo"))->isEnabled());
            QTRY_COMPARE(find(s.view, QStringLiteral("midAbove"))->opacity(), 0.55);
            QTRY_COMPARE(find(s.view, QStringLiteral("midIn"))->opacity(), 0.55);
            QCOMPARE(find(s.view, QStringLiteral("xoverHigh"))->opacity(), 1.0);
            QCOMPARE(find(s.view, QStringLiteral("highAbove"))->opacity(), 1.0);
            undo()->undo();
            QCOMPARE(value("mid_active"), 1.0);
            QVERIFY(s.graph->bandActive(kMid));
            QTRY_COMPARE(find(s.view, QStringLiteral("midAbove"))->opacity(), 1.0);
        }

        // Solo, Peak/RMS, Soft Knee: a click, a step each.
        int before = undo()->index();
        click(button(s.view, "midSolo"));
        QCOMPARE(value("mid_solo"), 1.0);
        QVERIFY(oneStepAfter(before));
        before = undo()->index();
        click(button(s.view, "modePeak"));
        QCOMPARE(value("mode"), 0.0);
        QVERIFY(find(s.view, QStringLiteral("modePeak"))->property("lit").toBool());
        QVERIFY(!find(s.view, QStringLiteral("modeRms"))->property("lit").toBool());
        QVERIFY(oneStepAfter(before));
        before = undo()->index();
        click(button(s.view, "softKnee"));
        QCOMPARE(value("soft_knee"), 1.0);
        QVERIFY(oneStepAfter(before));

        // Ratios typed as Live writes them ("1:2": 2 dB past the threshold come out as 1), bare (the number
        // after "1:") or as a compressor writes them ("4:1").
        ValueBoxItem* ratio = box(s.view, "midAboveRatio");
        QVERIFY(ratio);
        before = undo()->index();
        QVERIFY(ratio->applyTyped(QStringLiteral("1:2")));
        QCOMPARE(value("mid_above_ratio"), 2.0);
        QCOMPARE(ratio->text(), QStringLiteral("1:2.00"));
        QVERIFY(oneStepAfter(before));
        before = undo()->index();
        QVERIFY(ratio->applyTyped(QStringLiteral("1:66.7")));  // Over The Top's
        QVERIFY(std::abs(value("mid_above_ratio") - 66.7) < 1e-9);
        QCOMPARE(ratio->text(), QStringLiteral("1:66.7"));
        QVERIFY(oneStepAfter(before));
        before = undo()->index();
        QVERIFY(ratio->applyTyped(QStringLiteral("0.333")));
        QVERIFY(std::abs(value("mid_above_ratio") - 0.333) < 1e-9);
        QCOMPARE(ratio->text(), QStringLiteral("1:0.333"));
        QVERIFY(oneStepAfter(before));
        before = undo()->index();
        QVERIFY(ratio->applyTyped(QStringLiteral("4:1")));
        QCOMPARE(value("mid_above_ratio"), 4.0);
        QCOMPARE(ratio->text(), QStringLiteral("1:4.00"));
        QVERIFY(oneStepAfter(before));
        before = undo()->index();
        QVERIFY(!ratio->applyTyped(QStringLiteral("x")));
        QCOMPARE(value("mid_above_ratio"), 4.0);
        QCOMPARE(undo()->index(), before);
        // Times typed in either unit their boxes show ("1.5 s", "250 ms"); a bare number is milliseconds.
        ValueBoxItem* release = box(s.view, "midRelease");
        before = undo()->index();
        QVERIFY(release->applyTyped(QStringLiteral("1.5 s")));
        QCOMPARE(value("mid_release"), 1500.0);
        QCOMPARE(release->text(), QStringLiteral("1.50 s"));
        QVERIFY(oneStepAfter(before));
        QVERIFY(release->applyTyped(QStringLiteral("250ms")));
        QCOMPARE(value("mid_release"), 250.0);
        QVERIFY(release->applyTyped(QStringLiteral("80")));
        QCOMPARE(value("mid_release"), 80.0);
        QCOMPARE(release->text(), QStringLiteral("80 ms"));
        // Live's range, 0.1 ms to 5 s.
        QVERIFY(release->applyTyped(QStringLiteral("4.5 s")));
        QCOMPARE(value("mid_release"), 4500.0);
        QVERIFY(release->applyTyped(QStringLiteral("0.2")));
        QCOMPARE(value("mid_release"), 0.2);
        QCOMPARE(release->text(), QStringLiteral("0.20 ms"));
        before = undo()->index();
        QVERIFY(!release->applyTyped(QStringLiteral("soon")));
        QCOMPARE(undo()->index(), before);
        QVERIFY(box(s.view, "midAttack")->applyTyped(QStringLiteral("0.5 ms")));
        QCOMPARE(value("mid_attack"), 0.5);
        // A crossover typed as a frequency.
        QVERIFY(box(s.view, "xoverLow")->applyTyped(QStringLiteral("0.2k")));
        QCOMPARE(value("xover_low"), 200.0);
        QCOMPARE(box(s.view, "xoverLow")->text(), QStringLiteral("200 Hz"));

        // The engine has what the editor set.
        const auto id = bridge()->engineDeviceId(s.track, s.device);
        QVERIFY(id);
        for (const char* p : {"mid_above_ratio", "mid_solo", "mode", "soft_knee", "xover_low", "high_on", "mid_active"})
            QCOMPARE(engine_->processorParam(*id, engine_->processorParamIndex(*id, p)), float(value(p)));
    }

    void graphDragsAThreshold() {
        const Shown s = showDevice();
        QVERIFY(s.view && s.graph);
        auto value = [&](const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); };
        MultibandGraph* graph = s.graph;

        int before = undo()->index();
        const QPoint at = scenePoint(graph, graph->aboveHandle(kMid));
        drag(graph, graph->aboveHandle(kMid), QPoint(-30, 0));
        const double expected = -20.0 - 30.0 / graph->pixelsPerDb();
        QVERIFY2(std::abs(value("mid_above") - expected) <= 0.2, qPrintable(QString::number(value("mid_above"))));
        QCOMPARE(value("mid_below"), -40.0);
        QVERIFY(oneStepAfter(before));
        QVERIFY(std::abs(scenePoint(graph, graph->aboveHandle(kMid)).x() - (at.x() - 30)) <= 1);
        QCOMPARE(value("low_above"), -20.0);  // (only the band grabbed)
        undo()->undo();
        QCOMPARE(value("mid_above"), -20.0);

        // Above dragged past Below pushes it along: still one step.
        before = undo()->index();
        const int to = int(std::lround(graph->xOfDb(-50.0) - graph->aboveHandle(kMid).x()));
        drag(graph, graph->aboveHandle(kMid), QPoint(to, 0), 4);
        const double pushed = -20.0 + to / graph->pixelsPerDb();  // (about -50, in whole pixels)
        QVERIFY2(std::abs(value("mid_above") - pushed) <= 0.06 && value("mid_above") < -49.0,
                 qPrintable(QString::number(value("mid_above"))));
        QCOMPARE(value("mid_below"), value("mid_above"));
        QVERIFY(oneStepAfter(before));

        // Between the thresholds the press isn't the graph's (the frame's: selecting the device).
        before = undo()->index();
        undo()->undo();
        const QPointF between((graph->xOfDb(-40.0) + graph->xOfDb(-20.0)) / 2, graph->lane(kMid).center().y());
        drag(graph, between, QPoint(-20, 20));
        QCOMPARE(value("mid_above"), -20.0);
        QCOMPARE(value("mid_below"), -40.0);
        QCOMPARE(undo()->index(), before - 1);

        // The engine has what the graph set.
        drag(graph, graph->belowHandle(kLow), QPoint(-20, 0));
        const auto id = bridge()->engineDeviceId(s.track, s.device);
        QVERIFY(id);
        QCOMPARE(engine_->processorParam(*id, engine_->processorParamIndex(*id, "low_below")),
                 float(value("low_below")));
        QVERIFY(value("low_below") < -44.0);
    }

    void theBubbleClearsTheOffLabel() {
        // High off, Mid's Above handle dragged: its bubble, over the lane above, would sit on High's "→ Mid" (it
        // did, by a pixel or two). The label makes way while the bubble is on it, and comes back after.
        const Shown s = showDevice({{QStringLiteral("high_on"), 0.0}});
        QVERIFY(s.view && s.graph);
        MultibandGraph* graph = s.graph;
        QVERIFY(graph->offLabelOpacity(kHigh) > 0.99);  // (shown as it is: snapped)
        QCOMPARE(graph->offLabelOpacity(kMid), 0.0);
        QVERIFY(graph->bubbleRect().isEmpty());
        const QPoint at = scenePoint(graph, graph->aboveHandle(kMid));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at + QPoint(-12, 0));
        dragTo(at + QPoint(-24, 0));
        QVERIFY(graph->bubbleRect().intersects(graph->offLabelRect(kHigh)));  // (they meet)
        ticks(30);
        QVERIFY2(graph->offLabelOpacity(kHigh) < 0.01, qPrintable(QString::number(graph->offLabelOpacity(kHigh))));
        // Towards 0 dB the bubble leaves it: the label is back; over it again, gone.
        dragTo(at + QPoint(50, 0));
        QVERIFY(!graph->bubbleRect().intersects(graph->offLabelRect(kHigh)));
        ticks(30);
        QVERIFY(graph->offLabelOpacity(kHigh) > 0.99);
        dragTo(at + QPoint(-24, 0));
        ticks(30);
        QVERIFY(graph->offLabelOpacity(kHigh) < 0.01);
        // Let go: no bubble, the label back.
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(-24, 0));
        QVERIFY(graph->bubbleRect().isEmpty());
        ticks(45);
        QCOMPARE(graph->offLabelOpacity(kHigh), 1.0);
        QVERIFY(!graph->animating());
    }

    void graphDragsARatio() {
        const Shown s = showDevice();
        QVERIFY(s.view && s.graph);
        auto value = [&](const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); };
        MultibandGraph* graph = s.graph;

        // Above: down compresses, up expands; all one step.
        int before = undo()->index();
        {
            const QPoint at = scenePoint(graph, graph->aboveBlockPoint(kMid));
            QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
            for (int dy = 10; dy <= 30; dy += 10)
                dragTo(at + QPoint(0, dy));
            QVERIFY2(std::abs(value("mid_above_ratio") - 2.0) <= 0.02,
                     qPrintable(QString::number(value("mid_above_ratio"))));
            for (int dy = 20; dy >= -30; dy -= 10)
                dragTo(at + QPoint(0, dy));
            QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 30));
            QVERIFY2(std::abs(value("mid_above_ratio") - 0.5) <= 0.01,
                     qPrintable(QString::number(value("mid_above_ratio"))));
        }
        QVERIFY(oneStepAfter(before));

        // Below: up lifts the quiet parts (upward compression).
        before = undo()->index();
        drag(graph, graph->belowBlockPoint(kMid), QPoint(0, -30));
        QVERIFY2(std::abs(value("mid_below_ratio") - 2.0) <= 0.02,
                 qPrintable(QString::number(value("mid_below_ratio"))));
        QVERIFY(oneStepAfter(before));

        // A pixel from 1:1 is 1:1 exactly (the detent).
        drag(graph, graph->aboveBlockPoint(kLow), QPoint(0, 1), 1);
        QCOMPARE(value("low_above_ratio"), 1.0);
        // Shift: finely (a fifth as far).
        drag(graph, graph->aboveBlockPoint(kLow), QPoint(0, -30), 3, Qt::ShiftModifier);
        QVERIFY2(std::abs(value("low_above_ratio") - std::pow(2.0, -0.2)) <= 0.01,
                 qPrintable(QString::number(value("low_above_ratio"))));

        // A band switched off keeps its settings, and its lane still takes drags.
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("high_on"), 0.0);
        before = undo()->index();
        drag(graph, graph->aboveBlockPoint(kHigh), QPoint(0, 30));
        QVERIFY2(std::abs(value("high_above_ratio") - 2.0) <= 0.02,
                 qPrintable(QString::number(value("high_above_ratio"))));
        QVERIFY(oneStepAfter(before));

        const auto id = bridge()->engineDeviceId(s.track, s.device);
        QVERIFY(id);
        for (const char* p : {"mid_above_ratio", "mid_below_ratio", "high_above_ratio"})
            QCOMPARE(engine_->processorParam(*id, engine_->processorParamIndex(*id, p)), float(value(p)));
    }

    void modifiedDrags() {
        const Shown s = showDevice();
        QVERIFY(s.view && s.graph);
        auto value = [&](const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); };
        MultibandGraph* graph = s.graph;

        // Ctrl: every band's Above, by as much.
        int before = undo()->index();
        drag(graph, graph->aboveHandle(kMid), QPoint(-20, 0), 2, Qt::ControlModifier);
        const double moved = -20.0 - 20.0 / graph->pixelsPerDb();
        for (const char* id : {"low_above", "mid_above", "high_above"})
            QVERIFY2(std::abs(value(id) - moved) <= 0.2, id);
        QVERIFY(oneStepAfter(before));
        undo()->undo();
        for (const char* id : {"low_above", "mid_above", "high_above"})
            QCOMPARE(value(id), -20.0);

        // Alt: both thresholds of the band, the gap kept; stopped at the edge as a pair.
        before = undo()->index();
        drag(graph, graph->aboveHandle(kMid), QPoint(20, 0), 2, Qt::AltModifier);
        QVERIFY2(std::abs(value("mid_above") - (-20.0 + 20.0 / graph->pixelsPerDb())) <= 0.2,
                 qPrintable(QString::number(value("mid_above"))));
        QVERIFY(std::abs((value("mid_above") - value("mid_below")) - 20.0) < 1e-9);
        QVERIFY(oneStepAfter(before));
        before = undo()->index();
        drag(graph, graph->aboveHandle(kMid), QPoint(120, 0), 3, Qt::AltModifier);
        QCOMPARE(value("mid_above"), 0.0);
        QCOMPARE(value("mid_below"), -20.0);
        QVERIFY(oneStepAfter(before));
        QCOMPARE(value("low_above"), -20.0);

        // Ctrl+Alt-drag is the device chain's (it scrolls by hand): the graph leaves the press.
        before = undo()->index();
        drag(graph, graph->aboveHandle(kHigh), QPoint(-20, 0), 2, Qt::ControlModifier | Qt::AltModifier);
        QCOMPARE(value("high_above"), -20.0);
        QCOMPARE(undo()->index(), before);

        // Ctrl on a ratio: every band's, times as much.
        before = undo()->index();
        drag(graph, graph->belowBlockPoint(kLow), QPoint(0, -30), 3, Qt::ControlModifier);
        for (const char* id : {"low_below_ratio", "mid_below_ratio", "high_below_ratio"})
            QVERIFY2(std::abs(value(id) - 2.0) <= 0.02, id);
        QVERIFY(oneStepAfter(before));
    }

    void doubleClickAndWheel() {
        const Shown s = showDevice({{QStringLiteral("mid_above"), -35.0}, {QStringLiteral("mid_above_ratio"), 4.0}});
        QVERIFY(s.view && s.graph);
        auto value = [&](const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); };
        MultibandGraph* graph = s.graph;
        QCOMPARE(value("mid_above"), -35.0);

        int before = undo()->index();
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, scenePoint(graph, graph->aboveHandle(kMid)));
        QCOMPARE(value("mid_above"), -20.0);
        QVERIFY(oneStepAfter(before));
        before = undo()->index();
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, scenePoint(graph, graph->aboveBlockPoint(kMid)));
        QCOMPARE(value("mid_above_ratio"), 1.0);
        QVERIFY(oneStepAfter(before));
        // A threshold reset under the other pushes it along.
        editor()->setDeviceParams(s.track, s.device,
                                  {{QStringLiteral("low_below"), -10.0}, {QStringLiteral("low_above"), -5.0}});
        before = undo()->index();
        QTest::mouseDClick(window_, Qt::LeftButton, Qt::NoModifier, scenePoint(graph, graph->aboveHandle(kLow)));
        QCOMPARE(value("low_above"), -20.0);
        QCOMPARE(value("low_below"), -20.0);
        QVERIFY(oneStepAfter(before));

        // The wheel over a block: louder in that region (above: towards expansion), the notches one step.
        before = undo()->index();
        const QPoint block = scenePoint(graph, graph->aboveBlockPoint(kMid));
        for (int i = 0; i < 3; ++i)
            wheel(block, 120);
        QVERIFY2(std::abs(value("mid_above_ratio") - 0.771) <= 0.005,
                 qPrintable(QString::number(value("mid_above_ratio"))));
        QVERIFY(oneStepAfter(before));
        // Over a handle: half a dB a notch.
        before = undo()->index();
        wheel(scenePoint(graph, graph->aboveHandle(kHigh)), -120);
        QCOMPARE(value("high_above"), -20.5);
        QVERIFY(oneStepAfter(before));

        // A high-resolution wheel (a touchpad, a smooth-scrolling mouse): its small steps add up over a run, as a
        // drag's moves do, rather than each being rounded away or held in 1:1's detent.
        before = undo()->index();
        const QPoint lowBlock = scenePoint(graph, graph->aboveBlockPoint(kLow));
        for (int i = 0; i < 16; ++i)  // four notches in quarters
            wheel(lowBlock, 30);
        QVERIFY2(std::abs(value("low_above_ratio") - 0.707) < 1e-9,
                 qPrintable(QString::number(value("low_above_ratio"))));
        for (int i = 0; i < 16; ++i)  // and back
            wheel(lowBlock, -30);
        QCOMPARE(value("low_above_ratio"), 1.0);
        QVERIFY(oneStepAfter(before));
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("low_below"), -40.0);  // (apart from Above)
        before = undo()->index();
        const QPoint lowHandle = scenePoint(graph, graph->aboveHandle(kLow));
        for (int i = 0; i < 16; ++i)  // two notches in eighths
            wheel(lowHandle, 15);
        QCOMPARE(value("low_above"), -19.0);
        wheel(lowHandle, 8);  // (a fifteenth of a notch, 0.03 dB: nothing yet)
        QCOMPARE(value("low_above"), -19.0);
        for (int i = 0; i < 7; ++i)
            wheel(lowHandle, 8);
        QCOMPARE(value("low_above"), -18.7);
        QVERIFY(oneStepAfter(before));
        // Shift+wheel is the device chain's (it scrolls): the graph leaves it.
        before = undo()->index();
        wheel(lowHandle, 120, Qt::ShiftModifier);
        QCOMPARE(value("low_above"), -18.7);
        QCOMPARE(undo()->index(), before);
    }

    void aWheelRunKeepsItsTarget() {
        // A threshold the wheel moves slides out from under the mouse (half a dB, 1.5 px, a notch against a handle's
        // 5 px grab). The run stays on it: a flick of eight notches moves it 4 dB in one undo step, rather than going
        // on with the block's ratio (down) or stopping in the gap between the thresholds (up).
        const Shown s = showDevice({{QStringLiteral("mid_below"), -70.0}});
        QVERIFY(s.view && s.graph);
        auto value = [&](const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); };
        MultibandGraph* graph = s.graph;
        const QPoint handle = scenePoint(graph, graph->aboveHandle(kMid));
        int before = undo()->index();
        for (const int nudge : {0, 1, 2, 1, 0, -1, -2, -1})  // (a hand on the wheel moves the mouse a little)
            wheel(handle + QPoint(nudge, 0), -120);
        QCOMPARE(value("mid_above"), -24.0);
        QCOMPARE(value("mid_above_ratio"), 1.0);
        QVERIFY(oneStepAfter(before));
        // The other way, a run of its own (the last one over), from where the handle is now.
        QTest::qWait(int(WheelGesture::kWindowMs) + 100);
        before = undo()->index();
        const QPoint moved = scenePoint(graph, graph->aboveHandle(kMid));
        for (int i = 0; i < 8; ++i)
            wheel(moved, 120);
        QCOMPARE(value("mid_above"), -20.0);
        QCOMPARE(value("mid_above_ratio"), 1.0);
        QVERIFY(oneStepAfter(before));
        // The mouse moved on to something else (further than a handle's grab): a run on that, at once.
        before = undo()->index();
        wheel(scenePoint(graph, graph->aboveBlockPoint(kMid)), 120);
        QCOMPARE(value("mid_above"), -20.0);
        QVERIFY2(std::abs(value("mid_above_ratio") - 0.917) < 1e-9,
                 qPrintable(QString::number(value("mid_above_ratio"))));
        QVERIFY(oneStepAfter(before));
        // And another lane.
        before = undo()->index();
        wheel(scenePoint(graph, graph->aboveHandle(kLow)), 120);
        QCOMPARE(value("low_above"), -19.5);
        QVERIFY(std::abs(value("mid_above_ratio") - 0.917) < 1e-9);
        QVERIFY(oneStepAfter(before));
    }

    void displaysReachTheGraph() {
        // One band (High and Low off), Peak, Above -20 at 1:4; a tone at 0.5 (-6.02 dB).
        const Shown s = showDevice({{QStringLiteral("low_on"), 0.0},
                                    {QStringLiteral("high_on"), 0.0},
                                    {QStringLiteral("mode"), 0.0},
                                    {QStringLiteral("mid_above"), -20.0},
                                    {QStringLiteral("mid_above_ratio"), 4.0}},
                                   tone(1000.0, kSampleRate, 0.5));
        QVERIFY(s.view && s.graph);
        MultibandGraph* graph = s.graph;
        // Shown as it is, not fading in.
        QVERIFY(!graph->bandOn(kHigh) && !graph->bandOn(kLow) && graph->bandOn(kMid));
        QCOMPARE(graph->laneOpacity(kHigh), 0.35);
        QCOMPARE(graph->laneOpacity(kLow), 0.35);
        QCOMPARE(graph->laneOpacity(kMid), 1.0);
        ticks(30);
        QVERIFY(!graph->animating());  // (nothing playing: nothing moves)

        engine()->renderOffline(0.0, kSampleRate / 2);
        refreshDisplays();
        const double in = 20 * std::log10(0.5);
        const double gain = (in + 20.0) * (1.0 / 4.0 - 1.0);
        QVERIFY2(std::abs(graph->inLevel(kMid) - in) < 0.3, qPrintable(QString::number(graph->inLevel(kMid))));
        QVERIFY2(std::abs(graph->gainTarget(kMid) - gain) < 0.3, qPrintable(QString::number(graph->gainTarget(kMid))));
        QVERIFY2(std::abs(graph->outLevel(kMid) - (in + gain)) < 0.3,
                 qPrintable(QString::number(graph->outLevel(kMid))));
        QVERIFY(graph->animating());
        // The gain eases there.
        QVERIFY(graph->gainShown(kMid) < 0.0 && graph->gainShown(kMid) > graph->gainTarget(kMid));
        ticks(12);  // (no new audio: the readings held)
        QVERIFY(std::abs(graph->gainShown(kMid) - graph->gainTarget(kMid)) < 0.1);
        QVERIFY2(graph->glow(kMid, kAbove) > 0.9, qPrintable(QString::number(graph->glow(kMid, kAbove))));
        QCOMPARE(graph->glow(kMid, kBelow), 0.0);  // (1:1: doing nothing)
        QCOMPARE(graph->glow(kHigh, kAbove), 0.0);
        // The level the static curve gives is the engine's.
        QCOMPARE(graph->staticOutDb(kMid, in), in + multibandGainDb(in, -20.0, 4.0, -40.0, 1.0, false, 100.0));
        QVERIFY(std::abs(graph->staticOutDb(kMid, in) - (in + gain)) < 0.01);

        // The audio stopped: the meters fall, the gain change shown goes down with them (the change between them:
        // lettingGoKeepsTheBarsTogether), the glow fades.
        ticks(75);
        QVERIFY2(graph->outLevel(kMid) < -46.0, qPrintable(QString::number(graph->outLevel(kMid))));
        QVERIFY(std::abs(graph->gainShown(kMid) - (graph->outLevel(kMid) - graph->inLevel(kMid))) < 0.1);
        QVERIFY2(graph->glow(kMid, kAbove) < 0.1, qPrintable(QString::number(graph->glow(kMid, kAbove))));
        // And once all is still (the gain home), it stops repainting.
        ticks(300);
        QVERIFY(!graph->animating());
        QCOMPARE(graph->inLevel(kMid), MultibandGraph::kFloorDb);
        QCOMPARE(graph->outPeak(kMid), MultibandGraph::kFloorDb);
        QCOMPARE(graph->gainShown(kMid), 0.0);
    }

    void aBypassedBandShowsItsLevelOnly() {
        // One band (High and Low off), Peak, Above -20 at 1:4 with Input +6, bypassed by its activator: its
        // lane dims (shown so at once), and it shows the band's level as it comes, without the Input, no change,
        // no glow, no marker and no figure. Activated again, it works.
        const Shown s = showDevice({{QStringLiteral("low_on"), 0.0},
                                    {QStringLiteral("high_on"), 0.0},
                                    {QStringLiteral("mode"), 0.0},
                                    {QStringLiteral("mid_in"), 6.0},
                                    {QStringLiteral("mid_above"), -20.0},
                                    {QStringLiteral("mid_above_ratio"), 4.0},
                                    {QStringLiteral("mid_active"), 0.0}},
                                   tone(1000.0, kSampleRate, 0.5));
        QVERIFY(s.view && s.graph);
        MultibandGraph* graph = s.graph;
        QVERIFY(!graph->bandActive(kMid));
        QCOMPARE(graph->laneOpacity(kMid), 0.35);
        engine()->renderOffline(0.0, kSampleRate / 2);
        ticks(10);
        const double in = 20 * std::log10(0.5);
        QVERIFY2(std::abs(graph->inLevel(kMid) - in) < 0.3, qPrintable(QString::number(graph->inLevel(kMid))));
        QVERIFY2(std::abs(graph->outLevel(kMid) - in) < 0.3, qPrintable(QString::number(graph->outLevel(kMid))));
        QCOMPARE(graph->gainTarget(kMid), 0.0);
        QCOMPARE(graph->glow(kMid, kAbove), 0.0);
        QVERIFY(!graph->targetMarkerDb(kMid));
        QVERIFY(graph->gainLabelRect(kMid).isEmpty());

        click(button(s.view, "midActive"));
        QCOMPARE(param(s.track, s.device, QStringLiteral("mid_active")), 1.0);
        engine()->renderOffline(0.0, kSampleRate / 2);
        ticks(13);
        const double driven = in + 6.0;
        QVERIFY2(std::abs(graph->inLevel(kMid) - driven) < 0.3, qPrintable(QString::number(graph->inLevel(kMid))));
        QVERIFY2(std::abs(graph->gainTarget(kMid) - (driven + 20.0) * -0.75) < 0.3,
                 qPrintable(QString::number(graph->gainTarget(kMid))));
        QVERIFY(graph->glow(kMid, kAbove) > 0.9);
        QVERIFY(!graph->gainLabelRect(kMid).isEmpty());
        QVERIFY2(graph->laneOpacity(kMid) > 0.9, qPrintable(QString::number(graph->laneOpacity(kMid))));
    }

    void aCutUnderTheFloorStopsAtTheLevelBefore() {
        // Gate-like downward expansion, one band, Peak, Below -40 at 1:0.250: a steady tone at -55 dB comes out
        // at -100, under the graph's floor.
        const Shown s = showDevice({{QStringLiteral("low_on"), 0.0},
                                    {QStringLiteral("high_on"), 0.0},
                                    {QStringLiteral("mode"), 0.0},
                                    {QStringLiteral("mid_below"), -40.0},
                                    {QStringLiteral("mid_below_ratio"), 0.25}},
                                   tone(1000.0, kSampleRate, std::pow(10.0, -55.0 / 20.0)));
        QVERIFY(s.view && s.graph);
        MultibandGraph* graph = s.graph;
        engine()->renderOffline(0.0, kSampleRate / 2);
        ticks(13);  // (within the hold: as if it played on)
        QVERIFY2(std::abs(graph->inLevel(kMid) + 55.0) < 0.3, qPrintable(QString::number(graph->inLevel(kMid))));
        QCOMPARE(graph->outLevel(kMid), MultibandGraph::kFloorDb);
        QVERIFY2(std::abs(graph->gainShown(kMid) + 45.0) < 0.5, qPrintable(QString::number(graph->gainShown(kMid))));
        // The change drawn runs from the out meter, at the floor, to the level before it, not on past it.
        const auto [from, to] = graph->changeSpan(kMid);
        QCOMPARE(to, MultibandGraph::kFloorDb);
        QCOMPARE(from, graph->inLevel(kMid));
        // Settled there: no target marker (a target under the floor is at it).
        QVERIFY(!graph->targetMarkerDb(kMid));
    }

    void lettingGoKeepsTheBarsTogether() {
        // Once the audio stops the meters fall at 36 dB/s, for seconds; the change drawn between them and its figure
        // go down with them rather than easing home at once (which left a lifted band's out bar in the meters'
        // colours past its input, reading "0.0", and a cut's short of it with no orange).
        const double in = 20 * std::log10(0.5);
        const auto oneBand = [](const char* id, double threshold, double ratio) {
            return sub::app::OrderedMap<QString, double>{{QStringLiteral("low_on"), 0.0},
                                                         {QStringLiteral("high_on"), 0.0},
                                                         {QStringLiteral("mode"), 0.0},
                                                         {QStringLiteral("mid_") + QLatin1String(id), threshold},
                                                         {QStringLiteral("mid_%1_ratio").arg(id), ratio}};
        };
        // A lift: Above -24 at 1:0.500 doubles the 18 dB over it, to +12 dB; the in meter reaches the floor first.
        Shown s = showDevice(oneBand("above", -24.0, 0.5), tone(1000.0, kSampleRate, 0.5));
        QVERIFY(s.view && s.graph);
        engine()->renderOffline(0.0, kSampleRate / 2);
        ticks(13);  // (within the hold: as if it played on)
        QVERIFY2(std::abs(s.graph->gainShown(kMid) - (in + 24.0)) < 0.3,
                 qPrintable(QString::number(s.graph->gainShown(kMid))));
        int ticked = 0;
        QString wrong = lettingGo(s.graph, kMid, s.graph->gainTarget(kMid), &ticked);
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
        QVERIFY2(ticked > 150, qPrintable(QString::number(ticked)));  // (+12 dB to the floor: 2.6 s, 160 ticks)

        // A cut: Above -20 at 1:4 takes 10.5 dB off; the out meter reaches the floor first.
        clearHost();
        s = showDevice(oneBand("above", -20.0, 4.0), tone(1000.0, kSampleRate, 0.5));
        QVERIFY(s.view && s.graph);
        engine()->renderOffline(0.0, kSampleRate / 2);
        ticks(13);
        QVERIFY(s.graph->gainShown(kMid) < -10.0);
        wrong = lettingGo(s.graph, kMid, s.graph->gainTarget(kMid));
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));

        // A cut under the floor all along (Below -40 at 1:0.250, a -55 dB tone: out at -100): the change drawn runs
        // from the falling in meter to the floor, its figure the reading, until the in meter is at the floor too.
        clearHost();
        s = showDevice(oneBand("below", -40.0, 0.25), tone(1000.0, kSampleRate, std::pow(10.0, -55.0 / 20.0)));
        QVERIFY(s.view && s.graph);
        engine()->renderOffline(0.0, kSampleRate / 2);
        ticks(13);
        QCOMPARE(s.graph->outLevel(kMid), MultibandGraph::kFloorDb);
        QVERIFY(s.graph->gainShown(kMid) < -44.0);
        wrong = lettingGo(s.graph, kMid, s.graph->gainTarget(kMid));
        QVERIFY2(wrong.isEmpty(), qPrintable(wrong));
    }

    void glowsWhileTheBandIsWorked() {
        // Over The Top's kind of Below (-40 at 1:4, lifting quiet sound) on one band.
        const Shown s = showDevice({{QStringLiteral("low_on"), 0.0},
                                    {QStringLiteral("high_on"), 0.0},
                                    {QStringLiteral("mode"), 0.0},
                                    {QStringLiteral("mid_below"), -40.0},
                                    {QStringLiteral("mid_below_ratio"), 4.0}},
                                   tone(1000.0, kSampleRate, 0.5));
        QVERIFY(s.view && s.graph);
        MultibandGraph* graph = s.graph;
        // A tone at -6 dB, which it leaves alone. Once the audio stops the meter falls through the Below region,
        // but that isn't the band being lifted: no glow, no marker.
        engine()->renderOffline(0.0, kSampleRate / 2);
        double most = 0.0, lowest = 0.0;
        bool marker = false;
        for (int i = 0; i < 200; ++i) {
            refreshDisplays();
            most = std::max(most, graph->glow(kMid, kBelow));
            marker = marker || graph->targetMarkerDb(kMid).has_value();
            lowest = std::min(lowest, graph->inLevel(kMid));
        }
        QVERIFY(lowest < -60.0);  // (the meter did fall through it)
        QVERIFY2(most < 0.01, qPrintable(QString::number(most)));
        QVERIFY(!marker);
        // Under the threshold (Input -24 dB, Below -20: 10 dB under), the Below side glows while it plays, and
        // lets go once the audio has stopped.
        editor()->setDeviceParams(s.track, s.device,
                                  {{QStringLiteral("mid_in"), -24.0}, {QStringLiteral("mid_below"), -20.0}});
        engine()->renderOffline(0.0, kSampleRate / 2);
        ticks(13);
        QVERIFY2(graph->glow(kMid, kBelow) > 0.9, qPrintable(QString::number(graph->glow(kMid, kBelow))));
        QVERIFY2(std::abs(graph->gainTarget(kMid) - 7.5) < 0.2, qPrintable(QString::number(graph->gainTarget(kMid))));
        ticks(kHoldTicks + 60);
        QVERIFY2(graph->glow(kMid, kBelow) < 0.1, qPrintable(QString::number(graph->glow(kMid, kBelow))));
    }

    void aWholeBufferIsRead() {
        // A 2048-sample buffer's eight display values arrive at once (43 ms at 48 kHz) and a tick reads them
        // all: a 10 kHz burst in its first quarter shows on the high band.
        std::vector<float> burst(2048, 0.f);
        const std::vector<float> part = tone(10000.0, 512, 0.5);
        std::copy(part.begin(), part.end(), burst.begin());
        const Shown s = showDevice({{QStringLiteral("mode"), 0.0}}, burst);
        QVERIFY(s.view && s.graph);
        engine()->renderOffline(0.0, 2048);
        refreshDisplays();
        QVERIFY2(s.graph->inLevel(kHigh) > -7.0, qPrintable(QString::number(s.graph->inLevel(kHigh))));
    }

    void lanesAndHighlightsEase() {
        const Shown s = showDevice();
        QVERIFY(s.view && s.graph);
        MultibandGraph* graph = s.graph;
        QCOMPARE(graph->laneOpacity(kHigh), 1.0);

        click(button(s.view, "highOn"));
        QVERIFY(!graph->bandOn(kHigh));
        refreshDisplays();
        QVERIFY(graph->laneOpacity(kHigh) < 1.0 && graph->laneOpacity(kHigh) > 0.35);
        ticks(25);
        QVERIFY(std::abs(graph->laneOpacity(kHigh) - 0.35) < 0.01);

        click(button(s.view, "midSolo"));  // Low muted by it (High is off: it follows Mid)
        refreshDisplays();
        QVERIFY(graph->laneOpacity(kLow) < 1.0 && graph->laneOpacity(kLow) > 0.5);
        ticks(25);
        QVERIFY(std::abs(graph->laneOpacity(kLow) - 0.5) < 0.01);
        QCOMPARE(graph->laneOpacity(kMid), 1.0);

        // Under the mouse, a handle lights up and the cursor says what a drag does.
        QTest::mouseMove(window_, scenePoint(graph, graph->aboveHandle(kMid)));
        ticks(10);
        QVERIFY2(graph->highlight(kMid, kAbove, true) > 0.9,
                 qPrintable(QString::number(graph->highlight(kMid, kAbove, true))));
        QCOMPARE(graph->cursor().shape(), Qt::SizeHorCursor);
        QTest::mouseMove(window_, scenePoint(graph, graph->aboveBlockPoint(kMid)));
        QCOMPARE(graph->cursor().shape(), Qt::SizeVerCursor);
        ticks(25);
        QVERIFY(graph->highlight(kMid, kAbove, true) < 0.1);
        QVERIFY(graph->highlight(kMid, kAbove, false) > 0.9);
        QTest::mouseMove(window_, QPoint(1, 1));
        ticks(60);
        QVERIFY(graph->highlight(kMid, kAbove, false) == 0.0);
        QVERIFY(!graph->animating());

        // A band bypassed (its activator) dims as one switched off, solo or not, and comes back.
        click(button(s.view, "lowActive"));
        QVERIFY(!graph->bandActive(kLow));
        ticks(25);
        QVERIFY(std::abs(graph->laneOpacity(kLow) - 0.35) < 0.01);
        QCOMPARE(graph->offLabelOpacity(kLow), 0.0);  // ("→ Mid" is for a band switched off)
        click(button(s.view, "lowActive"));
        ticks(25);
        QVERIFY(std::abs(graph->laneOpacity(kLow) - 0.5) < 0.01);  // (muted by Mid's solo still)
    }

    void theReadoutClearsTheGainFigure() {
        // One band playing a tone at 0.5, Above -20 at 1:4: the gain change's figure (-8.2) at the lane's top right.
        // The static curve's readout under the mouse, at the same height, sits beside the hairline, right of it where
        // it fits, and never over the figure (it hid all but its last digit from -1 to +6 dB).
        const Shown s = showDevice({{QStringLiteral("low_on"), 0.0},
                                    {QStringLiteral("high_on"), 0.0},
                                    {QStringLiteral("mid_above"), -20.0},
                                    {QStringLiteral("mid_above_ratio"), 4.0}},
                                   tone(1000.0, kSampleRate, 0.5));
        QVERIFY(s.view && s.graph);
        MultibandGraph* graph = s.graph;
        engine()->renderOffline(0.0, kSampleRate / 2);
        ticks(10);
        const QRectF figure = graph->gainLabelRect(kMid), lane = graph->lane(kMid);
        QVERIFY2(graph->gainShown(kMid) < -8.0 && !figure.isEmpty(),
                 qPrintable(QString::number(graph->gainShown(kMid))));
        QVERIFY(graph->hoverLabelRect(kMid).isEmpty());  // (no mouse)
        for (double db = -79.5; db <= 6.0; db += 0.5) {
            const QPoint at = scenePoint(graph, QPointF(graph->xOfDb(db), lane.center().y() + 12.0));
            QTest::mouseMove(window_, at);
            const double hairline = graph->mapFromScene(QPointF(at)).x();
            const QRectF readout = graph->hoverLabelRect(kMid);
            const QString where = QStringLiteral("%1 dB: readout %2..%3, figure %4..%5")
                                      .arg(db)
                                      .arg(readout.left())
                                      .arg(readout.right())
                                      .arg(figure.left())
                                      .arg(figure.right());
            QVERIFY2(!readout.isEmpty() && !readout.intersects(figure), qPrintable(where));
            QVERIFY2(readout.left() >= lane.left() && readout.right() <= lane.right(), qPrintable(where));
            // Beside the hairline, right of it while there is room before the figure.
            if (hairline + 4.0 + readout.width() <= figure.left() - 4.0)
                QVERIFY2(std::abs(readout.left() - (hairline + 4.0)) < 1e-9, qPrintable(where));
            else
                QVERIFY2(readout.right() <= hairline - 4.0 + 1e-9, qPrintable(where));
            QVERIFY(graph->hoverLabelRect(kHigh).isEmpty() && graph->hoverLabelRect(kLow).isEmpty());
        }
        QTest::mouseMove(window_, scenePoint(graph, QPointF(graph->xOfDb(4.0), lane.center().y() + 12.0)));
        save(grab(), QStringLiteral("multiband-readout.png"));  // (the readout ending short of the figure)
    }

    void sidechainControls() {
        const Shown s = showDevice();
        QVERIFY(s.view && s.graph);
        auto value = [&](const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); };
        QVERIFY(!s.graph->sidechained());
        // Without a sidechain, S/C Gain and Mix dim but can be set first (as the Gate's, and Live's greyed ones).
        QQuickItem* gain = find(s.view, QStringLiteral("scGain"));
        QQuickItem* mix = find(s.view, QStringLiteral("scMix"));
        QVERIFY(gain->isEnabled() && mix->isEnabled());
        QTRY_COMPARE(gain->opacity(), 0.55);
        QTRY_COMPARE(mix->opacity(), 0.55);
        QVERIFY(!find(s.view, QStringLiteral("sidechainButton"))->property("checked").toBool());
        auto* knob = qvariant_cast<KnobItem*>(qvariant_cast<QQuickItem*>(gain->property("knob"))->property("knob"));
        QVERIFY(knob);
        const int before = undo()->index();
        wheel(centerOf(knob), 120);
        QVERIFY2(value("sc_gain") > 0.0, qPrintable(QString::number(value("sc_gain"))));
        QVERIFY(oneStepAfter(before));
        // Their readouts are whole, the longest too ("-70.0 dB", the least S/C Gain).
        editor()->setDeviceParam(s.track, s.device, QStringLiteral("sc_gain"), -70.0);
        for (QQuickItem* cell : {gain, mix}) {
            const QList<QQuickItem*> children = cell->childItems();
            QQuickItem* readout = children.last();
            QVERIFY(!readout->property("text").toString().isEmpty());
            QVERIFY2(!readout->property("truncated").toBool(), qPrintable(readout->property("text").toString()));
        }
        QCOMPARE(qvariant_cast<QQuickItem*>(mix->property("knob"))->property("bipolar").toBool(), false);
        QVERIFY(qvariant_cast<QQuickItem*>(find(s.view, QStringLiteral("output"))->property("knob"))
                    ->property("bipolar")
                    .toBool());  // (±24 dB: drawn from the middle)

        const QString other = audioTrackWith(tone(60.0, kSampleRate), fileName("kick"), 1.0);
        QVERIFY(!other.isEmpty());
        QVERIFY(editor()->trySetDeviceSidechain(s.track, s.device, other));
        QVERIFY(s.graph->sidechained());
        QTRY_COMPARE(gain->opacity(), 1.0);
        QTRY_COMPARE(mix->opacity(), 1.0);
        QVERIFY(find(s.view, QStringLiteral("sidechainButton"))->property("checked").toBool());

        // The button asks for the sidechain's menu under itself.
        QSignalSpy asked(s.view, SIGNAL(sidechainMenuRequested(QVariant)));
        QQuickItem* chooser = find(s.view, QStringLiteral("sidechainButton"));
        click(chooser);
        QCOMPARE(asked.count(), 1);
        QCOMPARE(qvariant_cast<QQuickItem*>(asked.first().first()), chooser);
        // Its text clear of its borders: the small role's padding, and room for the text within it.
        const double padding =
            chooser->property("leftPadding").toDouble() + chooser->property("rightPadding").toDouble();
        QVERIFY(chooser->property("leftPadding").toDouble() >= 7 && chooser->property("rightPadding").toDouble() >= 7);
        QVERIFY2(chooser->property("implicitContentWidth").toDouble() <= chooser->width() - padding,
                 qPrintable(QString::number(chooser->property("implicitContentWidth").toDouble())));

        // Listen: a switch (not automatable), one step; the engine has it.
        const int listenBefore = undo()->index();
        click(button(s.view, "scListen"));
        QCOMPARE(value("sc_listen"), 1.0);
        QVERIFY(oneStepAfter(listenBefore));
        QVERIFY(find(s.view, QStringLiteral("scListen"))->property("lit").toBool());
        const auto id = bridge()->engineDeviceId(s.track, s.device);
        QVERIFY(id);
        QCOMPARE(engine_->processorParam(*id, engine_->processorParamIndex(*id, "sc_listen")), 1.f);
    }

    void ratioTexts() {
        // Live's notation, "1:R" (R dB past the threshold come out as 1), three significant digits.
        QCOMPARE(formatValue(4.0, QStringLiteral("ratio")), QStringLiteral("1:4.00"));
        QCOMPARE(formatValue(66.7, QStringLiteral("ratio")), QStringLiteral("1:66.7"));
        QCOMPARE(formatValue(100.0, QStringLiteral("ratio")), QStringLiteral("1:100"));
        QCOMPARE(formatValue(1.0, QStringLiteral("ratio")), QStringLiteral("1:1.00"));
        QCOMPARE(formatValue(0.5, QStringLiteral("ratio")), QStringLiteral("1:0.500"));
        QCOMPARE(formatValue(0.25, QStringLiteral("ratio")), QStringLiteral("1:0.250"));
        QCOMPARE(formatValue(10.0, QStringLiteral("ratio")), QStringLiteral("1:10.0"));
        // Counted on the value as rounded: just under a decade it rounds into the next one's digits.
        QCOMPARE(formatValue(9.996, QStringLiteral("ratio")), QStringLiteral("1:10.0"));
        QCOMPARE(formatValue(99.96, QStringLiteral("ratio")), QStringLiteral("1:100"));
        QCOMPARE(formatValue(0.9996, QStringLiteral("ratio")), QStringLiteral("1:1.00"));
        QCOMPARE(formatValue(9.994, QStringLiteral("ratio")), QStringLiteral("1:9.99"));
        QCOMPARE(formatValue(0.9994, QStringLiteral("ratio")), QStringLiteral("1:0.999"));

        // Typed: Live's way, the R alone, or as a compressor writes it ("4:1"; any "a:b" not starting with 1).
        QCOMPARE(multibandParseRatio(QStringLiteral("1:4")), 4.0);
        QCOMPARE(multibandParseRatio(QStringLiteral("1:66.7")), 66.7);  // Over The Top's Above
        QCOMPARE(multibandParseRatio(QStringLiteral(" 1 : 0.5 ")), 0.5);
        QCOMPARE(multibandParseRatio(QStringLiteral("1:inf")), 100.0);  // (Live's brick wall: held)
        QCOMPARE(multibandParseRatio(QStringLiteral("4")), 4.0);
        QCOMPARE(multibandParseRatio(QStringLiteral("0.5")), 0.5);
        QCOMPARE(multibandParseRatio(QStringLiteral("4:1")), 4.0);
        QCOMPARE(multibandParseRatio(QStringLiteral("4.00:1")), 4.0);
        QCOMPARE(multibandParseRatio(QStringLiteral("3:2")), 1.5);
        QCOMPARE(multibandParseRatio(QStringLiteral("1000")), 100.0);  // held
        QCOMPARE(multibandParseRatio(QStringLiteral("1:0.1")), 0.25);
        for (const char* text : {"x", "", "1:0", "0", "-2", "1:2:3", "a:1", "1:-4", "nan", "-inf"})
            QVERIFY2(multibandParseRatio(QString::fromLatin1(text)) == 0.0, text);
        for (const double r : {0.25, 0.3, 0.333, 0.5, 0.77, 1.0, 1.5, 3.33, 4.0, 9.99, 10.0, 66.7, 99.5, 100.0}) {
            const double back = multibandParseRatio(formatValue(r, QStringLiteral("ratio")));
            QVERIFY2(std::abs(back / r - 1.0) < 0.005, qPrintable(QString::number(r)));
        }
        // The static curve is the engine's law: past a threshold, the distance from it divided by the ratio.
        QVERIFY(std::abs(multibandGainDb(-10.0, -20.0, 4.0, -40.0, 1.0, false, 100.0) + 7.5) < 1e-5);
        QVERIFY(std::abs(multibandGainDb(-60.0, -20.0, 1.0, -40.0, 4.0, false, 100.0) - 15.0) < 1e-5);
        QVERIFY(std::abs(multibandGainDb(-60.0, -20.0, 1.0, -40.0, 4.0, false, 50.0) - 7.5) < 1e-5);
        QVERIFY(std::abs(multibandGainDb(-20.0, -20.0, 4.0, -40.0, 1.0, true, 100.0) + 0.5625) < 1e-5);
    }

    void timeTexts() {
        // Attack and release typed in either unit their boxes show; a bare number is milliseconds.
        QCOMPARE(multibandParseMs(QStringLiteral("250")), std::optional<double>(250.0));
        QCOMPARE(multibandParseMs(QStringLiteral("250 ms")), std::optional<double>(250.0));
        QCOMPARE(multibandParseMs(QStringLiteral("250ms")), std::optional<double>(250.0));
        QCOMPARE(multibandParseMs(QStringLiteral("1.5 s")), std::optional<double>(1500.0));
        QCOMPARE(multibandParseMs(QStringLiteral(" 2 S ")), std::optional<double>(2000.0));
        QCOMPARE(multibandParseMs(QStringLiteral("0.1 ms")), std::optional<double>(0.1));
        QCOMPARE(multibandParseMs(QStringLiteral("0")), std::optional<double>(0.0));
        for (const char* text : {"", "x", "ms", "s", "-5 ms", "1.5 min", "nan", "inf"})
            QVERIFY2(!multibandParseMs(QString::fromLatin1(text)), text);
        // What the boxes show reads back.
        for (const double ms : {0.15, 5.0, 99.0, 250.0, 1500.0, 3000.0}) {
            const std::optional<double> back = multibandParseMs(formatValue(ms, QStringLiteral("ms")));
            QVERIFY2(back && std::abs(*back / ms - 1.0) < 0.01, qPrintable(QString::number(ms)));
        }
    }

    void screenshot() {
        std::vector<float> mix = tone(40.0, kSampleRate, 0.3);
        for (const double f : {1000.0, 10000.0}) {
            const std::vector<float> part = tone(f, kSampleRate, f == 1000.0 ? 0.3 : 0.15);
            for (std::size_t i = 0; i < mix.size(); ++i)
                mix[i] += part[i];
        }
        const Shown s = showDevice({{QStringLiteral("high_above"), -30.0},
                                    {QStringLiteral("high_above_ratio"), 8.0},
                                    {QStringLiteral("mid_below"), -50.0},
                                    {QStringLiteral("mid_below_ratio"), 3.0},
                                    {QStringLiteral("mid_above"), -14.0},
                                    {QStringLiteral("mid_above_ratio"), 2.0},
                                    {QStringLiteral("low_above"), -24.0},
                                    {QStringLiteral("low_above_ratio"), 0.5}},
                                   mix);
        QVERIFY(s.view && s.graph);
        // Rendered, then ticks with no new audio (within the hold, so it reads as playing on), then
        // rendered again so the meters hold what is drawn while the window paints.
        engine()->renderOffline(0.0, kSampleRate / 2);
        ticks(12);
        engine()->renderOffline(0.0, kSampleRate / 2);
        refreshDisplays();
        QVERIFY(s.graph->gainTarget(kHigh) < -6.0 && s.graph->gainTarget(kLow) > 6.0);  // cut and lifted
        QVERIFY(std::abs(s.graph->gainShown(kHigh) - s.graph->gainTarget(kHigh)) < 0.1);
        QTest::qWait(30);
        save(grab(), QStringLiteral("multiband.png"));

        // High off, Low bypassed, the Below fields, the Mid Above handle dragged (its value over it).
        editor()->setDeviceParams(s.track, s.device,
                                  {{QStringLiteral("high_on"), 0.0}, {QStringLiteral("low_active"), 0.0}});
        click(find(s.view, QStringLiteral("pageBelow")));
        ticks(30);
        QTest::qWait(200);  // (the fields' fades)
        const QPoint at = scenePoint(s.graph, s.graph->aboveHandle(kMid));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
        dragTo(at + QPoint(-12, 0));
        dragTo(at + QPoint(-24, 0));
        ticks(100);  // (High's "→ Mid" makes way for the bubble; its meter, off, falls to the floor)
        engine()->renderOffline(0.0, kSampleRate / 2);
        ticks(10);
        QTest::qWait(30);
        save(grab(), QStringLiteral("multiband-drag.png"));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at + QPoint(-24, 0));

        // The Time fields, keyed by a sidechain, Soft Knee, the static curve read under the mouse.
        const QString kick = audioTrackWith(tone(60.0, kSampleRate, 0.8), fileName("kick"), 1.0);
        QVERIFY(editor()->trySetDeviceSidechain(s.track, s.device, kick));
        editor()->setDeviceParams(s.track, s.device,
                                  {{QStringLiteral("high_on"), 1.0},
                                   {QStringLiteral("low_active"), 1.0},
                                   {QStringLiteral("soft_knee"), 1.0},
                                   {QStringLiteral("low_below"), -60.0},
                                   {QStringLiteral("low_below_ratio"), 0.4}});
        click(find(s.view, QStringLiteral("pageTime")));
        QTest::qWait(200);
        QTest::mouseMove(window_,
                         scenePoint(s.graph, QPointF(s.graph->xOfDb(-12.0), s.graph->lane(kMid).center().y())));
        ticks(300);  // (the meters let go of what played before)
        engine()->renderOffline(0.0, kSampleRate / 2);
        ticks(10);
        for (int i = 0; i < 50 && s.graph->inLevel(kLow) < -20.0; ++i) {  // (until the key's clip has come)
            QTest::qWait(20);
            engine()->renderOffline(0.0, kSampleRate / 2);
            ticks(10);
        }
        QVERIFY2(s.graph->inLevel(kLow) > -20.0,  // keyed by the kick
                 qPrintable(QStringLiteral("low %1 mid %2 high %3 gain %4 sidechained %5")
                                .arg(s.graph->inLevel(kLow))
                                .arg(s.graph->inLevel(kMid))
                                .arg(s.graph->inLevel(kHigh))
                                .arg(s.graph->gainTarget(kLow))
                                .arg(s.graph->sidechained())));
        QTest::qWait(30);
        save(grab(), QStringLiteral("multiband-time.png"));

        // Automated boxes (the dot at their left) still read whole: "-12.0 dB", "1:0.500", "0.15 ms".
        editor()->setDeviceParams(s.track, s.device,
                                  {{QStringLiteral("mid_in"), -12.0},
                                   {QStringLiteral("mid_out"), -24.0},
                                   {QStringLiteral("high_in"), -24.0},
                                   {QStringLiteral("mid_attack"), 0.15},
                                   {QStringLiteral("low_release"), 999.0}});
        for (const char* name : {"highIn", "midIn", "lowIn", "highOut", "midOut", "lowOut", "highAttack",
                                 "midAttack", "lowAttack", "highRelease", "midRelease", "lowRelease", "xoverHigh"})
            box(s.view, name)->setProperty("automation", QStringLiteral("on"));
        QTest::qWait(30);
        save(grab(), QStringLiteral("multiband-automation-time.png"));
        click(find(s.view, QStringLiteral("pageAbove")));
        for (const char* name :
             {"highAbove", "midAbove", "lowAbove", "highAboveRatio", "midAboveRatio", "lowAboveRatio"})
            box(s.view, name)->setProperty("automation", QStringLiteral("on"));
        QTest::qWait(200);  // (the page's fade)
        save(grab(), QStringLiteral("multiband-automation.png"));
    }
};

QTEST_MAIN(TestUiDeviceEditorsMultiband)
#include "test_ui_device_editors_multiband.moc"
