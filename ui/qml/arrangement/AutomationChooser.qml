import QtQuick
import QtQuick.Controls
import SUBstation

// A compact drop-down: its text and a "▾"; a click asks for its menu.
Rectangle {
    id: chooser

    property string text
    property string tooltip

    signal clicked()

    height: 18
    radius: 2
    color: Theme.surface
    border.color: Theme.border

    Text {
        x: 4
        width: chooser.width - 16
        anchors.verticalCenter: parent.verticalCenter
        text: chooser.text
        elide: Text.ElideRight
        color: Theme.text
        font: Theme.uiFont(7.5)
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: 3
        anchors.verticalCenter: parent.verticalCenter
        text: "▾"
        color: Theme.textDim
        font: Theme.uiFont(7.5)
    }

    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onPressed: chooser.clicked()
    }

    ToolTip.visible: tooltip !== "" && area.containsMouse
    ToolTip.text: tooltip
    ToolTip.delay: 700
}
