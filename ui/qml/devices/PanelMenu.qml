import QtQuick
import QtQuick.Controls
import SUBstation

// The device view's menus (a device's, its sidechain's, a rack chain's, a
// macro's, the one beside the devices): one menu, filled anew each time it
// opens (DynamicMenu). It lives with the panel, not with what it is about: a
// device deleted from its own menu takes its frame along, not the menu.
// hinted() makes an entry showing a shortcut as a tip, as the old menus did
// (the window's actions handle the keys); afterClose runs once it closes,
// whatever was chosen.
DynamicMenu {
    id: menu

    property var afterClose: null

    delegate: PanelMenuItem {}

    // An entry showing `hint` ("Ctrl+X") on its right.
    function hinted(text, run, hint, enabled) {
        const action = hintedAction.createObject(menu, {
            text: text,
            hint: hint,
            enabled: enabled !== false
        })
        action.triggered.connect(run)
        menu.addAction(action)
        made.push(action)
        return action
    }

    // The menu of the moment: emptied, and nothing to do after it.
    function reset() {
        afterClose = null
        clear()
    }

    onClosed: {
        const after = afterClose
        afterClose = null
        if (after)
            after()
    }

    Component {
        id: hintedAction
        Action {
            property string hint
        }
    }
}
