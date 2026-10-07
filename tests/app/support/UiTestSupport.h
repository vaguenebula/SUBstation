#pragma once
// What the UI's tests share: a session over an engine (no audio device),
// known to QML as the `Session` singleton, a window of QML on it, and mouse,
// wheel and key input sent to that window as a user would. Header only: the
// support library doesn't link Qt Quick; the UI tests (test_ui_*) do.

#include "Engine.h"
#include "TestSupport.h"
#include "Ui.h"
#include "app/AppTypes.h"
#include "session/Session.h"

#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPointF>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <QWheelEvent>
#include <QtDebug>

#include <memory>

namespace sub::app::test {

// Whether Qt Quick draws the UI's scene-graph items here (not on the offscreen
// platform, which renders Qt Quick in software, without their geometry).
inline bool haveDisplay() {
    const QString platform = QGuiApplication::platformName();
    return platform != QLatin1String("offscreen") && platform != QLatin1String("minimal");
}

// A session on a fresh engine, registered for QML, and a QML engine set up for
// the UI's module. One per test program (the Session singleton is registered once).
class UiSession {
public:
    // `analyseSounds`: fingerprint the browser's files for Find Similar (off
    // unless a test sets places of its own: by default it lists the user's Music).
    explicit UiSession(bool analyseSounds = false) : engine_(std::make_unique<sub::Engine>()) {
        // Qt Quick's own file dialogs, as on Linux: a native one (Windows) is
        // modal and out of the tests' reach.
        QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
        engine_->setClipFadeMs(0);
        Session::Options options;
#ifdef SUBSTATION_SCANNER
        options.scanner = QStringLiteral(SUBSTATION_SCANNER);
#endif
        options.scanPlugins = false;
        options.browserIndex = false;
        options.analyseSounds = analyseSounds;
        session_ = std::make_unique<Session>(*engine_, options);
        sub::ui::registerSession(session_.get());
        qml_ = std::make_unique<QQmlEngine>();
        sub::ui::setUpEngine(*qml_);
    }

    ~UiSession() {
        root_.reset();
        qml_.reset();
        session_->shutdown();
        session_.reset();
    }

    sub::Engine& engine() { return *engine_; }
    Session& session() { return *session_; }
    QQmlEngine& qml() { return *qml_; }

    // Shows `qml` (a Window), active, away from the screen's edges; null if it didn't load.
    QQuickWindow* show(const char* qml) {
        QQmlComponent component(qml_.get());
        component.setData(qml, QUrl(QStringLiteral("qrc:/test/Window.qml")));
        root_.reset(component.create());
        if (!root_) {
            qWarning() << component.errors();
            return nullptr;
        }
        auto* window = qobject_cast<QQuickWindow*>(root_.get());
        if (!window) return nullptr;
        window->setPosition(200, 100);
        window->show();
        window->requestActivate();  // (shortcuts need an active window)
        if (!QTest::qWaitForWindowExposed(window) || !QTest::qWaitForWindowActive(window)) return nullptr;
        return window;
    }

    QObject* root() const { return root_.get(); }

private:
    std::unique_ptr<sub::Engine> engine_;
    std::unique_ptr<Session> session_;
    std::unique_ptr<QQmlEngine> qml_;
    std::unique_ptr<QObject> root_;
};

// A point of an item, in its window.
inline QPoint at(const QQuickItem* item, const QPointF& local) { return item->mapToScene(local).toPoint(); }
inline QPoint centerOf(const QQuickItem* item) { return at(item, QPointF(item->width() / 2, item->height() / 2)); }

// Mouse input, the left button, with modifiers held throughout (QTest's
// mouseMove has none). Moves go to the window directly, the button held.
inline void press(QQuickWindow* window, QPoint pos, Qt::KeyboardModifiers modifiers = {}) {
    QTest::mousePress(window, Qt::LeftButton, modifiers, pos);
}
inline void moveTo(QQuickWindow* window, QPoint pos, Qt::KeyboardModifiers modifiers = {},
                   Qt::MouseButtons buttons = Qt::LeftButton) {
    QMouseEvent event(QEvent::MouseMove, QPointF(pos), QPointF(window->mapToGlobal(pos)), Qt::NoButton, buttons,
                      modifiers);
    QGuiApplication::sendEvent(window, &event);
}
inline void release(QQuickWindow* window, QPoint pos, Qt::KeyboardModifiers modifiers = {}) {
    QTest::mouseRelease(window, Qt::LeftButton, modifiers, pos);
}
inline void click(QQuickWindow* window, QPoint pos, Qt::KeyboardModifiers modifiers = {}) {
    QTest::mouseClick(window, Qt::LeftButton, modifiers, pos);
}
inline void doubleClick(QQuickWindow* window, QPoint pos, Qt::KeyboardModifiers modifiers = {}) {
    QTest::mouseDClick(window, Qt::LeftButton, modifiers, pos);
}
// Press, move halfway, move to the end, release (the Python tests' drag()).
inline void drag(QQuickWindow* window, QPoint start, QPoint end, Qt::KeyboardModifiers modifiers = {}) {
    press(window, start, modifiers);
    moveTo(window, start + (end - start) / 2, modifiers);
    moveTo(window, end, modifiers);
    release(window, end, modifiers);
}
// Wheel notches (120 each) up (or right, with `horizontal`).
inline void wheel(QQuickWindow* window, QPoint pos, double notches, Qt::KeyboardModifiers modifiers = {},
                  bool horizontal = false) {
    const int angle = static_cast<int>(notches * 120);
    QWheelEvent event(QPointF(pos), QPointF(window->mapToGlobal(pos)), QPoint(),
                      horizontal ? QPoint(angle, 0) : QPoint(0, angle), Qt::NoButton, modifiers, Qt::NoScrollPhase,
                      false);
    QGuiApplication::sendEvent(window, &event);
}

// A screenshot of the window (or a part of it) into $SUBSTATION_UI_SCREENSHOTS, if set
// (as the other UI tests; $SUBSTATION_SCREENS is read too).
inline void screenshot(QQuickWindow* window, const QString& name, const QRect& part = QRect()) {
    QString folder = qEnvironmentVariable("SUBSTATION_UI_SCREENSHOTS");
    if (folder.isEmpty()) folder = qEnvironmentVariable("SUBSTATION_SCREENS");
    if (folder.isEmpty()) return;
    QDir().mkpath(folder);
    QImage image = window->grabWindow();
    if (!part.isNull()) {
        const qreal dpr = image.devicePixelRatio();
        image = image.copy(QRect(part.topLeft() * dpr, part.size() * dpr));
    }
    image.save(QDir(folder).filePath(name + QStringLiteral(".png")));
}

}  // namespace sub::app::test
