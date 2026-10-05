import QtQuick
import QtQuick.Templates as T
import SUBstation

// A thin scroll position (combo box lists, long menus): the scroll bar's handle
// colour, shown while scrolling.
T.ScrollIndicator {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: 2

    contentItem: Rectangle {
        implicitWidth: 4
        implicitHeight: 4
        radius: 2
        color: Theme.surfaceHover
        visible: control.size < 1.0
        opacity: control.active ? 1.0 : 0.0

        Behavior on opacity {
            NumberAnimation {
                duration: 200
            }
        }
    }
}
