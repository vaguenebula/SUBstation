import QtQuick
import QtQuick.Controls
import SUBstation

// One of a rack's macros (rack_view.py's MacroPanel cell): "Macro N" over its
// knob (0..1), the name lit while something is mapped to it; the knob's
// tooltip lists what it moves. Turning it sets them (one undo step per drag);
// right-click to unmap one of them.
Item {
    id: cell

    property string trackId
    property string rackId
    property int macroIndex: 0
    required property var panel
    readonly property RackMacro macro: rackMacro
    readonly property alias knob: knob

    implicitWidth: 44  // MACRO_WIDTH
    implicitHeight: column.implicitHeight

    RackMacro {
        id: rackMacro
        session: Session
        trackId: cell.trackId
        rackId: cell.rackId
        index: cell.macroIndex
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onPressed: mouse => cell.panel.showMacroMenu(rackMacro, cell, mouse.x, mouse.y)
    }

    Column {
        id: column
        width: cell.implicitWidth
        spacing: 0

        Text {
            id: name
            objectName: "macroName"
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: rackMacro.name
            color: rackMacro.mapped ? Theme.text : Theme.textDim
            font: Theme.uiFont(7)
        }
        Knob {
            id: knob
            objectName: "knob"
            anchors.horizontalCenter: parent.horizontalCenter
            width: 26  // MACRO_KNOB
            height: 26
            from: 0
            to: 1
            defaultValue: 0
            value: rackMacro.value
            formatter: v => Math.round(v * 100) + " %"
            onMoved: (v, key) => rackMacro.set(v, key)
            ToolTip.text: rackMacro.toolTip
        }
    }
}
