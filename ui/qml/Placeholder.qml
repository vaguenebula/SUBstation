import QtQuick
import SUBstation

// Where a view goes until it is written: its name on its background colour.
Rectangle {
    id: placeholder

    property string label
    property string detail

    color: Theme.panel
    clip: true

    Column {
        anchors.centerIn: parent
        spacing: 4

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: placeholder.label
            color: Theme.textDim
            font: Theme.uiFont(12, true)
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: text !== ""
            text: placeholder.detail
            color: Theme.textDisabled
            font: Theme.font
        }
    }
}
