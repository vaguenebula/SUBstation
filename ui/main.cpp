// SUBstation: starts the application layer and shows the Qt Quick UI.
//
//   substation [song.gilproj]
//
// This is where the layers are put together: the real-time engine, the
// application's session on it, and the QML UI on the session.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QtQml/qqmlextensionplugin.h>

#include "AppInfo.h"
#include "app/AppTypes.h"
#include "Engine.h"
#include "Ui.h"
#include "session/Session.h"

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#endif

Q_IMPORT_QML_PLUGIN(SUBstationPlugin)

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // The taskbar groups the windows under our own icon, not the executable's host.
    SetCurrentProcessExplicitAppUserModelID(L"SUBstation.DAW");
#endif
    QGuiApplication::setOrganizationName(QString::fromLatin1(sub::app::kOrganization));
    QGuiApplication::setApplicationName(QString::fromLatin1(sub::app::kAppName));
    QGuiApplication app(argc, argv);
    sub::ui::setUpApplication();

    // The real-time engine, then the application layer on it, then the UI on that.
    sub::Engine engine;
    sub::app::Session session(engine);
    sub::ui::registerSession(&session);

    QQmlApplicationEngine qml;
    sub::ui::setUpEngine(qml);
    QObject::connect(&qml, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    qml.load(QUrl(QStringLiteral("qrc:/qt/qml/SUBstation/qml/Main.qml")));
    if (qml.rootObjects().isEmpty()) return 1;
    if (auto* window = qobject_cast<QQuickWindow*>(qml.rootObjects().first())) {
        // Plug-in editors float above the main window.
        session.setOwnerWindow([window] { return static_cast<uintptr_t>(window->winId()); });
    }
    // The audio device opens once the window shows; then the project asked for,
    // if any, else a new one (the template, if one was saved).
    QMetaObject::invokeMethod(&session, [&session] { session.start(); }, Qt::QueuedConnection);
    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() > 1) {
        const QString path = arguments.at(1);
        QMetaObject::invokeMethod(&session, [&session, path] { session.openProject(path); }, Qt::QueuedConnection);
    } else if (session.hasTemplate()) {
        QMetaObject::invokeMethod(&session, [&session] { session.newProject(); }, Qt::QueuedConnection);
    }
    const int result = app.exec();
    session.shutdown();
    return result;
}
