// SUBstation: starts the application layer and shows the Qt Quick UI.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QtQml/qqmlextensionplugin.h>

#include "AppInfo.h"
#include "app/AppTypes.h"
#include "Engine.h"
#include "Ui.h"
#include "session/Session.h"

Q_IMPORT_QML_PLUGIN(SUBstationPlugin)

int main(int argc, char* argv[]) {
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
    // The audio device opens once the window shows.
    QMetaObject::invokeMethod(&session, [&session] { session.start(); }, Qt::QueuedConnection);
    const int result = app.exec();
    session.shutdown();
    return result;
}
