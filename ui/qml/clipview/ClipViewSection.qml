import QtQuick
import QtQuick.Controls
import SUBstation

// One of the clip view's boxes of controls: a bold title over its content, on
// PANEL_ALT with a BORDER line and 4 px corners.
Rectangle {
    id: section

    property string title
    default property alias content: body.data

    implicitHeight: body.y + body.implicitHeight + 8
    color: Theme.panelAlt
    border.color: Theme.border
    radius: 4

    Label {
        id: heading
        x: 8
        y: 6
        text: section.title
        font: Theme.uiFont(9, true)
    }

    Column {
        id: body
        x: 8
        y: heading.y + heading.height + 4
        width: section.width - 16
        spacing: 4
    }
}
