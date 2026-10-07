import QtQuick
import QtQuick.Templates as T
import SUBstation

// QMenuBar::item: padding 4 10, SURFACE (rounded) while open or under the mouse.
T.MenuBarItem {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    topPadding: 4
    bottomPadding: 4
    leftPadding: 10
    rightPadding: 10
    hoverEnabled: true

    contentItem: Text {
        text: Theme.withoutMnemonics(control.text)
        font: control.font
        color: control.enabled ? Theme.text : Theme.textDisabled
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: Theme.radius
        color: control.down || control.highlighted ? Theme.surface : "transparent"
    }
}
