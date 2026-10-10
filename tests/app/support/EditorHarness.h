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
    // A parameter's cell, as the device view shows those of devices without an editor.
    function showKnob(trackId, deviceId, paramId) {
        loader.sourceComponent = knob
        loader.item.trackId = trackId
        loader.item.deviceId = deviceId
        loader.item.paramId = paramId
        window.bodyHeight = loader.item.implicitHeight + 8
        return loader.item
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
    // the window changes under it (as each editor is shown, taking its size): what lies there would be hovered,
    // and its tooltip would pop up over the editor.
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

    // The displays' clock ticked: what the editors read from the engine's displays as the view refreshes.
    void refreshDisplays() { Q_EMIT sub::ui::DisplayClock::instance()->tick(); }

    // A track playing `mono` (as stereo) from the start for `seconds`: its id ("" if it failed).
    QString audioTrackWith(const std::vector<float>& mono, const QString& name, double seconds) {
        const QString path = writeWav(dir_.path(name + QStringLiteral(".wav")), stereo(mono), 2);
        const ClipRefs refs = editor()->addClips(QString(), 0.0, {{path, seconds}});
        return refs.isEmpty() ? QString() : refs.first().trackId;
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
