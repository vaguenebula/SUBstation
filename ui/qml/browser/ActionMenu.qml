import QtQuick
import QtQuick.Controls
import SUBstation

// A context menu made from a list of actions as the browser controller hands
// them out ([{action, label}], {} for a separator): popupWith(actions) shows
// it at the mouse, chosen(action) says which was picked.
Menu {
    id: menu

    signal chosen(string action)

    // (The items have a parent: the garbage collector leaves them be.)
    function popupWith(actions) {
        while (menu.count > 0) {
            const item = menu.takeItem(0)
            if (item)
                item.destroy()
        }
        for (const entry of actions) {
            if (entry.action)
                menu.addItem(entryItem.createObject(menu.contentItem, {text: entry.label, actionId: entry.action}))
            else
                menu.addItem(separator.createObject(menu.contentItem))
        }
        if (menu.count > 0)
            menu.popup()
    }

    Component {
        id: entryItem

        MenuItem {
            property string actionId
            objectName: "menu_" + actionId
            onTriggered: menu.chosen(actionId)
        }
    }

    Component {
        id: separator

        MenuSeparator {}
    }
}
