import QtQuick
import QtQuick.Controls
import SUBstation

// The bend bar: while the piano roll is in bend mode (B), a small rounded bar at
// the top right of the note grid with the bend tools: Draw (points on the
// notes' bend curves, as automation's) and Vibrato (V: drag across a note to
// draw vibrato on it), the rate, depth and ramp a new vibrato takes, and Clear
// (the selected notes' bends, every note's with none selected). Its buttons and
// boxes never take the focus, so the notes keep the keyboard; it keeps its
// clicks to itself (the grid is underneath).
//
// A child of the NoteGrid: its coordinates are the grid's.
Rectangle {
    id: bar

    required property PianoRoll roll

    readonly property int margin: 6
    readonly property bool vibrato: roll.bendTool === "vibrato"

    x: parent ? parent.width - width - margin : 0
    y: margin + (Session.harmony.shown ? 18 : 0)  // (below the chords)
    width: row.implicitWidth + 12
    height: row.implicitHeight + 8
    radius: 5
    color: Theme.panelAlt
    border.color: Theme.border
    antialiasing: true
    visible: roll.bendMode

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        cursorShape: Qt.ArrowCursor
        onWheel: wheel => wheel.accepted = false
    }

    component Separator: Item {
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
        width: 3
        height: 16

        Rectangle {
            x: 1
            width: 1
            height: parent.height
            color: Theme.surfaceHover
        }
    }

    component Box: ValueBox {
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
        ToolTip.visible: hovered && !dragging && tip !== ""
        ToolTip.delay: 700
        property string tip
        ToolTip.text: tip
    }

    Row {
        id: row
        x: 6
        y: 4
        spacing: 5

        Label {
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Bend")
            color: Theme.textDim
            font: Theme.uiFont(8)
        }
        ToggleButton {
            objectName: "bendDraw"
            anchors.verticalCenter: parent.verticalCenter
            role: "tool"
            iconName: "bend"
            checked: !bar.vibrato
            tooltip: qsTr("Draw bends: click a note's curve to add a point, drag points,\nclick one to delete it, Alt-drag a segment to curve it")
            onToggled: {
                bar.roll.bendTool = "draw"
                checked = Qt.binding(() => !bar.vibrato)
            }
        }
        ToggleButton {
            objectName: "bendVibrato"
            anchors.verticalCenter: parent.verticalCenter
            role: "tool"
            iconName: "vibrato"
            checked: bar.vibrato
            tooltip: qsTr("Draw vibrato (V): drag across a note (up for deeper; hold Shift and drag sideways\nfor its speed, Alt for its ramp), click a note for vibrato to its end,\nclick a vibrato to remove it")
            onToggled: {
                bar.roll.bendTool = "vibrato"
                checked = Qt.binding(() => bar.vibrato)
            }
        }
        Separator {}
        Box {
            objectName: "vibratoRate"
            from: 0.5
            to: 20
            step: 0.1
            decimals: 1
            sampleText: "20.0 Hz"
            formatter: v => v.toFixed(1) + " Hz"
            value: bar.roll.vibratoRate
            onMoved: value => bar.roll.vibratoRate = value
            tip: qsTr("Vibrato rate: how many times a second a new vibrato swings\n(hold Shift and drag sideways as you draw it to change it)")
        }
        Box {
            objectName: "vibratoDepth"
            from: 0.05
            to: 12
            step: 0.05
            decimals: 2
            sampleText: "12.00 st"
            formatter: v => v.toFixed(2) + " st"
            value: bar.roll.vibratoDepth
            onMoved: value => bar.roll.vibratoDepth = value
            tip: qsTr("Vibrato depth: how far a new vibrato swings either way, in semitones\n(drag up as you draw it for more)")
        }
        Box {
            objectName: "vibratoFade"
            from: 0
            to: 100
            step: 1
            decimals: 0
            sampleText: "100 %"
            formatter: v => v.toFixed(0) + " %"
            value: bar.roll.vibratoFade
            onMoved: value => bar.roll.vibratoFade = value
            tip: qsTr("Vibrato ramp: how much of a new vibrato's length it takes to reach its depth\n(hold Alt and drag sideways as you draw it to change it)")
        }
        Separator {}
        RoleButton {
            objectName: "clearBends"
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("Clear")
            tooltip: qsTr("Take away the selected notes' bends and vibrato (every note's, with none selected)")
            onClicked: bar.roll.clearBends()
        }
    }
}
