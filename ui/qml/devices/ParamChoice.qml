import QtQuick
import QtQuick.Controls
import SUBstation

// A small drop-down bound to a built-in device's list parameter: the value's
// name (and its icon, or only its icon) with an arrow; a click opens the list
// to choose from (one undo step each), right-click its parameter menu. It
// touches the parameter when pressed, and always shows it as it is now.
//
//   ParamChoice { param: params["filter_type"]; icons: ["filter_lowpass", ...]; iconOnly: true }
Item {
    id: control

    property DeviceParam param: null
    property var icons: []          // an icon name per value ("" or none: no icon)
    property bool iconOnly: false   // the face shows the icon alone (the list still names them)
    property var labels: null       // names to show instead of the parameter's own
    property string tooltip: ""
    readonly property alias button: button
    readonly property alias menu: menu
    readonly property int index: param ? param.index : 0
    readonly property var names: labels || (param ? param.labels : [])

    implicitWidth: button.implicitWidth
    implicitHeight: 16

    // Chooses value `index` (as the list does).
    function choose(index) {
        if (param)
            param.set(index)
    }

    ParamArea {
        anchors.fill: parent
        param: control.param
    }

    RoleButton {
        id: button
        anchors.fill: parent
        role: "small"
        iconSize: 12
        iconName: control.icons.length > control.index ? control.icons[control.index] : ""
        text: control.iconOnly ? "" : (control.names[control.index] || "")
        tooltip: control.tooltip
        checkable: false
        rightPadding: 13
        onPressed: if (control.param) control.param.touch()
        onClicked: menu.show()

        Icon {
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            name: "fold"
            size: 7
            color: button.look.text
        }
    }

    DynamicMenu {
        id: menu
        objectName: "choiceMenu"

        function show() {
            clear()
            for (let i = 0; i < control.names.length; ++i) {
                const index = i
                entry(control.names[i], () => control.choose(index), control.index === i)
            }
            popup(control, 0, control.height)
        }
    }
}
