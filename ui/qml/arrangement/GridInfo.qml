import QtQuick
import QtQuick.Controls
import SUBstation

// The corner above the headers (the old arrangement_view.py's GridInfo): the
// grid's size; a click toggles snapping.
Rectangle {
    id: info

    required property Arrangement arrangement

    color: Theme.panel

    Rectangle {
        width: parent.width
        height: 1
        anchors.bottom: parent.bottom
        color: Theme.border
    }
    Rectangle {
        width: 1
        height: parent.height
        color: Theme.border
    }

    Text {
        objectName: "gridLabel"
        x: 10
        width: parent.width - 18
        anchors.verticalCenter: parent.verticalCenter
        text: info.arrangement.gridLabel
        color: info.arrangement.snap ? Theme.text : Theme.textDisabled
        font: Theme.font
        elide: Text.ElideRight
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        onPressed: info.arrangement.snap = !info.arrangement.snap
    }

    ToolTip.visible: area.containsMouse
    ToolTip.text: qsTr("Grid (Ctrl+1 narrower, Ctrl+2 wider). Click to toggle snapping (Ctrl+4).")
    ToolTip.delay: 700
}
