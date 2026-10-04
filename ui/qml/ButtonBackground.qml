import QtQuick
import SUBstation

// A push button's background from its look (Theme.buttonStyle): its colour,
// BORDER line and corners, as the old stylesheet drew QPushButton.
Rectangle {
    required property var look

    color: look.background
    radius: look.radius
    border.width: look.border
    border.color: Theme.border
}
