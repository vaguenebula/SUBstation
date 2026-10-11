// The Reverb's editor (ui/qml/devices/editors/ReverbEditor.qml; ui/src/devices/ReverbFilterPad, ReverbSpinPad,
// ReverbDecayGraph): loaded as the device view loads it, over a real engine. It fits the view's height (and its own
// least height), nothing overlapping, its boxes, lists and switches as wide as their text (the boxes' every value a
// pixel clear of the automation dot) and its knobs the house's 34 px in rows; every caption and readout reads whole at
// its widest, whatever the font, and nothing is wider than its text needs; every control is bound to its parameter
// (undoably, with a tooltip), each switch the one under the mouse over it (Chorus's over its knob's caption too); the
// pads' and the graph's drags are one undo step each and set what the engine plays; the curves are the engine's own
// maths (ReverbResponse.h); what the engine publishes as it renders reaches the pads and the graph (lit by what is now,
// not by a backlog's loudest), which animate and then rest. With SUBSTATION_UI_SCREENSHOTS set to a folder, it is saved
// there playing, frozen, in other modes, and with every box automated at its widest value.

#include <QCursor>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QLineF>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <QUndoStack>

#include <algorithm>
#include <cmath>
#include <tuple>
#include <vector>

#include "EditorHarness.h"
#include "analysis/Spectrum.h"
#include "audio/ReverbResponse.h"
#include "controls/KnobItem.h"
#include "controls/ValueBoxItem.h"
#include "devices/DeviceParam.h"
#include "devices/ReverbDecayGraph.h"
#include "devices/ReverbFilterPad.h"
#include "devices/ReverbSpinPad.h"
#include "model/ParamSpec.h"

using namespace sub::app;
using namespace sub::ui;
using sub::app::test::kSampleRate;

namespace {

// Every control of the editor and the parameter it is bound to (the pads and the graph aside).
const QList<std::pair<const char*, const char*>> kControls = {
    {"loCutButton", "lo_cut"},       {"hiCutButton", "hi_cut"},         {"inFreqBox", "in_freq"},
    {"inWidthBox", "in_width"},      {"spinButton", "spin"},            {"spinAmountBox", "spin_amount"},
    {"spinRateBox", "spin_rate"},    {"shapeKnob", "shape"},            {"predelayKnob", "predelay"},
    {"sizeKnob", "size"},            {"stereoKnob", "stereo"},          {"densityChoice", "density"},
    {"smoothChoice", "smooth"},      {"loShelfButton", "lo_shelf"},     {"hiFilterButton", "hi_filter"},
    {"hiTypeChoice", "hi_type"},     {"loFreqBox", "lo_freq"},          {"loGainBox", "lo_gain"},
    {"hiFreqBox", "hi_freq"},        {"hiGainBox", "hi_gain"},          {"decayKnob", "decay"},
    {"freezeButton", "freeze"},      {"flatButton", "flat"},            {"cutButton", "cut"},
    {"diffusionKnob", "diffusion"},  {"scaleKnob", "scale"},            {"chorusButton", "chorus"},
    {"chorusAmountKnob", "chorus_amount"}, {"chorusRateKnob", "chorus_rate"}, {"reflectKnob", "reflect"},
    {"diffuseKnob", "diffuse"},      {"mixKnob", "mix"}};
const QStringList kCanvases = {QStringLiteral("filterPad"), QStringLiteral("spinPad"), QStringLiteral("decayGraph")};
// A value box's automation dot reaches this far in from its left (ValueBoxItem: 2.5 px round at 6 px), lighting
// part of the pixel there; its text keeps a pixel clear of it, drawn from kTextFrom px in or further (ValueBoxItem
// centres it on a whole pixel).
constexpr double kDotRight = 8.5, kTextFrom = kDotRight + 1.5;
// The knobs of each row, left to right.
const QList<const char*> kFirstRow = {"shapeKnob",     "sizeKnob",         "stereoKnob",  "decayKnob",
                                      "diffusionKnob", "chorusAmountKnob", "reflectKnob", "mixKnob"};
const QList<const char*> kSecondRow = {"predelayKnob", "scaleKnob", "chorusRateKnob", "diffuseKnob"};

// How far `text` reaches from where it starts in `metrics`' font: its advance, or its last glyph's ink past it.
double extentOf(const QString& text, const QFontMetricsF& metrics) {
    return std::max(metrics.horizontalAdvance(text), metrics.boundingRect(text).right());
}

// `text` with every figure the font's widest (as the editor's sample texts have them).
QString widened(QString text, const QFontMetricsF& metrics) {
    QChar widest = u'0';
    for (const QChar digit : QStringLiteral("123456789"))
        if (metrics.horizontalAdvance(digit) > metrics.horizontalAdvance(widest))
            widest = digit;
    for (QChar& c : text)
        if (c.isDigit())
            c = widest;
    return text;
}

// A parameter's values over its range: 2001 of them, evenly (in log for a log-scaled one).
std::vector<double> valuesOf(const sub::ui::DeviceParam* p) {
    std::vector<double> values;
    for (int i = 0; i <= 2000; ++i) {
        const double t = i / 2000.0;
        values.push_back(p->logScale() ? p->minimum() * std::pow(p->maximum() / p->minimum(), t)
                                       : p->minimum() + t * (p->maximum() - p->minimum()));
    }
    return values;
}

// Of a parameter's values, the one whose text reaches widest in `metrics`' font.
double widestValue(const sub::ui::DeviceParam* p, const QFontMetricsF& metrics) {
    double widest = p->minimum(), most = -1.0;
    for (const double v : valuesOf(p)) {
        if (const double extent = extentOf(p->format(v), metrics); extent > most) {
            most = extent;
            widest = v;
        }
    }
    return widest;
}

// The first of `frequencies` (rising) at or above `hz`.
std::size_t indexOf(const std::vector<double>& frequencies, double hz) {
    return std::size_t(std::lower_bound(frequencies.begin(), frequencies.end(), hz) - frequencies.begin());
}

}  // namespace

class TestUiDeviceEditorsReverb : public QObject, public sub::app::test::EditorHarness {
    Q_OBJECT

    struct Shown {
        QString track, device;
        QQuickItem* view = nullptr;
        ReverbFilterPad* filter = nullptr;
        ReverbSpinPad* spin = nullptr;
        ReverbDecayGraph* decay = nullptr;
    };

    // A track playing a 1 kHz tone at 0.5 (-6.02 dBFS) through a Reverb, its editor shown.
    Shown reverb(double seconds = 1.0) {
        Shown s;
        s.track = audioTrackWith(tone(1000.0, int(seconds * kSampleRate)), QStringLiteral("tone"), seconds);
        if (s.track.isEmpty())
            return s;
        s.device = editor()->addDevice(s.track, QStringLiteral("reverb"));
        s.view = show(QStringLiteral("reverb"), s.track, s.device);
        if (s.view) {
            s.filter = find<ReverbFilterPad>(s.view, QStringLiteral("filterPad"));
            s.spin = find<ReverbSpinPad>(s.view, QStringLiteral("spinPad"));
            s.decay = find<ReverbDecayGraph>(s.view, QStringLiteral("decayGraph"));
        }
        return s;
    }

    double value(const Shown& s, const char* id) { return param(s.track, s.device, QString::fromLatin1(id)); }
    void set(const Shown& s, const char* id, double v) {
        editor()->setDeviceParam(s.track, s.device, QString::fromLatin1(id), v);
    }

