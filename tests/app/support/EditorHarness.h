#pragma once
// What the built-in devices' editors' tests share (test_ui_device_editors*.cpp):
// a session over a real engine (no audio device), and a window showing one
// editor at a time as the device view shows it: DeviceEditors's component for
// the device's kind, given the track and the device, the body's size (as tall
// as the view makes every device), in its frame's colours. Then finding its
// parts by object name, the mouse and wheel as a user moves them, the
// displays' clock ticked by hand, and the project, the undo stack and the
// engine to check. With SUBSTATION_UI_SCREENSHOTS set to a folder, save()
// writes PNGs there.
//
//   class TestUiThing : public QObject, public sub::app::test::EditorHarness {
//       Q_OBJECT
//   private Q_SLOTS:
//       void initTestCase() { if (!haveDisplay()) QSKIP("..."); startHost(); }
//       void cleanupTestCase() { stopHost(); }
//       void init() { clearHost(); }
//       void thing() { QQuickItem* view = show("thing", track, device); ... }
//   };
//
// Header only, as UiTestSupport.h (the support library doesn't link Qt Quick).

#include "Engine.h"
#include "TestSupport.h"
#include "Ui.h"
#include "app/AppTypes.h"
#include "audio/EngineBridge.h"
#include "devices/DisplayClock.h"
#include "editor/ClipRef.h"
#include "editor/ProjectEditor.h"
#include "model/Clip.h"
#include "model/Device.h"
#include "model/Project.h"
#include "session/Session.h"

#include <QCursor>
#include <QDir>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPointF>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QTest>
#include <QUndoStack>
#include <QUrl>
#include <QWheelEvent>
#include <QtDebug>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace sub::app::test {

// The host window's QML.
inline constexpr const char* kEditorHost = R"QML(
import QtQuick
import SUBstation

Window {
    id: window

    // The editor shown, the body of a device in the device view, in its frame's colours.
    function show(url, trackId, deviceId, height) {
        loader.setSource(url, { trackId: trackId, deviceId: deviceId })
        window.bodyHeight = height
        return loader.item
    }
    function clear() {
        loader.sourceComponent = null
        loader.source = ""
    }
    function editorFor(kind) {
        return DeviceEditors.editorFor(kind)
    }
    function eqWindow(trackId, deviceId) {
        return EqWindows.windowOf(trackId, deviceId)
    }
    // How wide `text` is in `font` as a Text lays it out.
    function textWidth(text, font) {
        textProbe.font = font
        textProbe.text = text
        return textProbe.implicitWidth
    }
    // A parameter's cell, as the device view shows those of devices without an editor.
    function showKnob(trackId, deviceId, paramId) {
        loader.sourceComponent = knob
        loader.item.trackId = trackId
        loader.item.deviceId = deviceId
        loader.item.paramId = paramId
        window.bodyHeight = loader.item.implicitHeight + 8
        return loader.item
    }

    Text {
        id: textProbe
        visible: false
        textFormat: Text.PlainText
    }
    Component {
        id: knob
        DeviceParamKnob {
            x: 8
            y: 4
        }
    }

    // The body of the device view's tallest device: two rows of knobs (a hidden one measured),
    // 14 px apart, 6 px above and below them (DevicePanel's deviceHeight, less the frame and title bar).
    readonly property int deviceBodyHeight: 6 + 2 * probe.implicitHeight + 14 + 6
    DeviceParamKnob {
        id: probe
        visible: false
    }

    // The body's size, which the window follows (as the X server resizes it: later).
    property int bodyHeight: deviceBodyHeight
    readonly property int bodyWidth: loader.item ? loader.item.implicitWidth : 198

    width: bodyWidth + 2
    height: bodyHeight + 2
    visible: true
    color: Theme.border

    Rectangle {
        anchors.fill: parent
        anchors.margins: 1
        color: Theme.panelAlt
        radius: 3
    }
    Loader {
        id: loader
        x: 1
        y: 1
        width: window.bodyWidth
        height: window.bodyHeight
    }
}
)QML";

class EditorHarness {
protected:
    std::unique_ptr<sub::Engine> engine_;
    std::unique_ptr<Session> session_;
    std::unique_ptr<QQmlEngine> qml_;
    std::unique_ptr<QObject> root_;
    QQuickWindow* window_ = nullptr;
    TempDir dir_;

