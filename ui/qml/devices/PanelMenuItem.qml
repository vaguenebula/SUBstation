import QtQuick
import QtQuick.Controls
import SUBstation

// An entry of the device view's menus (PanelMenu): as the style's, but its
// action's `hint` (a shortcut shown as a tip, "Ctrl+X": the window's own
// action handles the key) shows on the right where a shortcut would.
MenuItem {
    id: control

    readonly property string hintText: {
        if (!action)
            return ""
        if (action.hint !== undefined && action.hint !== "")
            return action.hint
        return Theme.shortcutText(action.shortcut)
    }
    readonly property color entryColor: !enabled ? Theme.textDisabled : (highlighted ? Theme.accentText : Theme.text)

    contentItem: Item {
        implicitWidth: label.implicitWidth + (hint.text ? control.spacing + hint.implicitWidth : 0)
        implicitHeight: label.implicitHeight

        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            text: Theme.withoutMnemonics(control.text)
            font: control.font
            color: control.entryColor
        }
        Text {
            id: hint
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: control.hintText
            font: control.font
            color: control.entryColor
        }
    }
}