    // The control named `name` (exactly).
    QQuickItem* control(QQuickItem* view, const char* name) { return find(view, QString::fromLatin1(name)); }
    // An EditorKnob's dial.
    KnobItem* knob(QQuickItem* view, const char* name) {
        QQuickItem* cell = control(view, name);
        auto* paramKnob = cell ? qvariant_cast<QQuickItem*>(cell->property("knob")) : nullptr;
        return paramKnob ? qvariant_cast<KnobItem*>(paramKnob->property("knob")) : nullptr;
    }
    // A ParamBox's box.
    ValueBoxItem* box(QQuickItem* view, const char* name) {
        QQuickItem* item = control(view, name);
        return item ? qvariant_cast<ValueBoxItem*>(item->property("box")) : nullptr;
    }
    static sub::ui::DeviceParam* paramOf(QQuickItem* item) {
        return item ? qvariant_cast<sub::ui::DeviceParam*>(item->property("param")) : nullptr;
    }
    bool lit(QQuickItem* view, const char* name) {
        QQuickItem* item = control(view, name);
        return item && item->property("lit").toBool();
    }
    void click(QQuickItem* view, const char* name) {
        QQuickItem* item = control(view, name);
        QVERIFY(item);
        QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier,
                          centerOf(qvariant_cast<QQuickItem*>(item->property("button"))));
    }
    // The control as the user sees it: a knob's dial, a box's value, a switch lit, a list's choice.
    double shownValue(QQuickItem* view, const char* name) {
        if (KnobItem* k = QByteArray(name).endsWith("Knob") ? knob(view, name) : nullptr)
            return k->value();
        if (ValueBoxItem* b = QByteArray(name).endsWith("Box") ? box(view, name) : nullptr)
            return b->value();
        if (QByteArray(name).endsWith("Button"))
            return lit(view, name) ? 1.0 : 0.0;
        QQuickItem* item = control(view, name);
        return item ? item->property("index").toDouble() : -999.0;
    }
    // Where a control is in the editor.
    static QRectF rectIn(QQuickItem* view, QQuickItem* item) {
        return item ? item->mapRectToItem(view, QRectF(0, 0, item->width(), item->height())) : QRectF();
    }
    // The engine's value of the device's parameter.
    double engineParam(const Shown& s, const char* id) {
        const auto pid = bridge()->engineDeviceId(s.track, s.device);
        return pid ? double(engine()->processorParam(*pid, engine()->processorParamIndex(*pid, id))) : -999.0;
    }

    // The mouse at exact positions in an item (QTest's are whole pixels in the window).
    void mouse(QEvent::Type type, QQuickItem* item, QPointF at, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        const QPointF scene = item->mapToScene(at);
        const Qt::MouseButton button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
        const Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
        QMouseEvent event(type, scene, scene, window_->mapToGlobal(scene), button, buttons, modifiers);
        QGuiApplication::sendEvent(window_, &event);
    }
    // A drag from `from` to `to` in `steps` moves, then let go.
    void drag(QQuickItem* item, QPointF from, QPointF to, int steps = 3,
              Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        mouse(QEvent::MouseButtonPress, item, from, modifiers);
        for (int i = 1; i <= steps; ++i)
            mouse(QEvent::MouseMove, item, from + (to - from) * (double(i) / steps), modifiers);
        mouse(QEvent::MouseButtonRelease, item, to, modifiers);
    }
    // The hovering mouse (no button).
    void hover(QQuickItem* item, QPointF at) {
        const QPointF scene = item->mapToScene(at);
        QMouseEvent event(QEvent::MouseMove, scene, scene, window_->mapToGlobal(scene), Qt::NoButton, Qt::NoButton,
                          Qt::NoModifier);
        QGuiApplication::sendEvent(window_, &event);
    }

    // Display refreshes `ms` apart, `count` of them.
    void refreshes(int count, int ms = 20) {
        for (int i = 0; i < count; ++i) {
            QTest::qWait(ms);
            refreshDisplays();
        }
    }
    // Plays the track from the start for `seconds` (offline), then reads the displays.
    void play(double seconds = 0.5) {
        engine()->renderOffline(0.0, int64_t(seconds * kSampleRate));
        refreshDisplays();
    }

    // Whether the part of `image` over `item` holds more than one colour.
    QImage partOf(const QImage& image, QQuickItem* item) {
        const QRectF scene = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
        const qreal dpr = image.devicePixelRatio();
        return image.copy(QRectF(scene.topLeft() * dpr, scene.size() * dpr).toRect());
    }
    static bool varied(const QImage& image) {
        const QRgb first = image.pixel(0, 0);
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (image.pixel(x, y) != first)
                    return true;
        return false;
    }

    // The controls within the body's margins, none overlapping another.
    void checkLaidOut(QQuickItem* view) {
        QList<std::pair<QString, QRectF>> rects;
        for (const auto& [name, id] : kControls)
            rects.append({QString::fromLatin1(name), rectIn(view, control(view, name))});
        for (const QString& name : kCanvases) rects.append({name, rectIn(view, find(view, name))});
        for (const auto& [name, r] : rects) {
            QVERIFY2(!r.isEmpty(), qPrintable(name));
            QVERIFY2(r.left() >= 8 - 1e-6 && r.right() <= view->width() - 8 + 1e-6 && r.top() >= 6 - 1e-6 &&
                         r.bottom() <= view->height() - 6 + 1e-6,
                     qPrintable(name + QStringLiteral(" at ") + QString::number(r.x()) + u',' +
                                QString::number(r.y()) + u' ' + QString::number(r.width()) + u'x' +
                                QString::number(r.height())));
        }
        // (Chorus's switch is its Amount knob's title: it lies over the knob's empty caption, clear of its dial.)
        const auto title = [](const QString& a, const QString& b) {
            return (a == u"chorusButton" && b == u"chorusAmountKnob") ||
                   (b == u"chorusButton" && a == u"chorusAmountKnob");
        };
        for (int i = 0; i < rects.size(); ++i)
            for (int j = i + 1; j < rects.size(); ++j)
                QVERIFY2(title(rects[i].first, rects[j].first) || !rects[i].second.intersects(rects[j].second),
                         qPrintable(rects[i].first + QStringLiteral(" over ") + rects[j].first));
        const QRectF chorus = rectIn(view, control(view, "chorusButton"));
        const QRectF dial = rectIn(view, knob(view, "chorusAmountKnob"));
        QVERIFY2(chorus.top() == rectIn(view, control(view, "chorusAmountKnob")).top() &&
                     chorus.bottom() < dial.top() && std::abs(chorus.center().x() - dial.center().x()) < 0.5,
                 qPrintable(QString::number(chorus.bottom()) + u' ' + QString::number(dial.top())));
        // Each row's dials in line, and so their readouts (Chorus Amount's under its switch too).
        for (const QList<const char*>& row : {kFirstRow, kSecondRow}) {
            const double top = rectIn(view, knob(view, row[0])).top();
            for (const char* name : row)
                QVERIFY2(rectIn(view, knob(view, name)).top() == top,
                         qPrintable(QString::fromLatin1(name) + u' ' +
                                    QString::number(rectIn(view, knob(view, name)).top()) + u' ' +
                                    QString::number(top)));
        }
    }

    // Every box as wide as the widest text its parameter takes (over its whole range, whatever the font's
    // figures) with the automation dot clear of it (every value's text, centred on a whole pixel as the box draws
    // it, and its ink too, from kTextFrom px in: a pixel clear of the dot), and no wider (its widest text with every
    // figure the font's widest no further in than that); every list as wide as its longest name with the arrow, and
    // its caption (or a knob's column), and no wider; every switch as wide as its text (and icon); the knobs the
    // house's 34 px.
    void checkWidths(QQuickItem* view) {
        const double cell = control(view, "shapeKnob")->width();
        for (const auto& [name, id] : kControls) {
            QQuickItem* item = control(view, name);
            const QByteArray kind(name);
            if (kind.endsWith("Box")) {
                ValueBoxItem* b = box(view, name);
                sub::ui::DeviceParam* p = paramOf(item);
                const QFontMetricsF metrics(b->font());  // (as ValueBoxItem centres its text: by its advance)
                double nearest = item->width(), widened = 0.0;
                QString text;
                for (const double v : valuesOf(p)) {
                    const QString shown = p->format(v);
                    const double left = std::round((item->width() - metrics.horizontalAdvance(shown)) / 2) +
                                        std::min(0.0, metrics.boundingRect(shown).left());
                    if (left < nearest) {
                        nearest = left;
                        text = shown;
                    }
                    widened = std::max(widened, metrics.horizontalAdvance(::widened(p->format(v), metrics)));
                }
                QVERIFY2(nearest >= kTextFrom - 1e-6,
                         qPrintable(QStringLiteral("%1 %2 px: \"%3\" %4 px in")
                                        .arg(QString::fromLatin1(name))
                                        .arg(item->width())
                                        .arg(text)
                                        .arg(nearest)));
                QVERIFY2((item->width() - widened) / 2 <= kTextFrom + 1e-6,
                         qPrintable(QStringLiteral("%1 %2 px for %3").arg(QString::fromLatin1(name)).arg(item->width())
                                        .arg(widened)));
            } else if (kind.endsWith("Choice")) {
                auto* button = qvariant_cast<QQuickItem*>(item->property("button"));
                const QFontMetricsF metrics(qvariant_cast<QFont>(button->property("font")));
                double longest = 0.0;
                for (const QString& label : paramOf(item)->labels())
                    longest = std::max(longest, metrics.horizontalAdvance(label));
                const double padding =
                    button->property("leftPadding").toDouble() + button->property("rightPadding").toDouble();
                QVERIFY2(item->width() >= longest + padding - 1e-6,
                         qPrintable(QString::fromLatin1(name) + u' ' + QString::number(item->width()) + u' ' +
                                    QString::number(longest + padding)));
                // Density's and Smooth's: under their captions, whole, as wide as the list or the caption needs,
                // or a knob's column.
                if (name != QByteArrayLiteral("hiTypeChoice")) {
                    QQuickItem* caption = nullptr;
                    for (QQuickItem* child : item->parentItem()->childItems())
                        if (child->isVisible() && child->property("truncated").isValid())
                            caption = child;
                    QVERIFY2(caption && caption->implicitWidth() <= item->width() + 1e-6, name);
                    QVERIFY2(item->width() <= std::max({cell, std::ceil(longest) + padding,
                                                         std::ceil(caption->implicitWidth())}) + 1e-6,
                             name);
                }
            } else if (kind.endsWith("Button")) {
                QVERIFY2(item->width() >= item->implicitWidth() - 1e-6,
                         qPrintable(QString::fromLatin1(name) + u' ' + QString::number(item->width()) + u' ' +
                                    QString::number(item->implicitWidth())));
            } else if (kind.endsWith("Knob")) {
                QCOMPARE(item->property("size").toDouble(), 34.0);
            }
        }
    }

