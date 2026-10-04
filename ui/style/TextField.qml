import QtQuick
import QtQuick.Templates as T
import SUBstation

// QLineEdit: SURFACE, a BORDER line (ACCENT while it has the focus), 3 px
// corners, padding 3 6; the selection in ACCENT.
T.TextField {
    id: control

    implicitWidth: implicitBackgroundWidth + leftInset + rightInset
                   || Math.max(contentWidth, placeholder.implicitWidth) + leftPadding + rightPadding
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding,
                             placeholder.implicitHeight + topPadding + bottomPadding)

    topPadding: 4
    bottomPadding: 4
    leftPadding: 7
    rightPadding: 7

    color: enabled ? Theme.text : Theme.textDisabled
    selectionColor: Theme.accent
    selectedTextColor: Theme.accentText
    placeholderTextColor: Theme.textDim
    verticalAlignment: TextInput.AlignVCenter
    selectByMouse: true

    Text {
        id: placeholder
        x: control.leftPadding
        y: control.topPadding
        width: control.width - (control.leftPadding + control.rightPadding)
        height: control.height - (control.topPadding + control.bottomPadding)
        text: control.placeholderText
        font: control.font
        color: control.placeholderTextColor
        verticalAlignment: control.verticalAlignment
        horizontalAlignment: control.horizontalAlignment
        visible: !control.length && !control.preeditText
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitWidth: 120
        implicitHeight: 22
        radius: Theme.radius
        color: Theme.surface
        border.color: control.activeFocus ? Theme.accent : Theme.border
    }
}
