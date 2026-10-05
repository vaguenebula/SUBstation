import QtQuick
import QtQuick.Controls
import SUBstation

// A built-in device's parameter, as a cell of the device view shows it: its
// name on top (elided, the whole name in the tooltip), then a knob with its
// value under it, or, for a parameter that chooses between named values, a
// list. The knob turns logarithmically where the engine says so, is drawn from
// the middle for a range across 0 without a unit (or in semitones or cents),
// moves in whole steps for a stepped parameter, shows the automation dot (red
// while automation plays, grey while overridden) and follows the automation as
// it plays. Pressing it (or the cell) shows its automation in the arrangement;
// right-click for its menu (ParamMenu). Edits go through the editor, one undo
// step per drag.
//
//   DeviceParamKnob { trackId: track; deviceId: device; paramId: "threshold" }
Item {
    id: cell

    property string trackId
    property string deviceId
    property string paramId
    readonly property DeviceParam param: parameter
    property real cellWidth: 84  // PARAM_WIDTH
    property real knobSize: 34   // KNOB_SIZE
    readonly property alias knob: knob
    readonly property alias list: list
    readonly property alias menu: area.menu

    implicitWidth: cellWidth
    implicitHeight: column.implicitHeight

    DeviceParam {
        id: parameter
        session: Session
        trackId: cell.trackId
        deviceId: cell.deviceId
        paramId: cell.paramId
    }

    ParamArea {
        id: area
        anchors.fill: parent
        param: parameter
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
            visible: !parameter.isList
            anchors.horizontalCenter: parent.horizontalCenter
            width: cell.knobSize
            height: cell.knobSize
            from: parameter.minimum
            to: parameter.maximum
            defaultValue: parameter.defaultValue
            bipolar: parameter.bipolar
            logScale: parameter.logScale
            step: parameter.steps > 0 ? 1 : 0
            value: parameter.value
            automation: parameter.automation
            formatter: v => parameter.format(v)
            onMoved: (v, key) => parameter.set(v, key)
            onTouched: parameter.touch()
        }

        Text {
            id: readout
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
            visible: parameter.isList
            width: cell.cellWidth
            focusPolicy: Qt.NoFocus
            model: parameter.labels
            currentIndex: parameter.index
            onActivated: index => parameter.set(index)
            onPressedChanged: if (pressed) parameter.touch()
        }
    }
}
