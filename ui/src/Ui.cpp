#include "Ui.h"

#include "theme/Icons.h"
#include "theme/Theme.h"

#include <QGuiApplication>
#include <QIcon>
#include <QPalette>
#include <QPixmap>
#include <QQmlEngine>
#include <QQuickStyle>

namespace sub::ui {

void setUpApplication() {
    QQuickStyle::setStyle(QStringLiteral("SUBstation.Style"));
    QGuiApplication::setFont(uiFont());

    // theme.apply()'s palette: what controls without a look of their own fall back to.
    QPalette palette;
    const std::pair<QPalette::ColorRole, QColor> roles[] = {
        {QPalette::Window, Theme::kWindow},          {QPalette::WindowText, Theme::kText},
        {QPalette::Base, Theme::kPanel},             {QPalette::AlternateBase, Theme::kPanelAlt},
        {QPalette::Text, Theme::kText},              {QPalette::Button, Theme::kSurface},
        {QPalette::ButtonText, Theme::kText},        {QPalette::Highlight, Theme::kAccent},
        {QPalette::HighlightedText, Theme::kAccentText}, {QPalette::ToolTipBase, Theme::kPanelAlt},
        {QPalette::ToolTipText, Theme::kText},       {QPalette::PlaceholderText, Theme::kTextDim},
        {QPalette::Link, Theme::kAccent},
    };
    for (const auto& [role, color] : roles)
        palette.setColor(role, color);
    for (QPalette::ColorRole role : {QPalette::Text, QPalette::ButtonText, QPalette::WindowText})
        palette.setColor(QPalette::Disabled, role, Theme::kTextDisabled);
    QGuiApplication::setPalette(palette);

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
