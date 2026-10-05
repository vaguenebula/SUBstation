import QtQuick
import QtQuick.Controls

// A menu filled anew each time it opens, for how things are then (the old
// widgets built their QMenus on every right-click): clear() it, add entries
// (Actions), separators and submenus, then popup(). What it made goes with the
// next clear().
Menu {
    id: menu

    // What the entries were made from (actions, separators, submenus), destroyed by clear().
    property var made: []

    function clear() {
        while (count > 0)
            removeItem(itemAt(0))
        for (const object of made)
            object.destroy()
        made = []
    }

    // An entry of `target` (this menu or a submenu): `checked` makes it checkable (undefined: not).
    function entry(text, run, checked, enabled, target) {
        const into = target || menu
        const action = actionComponent.createObject(menu, {
            text: text,
            checkable: checked !== undefined,
            checked: checked === true,
            enabled: enabled !== false
        })
        action.triggered.connect(run)
        into.addAction(action)
        made.push(action)
        return action
    }

    function separator(target) {
        const into = target || menu
        into.addItem(separatorComponent.createObject(into.contentItem))
    }

    function submenu(title, target) {
        const into = target || menu
        const made_ = submenuComponent.createObject(menu, { title: title })
        into.addMenu(made_)
        made.push(made_)
        return made_
    }

    Component {
        id: actionComponent
        Action {}
    }
    Component {
        id: separatorComponent
        MenuSeparator {}
    }
    Component {
        id: submenuComponent
        Menu {}
    }
}