private Q_SLOTS:
    void initTestCase() {
        if (!haveDisplay())
            QSKIP("needs a display: the offscreen platform renders Qt Quick in software, without this geometry");
        startHost();
    }

    void cleanupTestCase() { stopHost(); }

    void init() {
        clearHost();
        // The real pointer off the window: a box's or a knob's drag puts it back where the drag began
        // (DragCursor), where it would hover whatever the next editor has there (and show its tooltip).
        QCursor::setPos(window_->screen(), QPoint(0, 0));
    }

    // --- Laid out --------------------------------------------------------------------------------

    void layout() {
        QVariant url;
        QMetaObject::invokeMethod(root_.get(), "editorFor", Q_RETURN_ARG(QVariant, url),
                                  Q_ARG(QVariant, QStringLiteral("reverb")));
        QVERIFY(url.toString().endsWith(QStringLiteral("ReverbEditor.qml")));
        Shown s = reverb();
        QVERIFY(s.view && s.filter && s.spin && s.decay);
        QVERIFY2(s.view->implicitHeight() <= bodyHeight(), qPrintable(QString::number(s.view->implicitHeight())));
        QCOMPARE(s.view->height(), double(bodyHeight()));
        for (const auto& [name, id] : kControls) QVERIFY2(control(s.view, name), name);
        // As wide as its sections, 8 px in from either side (Dry/Wet the last).
        QCOMPARE(s.view->implicitWidth(), rectIn(s.view, control(s.view, "mixKnob")).right() + 8.0);
        // The pads and the graph grow into the body: 26 px from its top, 28 from its bottom (their boxes), and
        // span the boxes under them: as wide as the boxes side by side, or as the switches over them or their own
        // captions need (their implicit widths), and no wider.
        const auto widthOf = [&](const char* name) { return control(s.view, name)->width(); };
        const auto needOf = [&](const char* name) { return std::ceil(control(s.view, name)->implicitWidth()); };
        auto* hiType = control(s.view, "hiTypeChoice");
        auto* hiTypeButton = qvariant_cast<QQuickItem*>(hiType->property("button"));
        double hiTypeNeed = 0.0;
        for (const QString& label : paramOf(hiType)->labels())
            hiTypeNeed = std::max(hiTypeNeed, QFontMetricsF(qvariant_cast<QFont>(hiTypeButton->property("font")))
                                                  .horizontalAdvance(label));
        hiTypeNeed = std::ceil(hiTypeNeed) + hiTypeButton->property("leftPadding").toDouble() +
                     hiTypeButton->property("rightPadding").toDouble();
        const QList<std::tuple<QQuickItem*, const char*, const char*, double, double>> spans = {
            {s.filter, "inFreqBox", "inWidthBox", widthOf("inFreqBox") + 4 + widthOf("inWidthBox"),
             2 * std::max(needOf("loCutButton"), needOf("hiCutButton")) + 4},
            {s.spin, "spinAmountBox", "spinRateBox", widthOf("spinAmountBox") + 4 + widthOf("spinRateBox"),
             needOf("spinButton")},
            {s.decay, "loFreqBox", "hiGainBox",
             widthOf("loFreqBox") + 4 + widthOf("loGainBox") + 8 + widthOf("hiFreqBox") + 4 + widthOf("hiGainBox"),
             widthOf("loShelfButton") + 4 + widthOf("hiFilterButton") + 4 + hiTypeNeed}};
        for (const auto& [canvas, first, last, boxes, switches] : spans) {
            QVERIFY2(std::abs(canvas->height() - (s.view->height() - 54)) <= 1.0, qPrintable(canvas->objectName()));
            const QRectF r = rectIn(s.view, canvas);
            QCOMPARE(r.left(), rectIn(s.view, control(s.view, first)).left());
            QCOMPARE(r.right(), rectIn(s.view, control(s.view, last)).right());
            QVERIFY(canvas->width() >= canvas->implicitWidth());
            QVERIFY2(canvas->width() >= boxes - 1e-6 && canvas->width() >= switches - 1e-6 &&
                         canvas->width() <= std::max({boxes, switches, canvas->implicitWidth()}) + 1e-6,
                     qPrintable(QStringLiteral("%1 %2: boxes %3, switches %4, implicitly %5")
                                    .arg(canvas->objectName())
                                    .arg(canvas->width())
                                    .arg(boxes)
                                    .arg(switches)
                                    .arg(canvas->implicitWidth())));
        }
        checkWidths(s.view);
        checkLaidOut(s.view);
        if (QTest::currentTestFailed())
            return;

        // At its own least height too.
        const int least = int(std::ceil(s.view->implicitHeight()));
        QQuickItem* view = show(QStringLiteral("reverb"), s.track, s.device, least);
        QVERIFY(view);
        QCOMPARE(view->height(), double(least));
        checkLaidOut(view);
        for (const QString& name : kCanvases) {
            QQuickItem* canvas = find(view, name);
            QVERIFY(canvas->height() >= canvas->implicitHeight());
        }
    }

    // Every caption and readout whole: each knob's at the value whose text is widest over its parameter's range,
    // the knobs' columns no wider than the widest of those (every figure the font's widest) needs, or the house's
    // 52 px (Chorus's, its switch's, on an even width); the pads' and the graph's captions at their widest (the
    // tail's onset at the parameters' ends, each readout at its values' widest), apart and inside, as wide as
    // their implicit widths make room for.
    void textsWhole() {
        Shown s = reverb();
        QVERIFY(s.view && s.spin && s.decay);
        const int before = undo()->index();
        const auto idOf = [](const char* name) {
            for (const auto& [control, id] : kControls)
                if (QByteArray(control) == name)
                    return id;
            return "";
        };
        double need = 0.0;
        for (const QList<const char*>& row : {kFirstRow, kSecondRow}) {
            for (const char* name : row) {
                QQuickItem* cell = control(s.view, name);
                QList<QQuickItem*> texts;  // its caption and its readout
                for (QQuickItem* child : cell->childItems())
                    if (child->property("truncated").isValid())
                        texts.append(child);
                QCOMPARE(texts.size(), 2);
                const QFontMetricsF metrics(qvariant_cast<QFont>(texts[1]->property("font")));
                sub::ui::DeviceParam* p = paramOf(cell);
                set(s, idOf(name), widestValue(p, metrics));
                for (QQuickItem* text : texts)
                    QVERIFY2(!text->property("truncated").toBool() && text->implicitWidth() <= cell->width() + 1e-6,
                             qPrintable(QStringLiteral("%1: \"%2\" in %3 px")
                                            .arg(QString::fromLatin1(name), text->property("text").toString())
                                            .arg(cell->width())));
                need = std::max(need, extentOf(texts[0]->property("text").toString(), metrics));
                for (const double v : valuesOf(p))
                    need = std::max(need, extentOf(widened(p->format(v), metrics), metrics));
            }
        }
        const double cell = std::max(52.0, std::ceil(need));
        for (const QList<const char*>& row : {kFirstRow, kSecondRow}) {
            for (const char* name : row) {
                const double width = control(s.view, name)->width();
                const bool chorus = QByteArray(name).startsWith("chorus");
                const double expected =
                    chorus ? 2 * std::ceil(std::max(cell, control(s.view, "chorusButton")->implicitWidth()) / 2) : cell;
                QVERIFY2(width >= 52.0 && width <= expected + 1e-6,
                         qPrintable(QStringLiteral("%1 %2, the texts need %3").arg(QString::fromLatin1(name))
                                        .arg(width).arg(need)));
            }
        }
        while (undo()->index() > before) undo()->undo();

        // The tail's onset at its latest (the largest room, the latest Shape and Predelay, each Density).
        ReverbSpinPad* pad = s.spin;
        const QFontMetricsF captions(ReverbSpinPad::captionFont());
        set(s, "size", paramOf(control(s.view, "sizeKnob"))->maximum());
        set(s, "shape", 100.0);
        set(s, "predelay", paramOf(control(s.view, "predelayKnob"))->maximum());
        for (int density = 0; density <= 3; ++density) {
            set(s, "density", density);
            const QString text = pad->onsetText();
            const QRectF early = pad->earlyRect(), onset = pad->onsetRect();
            const double width = std::ceil(extentOf(text, captions));
            QVERIFY2(onset.width() >= width - 1e-6 && onset.left() >= early.right() + ReverbSpinPad::kCaptionGap - 1e-6
                         && early.left() >= pad->plot().left() && onset.right() <= pad->plot().right() &&
                         early.width() >= extentOf(QStringLiteral("Early"), captions),
                     qPrintable(QStringLiteral("%1 in %2..%3").arg(text).arg(onset.left()).arg(onset.right())));
            QVERIFY2(pad->implicitWidth() >= 2 * (1 + ReverbSpinPad::kCaptionInset) + early.width() +
                                                 ReverbSpinPad::kCaptionGap + width - 1e-6,
                     qPrintable(QStringLiteral("%1: %2").arg(text).arg(pad->implicitWidth())));
        }
        while (undo()->index() > before) undo()->undo();

        // The decay graph's readouts at their widest: each shelf's (its frequency and gain at their widest
        // texts), the Low-pass's, the decay time's, Frozen.
        ReverbDecayGraph* graph = s.decay;
        const QFontMetricsF readouts(ReverbDecayGraph::captionFont());
        const auto checkReadout = [&]() {
            const QString text = graph->readout();
            const QRectF caption = graph->captionRect(), readout = graph->readoutRect();
            const double width = std::ceil(extentOf(text, readouts));
            QVERIFY2(readout.width() >= width - 1e-6 &&
                         readout.left() >= caption.right() + ReverbDecayGraph::kCaptionGap - 1e-6 &&
                         caption.left() >= graph->plot().left() && readout.right() <= graph->plot().right() &&
                         caption.width() >= extentOf(QStringLiteral("Decay time"), readouts),
                     qPrintable(QStringLiteral("%1 in %2..%3").arg(text).arg(readout.left()).arg(readout.right())));
            QVERIFY2(graph->implicitWidth() >= 2 + 2 * ReverbDecayGraph::kCaptionInset + caption.width() +
                                                   ReverbDecayGraph::kCaptionGap + width +
                                                   ReverbDecayGraph::kMeterWidth + 2 - 1e-6,
                     qPrintable(QStringLiteral("%1: %2").arg(text).arg(graph->implicitWidth())));
        };
        const auto setWidest = [&](const char* name) {
            set(s, idOf(name), widestValue(paramOf(control(s.view, name)), readouts));
        };
        for (const auto& [handle, freq, gain] : {std::tuple{ReverbDecayGraph::Lo, "loFreqBox", "loGainBox"},
                                                 std::tuple{ReverbDecayGraph::Hi, "hiFreqBox", "hiGainBox"}}) {
            const int shelf = undo()->index();
            setWidest(freq);
            setWidest(gain);
            refreshes(2);
            hover(graph, graph->handleAt(handle));
            refreshes(2);
            QCOMPARE(graph->hovered(), handle);
            checkReadout();
            hover(s.view, QPointF(1, 1));
            while (undo()->index() > shelf) undo()->undo();
        }
        setWidest("hiFreqBox");
        setWidest("decayKnob");
        set(s, "hi_type", 1.0);  // Low-pass
        refreshes(2);
        hover(graph, graph->hiHandle());
        refreshes(2);
        QVERIFY(graph->readout().startsWith(QStringLiteral("Low-pass")));
        checkReadout();
        hover(s.view, QPointF(1, 1));
        refreshes(2);
        QCOMPARE(graph->hovered(), ReverbDecayGraph::None);
        checkReadout();  // (the decay time)
        set(s, "freeze", 1.0);
        QCOMPARE(graph->readout(), QStringLiteral("Frozen"));
        checkReadout();
        while (undo()->index() > before) undo()->undo();
    }

    // --- Bound, undoable ------------------------------------------------------------------------

    void controls() {
        Shown s = reverb();
        QVERIFY(s.view);
        // Every control is bound to its parameter, with a tooltip.
        for (const auto& [name, id] : kControls) {
            sub::ui::DeviceParam* p = paramOf(control(s.view, name));
            QVERIFY2(p && p->valid(), name);
            QCOMPARE(p->paramId(), QString::fromLatin1(id));
            QVERIFY2(!control(s.view, name)->property("tooltip").toString().isEmpty(), name);
        }
        // Each shows its default.
        const QList<std::pair<const char*, double>> defaults = {
            {"loCutButton", 1},   {"hiCutButton", 1},     {"inFreqBox", 830},    {"inWidthBox", 7.5},
            {"spinButton", 1},    {"spinAmountBox", 25},  {"spinRateBox", 0.3},  {"shapeKnob", 50},
            {"predelayKnob", 2.5}, {"sizeKnob", 100},     {"stereoKnob", 100},   {"densityChoice", 3},
            {"smoothChoice", 1},  {"loShelfButton", 1},   {"hiFilterButton", 1}, {"hiTypeChoice", 0},
            {"loFreqBox", 90},    {"loGainBox", 75},      {"hiFreqBox", 4500},   {"hiGainBox", 70},
            {"decayKnob", 1200},  {"freezeButton", 0},    {"flatButton", 1},     {"cutButton", 1},
            {"diffusionKnob", 70}, {"scaleKnob", 50},     {"chorusButton", 1},   {"chorusAmountKnob", 20},
            {"chorusRateKnob", 0.8}, {"reflectKnob", 0},  {"diffuseKnob", 0},    {"mixKnob", 40}};
        QCOMPARE(defaults.size(), kControls.size());
        for (const auto& [name, v] : defaults)
            QVERIFY2(std::abs(shownValue(s.view, name) - v) < 1e-4 * std::max(1.0, std::abs(v)),
                     qPrintable(QString::fromLatin1(name) + u' ' + QString::number(shownValue(s.view, name))));
        QVERIFY(knob(s.view, "sizeKnob")->logScale() && knob(s.view, "decayKnob")->logScale());
        QVERIFY(box(s.view, "inFreqBox")->logScale() && box(s.view, "hiFreqBox")->logScale());
        // Size reads as Live's, a bare number.
        QCOMPARE(paramOf(control(s.view, "sizeKnob"))->text(), QStringLiteral("100.00"));
        QCOMPARE(formatValue(0.22, QStringLiteral("size")), QStringLiteral("0.22"));
        QCOMPARE(formatValue(500.0, QStringLiteral("size")), QStringLiteral("500.00"));
        // Stereo in whole degrees.
        QCOMPARE(paramOf(control(s.view, "stereoKnob"))->text(), QStringLiteral("100°"));
        QCOMPARE(formatValue(119.6, QStringLiteral("°")), QStringLiteral("120°"));
        QCOMPARE(box(s.view, "inFreqBox")->text(), QStringLiteral("830 Hz"));
        QCOMPARE(box(s.view, "hiFreqBox")->text(), QStringLiteral("4.50 kHz"));

        // A change of each shows on its control, and undo restores it.
        const QList<std::pair<const char*, double>> changes = {
            {"loCutButton", 0},   {"hiCutButton", 0},     {"inFreqBox", 2000},   {"inWidthBox", 3},
            {"spinButton", 0},    {"spinAmountBox", 60},  {"spinRateBox", 1},    {"shapeKnob", 80},
            {"predelayKnob", 40}, {"sizeKnob", 250},      {"stereoKnob", 50},    {"densityChoice", 1},
            {"smoothChoice", 2},  {"loShelfButton", 0},   {"hiFilterButton", 0}, {"hiTypeChoice", 1},
            {"loFreqBox", 300},   {"loGainBox", 40},      {"hiFreqBox", 2000},   {"hiGainBox", 30},
            {"decayKnob", 5000},  {"freezeButton", 1},    {"flatButton", 0},     {"cutButton", 0},
            {"diffusionKnob", 20}, {"scaleKnob", 90},     {"chorusButton", 0},   {"chorusAmountKnob", 70},
            {"chorusRateKnob", 3}, {"reflectKnob", -6},   {"diffuseKnob", 3},    {"mixKnob", 100}};
        for (int i = 0; i < changes.size(); ++i) {
            const auto& [name, v] = changes[i];
            const char* id = kControls[i].second;
            QCOMPARE(QByteArray(kControls[i].first), QByteArray(name));
            const double before = shownValue(s.view, name);
            set(s, id, v);
            QVERIFY2(std::abs(shownValue(s.view, name) - v) < 1e-4 * std::max(1.0, std::abs(v)), name);
            undo()->undo();
            QVERIFY2(shownValue(s.view, name) == before, name);
        }

        // Freeze, clicked: one undo step; the graph knows.
        int steps = undo()->index();
        click(s.view, "freezeButton");
        QCOMPARE(value(s, "freeze"), 1.0);
        QVERIFY(lit(s.view, "freezeButton"));
        QVERIFY(s.decay->frozen());
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();
        QVERIFY(!s.decay->frozen());

        // Density from its list.
        steps = undo()->index();
        QMetaObject::invokeMethod(control(s.view, "densityChoice"), "choose", Q_ARG(QVariant, 0));
        QCOMPARE(value(s, "density"), 0.0);
        QCOMPARE(control(s.view, "densityChoice")->property("index").toInt(), 0);
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();

        // A knob dragged up: one undo step.
        steps = undo()->index();
        const QPoint decayAt = centerOf(qvariant_cast<QQuickItem*>(control(s.view, "decayKnob")->property("knob")));
        QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, decayAt);
        for (int dy = 10; dy <= 40; dy += 10) dragTo(decayAt - QPoint(0, dy));
        QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, decayAt - QPoint(0, 40));
        QVERIFY2(value(s, "decay") > 1500.0, qPrintable(QString::number(value(s, "decay"))));
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(engineParam(s, "decay"), double(float(value(s, "decay"))));
        undo()->undo();
        QCOMPARE(value(s, "decay"), 1200.0);

        // A frequency box dragged up moves it in log: by the same ratio from 4.5 kHz and from 450 Hz.
        int boxSteps = 0;
        const auto dragBoxUp = [&](double from) {
            set(s, "hi_freq", from);
            const int before = undo()->index();
            const QPoint at = centerOf(control(s.view, "hiFreqBox"));
            QTest::mousePress(window_, Qt::LeftButton, Qt::NoModifier, at);
            for (int dy = 5; dy <= 20; dy += 5) dragTo(at - QPoint(0, dy));
            QTest::mouseRelease(window_, Qt::LeftButton, Qt::NoModifier, at - QPoint(0, 20));
            boxSteps += undo()->index() - before;
            return value(s, "hi_freq") / from;
        };
        const double high = dragBoxUp(4500.0), low = dragBoxUp(450.0);
        QCOMPARE(boxSteps, 2);  // one undo step a drag
        QVERIFY2(high > 1.05, qPrintable(QString::number(high)));
        QVERIFY2(std::abs(high / low - 1.0) < 0.02, qPrintable(QString::number(high) + u' ' + QString::number(low)));
        set(s, "hi_freq", 4500.0);

        // Switches dim what they leave unused (still editable). (The fades run on the render loop's frames:
        // waited for, not assumed done after a while.)
        const auto opacityOf = [&](const char* name) { return control(s.view, name)->opacity(); };
        QCOMPARE(opacityOf("hiTypeChoice"), 1.0);
        set(s, "hi_filter", 0.0);
        QTRY_VERIFY(opacityOf("hiTypeChoice") < 1.0 && opacityOf("hiFreqBox") < 1.0 && opacityOf("hiGainBox") < 1.0);
        QTRY_COMPARE(opacityOf("hiFreqBox"), 0.55);  // (the house's dimmed but settable)
        QVERIFY(control(s.view, "hiFreqBox")->isEnabled());
        set(s, "hi_filter", 1.0);
        set(s, "hi_type", 1.0);
        QTRY_COMPARE(opacityOf("hiFreqBox"), 1.0);
        QTRY_COMPARE(opacityOf("hiTypeChoice"), 1.0);
        QVERIFY(opacityOf("hiGainBox") < 1.0);
        QCOMPARE(control(s.view, "hiTypeChoice")->property("index").toInt(), 1);
        // Flat and Cut act only frozen; the chorus's knobs only with Chorus; the input's boxes only with a cut.
        QVERIFY(opacityOf("flatButton") < 1.0 && opacityOf("cutButton") < 1.0);
        set(s, "freeze", 1.0);
        set(s, "chorus", 0.0);
        set(s, "lo_cut", 0.0);
        set(s, "hi_cut", 0.0);
        set(s, "spin", 0.0);
        QTRY_COMPARE(opacityOf("flatButton"), 1.0);
        QTRY_VERIFY(opacityOf("chorusAmountKnob") < 1.0 && opacityOf("chorusRateKnob") < 1.0);
        QTRY_VERIFY(opacityOf("inFreqBox") < 1.0 && opacityOf("inWidthBox") < 1.0);
        QTRY_VERIFY(opacityOf("spinAmountBox") < 1.0 && opacityOf("spinRateBox") < 1.0);
    }

    // --- Under the mouse --------------------------------------------------------------------------

    void hovering() {
        Shown s = reverb();
        QVERIFY(s.view);
        // The tooltip shown (Qt Quick Controls' one, shared by every control): its text, or none.
        const auto tipShown = [&]() -> QString {
            auto* tip = qmlEngine(s.view)->property("_q_QQuickToolTip").value<QObject*>();
            return tip && tip->property("visible").toBool() ? tip->property("text").toString() : QString();
        };
        const auto buttonOf = [&](const char* name) {
            return qvariant_cast<QQuickItem*>(control(s.view, name)->property("button"));
        };
        // Every switch lights under the mouse: no other control lies over it.
        for (const auto& [name, id] : kControls) {
            if (!QByteArray(name).endsWith("Button"))
                continue;
            QTest::mouseMove(window_, QPoint(1, 1));
            QTRY_VERIFY2(!buttonOf(name)->property("hovered").toBool(), name);
            QTest::mouseMove(window_, centerOf(buttonOf(name)));
            QTRY_VERIFY2(buttonOf(name)->property("hovered").toBool(), name);
        }
        // Chorus's switch lies over its Amount knob's (empty) caption: it, not the knob, is under the mouse
        // there, and shows its own tooltip; the knob's dial below, the knob's. Up from the dial onto the
        // switch, and down again.
        const QString chorusTip = buttonOf("chorusButton")->property("tooltip").toString();
        const QString amountTip = control(s.view, "chorusAmountKnob")->property("tooltip").toString();
        QVERIFY(!chorusTip.isEmpty() && !amountTip.isEmpty() && chorusTip != amountTip);
        QTest::mouseMove(window_, QPoint(1, 1));
        QTRY_COMPARE(tipShown(), QString());
        QTest::mouseMove(window_, centerOf(buttonOf("chorusButton")));
        QTRY_COMPARE(tipShown(), chorusTip);
        QTest::mouseMove(window_, centerOf(knob(s.view, "chorusAmountKnob")));
        QTRY_VERIFY(!buttonOf("chorusButton")->property("hovered").toBool());
        QTRY_COMPARE(tipShown(), amountTip);
        QTest::mouseMove(window_, centerOf(buttonOf("chorusButton")));
        QTRY_VERIFY(buttonOf("chorusButton")->property("hovered").toBool());
        QTRY_COMPARE(tipShown(), chorusTip);
        QTest::mouseMove(window_, centerOf(knob(s.view, "chorusAmountKnob")));
        QTRY_COMPARE(tipShown(), amountTip);
        QTest::mouseMove(window_, QPoint(1, 1));
        QTRY_COMPARE(tipShown(), QString());
    }

    // --- The input filter's pad -------------------------------------------------------------------

    void filterPad() {
        Shown s = reverb();
        QVERIFY(s.filter);
        ReverbFilterPad* pad = s.filter;
        const double rate = bridge()->sampleRate();
        // The curve is the engine's filter.
        const std::vector<double>& frequencies = pad->curveFrequencies();
        const std::vector<double>& db = pad->curveDb();
        QVERIFY(frequencies.size() >= 100 && db.size() == frequencies.size());
        for (std::size_t i = 0; i < frequencies.size(); i += 13)
            QCOMPARE(db[i], reverbInputFilterDb(830.0, 7.5, true, true, rate, {frequencies[i]})[0]);
        QVERIFY(std::abs(pad->dot().x() - pad->xOf(830.0)) < 1e-9);
        QVERIFY(std::abs(pad->dot().y() - pad->yOfWidth(7.5)) < 1e-9);
        // The dot (its 5 px ring) stays under the caption's strip at the widest band, and in the pad.
        QVERIFY(pad->yOfWidth(9.0) - 6.0 >= pad->plot().top() + 13.0);
        QVERIFY(pad->yOfWidth(0.5) + 6.0 <= pad->plot().bottom());

        // Dragged: across for the frequency, up and down for the width, one undo step.
        const int steps = undo()->index();
        drag(pad, QPointF(pad->xOf(400.0), pad->yOfWidth(3.0)), QPointF(pad->xOf(2000.0), pad->yOfWidth(6.0)));
        QVERIFY2(std::abs(value(s, "in_freq") / 2000.0 - 1.0) < 0.02, qPrintable(QString::number(value(s, "in_freq"))));
        QVERIFY2(std::abs(value(s, "in_width") - 6.0) < 0.06, qPrintable(QString::number(value(s, "in_width"))));
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(undo()->undoText(), QStringLiteral("Change Reverb Input Filter"));
        QCOMPARE(engineParam(s, "in_freq"), double(float(value(s, "in_freq"))));
        QCOMPARE(engineParam(s, "in_width"), double(float(value(s, "in_width"))));
        QCOMPARE(box(s.view, "inWidthBox")->value(), value(s, "in_width"));
        undo()->undo();
        QCOMPARE(value(s, "in_freq"), 830.0);
        QCOMPARE(value(s, "in_width"), 7.5);

        // Shift drags finely: a quarter as far.
        drag(pad, pad->dot(), pad->dot() + QPointF(0, -40), 3, Qt::ShiftModifier);
        const double fine = value(s, "in_width") - 7.5;
        undo()->undo();
        QVERIFY2(fine > 0.0 && std::abs(fine - (pad->widthAt(pad->yOfWidth(7.5) - 10) - 7.5)) < 0.02,
                 qPrintable(QString::number(fine)));

        // Lo Cut off: the lows pass (once it has eased), the high cut's slope is unchanged.
        const std::size_t at30 = indexOf(frequencies, 30.0), at16k = indexOf(frequencies, 16000.0);
        const double hiBefore = pad->curveDb()[at16k];
        set(s, "lo_cut", 0.0);
        refreshes(20);
        QVERIFY2(std::abs(pad->curveShown()[at30]) < 0.1, qPrintable(QString::number(pad->curveShown()[at30])));
        QVERIFY(std::abs(pad->curveDb()[at16k] - hiBefore) < 1e-9);
        QVERIFY(hiBefore < -3.0);
    }

    // --- Spin's pad -----------------------------------------------------------------------------

    void spinPad() {
        Shown s = reverb();
        QVERIFY(s.spin);
        ReverbSpinPad* pad = s.spin;
        QVERIFY(std::abs(pad->handle().x() - pad->xOfRate(0.3)) < 1e-4);  // (0.3 as a float)
        QVERIFY(std::abs(pad->handle().y() - pad->yOfAmount(25.0)) < 1e-4);
        // Its particles are the reflections at rest: across by their pans, down by their times (the
        // earliest at the top), all twelve at High.
        const QList<ReverbTap> taps = reverbEarlyTaps(100.0, 50.0, 3);
        for (int k = 0; k < 12; ++k) QVERIFY(pad->particleShown(k));
        QVERIFY(pad->particles()[0].y() < pad->particles()[11].y());
        QVERIFY((pad->particles()[3].x() > pad->width() / 2) == (taps[3].pan > 0));
        // Each, lit up and bobbing at Spin's most, keeps under the captions' strip ("Early", the tail's onset)
        // and in the pad, at every Shape (how loud each is): the first, the loudest and largest, too.
        for (const double shape : {0.0, 50.0, 100.0}) {
            set(s, "shape", shape);
            QVERIFY(pad->particleReach(0) > pad->particleReach(11));
            for (int k = 0; k < 12; ++k) {
                const double y = pad->particles()[k].y(), reach = pad->particleReach(k);
                QVERIFY2(reach > 0.0 && y - reach >= pad->plot().top() + 13.0 && y + reach <= pad->plot().bottom(),
                         qPrintable(QStringLiteral("Shape %1, %2: %3 +- %4").arg(shape).arg(k).arg(y).arg(reach)));
            }
        }
        set(s, "shape", 50.0);

        const int steps = undo()->index();
        drag(pad, QPointF(pad->xOfRate(0.1), pad->yOfAmount(10.0)), QPointF(pad->xOfRate(1.0), pad->yOfAmount(80.0)));
        QVERIFY2(std::abs(value(s, "spin_rate") - 1.0) < 0.03, qPrintable(QString::number(value(s, "spin_rate"))));
        QVERIFY2(std::abs(value(s, "spin_amount") - 80.0) < 1.0, qPrintable(QString::number(value(s, "spin_amount"))));
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(undo()->undoText(), QStringLiteral("Change Reverb Spin"));
        QVERIFY(std::abs(pad->handle().x() - pad->xOfRate(value(s, "spin_rate"))) < 1e-9);
        QVERIFY(std::abs(pad->handle().y() - pad->yOfAmount(value(s, "spin_amount"))) < 1e-9);
        QCOMPARE(engineParam(s, "spin_rate"), double(float(value(s, "spin_rate"))));
        QCOMPARE(engineParam(s, "spin_amount"), double(float(value(s, "spin_amount"))));
        QCOMPARE(box(s.view, "spinAmountBox")->value(), value(s, "spin_amount"));
        undo()->undo();

        // Shift drags finely: a quarter as far (from where Shift went down).
        drag(pad, pad->handle(), pad->handle() + QPointF(0, -40), 3, Qt::ShiftModifier);
        const double fine = value(s, "spin_amount") - 25.0;
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();
        QVERIFY2(std::abs(fine - (pad->amountAt(pad->yOfAmount(25.0) - 10.0) - 25.0)) < 0.11,
                 qPrintable(QString::number(fine)));
        // The handle (its 5 px ring) stays under the captions' strip at the most Spin, and in the pad.
        QVERIFY(pad->yOfAmount(100.0) - 6.0 >= pad->plot().top() + 13.0);
        QVERIFY(pad->xOfRate(0.07) - 6.0 >= pad->plot().left() && pad->xOfRate(1.3) + 6.0 <= pad->plot().right());

        // Sparse plays six of the twelve: the others are not drawn.
        set(s, "density", 0.0);
        for (int k = 0; k < 12; ++k) QCOMPARE(pad->particleShown(k), k % 2 == 0);
        undo()->undo();

        // Playing, the particles swing and trail their last positions. Any parameter's change (an automated
        // one's, every playhead move) leaves the trails be; one that moves the particles' homes starts them
        // again.
        set(s, "spin_amount", 100.0);
        set(s, "spin_rate", 1.3);
        const auto distinct = [&](int k) {
            const QList<QPointF> trail = pad->trail(k);
            int count = 1;
            for (int i = 1; i < trail.size(); ++i)
                if (QLineF(trail[i], trail[i - 1]).length() > 0.05)
                    ++count;
            return count;
        };
        play(0.3);
        refreshes(4, 12);
        QVERIFY2(distinct(5) >= 3, qPrintable(QString::number(distinct(5))));
        const QList<QPointF> trail = pad->trail(5);
        set(s, "mix", 60.0);
        QCOMPARE(pad->trail(5), trail);
        set(s, "stereo", 60.0);
        QCOMPARE(distinct(5), 1);
    }

    // --- The decay graph --------------------------------------------------------------------------

    void decayGraph() {
        Shown s = reverb();
        QVERIFY(s.decay);
        ReverbDecayGraph* graph = s.decay;
        const double rate = bridge()->sampleRate();
        // The curve is the engine's decay per frequency.
        const std::vector<double>& frequencies = graph->frequencies();
        QVERIFY(frequencies.size() >= 150 && graph->seconds().size() == frequencies.size());
        const ReverbDecaySettings defaults;
        for (std::size_t i = 0; i < frequencies.size(); i += 11)
            QCOMPARE(graph->seconds()[i], reverbDecaySeconds(defaults, rate, {frequencies[i]})[0]);
        // The handles: Decay at 1.2 s between the shelves, the shelves at their bands' times.
        QVERIFY(std::abs(graph->decayHandle().y() - graph->yOf(1.2)) < 1e-9);
        QVERIFY(std::abs(graph->loHandle().x() - graph->xOf(90.0)) < 1e-9);
        QVERIFY(std::abs(graph->loHandle().y() - graph->yOf(1.2 * 0.75)) < 1e-9);
        QVERIFY(std::abs(graph->hiHandle().x() - graph->xOf(4500.0)) < 1e-9);
        QVERIFY(std::abs(graph->hiHandle().y() - graph->yOf(1.2 * 0.7)) < 1e-9);
        QVERIFY(graph->decayHandle().x() > graph->loHandle().x() && graph->decayHandle().x() < graph->hiHandle().x());

        // Decay, dragged up: twice as long, one undo step.
        int steps = undo()->index();
        const QPointF decayAt = graph->decayHandle();
        drag(graph, decayAt, decayAt - QPointF(0, graph->yOf(1.2) - graph->yOf(2.4)));
        QVERIFY2(std::abs(value(s, "decay") / 2400.0 - 1.0) < 0.03, qPrintable(QString::number(value(s, "decay"))));
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(undo()->undoText(), QStringLiteral("Change Reverb Decay"));
        QCOMPARE(engineParam(s, "decay"), double(float(value(s, "decay"))));
        QCOMPARE(knob(s.view, "decayKnob")->value(), value(s, "decay"));
        undo()->undo();

        // Shift drags finely: the ratio to the power of a quarter.
        const double up = graph->yOf(1.2) - graph->yOf(2.4);
        drag(graph, graph->decayHandle(), graph->decayHandle() - QPointF(0, up), 3, Qt::ShiftModifier);
        QVERIFY2(std::abs(value(s, "decay") / (1200.0 * std::pow(2.0, 0.25)) - 1.0) < 0.03,
                 qPrintable(QString::number(value(s, "decay"))));
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();
        drag(graph, decayAt, decayAt - QPointF(0, up));

        // The high shelf's handle to 2 kHz and 0.4 of the decay: one undo step.
        steps = undo()->index();
        const double decay = value(s, "decay") / 1000.0;
        drag(graph, graph->hiHandle(), QPointF(graph->xOf(2000.0), graph->yOf(decay * 0.4)));
        QVERIFY2(std::abs(value(s, "hi_freq") / 2000.0 - 1.0) < 0.03, qPrintable(QString::number(value(s, "hi_freq"))));
        QVERIFY2(std::abs(value(s, "hi_gain") - 40.0) < 2.0, qPrintable(QString::number(value(s, "hi_gain"))));
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(engineParam(s, "hi_gain"), double(float(value(s, "hi_gain"))));
        QCOMPARE(box(s.view, "hiGainBox")->value(), value(s, "hi_gain"));
        undo()->undo();
        undo()->undo();

        // A shelf switched off has no gain the handle shows: dragged, only its frequency moves.
        set(s, "hi_filter", 0.0);
        steps = undo()->index();
        drag(graph, graph->hiHandle(), graph->hiHandle() + QPointF(20, 30));
        QVERIFY(value(s, "hi_freq") > 4500.0);
        QCOMPARE(value(s, "hi_gain"), 70.0);
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();
        undo()->undo();

        // At the ends of their ranges the handles stay inside the plot, ring and all.
        set(s, "lo_freq", 20.0);
        set(s, "lo_gain", 20.0);
        set(s, "decay", 200.0);
        set(s, "hi_freq", 16000.0);
        const QRectF inside = graph->plot().adjusted(6, 6, -6, -6);
        for (const QPointF& at : {graph->loHandle(), graph->hiHandle(), graph->decayHandle()})
            QVERIFY2(inside.contains(at), qPrintable(QString::number(at.x()) + u',' + QString::number(at.y())));
        for (int i = 0; i < 4; ++i) undo()->undo();

        // A low-pass has no gain to drag: its handle sits on the curve and moves across only.
        set(s, "hi_type", 1.0);
        QVERIFY(std::abs(graph->hiHandle().y() - graph->yOf(reverbDecaySeconds(
                    [] { ReverbDecaySettings lp; lp.hiLowpass = true; return lp; }(), rate, {4500.0})[0])) < 1e-6);
        steps = undo()->index();
        drag(graph, graph->hiHandle(), graph->hiHandle() + QPointF(0, -30));
        QCOMPARE(undo()->index(), steps);  // (nothing to change)
        drag(graph, graph->hiHandle(), graph->hiHandle() + QPointF(20, -30));
        QVERIFY(value(s, "hi_freq") > 4500.0);
        QCOMPARE(value(s, "hi_gain"), 70.0);
        QCOMPARE(undo()->index(), steps + 1);
        undo()->undo();
        undo()->undo();

        // A double-click on the low shelf's handle switches it off: one undo step; its handle dims to
        // the decay's line. Its second press, held and dragged, drags nothing. (By hand, as the platform
        // sends it: press, release, press at once, Qt making the double-click; then moves, held.)
        steps = undo()->index();
        const QPointF lo = graph->mapToScene(graph->loHandle()), global = window_->mapToGlobal(lo);
        int& time = QTest::lastMouseTimestamp;
        time += 1000;
        qt_handleMouseEvent(window_, lo, global, Qt::LeftButton, Qt::LeftButton, QEvent::MouseButtonPress,
                            Qt::NoModifier, time);
        qt_handleMouseEvent(window_, lo, global, Qt::NoButton, Qt::LeftButton, QEvent::MouseButtonRelease,
                            Qt::NoModifier, time);
        time += 20;
        qt_handleMouseEvent(window_, lo, global, Qt::LeftButton, Qt::LeftButton, QEvent::MouseButtonPress,
                            Qt::NoModifier, time);
        for (int i = 1; i <= 4; ++i) {
            const QPointF at = lo + QPointF(5.0 * i, -5.0 * i);
            qt_handleMouseEvent(window_, at, window_->mapToGlobal(at), Qt::LeftButton, Qt::NoButton,
                                QEvent::MouseMove, Qt::NoModifier, time += 10);
        }
        const QPointF end = lo + QPointF(20, -20);
        qt_handleMouseEvent(window_, end, window_->mapToGlobal(end), Qt::NoButton, Qt::LeftButton,
                            QEvent::MouseButtonRelease, Qt::NoModifier, time += 10);
        time += 1000;  // (no double-click with what comes next)
        QCoreApplication::processEvents();
        QCOMPARE(value(s, "lo_shelf"), 0.0);
        QCOMPARE(undo()->index(), steps + 1);
        QCOMPARE(undo()->undoText(), QStringLiteral("Switch Reverb Shelf"));
        QVERIFY(!lit(s.view, "loShelfButton"));
        QVERIFY(std::abs(graph->loHandle().y() - graph->yOf(1.2)) < 1e-9);
        QCOMPARE(value(s, "lo_freq"), 90.0);
        QCOMPARE(value(s, "lo_gain"), 75.0);
        undo()->undo();
        QCOMPARE(value(s, "lo_shelf"), 1.0);

        // The shelves' ranges overlap: the low one right of the high one still picks each by its handle.
        set(s, "lo_freq", 2000.0);
        set(s, "hi_freq", 500.0);
        drag(graph, graph->loHandle(), graph->loHandle() + QPointF(10, 0));
        QVERIFY(value(s, "lo_freq") > 2000.0 && value(s, "hi_freq") == 500.0);
        drag(graph, graph->hiHandle(), graph->hiHandle() + QPointF(-10, 0));
        QVERIFY(value(s, "hi_freq") < 500.0 && value(s, "lo_freq") > 2000.0);
        for (int i = 0; i < 4; ++i) undo()->undo();
        QCOMPARE(value(s, "lo_freq"), 90.0);
        QCOMPARE(value(s, "hi_freq"), 4500.0);

        // Frozen: every band holds (with Cut and Flat about 1000 s, along the top).
        set(s, "freeze", 1.0);
        QVERIFY(graph->frozen());
        for (const double seconds : graph->seconds()) QVERIFY2(seconds >= 900.0, qPrintable(QString::number(seconds)));
        hover(s.view, QPointF(1, 1));  // (the mouse off the handles)
        QCOMPARE(graph->hovered(), ReverbDecayGraph::None);
        QCOMPARE(graph->readout(), QStringLiteral("Frozen"));
        // Without Flat the shelves still take their bands away.
        set(s, "flat", 0.0);
        const auto at = [&](double hz) {
            return graph->seconds()[indexOf(frequencies, hz)];
        };
        QVERIFY2(at(10000.0) < 10.0, qPrintable(QString::number(at(10000.0))));
        QVERIFY2(at(1000.0) > 20.0, qPrintable(QString::number(at(1000.0))));
    }

    // --- What the engine publishes reaches them -------------------------------------------------

    void displays() {
        Shown s = reverb();
        QVERIFY(s.filter && s.spin && s.decay);
        refreshDisplays();  // (nothing yet)
        QCOMPARE(s.filter->inputLevel(), kReverbMeterFloorDb);
        play();
        // The input: the tone's 0.5 summed to mono.
        QVERIFY2(std::abs(s.filter->inputLevel() + 6.02) < 0.6, qPrintable(QString::number(s.filter->inputLevel())));
        QVERIFY(s.filter->glow() > 0.5);
        const auto peakHz = [](const sub::app::analysis::FallingSpectrum& spectrum) {
            const std::vector<float>& levels = spectrum.levels();
            const std::size_t top = std::size_t(std::max_element(levels.begin(), levels.end()) - levels.begin());
            return double(top) * spectrum.sampleRate() / sub::app::analysis::FallingSpectrum::kFftSize;
        };
        const double inputPeak = peakHz(s.filter->spectrum());
        QVERIFY2(std::abs(std::log2(inputPeak / 1000.0)) < 1.0 / 6, qPrintable(QString::number(inputPeak)));
        // The tail: its meter, and its spectrum around the tone.
        QVERIFY2(s.decay->tailLevel() > -50.0, qPrintable(QString::number(s.decay->tailLevel())));
        const double tailPeak = peakHz(s.decay->tailSpectrum());
        QVERIFY2(std::abs(std::log2(tailPeak / 1000.0)) < 1.0 / 6, qPrintable(QString::number(tailPeak)));
        const std::size_t bin1k = std::size_t(std::lround(1000.0 * sub::app::analysis::FallingSpectrum::kFftSize /
                                                          s.decay->tailSpectrum().sampleRate()));
        QVERIFY(s.decay->tailSpectrum().levels()[bin1k] > sub::app::analysis::FallingSpectrum::kFloorDb + 30);
        // Spin's phase, and the reflections lit.
        QVERIFY2(s.spin->phase() >= 0.0 && s.spin->phase() < 1.0, qPrintable(QString::number(s.spin->phase())));
        QVERIFY(s.spin->flash() > 0.5);
        QVERIFY(s.spin->amountShown() > 0.0);  // (the swing easing in as the reflections sound)
        // A tick that brings nothing at once (an audio block longer than a tick) keeps what was there.
        const double held = s.decay->tailLevel(), input = s.filter->inputLevel();
        refreshDisplays();
        QVERIFY2(s.decay->tailLevel() >= held - 0.5, qPrintable(QString::number(s.decay->tailLevel())));
        QCOMPARE(s.filter->inputLevel(), input);

        // Nothing more played: the tail meter falls (at least as fast as the tail would), the input reads
        // nothing and the glow fades.
        const double tail = s.decay->tailLevel(), glow = s.filter->glow(), flash = s.spin->flash();
        refreshes(10, 30);
        QVERIFY2(s.decay->tailLevel() <= tail - 10.0, qPrintable(QString::number(s.decay->tailLevel())));
        QVERIFY2(s.spin->amountShown() > 0.15, qPrintable(QString::number(s.spin->amountShown())));  // (they ring on)
        QCOMPARE(s.filter->inputLevel(), kReverbMeterFloorDb);
        QVERIFY(s.filter->glow() < glow);
        QVERIFY(s.spin->flash() < flash);
    }

    // A read can hold a long backlog (an editor shown, or shown again, after the sound stopped): what is lit is
    // now (the newest values, DeviceCanvas::readRecent), not the backlog's loudest.
    void displaysAfterABacklog() {
        Shown s = reverb();  // (a second of the tone)
        QVERIFY(s.filter && s.spin && s.decay);
        refreshDisplays();
        play(3.0);  // the tone, and two seconds of silence after it, read at once
        QCOMPARE(s.filter->inputLevel(), kReverbMeterFloorDb);
        QCOMPARE(s.filter->glow(), 0.0);
        QCOMPARE(s.spin->flash(), 0.0);
        QCOMPARE(s.decay->tailLevel(), ReverbDecayGraph::kMeterFloorDb);
    }

    // --- Transitions ease, and then everything rests --------------------------------------------

    void animation() {
        Shown s = reverb();
        QVERIFY(s.filter && s.spin && s.decay);
        refreshes(3);
        // Freeze lifts the curve over a moment, not at once (looked at before anything is processed, so no
        // display tick can come between), and eases into the top edge rather than running into it.
        set(s, "freeze", 1.0);
        QVERIFY(s.decay->frozen());
        QCOMPARE(s.decay->frozenShown(), 0.0);
        refreshDisplays();
        QVERIFY2(s.decay->frozenShown() > 0.0 && s.decay->frozenShown() < 1.0,
                 qPrintable(QString::number(s.decay->frozenShown())));
        const std::size_t mid = s.decay->shownY().size() / 2;
        double middle = s.decay->shownY()[mid];
        QVERIFY(middle < s.decay->yOf(1.2) && middle > s.decay->yOf(1000.0));
        double lastStep = 0.0;
        for (int i = 0; i < 60 && s.decay->frozenShown() < 1.0; ++i) {
            refreshes(1);
            if (s.decay->shownY()[mid] != middle)
                lastStep = middle - s.decay->shownY()[mid];
            middle = s.decay->shownY()[mid];
        }
        QCOMPARE(s.decay->frozenShown(), 1.0);
        QCOMPARE(s.decay->shownY()[mid], s.decay->yOf(1000.0));  // along the top
        QVERIFY2(lastStep < 0.5, qPrintable(QString::number(lastStep)));

        // A switch eases the filter's curve: Lo Cut off.
        ReverbFilterPad* pad = s.filter;
        const std::vector<double> frequencies = pad->curveFrequencies();
        const std::size_t at30 = indexOf(frequencies, 30.0);
        const double before = pad->curveShown()[at30];
        QVERIFY(before < -6.0);
        set(s, "lo_cut", 0.0);
        QCOMPARE(pad->curveShown()[at30], before);  // (not at once)
        refreshDisplays();
        QVERIFY2(pad->curveShown()[at30] > before && pad->curveShown()[at30] < pad->curveDb()[at30],
                 qPrintable(QString::number(pad->curveShown()[at30])));
        QVERIFY(pad->animating());
        refreshes(20);
        QVERIFY(pad->curveShown() == pad->curveDb());

        // A drag never animates: the curve follows the parameter at once.
        set(s, "in_width", 2.0);
        QVERIFY(pad->curveShown() == pad->curveDb());

        // Something played, then nothing: within a few seconds of refreshes nothing asks to be painted.
        set(s, "freeze", 0.0);  // (frozen with Cut, nothing new reaches the tail)
        play(0.3);
        QVERIFY(s.decay->animating() && pad->animating() && s.spin->animating());
        QVERIFY(s.decay->tailLevel() > -50.0);
        bool rested = false;
        for (int i = 0; i < 200 && !rested; ++i) {
            refreshes(1);
            rested = !s.decay->animating() && !pad->animating() && !s.spin->animating();
        }
        QVERIFY2(rested, qPrintable(QStringLiteral("decay %1 filter %2 spin %3")
                                        .arg(s.decay->animating())
                                        .arg(pad->animating())
                                        .arg(s.spin->animating())));
        QCOMPARE(s.decay->tailLevel(), ReverbDecayGraph::kMeterFloorDb);
        QCOMPARE(s.spin->amountShown(), 0.0);  // (in silence the reflections settle at rest, all the way)
        QCOMPARE(s.spin->flash(), 0.0);
        QCOMPARE(s.filter->glow(), 0.0);
        // And it stays at rest: no tick asks for a repaint again (none at all, the display clock's own between
        // these too: a tick that moves anything says so).
        QSignalSpy decayTicks(s.decay, &ReverbDecayGraph::levelsChanged);
        QSignalSpy filterTicks(pad, &ReverbFilterPad::levelsChanged);
        QSignalSpy spinTicks(s.spin, &ReverbSpinPad::levelsChanged);
        refreshes(25);
        QVERIFY2(decayTicks.isEmpty() && filterTicks.isEmpty() && spinTicks.isEmpty(),
                 qPrintable(QStringLiteral("decay %1 filter %2 spin %3")
                                .arg(decayTicks.count())
                                .arg(filterTicks.count())
                                .arg(spinTicks.count())));
        QVERIFY(!s.decay->animating() && !pad->animating() && !s.spin->animating());
    }

    // --- How it looks ------------------------------------------------------------------------------

    void screenshot() {
        Shown s = reverb(2.0);
        QVERIFY(s.view && s.spin && s.decay);
        play(0.6);
        refreshes(2, 16);
        QTest::qWait(50);
        const QImage playing = grab();
        QVERIFY(varied(partOf(playing, s.decay)));
        QVERIFY(varied(partOf(playing, s.spin)));
        QVERIFY(varied(partOf(playing, s.filter)));
        save(playing, QStringLiteral("reverb.png"));

        // Hovering a shelf's handle reads it out.
        hover(s.decay, s.decay->hiHandle());
        refreshes(10, 16);
        QCOMPARE(s.decay->hovered(), ReverbDecayGraph::Hi);
        QCOMPARE(s.decay->readout(), QStringLiteral("Hi 4.50 kHz · 70 %"));
        QTest::qWait(50);
        save(grab(), QStringLiteral("reverb-hover.png"));
        hover(s.view, QPointF(1, 1));

        // Frozen while it plays (Cut off: the input keeps feeding the frozen tail), Spin wide and fast, Chorus
        // deep: first as Freeze lifts the curve, then held.
        set(s, "cut", 0.0);
        set(s, "spin_amount", 90.0);
        set(s, "spin_rate", 1.1);
        set(s, "chorus_amount", 80.0);
        play(0.6);
        click(s.view, "freezeButton");
        hover(s.view, QPointF(2, 2));  // (off the button: no tooltip)
        QTest::qWait(60);
        QVERIFY(s.decay->frozenShown() > 0.0 && s.decay->frozenShown() < 1.0);
        save(grab(), QStringLiteral("reverb-freezing.png"));
        refreshes(60, 16);
        engine()->renderOffline(0.0, int64_t(0.8 * kSampleRate));
        refreshDisplays();
        refreshes(2, 16);
        QTest::qWait(50);
        QCOMPARE(s.decay->frozenShown(), 1.0);
        save(grab(), QStringLiteral("reverb-frozen.png"));

        // Another mode: Sparse, the high filter a low-pass, the input band narrow, Lo Shelf, Spin and Chorus off.
        set(s, "freeze", 0.0);
        set(s, "cut", 1.0);
        set(s, "density", 0.0);
        set(s, "hi_type", 1.0);
        set(s, "hi_freq", 2500.0);
        set(s, "lo_shelf", 0.0);
        set(s, "in_width", 2.5);
        set(s, "in_freq", 1500.0);
        set(s, "spin", 0.0);
        set(s, "chorus", 0.0);
        set(s, "decay", 6000.0);
        refreshes(30, 16);
        QTest::qWait(50);
        save(grab(), QStringLiteral("reverb-sparse.png"));

        // Every box at its widest text and automated (the dot at its left, clear of the text), every switch on.
        for (const auto& [name, id] : kControls) {
            if (QByteArray(name).endsWith("Button")) {
                set(s, id, 1.0);
            } else if (QByteArray(name).endsWith("Box")) {
                set(s, id, widestValue(paramOf(control(s.view, name)), QFontMetricsF(box(s.view, name)->font())));
                box(s.view, name)->setProperty("automation", QStringLiteral("on"));
            }
        }
        refreshes(30, 16);
        QTest::qWait(200);  // (the dimmed controls' fades)
        const QImage image = grab();
        save(image, QStringLiteral("reverb-automation.png"));
        // As drawn: in each box the dot (6 px in) and the text, with the pixel past where the dot ends the box's
        // own colour all the way down inside its border.
        const double dpr = image.devicePixelRatio();
        for (const auto& [name, id] : kControls) {
            if (!QByteArray(name).endsWith("Box"))
                continue;
            QQuickItem* item = control(s.view, name);
            const QRectF r = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
            const auto pixel = [&](double x, double y) { return image.pixel(int(x * dpr), int(y * dpr)); };
            const QRgb inside = pixel(r.left() + kDotRight + 0.5, r.top() + 2);
            QVERIFY2(pixel(r.left() + 6, r.center().y()) != inside, name);  // (the dot)
            for (int x = int(std::round((r.left() + kDotRight + 0.5) * dpr)); x < int((r.left() + kTextFrom) * dpr);
                 ++x) {
                for (int y = int((r.top() + 2) * dpr); y < int((r.bottom() - 2) * dpr); ++y) {
                    QVERIFY2(image.pixel(x, y) == inside,
                             qPrintable(QStringLiteral("%1: (%2, %3) in it").arg(QString::fromLatin1(name))
                                            .arg(x / dpr - r.left())
                                            .arg(y / dpr - r.top())));
                }
            }
        }
    }
};

QTEST_MAIN(TestUiDeviceEditorsReverb)
#include "test_ui_device_editors_reverb.moc"