    // Whether Qt Quick draws the UI's scene-graph items here (not on the offscreen platform, which
    // renders Qt Quick in software, without their geometry): initTestCase skips without one.
    static bool haveDisplay() {
        const QString platform = QGuiApplication::platformName();
        return platform != QLatin1String("offscreen") && platform != QLatin1String("minimal");
    }

    // initTestCase: the session over an engine, the QML engine, the host window shown and active.
    // A failure is a QVERIFY's (the test function goes on to check QTest::currentTestFailed()).
    void startHost() {
        prepareApplication();
        sub::ui::setUpApplication();
        engine_ = std::make_unique<sub::Engine>();
        engine_->setClipFadeMs(0);
        Session::Options options;
#ifdef SUBSTATION_SCANNER
        options.scanner = QStringLiteral(SUBSTATION_SCANNER);
#endif
        options.scanPlugins = false;
        options.browserIndex = false;
        session_ = std::make_unique<Session>(*engine_, options);
        sub::ui::registerSession(session_.get());
        qml_ = std::make_unique<QQmlEngine>();
        sub::ui::setUpEngine(*qml_);
        QQmlComponent component(qml_.get());
        component.setData(kEditorHost, QUrl(QStringLiteral("qrc:/test/Host.qml")));
        root_.reset(component.create());
        if (!root_)
            qWarning() << component.errors();
        QVERIFY(root_);
        window_ = qobject_cast<QQuickWindow*>(root_.get());
        QVERIFY(window_);
        window_->setPosition(200, 200);
        QVERIFY(QTest::qWaitForWindowExposed(window_));
        window_->requestActivate();
        QVERIFY(QTest::qWaitForWindowActive(window_));
    }

    // cleanupTestCase.
    void stopHost() {
        root_.reset();
        qml_.reset();
        if (session_)
            session_->shutdown();
        session_.reset();
        engine_.reset();
    }

    // init: no editor shown, an empty project, nothing to undo, the mouse out of the way.
    void clearHost() {
        QMetaObject::invokeMethod(root_.get(), "clear");
        project()->clear();
        undo()->clear();
        parkCursor();
        QTest::mouseMove(window_, QPoint(1, 1));
    }

    // The system's cursor out of the window's way, at the screen's bottom left. A knob's or value box's drag puts
    // it back where the drag began (DragCursor), over the editor, and Windows tells a window where it is whenever
    // the window changes under it: shrunk as an editor goes, the window grows back under it as the next is shown,
    // and Qt has the mouse enter there. What lies there would be hovered, and its tooltip would pop up over the
    // editor. (Only this program moves the cursor meanwhile: on Windows CTest runs the UI's tests one at a time.)
    void parkCursor() {
        QScreen* screen = window_->screen();
        if (!screen)
            return;
        const QRect area = screen->availableGeometry();
        const QPoint away(area.left() + 8, area.bottom() - 8);
        if (QCursor::pos(screen) == away)
            return;
        QCursor::setPos(screen, away);
        QCoreApplication::processEvents();  // (the window told it left)
    }

    sub::Engine* engine() const { return engine_.get(); }
    Project* project() const { return session_->project(); }
    ProjectEditor* editor() const { return session_->editor(); }
    QUndoStack* undo() const { return session_->undoStack(); }
    EngineBridge* bridge() const { return session_->bridge(); }

    // A device's parameter as the project has it (`otherwise` without one).
    double param(const QString& trackId, const QString& deviceId, const QString& paramId, double otherwise = -999) {
        const Device* device = project()->findDevice(trackId, deviceId);
        return device && device->params.contains(paramId) ? device->params.value(paramId) : otherwise;
    }

    // The device's body in the device view: the tallest device's (two rows of knobs and their names,
    // 6 px above and below them), as the host measures it.
    int bodyHeight() const { return root_->property("deviceBodyHeight").toInt(); }

    // Shows a device's editor (DeviceEditors's component for its kind), the body's size (the device
    // view's by default); the editor.
    QQuickItem* show(const QString& kind, const QString& trackId, const QString& deviceId, int height = 0) {
        if (height <= 0)
            height = bodyHeight();
        parkCursor();  // (before the window takes the editor's size)
        QVariant url;
        QMetaObject::invokeMethod(root_.get(), "editorFor", Q_RETURN_ARG(QVariant, url), Q_ARG(QVariant, kind));
        if (url.toString().isEmpty())
            return nullptr;
        QVariant item;
        QMetaObject::invokeMethod(root_.get(), "show", Q_RETURN_ARG(QVariant, item), Q_ARG(QVariant, url),
                                  Q_ARG(QVariant, trackId), Q_ARG(QVariant, deviceId), Q_ARG(QVariant, height));
        auto* editor = qvariant_cast<QQuickItem*>(item);
        if (!editor)
            return nullptr;
        fitted();
        QTest::qWait(50);  // (laid out, painted)
        return editor;
    }

