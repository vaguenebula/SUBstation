import QtQuick
import QtQuick.Controls
import QtQuick.Templates as T
import SUBstation

// A push button coloured by its `role`, as the old stylesheet's
// QPushButton[role=...] rules (Theme.buttonStyle): "" (plain), "activator",
// "solo", "play", "record", "arm", "re-enable", "tool", "flat", "small" or
// "device-header". It never takes the keyboard focus, so Space stays
// play/stop. `iconName` shows one of the icons (image://icons, 14 px) before
// the text; `tooltip` shows under the mouse. `automation` marks a switch that
// can be automated with a dot in its top right corner, as knobs have: "on"
// (red) while its automation plays, "off" (grey) while overridden.
T.Button {
    id: control

    property string role: ""
    property string automation: ""
    property string iconName: ""
    property var iconColor: undefined  // the icon's own colour
    property real iconSize: Theme.iconSize
    property string tooltip: ""
    // Drawn checked (the style's Button also lights up when highlighted).
    property bool lit: checked
    readonly property var look: Theme.buttonStyle(role, hovered, down, lit, enabled)

    focusPolicy: Qt.NoFocus
    hoverEnabled: true
    spacing: 4
    font.pointSize: look.pointSize
    font.weight: look.weight
    leftPadding: look.paddingH + look.border
    rightPadding: look.paddingH + look.border
    topPadding: look.paddingV + look.border
    bottomPadding: look.paddingV + look.border
    implicitWidth: Math.max(look.minWidth, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(look.minHeight, implicitContentHeight + topPadding + bottomPadding)

    background: ButtonBackground {
        look: control.look
    }
    contentItem: ButtonContent {
        control: control
    }

    Rectangle {
        objectName: "automationDot"
        visible: control.automation !== ""
        z: 2
        width: 5
        height: 5
        radius: 2.5
        x: control.width - width - 1
        y: 1
        color: Theme.automationColor(control.automation)
    }

    ToolTip.visible: tooltip !== "" && hovered && !down
    ToolTip.text: tooltip
    ToolTip.delay: 700
}
