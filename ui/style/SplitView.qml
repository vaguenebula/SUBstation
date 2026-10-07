import QtQuick
import QtQuick.Templates as T
import SUBstation

// The splitters between the window's sections: 4 px handles of BORDER.
T.SplitView {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    handle: Rectangle {
        implicitWidth: control.orientation === Qt.Horizontal ? 4 : control.width
        implicitHeight: control.orientation === Qt.Horizontal ? control.height : 4
        color: Theme.border
    }
}
