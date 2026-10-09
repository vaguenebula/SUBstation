import QtQuick
import QtQuick.Controls
import SUBstation

// A built-in device parameter's right-click menu: Show Automation (if it can be
// automated), Delete Automation (if it has an envelope), Re-Enable Automation
// (while its automation is overridden) and, on a device inside a rack, Map to
// Macro (the rack's macros, by name, the one it is mapped to checked) and Unmap
// from its macro. Its entries are made for how things are when it opens: show()
// fills it and pops it up at the mouse.
DynamicMenu {
    id: menu

    property DeviceParam param: null

    // Fills the menu for the parameter as it is now.
    function build() {
        clear()
        if (!param)
            return
        const p = param
        automationEntries(p)
        const rack = p.rackId()
        if (rack !== "") {  // its rack's macros can move it
            separator()
            const mapped = p.macro()
            const mappedRack = p.macroRack()
            const names = p.macroNames()
            const macros = submenu(qsTr("Map to Macro"))
            for (let i = 0; i < names.length; ++i) {
                const index = i
                entry(names[i], () => p.mapToMacro(index), mappedRack === rack && mapped === i, true, macros)
            }
            if (mapped >= 0) {
                const name = mappedRack === rack && mapped < names.length ? names[mapped]
                                                                          : qsTr("Macro %1").arg(mapped + 1)
                entry(qsTr("Unmap from %1").arg(name), () => p.unmapFromMacro())
            }
        }
    }

    function show() {
        build()
        popup()
    }
}
