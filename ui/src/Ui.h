#pragma once

// Setting up the Qt Quick UI, for main.cpp and the tests.

class QQmlEngine;

namespace sub::ui {

// Once, after the QGuiApplication and before the first QML engine: selects the
// Qt Quick Controls style (SUBstation.Style: the old stylesheet's look), and
// sets the application's font (Theme's UI font), palette and window icon, as
// theme.apply() did.
void setUpApplication();

// On every QML engine: the import path of the built-in modules and the icon
// provider (image://icons/...).
void setUpEngine(QQmlEngine& engine);

}  // namespace sub::ui
