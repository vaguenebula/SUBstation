import QtQuick
import QtQuick.Controls
import SUBstation

// An entry of an ArrangementMenu: the style's menu item with its shortcut as a
// hint on the right (the window's actions handle the keys, as the old menus'
// setShortcutVisibleInContextMenu had it), and a colour swatch (a track
// colour) or a dot (an automated parameter) before its text.
MenuItem {
    id: control

    property string shortcutHint
    property string toolTipText
    property color swatch: "transparent"
    property color dot: "transparent"
    property int entryId: -1

    readonly property color labelColor: !enabled ? Theme.textDisabled : (highlighted ? Theme.accentText : Theme.text)

    contentItem: Item {
        implicitWidth: label.implicitWidth + (hint.text ? control.spacing + hint.implicitWidth : 0)
        implicitHeight: label.implicitHeight

        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            text: Theme.withoutMnemonics(control.text)
            font: control.font
            color: control.labelColor
        }
        Text {
            id: hint
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: Theme.shortcutText(control.shortcutHint)
            font: control.font
            color: control.labelColor
        }
    }

    // The swatch or the dot, where a menu's icons go.
    Rectangle {
        x: 3
        anchors.verticalCenter: parent.verticalCenter
        width: 12
        height: 12
        visible: control.swatch.a > 0
        color: control.swatch
    }
    Rectangle {
        x: 5
        anchors.verticalCenter: parent.verticalCenter
        width: 6
        height: 6
        radius: 3
        visible: control.dot.a > 0
        color: control.dot
    }

    ToolTip.visible: toolTipText !== "" && hovered
    ToolTip.text: toolTipText
    ToolTip.delay: 500
}
