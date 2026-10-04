import QtQuick
import QtQuick.Templates as T
import SUBstation

// QToolTip: PANEL_ALT with a BORDER line, 3 px of padding; below what it is about.
T.ToolTip {
    id: control

    x: parent ? (parent.width - implicitWidth) / 2 : 0
    y: parent ? parent.height + 4 : 0

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    margins: 6
    padding: 4
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent | T.Popup.CloseOnReleaseOutsideParent

    contentItem: Text {
        text: control.text
        font: control.font
        color: Theme.text
    }

    background: Rectangle {
        color: Theme.panelAlt
        border.color: Theme.border
    }
}
