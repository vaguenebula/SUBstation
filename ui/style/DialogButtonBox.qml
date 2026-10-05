import QtQuick
import QtQuick.Templates as T
import SUBstation

// QDialogButtonBox: the buttons in a row at the right.
T.DialogButtonBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    spacing: 6
    padding: 12
    alignment: Qt.AlignRight

    delegate: Button {
        implicitWidth: Math.max(80, implicitContentWidth + leftPadding + rightPadding)
    }

    contentItem: ListView {
        implicitWidth: contentWidth
        model: control.contentModel
        spacing: control.spacing
        orientation: ListView.Horizontal
        boundsBehavior: Flickable.StopAtBounds
        snapMode: ListView.SnapToItem
    }

    background: Rectangle {
        implicitHeight: 40
        x: 1
        y: 1
        width: parent.width - 2
        height: parent.height - 2
        color: Theme.window
    }
}
