import QtQuick
import QtQuick.Templates as T
import SUBstation

// The main window: WINDOW behind everything, the UI font, and theme.apply()'s
// palette for anything drawn from a palette (the Basic style's controls this
// style leaves alone).
T.ApplicationWindow {
    id: window

    color: Theme.window
    font: Theme.font

    palette.window: Theme.window
    palette.windowText: Theme.text
    palette.base: Theme.panel
    palette.alternateBase: Theme.panelAlt
    palette.text: Theme.text
    palette.button: Theme.surface
    palette.buttonText: Theme.text
    palette.brightText: Theme.accentText
    palette.highlight: Theme.accent
    palette.highlightedText: Theme.accentText
    palette.toolTipBase: Theme.panelAlt
    palette.toolTipText: Theme.text
    palette.placeholderText: Theme.textDim
    palette.link: Theme.accent
    palette.light: Theme.surfaceHover
    palette.midlight: Theme.surfaceHover
    palette.mid: Theme.panelAlt
    palette.dark: Theme.border
    palette.shadow: "#000000"
    palette.disabled.text: Theme.textDisabled
    palette.disabled.buttonText: Theme.textDisabled
    palette.disabled.windowText: Theme.textDisabled
}
