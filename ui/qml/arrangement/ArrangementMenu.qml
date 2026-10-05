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

    function show(entries, target, parentItem, x, y) {
        menu.target = target
        clear(menu)
        fill(menu, entries)
        if (menu.count > 0)
            menu.popup(parentItem, x, y)
    }

    function choose(id) {
        if (menu.target)
            menu.target.triggerMenu(id)
    }

    function clear(m) {
        while (m.count > 0) {
            const item = m.takeItem(0)
            if (!item)
                break
            if (item.subMenu)
                item.subMenu.destroy()
            item.destroy()
        }
    }

    function fill(m, entries) {
        for (let i = 0; i < entries.length; ++i) {
            const entry = entries[i]
            if (entry.separator) {
                m.addItem(separatorComponent.createObject(null))
            } else if (entry.submenu) {
                const sub = submenuComponent.createObject(null, { title: entry.text, enabled: entry.enabled })
                fill(sub, entry.children)
                m.addMenu(sub)
            } else {
                m.addItem(itemComponent.createObject(null, {
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
