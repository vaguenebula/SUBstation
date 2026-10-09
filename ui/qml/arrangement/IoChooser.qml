import QtQuick
import QtQuick.Controls
import SUBstation

// One of a track header's In/Out choosers, as Ableton's: its text and a "▾";
// a click asks for its menu. One under another (a
// channel, where an input is taken, where the output goes in its track) has a
// grip at its left. With no text it is an empty frame and takes no click:
// there is nothing to choose.
Item {
    id: chooser

    property string text
    property string tooltip
    property bool sub: false

    readonly property bool empty: text === ""

    signal clicked()

    height: 16

    Rectangle {
        anchors.fill: parent
        radius: 2
        color: chooser.empty ? "transparent" : (area.containsMouse ? Theme.surfaceHover : Theme.surface)
        border.color: chooser.empty ? Theme.surface : Theme.border
    }
    Row {  // the grip
        x: 3
        anchors.verticalCenter: parent.verticalCenter
        spacing: 1
        visible: chooser.sub && !chooser.empty

        Repeater {
            model: 2

            Rectangle {
                width: 1
                height: 8
                color: Theme.textDisabled
            }
        }
    }
    Text {
        objectName: "chooserText"
        x: chooser.sub ? 9 : 4
        width: chooser.width - x - 11
        height: chooser.height
        verticalAlignment: Text.AlignVCenter
        text: chooser.text
        elide: Text.ElideRight
        color: Theme.text
        font: Theme.uiFont(8)
    }
    Text {
        anchors.right: parent.right
        anchors.rightMargin: 3
        anchors.verticalCenter: parent.verticalCenter
        visible: !chooser.empty
        text: "▾"
        color: Theme.textDim
        font: Theme.uiFont(7.5)
    }

    MouseArea {
        id: area
        anchors.fill: parent
        enabled: !chooser.empty
        hoverEnabled: true
        onPressed: chooser.clicked()
    }

    ToolTip.visible: tooltip !== "" && area.containsMouse
    ToolTip.text: tooltip
    ToolTip.delay: 700
}
