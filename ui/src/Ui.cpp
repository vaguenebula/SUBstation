#include "Ui.h"

#include "theme/Icons.h"
#include "theme/Theme.h"

#include <QGuiApplication>
#include <QIcon>
#include <QPixmap>
#include <QQmlEngine>
#include <QQuickStyle>

namespace sub::ui {

void setUpApplication() {
    QQuickStyle::setStyle(QStringLiteral("SUBstation.Style"));
    QGuiApplication::setFont(uiFont());

    // The theme last chosen (Look and Feel), and its palette: what controls
    // without a look of their own fall back to.
    Theme::apply(Theme::savedName());
    QGuiApplication::setPalette(Theme::qtPalette());

    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256})
        icon.addPixmap(QPixmap::fromImage(Icons::image(QStringLiteral("app_icon"), size)));
    QGuiApplication::setWindowIcon(icon);
}

void setUpEngine(QQmlEngine& engine) {
    engine.addImportPath(QStringLiteral("qrc:/qt/qml"));
    if (!engine.imageProvider(QStringLiteral("icons")))
        engine.addImageProvider(QStringLiteral("icons"), new IconProvider);  // the engine owns it
}

}  // namespace sub::ui
