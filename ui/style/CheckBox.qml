import QtQuick
import QtQuick.Templates as T
import SUBstation

// QCheckBox with its 14 px indicator.
T.CheckBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)
    padding: 2
    spacing: 6
    hoverEnabled: true

    indicator: Rectangle {
        implicitWidth: 14
        implicitHeight: 14
        x: control.text ? control.leftPadding : control.leftPadding + (control.availableWidth - width) / 2
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: 2
        color: control.down ? Theme.panel : (control.hovered ? Theme.surfaceHover : Theme.surface)
        border.color: control.visualFocus ? Theme.accent : Theme.border

        Text {
            anchors.centerIn: parent
            visible: control.checkState === Qt.Checked
            text: "✓"
            font.pixelSize: 12
            font.bold: true
            color: control.enabled ? Theme.accent : Theme.textDisabled
        }
        Rectangle {
            anchors.centerIn: parent
            visible: control.checkState === Qt.PartiallyChecked
            width: 8
            height: 2
            color: control.enabled ? Theme.accent : Theme.textDisabled
        }
    }

    contentItem: Text {
        leftPadding: control.indicator ? control.indicator.width + control.spacing : 0
        text: control.text
        font: control.font
        color: control.enabled ? Theme.text : Theme.textDisabled
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }
}
