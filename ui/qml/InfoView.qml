import QtQuick
import SUBstation

// The info view, at the bottom left of the main window (as Ableton's): what
// the control under the mouse is, its tooltip said here at once rather than
// popping up (Hints). Its first line is the title, the rest under it; a tip
// of one line too long for the title bar is all under it ("Info" over it, as
// over nothing). A panel as a device is (a title bar, a body), as tall as the
// devices beside it.
Rectangle {
    id: view

    readonly property string hint: Hints.text
    readonly property int newline: hint.indexOf("\n")
    readonly property bool longLine: newline < 0 && lineMetrics.advanceWidth > title.width
    readonly property alias title: title
    readonly property alias body: body

    Component.onCompleted: Hints.view = view
    Component.onDestruction: {
        if (Hints.view === view)
            Hints.view = null
    }

    radius: 3
    color: Theme.panelAlt
    border.width: 1
    border.color: Theme.deviceHeader

    FontMetrics {
        id: titleMetrics
        font: Theme.uiFont(9, true)
    }
    TextMetrics {
        id: lineMetrics
        font: titleMetrics.font
        text: view.hint
    }

    // The title bar.
    Rectangle {
        id: header
        x: 1
        y: 1
        width: view.width - 2
        height: Math.max(16, titleMetrics.height) + 4
        radius: 2
        color: Theme.deviceHeader

        Rectangle {  // (square below)
            y: parent.height / 2
            width: parent.width
            height: parent.height - y
            color: parent.color
        }

        Text {
            id: title
            objectName: "infoTitle"
            anchors.fill: parent
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            text: view.hint === "" || view.longLine ? qsTr("Info")
                                                    : view.newline < 0 ? view.hint : view.hint.slice(0, view.newline)
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            color: view.hint === "" || view.longLine ? Theme.textDim : Theme.text
            font: titleMetrics.font
        }
    }

    Text {
        id: body
        objectName: "infoText"
        x: 6
        y: header.y + header.height + 5
        width: view.width - 12
        height: view.height - y - 5
        text: view.hint === "" ? qsTr("Move the mouse over a control to see what it is.")
                               : view.longLine ? view.hint : view.newline < 0 ? "" : view.hint.slice(view.newline + 1)
        wrapMode: Text.Wrap
        elide: Text.ElideRight
        color: view.hint === "" ? Theme.textDisabled : Theme.text
        font: Theme.font
        lineHeight: 1.1
    }

    // The grip in the gap before the devices.
    DeviceGrip {
        x: view.width + 2
        height: view.height
    }
}
