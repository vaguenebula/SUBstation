import QtQuick
import QtQuick.Controls
import SUBstation

// A built-in device parameter's right-click menu (frame.py's _automation_menu):
// Show Automation (if it can be automated), Delete Automation (if it has an
// envelope), Re-Enable Automation (while its automation is overridden) and,
// on a device inside a rack, Map to Macro (the rack's eight, the one it is
// mapped to checked) and Unmap from Macro N. Its entries are made for how
// things are when it opens: show() fills it and pops it up at the mouse.
DynamicMenu {
    id: menu

    property DeviceParam param: null

    // Fills the menu for the parameter as it is now.
    function build() {
        clear()
        if (!param)
            return
        const p = param
        entry(qsTr("Show Automation"), () => p.showAutomation(), undefined, p.canAutomate())
        entry(qsTr("Delete Automation"), () => p.deleteAutomation(), undefined, p.hasEnvelope())
        if (p.isOverridden())
            entry(qsTr("Re-Enable Automation"), () => p.reEnableAutomation())
        const rack = p.rackId()
        if (rack !== "") {  // its rack's macros can move it
            separator()
            const mapped = p.macro()
            const mappedRack = p.macroRack()
            const macros = submenu(qsTr("Map to Macro"))
            for (let i = 0; i < 8; ++i) {
                const index = i
                entry(qsTr("Macro %1").arg(i + 1), () => p.mapToMacro(index), mappedRack === rack && mapped === i,
                      true, macros)
            }
            if (mapped >= 0)
                entry(qsTr("Unmap from Macro %1").arg(mapped + 1), () => p.unmapFromMacro())
        }
    }

    function show() {
        build()
        popup()
    }
}
