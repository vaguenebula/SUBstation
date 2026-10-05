import QtQuick
import SUBstation

// A small button bound to a built-in device's parameter: an on/off switch
// (`choice` -1: lit while the parameter is on, a click switches it) or one of a
// list's values (lit while the parameter is that one, a click chooses it). It
// touches the parameter when pressed, and right-click gives its menu. Its lit
// state always follows the parameter.
Item {
    id: control

    property DeviceParam param: null
    property int choice: -1
    property alias text: button.text
    property alias iconName: button.iconName
    property alias iconSize: button.iconSize
    property alias tooltip: button.tooltip
    property alias role: button.role
    property alias font: button.font
    readonly property alias button: button
    readonly property bool lit: param ? (choice < 0 ? param.value >= 0.5 : param.index === choice) : false
    // Sets the value a click asks for instead of the parameter's set() (an editor's own undo text): value => {}.
    property var setter: null

    implicitWidth: button.implicitWidth
    implicitHeight: 16

    ParamArea {
        anchors.fill: parent
        param: control.param
    }

    RoleButton {
        id: button
        anchors.fill: parent
        role: "small"
        iconSize: 12
        checkable: false
        checked: control.lit
        onPressed: if (control.param) control.param.touch()
        onClicked: {
            const value = control.choice < 0 ? (control.lit ? 0 : 1) : control.choice
            if (control.setter)
                control.setter(value)
            else if (control.param)
                control.param.set(value)
        }
    }
}