    // Waits for the window to take the body's size (the X server resizes it later), so
    // clicks land inside it.
    bool fitted() {
        return QTest::qWaitFor([this] {
            return window_->width() == root_->property("bodyWidth").toInt() + 2
                && window_->height() == root_->property("bodyHeight").toInt() + 2;
        }, 3000);
    }

    // Every item under `root` (in the item tree) whose object name starts with `prefix`, in order.
    static QList<QQuickItem*> itemsNamed(QQuickItem* root, const QString& prefix) {
        QList<QQuickItem*> found;
        for (QQuickItem* child : root->childItems()) {
            if (child->objectName().startsWith(prefix))
                found << child;
            found += itemsNamed(child, prefix);
        }
        return found;
    }

    // The item under `in` named `name` (of type T), or null (with a warning).
    template <typename T = QQuickItem>
    T* find(QQuickItem* in, const QString& name) {
        for (QQuickItem* item : itemsNamed(in, name)) {
            if (item->objectName() == name) {
                if (auto* found = qobject_cast<T*>(item))
                    return found;
            }
        }
        qWarning() << "no" << name;
        return nullptr;
    }

    QImage grab() { return window_->grabWindow(); }

    // Saves `image` as device-editors-<name> in SUBSTATION_UI_SCREENSHOTS, if set.
    static void save(const QImage& image, const QString& name) {
        const QString folder = qEnvironmentVariable("SUBSTATION_UI_SCREENSHOTS");
        if (folder.isEmpty())
            return;
        QDir().mkpath(folder);
        QVERIFY(image.save(QDir(folder).filePath(QStringLiteral("device-editors-") + name)));
    }

    // --- Texts -----------------------------------------------------------------------------

    // How wide `text` is in `font` (an item's "font" property) as a Text lays it out (a caption, a readout, a
    // button's or a list's name): what that Text needs to show it whole. (QFontMetricsF can be a pixel off it,
    // where the font is hinted or kerned.)
    double textWidth(const QString& text, const QVariant& font) const {
        QVariant width;
        QMetaObject::invokeMethod(root_.get(), "textWidth", Q_RETURN_ARG(QVariant, width), Q_ARG(QVariant, text),
                                  Q_ARG(QVariant, font));
        return width.toDouble();
    }

    // The widest of `texts` in `font`, as textWidth() measures them, a "#" in them as the font's widest figure
    // (by the same measure).
    double widestOf(const QStringList& texts, const QVariant& font) const {
        QString widestFigure = QStringLiteral("0");
        for (char figure = '1'; figure <= '9'; ++figure) {
            if (textWidth(QString(QLatin1Char(figure)), font) > textWidth(widestFigure, font))
                widestFigure = QString(QLatin1Char(figure));
        }
        double widest = 0.0;
        for (QString text : texts)
            widest = std::max(widest, textWidth(text.replace(QLatin1Char('#'), widestFigure), font));
        return widest;
    }

    // A text's form, as EditorKnob's texts() gives it: each number's figures after its first as "#".
    static QString formOf(QString text) {
        bool inNumber = false;
        for (QChar& c : text) {
            if (c.isDigit()) {
                if (inNumber)
                    c = QLatin1Char('#');
                inNumber = true;
            } else if (c != QLatin1Char('.')) {
                inNumber = false;
            }
        }
        return text;
    }

    // Every text an EditorKnob's readout shows across its knob's range (its parameter's, or its formatter's): at
    // `points` across it (in its own scale), and just under each power of ten in it, where a value rounds up to a
    // longer text ("1000 ms").
    QStringList readoutTexts(QQuickItem* cell, int points = 1000) const {
        auto* param = cell->property("param").value<QObject*>();
        if (!param)
            return {};
        const QJSValue formatter = cell->property("formatter").value<QJSValue>();
        const double lo = param->property("minimum").toDouble(), hi = param->property("maximum").toDouble();
        const bool log = param->property("logScale").toBool() && lo > 0;
        std::vector<double> values;
        for (int i = 0; i <= points; ++i) {
            const double t = double(i) / points;
            values.push_back(log ? lo * std::pow(hi / lo, t) : lo + (hi - lo) * t);
        }
        for (int n = -3; n <= 6; ++n) {
            for (const double power : {std::pow(10.0, n), -std::pow(10.0, n)}) {
                if (const double under = power * (1 - 1e-9); under > lo && under < hi)
                    values.push_back(under);
            }
        }
        QStringList texts;
        for (double value : values) {
            if (param->property("steps").toInt() > 0)
                value = std::round(value);
            QString text;
            if (formatter.isCallable())
                text = formatter.call({QJSValue(value)}).toString();
            else
                QMetaObject::invokeMethod(param, "format", Q_RETURN_ARG(QString, text), Q_ARG(double, value));
            texts << text;
        }
        texts.removeDuplicates();
        return texts;
    }

