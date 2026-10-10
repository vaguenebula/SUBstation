import QtQuick
import QtQuick.Controls
import SUBstation

// A device editor's labelled knob: its name above, the knob (a ParamKnob, bound to the parameter:
// its value as it is now, set undoably, touched when pressed, its menu on right-click) and its value
// below, dimmed with the knob while disabled. The tooltip, if any, shows after a moment.
//
//   EditorKnob { objectName: "drive"; param: p.get("drive"); title: qsTr("Drive"); tooltip: qsTr("...") }
Column {
    id: cell

    property DeviceParam param: null
    property string title: ""
    property string tooltip: ""
    property real size: 34
    property real step: 0
    property alias color: knob.color
    // Value -> text for the readout (the parameter's own by default).
    property var formatter: null
    readonly property alias knob: knob

    width: Math.max(size + 16, 52)
    spacing: 1
    opacity: enabled ? 1 : 0.55
    Behavior on opacity {
        NumberAnimation {
            duration: 120
        }
    }

    EditorCaption {
        width: parent.width
        text: cell.title
        elide: Text.ElideRight
    }
    ParamKnob {
        id: knob
        anchors.horizontalCenter: parent.horizontalCenter
        size: cell.size
        step: cell.step
        param: cell.param
    }
    EditorReadout {
        width: parent.width
        text: cell.param ? (cell.formatter ? cell.formatter(cell.param.value) : cell.param.text) : ""
        elide: Text.ElideRight
    }

    HoverHandler {
        id: hover
    }
    ToolTip.visible: cell.tooltip !== "" && hover.hovered && !knob.knob.dragging
    ToolTip.delay: 700
    ToolTip.text: cell.tooltip
}
