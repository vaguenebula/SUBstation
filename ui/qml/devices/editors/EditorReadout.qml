import QtQuick
import SUBstation

// A device editor's readout: a control's value, centred under it (dimmed while
// the control is disabled).
Text {
    horizontalAlignment: Text.AlignHCenter
    color: enabled ? Theme.text : Theme.textDisabled
    font: Theme.uiFont(8)
}
