import QtQuick
import QtQuick.Controls
import SUBstation

// The browser's lists as QListView/QTreeView with ExtendedSelection had them:
// a click selects a row (Ctrl toggles one, Shift a range from the last one
// clicked); a press on one of several selected rows keeps them all, so a drag
// takes them all, and selects only it on release; Up/Down (Shift extends),
// Page Up/Down and End move the current row; Return/Enter and a double-click
// activate it; a right-click selects the row and asks for its menu. The list
// takes the keyboard focus when clicked. Its rows give their mouse to a
// SelectionRowArea. A row the user chooses, clicked and let go of without a
// drag, or reached with the arrow keys, is said on chosen() (a hot swap swaps
// it in); the current row changing otherwise (a press that becomes a drag, the
// list changing) is no choice.
//
// Keys the list doesn't take (Home, Delete, letters...) are the window's
// shortcuts, as before.
ListView {
    id: list

    // The rows selected, as a JS array (in the order they were selected).
    property var selectedRows: []
    property int anchorRow: -1
    // A row pressed among several selected: selected alone on release, unless it was dragged.
    property int pendingRow: -1
    // Called first with each key pressed (return true: taken), for keys of a list's own.
    property var keyHandler: null
    readonly property int rowHeight: count > 0 && itemAtIndex(0) ? itemAtIndex(0).height : 20

    // Return/Enter (byKey) or a double-click on a row.
    signal activated(int row, bool byKey)
    // A row the user chose: a left click let go of without a drag, or Up/Down,
    // Page Up/Down and End.
    signal chosen(int row)
    // A drag of the selected rows starts (sorted).
    signal dragRequested(var rows)
    // A right-click on a row (it is selected).
    signal menuRequested(int row)

    function isSelected(row) {
        return selectedRows.indexOf(row) >= 0
    }

    function sortedSelection() {
        return selectedRows.slice().sort((a, b) => a - b)
    }

    // Nothing selected but `row` (none: -1); it is current.
    function resetSelection(row) {
        selectedRows = row >= 0 ? [row] : []
        anchorRow = row
        pendingRow = -1
    }

    function selectOnly(row) {
        selectedRows = [row]
        anchorRow = row
    }

    function selectRange(from, to) {
        const rows = []
        for (let row = Math.min(from, to); row <= Math.max(from, to); ++row)
            rows.push(row)
        selectedRows = rows
    }

    function toggleRow(row) {
        const rows = selectedRows.slice()
        const at = rows.indexOf(row)
        if (at >= 0)
            rows.splice(at, 1)
        else
            rows.push(row)
        selectedRows = rows
    }

    function pressRow(row, modifiers, button) {
        if (row < 0)
            return
        pendingRow = -1
        if (button === Qt.RightButton) {
            if (!isSelected(row))
                selectOnly(row)
            currentIndex = row
            return
        }
        if (modifiers & Qt.ControlModifier) {
            toggleRow(row)
            anchorRow = row
        } else if ((modifiers & Qt.ShiftModifier) && anchorRow >= 0) {
            selectRange(anchorRow, row)
        } else if (isSelected(row) && selectedRows.length > 1) {
            pendingRow = row  // (a drag takes them all)
        } else {
            selectOnly(row)
        }
        currentIndex = row
    }

    function releaseRow(row) {
        if (pendingRow === row)
            selectOnly(row)
        pendingRow = -1
    }

    function startDragFrom(row) {
        pendingRow = -1
        if (!isSelected(row))
            selectOnly(row)
        dragRequested(sortedSelection())
    }

    // The current row moves to `row` (Shift: the selection grows to it).
    function moveCurrent(row, extend) {
        if (row < 0 || row >= count)
            return
        if (extend && anchorRow >= 0)
            selectRange(anchorRow, row)
        else
            selectOnly(row)
        currentIndex = row
        positionViewAtIndex(row, ListView.Contain)
    }

    clip: true
    boundsBehavior: Flickable.StopAtBounds
    keyNavigationEnabled: false
    highlightFollowsCurrentItem: false
    currentIndex: -1
    ScrollBar.vertical: ScrollBar {}

    Keys.onPressed: event => {
        if (keyHandler && keyHandler(event)) {
            event.accepted = true
            return
        }
        const extend = (event.modifiers & Qt.ShiftModifier) !== 0
        const page = Math.max(1, Math.floor(height / Math.max(1, rowHeight)) - 1)
        switch (event.key) {
        case Qt.Key_Up:
            moveCurrent(Math.max(0, currentIndex - 1), extend)
            break
        case Qt.Key_Down:
            moveCurrent(Math.min(count - 1, currentIndex + 1), extend)
            break
        case Qt.Key_PageUp:
            moveCurrent(Math.max(0, currentIndex - page), extend)
            break
        case Qt.Key_PageDown:
            moveCurrent(Math.min(count - 1, currentIndex + page), extend)
            break
        case Qt.Key_End:
            moveCurrent(count - 1, extend)
            break
        case Qt.Key_Return:
        case Qt.Key_Enter:
            if (currentIndex >= 0)
                activated(currentIndex, true)
            event.accepted = true
            return
        default:
            return
        }
        event.accepted = true
        if (currentIndex >= 0)
            chosen(currentIndex)
    }
}
