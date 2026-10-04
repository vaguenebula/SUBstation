import QtQuick
import QtQuick.Templates as T
import SUBstation

// QGroupBox: a BORDER line with 4 px corners, the title dim at its top left.
T.GroupBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding,
                            implicitLabelWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)

    spacing: 6
    padding: 8
    topPadding: padding + (implicitLabelWidth > 0 ? implicitLabelHeight + spacing : 0)

    label: Text {
        x: 8
        width: control.availableWidth
        text: control.title
        font: control.font
        color: Theme.textDim
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        y: control.topPadding - control.bottomPadding
        width: parent.width
        height: parent.height - control.topPadding + control.bottomPadding
        color: "transparent"
        border.color: Theme.border
        radius: 4
    }
}
