import QtQuick
import QtQuick.Templates as T
import SUBstation

// A popup panel: PANEL_ALT with a BORDER line.
T.Popup {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    padding: 6

    background: Rectangle {
        color: Theme.panelAlt
        border.color: Theme.border
    }

    T.Overlay.modal: Rectangle {
        color: "#60000000"
    }
    T.Overlay.modeless: Rectangle {
        color: "#20000000"
    }
}
