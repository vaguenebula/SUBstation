import QtQuick
import SUBstation

// A row's mouse in a SelectionList: presses and releases select, a double-click
// activates, a right-click asks for the row's menu, and dragging further than
// the platform's drag distance starts a drag of the selected rows.
MouseArea {
    id: area

    required property var list
    required property int row
    property point pressedAt
    property bool dragging: false

    acceptedButtons: Qt.LeftButton | Qt.RightButton
    hoverEnabled: true

    onPressed: mouse => {
        list.forceActiveFocus()
        pressedAt = Qt.point(mouse.x, mouse.y)
        dragging = false
        list.pressRow(row, mouse.modifiers, mouse.button)
        if (mouse.button === Qt.RightButton)
            list.menuRequested(row)
    }
    onPositionChanged: mouse => {
        if (dragging || !(mouse.buttons & Qt.LeftButton))
            return
        if (Math.abs(mouse.x - pressedAt.x) + Math.abs(mouse.y - pressedAt.y) < Qt.styleHints.startDragDistance)
            return
        dragging = true
        list.startDragFrom(row)
    }
    onReleased: mouse => {
        if (!dragging && mouse.button === Qt.LeftButton)
            list.releaseRow(row)
        dragging = false
    }
    onDoubleClicked: mouse => {
        if (mouse.button === Qt.LeftButton)
            list.activated(row, false)
    }
}
