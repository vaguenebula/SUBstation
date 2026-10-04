import QtQuick
import QtQuick.Templates as T
import SUBstation

// A frame: a BORDER line round what it holds.
T.Frame {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    padding: 8

    background: Rectangle {
        color: "transparent"
        border.color: Theme.border
        radius: 4
    }
}
