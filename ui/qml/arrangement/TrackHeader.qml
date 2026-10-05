import QtQuick
import QtQuick.Controls
import SUBstation

// A track's header (a group's too), right of its lane: the meter on the right;
// the activator (mute, labelled with the track's number), solo and arm (not for
// a group: it records nothing) on the name row; on the second row (from 48 px)
// volume, pan, the input and the monitoring; then the send knobs (while there
// are returns), then the automation choosers in its own lane and in each lane
// below. What it paints, its mouse handling and what its controls do are
// TrackHeaderItem's. Everything below the name row starts after the group
// bands and ends where the buttons above do.
TrackHeaderItem {
    id: header

    property ArrangementMenu menu: null

    readonly property int nameRow: 22
    readonly property int meterWidth: 8
    readonly property int armWidth: 20
    readonly property int innerRight: width - meterWidth - 10
    readonly property int innerLeft: 10 + indent
    readonly property bool secondRow: mainHeight >= 48
    readonly property bool hasSends: sends.length > 0
    readonly property int chooserRow: 52 + (hasSends ? 24 : 0)

    nameRight: activator.x - 4
    meter: meterItem

    Meter {
        id: meterItem
        objectName: "meter"
        x: header.width - header.meterWidth - 4
        y: 4
        width: header.meterWidth
        height: Math.max(8, header.mainHeight - 9)
    }

    ToggleButton {
        id: arm
        objectName: "arm"
        x: header.innerRight - 18
        y: 4
        width: 18
        height: 17
        visible: header.records
        role: "arm"
        text: "●"
        tooltip: qsTr("Arm Recording; Ctrl-click to arm it along with others")
        checked: header.armed
        onToggled: header.armClicked(checked)
    }
    ToggleButton {
        id: solo
        objectName: "solo"
        x: header.innerRight - header.armWidth - 22
        y: 4
        width: 22
        height: 17
        role: "solo"
        text: "S"
        tooltip: header.soloToolTip
        checked: header.solo
        onToggled: header.soloClicked(checked)
    }
    ToggleButton {
        id: activator
        objectName: "activator"
        x: header.innerRight - header.armWidth - 22 - 30
        y: 4
        width: 28
        height: 17
        role: "activator"
        text: String(header.number)
        tooltip: qsTr("Track Activator (unmute)")
        checked: !header.mute
        onToggled: header.activatorToggled(checked)
    }

    ValueBox {
        id: volume
        objectName: "volume"
        x: header.innerLeft
        y: header.nameRow + 4
        width: 72
        height: 20
        visible: header.secondRow
        from: -70
        to: 6
        step: 0.25
        decimals: 1
        sampleText: "-70.0 dB"
        defaultValue: 0
        wheel: false
        formatter: v => header.formatDb(v)
        value: header.volume
        automation: header.volumeAutomation
        onMoved: (value, gestureKey) => header.setVolume(value, gestureKey, volume.relative)
        onTouched: header.touchVolume()

        HoverHandler {
            id: volumeHover
        }
        ToolTip.visible: volumeHover.hovered && !volume.dragging
        ToolTip.text: qsTr("Track Volume (drag; select and type a number; double-click to reset)")
        ToolTip.delay: 700
    }
    Knob {
        id: pan
        objectName: "pan"
        x: header.innerLeft + 77
        y: header.nameRow + 1
        width: 26
        height: 26
        visible: header.secondRow
        from: -1
        to: 1
        defaultValue: 0
        bipolar: true
        wheel: false
        formatter: v => header.formatPan(v)
        parser: text => header.parsePan(text)
        value: header.pan
        automation: header.panAutomation
        onMoved: (value, gestureKey) => header.setPan(value, gestureKey, pan.relative)
        onTouched: header.touchPan()
    }
    RoleButton {
        id: input
        objectName: "input"
        x: header.innerLeft + 108
        y: header.nameRow + 4
        width: Math.max(20, monitor.x - 4 - x)
        height: 20
        visible: header.secondRow && header.records
        role: "small"
        text: header.inputText
        tooltip: header.inputToolTip
        contentItem: Text {
            text: input.text
            font: input.font
            color: input.look.text
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        onClicked: header.menu.show(header.inputMenuEntries(), header, input, 0, input.height)
    }
    RoleButton {
        id: monitor
        objectName: "monitor"
        x: header.innerRight - width
        y: header.nameRow + 4
        width: Math.max(40, implicitWidth + 4)
        height: 20
        visible: header.secondRow && header.records
        role: "small"
        text: header.monitorText
        tooltip: header.monitorToolTip
        onClicked: header.menu.show(header.monitorMenuEntries(), header, monitor, 0, monitor.height)
    }

    // The sends, below volume and pan (and the automation choosers below them).
    SendKnobs {
        objectName: "sends"
        header: header
        menu: header.menu
        x: header.innerLeft
        y: 52
        width: header.innerRight - header.innerLeft
        height: 22
        visible: header.secondRow && header.hasSends && header.mainHeight >= 52 + 24 - 2
    }

    AutomationChoosers {
        objectName: "choosers"
        anchors.fill: parent
        header: header
        menu: header.menu
        mainRect: Qt.rect(header.innerLeft, header.chooserRow, header.innerRight - header.innerLeft, 18)
        laneLeft: header.innerLeft
        laneRight: header.innerRight
    }

    TextField {
        id: rename
        objectName: "rename"
        x: header.nameLeft - 2
        y: 2
        width: activator.x - x - 4
        height: header.nameRow - 2
        visible: header.renaming
        padding: 2
        font: Theme.uiFont(9)
        onVisibleChanged: {
            if (visible) {
                text = header.name
                selectAll()
                forceActiveFocus()
            }
        }
        onEditingFinished: header.finishRename(text)
        onActiveFocusChanged: if (!activeFocus && visible) header.finishRename(text)
    }

    onMenuRequested: (entries, pos) => header.menu.show(entries, header, header, pos.x, pos.y)
}
