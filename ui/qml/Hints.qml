pragma Singleton

import QtQuick
import QtQuick.Controls

// What the info view (InfoView, at the bottom left of the main window, as
// Ableton's) says: the tooltip of the control under the mouse. While an info
// view shows, the tooltips of the items in its window (but those in a popup:
// a dialog's pop up as before) are said there, at once, instead of popping up
// (the style's ToolTip); hidden, they pop up again.
QtObject {
    // The info view showing them (null: none; it says so itself).
    property Item view: null
    // What it says: a tooltip's text ("": nothing), and whose (the tooltip).
    property string text: ""
    property QtObject source: null

    // Whether the tooltip of `item` is said in the info view.
    function routes(item) {
        if (!view || !view.visible || !item || item.Window.window !== view.Window.window)
            return false
        const overlay = item.Overlay.overlay
        for (let p = item; p; p = p.parent) {
            if (p === overlay)
                return false
        }
        return true
    }

    function show(tip, hint) {
        source = tip
        text = hint
    }

    function hide(tip) {
        if (source !== tip)
            return
        source = null
        text = ""
    }
}
