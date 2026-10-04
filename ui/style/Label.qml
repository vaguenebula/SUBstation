import QtQuick
import QtQuick.Templates as T
import SUBstation

// QLabel: TEXT (dim when disabled), links in ACCENT.
T.Label {
    color: enabled ? Theme.text : Theme.textDisabled
    linkColor: Theme.accent
}
