import QtQuick
import QtQuick.Templates as T
import SUBstation

// A list's row (QTreeView/QListView::item): padding 2 0, PANEL_ALT under the
// mouse, ACCENT when highlighted (selected, or a combo box's current row).
T.ItemDelegate {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    topPadding: 3
    bottomPadding: 3
    leftPadding: 8
    rightPadding: 8
    hoverEnabled: true
    focusPolicy: Qt.NoFocus

    contentItem: Text {
        text: control.text
        font: control.font
        color: !control.enabled ? Theme.textDisabled : (control.highlighted ? Theme.accentText : Theme.text)
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        color: control.highlighted ? Theme.accent : (control.hovered ? Theme.panelAlt : "transparent")
    }
}
