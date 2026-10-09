import QtQuick
import QtQuick.Controls
import SUBstation

// A menu the arrangement's C++ items work out (their MenuEntries): the lanes',
// a header's, a chooser's. show() fills it with the entries
// ([{id, text, enabled, checkable, checked, shortcut, toolTip, swatch, dot,
// separator, search, submenu, children}]) and pops it up; the entry chosen
// goes back to `target.triggerMenu(id)`, which runs it in C++. A search entry
// is a search field over a scrolled list of its children (MenuSearch).
//
//   onMenuRequested: (entries, pos) => menu.show(entries, lanes, lanes, pos.x, pos.y)
Menu {
    id: menu

    property var target: null
    // What fill() made: held here until the next show() (made without a parent,
    // the garbage collector would take them otherwise, submenus while shown).
    property var made: []
    // Its search field, which its entries type into while highlighted (ArrangementMenuItem).
    property MenuSearch searchField: null

    function show(entries, target, parentItem, x, y) {
        menu.target = target
        release()
        fill(menu, entries)
        if (menu.count > 0)
            menu.popup(parentItem, x, y)
    }

    function choose(id) {
        if (menu.target)
            menu.target.triggerMenu(id)
    }

    // Empties the menu (and its submenus), then destroys what fill() made.
    function release() {
        searchField = null
        empty(menu)
        for (let i = 0; i < made.length; ++i)
            made[i].destroy()
        made = []
    }

    function empty(m) {
        while (m.count > 0) {
            const item = m.itemAt(0)
            if (item && item.subMenu)
                empty(m.takeMenu(0))  // (the menu's own item for it goes)
            else if (!m.takeItem(0))
                break
        }
    }

    function make(component, properties) {
        const object = component.createObject(null, properties)
        made.push(object)
        return object
    }

    function fill(m, entries) {
        for (let i = 0; i < entries.length; ++i) {
            const entry = entries[i]
            if (entry.separator) {
                m.addItem(make(separatorComponent, {}))
            } else if (entry.search) {
                const search = make(searchComponent, {
                    menu: m,
                    rows: entry.children,
                    run: child => menu.choose(child.id),
                    rowComponent: rowComponent
                })
                m.addItem(search)
                if (m === menu)
                    searchField = search
            } else if (entry.submenu) {
                const sub = make(submenuComponent, { title: entry.text, enabled: entry.enabled })
                fill(sub, entry.children)
                m.addMenu(sub)
            } else {
                m.addItem(make(itemComponent, { entry: entry }))
            }
        }
    }

    Component {
        id: itemComponent
        ArrangementMenuItem {
            onTriggered: menu.choose(entryId)
        }
    }

    // A row of a search field's list: drawn as the entries are.
    Component {
        id: rowComponent
        ArrangementMenuItem {}
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
