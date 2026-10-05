import QtQuick
import QtQuick.Templates as T
import SUBstation

// QTabBar::tab: PANEL with a BORDER line, padding 5 14; the current one SURFACE.
T.TabButton {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    topPadding: 6
    bottomPadding: 6
    leftPadding: 15
    rightPadding: 15
    focusPolicy: Qt.NoFocus
    hoverEnabled: true
    width: implicitWidth  // each tab as wide as its text, as QTabBar's

    contentItem: Text {
        text: control.text
        font: control.font
        color: control.enabled ? Theme.text : Theme.textDisabled
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        color: control.checked ? Theme.surface : Theme.panel
        border.color: Theme.border
    }
}
