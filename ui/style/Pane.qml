import QtQuick
import QtQuick.Templates as T
import SUBstation

// A plain panel: WINDOW.
T.Pane {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    padding: 8

    background: Rectangle {
        color: Theme.window
    }
}
