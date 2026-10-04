import QtQuick
import QtQuick.Templates as T
import SUBstation

// QProgressBar (the status bar's plug-ins loading): SURFACE with a BORDER
// line, filled with ACCENT; a moving block while indeterminate.
T.ProgressBar {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: 1

    contentItem: Item {
        implicitWidth: 120
        implicitHeight: 8
        clip: true

        Rectangle {
            width: control.indeterminate ? parent.width / 4 : control.visualPosition * parent.width
            height: parent.height
            color: Theme.accent

            NumberAnimation on x {
                running: control.indeterminate && control.visible
                loops: Animation.Infinite
                from: -control.availableWidth / 4
                to: control.availableWidth
                duration: 1200
            }
        }
    }

    background: Rectangle {
        implicitWidth: 120
        implicitHeight: 10
        color: Theme.surface
        border.color: Theme.border
        radius: 2
    }
}
