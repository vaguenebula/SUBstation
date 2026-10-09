import QtQuick
import QtQuick.Templates as T
import SUBstation

// QMenu::item: padding 4 22 4 18, the highlighted one in ACCENT, disabled ones
// dim; the action's shortcut on the right (as widget menus show them), a tick
// for checked items, an arrow for submenus. In a menu with a search field (its
// `searchField`: SUBstation's MenuSearch), what is typed while it is
// highlighted goes there.
T.MenuItem {
    id: control

    readonly property string shortcutText: action ? Theme.shortcutText(action.shortcut) : ""
    readonly property color textColor: !enabled ? Theme.textDisabled : (highlighted ? Theme.accentText : Theme.text)

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)

    topPadding: 4
    bottomPadding: 4
    leftPadding: 18
    rightPadding: 22
    spacing: 24
    hoverEnabled: true

    contentItem: Item {
        implicitWidth: label.implicitWidth + (shortcut.text ? control.spacing + shortcut.implicitWidth : 0)
        implicitHeight: label.implicitHeight

        Text {
            id: label
            anchors.verticalCenter: parent.verticalCenter
            text: Theme.withoutMnemonics(control.text)
            font: control.font
            color: control.textColor
        }
        Text {
            id: shortcut
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: control.shortcutText
            font: control.font
            color: control.textColor
        }
    }

    indicator: Text {
        x: 4
        y: control.topPadding + (control.availableHeight - height) / 2
        visible: control.checkable && control.checked
        text: "✓"
        font.pixelSize: 11
        font.bold: true
        color: control.textColor
    }

    arrow: Icon {
        x: control.width - width - 8
        y: control.topPadding + (control.availableHeight - height) / 2
        visible: control.subMenu !== null
        name: "fold"
        checked: true  // pointing right
        size: 9
        color: control.textColor
    }

    background: Rectangle {
        implicitWidth: 100
        implicitHeight: 20
        color: control.highlighted && control.enabled ? Theme.accent : "transparent"
    }

    // Typed while it is highlighted: into its menu's search field, if it has one (MenuSearch).
    Keys.onShortcutOverride: event => {
        const search = control.menu ? control.menu.searchField : null
        if (search && search.typed(event))
            event.accepted = true
    }
    Keys.onPressed: event => {
        const search = control.menu ? control.menu.searchField : null
        if (search)
            search.keyPressed(control, event)
    }
}
