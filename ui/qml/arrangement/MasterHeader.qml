import QtQuick
import QtQuick.Controls
import SUBstation

// The master's header: its volume, pan and meter on the right; and while its
// automation shows, its automation choosers (and those of the lanes below it).
// Click it to select the master: the device view shows its effects.
TrackHeaderItem {
    id: header

    property ArrangementMenu menu: null

    readonly property int masterHeight: 40

    trackId: "master"
    meter: meterItem

    // Over "Master": what its effects do to the mix (its label, then its effects). Hovering only.
    Item {
        objectName: "nameHover"
        x: 12
        width: 60
        height: header.masterHeight

        HoverHandler {
            id: nameHover
        }
        ToolTip.visible: nameHover.hovered && header.nameToolTip !== ""
        ToolTip.text: header.nameToolTip
        ToolTip.delay: 700
    }

    Meter {
        id: meterItem
        objectName: "meter"
        x: header.width - 12
        y: 4
        width: 8
        height: header.masterHeight - 8
    }
    ValueBox {
        id: volume
        objectName: "volume"
        x: header.width - 12 - 6 - 76
        y: (header.masterHeight - 20) / 2
        width: 76
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
        ToolTip.text: qsTr("Master Volume")
        ToolTip.delay: 700
    }
    Knob {
        id: pan
        objectName: "pan"
        x: header.width - 12 - 6 - 76 - 32
        y: (header.masterHeight - 26) / 2
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
    AutomationChoosers {
        objectName: "choosers"
        anchors.fill: parent
        header: header
        menu: header.menu
        mainRect: Qt.rect(12, header.masterHeight + 6, header.width - 12 - 18, 18)
        laneLeft: 12
        laneRight: header.width - 18
    }

    onMenuRequested: (entries, pos) => header.menu.show(entries, header, header, pos.x, pos.y)
}
