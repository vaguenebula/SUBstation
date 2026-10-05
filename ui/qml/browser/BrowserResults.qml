import QtQuick
import QtQuick.Controls
import SUBstation

// The browser's list of results (the controller's ItemListModel, a page at a
// time): what the sidebar entry and the search text find. Its current row is
// the controller's (selecting an audio file previews it); when the list is
// searched again for a change elsewhere it keeps its current item and place
// (positionRestored). A preset can be renamed in place (startRename()).
SelectionList {
    id: results

    readonly property var browser: Session.browser
    // The row whose name is being edited (-1: none).
    property int renamingRow: -1

    // Renames the preset at `path` in place, if the list shows it: false if not.
    function startRename(path) {
        let row = -1
        if (currentIndex >= 0 && browser.results.get(currentIndex).path === path)
            row = currentIndex
        for (let i = 0; row < 0 && i < count; ++i)
            if (browser.results.get(i).path === path)
                row = i
        if (row < 0)
            return false
        positionViewAtIndex(row, ListView.Contain)
        renamingRow = row
        return true
    }

    function finishRename(path, name) {
        if (renamingRow < 0)
            return
        renamingRow = -1
        browser.renamePreset(path, name)  // (an unchanged or empty name: nothing; failing: a status message)
        results.forceActiveFocus()
    }

    function cancelRename() {
        renamingRow = -1
        results.forceActiveFocus()
    }

    // The next page of results once the list is scrolled near its end.
    function fetchNearEnd() {
        if (pages.hasMore() && contentY + 2 * height >= contentHeight)
            pages.fetchMore()
    }

    objectName: "resultsList"
    model: PagedRows {
        id: pages
        model: results.browser.results
    }

    onCurrentIndexChanged: if (browser.currentRow !== currentIndex) browser.currentRow = currentIndex
    onContentYChanged: {
        browser.setTopRow(Math.max(0, indexAt(1, contentY + 1)))
        fetchNearEnd()
    }
    onCountChanged: fetchNearEnd()
    onHeightChanged: fetchNearEnd()
    onActivated: row => browser.activate(row)

    Connections {
        target: results.browser

        function onCurrentRowChanged() {
            if (results.currentIndex !== results.browser.currentRow)
                results.currentIndex = results.browser.currentRow
        }
        function onResultsShown() {
            results.renamingRow = -1
            results.resetSelection(results.browser.currentRow)
            if (results.currentIndex !== results.browser.currentRow)
                results.currentIndex = results.browser.currentRow
        }
        function onPositionRestored(currentRow, topRow) {
            results.positionViewAtIndex(topRow, ListView.Beginning)
        }
    }

    delegate: Item {
        id: row

        required property int index
        required property string name
        required property string path
        required property string kind
        required property string display
        required property string toolTip
        required property string icon

        readonly property bool selected: results.selectedRows.indexOf(index) >= 0
        // (A row going away has index -1: it isn't the one renamed.)
        readonly property bool renaming: results.renamingRow >= 0 && results.renamingRow === index

        width: ListView.view.width
        implicitHeight: Math.max(16, label.implicitHeight) + 4

        Rectangle {
            anchors.fill: parent
            color: row.selected ? Theme.accent : (area.containsMouse ? Theme.panelAlt : "transparent")
        }

        Icon {
            id: icon
            anchors.left: parent.left
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            name: row.icon
            size: 16
        }
        Text {
            id: label
            anchors.left: icon.right
            anchors.leftMargin: 4
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            visible: !row.renaming
            text: row.display
            font: Theme.listFont
            elide: Text.ElideRight
            color: row.selected ? Theme.accentText : Theme.text
        }

        SelectionRowArea {
            id: area
            anchors.fill: parent
            list: results
            row: row.index
        }

        Loader {
            anchors.left: icon.right
            anchors.leftMargin: 2
            anchors.right: parent.right
            anchors.rightMargin: 2
            anchors.verticalCenter: parent.verticalCenter
            active: row.renaming
            sourceComponent: TextField {
                objectName: "renameField"
                topPadding: 1
                bottomPadding: 1
                font: Theme.listFont
                text: row.name
                Component.onCompleted: {
                    forceActiveFocus()
                    selectAll()
                }
                onAccepted: results.finishRename(row.path, text)
                onActiveFocusChanged: if (!activeFocus) results.finishRename(row.path, text)
                Keys.onEscapePressed: results.cancelRename()
            }
        }

        // (a row being let go of when the list changes under the mouse has no window to show it in)
        ToolTip.visible: Window.window !== null && area.containsMouse && !area.pressed && row.toolTip !== ""
        ToolTip.text: row.toolTip
        ToolTip.delay: 700
    }
}
