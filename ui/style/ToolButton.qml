import QtQuick
import QtQuick.Templates as T
import SUBstation

// A tool button: the "tool" role's look (the transport bar's buttons), never
// taking the focus. `iconName` shows one of the icons (image://icons).
T.ToolButton {
    id: control

    property string iconName: ""
    property var iconColor: undefined
    property real iconSize: Theme.iconSize
    readonly property var look: Theme.buttonStyle("tool", hovered, down, checked || highlighted, enabled)

    focusPolicy: Qt.NoFocus
    hoverEnabled: true
    spacing: 4
    font.pointSize: look.pointSize
    leftPadding: look.paddingH + look.border
    rightPadding: look.paddingH + look.border
    topPadding: look.paddingV + look.border
    bottomPadding: look.paddingV + look.border
    implicitWidth: Math.max(look.minWidth, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(look.minHeight, implicitContentHeight + topPadding + bottomPadding)

    background: ButtonBackground { look: control.look }
    contentItem: ButtonContent { control: control }
}
