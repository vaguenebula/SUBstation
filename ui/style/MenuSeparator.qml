import QtQuick
import QtQuick.Templates as T
import SUBstation

// QMenu::separator: a 1 px BORDER line, 3 px above and below, 6 px in from the sides.
T.MenuSeparator {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    topPadding: 3
    bottomPadding: 3
    leftPadding: 6
    rightPadding: 6

    contentItem: Rectangle {
        implicitWidth: 40
        implicitHeight: 1
        color: Theme.border
    }
}
