import QtQuick
import QtQuick.Controls
import SUBstation

// A plug-in's parameter in its device's generic editor (device_widgets.py's
// PluginDeviceWidget._param_widget): its name on top, then a knob with the
// plug-in's own text for its value under it, or a list for one that chooses
// between named values. The knob moves in whole steps for a stepped
// parameter, and is drawn from the middle when its default is the middle of
// its range; it shows the automation dot (the plug-in reports its values as
// automation plays). Pressing it shows its automation in the arrangement;
// right-click for its menu (ParamMenu: automation, and in a rack the macros).
// Edits go through the editor with the value before, one undo step per drag.
Item {
    id: cell

    property string trackId
    property string deviceId
    property int index: -1
    readonly property PluginParam param: parameter
    property real cellWidth: 84  // PARAM_WIDTH
    property real knobSize: 34   // KNOB_SIZE
    readonly property alias knob: knob
    readonly property alias list: list
    readonly property alias menu: area.menu

    implicitWidth: cellWidth
    implicitHeight: column.implicitHeight

    PluginParam {
        id: parameter
        session: Session
        trackId: cell.trackId
        deviceId: cell.deviceId
        index: cell.index
    }
    // The same parameter as the menu sees it (its automation, its macro).
    DeviceParam {
        id: target
        session: Session
        trackId: cell.trackId
        deviceId: cell.deviceId
        paramId: parameter.paramId
    }

    ParamArea {
        id: area
        anchors.fill: parent
        param: target
    }

    Column {
        id: column
        width: cell.cellWidth
        spacing: 1

        Text {
            id: name
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: parameter.name
            color: Theme.textDim
            font: Theme.uiFont(8)

            HoverHandler {
                id: nameHover
            }
            ToolTip.visible: nameHover.hovered && name.truncated
            ToolTip.text: parameter.name
            ToolTip.delay: 700
        }

        Knob {
            id: knob
            objectName: "knob"
            visible: !parameter.isList
            anchors.horizontalCenter: parent.horizontalCenter
            width: cell.knobSize
            height: cell.knobSize
            from: parameter.minimum
            to: parameter.maximum
            defaultValue: parameter.defaultValue
            bipolar: parameter.bipolar
            step: parameter.steps > 0 ? 1 : 0
            value: parameter.value
            automation: parameter.automation
            formatter: v => parameter.format(v)
            onMoved: (v, key) => parameter.set(v, key)
            onTouched: parameter.touch()
        }

        Text {
            id: readout
            objectName: "readout"
            visible: !parameter.isList
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: parameter.text
            color: Theme.text
            font: Theme.uiFont(8)
        }

        ComboBox {
            id: list
            objectName: "list"
            visible: parameter.isList
            width: cell.cellWidth
            focusPolicy: Qt.NoFocus
            model: parameter.labels
            currentIndex: parameter.listIndex
            onActivated: index => parameter.set(index)
            onPressedChanged: if (pressed) parameter.touch()
        }
    }
}
