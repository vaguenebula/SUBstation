import QtQuick
import SUBstation

// A button's icon and text side by side, centred, the text in its look's
// colour. The icon is `control.iconName` from the icon provider (in
// `control.iconColor`, its On picture while checked, dim while disabled), or
// else `control.icon.source`.
Item {
    id: content

    required property var control

    implicitWidth: row.implicitWidth
    implicitHeight: row.implicitHeight

    Row {
        id: row
        anchors.centerIn: parent
        spacing: content.control.spacing

        Icon {
            anchors.verticalCenter: parent.verticalCenter
            visible: name !== ""
            name: content.control.iconName || ""
            color: content.control.iconColor
            checked: content.control.checked
            size: content.control.iconSize
        }
        Image {
            anchors.verticalCenter: parent.verticalCenter
            visible: !content.control.iconName && source != ""
            source: content.control.icon ? content.control.icon.source : ""
            sourceSize: Qt.size(content.control.iconSize, content.control.iconSize)
            width: content.control.iconSize
            height: content.control.iconSize
            opacity: content.control.enabled ? 1.0 : 0.4
        }
        Text {
            anchors.verticalCenter: parent.verticalCenter
            visible: text !== ""
            text: Theme.withoutMnemonics(content.control.text)
            font: content.control.font
            color: content.control.look.text
        }
    }
}
