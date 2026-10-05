import QtQuick
import QtQuick.Controls
import SUBstation

// A menu the arrangement's C++ items work out (their MenuEntries): the lanes',
// a header's, a chooser's. show() fills it with the entries
// ([{id, text, enabled, checkable, checked, shortcut, toolTip, swatch, dot,
// separator, submenu, children}]) and pops it up; the entry chosen goes back
// to `target.triggerMenu(id)`, which runs it in C++.
//
//   onMenuRequested: (entries, pos) => menu.show(entries, lanes, lanes, pos.x, pos.y)
Menu {
    id: menu

    property var target: null
    // What fill() made: held here until the next show() (made without a parent,
    // the garbage collector would take them otherwise, submenus while shown).
    property var made: []

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
            } else if (entry.submenu) {
                const sub = make(submenuComponent, { title: entry.text, enabled: entry.enabled })
                fill(sub, entry.children)
                m.addMenu(sub)
            } else {
                m.addItem(make(itemComponent, {
                    text: entry.text,
                    enabled: entry.enabled,
                    checkable: entry.checkable,
                    checked: entry.checked,
                    shortcutHint: entry.shortcut,
                    toolTipText: entry.toolTip,
                    swatch: entry.swatch !== undefined ? entry.swatch : "transparent",
                    dot: entry.dot !== undefined ? entry.dot : "transparent",
                    entryId: entry.id
                }))
            }
        }
    }

    Component {
        id: itemComponent
        ArrangementMenuItem {
            onTriggered: menu.choose(entryId)
        }
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
