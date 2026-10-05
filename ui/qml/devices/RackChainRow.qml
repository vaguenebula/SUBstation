import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import SUBstation

// A rack's chain in its chain list (rack_view.py's _ChainRow): its activator
// (unmuted), name, solo, volume, pan and meter, as a track's header has them;
// lit (with an accent bar) while the device view shows its devices beside the
// rack. A click shows them (and makes it the chain Ctrl+R renames); right-click
// for its menu (rename, duplicate, delete, add a chain, show its volume's or
// pan's automation), after which it is shown too. Renamed in place
// (startRename()): Enter, Escape or leaving the field keeps the name typed (an
// empty one keeps the old name).
Rectangle {
    id: row

    readonly property string panelRole: "chainRow"
    property string trackId
    property string rackId
    property string chainId
    property bool selected: false
    required property var panel
    readonly property RackChain chain: rackChain
    readonly property bool renaming: renameField.visible
    readonly property alias nameLabel: name
    readonly property alias activator: activator
    readonly property alias solo: solo
    readonly property alias volume: volume
    readonly property alias pan: pan
    readonly property alias meter: meter
    readonly property alias renameField: renameField

    implicitHeight: 22  // CHAIN_ROW_HEIGHT
    color: selected ? Theme.surfaceHover : Theme.surface

    // Rename it in place.
    function startRename() {
        if (renameField.visible)
            return
        renameField.text = rackChain.name
        renameField.visible = true
        renameField.selectAll()
        renameField.forceActiveFocus()
    }

    function finishRename() {
        if (!renameField.visible)
            return
        const typed = renameField.text
        renameField.visible = false
        rackChain.rename(typed)  // (an empty name keeps the old one)
    }

    RackChain {
        id: rackChain
        session: Session
        trackId: row.trackId
        rackId: row.rackId
        chainId: row.chainId
        onMeterUpdated: (left, right) => meter.setLevels(left, right)
    }

    Rectangle {
        visible: row.selected
        width: 2
        height: parent.height
        color: Theme.accent
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: mouse => {
            if (mouse.button === Qt.RightButton)
                row.panel.showChainMenu(row, mouse.x, mouse.y)
            else
                Session.deviceSelection.clickChain(row.rackId, row.chainId)
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 3
        anchors.rightMargin: 3
        anchors.topMargin: 1
        anchors.bottomMargin: 1
        spacing: 3

        ToggleButton {
            id: activator
            objectName: "activator"
            role: "activator"
            checkable: false
            checked: !rackChain.mute
            tooltip: qsTr("Chain Activator (unmute)")
            Layout.preferredWidth: 14
            Layout.preferredHeight: 14
            onClicked: rackChain.setMute(!rackChain.mute)
        }
        Text {
            id: name
            objectName: "name"
            Layout.fillWidth: true
            Layout.minimumWidth: 30
            text: rackChain.name
            elide: Text.ElideRight
            color: Theme.text
            font: Theme.font
            verticalAlignment: Text.AlignVCenter

            HoverHandler {
                id: nameHover
            }
            ToolTip.visible: nameHover.hovered && !renameField.visible
            ToolTip.text: rackChain.name
            ToolTip.delay: 700
        }
        ToggleButton {
            id: solo
            objectName: "solo"
            role: "solo"
            text: "S"
            checkable: false
            checked: rackChain.solo
            tooltip: qsTr("Solo: only the soloed chains of the rack are heard")
            Layout.preferredWidth: 18
            Layout.preferredHeight: 16
            onClicked: rackChain.setSolo(!rackChain.solo)
        }
        ValueBox {
            id: volume
            objectName: "volume"
            Layout.preferredWidth: 52
            Layout.fillHeight: true
            from: -70
            to: 6
            step: 0.25
            decimals: 1
            defaultValue: 0
            wheel: false
            sampleText: "-70.0 dB"
            formatter: v => rackChain.formatVolume(v)
            value: rackChain.volume
            automation: rackChain.volumeAutomation
            onMoved: (v, key) => rackChain.setVolume(v, key)
        }
        Knob {
            id: pan
            objectName: "pan"
            Layout.preferredWidth: 18
            Layout.preferredHeight: 18
            from: -1
            to: 1
            defaultValue: 0
            bipolar: true
            wheel: false
            formatter: v => rackChain.formatPan(v)
            parser: text => rackChain.parsePan(text)
            value: rackChain.pan
            automation: rackChain.panAutomation
            onMoved: (v, key) => rackChain.setPan(v, key)
        }
        Meter {
            id: meter
            objectName: "meter"
            Layout.preferredWidth: 6
            Layout.fillHeight: true
        }
    }

    TextField {
        id: renameField
        objectName: "renameField"
        visible: false
        x: name.x + 3
        y: 1
        width: name.width
        height: row.height - 2
        padding: 0
        leftPadding: 2
        font: Theme.font
        onEditingFinished: row.finishRename()
        onActiveFocusChanged: if (!activeFocus) row.finishRename()
        Keys.onEscapePressed: row.finishRename()
    }
}
