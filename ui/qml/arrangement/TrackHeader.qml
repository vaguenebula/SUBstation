import QtQuick
import QtQuick.Controls
import SUBstation

// A track's header (a group's, a return's too), right of its lane, laid out as
// Ableton's, in rows `row` apart:
//
//   name column             | In/Out column              | mixer column              | meter
//   [fold] name (its colour)| Audio From / MIDI From     | activator  S  arm         |
//   device chooser          |   its channel, or tap      | volume     pan            |
//   parameter chooser       | In  Auto  Off              | sends...                  |
//                    (+)    | Audio To                   |                           |
//                           |   Track In, Sidechain-...  |                           |
//
// A group (and a return) has no input, monitoring or arm (it records
// nothing): its Audio To is its In/Out column's first row, and its colour fills
// its name column's first two rows (its choosers below). A row shows where the
// track is tall enough for it (a folded track: its first). Without `ioShown`
// (View › In/Out) there is no In/Out column. Below its own lane, each lane's
// choosers. What it paints, its mouse handling and what its controls do are
// TrackHeaderItem's.
TrackHeaderItem {
    id: header

    property ArrangementMenu menu: null
    property bool ioShown: true

    readonly property int row: 18
    readonly property int pad: 2
    readonly property int box: 16
    readonly property int meterArea: 14
    readonly property int mixerWidth: 92
    readonly property int ioWidth: 84
    readonly property int mixerX: width - meterArea - mixerWidth
    readonly property int ioX: ioShown ? mixerX - ioWidth : 0
    readonly property int nameColumnRight: (ioShown ? ioX : mixerX) - 1
    readonly property bool isGroup: kind === "group"
    readonly property bool isReturn: kind === "return"
    readonly property bool hasSends: sends.length > 0
    // The In/Out rows: a track's input, its channel, monitoring, then Audio To; a group's Audio To first.
    readonly property int outputRow: records ? 3 : 0

    function rowY(r) {
        return pad + r * row
    }
    function fits(r) {
        return rowY(r) + box <= mainHeight - 1 - pad
    }

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

    // --- In/Out ---------------------------------------------------------------------------

    Item {
        id: io
        objectName: "io"
        x: header.ioX + 3
        width: header.ioWidth - 6
        height: header.mainHeight
        visible: header.ioShown

        IoChooser {
            id: input
            objectName: "input"
            y: header.rowY(0)
            width: io.width
            visible: header.records && header.fits(0)
            text: header.inputText
            tooltip: header.inputToolTip
            onClicked: header.menu.show(header.inputMenuEntries(), header, input, 0, input.height)
        }
        IoChooser {
            id: inputChannel
            objectName: "inputChannel"
            y: header.rowY(1)
            width: io.width
            sub: true
            visible: header.records && header.fits(1)
            text: header.inputChannelText
            tooltip: header.inputChannelToolTip
            onClicked: header.menu.show(header.inputChannelMenuEntries(), header, inputChannel, 0, inputChannel.height)
        }
        Row {
            objectName: "monitor"
            y: header.rowY(2)
            spacing: 2
            visible: header.records && header.fits(2)

            Repeater {
                model: [{mode: "in", label: qsTr("In")}, {mode: "auto", label: qsTr("Auto")}, {mode: "off", label: qsTr("Off")}]

                RoleButton {
                    required property var modelData
                    objectName: "monitor:" + modelData.mode
                    width: modelData.mode === "auto" ? io.width - 2 * 20 - 4 : 20
                    height: header.box
                    role: "monitor"
                    text: modelData.label
                    lit: header.monitor === modelData.mode
                    tooltip: header.monitorToolTip(modelData.mode, header.midi)
                    onClicked: header.setMonitor(modelData.mode)
                }
            }
        }
        IoChooser {
            id: output
            objectName: "output"
            y: header.rowY(header.outputRow)
            width: io.width
            visible: header.fits(header.outputRow)
            text: header.outputText
            tooltip: header.outputToolTip
            onClicked: header.menu.show(header.outputMenuEntries(), header, output, 0, output.height)
        }
        IoChooser {
            id: outputChannel
            objectName: "outputChannel"
            y: header.rowY(header.outputRow + 1)
            width: io.width
            sub: true
            visible: header.fits(header.outputRow + 1)
            text: header.outputChannelText
            tooltip: header.outputChannelToolTip
            onClicked: header.menu.show(header.outputChannelMenuEntries(), header, outputChannel, 0,
                                        outputChannel.height)
        }
    }

    // --- Mixer -----------------------------------------------------------------------------

    ToggleButton {
        id: activator
        objectName: "activator"
        x: header.mixerX + 4
        y: header.rowY(0)
        width: 40
        height: header.box
        role: "activator"
        text: header.isReturn ? header.letter : String(header.number)
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
    ToggleButton {
        id: solo
        objectName: "solo"
        x: header.mixerX + 48
        y: header.rowY(0)
        width: 20
        height: header.box
        role: "solo"
        text: "S"
        tooltip: header.soloToolTip
        checked: header.solo
        onToggled: header.soloClicked(checked)
    }
    ToggleButton {
        id: arm
        objectName: "arm"
        x: header.mixerX + 72
        y: header.rowY(0)
        width: 16
        height: header.box
        visible: header.records
        role: "arm"
        text: "●"
        tooltip: qsTr("Arm Recording; Ctrl-click to arm it along with others")
        checked: header.armed
        onToggled: header.armClicked(checked)
    }

    ValueBox {
        id: volume
        objectName: "volume"
        x: header.mixerX + 4
        y: header.rowY(1)
        width: 40
        height: header.box
        visible: header.fits(1)
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
        ToolTip.text: qsTr("Track Volume: %1 (drag; select and type a number; double-click to reset)")
                          .arg(header.formatDb(header.volume))
        ToolTip.delay: 700
    }
    ValueBox {
        id: pan
        objectName: "pan"
        x: header.mixerX + 48
        y: header.rowY(1)
        width: 40
        height: header.box
        visible: header.fits(1)
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

        HoverHandler {
            id: panHover
        }
        ToolTip.visible: panHover.hovered && !pan.dragging
        ToolTip.text: qsTr("Track Panning (drag; select and type 25L, C, 50R; double-click to center)")
        ToolTip.delay: 700
    }

    // The sends, below volume and pan.
    SendKnobs {
        objectName: "sends"
        header: header
        menu: header.menu
        x: header.mixerX + 2
        y: header.rowY(2)
        width: header.mixerWidth - 4
        height: Math.max(0, header.mainHeight - 1 - header.pad - y)
        visible: header.hasSends && header.fits(2)
    }

    // --- Name column ---------------------------------------------------------------------

    AutomationChoosers {
        objectName: "choosers"
        anchors.fill: parent
        header: header
        menu: header.menu
        columnLeft: (header.isReturn ? 1 : header.indent + (header.isGroup ? 6 : 0)) + 3  // (after a group's band)
        columnRight: header.nameColumnRight - 3
        mainTop: header.rowY(header.isGroup ? 2 : 1)
    }

    TextField {
        id: rename
        objectName: "rename"
        x: header.nameLeft - 2
        y: header.pad - 1
        width: header.nameColumnRight - x - 3
        height: header.box + 2
        visible: header.renaming
        padding: 2
        font: Theme.uiFont(9)
        onVisibleChanged: {
            if (visible) {
                text = header.nameTemplate  // ("# Kick": the # stays its number)
                selectAll()
                forceActiveFocus()
            }
        }
        onEditingFinished: header.finishRename(text)
        onActiveFocusChanged: if (!activeFocus && visible) header.finishRename(text)
    }

    onMenuRequested: (entries, pos) => header.menu.show(entries, header, header, pos.x, pos.y)
}
