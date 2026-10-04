// SUBstation: starts the application layer and shows the Qt Quick UI.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QtQml/qqmlextensionplugin.h>

#include "AppInfo.h"

Q_IMPORT_QML_PLUGIN(SUBstationPlugin)

int main(int argc, char* argv[]) {
    QGuiApplication::setOrganizationName(QString::fromLatin1(sub::app::kOrganization));
    QGuiApplication::setApplicationName(QString::fromLatin1(sub::app::kAppName));
    QGuiApplication app(argc, argv);
    QQmlApplicationEngine qml;
    qml.addImportPath(QStringLiteral("qrc:/qt/qml"));
    QObject::connect(&qml, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    qml.load(QUrl(QStringLiteral("qrc:/qt/qml/SUBstation/qml/Main.qml")));
    return app.exec();
}