    // How wide these EditorKnobs' cells must be for their captions and every text their readouts show
    // (readoutTexts(), each number's figures after its first the font's widest), as a Text lays them out.
    double knobsNeed(const QList<QQuickItem*>& cells) const {
        double widest = 0.0;
        for (QQuickItem* cell : cells) {
            QQuickItem* caption = cell->childItems().first();  // (caption, knob, readout)
            QQuickItem* readout = cell->childItems().last();
            QStringList forms;
            for (const QString& text : readoutTexts(cell))
                forms << formOf(text);
            forms.removeDuplicates();
            widest = std::max({widest, textWidth(caption->property("text").toString(), caption->property("font")),
                               widestOf(forms, readout->property("font"))});
        }
        return widest;
    }

    // What of an EditorKnob's texts isn't whole (empty if all are), as a Text lays them out in their fonts: its
    // caption (centred over its knob while shown), every text its readout shows (readoutTexts()), and what its
    // texts() gives an editor to size its cell by (its caption and the forms of those texts, all and no others).
    QString knobTextsProblem(QQuickItem* cell) const {
        const QString name = cell->objectName();
        const QList<QQuickItem*> parts = cell->childItems();  // (caption, knob, readout)
        QQuickItem* caption = parts.first();
        QQuickItem* readout = parts.last();
        const QString title = caption->property("text").toString();
        if (caption->property("truncated").toBool() || textWidth(title, caption->property("font")) > caption->width())
            return QStringLiteral("%1: its caption, in %2 px").arg(name).arg(caption->width());
        const QRectF knob = parts.at(1)->mapRectToItem(cell, QRectF(0, 0, parts.at(1)->width(), parts.at(1)->height()));
        if (cell->isVisible() && std::abs(caption->x() + caption->width() / 2 - knob.center().x()) > 0.5)
            return QStringLiteral("%1: its caption off its knob").arg(name);
        const QStringList texts = readoutTexts(cell);
        QStringList forms = {title};
        for (const QString& text : texts) {
            if (text.isEmpty() || textWidth(text, readout->property("font")) > readout->width())
                return QStringLiteral("%1: \"%2\" in %3 px").arg(name, text).arg(readout->width());
            forms << formOf(text);
        }
        forms.removeDuplicates();
        QVariant given;
        QMetaObject::invokeMethod(cell, "texts", Q_RETURN_ARG(QVariant, given));
        if (given.metaType() == QMetaType::fromType<QJSValue>())
            given = given.value<QJSValue>().toVariant();
        QStringList sizedBy = given.toStringList();
        sizedBy.removeDuplicates();
        forms.sort();
        sizedBy.sort();
        if (sizedBy != forms)
            return QStringLiteral("%1: sized by %2, not %3").arg(name, sizedBy.join(u'|'), forms.join(u'|'));
        return {};
    }

    // The displays' clock ticked: what the editors read from the engine's displays as the view refreshes.
    void refreshDisplays() { Q_EMIT sub::ui::DisplayClock::instance()->tick(); }

    // A track playing `mono` (as stereo) from the start for `seconds`: its id ("" if it failed).
    QString audioTrackWith(const std::vector<float>& mono, const QString& name, double seconds) {
        const QString path = writeWav(dir_.path(name + QStringLiteral(".wav")), stereo(mono), 2);
        const ClipRefs refs = editor()->addClips(QString(), 0.0, {{path, seconds}});
        return refs.isEmpty() ? QString() : refs.first().trackId;
    }

    // --- Value boxes -----------------------------------------------------------------------

