import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import SUBstation

// The browser on the left, on Session.browser: the search field and the sort,
// the sidebar (categories and places), the results or a place's folder tree,
// and the footer (preview on or off, and the status).
//
// Ctrl+F searches everything (the controller's focusSearch(): "All", then the
// field takes the keyboard). Enter or Down in the field selects the first
// result (once the results are there), and Enter on a result adds it, as a
// double-click does. Selecting an audio file previews it while the
// headphones are on; a press anywhere outside the browser stops the preview.
// Items dragged out carry the controller's drag data (dragData()), and count
// as used when the drop took them. Presets are renamed in place (Ctrl+R with
// the list focused, or their menu's Rename…), and deleted after asking.
//
// Find Similar Sounds (an audio file's menu here, or an audio clip's in the
// arrangement) lists the sounds most like it, most similar first: a bar over
// the list says like what, and its ✕ goes back to the list as it was.
//
// While a hot swap runs (Session.hotSwap: an audio clip's menu, the File
// Manager), a bar over the list says what the user's choice plays in: a file
// clicked (let go of without a drag) or reached with the arrow keys swaps in at
// once, a double-click or Enter keeps it and ends the hot swap, as Esc or the
// bar's ✕ do. Its Similar lists the sounds most like what is swapped in. The
// list takes the keyboard when a hot swap starts. A press anywhere outside the
// browser (but `hotSwapKeepers`: the File Manager, the transport bar) ends it,
// and so does a drag starting here: what is dragged out is added, not swapped.
Rectangle {
    id: panel

    readonly property var browser: Session.browser
    readonly property var hotSwap: Session.hotSwap
    // Items a press in doesn't end a hot swap (the window's File Manager and transport bar).
    property var hotSwapKeepers: []
    // The results list has the keyboard (Ctrl+R renames the preset there).
    readonly property bool listFocused: results.activeFocus
    readonly property alias searchField: search
    readonly property alias sidebarView: sidebar
    readonly property alias resultsView: results
    readonly property alias treeView: tree
    // What a drag carries now (set when it starts; for the tests too).
    readonly property alias dragSource: dragSource

    // Ctrl+F: the search field takes the keyboard, its text selected.
    function focusSearch() {
        search.forceActiveFocus()
        search.selectAll()
    }

    // Renames the preset at `path` in place (Edit › Rename with the list focused).
    function startRename(path) {
        return results.startRename(path)
    }

    // What a drag of these rows (of the results, or of the folder tree) carries.
    function dragPayload(rows, fromTree) {
        return fromTree ? tree.folders.dragData(rows) : browser.dragData(rows)
    }

    // A drag of these rows starts (QDrag: it ends when they are dropped, or not).
    function startDrag(rows, fromTree) {
        if (rows.length === 0)
            return
        hotSwap.stop()  // (what is dragged out is added: the hot swap is over)
        dragSource.rows = rows
        dragSource.paths = fromTree ? tree.folders.paths(rows) : []
        dragSource.fromTree = fromTree
        dragSource.Drag.mimeData = dragPayload(rows, fromTree)
        dragSource.Drag.imageSource = fromTree ? Icons.url("waveform", undefined, false, false)
                                               : Icons.url(browser.results.get(rows[0]).icon, undefined, false, false)
        dragSource.Drag.active = false  // (a drag that never said it ended)
        dragSource.Drag.active = true
    }

    // A drag ended: what a drop took counts as used (the list isn't sorted again).
    function dragFinished(dropAction) {
        if (dropAction === Qt.IgnoreAction)
            return
        if (dragSource.fromTree)
            browser.droppedFiles(dragSource.paths)
        else
            browser.dropped(dragSource.rows)
    }

    function runSidebarAction(action, scope) {
        switch (action) {
        case "removePlace":
            browser.removePlace(scope[1])
            break
        case "rescanPlugins":
            browser.rescanPlugins()
            break
        case "showPresetFolder":
            browser.showInFolder(browser.presetFolder(scope))
            break
        case "addPlace":
            placeDialog.open()
            break
        case "rescan":
            browser.rescan()
            break
        }
    }

    function runResultAction(action, row) {
        const item = browser.results.get(row)
        switch (action) {
        case "renamePreset":
            results.startRename(item.path)
            break
        case "deletePreset":
            deleteQuestion.path = item.path
            deleteQuestion.show(qsTr("Move the preset “%1” to the Recycle Bin?").arg(item.name))
            break
        case "showInFolder":
            browser.showInFolder(item.path)
            break
        case "findSimilar":
            browser.findSimilar(item.path)
            break
        }
    }

    objectName: "browser"
    color: Theme.window
    implicitWidth: 300

    // Esc (the list or the tree having the keyboard) ends a hot swap.
    Keys.onEscapePressed: event => {
        if (panel.hotSwap.active)
            panel.hotSwap.stop()
        else
            event.accepted = false
    }

    // A hot swap starting: the list (or the tree) takes the keyboard, for the arrow keys.
    Connections {
        target: panel.hotSwap

        property bool wasActive: false

        function onChanged() {
            const starting = panel.hotSwap.active && !wasActive
            wasActive = panel.hotSwap.active
            if (starting && !results.activeFocus && !tree.activeFocus)
                (panel.browser.showingTree ? tree : results).forceActiveFocus()
        }
    }

    Connections {
        target: panel.browser

        function onSearchFocusRequested() {
            panel.focusSearch()
        }
        function onSearchTextChanged() {
            if (search.text !== panel.browser.searchText)
                search.text = panel.browser.searchText
        }
        function onSelectRowRequested(row) {
            if (panel.browser.showingTree) {
                tree.moveCurrent(row, false)
                tree.forceActiveFocus()
            } else {
                results.moveCurrent(row, false)
                results.forceActiveFocus()
            }
        }
        function onAddPlaceRequested() {
            placeDialog.open()
        }
        function onShowingTreeChanged() {
            if (panel.browser.showingTree)
                tree.wanted = true
        }
    }

    // A press anywhere outside the browser stops a preview (its own menus count as inside).
    OutsidePresses {
        item: panel
        enabled: panel.browser.previewing
        ignore: sidebarMenu.visible || resultMenu.visible || sort.popup.visible
        onPressed: panel.browser.stopPreview()
    }

    // A press anywhere else ends a hot swap: the user is doing something else.
    OutsidePresses {
        item: panel
        enabled: panel.hotSwap.active
        alsoInside: panel.hotSwapKeepers
        ignore: sidebarMenu.visible || resultMenu.visible || sort.popup.visible
        onPressed: panel.hotSwap.stop()
    }

    // Carries a drag out of the browser (the platform's drag: to the arrangement, the device view...).
    Item {
        id: dragSource

        property var rows: []
        property var paths: []
        property bool fromTree: false

        Drag.dragType: Drag.Automatic
        Drag.supportedActions: Qt.CopyAction
        Drag.proposedAction: Qt.CopyAction
        Drag.onDragFinished: dropAction => panel.dragFinished(dropAction)
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 6
        spacing: 4

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            spacing: 6

            TextField {
                id: search
                objectName: "searchField"
                Layout.fillWidth: true
                placeholderText: qsTr("Search  (Ctrl+F)")
                rightPadding: clear.visible ? clear.width + 6 : 7
                Component.onCompleted: text = panel.browser.searchText
                onTextEdited: panel.browser.searchText = text
                onAccepted: panel.browser.selectFirstResult()
                Keys.onDownPressed: panel.browser.selectFirstResult()

                // The clear button (QLineEdit's).
                Text {
                    id: clear
                    anchors.right: parent.right
                    anchors.rightMargin: 5
                    anchors.verticalCenter: parent.verticalCenter
                    visible: search.text !== ""
                    text: "✕"
                    font.pixelSize: 10
                    color: clearArea.containsMouse ? Theme.text : Theme.textDim

                    MouseArea {
                        id: clearArea
                        anchors.fill: parent
                        anchors.margins: -3
                        hoverEnabled: true
                        cursorShape: Qt.ArrowCursor
                        onClicked: {
                            search.clear()
                            panel.browser.searchText = ""
                        }
                    }
                }
            }
            ChoiceBox {
                id: sort
                objectName: "sort"
                choices: panel.browser.sorts
                chosenIndex: {
                    const sorts = panel.browser.sorts
                    for (let i = 0; i < sorts.length; ++i)
                        if (sorts[i].value === panel.browser.sort)
                            return i
                    return 0
                }
                tooltip: panel.browser.similarTo !== "" ? qsTr("Similarity: the most similar sounds first")
                                                        : qsTr("Sort the list: Rank puts what you use most first")
                onChosen: index => panel.browser.sort = panel.browser.sorts[index].value
            }
        }

        // A hot swap: what the selection plays in the place of.
        Rectangle {
            id: hotSwapBar
            objectName: "hotSwapBar"
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            visible: panel.hotSwap.active
            implicitHeight: hotSwapRow.implicitHeight + 4
            radius: 3
            color: Theme.accent

            RowLayout {
                id: hotSwapRow
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 2
                spacing: 5

                Icon {
                    name: "hotswap"
                    color: Theme.accentText
                }
                Label {
                    objectName: "hotSwapLabel"
                    Layout.fillWidth: true
                    text: qsTr("Hot-Swap %1").arg(panel.hotSwap.name)
                          + (panel.hotSwap.usesText !== "" ? "  (" + panel.hotSwap.usesText + ")" : "")
                    color: Theme.accentText
                    elide: Text.ElideMiddle

                    HoverHandler {
                        id: hotSwapHover
                    }
                    ToolTip.visible: hotSwapHover.hovered
                    ToolTip.text: qsTr("Hot-Swap\nThe sample selected here plays in the place of %1 (%2): click through samples, or use the arrow keys, to hear them in the song. Double-click one (or press Enter) to keep it; Esc keeps what is swapped in.")
                                  .arg(panel.hotSwap.name).arg(panel.hotSwap.usesText)
                    ToolTip.delay: 700
                }
                RoleButton {
                    objectName: "hotSwapSimilar"
                    role: "small"
                    visible: panel.browser.canFindSimilar
                    text: qsTr("Similar")
                    tooltip: qsTr("Similar\nLists the sounds most like the one swapped in now.")
                    onClicked: panel.hotSwap.findSimilar()
                }
                RoleButton {
                    objectName: "stopHotSwap"
                    role: "small"
                    text: "✕"
                    tooltip: qsTr("End the hot swap, keeping what is swapped in (Esc)")
                    onClicked: panel.hotSwap.stop()
                }
            }
        }

        // Find Similar: the sound the list shows the sounds like.
        Rectangle {
            id: similarBar
            objectName: "similarBar"
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            visible: panel.browser.similarTo !== ""
            implicitHeight: similarRow.implicitHeight + 4
            radius: 3
            color: Theme.surface
            border.color: Theme.accent

            RowLayout {
                id: similarRow
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 2
                spacing: 5

                Icon {
                    name: "waveform"
                    color: Theme.accent
                }
                Label {
                    objectName: "similarLabel"
                    Layout.fillWidth: true
                    text: qsTr("Similar to %1").arg(panel.browser.similarName)
                    color: Theme.text
                    elide: Text.ElideMiddle

                    HoverHandler {
                        id: similarHover
                    }
                    ToolTip.visible: similarHover.hovered
                    ToolTip.text: panel.browser.similarTo
                    ToolTip.delay: 700
                }
                RoleButton {
                    objectName: "clearSimilar"
                    role: "flat"
                    text: "✕"
                    tooltip: qsTr("Back to the list")
                    onClicked: panel.browser.clearSimilar()
                }
            }
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            BrowserSidebar {
                id: sidebar
                SplitView.preferredWidth: 110
                SplitView.minimumWidth: 40
                onContextMenuRequested: scope => {
                    sidebarMenu.scope = scope
                    sidebarMenu.popupWith(panel.browser.sidebarActions(scope))
                }
            }

            Rectangle {
                SplitView.fillWidth: true
                SplitView.minimumWidth: 60
                color: Theme.panel

                BrowserResults {
                    id: results
                    anchors.fill: parent
                    visible: !panel.browser.showingTree
                    onDragRequested: rows => panel.startDrag(rows, false)
                    onMenuRequested: row => {
                        resultMenu.row = row
                        resultMenu.popupWith(panel.browser.resultActions(row))
                    }
                }
                BrowserFolderTree {
                    id: tree
                    anchors.fill: parent
                    visible: panel.browser.showingTree
                    wanted: panel.browser.showingTree
                    onDragRequested: rows => panel.startDrag(rows, true)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 6
            Layout.topMargin: 2
            Layout.bottomMargin: 4
            spacing: 6

            ToggleButton {
                objectName: "preview"
                role: "tool"
                iconName: "headphones"
                tooltip: qsTr("Preview files when selected")
                checkable: false
                checked: panel.browser.previewEnabled
                onClicked: panel.browser.previewEnabled = !panel.browser.previewEnabled
            }
            Label {
                objectName: "browserStatus"
                Layout.fillWidth: true
                text: panel.browser.statusText
                color: Theme.textDim
                elide: Text.ElideRight
            }
        }
    }

    ActionMenu {
        id: sidebarMenu
        objectName: "sidebarMenu"
        property var scope: []
        onChosen: action => panel.runSidebarAction(action, scope)
    }

    ActionMenu {
        id: resultMenu
        objectName: "resultMenu"
        property int row: -1
        onChosen: action => panel.runResultAction(action, row)
    }

    MessageBox {
        id: deleteQuestion
        objectName: "deletePresetQuestion"
        property string path
        title: qsTr("Delete Preset")
        icon: "question"
        choices: [{text: qsTr("Yes"), value: "yes"}, {text: qsTr("No"), value: "no"}]
        escapeValue: "no"
        onAnswered: value => {
            if (value === "yes")
                panel.browser.deletePreset(path)
        }
    }

    FolderDialog {
        id: placeDialog
        objectName: "placeDialog"
        title: qsTr("Add Folder to Places")
        currentFolder: FileUrls.homeUrl()
        onAccepted: panel.browser.addPlace(FileUrls.localPath(selectedFolder))
    }
}
