import QtQuick
import QtQuick.Templates as T
import SUBstation

// QDialog: WINDOW, its title above, its buttons at the bottom right.
T.Dialog {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding,
                            implicitHeaderWidth,
                            implicitFooterWidth)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding
                             + (implicitHeaderHeight > 0 ? implicitHeaderHeight + spacing : 0)
                             + (implicitFooterHeight > 0 ? implicitFooterHeight + spacing : 0))

    padding: 12

    background: Rectangle {
        color: Theme.window
        border.color: Theme.border
    }

    header: Label {
        text: control.title
        visible: control.title !== ""
        elide: Label.ElideRight
        font.bold: true
        padding: 12
        background: Rectangle {
            x: 1
            y: 1
            width: parent.width - 2
            height: parent.height - 1
            color: Theme.panel
        }
    }

    footer: DialogButtonBox {
        visible: count > 0
    }

    T.Overlay.modal: Rectangle {
        color: "#60000000"
    }
    T.Overlay.modeless: Rectangle {
        color: "#20000000"
    }
}
