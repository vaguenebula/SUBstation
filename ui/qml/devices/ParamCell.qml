import QtQuick
import QtQuick.Controls
import SUBstation

// A parameter's cell in the device view, as DeviceParamKnob (a built-in
// device's) and PluginParamKnob (a plug-in's) show it: its name on top (elided,
// the whole name in the tooltip), then a knob with its value under it or, for a
// parameter that chooses between named values, a list. Under it all a
// ParamArea: a press touches `menuParam`, a right press opens its menu. The
// cell shows what it is given and says what the user does (moved, chosen,
// touched); the parameter's own type does the rest.
Item {
    id: cell

    property DeviceParam menuParam: null  // what its right-click menu is about
    property string name
    property real minimum: 0
    property real maximum: 1
    property real defaultValue: 0
    property bool bipolar: false
    property bool logScale: false
    property bool stepped: false  // whole steps only
    property real value: 0
    property string text  // the value in its units
    property string automation  // "", "on" or "off"
    property bool isList: false
    property var labels: []  // a list's names
    property int listIndex: 0
    property var formatter: null  // the knob's tooltip: (value) => text
    property real cellWidth: 84  // PARAM_WIDTH
    property real knobSize: 34   // KNOB_SIZE
    readonly property alias knob: knob
    readonly property alias list: list
    readonly property alias menu: area.menu

    signal moved(real value, string gestureKey)
    signal chosen(int index)
    signal touched()

    implicitWidth: cellWidth
    implicitHeight: column.implicitHeight

    ParamArea {
        id: area
        anchors.fill: parent
        param: cell.menuParam
    }

    Column {
        id: column
        width: cell.cellWidth
        spacing: 1

        Text {
            id: nameText
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: cell.name
            color: Theme.textDim
            font: Theme.uiFont(8)

            HoverHandler {
                id: nameHover
            }
            ToolTip.visible: nameHover.hovered && nameText.truncated
            ToolTip.text: cell.name
            ToolTip.delay: 700
        }

        Knob {
            id: knob
            objectName: "knob"
            visible: !cell.isList
            anchors.horizontalCenter: parent.horizontalCenter
            width: cell.knobSize
            height: cell.knobSize
            from: cell.minimum
            to: cell.maximum
            defaultValue: cell.defaultValue
            bipolar: cell.bipolar
            logScale: cell.logScale
            step: cell.stepped ? 1 : 0
            value: cell.value
            automation: cell.automation
            formatter: cell.formatter
            onMoved: (v, key) => cell.moved(v, key)
            onTouched: cell.touched()
        }

        Text {
            id: readout
            objectName: "readout"
            visible: !cell.isList
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: cell.text
            color: Theme.text
            font: Theme.uiFont(8)
        }

        ComboBox {
            id: list
            objectName: "list"
            visible: cell.isList
            width: cell.cellWidth
            focusPolicy: Qt.NoFocus
            model: cell.labels
            currentIndex: cell.listIndex
            onActivated: index => cell.chosen(index)
            onPressedChanged: if (pressed) cell.touched()
        }
    }
}
