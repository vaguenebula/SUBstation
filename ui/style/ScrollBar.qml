import QtQuick
import QtQuick.Templates as T
import SUBstation

// QScrollBar: 12 px of PANEL with a rounded SURFACE_HOVER handle 2 px inside
// it (lighter under the mouse), at least 24 px long, no arrows. Shown whenever
// there is something to scroll, as the widgets' were.
T.ScrollBar {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    padding: 2
    visible: policy === T.ScrollBar.AlwaysOn || (policy === T.ScrollBar.AsNeeded && size < 1.0)
    minimumSize: {
        const length = orientation === Qt.Horizontal ? width : height
        return length > 0 ? Math.min(1.0, 24 / length) : 0
    }

    contentItem: Rectangle {
        implicitWidth: Theme.scrollBarWidth - 4
        implicitHeight: Theme.scrollBarWidth - 4
        radius: 4
        color: control.pressed || handleHover.hovered ? Theme.scrollHandleHover : Theme.surfaceHover

        HoverHandler {
            id: handleHover
        }
    }

    background: Rectangle {
        implicitWidth: Theme.scrollBarWidth
        implicitHeight: Theme.scrollBarWidth
        color: Theme.panel
    }
}