    // A value box (a ParamBox) draws its text centred by its advance (as QFontMetricsF measures it), from a whole
    // pixel, and its automation dot from 3.5 to 8.5 px from its left. What of its texts isn't whole or touches the
    // dot (empty if none): every text it shows (readoutTexts(), by the box's `param` and `formatter`) must start,
    // its ink too, 10 px in at the least, a pixel clear of the dot.
    QString boxTextsProblem(QQuickItem* control) const {
        const QString name = control->objectName();
        auto* box = qvariant_cast<QQuickItem*>(control->property("box"));
        if (!box)
            return QStringLiteral("%1: no box").arg(name);
        const QFontMetricsF metrics(qvariant_cast<QFont>(box->property("font")));
        const QStringList texts = readoutTexts(control);
        if (texts.isEmpty())
            return QStringLiteral("%1: no texts").arg(name);
        for (const QString& text : texts) {
            const double start = std::round((box->width() - metrics.horizontalAdvance(text)) / 2);
            const double ink = start + std::min(0.0, metrics.boundingRect(text).left());
            if (text.isEmpty() || ink < 10.0)
                return QStringLiteral("%1: \"%2\" from %3 px in %4 px").arg(name, text).arg(ink).arg(box->width());
        }
        return {};
    }

    // How wide these value boxes must be for every text they show (readoutTexts(), each number's figures after its
    // first the font's widest, by advance) with 9.5 px at either side, and as far again as a text's ink starts left
    // of its advance: the least that starts every text 10 px in (boxTextsProblem()), as EditorBoxWidth measures.
    double boxesNeed(const QList<QQuickItem*>& controls) const {
        double need = 0.0;
        for (QQuickItem* control : controls) {
            auto* box = qvariant_cast<QQuickItem*>(control->property("box"));
            if (!box)
                return 1e9;
            const QFontMetricsF metrics(qvariant_cast<QFont>(box->property("font")));
            QChar widestFigure = QLatin1Char('0');
            for (char figure = '1'; figure <= '9'; ++figure) {
                if (metrics.horizontalAdvance(QLatin1Char(figure)) > metrics.horizontalAdvance(widestFigure))
                    widestFigure = QLatin1Char(figure);
            }
            for (const QString& text : readoutTexts(control)) {
                const QString sample = formOf(text).replace(QLatin1Char('#'), widestFigure);
                const double inkBefore = std::max(0.0, -metrics.boundingRect(sample).left());
                need = std::max(need, std::ceil(metrics.horizontalAdvance(sample) + 2 * (inkBefore + 9.5)));
            }
        }
        return need;
    }

    // --- Input -----------------------------------------------------------------------------

    static QPoint scenePoint(QQuickItem* item, QPointF at) { return item->mapToScene(at).toPoint(); }
    static QPoint centerOf(QQuickItem* item) {
        return scenePoint(item, QPointF(item->width() / 2, item->height() / 2));
    }

    // A move with the left button held, with modifiers (QTest's mouseMove has none).
    static void dragTo(QQuickWindow* window, QPoint pos, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QMouseEvent event(QEvent::MouseMove, QPointF(pos), QPointF(window->mapToGlobal(pos)), Qt::NoButton,
                          Qt::LeftButton, modifiers);
        QGuiApplication::sendEvent(window, &event);
    }
    void dragTo(QPoint pos, Qt::KeyboardModifiers modifiers = Qt::NoModifier) { dragTo(window_, pos, modifiers); }

    static void wheel(QQuickWindow* window, QPoint pos, int angle, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        QWheelEvent event(QPointF(pos), QPointF(window->mapToGlobal(pos)), QPoint(), QPoint(0, angle), Qt::NoButton,
                          modifiers, Qt::NoScrollPhase, false);
        QGuiApplication::sendEvent(window, &event);
    }
    void wheel(QPoint pos, int angle, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
        wheel(window_, pos, angle, modifiers);
    }

    // --- Signals ---------------------------------------------------------------------------

    static std::vector<float> tone(double freq, int frames, double amplitude = 0.5) {
        std::vector<float> samples(static_cast<size_t>(frames));
        for (int i = 0; i < frames; ++i)
            samples[size_t(i)] = float(amplitude * std::sin(2 * 3.14159265358979323846 * freq * i / kSampleRate));
        return samples;
    }

    // Interleaved stereo of the same mono signal.
    static std::vector<float> stereo(const std::vector<float>& mono) {
        std::vector<float> both;
        both.reserve(mono.size() * 2);
        for (float s : mono) {
            both.push_back(s);
            both.push_back(s);
        }
        return both;
    }
};

}  // namespace sub::app::test
