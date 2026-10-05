import QtQuick
import SUBstation

// A rack's chain shown beside the rack (panel.py's _ChainView): its devices,
// in an accent bracket, where they are selected, dragged and dropped onto as
// on the track's own chain (racks in it show theirs further along); "Drop
// devices here" while it has none.
Item {
    id: view

    objectName: "chainView"
    readonly property string panelRole: "chain"
    property string chainId
    property var panel
    readonly property alias devices: devices

    implicitWidth: 6 + (devices.count > 0 ? devices.width : hint.implicitWidth) + 6

    // EMPTY_AREA, with ACCENT borders left and right (3 px corners).
    Rectangle {
        anchors.fill: parent
        radius: 3
        color: Theme.accent
    }
    Rectangle {
        x: 2
        width: parent.width - 4
        height: parent.height
        radius: 1
        color: Theme.emptyArea
    }

    DeviceChain {
        id: devices
        x: 6
        height: view.height
        visible: count > 0
        chainId: view.chainId
        panel: view.panel
    }

    Text {
        id: hint
        objectName: "chainHint"
        x: 6
        height: view.height
        visible: devices.count === 0
        verticalAlignment: Text.AlignVCenter
        text: qsTr("Drop devices here")
        color: Theme.textDisabled
        font: Theme.font
    }
}
