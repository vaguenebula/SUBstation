import QtQuick
import QtQuick.Controls
import SUBstation

// The master's header, laid out as a track's (TrackHeader) and named Main, as
// in Ableton 12: its name bar; in the In/Out column the audio device's outputs
// it plays on (Main Out); its volume, pan and meter; and while its automation
// shows, its automation choosers (and those of the lanes below it). Click it
// to select the master: the device view shows its effects.
TrackHeaderItem {
    id: header

    property ArrangementMenu menu: null
    property bool ioShown: true

    readonly property int row: 18
    readonly property int pad: 2
    readonly property int box: 16
    readonly property int meterArea: 14
    readonly property int mixerWidth: 112
    readonly property int ioWidth: 84
    readonly property int mixerX: width - meterArea - mixerWidth
    readonly property int ioX: ioShown ? mixerX - ioWidth : 0
    readonly property int nameColumnRight: (ioShown ? ioX : mixerX) - 1

    function rowY(r) {
        return pad + r * row
    }

    trackId: "master"
    nameRight: nameColumnRight - 4
    ioLeft: ioX
    mixerLeft: mixerX
    meter: meterItem

    Meter {
        id: meterItem
        objectName: "meter"
        x: header.width - header.meterArea + 3
        y: header.pad
        width: 8
        height: Math.max(8, header.mainHeight - 2 * header.pad - 1)
    }

    IoChooser {
        id: mainOut
        objectName: "mainOut"
        x: header.ioX + 3
        y: header.rowY(0)
        width: header.ioWidth - 6
        visible: header.ioShown
        sub: true
        text: header.mainOutText
        tooltip: qsTr("Main Out: the audio device's outputs the master plays on")
        onClicked: header.menu.show(header.mainOutMenuEntries(), header, mainOut, 0, mainOut.height)
    }

    ValueBox {
        id: volume
        objectName: "volume"
        x: header.mixerX + 4
        y: header.rowY(0)
        width: 50
        height: header.box
        flat: true
        fill: header.volumeFraction(header.volume)
        fillColor: Theme.volumeFill
        from: -70
        to: 6
        step: 0.25
        decimals: 1
        sampleText: "-70.0"
        font: Theme.uiFont(8)
        defaultValue: 0
        wheel: false
        formatter: v => header.formatVolume(v)
        value: header.volume
        automation: header.volumeAutomation
        onMoved: (value, gestureKey) => header.setVolume(value, gestureKey, volume.relative)
        onTouched: header.touchVolume()

        HoverHandler {
            id: volumeHover
        }
        ToolTip.visible: volumeHover.hovered && !volume.dragging
        ToolTip.text: qsTr("Main Volume: %1").arg(header.formatDb(header.volume))
        ToolTip.delay: 700
    }
    ValueBox {
        id: pan
        objectName: "pan"
        x: header.mixerX + 58
        y: header.rowY(1)
        width: 50
        height: header.box
        flat: true
        fill: (header.pan + 1) / 2
        fillFrom: 0.5
        fillColor: Theme.volumeFill
        from: -1
        to: 1
        step: 0.02
        decimals: 2
        sampleText: "50L"
        font: Theme.uiFont(8)
        defaultValue: 0
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
        columnLeft: 4
        columnRight: header.nameColumnRight - 3
        mainTop: header.rowY(1)
    }

    onMenuRequested: (entries, pos) => header.menu.show(entries, header, header, pos.x, pos.y)
}
