import QtQuick
import QtQuick.Controls
import SUBstation

// The note tools (note_tools.py): Legato, timing ×2 and ÷2, Quantize and
// Humanize, in a small rounded bar that glides in over the note grid next to a
// group of notes selected by dragging a rubber band or with Ctrl+A (the roll's
// toolsShown). Centred above the notes (below them when there is no room),
// kept inside the grid; it fades in rising 8 px (180 ms, ease out) and fades
// out sinking back (120 ms), a reversed animation going on from where the last
// one left off. Each tool is one undo step, on the selected notes (all of them
// when none are). Its buttons never take the focus, so the notes keep the
// keyboard; it keeps its clicks to itself (the grid is underneath), while wheel
// turns outside its boxes still scroll the grid.
//
// A child of the NoteGrid: its coordinates are the grid's.
Rectangle {
    id: tools

    required property PianoRoll roll

    readonly property bool shown: roll.toolsShown  // where it is heading
    property real progress: 0  // 0 (gone) .. 1 (in place)
    readonly property int gap: 8  // between the bar and the notes
    readonly property int margin: 4  // between the bar and the grid's edges
    readonly property int rise: 8  // pixels the bar rises as it fades in
    readonly property int showMs: 180
    readonly property int hideMs: 120

    // Centred above the notes, or below them when there is no room above, inside the grid.
    readonly property point target: {
        const area = roll.toolsArea
        const w = parent ? parent.width : 0
        const h = parent ? parent.height : 0
        let x = area.x + area.width / 2 - width / 2
        let y = area.y - gap - height
        if (y < margin)
            y = area.y + area.height + gap
        x = Math.max(margin, Math.min(w - width - margin, x))
        y = Math.max(margin, Math.min(h - height - margin, y))
        return Qt.point(Math.round(x), Math.round(y))
    }

    x: target.x
    y: target.y + Math.round((1 - progress) * rise)
    width: row.implicitWidth + 8 + 5
    height: row.implicitHeight + 8
    radius: 5
    color: Theme.panelAlt
    border.color: Theme.border
    antialiasing: true
    opacity: progress
    visible: shown || progress > 0

    onShownChanged: {
        animation.stop()
        // From wherever a reversed animation left off, taking the rest of its time.
        const remaining = shown ? 1 - progress : progress
        animation.from = progress
        animation.to = shown ? 1 : 0
        animation.duration = Math.max(1, Math.round((shown ? showMs : hideMs) * remaining))
        animation.easing.type = shown ? Easing.OutCubic : Easing.InCubic
        animation.start()
    }

    NumberAnimation {
        id: animation
        target: tools
        property: "progress"
    }

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

    component ToolsButton: RoleButton {
        anchors.verticalCenter: parent ? parent.verticalCenter : undefined
    }

    Row {
        id: row
        x: 8
        y: 4
        spacing: 5

        Label {
            objectName: "noteCount"
            anchors.verticalCenter: parent.verticalCenter
            text: qsTr("%1 note%2").arg(tools.roll.toolsCount).arg(tools.roll.toolsCount !== 1 ? "s" : "")
            color: Theme.textDim
            font: Theme.uiFont(8)
        }
        Separator {}
        ToolsButton {
            objectName: "legato"
            text: qsTr("Legato")
            tooltip: qsTr("Make the selected notes last until the next one starts;\nthe last ones until the next note after them, or the end of the clip")
            onClicked: tools.roll.legato()
        }
        Separator {}
        ToolsButton {
            objectName: "doubleTime"
            text: "×2"
            tooltip: qsTr("Double the selected notes' timing: they spread out\nfrom the first one and last twice as long")
            onClicked: tools.roll.scaleTime(2.0)
        }
        ToolsButton {
            objectName: "halfTime"
            text: "÷2"
            tooltip: qsTr("Halve the selected notes' timing: they draw in\ntoward the first one and last half as long")
            onClicked: tools.roll.scaleTime(0.5)
        }
        Separator {}
        ToolsButton {
            objectName: "quantize"
            text: qsTr("Quantize")
            tooltip: qsTr("Move the selected notes' starts onto the grid chosen next to it (Ctrl+U)")
            onClicked: tools.roll.quantize()
        }
        ComboBox {
            id: grid
            objectName: "quantizeGrid"
            anchors.verticalCenter: parent.verticalCenter
            focusPolicy: Qt.NoFocus
            model: tools.roll.quantizeGrids
            currentIndex: tools.roll.quantizeGrids.indexOf(tools.roll.quantizeGrid)
            onActivated: index => tools.roll.quantizeGrid = tools.roll.quantizeGrids[index]
            ToolTip.visible: hovered && !popup.visible
            ToolTip.text: qsTr("The grid Quantize moves notes onto (T: triplets)")
            ToolTip.delay: 700
        }
        ValueBox {
            objectName: "quantizeAmount"
            anchors.verticalCenter: parent.verticalCenter
            from: 0
            to: 100
            step: 1
            decimals: 0
            sampleText: "100 %"
            formatter: v => v.toFixed(0) + " %"
            value: tools.roll.quantizeAmount
            onMoved: value => tools.roll.quantizeAmount = value
            ToolTip.visible: hovered && !dragging
            ToolTip.text: qsTr("Quantize amount: how far notes move toward the grid (100 %: all the way)")
            ToolTip.delay: 700
        }
        Separator {}
        ToolsButton {
            objectName: "humanize"
            text: qsTr("Humanize")
            tooltip: qsTr("Nudge the selected notes' timing and velocity at random,\nas a player would")
            onClicked: tools.roll.humanize()
        }
        ValueBox {
            objectName: "humanizeAmount"
            anchors.verticalCenter: parent.verticalCenter
            from: 0
            to: 100
            step: 1
            decimals: 0
            sampleText: "100 %"
            formatter: v => v.toFixed(0) + " %"
            value: tools.roll.humanizeAmount
            onMoved: value => tools.roll.humanizeAmount = value
            ToolTip.visible: hovered && !dragging
            ToolTip.text: qsTr("Humanize amount. At 100 % notes move by up to a 32nd note (%1 beats)\neither way and velocities change by up to %2.").arg(tools.roll.humanizeBeats).arg(tools.roll.humanizeVelocity)
            ToolTip.delay: 700
        }
    }
}
