import QtQuick
import QtQuick.Controls
import SUBstation

// A menu filled anew each time it opens, for how things are then (the old
// widgets built their QMenus on every right-click): clear() it, add entries
// (Actions), separators, submenus and a search field, then popup(). What it
// made goes with the next clear().
Menu {
    id: menu

    // What the entries were made from (actions, separators, submenus), destroyed by clear().
    property var made: []
    // Its search field (search()), which its entries type into while highlighted (PanelMenuItem).
    property MenuSearch searchField: null

    function clear() {
        searchField = null
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

    // A search field over a scrolled list of `rows` ({text, enabled, checkable,
    // checked}: MenuSearch), run(row) when one is chosen; it takes the keyboard
    // as the menu opens.
    function search(rows, run, target) {
        const into = target || menu
        const field = searchComponent.createObject(into.contentItem, { menu: into, rows: rows, run: run })
        into.addItem(field)
        if (into === menu)
            searchField = field
        return field
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
        id: searchComponent
        MenuSearch {}
    }
    Component {
        id: submenuComponent
        Menu {}
    }
}
