import QtQuick
import QtQuick.Controls
import SUBstation

// A return track's header, in the returns' rows above the master: its letter
// (the activator) and name, solo, volume, pan, its sends (to the other returns)
// and meter; and while its automation shows, its automation choosers (and those
// of the lanes below it). Click it to select it: the device view shows its
// effects.
TrackHeaderItem {
    id: header

    property ArrangementMenu menu: null

    readonly property int nameRow: 22
    // The name row's buttons, as a track's (TrackHeaderItem::kNamePad, kNameButton).
    readonly property int buttonTop: 2
    readonly property int buttonHeight: 16
    readonly property int returnHeight: 52
    readonly property int meterWidth: 8
    readonly property int innerRight: width - meterWidth - 10
    readonly property int innerLeft: 10

    nameRight: activator.x - 4
    meter: meterItem

    Meter {
        id: meterItem
        objectName: "meter"
        x: header.width - header.meterWidth - 4
        y: 4
        width: header.meterWidth
        height: header.returnHeight - 9
    }
    ToggleButton {
        id: solo
        objectName: "solo"
        x: header.innerRight - 22
        y: header.buttonTop
        width: 22
        height: header.buttonHeight
        role: "solo"
        text: "S"
        tooltip: header.soloToolTip
        checked: header.solo
        onToggled: header.soloClicked(checked)
    }
    ToggleButton {
        id: activator
        objectName: "activator"
        x: header.innerRight - 22 - 30
        y: header.buttonTop
        width: 28
        height: header.buttonHeight
        role: "activator"
        text: header.letter
        tooltip: qsTr("Track Activator (unmute)")
        checked: !header.mute
        automation: header.activatorAutomation
        onToggled: header.activatorToggled(checked)

        MouseArea {  // its automation's menu
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onPressed: mouse => header.menu.show(header.activatorMenuEntries(), header, activator, mouse.x, mouse.y)
        }
    }
    ValueBox {
        id: volume
        objectName: "volume"
        x: header.innerLeft
        y: header.nameRow + 4
        width: 72
        height: 20
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
        ToolTip.text: qsTr("Return Volume")
        ToolTip.delay: 700
    }
    Knob {
        id: pan
        objectName: "pan"
        x: header.innerLeft + 77
        y: header.nameRow + 1
        width: 26
        height: 26
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
    SendKnobs {
        objectName: "sends"
        header: header
        menu: header.menu
        x: header.innerLeft + 108
        y: header.nameRow + 3
        width: header.innerRight - header.innerLeft - 108
        height: 22
    }
    AutomationChoosers {
        objectName: "choosers"
        anchors.fill: parent
        header: header
        menu: header.menu
        mainRect: Qt.rect(header.innerLeft, header.returnHeight + 4, header.innerRight - header.innerLeft, 18)
        laneLeft: header.innerLeft
        laneRight: header.innerRight
    }

    TextField {
        id: rename
        objectName: "rename"
        x: 8
        y: header.buttonTop - 1
        width: activator.x - 12
        height: header.buttonHeight + 2
        visible: header.renaming
        padding: 2
        font: Theme.uiFont(9, true)
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
